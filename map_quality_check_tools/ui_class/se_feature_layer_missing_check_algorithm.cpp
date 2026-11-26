#define _HAS_STD_BYTE 0
#include "se_feature_layer_missing_check_algorithm.h"

// 使用头文件中的错误码定义

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QTextCodec>
#include <QJsonDocument>
#include <QJsonObject>
#include <vector>
#include <memory>
#include <cmath>
#include <QtCore/QDirIterator>

// 将UTF-8字符串转换为本地编码
static QString toLocalEncoding(const char* utf8Str)
{
    // 输入的 char* 按 /utf-8 编译，统一按 UTF-8 解析
    return QString::fromUtf8(utf8Str);
}

// 将本地编码字符串转换为UTF-8
static std::string fromLocalEncoding(const QString& localStr)
{
    QByteArray utf8Bytes = localStr.toUtf8();
    return std::string(utf8Bytes.constData(), utf8Bytes.length());
}

// 写JSON结果
static void WriteJsonResult(const char* szOutputJsonPath, const QString& xmlBaseName, bool success, bool layerExists)
{
    QJsonObject item;
    item["id"] = "101";
    item["name"] = QStringLiteral("要素层缺失检查");
    item["status"] = success ? QStringLiteral("质检完成") : QStringLiteral("质检失败");
    item["err_exist"] = layerExists ? QStringLiteral("false") : QStringLiteral("true");
    item["info"] = QString("%1图层要素层%2").arg(xmlBaseName).arg(layerExists ? QStringLiteral("存在") : QStringLiteral("缺失"));

    QJsonObject root;
    root["check_item"] = item;
    QJsonDocument doc(root);

    // 确保路径按 UTF-8 解析，并在必要时创建输出目录
    QFileInfo jsonInfo(toLocalEncoding(szOutputJsonPath));
    QDir().mkpath(jsonInfo.dir().absolutePath());
    QFile jsonFile(jsonInfo.absoluteFilePath());
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Compact));
        jsonFile.close();
    }
}

// XML解析函数：获取要素层缺失检查的缓冲区半径
SE_Error ParseXmlForLayerMissingBufferRadius(const char* szSchemaXmlPath, double& dBufferRadius, int nScale)
{
    QFile file(toLocalEncoding(szSchemaXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn))
    {
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int j = 0; j < checkItems.count(); ++j)
    {
        QDomElement checkItem = checkItems.at(j).toElement();
        QDomElement nameElement = checkItem.firstChildElement("name");
        if (!nameElement.isNull() && nameElement.text() == "要素层缺失检查")
        {
            QDomNodeList rules = checkItem.elementsByTagName("rule");
            for (int k = 0; k < rules.count(); ++k)
            {
                QDomElement rule = rules.at(k).toElement();
                QDomElement ruleNameElement = rule.firstChildElement("name");
                if (!ruleNameElement.isNull() && ruleNameElement.text() == "缓冲区半径")
                {
                    QDomElement ruleValueElement = rule.firstChildElement("value");
                    if (!ruleValueElement.isNull())
                    {
                        bool ok;
                        dBufferRadius = ruleValueElement.text().toDouble(&ok);
                        if (ok)
                        {
                            return SE_ERROR_NONE;
                        }
                    }
                }
            }
        }
    }
    dBufferRadius = 0.3;
    return SE_ERROR_NONE;
}

