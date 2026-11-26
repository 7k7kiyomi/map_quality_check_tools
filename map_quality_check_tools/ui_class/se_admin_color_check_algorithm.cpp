#define _HAS_STD_BYTE 0
#include "se_admin_color_check_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <set>
#include <map>
#include <string>

// 将UTF-8字符串转换为本地编码
static QString toLocalEncoding(const char* utf8Str)
{
    return QString::fromUtf8(utf8Str);
}

// 将本地编码字符串转换为UTF-8
static std::string fromLocalEncoding(const QString& localStr)
{
    QByteArray utf8Bytes = localStr.toUtf8();
    return std::string(utf8Bytes.constData(), utf8Bytes.length());
}

// 写JSON结果
static void WriteJsonResult(
    const char* szOutputJsonPath, 
    bool success,
    bool passed,
    int maxColorCount,
    int actualColorCount,
    bool hasAdjacentSameColor,
    const std::string& errorDescription = ""
) {
    QJsonObject item;
    item["id"] = "303";
    item["name"] = QStringLiteral("行政区划颜色检查");
    
    if (!success) {
        item["status"] = QStringLiteral("质检失败");
    } else {
        item["status"] = QStringLiteral("质检成功");
    }
    
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");
    
    // info 按照 check_item3 格式
    if (passed) {
        item["info"] = QStringLiteral("行政区划图层不存在颜色错误");
    } else {
        item["info"] = QStringLiteral("行政区划图层存在颜色错误");
    }

    // 如果检查未通过，添加错误信息（按照 check_item3 格式）
    if (!passed) {
        QJsonObject errors;
        int errorIndex = 1;
        
        if (actualColorCount > maxColorCount) {
            QString errorKey = QString("error%1").arg(errorIndex++);
            errors[errorKey] = QString(QStringLiteral("颜色数量超限,实际使用%1个颜色,超过限制%2个"))
                .arg(actualColorCount).arg(maxColorCount);
        }
        
        if (hasAdjacentSameColor) {
            QString errorKey = QString("error%1").arg(errorIndex++);
            errors[errorKey] = QStringLiteral("相邻要素使用相同颜色");
        }
        
        if (!errorDescription.empty()) {
            QString errorKey = QString("error%1").arg(errorIndex++);
            errors[errorKey] = QString::fromStdString(errorDescription);
        }
        
        item["errors"] = errors;
    }

    QJsonObject root;
    root["check_item"] = item;
    // 额外提供布尔化字段，便于外部系统集成判定
    root["ok"] = success;
    root["passed"] = passed;

    QJsonDocument doc(root);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
}

SE_Error ParseXmlForAdminColor(const char* szSchemaXmlPath, int& nMaxColorCount)
{
    QFile file(szSchemaXmlPath);
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

    // 解析XML，查找颜色数量配置
    // XML结构: <check_item><rule><name>颜色</name><value>4</value></rule></check_item>
    QDomElement root = doc.documentElement();
    QDomNodeList items = root.elementsByTagName("check_item");
    for (int i = 0; i < items.count(); ++i)
    {
        QDomElement item = items.at(i).toElement();
        
        // 直接在check_item下查找rule元素，不再判断name
        QDomElement ruleElement = item.firstChildElement("rule");
        if (!ruleElement.isNull())
        {
            // 在rule下查找value元素
            QDomElement valueElement = ruleElement.firstChildElement("value");
            if (!valueElement.isNull())
            {
                bool ok;
                int count = valueElement.text().toInt(&ok);
                if (ok && count > 0)
                {
                    nMaxColorCount = count;
                    return SE_ERROR_NONE;
                }
            }
        }
    }

    return SE_ERROR_XML_PARSE_FAILED;
}

