#define _HAS_STD_BYTE 0
#include "se_map_style_check_algorithm.h"
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cmath>
#include <cstring>

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

// 颜色名称转CMYK
static QString getCMYKColor(const QString& colorName)
{
    if (colorName.contains(QStringLiteral("白色")))
        return "0.00,0.00,0.00,0.00";
    if (colorName.contains(QStringLiteral("黑色")) || colorName.contains("K100"))
        return "0.00,0.00,0.00,100.00";
    // 可以扩展更多颜色映射
    return colorName;
}

// 字体匹配检查
static bool isFontMatch(const QString& fontXml, const QString& fontShp)
{
    // 方正美黑体（K100）对应 FZMHJW--GB1-0
    if (fontXml.contains(QStringLiteral("方正美黑")) && fontShp.contains("FZMHJW"))
        return true;
    
    // 直接匹配
    if (fontXml == fontShp)
        return true;
    
    // 可以扩展更多字体匹配规则
    return false;
}

// 写JSON结果（参照 se_map_annotation_check_algorithm 格式）
static void WriteJsonResult(
    const char* szOutputJsonPath, 
    bool success,
    bool passed,
    const QJsonArray& errorDetails = QJsonArray(),
    const QJsonObject& statistics = QJsonObject()
) {
    QJsonObject item;
    item["id"] = "304";
    item["name"] = QStringLiteral("图名内容准确性");
    item["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");

    // info 字段：质检成功时显示"图名内容不存在错误"
    if (passed) {
        item["info"] = QStringLiteral("图名内容不存在错误");
    } else {
        item["info"] = QStringLiteral("图名内容存在错误");
    }

    // errors 字段：只保留 description 作为错误信息（字符串格式）
    if (!passed && !errorDetails.isEmpty()) {
        QJsonObject errors;
        for (int i = 0; i < errorDetails.size(); ++i) {
            QJsonObject detail = errorDetails[i].toObject();
            // 只保留description作为错误信息
            errors[QString("error%1").arg(i + 1)] = detail.value("description").toString();
        }
        item["errors"] = errors;
    } else {
        item["errors"] = QJsonObject();
    }

    QJsonObject root;
    root["check_item"] = item;

    QJsonDocument doc(root);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        // 使用 UTF-8 编码写入，避免中文乱码
        QByteArray jsonData = doc.toJson(QJsonDocument::Indented);
        jsonFile.write(jsonData);
        jsonFile.close();
    }
}

SE_Error ParseXmlForMapStyle(const char* szSchemaXmlPath, MapStyleRules* pRules)
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

    // 解析XML，查找图名样式相关配置
    QDomElement root = doc.documentElement();
    QDomNodeList checkItems = root.elementsByTagName("check_item");
    
    bool foundFont = false, foundSize = false, foundStroke = false, 
         foundColor = false, foundShadow = false;
    
    // 遍历所有 check_item
    for (int i = 0; i < checkItems.count(); ++i)
    {
        QDomElement checkItem = checkItems.at(i).toElement();
        
        // 检查 check_item 的 name 是否为"图名内容准确性"
        QDomElement checkItemName = checkItem.firstChildElement("name");
        if (checkItemName.isNull())
            continue;
            
        QString itemName = checkItemName.text();
        if (!itemName.contains(QStringLiteral("图名")) && 
            !itemName.contains(QStringLiteral("图名图层")))
            continue;
        
        // 遍历该 check_item 下的所有 rule
        QDomNodeList rules = checkItem.elementsByTagName("rule");
        for (int j = 0; j < rules.count(); ++j)
        {
            QDomElement rule = rules.at(j).toElement();
            QDomElement ruleName = rule.firstChildElement("name");
            QDomElement ruleValue = rule.firstChildElement("value");
            
            if (ruleName.isNull() || ruleValue.isNull())
                continue;
            
            QString name = ruleName.text();
            QString value = ruleValue.text();
            
            if (name.contains(QStringLiteral("字体")))
            {
                strncpy(pRules->szFont, value.toUtf8().constData(), 255);
                pRules->szFont[255] = '\0';
                foundFont = true;
            }
            else if (name.contains(QStringLiteral("字大")) || name.contains(QStringLiteral("字号")))
            {
                pRules->dFontSizeMM = value.toDouble();
                foundSize = true;
            }
            else if (name.contains(QStringLiteral("描边宽度")))
            {
                pRules->dStrokeWidthMM = value.toDouble();
                foundStroke = true;
            }
            else if (name.contains(QStringLiteral("颜色")))
            {
                strncpy(pRules->szColor, value.toUtf8().constData(), 63);
                pRules->szColor[63] = '\0';
                
                QString cmyk = getCMYKColor(value);
                strncpy(pRules->szColorCMYK, cmyk.toUtf8().constData(), 63);
                pRules->szColorCMYK[63] = '\0';
                foundColor = true;
            }
            else if (name.contains(QStringLiteral("立体阴影")))
            {
                pRules->bHasShadow = (value.contains(QStringLiteral("是")) || 
                                      value.toLower() == "true" || 
                                      value == "1");
                foundShadow = true;
            }
        }
    }

    // 检查是否至少找到了一些配置
    if (!foundFont && !foundSize && !foundStroke && !foundColor && !foundShadow)
    {
        return SE_ERROR_XML_PARSE_FAILED;
    }

    return SE_ERROR_NONE;
}