// 要素层缺失检查主函数（输出JSON）
SE_Error FeatureLayerMissingCheck(
    const char* szSourceShpFilePath,
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    int nScale)
{
    // 初始化GDAL
    GDALAllRegister();
    // 强制使用 UTF-8 解释传入的文件名（包含中文时避免失败）
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");

    // 解析XML获取缓冲区半径
    double dBufferRadius = 0.3;
    SE_Error xmlResult = ParseXmlForLayerMissingBufferRadius(szSchemaXmlPath, dBufferRadius, nScale);

    // 取XML文件名作为信息中的图层名
    QString xmlPath = toLocalEncoding(szSchemaXmlPath);
    QFileInfo xmlInfo(xmlPath);
    QString xmlBaseName = xmlInfo.baseName();

    bool bLayerExists = false; // 本地变量替代输出参数

    if (xmlResult != SE_ERROR_NONE)
    {
        WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
        return xmlResult;
    }

    // 打开库体数据源（SHP）
    QString sourcePath = toLocalEncoding(szSourceShpFilePath);
    GDALDataset* poSourceDS = (GDALDataset*)GDALOpenEx(sourcePath.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
    if (poSourceDS == NULL)
    {
        WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
        return SE_ERROR_GDAL_OPEN_FAILED;
    }

    // 获取第一个图层作为库体数据图层
    OGRLayer* poSourceLayer = poSourceDS->GetLayer(0);
    if (poSourceLayer == NULL)
    {
        GDALClose(poSourceDS);
        WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
        return SE_ERROR_LAYER_NOT_FOUND;
    }

    // 解析多个文件路径（分号分隔）或单个文件
    QString strParsedDataPaths = toLocalEncoding(szParsedDataPath);
    QStringList shpFiles;
    QStringList parsedFilePaths;

    auto collectShpFilesFromDir = [&](const QString& dirPath) {
        QDir dir(dirPath);
        if (!dir.exists()) return;
        QDirIterator it(dirPath, QStringList() << "*.shp", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString shpPath = it.next();
            QFileInfo fi(shpPath);
            if (fi.exists() && fi.isFile() && fi.suffix().toLower() == "shp") {
                parsedFilePaths << fi.absoluteFilePath();
                shpFiles << fi.fileName();
            }
        }
    };

    if (strParsedDataPaths.contains(";"))
    {
        QStringList pathList = strParsedDataPaths.split(";", Qt::SkipEmptyParts);
        for (const QString& path : pathList)
        {
            QString trimmedPath = path.trimmed();
            QFileInfo fileInfo(trimmedPath);
            if (fileInfo.exists() && fileInfo.isFile() && fileInfo.suffix().toLower() == "shp")
            {
                parsedFilePaths << fileInfo.absoluteFilePath();
                shpFiles << fileInfo.fileName();
            }
            else if (fileInfo.exists() && fileInfo.isDir())
            {
                collectShpFilesFromDir(fileInfo.absoluteFilePath());
            }
            else
            {
                GDALClose(poSourceDS);
                WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
    }
    else
    {
        QFileInfo parsedInfo(toLocalEncoding(szParsedDataPath));
        if (parsedInfo.exists() && parsedInfo.isFile() && parsedInfo.suffix().toLower() == "shp")
        {
            parsedFilePaths << parsedInfo.absoluteFilePath();
            shpFiles << parsedInfo.fileName();
        }
        else if (parsedInfo.exists() && parsedInfo.isDir())
        {
            collectShpFilesFromDir(parsedInfo.absoluteFilePath());
            if (parsedFilePaths.isEmpty())
            {
                GDALClose(poSourceDS);
                WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else
        {
            GDALClose(poSourceDS);
            WriteJsonResult(szOutputJsonPath, xmlBaseName, false, false);
            return SE_ERROR_FILE_NOT_FOUND;
        }
    }

    int nTotalSourceFeatures = 0;
    int nFoundFeatures = 0;

    // 遍历库体数据的要素
    poSourceLayer->ResetReading();
    OGRFeature* poSourceFeature;

    while ((poSourceFeature = poSourceLayer->GetNextFeature()) != NULL)
    {
        nTotalSourceFeatures++;
        OGRGeometry* poSourceGeom = poSourceFeature->GetGeometryRef();
        if (poSourceGeom == NULL || !poSourceGeom->IsValid())
        {
            OGRFeature::DestroyFeature(poSourceFeature);
            continue;
        }

        // 创建缓冲区（毫米 -> 地图单位）。假设地图单位为米：r = (mm/1000) * nScale
        double dBufferRadiusInMapUnits = (dBufferRadius / 1000.0) * (double)nScale;
        OGRGeometry* poBuffer = poSourceGeom->Buffer(dBufferRadiusInMapUnits);
        if (poBuffer == NULL)
        {
            OGRFeature::DestroyFeature(poSourceFeature);
            continue;
        }

        bool bFeatureFound = false;
        for (const QString& fullPath : parsedFilePaths)
        {
            GDALDataset* poParsedDS = (GDALDataset*)GDALOpenEx(fullPath.toUtf8().constData(), GDAL_OF_VECTOR, NULL, NULL, NULL);
            if (poParsedDS == NULL)
            {
                continue;
            }

            OGRLayer* poParsedLayer = poParsedDS->GetLayer(0);
            if (poParsedLayer == NULL)
            {
                GDALClose(poParsedDS);
                continue;
            }

            poParsedLayer->SetSpatialFilter(poBuffer);
            poParsedLayer->ResetReading();

            OGRFeature* poParsedFeature;
            while ((poParsedFeature = poParsedLayer->GetNextFeature()) != NULL)
            {
                OGRGeometry* poParsedGeom = poParsedFeature->GetGeometryRef();
                if (poParsedGeom != NULL && poParsedGeom->IsValid())
                {
                    if (poBuffer->Intersects(poParsedGeom) || poBuffer->Contains(poParsedGeom))
                    {
                        bFeatureFound = true;
                        OGRFeature::DestroyFeature(poParsedFeature);
                        break;
                    }
                }
                OGRFeature::DestroyFeature(poParsedFeature);
            }

            GDALClose(poParsedDS);

            if (bFeatureFound)
            {
                break;
            }
        }

        if (bFeatureFound)
        {
            nFoundFeatures++;
            bLayerExists = true;
        }

        delete poBuffer;
        OGRFeature::DestroyFeature(poSourceFeature);

        if (bLayerExists)
        {
            break;
        }
    }

    // 输出JSON
    WriteJsonResult(szOutputJsonPath, xmlBaseName, true, bLayerExists);

    // 清理资源
    GDALClose(poSourceDS);

    return SE_ERROR_NONE;
}