#define _HAS_STD_BYTE 0
#include "se_feature_misplacement_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <cpl_string.h>
#include <cpl_conv.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>
#include <memory>
#include <cmath>

// 编码转换辅助函数
static QString fromUtf8ToQString(const char* utf8Str)
{
    return QString::fromUtf8(utf8Str);
}

static std::string fromQStringToUtf8(const QString& qstr)
{
    return qstr.toUtf8().toStdString();
}

// 清理日志器资源的辅助函数
static void CleanupLogger() {
    try {
        spdlog::drop("feature_misplacement_check");
        spdlog::set_default_logger(nullptr);
    } catch (...) {
        // 忽略清理异常
    }
}

// XML解析函数：获取缓冲区半径
SE_Error ParseXmlForBufferRadius(const char* szSchemaXmlPath, int scale, double& dBufferRadius)
{
    QFile file(fromUtf8ToQString(szSchemaXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        spdlog::error("无法打开XML文件: {}", fromQStringToUtf8(fromUtf8ToQString(szSchemaXmlPath)));
        return SE_ERROR_FAILURE;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn))
    {
        spdlog::error("XML解析失败: {} (行: {}, 列: {})", 
                     fromQStringToUtf8(errorMsg), errorLine, errorColumn);
        file.close();
        return SE_ERROR_CORRUPT_DATA;
    }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int j = 0; j < checkItems.count(); ++j)
    {
        QDomElement item = checkItems.at(j).toElement();
        QDomElement nameElement = item.firstChildElement("name");
        if (!nameElement.isNull() && nameElement.text() == "要素错绘检查")
        {
            QDomNodeList rules = item.elementsByTagName("rule");
            for (int k = 0; k < rules.count(); ++k)
            {
                QDomElement rule = rules.at(k).toElement();
                QDomElement ruleNameElement = rule.firstChildElement("name");
                if (!ruleNameElement.isNull() && ruleNameElement.text() == "缓冲区半径")
                {
                    QDomElement ruleValueElement = rule.firstChildElement("value");
                    QDomElement unitElement = rule.firstChildElement("unit");
                    if (!ruleValueElement.isNull())
                    {
                        bool ok;
                        dBufferRadius = ruleValueElement.text().toDouble(&ok);
                        if (ok && dBufferRadius > 0)
                        {
                            QString unit = unitElement.isNull() ? "毫米" : unitElement.text();
                            spdlog::info("从XML中解析到缓冲区半径: {} {}", dBufferRadius, unit.toStdString());
                            return SE_ERROR_NONE;
                        }
                    }
                }
            }
        }
    }
    dBufferRadius = 3.0;
    spdlog::warn("未在XML中找到要素错绘检查缓冲区半径配置，使用默认值: {} 毫米", dBufferRadius);
    return SE_ERROR_NONE;
}

