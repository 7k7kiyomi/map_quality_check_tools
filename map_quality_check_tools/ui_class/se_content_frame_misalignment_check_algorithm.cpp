#define _HAS_STD_BYTE 0
#include "se_content_frame_misalignment_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QDir>
#include <QtXml/QDomDocument>
#include <QtXml/QDomElement>
#include <cpl_string.h>
#include <cpl_conv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <limits>

static QString fromUtf8ToQString(const char* s){ return QString::fromUtf8(s ? s : ""); }
static std::string fromQStringToUtf8(const QString& q){ return q.toUtf8().toStdString(); }

static void DropLogger(){ try { spdlog::drop("content_frame_misalignment"); spdlog::set_default_logger(nullptr);} catch(...){} }

// 由包络创建矩形面
static OGRPolygon* MakeRectFromEnvelope(const OGREnvelope& env)
{
    OGRLinearRing ring;
    ring.addPoint(env.MinX, env.MinY);
    ring.addPoint(env.MaxX, env.MinY);
    ring.addPoint(env.MaxX, env.MaxY);
    ring.addPoint(env.MinX, env.MaxY);
    ring.addPoint(env.MinX, env.MinY);
    OGRPolygon* poly = new OGRPolygon();
    poly->addRing(&ring);
    return poly;
}

// 毫米到图层单位：mm -> 米 -> 图层单位
static double MmToUnits(double mm, int scale, OGRSpatialReference* srs)
{
    if (mm <= 0.0) return 0.0;
    double meters = (mm * scale) / 1000.0;
    double lu = 1.0;
    if (srs && !srs->IsGeographic()) lu = srs->GetLinearUnits(nullptr);
    if (lu <= 0.0) lu = 1.0;
    return meters / lu;
}