SE_Error MapStyleCheck(
    const char* szMapNameShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed)
{
    bPassed = false;
    MapStyleRules rules;
    QJsonArray errors;
    QJsonObject info;

    // 1. 解析XML获取配置
    SE_Error error = ParseXmlForMapStyle(szSchemaXmlPath, &rules);
    if (error != SE_ERROR_NONE)
    {
        WriteJsonResult(szOutputJsonPath, false, false);
        return error;
    }

    // 2. 检查文件是否存在
    QString qPath = QString::fromUtf8(szMapNameShpPath);
    QFileInfo fileInfo(qPath);
    
    if (!fileInfo.exists())
    {
        QJsonObject errorInfo;
        errorInfo["error"] = QString("Shapefile not found: %1").arg(szMapNameShpPath);
        WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), errorInfo);
        return SE_ERROR_FILE_NOT_FOUND;
    }
    
    // 检查必需的相关文件
    QString basePath = fileInfo.path() + "/" + fileInfo.completeBaseName();
    QFileInfo shxInfo(basePath + ".shx");
    QFileInfo dbfInfo(basePath + ".dbf");
    
    if (!shxInfo.exists())
    {
        QJsonObject errorInfo;
        errorInfo["error"] = QString("Missing .shx file: %1").arg(shxInfo.filePath());
        WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), errorInfo);
        return SE_ERROR_FILE_NOT_FOUND;
    }
    
    if (!dbfInfo.exists())
    {
        QJsonObject errorInfo;
        errorInfo["error"] = QString("Missing .dbf file: %1").arg(dbfInfo.filePath());
        WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), errorInfo);
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
            szMapNameShpPath,
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr
        );
        
        if (poDS == nullptr)
        {
            // 获取详细错误信息
            const char* errMsg = CPLGetLastErrorMsg();
            QJsonObject errorInfo;
            errorInfo["error"] = QString("Failed to open shapefile: %1").arg(
                errMsg && strlen(errMsg) > 0 ? errMsg : "Unknown GDAL error");
            errorInfo["path"] = szMapNameShpPath;
            WriteJsonResult(szOutputJsonPath, false, false, QJsonArray(), errorInfo);
            return SE_ERROR_LAYER_INVALID;
        }
    }

    // 5. 获取图层
    OGRLayer* poLayer = poDS->GetLayer(0);
    if (poLayer == nullptr)
    {
        GDALClose(poDS);
        WriteJsonResult(szOutputJsonPath, false, false);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();

    // 6. 检查必需字段是否存在
    int fontFamilIndex = poFDefn->GetFieldIndex("font_famil");
    int fontSizeIndex = poFDefn->GetFieldIndex("font_size");
    int strokeIndex = poFDefn->GetFieldIndex("text_strok");
    int colorIndex = poFDefn->GetFieldIndex("text_str_1");
    int shadowIndex = poFDefn->GetFieldIndex("has_shadow");

    if (fontFamilIndex == -1 || fontSizeIndex == -1)
    {
        GDALClose(poDS);
        QJsonArray errorDetails;
        QJsonObject error;
        error["description"] = QStringLiteral("缺少必需的字段(font_famil或font_size)");
        error["expected"] = QStringLiteral("font_famil, font_size");
        error["actual"] = QStringLiteral("字段缺失");
        errorDetails.append(error);
        WriteJsonResult(szOutputJsonPath, false, false, errorDetails);
        return SE_ERROR_FIELD_NOT_FOUND;
    }

    // 7. 遍历要素进行样式检查
    int totalFeatures = 0;
    int fontMismatchCount = 0;
    int sizeMismatchCount = 0;
    int strokeMismatchCount = 0;
    int colorMismatchCount = 0;
    int shadowMismatchCount = 0;

    poLayer->ResetReading();
    OGRFeature* poFeature;
    while ((poFeature = poLayer->GetNextFeature()) != nullptr)
    {
        totalFeatures++;

        // 检查字体
        if (fontFamilIndex != -1 && strlen(rules.szFont) > 0)
        {
            const char* fontFamily = poFeature->GetFieldAsString(fontFamilIndex);
            QString fontFamilyStr = QString::fromUtf8(fontFamily);
            QString requiredFont = QString::fromUtf8(rules.szFont);
            
            if (!isFontMatch(requiredFont, fontFamilyStr))
            {
                fontMismatchCount++;
                if (errors.size() < 10) // 限制错误数量
                {
                    QJsonObject error;
                    error["type"] = QStringLiteral("font_mismatch");
                    error["feature_id"] = static_cast<int>(poFeature->GetFID());
                    error["description"] = QString(QStringLiteral("字体不匹配"));
                    error["expected"] = requiredFont;
                    error["actual"] = fontFamilyStr;
                    errors.append(error);
                }
            }
        }

        // 检查字号（pt转mm：1pt = 0.3528mm）
        if (fontSizeIndex != -1 && rules.dFontSizeMM > 0)
        {
            double fontSizePt = poFeature->GetFieldAsDouble(fontSizeIndex);
            double fontSizeMm = fontSizePt * 0.3528;
            
            if (fabs(fontSizeMm - rules.dFontSizeMM) > 0.1)
            {
                sizeMismatchCount++;
                if (errors.size() < 10)
                {
                    QJsonObject error;
                    error["type"] = QStringLiteral("font_size_mismatch");
                    error["feature_id"] = static_cast<int>(poFeature->GetFID());
                    error["description"] = QStringLiteral("字号不匹配");
                    error["expected_mm"] = rules.dFontSizeMM;
                    error["actual_pt"] = fontSizePt;
                    error["actual_mm"] = fontSizeMm;
                    errors.append(error);
                }
            }
        }

        // 检查描边宽度
        if (strokeIndex != -1 && rules.dStrokeWidthMM > 0)
        {
            double strokeWidthPt = poFeature->GetFieldAsDouble(strokeIndex);
            double strokeWidthMm = strokeWidthPt * 0.3528;
            
            if (fabs(strokeWidthMm - rules.dStrokeWidthMM) > 0.01)
            {
                strokeMismatchCount++;
                if (errors.size() < 10)
                {
                    QJsonObject error;
                    error["type"] = QStringLiteral("stroke_width_mismatch");
                    error["feature_id"] = static_cast<int>(poFeature->GetFID());
                    error["description"] = QStringLiteral("描边宽度不匹配");
                    error["expected_mm"] = rules.dStrokeWidthMM;
                    error["actual_pt"] = strokeWidthPt;
                    error["actual_mm"] = strokeWidthMm;
                    errors.append(error);
                }
            }
        }

        // 检查颜色
        if (colorIndex != -1 && strlen(rules.szColorCMYK) > 0)
        {
            const char* textFill = poFeature->GetFieldAsString(colorIndex);
            QString textFillStr = QString::fromUtf8(textFill);
            QString requiredColor = QString::fromUtf8(rules.szColorCMYK);
            
            if (textFillStr != requiredColor)
            {
                colorMismatchCount++;
                if (errors.size() < 10)
                {
                    QJsonObject error;
                    error["type"] = QStringLiteral("color_mismatch");
                    error["feature_id"] = static_cast<int>(poFeature->GetFID());
                    error["description"] = QStringLiteral("颜色不匹配");
                    error["expected"] = requiredColor;
                    error["actual"] = textFillStr;
                    errors.append(error);
                }
            }
        }

        // 检查立体阴影
        if (shadowIndex != -1)
        {
            const char* shadowValue = poFeature->GetFieldAsString(shadowIndex);
            QString shadowStr = QString::fromUtf8(shadowValue).toLower();
            
            // 支持多种布尔值格式：true/false, 1/0, 是/否
            bool hasShadowBool = false;
            if (shadowStr == "true" || shadowStr == "1" || shadowStr.contains(QStringLiteral("是")))
            {
                hasShadowBool = true;
            }
            else if (shadowStr == "false" || shadowStr == "0" || shadowStr.contains(QStringLiteral("否")))
            {
                hasShadowBool = false;
            }
            
            if (hasShadowBool != rules.bHasShadow)
            {
                shadowMismatchCount++;
                if (errors.size() < 10)
                {
                    QJsonObject error;
                    error["type"] = QStringLiteral("shadow_mismatch");
                    error["feature_id"] = static_cast<int>(poFeature->GetFID());
                    error["description"] = QStringLiteral("立体阴影不匹配");
                    error["expected"] = rules.bHasShadow ? QStringLiteral("是") : QStringLiteral("否");
                    error["actual"] = hasShadowBool ? QStringLiteral("是") : QStringLiteral("否");
                    error["actual_raw"] = shadowStr;  // 显示原始值便于调试
                    errors.append(error);
                }
            }
        }

        OGRFeature::DestroyFeature(poFeature);
    }

    // 8. 汇总检查结果
    QJsonObject statistics;
    statistics["total_features"] = totalFeatures;
    statistics["font_mismatch_count"] = fontMismatchCount;
    statistics["size_mismatch_count"] = sizeMismatchCount;
    statistics["stroke_mismatch_count"] = strokeMismatchCount;
    statistics["color_mismatch_count"] = colorMismatchCount;
    statistics["shadow_mismatch_count"] = shadowMismatchCount;
    statistics["total_errors"] = fontMismatchCount + sizeMismatchCount + strokeMismatchCount + 
                           colorMismatchCount + shadowMismatchCount;

    // 9. 判断是否通过
    bPassed = (errors.isEmpty());

    // 10. 写入JSON结果
    WriteJsonResult(szOutputJsonPath, true, bPassed, errors, statistics);

    // 11. 清理资源
    GDALClose(poDS);

    return SE_ERROR_NONE;
}