// 主要的要素错绘检查算法
SE_Error FeatureMisplacementCheck(
    const char* szSourceShpFilePath,
    const char* szParsedShpFilePath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath)
{
    // 初始化日志
    try {
        // 先清理可能存在的同名日志器
        auto existing_logger = spdlog::get("feature_misplacement_check");
        if (existing_logger) {
            spdlog::drop("feature_misplacement_check");
        }
        
        // 处理中文路径（使用本地编码，避免中文文件名乱码）
        QString logPathQ = fromUtf8ToQString(szOutputLogFilePath);
        QByteArray logPathLocalBytes = logPathQ.toLocal8Bit();
        std::string logFilePath(logPathLocalBytes.constData(), logPathLocalBytes.size());
        auto logger = spdlog::basic_logger_mt("feature_misplacement_check", logFilePath);
        spdlog::set_default_logger(logger);
        spdlog::set_level(spdlog::level::info);
        spdlog::flush_on(spdlog::level::info);
        spdlog::info("开始要素错绘检查算法");
    }
    catch (const std::exception&) {
        return SE_ERROR_FAILURE;
    }

    // JSON写出辅助函数（按指定格式）
    auto writeJsonResult = [&](bool success, bool errExist, const QString& layerName) {
        QJsonObject checkItem;
        checkItem["err_exist"] = errExist ? "true" : "false";
        checkItem["id"] = "103";
        checkItem["name"] = "要素错绘检查";
        checkItem["status"] = success ? "质检完成" : "质检失败";
        if (success) {
            QString info = errExist ? QString("%1图层存在要素错绘").arg(layerName)
                                    : QString("%1图层不存在要素错绘").arg(layerName);
            checkItem["info"] = info.toStdString().c_str();
        } else {
            QString info = QString("%1图层质检失败").arg(layerName);
            checkItem["info"] = info.toStdString().c_str();
        }

        QJsonObject root;
        root["check_item"] = checkItem;
        QJsonDocument doc(root);
        QFile jsonFile(fromUtf8ToQString(szOutputJsonPath));
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            jsonFile.write(doc.toJson(QJsonDocument::Compact));
            jsonFile.close();
            spdlog::info("JSON结果已生成: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        } else {
            spdlog::error("无法创建JSON结果文件: {}", fromQStringToUtf8(fromUtf8ToQString(szOutputJsonPath)));
        }
    };

    // 解析XML获取缓冲区半径
    double dBufferRadius = 3.0;
    SE_Error xmlResult = ParseXmlForBufferRadius(szSchemaXmlPath, scale, dBufferRadius);
    if (xmlResult != SE_ERROR_NONE)
    {
        spdlog::error("XML解析失败");
        QString layerName = QFileInfo(fromUtf8ToQString(szSchemaXmlPath)).baseName();
        writeJsonResult(false, false, layerName);
        CleanupLogger();
        return xmlResult;
    }

    // 注册GDAL驱动
    GDALAllRegister();
    OGRRegisterAll();

    // 打开源数据（库体数据）
    GDALDataset* poSourceDS = (GDALDataset*)GDALOpenEx(szSourceShpFilePath, GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (poSourceDS == NULL)
    {
        spdlog::error("无法打开源数据文件: {}", fromQStringToUtf8(fromUtf8ToQString(szSourceShpFilePath)));
        QString layerName = QFileInfo(fromUtf8ToQString(szSchemaXmlPath)).baseName();
        writeJsonResult(false, false, layerName);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    OGRLayer* poSourceLayer = poSourceDS->GetLayer(0);
    if (poSourceLayer == NULL)
    {
        spdlog::error("无法获取图层");
        GDALClose(poSourceDS);
        QString layerName = QFileInfo(fromUtf8ToQString(szSchemaXmlPath)).baseName();
        writeJsonResult(false, false, layerName);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    const char* pszDriverName = "ESRI Shapefile";
    GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName(pszDriverName);
    if (poDriver == NULL)
    {
        spdlog::error("无法获取Shapefile驱动");
        GDALClose(poSourceDS);
        QString layerName = QFileInfo(fromUtf8ToQString(szSourceShpFilePath)).baseName();
        writeJsonResult(false, false, layerName);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }
    OGRSpatialReference* poSRS = poSourceLayer->GetSpatialRef();

    // 统计变量
    int nTotalParsedFeatures = 0;
    int nErrorFeatures = 0;
    double dTotalErrorLength = 0.0;
    double dTotalErrorArea = 0.0;

    // 创建源数据的缓冲区
    std::vector<OGRGeometry*> sourceBuffers;
    poSourceLayer->ResetReading();
    OGRFeature* poSourceFeature;
    
    spdlog::info("开始创建源数据缓冲区，半径: {}", dBufferRadius);
    
    while ((poSourceFeature = poSourceLayer->GetNextFeature()) != NULL)
    {
        OGRGeometry* poGeometry = poSourceFeature->GetGeometryRef();
        if (poGeometry != NULL)
        {
            OGRGeometry* poBuffer = poGeometry->Buffer(dBufferRadius);
            if (poBuffer != NULL)
            {
                sourceBuffers.push_back(poBuffer);
            }
        }
        OGRFeature::DestroyFeature(poSourceFeature);
    }
    
    spdlog::info("创建了 {} 个源数据缓冲区", sourceBuffers.size());
    
    if (sourceBuffers.empty())
    {
        spdlog::error("源数据中没有有效的几何要素");
        // 清理缓冲区几何对象
        for (size_t i = 0; i < sourceBuffers.size(); ++i)
        {
            delete sourceBuffers[i];
        }
        sourceBuffers.clear();
        GDALClose(poSourceDS);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    std::vector<std::string> parsedFiles;
    {
        std::string paths(szParsedShpFilePath);
        size_t pos = 0;
        while ((pos = paths.find(';')) != std::string::npos) {
            std::string token = paths.substr(0, pos);
            if (!token.empty()) parsedFiles.push_back(token);
            paths.erase(0, pos + 1);
        }
        if (!paths.empty()) parsedFiles.push_back(paths);
    }

    if (parsedFiles.empty())
    {
        spdlog::error("解析数据路径为空");
        GDALClose(poSourceDS);
        QString layerName = QFileInfo(fromUtf8ToQString(szSchemaXmlPath)).baseName();
        writeJsonResult(false, false, layerName);
        CleanupLogger();
        return SE_ERROR_FAILURE;
    }

    int totalParsedFeatures = 0;
    int totalErrorFeatures = 0;

    QFileInfo outInfo(fromUtf8ToQString(szOutputCheckResultLayerPath));
    QString outDir = outInfo.dir().absolutePath();
    QString outBase = outInfo.completeBaseName();

    for (const auto& parsedPath : parsedFiles)
    {
        GDALDataset* poParsedDS = (GDALDataset*)GDALOpenEx(parsedPath.c_str(), GDAL_OF_VECTOR, NULL, NULL, NULL);
        if (!poParsedDS)
        {
            spdlog::error("无法打开解析数据文件: {}", parsedPath);
            continue;
        }
        OGRLayer* poParsedLayer = poParsedDS->GetLayer(0);
        if (!poParsedLayer)
        {
            spdlog::error("无法获取解析数据图层: {}", parsedPath);
            GDALClose(poParsedDS);
            continue;
        }

        poParsedLayer->ResetReading();
        OGRFeature* poParsedFeature;
        int fileParsedFeatures = 0;
        int fileErrorFeatures = 0;

        std::vector<OGRFeature*> errorFeatures;
        while ((poParsedFeature = poParsedLayer->GetNextFeature()) != NULL)
        {
            fileParsedFeatures++;
            OGRGeometry* poParsedGeometry = poParsedFeature->GetGeometryRef();
            if (poParsedGeometry)
            {
                bool bWithinBuffer = false;
                for (size_t i = 0; i < sourceBuffers.size(); ++i)
                {
                    if (poParsedGeometry->Intersects(sourceBuffers[i]))
                    {
                        bWithinBuffer = true;
                        break;
                    }
                }
                if (!bWithinBuffer)
                {
                    fileErrorFeatures++;
                    errorFeatures.push_back(poParsedFeature->Clone());
                }
            }
            OGRFeature::DestroyFeature(poParsedFeature);
        }

        totalParsedFeatures += fileParsedFeatures;
        totalErrorFeatures += fileErrorFeatures;

        if (fileErrorFeatures > 0)
        {
            QFileInfo parsedInfo(fromUtf8ToQString(parsedPath.c_str()));
            QString parsedBase = parsedInfo.baseName();
            QString finalPath = outDir + QDir::separator() + outBase + "_" + parsedBase + ".shp";
            if (QFile::exists(finalPath))
            {
                poDriver->Delete(finalPath.toStdString().c_str());
            }
            CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
            GDALDataset* poOutputDS = poDriver->Create(finalPath.toStdString().c_str(), 0, 0, 0, GDT_Unknown, NULL);
            if (poOutputDS)
            {
                char** lco = nullptr;
                lco = CSLSetNameValue(lco, "ENCODING", "UTF-8");
                OGRLayer* poOutputLayer = poOutputDS->CreateLayer("misplacement_errors", poSRS, wkbUnknown, lco);
                CSLDestroy(lco);
                if (poOutputLayer)
                {
                    OGRFieldDefn oFieldType("ErrorType", OFTString); oFieldType.SetWidth(50); poOutputLayer->CreateField(&oFieldType);
                    OGRFieldDefn oFieldDesc("Description", OFTString); oFieldDesc.SetWidth(200); poOutputLayer->CreateField(&oFieldDesc);
                    OGRFieldDefn oFieldValue("ErrorValue", OFTReal); poOutputLayer->CreateField(&oFieldValue);

                    for (auto* featClone : errorFeatures)
                    {
                        OGRGeometry* g = featClone->GetGeometryRef();
                        OGRFeature* poErrorFeature = OGRFeature::CreateFeature(poOutputLayer->GetLayerDefn());
                        poErrorFeature->SetGeometry(g);
                        OGRwkbGeometryType gt = g ? g->getGeometryType() : wkbUnknown;
                        if (gt == wkbLineString || gt == wkbMultiLineString)
                        {
                            double length = 0.0;
                            if (gt == wkbLineString) length = ((OGRLineString*)g)->get_Length();
                            else {
                                OGRMultiLineString* ml = (OGRMultiLineString*)g;
                                for (int i = 0; i < ml->getNumGeometries(); ++i) length += ((OGRLineString*)ml->getGeometryRef(i))->get_Length();
                            }
                            poErrorFeature->SetField("ErrorType", fromQStringToUtf8(QStringLiteral("长度超限")).c_str());
                            poErrorFeature->SetField("Description", fromQStringToUtf8(QStringLiteral("线要素不在库体数据缓冲区内")).c_str());
                            poErrorFeature->SetField("ErrorValue", length);
                        }
                        else if (gt == wkbPolygon || gt == wkbMultiPolygon)
                        {
                            double area = 0.0;
                            if (gt == wkbPolygon) area = ((OGRPolygon*)g)->get_Area();
                            else {
                                OGRMultiPolygon* mp = (OGRMultiPolygon*)g;
                                for (int i = 0; i < mp->getNumGeometries(); ++i) area += ((OGRPolygon*)mp->getGeometryRef(i))->get_Area();
                            }
                            poErrorFeature->SetField("ErrorType", fromQStringToUtf8(QStringLiteral("面积超限")).c_str());
                            poErrorFeature->SetField("Description", fromQStringToUtf8(QStringLiteral("面要素不在库体数据缓冲区内")).c_str());
                            poErrorFeature->SetField("ErrorValue", area);
                        }
                        poOutputLayer->CreateFeature(poErrorFeature);
                        OGRFeature::DestroyFeature(poErrorFeature);
                    }
                }
                GDALClose(poOutputDS);
                spdlog::info("错误图层已生成: {}", fromQStringToUtf8(finalPath));
            }
        }

        for (auto* f : errorFeatures) OGRFeature::DestroyFeature(f);
        GDALClose(poParsedDS);
    }

    for (size_t i = 0; i < sourceBuffers.size(); ++i) delete sourceBuffers[i];
    sourceBuffers.clear();

    {
        QString layerName = QFileInfo(fromUtf8ToQString(szSchemaXmlPath)).baseName();
        bool errExist = (totalErrorFeatures > 0);
        writeJsonResult(true, errExist, layerName);
    }

    GDALClose(poSourceDS);
    spdlog::info("要素错绘检查完成，共检查 {} 个要素，发现 {} 个错绘要素", totalParsedFeatures, totalErrorFeatures);
    CleanupLogger();
    return SE_ERROR_NONE;
}