// 解析XML：读取最大居中错位容差（毫米）
static SE_Error ParseXmlToleranceMM(const char* szSchemaXmlPath, double& outMM)
{
    outMM = 0.0;
    QFile file(fromUtf8ToQString(szSchemaXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return SE_ERROR_FILE_NOT_FOUND;
    QDomDocument doc; QString err; int el=0, ec=0; if (!doc.setContent(&file, &err, &el, &ec)){ file.close(); return SE_ERROR_XML_PARSE_FAILED; } file.close();
    QDomElement root = doc.documentElement();
    QDomNodeList items = root.elementsByTagName("check_item");
    for (int i=0;i<items.count();++i){
        QDomElement it = items.at(i).toElement();
        QString name = it.firstChildElement("name").text().trimmed();
        if (name == QStringLiteral("内容与图廓错位检查")){
            QDomNodeList rules = it.elementsByTagName("rule");
            for (int k=0;k<rules.count();++k){ QDomElement r = rules.at(k).toElement(); QString rname = r.firstChildElement("name").text().trimmed(); QString rv = r.firstChildElement("value").text().trimmed(); if (rname == QStringLiteral("最大居中错位容差")) { bool ok=false; double v=rv.toDouble(&ok); if (ok && v>0.0){ outMM = v; return SE_ERROR_NONE; } } }
        }
    }
    return SE_ERROR_XML_PARSE_FAILED;
}

SE_Error ContentFrameMisalignmentCheck(
    const char* szAdminAreaShpFilePath,
    const char* szInnerFrameShpFilePath,
    const char* szSchemaXmlPath,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath,
    int nScale)
{
    try{
        // 初始化日志
        DropLogger();
        QString logQ = fromUtf8ToQString(szOutputLogFilePath);
        QByteArray logLocal = logQ.toLocal8Bit();
        auto logger = spdlog::basic_logger_mt("content_frame_misalignment", std::string(logLocal.constData(), logLocal.size()));
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("开始内容与图廓错位检查，比例尺: {}", nScale);
    } catch (...) { return SE_ERROR_FAILURE; }

    if (!szAdminAreaShpFilePath || !szInnerFrameShpFilePath || !szSchemaXmlPath || !szOutputJsonPath || !szOutputErrorLayerPath){ spdlog::error("输入参数无效"); DropLogger(); return SE_ERROR_INVALID_PARAMETER; }

    // 解析容差（毫米）
    double tolMM = 0.0;
    SE_Error xmlRes = ParseXmlToleranceMM(szSchemaXmlPath, tolMM);
    if (xmlRes != SE_ERROR_NONE){ spdlog::error("XML解析失败，未获取最大居中错位容差"); DropLogger(); return xmlRes; }
    spdlog::info("最大居中错位容差(毫米): {}", tolMM);

    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
    GDALAllRegister();

    // 打开行政区划面
    GDALDataset* dsAdmin = (GDALDataset*)GDALOpenEx(szAdminAreaShpFilePath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (!dsAdmin){ spdlog::error("无法打开行政区划面: {}", fromQStringToUtf8(fromUtf8ToQString(szAdminAreaShpFilePath))); DropLogger(); return SE_ERROR_GDAL_OPEN_FAILED; }
    OGRLayer* lyrAdmin = dsAdmin->GetLayer(0); if (!lyrAdmin){ GDALClose(dsAdmin); spdlog::error("无法获取行政区划图层"); DropLogger(); return SE_ERROR_LAYER_NOT_FOUND; }

    OGRSpatialReference* srs = lyrAdmin->GetSpatialRef();

    // 融合行政区划面并计算包络
    OGRMultiPolygon* adminColl = new OGRMultiPolygon();
    lyrAdmin->ResetReading(); OGRFeature* fA;
    while ((fA = lyrAdmin->GetNextFeature()) != NULL){
        OGRGeometry* g = fA->GetGeometryRef();
        if (g){ OGRGeometry* used = g; if (!g->IsValid()){ OGRGeometry* v=g->MakeValid(); if (!v) v=g->Buffer(0); if (v && !v->IsEmpty()) used=v; }
            OGRwkbGeometryType gt = wkbFlatten(used->getGeometryType());
            if (gt == wkbPolygon){ adminColl->addGeometry(used); }
            else if (gt == wkbMultiPolygon){ OGRGeometryCollection* gc = used->toGeometryCollection(); for (int i=0;i<gc->getNumGeometries();++i){ OGRGeometry* gi = gc->getGeometryRef(i); if (gi && wkbFlatten(gi->getGeometryType())==wkbPolygon) adminColl->addGeometry(gi); } }
            if (used != g) delete used;
        }
        OGRFeature::DestroyFeature(fA);
    }
    if (adminColl->IsEmpty()){ delete adminColl; GDALClose(dsAdmin); spdlog::error("行政区划面为空"); DropLogger(); return SE_ERROR_INVALID_GEOMETRY; }
    OGRGeometry* adminUnion = adminColl->UnionCascaded(); delete adminColl;
    if (!adminUnion || adminUnion->IsEmpty()){ GDALClose(dsAdmin); spdlog::error("行政区划面融合失败"); DropLogger(); return SE_ERROR_FAILURE; }
    OGREnvelope envContent; adminUnion->getEnvelope(&envContent);
    OGRPolygon* rectContent = MakeRectFromEnvelope(envContent);

    // 打开内图廓线
    GDALDataset* dsFrame = (GDALDataset*)GDALOpenEx(szInnerFrameShpFilePath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (!dsFrame){ delete rectContent; delete adminUnion; GDALClose(dsAdmin); spdlog::error("无法打开内图廓线: {}", fromQStringToUtf8(fromUtf8ToQString(szInnerFrameShpFilePath))); DropLogger(); return SE_ERROR_GDAL_OPEN_FAILED; }
    OGRLayer* lyrFrame = dsFrame->GetLayer(0); if (!lyrFrame){ GDALClose(dsFrame); delete rectContent; delete adminUnion; GDALClose(dsAdmin); spdlog::error("无法获取内图廓图层"); DropLogger(); return SE_ERROR_LAYER_NOT_FOUND; }

    // 计算内图廓的轴对齐包络并构矩形
    lyrFrame->ResetReading(); OGRFeature* fF; OGREnvelope envFrame; envFrame.MinX = envFrame.MinY = std::numeric_limits<double>::infinity(); envFrame.MaxX = envFrame.MaxY = -std::numeric_limits<double>::infinity();
    while ((fF = lyrFrame->GetNextFeature()) != NULL){ OGRGeometry* g = fF->GetGeometryRef(); if (g){ OGREnvelope e; g->getEnvelope(&e); envFrame.MinX = std::min(envFrame.MinX, e.MinX); envFrame.MinY = std::min(envFrame.MinY, e.MinY); envFrame.MaxX = std::max(envFrame.MaxX, e.MaxX); envFrame.MaxY = std::max(envFrame.MaxY, e.MaxY);} OGRFeature::DestroyFeature(fF);}    
    if (!std::isfinite(envFrame.MinX) || !std::isfinite(envFrame.MinY)){ GDALClose(dsFrame); delete rectContent; delete adminUnion; GDALClose(dsAdmin); spdlog::error("内图廓范围为空"); DropLogger(); return SE_ERROR_INVALID_GEOMETRY; }
    OGRPolygon* rectFrame = MakeRectFromEnvelope(envFrame);

    // 包络包含检查（范围超限）
    bool rangeError = false;
    if (envContent.MinX < envFrame.MinX || envContent.MaxX > envFrame.MaxX || envContent.MinY < envFrame.MinY || envContent.MaxY > envFrame.MaxY) rangeError = true;
    else { try { if (!rectFrame->Contains(rectContent)) rangeError = true; } catch(...) { rangeError = true; } }

    // 中心点与错位
    double cx = (envContent.MinX + envContent.MaxX) * 0.5;
    double cy = (envContent.MinY + envContent.MaxY) * 0.5;
    double fx = (envFrame.MinX + envFrame.MaxX) * 0.5;
    double fy = (envFrame.MinY + envFrame.MaxY) * 0.5;
    double dx = std::fabs(cx - fx);
    double dy = std::fabs(cy - fy);

    double tolUnits = MmToUnits(tolMM, nScale, srs);
    spdlog::info("中心偏差: Dx={} Dy={} (单位), 容差={} (单位)", dx, dy, tolUnits);
    bool centerError = (dx > tolUnits) || (dy > tolUnits);

    bool hasError = rangeError || centerError;

    // 错误点输出（两个中心点）
    if (hasError){
        GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
        if (drv){
            QString outQ = fromUtf8ToQString(szOutputErrorLayerPath);
            QFileInfo fi(outQ);
            QString baseNoExt = fi.path() + QDir::separator() + fi.completeBaseName();
            QStringList exts = {".shp", ".shx", ".dbf", ".prj", ".cpg"};
            for (const QString& ext : exts){ QFile::remove(baseNoExt + ext); }
            CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            QByteArray outNative = QFile::encodeName(QDir::toNativeSeparators(outQ));
            GDALDataset* dsOut = drv->Create(outNative.constData(), 0,0,0, GDT_Unknown, nullptr);
            if (dsOut){
                QString cpgPath = baseNoExt + ".cpg"; QFile cpgFile(cpgPath); if (cpgFile.open(QIODevice::WriteOnly|QIODevice::Text)) { cpgFile.write("UTF-8"); cpgFile.close(); }
                OGRSpatialReference* srsClone = srs ? srs->Clone() : nullptr;
                char** lco = nullptr; lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
                OGRLayer* lyr = dsOut->CreateLayer("frame_misalignment_errors", srsClone, wkbPoint, lco); CSLDestroy(lco); if (srsClone) srsClone->Release();
                if (lyr){
                    OGRFieldDefn fType("Type", OFTString); fType.SetWidth(32); lyr->CreateField(&fType);
                    OGRFieldDefn fDx("Dx", OFTReal); fDx.SetWidth(16); fDx.SetPrecision(6); lyr->CreateField(&fDx);
                    OGRFieldDefn fDy("Dy", OFTReal); fDy.SetWidth(16); fDy.SetPrecision(6); lyr->CreateField(&fDy);
                    OGRFieldDefn fTol("Tolerance", OFTReal); fTol.SetWidth(16); fTol.SetPrecision(6); lyr->CreateField(&fTol);
                    OGRFieldDefn fDesc("Description", OFTString); fDesc.SetWidth(128); lyr->CreateField(&fDesc);
                    // Content center
                    OGRFeature* fC = OGRFeature::CreateFeature(lyr->GetLayerDefn()); OGRPoint pc(cx, cy); fC->SetGeometry(&pc);
                    fC->SetField("Type", "ContentCenter"); fC->SetField("Dx", dx); fC->SetField("Dy", dy); fC->SetField("Tolerance", tolUnits);
                    fC->SetField("Description", QStringLiteral("地图内容中心").toUtf8().constData()); lyr->CreateFeature(fC); OGRFeature::DestroyFeature(fC);
                    // Frame center
                    OGRFeature* fF2 = OGRFeature::CreateFeature(lyr->GetLayerDefn()); OGRPoint pf(fx, fy); fF2->SetGeometry(&pf);
                    fF2->SetField("Type", "FrameCenter"); fF2->SetField("Dx", dx); fF2->SetField("Dy", dy); fF2->SetField("Tolerance", tolUnits);
                    fF2->SetField("Description", QStringLiteral("图廓中心").toUtf8().constData()); lyr->CreateFeature(fF2); OGRFeature::DestroyFeature(fF2);
                }
                GDALClose(dsOut);
                spdlog::info("错误点图层已生成: {}", fromQStringToUtf8(outQ));
            } else {
                spdlog::error("无法创建错误点输出: {}", fromQStringToUtf8(outQ));
            }
        } else {
            spdlog::error("无法获取Shapefile驱动");
        }
    }

    // 输出JSON
    QJsonObject item;
    item["id"] = "306";
    item["name"] = QString::fromUtf8("内容与图廓错位检查");
    item["status"] = hasError ? QString::fromUtf8("质检完成") : QString::fromUtf8("质检完成");
    item["err_exist"] = hasError ? QString::fromUtf8("true") : QString::fromUtf8("false");
    if (hasError){
        if (rangeError && centerError) item["info"] = QString::fromUtf8("存在地图内容溢出到图廓以外，且存在内容与图廓错位");
        else if (rangeError) item["info"] = QString::fromUtf8("存在地图内容溢出到图廓以外");
        else item["info"] = QString::fromUtf8("存在内容与图廓错位");
    } else {
        item["info"] = QString::fromUtf8("不存在内容与图廓错位或内容溢出");
    }
    QJsonObject root; root["check_item1"] = item;
    QJsonDocument doc(root);
    QFile jf(fromUtf8ToQString(szOutputJsonPath));
    if (jf.open(QIODevice::WriteOnly | QIODevice::Text)) { jf.write(doc.toJson(QJsonDocument::Compact)); jf.close(); spdlog::info("JSON结果已生成: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); }
    else { spdlog::error("无法创建JSON结果文件: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); delete rectFrame; delete rectContent; delete adminUnion; GDALClose(dsFrame); GDALClose(dsAdmin); DropLogger(); return SE_ERROR_WRITE_FILE_FAILED; }

    // 清理
    delete rectFrame;
    delete rectContent;
    delete adminUnion;
    GDALClose(dsFrame);
    GDALClose(dsAdmin);
    DropLogger();
    return SE_ERROR_NONE;
}
