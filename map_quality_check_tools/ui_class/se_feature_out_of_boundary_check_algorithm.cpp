#define _HAS_STD_BYTE 0
#include "se_feature_out_of_boundary_check_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDomDocument>
#include <cpl_string.h>
#include <cpl_conv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>
#include <string>
#include <memory>

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

static SE_Error ReadConnectionRule(const char* xmlPath, int scale, bool& allowConnection)
{
    allowConnection = true;
    if (!xmlPath || !*xmlPath || scale <= 0) return SE_ERROR_INVALID_PARAMETER;
    QFile file(fromUtf8ToQString(xmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return SE_ERROR_FILE_NOT_FOUND;
    QDomDocument doc;
    if (!doc.setContent(&file))
        return SE_ERROR_XML_PARSE_FAILED;
    const QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list" && root.tagName() != "check_item")
        return SE_ERROR_XML_PARSE_FAILED;
    QDomElement exactScale, emptyScale;
    if (root.tagName() == "check_list")
    {
        const QDomNodeList scales = root.elementsByTagName("scale");
        for (int i = 0; i < scales.count(); ++i)
        {
            QDomElement element = scales.at(i).toElement();
            const QString value = element.firstChildElement("value").text().trimmed();
            bool ok = false;
            const int parsedScale = value.toInt(&ok);
            if (ok && parsedScale == scale && exactScale.isNull()) exactScale = element;
            if (value.isEmpty() && emptyScale.isNull()) emptyScale = element;
        }
    }
    // 已匹配比例尺时不借用其他比例尺的规则；兼容根节点下直接配置检查项。
    const QDomElement container = !exactScale.isNull() ? exactScale : (!emptyScale.isNull() ? emptyScale : root);
    for (QDomElement item = root.tagName() == "check_item" ? root : container.firstChildElement("check_item");
         !item.isNull();
         item = root.tagName() == "check_item" ? QDomElement() : item.nextSiblingElement("check_item"))
    {
        if (item.firstChildElement("name").text().trimmed() != QStringLiteral("要素超出边界检查")) continue;
        bool found = false;
        for (QDomElement rule = item.firstChildElement("rule"); !rule.isNull(); rule = rule.nextSiblingElement("rule"))
        {
            if (rule.firstChildElement("name").text().trimmed() != QStringLiteral("是否允许要素与图廓连接")) continue;
            const QString value = rule.firstChildElement("value").text().trimmed();
            if (found || (value != QStringLiteral("是") && value != QStringLiteral("否"))) return SE_ERROR_XML_PARSE_FAILED;
            found = true;
            allowConnection = value == QStringLiteral("是");
        }
        return found ? SE_ERROR_NONE : SE_ERROR_XML_PARSE_FAILED;
    }
    return SE_ERROR_XML_PARSE_FAILED;
}

SE_Error FeatureOutOfBoundaryCheck(
    const char* szBoundaryShpFilePath,
    const char* szFeatureShpPaths,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath,
    const char* szSchemaXmlPath,
    int scale)
{
    if (!szSchemaXmlPath || !*szSchemaXmlPath || scale <= 0) return SE_ERROR_INVALID_PARAMETER;
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
    bool allowConnection = true;
    const SE_Error ruleError = ReadConnectionRule(szSchemaXmlPath, scale, allowConnection);
    if (ruleError != SE_ERROR_NONE)
    {
        spdlog::error("读取是否允许要素与图廓连接规则失败，错误码：{}", ruleError);
        DropLogger();
        return ruleError;
    }
    spdlog::info("是否允许要素与图廓连接：{}", allowConnection ? "是" : "否");

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
        std::unique_ptr<OGRGeometry> polygons(lineColl->Polygonize());
        OGRMultiPolygon faces;
        if (polygons && wkbFlatten(polygons->getGeometryType()) == wkbGeometryCollection)
        {
            auto* collection = static_cast<OGRGeometryCollection*>(polygons.get());
            for (int i = 0; i < collection->getNumGeometries(); ++i)
            {
                OGRGeometry* face = collection->getGeometryRef(i);
                if (face && wkbFlatten(face->getGeometryType()) == wkbPolygon) faces.addGeometry(face);
            }
        }
        else if (polygons && wkbFlatten(polygons->getGeometryType()) == wkbPolygon)
        {
            faces.addGeometry(polygons.get());
        }
        if (faces.getNumGeometries() > 0) boundaryUnion = faces.UnionCascaded();
        // 防止构面时静默丢弃未闭合线段、悬挂线或额外分支。
        if (boundaryUnion)
        {
            std::unique_ptr<OGRGeometry> edge(boundaryUnion->Boundary());
            std::unique_ptr<OGRGeometry> unused(edge ? lineColl->Difference(edge.get()) : nullptr);
            if (!unused || !unused->IsEmpty()) { delete boundaryUnion; boundaryUnion = nullptr; }
        }
    }
    delete polyColl;
    delete lineColl;
    if (!boundaryUnion || boundaryUnion->IsEmpty()){
        delete boundaryUnion;
        GDALClose(dsBoundary);
        spdlog::error("内图廓无法形成有效闭合面，请检查未闭合线段或多余分支；不使用外包矩形替代");
        DropLogger();
        return SE_ERROR_INVALID_GEOMETRY;
    }
    if (!boundaryUnion->IsValid()){
        OGRGeometry* v = boundaryUnion->MakeValid();
        if (!v) v = boundaryUnion->Buffer(0);
        if (v && !v->IsEmpty()){ delete boundaryUnion; boundaryUnion = v; }
    }
    std::unique_ptr<OGRGeometry> boundaryLine(boundaryUnion->Boundary());
    if (!boundaryUnion->IsValid() || !boundaryLine || boundaryLine->IsEmpty())
    {
        delete boundaryUnion; GDALClose(dsBoundary); DropLogger(); return SE_ERROR_INVALID_GEOMETRY;
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
        // DBF字段名最多10字符，与旧文件实际截断后的字段名一致。
        OGRFieldDefn fDesc("Descriptio", OFTString); fDesc.SetWidth(128); lyrOut->CreateField(&fDesc);
    }

    bool hasError = false;
    int outsideCount = 0, connectionCount = 0;
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
                bool connected = false;
                if (within && !allowConnection)
                {
                    CPLErrorReset();
                    connected = used->Intersects(boundaryLine.get());
                    if (CPLGetLastErrorType() >= CE_Failure)
                    {
                        spdlog::error("要素FID={} 与内图廓接触判断失败", pf->GetFID());
                        if (used != g) delete used;
                        OGRFeature::DestroyFeature(pf); GDALClose(dsFeat); GDALClose(dsOut);
                        delete boundaryUnion; GDALClose(dsBoundary); DropLogger(); return SE_ERROR_INVALID_GEOMETRY;
                    }
                }
                if (!within || connected){
                    hasError = true;
                    if (!within) ++outsideCount; else ++connectionCount;
                    OGRFeature* outF = OGRFeature::CreateFeature(lyrOut->GetLayerDefn());
                    outF->SetGeometry(used);
                    outF->SetField("LayerName", layerName.c_str());
                    outF->SetField("FeatureID", pf->GetFID());
                    outF->SetField("ErrorType", fromQStringToUtf8(!within ? QStringLiteral("要素超出内图廓范围") : QStringLiteral("要素与内图廓连接")).c_str());
                    outF->SetField("Descriptio", fromQStringToUtf8(!within ? QStringLiteral("要素不完全位于内图廓图层内") : QStringLiteral("规则不允许要素接触或重合内图廓线")).c_str());
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
    item["id"] = "307";
    item["name"] = QString::fromUtf8("要素超出边界检查");
    item["status"] = hasError ? QString::fromUtf8("质检完成") : QString::fromUtf8("质检完成");
    item["err_exist"] = hasError ? QString::fromUtf8("true") : QString::fromUtf8("false");
    item["info"] = hasError ? QString("存在%1个超界要素、%2个与内图廓连接要素").arg(outsideCount).arg(connectionCount)
                            : QString::fromUtf8("不存在超界或不允许的图廓连接问题");
    QJsonObject root; root["check_item1"] = item;
    QJsonDocument doc(root);
    QFile jf(fromUtf8ToQString(szOutputJsonPath));
    if (jf.open(QIODevice::WriteOnly | QIODevice::Text)) { jf.write(doc.toJson(QJsonDocument::Compact)); jf.close(); spdlog::info("JSON结果已生成: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); }
    else { spdlog::error("无法创建JSON结果文件: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath))); DropLogger(); return SE_ERROR_WRITE_FILE_FAILED; }

    DropLogger();
    return SE_ERROR_NONE;
}

