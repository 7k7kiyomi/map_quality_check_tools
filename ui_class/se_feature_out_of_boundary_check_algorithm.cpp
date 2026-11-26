#define _HAS_STD_BYTE 0
#include "se_feature_out_of_boundary_check_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <cpl_string.h>
#include <cpl_conv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>
#include <string>

static QString fromUtf8ToQString(const char* s){ return QString::fromUtf8(s ? s : ""); }
static std::string fromQStringToUtf8(const QString& q){ return q.toUtf8().toStdString(); }

static void DropLogger(){ try { spdlog::drop("feature_out_of_boundary_check"); spdlog::set_default_logger(nullptr);} catch(...){} }

static bool GeometryWithin(OGRGeometry* g, OGRGeometry* boundary){
    if (!g || !boundary || g->IsEmpty() || boundary->IsEmpty()) return false;
    OGREnvelope ge, be; g->getEnvelope(&ge); boundary->getEnvelope(&be);
    if (ge.MinX < be.MinX || ge.MaxX > be.MaxX || ge.MinY < be.MinY || ge.MaxY > be.MaxY) return false;
    if (boundary->Contains(g)) return true;
    try {
        OGRGeometry* diff = g->Difference(boundary);
        if (diff){ bool empty = diff->IsEmpty(); delete diff; if (empty) return true; }
    } catch (...) {}
    return false;
}