SE_Error AdminColorCheck(
    const char* szShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed)
{
    bPassed = false;
    int maxColorCount = 0;
    int actualColorCount = 0;
    bool hasAdjacentSameColor = false;

    // 1. 解析XML获取配置
    SE_Error error = ParseXmlForAdminColor(szSchemaXmlPath, maxColorCount);
    if (error != SE_ERROR_NONE)
    {
        WriteJsonResult(szOutputJsonPath, false, false, 0, 0, false, 
            "Failed to parse XML configuration");
        return error;
    }

    // 2. 检查文件是否存在
    QString qPath = QString::fromUtf8(szShpPath);
    QFileInfo fileInfo(qPath);
    
    if (!fileInfo.exists())
    {
        std::string errorMsg = "Shapefile not found: ";
        errorMsg += szShpPath;
        WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }
    
    // 检查必需的相关文件
    QString basePath = fileInfo.path() + "/" + fileInfo.completeBaseName();
    QFileInfo shxInfo(basePath + ".shx");
    QFileInfo dbfInfo(basePath + ".dbf");
    
    if (!shxInfo.exists())
    {
        std::string errorMsg = "Missing .shx file: ";
        errorMsg += shxInfo.filePath().toUtf8().constData();
        WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }
    
    if (!dbfInfo.exists())
    {
        std::string errorMsg = "Missing .dbf file: ";
        errorMsg += dbfInfo.filePath().toUtf8().constData();
        WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 3. 注册GDAL驱动
    GDALAllRegister();
    
    // 设置编码以支持中文路径（Windows系统）
    #ifdef _WIN32
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
    #else
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");
    CPLSetConfigOption("SHAPE_ENCODING", "");
    #endif

    // 4. 打开数据集
    // 将QString路径转换为本地编码
    std::string localPath;
    
    #ifdef _WIN32
    // Windows: 使用UTF-8编码
    localPath = qPath.toUtf8().constData();
    #else
    // Linux/Mac: 使用本地编码
    localPath = qPath.toLocal8Bit().constData();
    #endif
    
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
        localPath.c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    );

    if (poDS == nullptr)
    {
        // 尝试使用原始路径再次打开
        poDS = (GDALDataset*)GDALOpenEx(
            szShpPath,
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr
        );
        
        if (poDS == nullptr)
        {
            // 获取详细错误信息
            const char* errMsg = CPLGetLastErrorMsg();
            std::string errorDetail = "Failed to open shapefile";
            if (errMsg && strlen(errMsg) > 0)
            {
                errorDetail += ": ";
                errorDetail += errMsg;
            }
            errorDetail += " (Path: ";
            errorDetail += szShpPath;
            errorDetail += ")";
            
            WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, 
                errorDetail);
            return SE_ERROR_LAYER_INVALID;
        }
    }

    // 5. 获取图层
    OGRLayer* poLayer = poDS->GetLayer(0);
    if (poLayer == nullptr)
    {
        GDALClose(poDS);
        WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, 
            "No layer found in shapefile");
        return SE_ERROR_LAYER_INVALID;
    }

    // 6. 检查fill_color字段是否存在
    OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();
    int colorFieldIndex = poFDefn->GetFieldIndex("fill_color");
    if (colorFieldIndex == -1)
    {
        GDALClose(poDS);
        WriteJsonResult(szOutputJsonPath, false, false, maxColorCount, 0, false, 
            "Field 'fill_color' not found");
        return SE_ERROR_FIELD_NOT_FOUND;
    }

    // 7. 收集所有颜色和要素几何
    std::set<std::string> uniqueColors;
    std::map<OGRFeature*, std::string> featureColors;
    
    poLayer->ResetReading();
    OGRFeature* poFeature;
    while ((poFeature = poLayer->GetNextFeature()) != nullptr)
    {
        const char* colorValue = poFeature->GetFieldAsString(colorFieldIndex);
        std::string color(colorValue ? colorValue : "");
        
        uniqueColors.insert(color);
        
        // 克隆要素以保存几何信息
        OGRFeature* clonedFeature = poFeature->Clone();
        featureColors[clonedFeature] = color;
        
        OGRFeature::DestroyFeature(poFeature);
    }

    actualColorCount = static_cast<int>(uniqueColors.size());

    // 8. 检查相邻要素是否有相同颜色
    hasAdjacentSameColor = false;
    for (auto it1 = featureColors.begin(); it1 != featureColors.end() && !hasAdjacentSameColor; ++it1)
    {
        OGRGeometry* geom1 = it1->first->GetGeometryRef();
        if (geom1 == nullptr) continue;

        for (auto it2 = featureColors.begin(); it2 != featureColors.end(); ++it2)
        {
            // 跳过同一要素
            if (it1->first->GetFID() == it2->first->GetFID())
                continue;

            OGRGeometry* geom2 = it2->first->GetGeometryRef();
            if (geom2 == nullptr) continue;

            // 检查是否相邻（touches）且颜色相同
            if (geom1->Touches(geom2) && it1->second == it2->second)
            {
                hasAdjacentSameColor = true;
                break;
            }
        }
    }

    // 9. 判断检查结果
    bool colorCountPassed = (actualColorCount <= maxColorCount);
    bool adjacentCheckPassed = !hasAdjacentSameColor;
    bPassed = colorCountPassed && adjacentCheckPassed;

    // 10. 写入JSON结果
    WriteJsonResult(szOutputJsonPath, true, bPassed, maxColorCount, actualColorCount, hasAdjacentSameColor);

    // 11. 清理资源
    for (auto& pair : featureColors)
    {
        OGRFeature::DestroyFeature(pair.first);
    }
    GDALClose(poDS);

    return SE_ERROR_NONE;
}