SE_Error FeatureOutOfBoundaryCheck(
    const char* szBoundaryShpFilePath,
    const char* szFeatureShpPaths,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath)
{
    try {
        DropLogger();
        QString logQ = fromUtf8ToQString(szOutputLogFilePath);
        QByteArray logLocal = logQ.toLocal8Bit();
        auto logger = spdlog::basic_logger_mt("feature_out_of_boundary_check", std::string(logLocal.constData(), logLocal.size()));
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("开始要素超出边界检查");
    } catch (...) {
        return SE_ERROR_FAILURE;
    }

    if (!szBoundaryShpFilePath || !szFeatureShpPaths || !szOutputErrorLayerPath || !szOutputJsonPath){
        spdlog::error("输入参数无效");
        DropLogger();
        return SE_ERROR_INVALID_PARAMETER;
    }

    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
    GDALAllRegister();

    GDALDataset* dsBoundary = (GDALDataset*)GDALOpenEx(szBoundaryShpFilePath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (!dsBoundary){
        spdlog::error("无法打开内图廓图层: {}", fromQStringToUtf8(fromUtf8ToQString(szBoundaryShpFilePath)));
        DropLogger();
        return SE_ERROR_GDAL_OPEN_FAILED;
    }
    OGRLayer* lyrBoundary = dsBoundary->GetLayer(0);
    if (!lyrBoundary){
        GDALClose(dsBoundary);
        spdlog::error("无法获取内图廓图层");
        DropLogger();
        return SE_ERROR_LAYER_NOT_FOUND;
    }

    OGRSpatialReference* outSRS = lyrBoundary->GetSpatialRef();
    OGRMultiPolygon* polyColl = new OGRMultiPolygon();
    OGRMultiLineString* lineColl = new OGRMultiLineString();
    lyrBoundary->ResetReading();
    OGRFeature* f;
    while ((f = lyrBoundary->GetNextFeature()) != NULL){
        OGRGeometry* g = f->GetGeometryRef();
        if (g){
            OGRGeometry* used = g;
            if (!g->IsValid()){
                OGRGeometry* v = g->MakeValid();
                if (!v) v = g->Buffer(0);
                if (v && !v->IsEmpty()) used = v;
            }
            OGRwkbGeometryType gt = wkbFlatten(used->getGeometryType());
            if (gt == wkbPolygon){
                polyColl->addGeometry(used);
            } else if (gt == wkbMultiPolygon){
                OGRMultiPolygon* mp = (OGRMultiPolygon*)used;
                for (int i = 0; i < mp->getNumGeometries(); ++i){
                    OGRGeometry* gi = mp->getGeometryRef(i);
                    if (gi) polyColl->addGeometry(gi);
                }
            } else if (gt == wkbLineString){
                lineColl->addGeometry(used);
            } else if (gt == wkbMultiLineString || gt == wkbLinearRing){
                OGRGeometryCollection* ml = dynamic_cast<OGRGeometryCollection*>(used);
                if (ml){
                    for (int i = 0; i < ml->getNumGeometries(); ++i){
                        OGRGeometry* gi = ml->getGeometryRef(i);
                        if (gi) lineColl->addGeometry(gi);
                    }
                } else {
                    lineColl->addGeometry(used);
                }
            }
            if (used != g) delete used;
        }
        OGRFeature::DestroyFeature(f);
    }
    OGRGeometry* boundaryUnion = nullptr;
    if (polyColl->getNumGeometries() > 0){
        boundaryUnion = polyColl->UnionCascaded();
    }
    if (!boundaryUnion && lineColl->getNumGeometries() > 0){
        OGREnvelope env; lineColl->getEnvelope(&env);
        OGRLinearRing ring; ring.addPoint(env.MinX, env.MinY); ring.addPoint(env.MaxX, env.MinY); ring.addPoint(env.MaxX, env.MaxY); ring.addPoint(env.MinX, env.MaxY); ring.addPoint(env.MinX, env.MinY);
        OGRPolygon* poly = new OGRPolygon(); poly->addRing(&ring);
        boundaryUnion = poly;
    }
    delete polyColl;
    delete lineColl;
    if (!boundaryUnion){
        GDALClose(dsBoundary);
        spdlog::error("内图廓合并失败");
        DropLogger();
        return SE_ERROR_FAILURE;
    }
    if (!boundaryUnion->IsValid()){
        OGRGeometry* v = boundaryUnion->MakeValid();
        if (!v) v = boundaryUnion->Buffer(0);
        if (v && !v->IsEmpty()){ delete boundaryUnion; boundaryUnion = v; }
    }

    std::vector<std::string> featureFiles;
    {
        std::string s(szFeatureShpPaths);
        size_t pos = 0;
        while ((pos = s.find(';')) != std::string::npos){ std::string t = s.substr(0,pos); if(!t.empty()) featureFiles.push_back(t); s.erase(0,pos+1);} if(!s.empty()) featureFiles.push_back(s);
    }
    if (featureFiles.empty()){
        delete boundaryUnion; GDALClose(dsBoundary); spdlog::error("要素图层路径为空"); DropLogger(); return SE_ERROR_INVALID_PARAMETER;
    }

    GDALDriver* shpDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!shpDriver){ delete boundaryUnion; GDALClose(dsBoundary); spdlog::error("无法获取Shapefile驱动"); DropLogger(); return SE_ERROR_GDAL_DRIVER_NOT_FOUND; }

    QFileInfo outInfo(fromUtf8ToQString(szOutputErrorLayerPath));
    if (outInfo.exists()) shpDriver->Delete(outInfo.absoluteFilePath().toStdString().c_str());
    GDALDataset* dsOut = shpDriver->Create(outInfo.absoluteFilePath().toStdString().c_str(), 0,0,0, GDT_Unknown, NULL);
    if (!dsOut){ delete boundaryUnion; GDALClose(dsBoundary); spdlog::error("无法创建输出文件: {}", fromQStringToUtf8(outInfo.absoluteFilePath())); DropLogger(); return SE_ERROR_GDAL_CREATE_FAILED; }
    char** lco = nullptr; lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
    OGRLayer* lyrOut = dsOut->CreateLayer("out_of_boundary_errors", outSRS, wkbUnknown, lco); CSLDestroy(lco);
    if (!lyrOut){ GDALClose(dsOut); delete boundaryUnion; GDALClose(dsBoundary); spdlog::error("无法创建输出图层"); DropLogger(); return SE_ERROR_LAYER_CREATE_FAILED; }
    {
        OGRFieldDefn fLayer("LayerName", OFTString); fLayer.SetWidth(100); lyrOut->CreateField(&fLayer);
        OGRFieldDefn fFid("FeatureID", OFTInteger64); lyrOut->CreateField(&fFid);
        OGRFieldDefn fType("ErrorType", OFTString); fType.SetWidth(64); lyrOut->CreateField(&fType);
        OGRFieldDefn fDesc("Description", OFTString); fDesc.SetWidth(128); lyrOut->CreateField(&fDesc);
    }

    bool hasError = false;
    for (const auto& path : featureFiles){
        GDALDataset* dsFeat = (GDALDataset*)GDALOpenEx(path.c_str(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (!dsFeat){ spdlog::error("无法打开要素图层: {}", path); continue; }
        OGRLayer* lyrFeat = dsFeat->GetLayer(0);
        if (!lyrFeat){ spdlog::error("无法获取要素图层: {}", path); GDALClose(dsFeat); continue; }
        std::string layerName = lyrFeat->GetName();
        lyrFeat->ResetReading();
        OGRFeature* pf;
        while ((pf = lyrFeat->GetNextFeature()) != NULL){
            OGRGeometry* g = pf->GetGeometryRef();
            if (g && !g->IsEmpty()){
                OGRGeometry* used = g;
                if (!g->IsValid()){ OGRGeometry* v = g->MakeValid(); if(!v) v = g->Buffer(0); if (v && !v->IsEmpty()) used = v; }
                bool within = false; try { within = GeometryWithin(used, boundaryUnion);} catch(...) { within = false; }
                if (!within){
                    hasError = true;
                    OGRFeature* outF = OGRFeature::CreateFeature(lyrOut->GetLayerDefn());
                    outF->SetGeometry(used);
                    outF->SetField("LayerName", layerName.c_str());
                    outF->SetField("FeatureID", pf->GetFID());
                    outF->SetField("ErrorType", fromQStringToUtf8(QStringLiteral("要素超出内图廓范围")).c_str());
                    outF->SetField("Description", fromQStringToUtf8(QStringLiteral("要素不完全位于内图廓图层内")).c_str());
                    lyrOut->CreateFeature(outF);
                    OGRFeature::DestroyFeature(outF);
                }
                if (used != g) delete used;
            }
            OGRFeature::DestroyFeature(pf);
        }
        GDALClose(dsFeat);
    }

    GDALClose(dsOut);
    GDALClose(dsBoundary);
    delete boundaryUnion;

    QJsonObject item;
    item["id"] = "305";
    item["name"] = QString::fromUtf8("要素超出边界检查");
    item["status"] = hasError ? QString::fromUtf8("质检完成") : QString::fromUtf8("质检完成");
    item["err_exist"] = hasError ? QString::fromUtf8("true") : QString::fromUtf8("false");
    item["info"] = hasError ? QString::fromUtf8("存在要素图层超出边界") : QString::fromUtf8("不存在要素图层超出边界");
    QJsonObject root; root["check_item1"] = item;
    QJsonDocument doc(root);
    QFile jf(fromUtf8ToQString(szOutputJsonPath));
    if (jf.open(QIODevice::WriteOnly | QIODevice::Text)) { jf.write(doc.toJson(QJsonDocument::Compact)); jf.close(); spdlog::info("JSON结果已生成: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); }
    else { spdlog::error("无法创建JSON结果文件: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); DropLogger(); return SE_ERROR_WRITE_FILE_FAILED; }

    DropLogger();
    return SE_ERROR_NONE;
}
