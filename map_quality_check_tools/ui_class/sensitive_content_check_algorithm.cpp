/**
 * @file sensitive_content_check_algorithm.cpp
 * @brief 敏感文字合规性检查（算法212）
 *
 * 检测注记图层中是否包含敏感词。
 * - 多字符注记（≥2字）：直接子串匹配
 * - 单字注记：通过空间聚类链式拼词后匹配，含四项保护校验
 */

#include "sensitive_content_check_algorithm.h"

#include <QString>
#include <QStringList>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDomDocument>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QMap>
#include <QPair>
#include <QList>
#include <QPointF>
#include <QVector>

#include <QgsVectorLayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsFields.h>
#include <QgsField.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsPointXY.h>
#include <QgsFeatureRequest.h>

#include <windows.h>

#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <functional>

// ==================== 212 内部数据结构 ====================

struct TextErrorInfo
{
    QString featureId;
    QString actualText;
    QString expectedText;
    QPointF position;
    QVector<QPointF> allPositions;
    QString errorDescription;
    QString sensitiveCategory;
};

struct AnnoFeatureInfo
{
    QString fid;
    QString text;
    QgsPointXY position;
    double fontSize = 0.0;
    QString fontFamil;
    QString fontStyle;
    QString fontColor;
    double shearAngle = 0.0;
    QString annoGroupId;
};

/// 扁平要素（用于212内部的高效处理）
struct FlatFeature
{
    QString fid;
    QString text;
    QgsPointXY pos;
    // 字体属性
    double fontSize = 0.0;
    QString fontFamil;
    QString fontColor;
    // 原始注记分组ID
    QString annoGroupId;
};

// ==================== 文本正规化 ====================

/**
 * @brief 文本正规化：去空格 + 全角→半角 + 繁体→简体 + 统一小写
 */
static QString NormalizeTextForMatch(const QString& text)
{
    QString result = text;

    // 1. 去除空格：半角 0x0020 + 全角 0x3000
    result = result.remove(QChar(0x0020)).remove(QChar(0x3000));

    // 2. 全角→半角转换
    for (int i = 0; i < result.length(); ++i)
    {
        ushort u = result[i].unicode();
        if (u >= 0xFF01 && u <= 0xFF5E)
            result[i] = QChar(u - 0xFEE0);
        else if (u == 0x3000)
            result[i] = QChar(0x0020);
    }
    result = result.remove(QChar(0x0020));

    // 3. 繁体→简体：使用 Windows LCMapStringEx API
    std::wstring wstr = result.toStdWString();
    int len = LCMapStringEx(
        LOCALE_NAME_SYSTEM_DEFAULT,
        LCMAP_SIMPLIFIED_CHINESE,
        wstr.c_str(), static_cast<int>(wstr.length()),
        nullptr, 0,
        nullptr, nullptr, 0);
    if (len > 0)
    {
        std::vector<wchar_t> buf(len + 1);
        LCMapStringEx(
            LOCALE_NAME_SYSTEM_DEFAULT,
            LCMAP_SIMPLIFIED_CHINESE,
            wstr.c_str(), static_cast<int>(wstr.length()),
            buf.data(), len + 1,
            nullptr, nullptr, 0);
        result = QString::fromStdWString(std::wstring(buf.data()));
    }

    // 4. 统一小写（解决 110kV/110KV/110kv 的大小写不一致）
    result = result.toLower();

    return result;
}

/**
 * @brief 判断一个字符是否为 CJK（中日韩统一表意文字）
 */
static bool IsCJKCharacter(QChar c)
{
    ushort u = c.unicode();
    if (u >= 0x4E00 && u <= 0x9FFF) return true;
    if (u >= 0x3400 && u <= 0x4DBF) return true;
    if (u >= 0xF900 && u <= 0xFAFF) return true;
    return false;
}

/**
 * @brief 从图层中提取所有注记要素，建立 text → 要素列表 的索引
 */
static void BuildAnnotationIndex(
    QgsVectorLayer* layer,
    int textFieldIdx,
    QMap<QString, QList<AnnoFeatureInfo>>& textIndex,
    QSet<QString>& allFids)
{
    const QgsFields& fields = layer->fields();
    int fontSizeIdx    = fields.indexOf("font_size");
    int fontFamilIdx   = fields.indexOf("font_famil");
    int fontStyleIdx   = fields.indexOf("font_style");
    int fontColorIdx   = fields.indexOf("font_color");
    int shearAnglIdx   = fields.indexOf("shear_angl");

    int annoGroupIdx = fields.indexOf("text_string");
    if (annoGroupIdx < 0) annoGroupIdx = fields.indexOf("anno_id");
    if (annoGroupIdx < 0) annoGroupIdx = fields.indexOf("group_id");

    QgsFeatureRequest request;
    QgsFeatureIterator it = layer->getFeatures(request);
    QgsFeature feature;

    while (it.nextFeature(feature))
    {
        QString text = feature.attribute(textFieldIdx).toString().trimmed();
        text.remove(' ');
        if (text.isEmpty()) continue;

        QString fid = QString::number(feature.id());
        allFids.insert(fid);

        QgsPointXY pt;
        QgsGeometry geom = feature.geometry();
        if (!geom.isNull())
        {
            if (geom.type() == QgsWkbTypes::PointGeometry)
                pt = geom.asPoint();
            else
                pt = geom.centroid().asPoint();
        }

        AnnoFeatureInfo info;
        info.fid = fid;
        info.text = text;
        info.position = pt;

        if (fontSizeIdx >= 0)
            info.fontSize = feature.attribute(fontSizeIdx).toDouble();
        if (fontFamilIdx >= 0)
            info.fontFamil = feature.attribute(fontFamilIdx).toString().trimmed();
        if (fontStyleIdx >= 0)
            info.fontStyle = feature.attribute(fontStyleIdx).toString().trimmed();
        if (fontColorIdx >= 0)
            info.fontColor = feature.attribute(fontColorIdx).toString().trimmed();
        if (shearAnglIdx >= 0)
            info.shearAngle = feature.attribute(shearAnglIdx).toDouble();
        if (annoGroupIdx >= 0)
            info.annoGroupId = feature.attribute(annoGroupIdx).toString().trimmed();

        textIndex[text].append(info);
    }
}

/**
 * @brief 写入错误要素 SHP
 */
static SE_Error WriteErrorShp(
    const QMap<QString, const TextErrorInfo*>& errorMap,
    QgsVectorLayer* sourceLayer,
    const char* szOutputShpPath)
{
    if (errorMap.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    try
    {
        QString outPath = QString::fromUtf8(szOutputShpPath);

        QDir outDir = QFileInfo(outPath).dir();
        if (!outDir.exists())
        {
            outDir.mkpath(".");
        }

        QStringList exts = { "shp", "shx", "dbf", "prj", "cpg" };
        QString baseName = QFileInfo(outPath).baseName();
        QString dirPath = QFileInfo(outPath).dir().absolutePath();
        for (const QString& ext : exts)
        {
            QString fp = QString("%1/%2.%3").arg(dirPath, baseName, ext);
            if (QFile::exists(fp)) QFile::remove(fp);
        }

        QgsFields outFields = sourceLayer->fields();
        outFields.append(QgsField("FEATURE_ID", QVariant::String, "String", 20));
        outFields.append(QgsField("ERR_ACTUAL", QVariant::String, "String", 200));
        outFields.append(QgsField("ERR_EXPECT", QVariant::String, "String", 200));
        outFields.append(QgsField("ERR_DESC", QVariant::String, "String", 254));
        outFields.append(QgsField("ERR_CATEG", QVariant::String, "String", 100));

        QList<QgsFeature> outFeatures;
        QgsFeatureRequest req;
        QgsFeatureIterator it = sourceLayer->getFeatures(req);
        QgsFeature feat;
        while (it.nextFeature(feat))
        {
            QString fid = QString::number(feat.id());
            if (errorMap.contains(fid))
            {
                const TextErrorInfo* errInfo = errorMap[fid];

                QgsFeature newFeat(outFields);
                newFeat.setGeometry(feat.geometry());

                QgsAttributes attrs = feat.attributes();
                attrs.append(fid);
                attrs.append(errInfo->actualText);
                attrs.append(errInfo->expectedText);
                attrs.append(errInfo->errorDescription);
                attrs.append(errInfo->sensitiveCategory);

                newFeat.setAttributes(attrs);
                outFeatures.append(newFeat);
            }
        }

        if (outFeatures.isEmpty())
        {
            return SE_ERROR_NONE;
        }

        QgsVectorFileWriter writer(outPath, "UTF-8", outFields,
            sourceLayer->wkbType(), sourceLayer->crs(), "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        for (QgsFeature& f : outFeatures)
        {
            if (!writer.addFeature(f))
            {
                return SE_ERROR_WRITE_FILE_FAILED;
            }
        }

        return SE_ERROR_NONE;
    }
    catch (...)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

// ==================== 内部辅助函数 ====================

static SE_Error WriteSensitiveCheckJsonReport(
    const std::vector<TextErrorInfo>& errors,
    const char* szOutputJsonPath,
    int totalAnnotations,
    int hitCount)
{
    try
    {
        QJsonObject root;
        root["id"] = "212";
        root["name"] = QStringLiteral("敏感文字合规性检查");
        root["err_exist"] = !errors.empty();

        QString infoText;
        if (errors.empty())
        {
            root["status"] = QStringLiteral("质检完成");
            infoText = QString("未发现敏感内容，共检查%1个注记").arg(totalAnnotations);
        }
        else
        {
            root["status"] = QStringLiteral("质检完成");
            infoText = QString("发现%1个敏感内容注记，共检查%2个注记").arg(hitCount).arg(totalAnnotations);
        }
        infoText += QString("\n命中敏感词数: %1").arg(hitCount);
        infoText += QString("\n检查注记总数: %1").arg(totalAnnotations);

        QJsonObject info;
        info["description"] = infoText;
        root["info"] = info;

        if (!errors.empty())
        {
            QJsonObject errObj;
            for (size_t i = 0; i < errors.size(); ++i)
            {
                QJsonObject detail;
                detail["description"] = errors[i].errorDescription;
                detail["actual_value"] = errors[i].actualText;
                detail["expected_value"] = errors[i].expectedText;
                detail["feature_id"] = errors[i].featureId;
                detail["position_x"] = errors[i].position.x();
                detail["position_y"] = errors[i].position.y();

                if (!errors[i].allPositions.isEmpty())
                {
                    QJsonArray posArray;
                    for (const QPointF& p : errors[i].allPositions)
                    {
                        QJsonObject posObj;
                        posObj["x"] = p.x();
                        posObj["y"] = p.y();
                        posArray.append(posObj);
                    }
                    detail["positions"] = posArray;
                }

                if (!errors[i].sensitiveCategory.isEmpty())
                    detail["sensitive_category"] = errors[i].sensitiveCategory;

                errObj[QString("error%1").arg(i + 1)] = detail;
            }
            root["errors"] = errObj;
        }

        QJsonDocument doc(root);
        QFile file(QString::fromUtf8(szOutputJsonPath));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
            return SE_ERROR_WRITE_FILE_FAILED;
        QTextStream stream(&file);
        stream.setCodec("UTF-8");
        stream << doc.toJson(QJsonDocument::Indented);
        file.close();
        return SE_ERROR_NONE;
    }
    catch (...)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

static SE_Error WriteFailureJsonAndReturn212(SE_Error errCode, const char* szOutputJsonPath, const char* szErrorMsg)
{
    QJsonObject root;
    root["id"] = "212";
    root["name"] = QStringLiteral("敏感文字合规性检查");
    root["status"] = QStringLiteral("质检失败");
    root["err_exist"] = true;
    QJsonObject info;
    info["description"] = QString::fromUtf8(szErrorMsg);
    root["info"] = info;
    root["errors"] = QJsonObject();

    QJsonDocument doc(root);
    QFile jsonFile(QString::fromUtf8(szOutputJsonPath));
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
    return errCode;
}

// ==================== 主函数 ====================

SE_Error SensitiveContentCheck(
    const char* szAnnoShpPath,
    const char* szXmlConfigPath,
    const char* szOutputJsonPath,
    const char* szOutputShpPath,
    const char* szOutputLogPath)
{
    if (!szAnnoShpPath || !szXmlConfigPath || !szOutputJsonPath || !szOutputLogPath)
        return SE_ERROR_INVALID_PARAMETER;

    // ========== 打开日志文件 ==========
    QFile logFile(QString::fromUtf8(szOutputLogPath));
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return SE_ERROR_WRITE_FILE_FAILED;
    QTextStream logStream(&logFile);
    logStream.setCodec("UTF-8");

    auto writeLog = [&](const QString& msg) {
        logStream << msg << "\n";
        logStream.flush();
    };

    writeLog("============================================");
    writeLog("  敏感文字合规性检查 (算法212)");
    writeLog("  开始时间: " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    writeLog("============================================");
    writeLog("");

    QgsVectorLayer* layer = nullptr;

    try
    {
        // ========== 从 XML 读取敏感词库 ==========
        QStringList sensitiveWords;
        QMap<QString, QString> wordToCategory;

        QString rawSensitive;
        {
            QFile xmlFile(QString::fromUtf8(szXmlConfigPath));
            if (xmlFile.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                QDomDocument doc;
                if (doc.setContent(&xmlFile))
                {
                    QDomElement root = doc.documentElement();

                    // 查找 check_item id=212, name=敏感文字合规性检查
                    QDomNodeList checkItems = root.elementsByTagName("check_item");
                    for (int ci = 0; ci < checkItems.count(); ++ci)
                    {
                        QDomElement itemElem = checkItems.at(ci).toElement();
                        QString itemId   = itemElem.firstChildElement("id").text().trimmed();
                        QString itemName = itemElem.firstChildElement("name").text().trimmed();
                        if (itemId == QStringLiteral("212") && itemName == QStringLiteral("敏感文字合规性检查"))
                        {
                            QDomNodeList ruleNodes = itemElem.elementsByTagName("rule");
                            QStringList categoryEntries;
                            for (int ri = 0; ri < ruleNodes.count(); ++ri)
                            {
                                QDomElement ruleElem = ruleNodes.at(ri).toElement();
                                QString ruleName = ruleElem.firstChildElement("name").text().trimmed();
                                QString ruleVal  = ruleElem.firstChildElement("value").text().trimmed();
                                if (!ruleName.isEmpty() && !ruleVal.isEmpty())
                                    categoryEntries.append(ruleName + QStringLiteral("=") + ruleVal);
                            }
                            if (!categoryEntries.isEmpty())
                                rawSensitive = categoryEntries.join(QStringLiteral("|"));
                            break;
                        }
                    }
                }
                xmlFile.close();
            }
        }
        writeLog(QString("XML配置文件: %1").arg(QString::fromUtf8(szXmlConfigPath)));
        writeLog(QString("敏感词原始内容: %1").arg(rawSensitive));

        // ========== 解析敏感词库 ==========
        // 支持两种格式：
        //   旧格式：逗号分隔 "词1,词2,词3"
        //   新格式：分类=词1,词2|分类=词3,词4
        if (rawSensitive.contains('=') || rawSensitive.contains('|'))
        {
            // 新格式：提取所有逗号分隔的项，追踪分类
            QStringList pipeParts = rawSensitive.split('|', QString::SkipEmptyParts);
            for (const QString& part : pipeParts)
            {
                QString trimmed = part.trimmed();
                int eqIdx = trimmed.indexOf('=');
                if (eqIdx > 0)
                {
                    QString category = trimmed.left(eqIdx).trimmed();
                    QString words = trimmed.mid(eqIdx + 1).trimmed();
                    QStringList wordList = words.split(',', QString::SkipEmptyParts);
                    for (const QString& w : wordList)
                    {
                        QString tw = w.trimmed();
                        if (!tw.isEmpty())
                        {
                            sensitiveWords.append(tw);
                            wordToCategory[tw] = category;
                        }
                    }
                }
                else
                {
                    QStringList wordList = trimmed.split(',', QString::SkipEmptyParts);
                    for (const QString& w : wordList)
                    {
                        QString tw = w.trimmed();
                        if (!tw.isEmpty())
                            sensitiveWords.append(tw);
                    }
                }
            }
        }
        else
        {
            // 旧格式：纯逗号分隔
            QStringList wordList = rawSensitive.split(',', QString::SkipEmptyParts);
            for (const QString& w : wordList)
            {
                QString tw = w.trimmed();
                if (!tw.isEmpty())
                    sensitiveWords.append(tw);
            }
        }

        writeLog("--- 敏感词库加载 ---");
        writeLog(QString("敏感词总数: %1").arg(sensitiveWords.size()));
        if (sensitiveWords.isEmpty())
        {
            writeLog("[错误] 敏感词库为空！请检查 XML 配置文件格式。");
            writeLog("支持的XML格式：<check_item><id>212</id><name>敏感文字合规性检查</name><rule><name>分类</name><value>词1,词2</value></rule>...</check_item>");
            return WriteFailureJsonAndReturn212(SE_ERROR_INVALID_PARAMETER, szOutputJsonPath, "敏感词库为空，XML配置文件中未找到有效的敏感词内容");
        }

        // ========== 敏感词归一化：繁体→简体、全角→半角、统一小写 ==========
        {
            QMap<QString, QString> origToNormalized;
            QStringList normWords;
            for (const QString& w : sensitiveWords)
            {
                QString nw = NormalizeTextForMatch(w);
                normWords.append(nw);
                origToNormalized[w] = nw;
            }
            sensitiveWords = normWords;

            // 同步更新 wordToCategory（键改为归一化后的词）
            QMap<QString, QString> newW2C;
            for (auto it = wordToCategory.constBegin(); it != wordToCategory.constEnd(); ++it)
            {
                QString nw = origToNormalized.value(it.key());
                if (!newW2C.contains(nw))  // 去重：同名归一化词只保留第一个分类
                    newW2C[nw] = it.value();
            }
            wordToCategory = newW2C;
        }
        {
            QStringList catList;
            for (auto it = wordToCategory.constBegin(); it != wordToCategory.constEnd(); ++it)
                catList << QString("%1=%2").arg(it.value()).arg(it.key());
            if (!catList.isEmpty())
            {
                writeLog("分类列表: " + catList.join(", "));
                writeLog("");
            }
        }
        writeLog("敏感词: " + sensitiveWords.join(", "));
        writeLog("");

        // ========== Phase 1: 加载注记SHP ==========
        writeLog("--- Phase 1: 加载注记SHP ---");
        layer = new QgsVectorLayer(QString::fromUtf8(szAnnoShpPath), "annotations", "ogr");
        if (!layer || !layer->isValid())
        {
            writeLog("[错误] 无法加载注记SHP: " + QString::fromUtf8(szAnnoShpPath));
            return WriteFailureJsonAndReturn212(SE_ERROR_FILE_NOT_FOUND, szOutputJsonPath, "无法加载注记SHP文件");
        }
        writeLog(QString("注记图层要素数: %1").arg(layer->featureCount()));

        // 查找文本字段（支持 text_conte / text_strin / full_text 等常见字段名）
        const QgsFields& fields = layer->fields();
        int textFieldIdx = -1;
        QString textFieldName;
        QStringList candidateTextFields = {"text_conte", "text_strin", "full_text", "text", "name", "content"};
        for (const QString& candidate : candidateTextFields)
        {
            textFieldIdx = fields.indexOf(candidate);
            if (textFieldIdx >= 0)
            {
                textFieldName = candidate;
                break;
            }
        }
        if (textFieldIdx < 0)
        {
            writeLog("[错误] 未找到注记文本字段（已尝试: " + candidateTextFields.join(", ") + "）");
            return WriteFailureJsonAndReturn212(SE_ERROR_INVALID_PARAMETER, szOutputJsonPath, "未找到注记文本字段");
        }
        writeLog(QString("文本字段: %1 (索引=%2)").arg(textFieldName).arg(textFieldIdx));

        // ========== Phase 1.5: 建立文本索引 + 扁平要素数组 ==========
        QMap<QString, QList<AnnoFeatureInfo>> textIndex;
        QSet<QString> allFids;
        BuildAnnotationIndex(layer, textFieldIdx, textIndex, allFids);

        // 构建扁平要素数组（用于高效的空间查询）
        QVector<FlatFeature> allFeatures;
        QStringList groupIdFieldNames = {"text_string", "anno_id", "group_id"};
        int groupIdFieldIdx = -1;
        for (const QString& gf : groupIdFieldNames)
        {
            groupIdFieldIdx = fields.indexOf(gf);
            if (groupIdFieldIdx >= 0) break;
        }
        bool hasAnnoGroupId = (groupIdFieldIdx >= 0);

        // 字体相关字段查找
        int fontSizeFieldIdx = fields.indexOf("font_size");
        int fontFamilyFieldIdx = fields.indexOf("font_famil");
        int fontColorFieldIdx = fields.indexOf("font_color");

        QgsFeature feature;
        QgsFeatureRequest request;
        QgsFeatureIterator it = layer->getFeatures(request);
        int fidFieldIdx = fields.indexOf("fid");
        if (fidFieldIdx < 0) fidFieldIdx = fields.indexOf("FID");
        if (fidFieldIdx < 0) fidFieldIdx = fields.indexOf("ID");

        while (it.nextFeature(feature))
        {
            if (!feature.isValid()) continue;
            QgsGeometry geom = feature.geometry();
            if (geom.isNull()) continue;

            QgsPointXY pt;
            if (geom.type() == QgsWkbTypes::PointGeometry)
                pt = geom.asPoint();
            else
                pt = geom.centroid().asPoint();

            FlatFeature ff;
            ff.pos = pt;

            if (fidFieldIdx >= 0)
                ff.fid = feature.attribute(fidFieldIdx).toString();
            else
                ff.fid = QString::number(feature.id());

            ff.text = feature.attribute(textFieldIdx).toString().trimmed();
            ff.text.remove(' ');  // 与 BuildAnnotationIndex 保持一致
            if (ff.text.isEmpty()) continue;

            if (fontSizeFieldIdx >= 0)
                ff.fontSize = feature.attribute(fontSizeFieldIdx).toDouble();
            if (fontFamilyFieldIdx >= 0)
                ff.fontFamil = feature.attribute(fontFamilyFieldIdx).toString().trimmed();
            if (fontColorFieldIdx >= 0)
                ff.fontColor = feature.attribute(fontColorFieldIdx).toString().trimmed();
            if (hasAnnoGroupId)
            {
                QVariant gv = feature.attribute(groupIdFieldIdx);
                if (!gv.isNull())
                    ff.annoGroupId = gv.toString().trimmed();
            }

            allFeatures.append(ff);
        }

        writeLog("");
        writeLog(QString("索引统计: 唯一文本数=%1 唯一FID数=%2").arg(textIndex.size()).arg(allFids.size()));
        writeLog(QString("扁平要素总数: %1").arg(allFeatures.size()));

        // 分离：单字符（含数字/英文） vs 多字符文本
        QList<int> singleCjkIndices;
        QList<int> multiCharIndices;
        for (int i = 0; i < allFeatures.size(); ++i)
        {
            const QString& text = allFeatures[i].text;
            if (text.length() == 1)
                singleCjkIndices.append(i);
            else if (text.length() >= 2)
                multiCharIndices.append(i);
        }

        writeLog("--- Phase 1.5: 要素分离 ---");
        writeLog(QString("总要素数: %1").arg(allFeatures.size()));
        writeLog(QString("  单字符: %1 (需空间拼词)").arg(singleCjkIndices.size()));
        writeLog(QString("  多字符文本: %1 (直接子串匹配)").arg(multiCharIndices.size()));
        writeLog(QString("  分组字段(text_string/anno_id/group_id): %1").arg(hasAnnoGroupId ? "存在" : "不存在"));
        writeLog("");

        // ========== Phase 2: 敏感词扫描 ==========
        writeLog("--- Phase 2: 敏感词扫描 ---");
        writeLog(QString("敏感词总数: %1").arg(sensitiveWords.size()));

        // ----- Step 0: 提取敏感字符集 = 所有敏感词拆字去重 -----
        QSet<QChar> sensitiveCharSet;
        for (const QString& word : sensitiveWords)
        {
            for (int i = 0; i < word.length(); ++i)
                sensitiveCharSet.insert(word[i]);
        }
        writeLog(QString("Step 0: 敏感字符集 (共%1个): ").arg(sensitiveCharSet.size()));
        {
            QStringList chars;
            for (QChar c : sensitiveCharSet) chars << QString(c);
            writeLog("  " + chars.join(" "));
        }

        // ----- Step 1: 单向过滤 —— 繁简统一后比对，避免繁体单字被过滤 -----
        QList<int> filteredSingleIndices;
        for (int si : singleCjkIndices)
        {
            QChar nc = NormalizeTextForMatch(QString(allFeatures[si].text[0]))[0];
            if (sensitiveCharSet.contains(nc))
                filteredSingleIndices.append(si);
        }
        int excludedCount = singleCjkIndices.size() - filteredSingleIndices.size();
        writeLog(QString("Step 1: 单向过滤: %1 → %2 (排除%3个非敏感单字)")
                 .arg(singleCjkIndices.size()).arg(filteredSingleIndices.size()).arg(excludedCount));

        // ----- 构建过滤后的单字符倒排索引（键为正规化后字符，链式匹配统一走简体） -----
        QMap<QChar, QList<int>> charIndex;
        for (int si : filteredSingleIndices)
        {
            QChar nc = NormalizeTextForMatch(QString(allFeatures[si].text[0]))[0];
            charIndex[nc].append(si);
        }

        // ----- Step 2: 排除整词 —— 词中任一字在数据中0出现，整词跳过 -----
        {
            QStringList availableChars;
            for (auto ct = charIndex.constBegin(); ct != charIndex.constEnd(); ++ct)
                availableChars << QString("%1(%2个)").arg(ct.key()).arg(ct.value().size());
            writeLog(QString("Step 2: 数据中可拼词的单字: [%1]").arg(availableChars.join(", ")));
        }

        QStringList activeWords;
        {
            QStringList skippedWordsInfo;
            for (const QString& word : sensitiveWords)
            {
                if (word.length() < 2)
                {
                    skippedWordsInfo << QString("\"%1\"(长度<2)").arg(word);
                    continue;
                }
                bool allCharsPresent = true;
                QStringList missingChars;
                for (int i = 0; i < word.length(); ++i)
                {
                    if (!charIndex.contains(word[i]))
                    {
                        allCharsPresent = false;
                        missingChars << word[i];
                    }
                }
                if (allCharsPresent)
                    activeWords.append(word);
                else
                    skippedWordsInfo << QString("\"%1\"(缺字: [%2])")
                        .arg(word).arg(missingChars.join(", "));
            }
            writeLog(QString("Step 2: 排除整词: %1 → %2 个有效")
                     .arg(sensitiveWords.size()).arg(activeWords.size()));
            if (!activeWords.isEmpty())
                writeLog("  有效词: [" + activeWords.join(", ") + "]");
            if (!skippedWordsInfo.isEmpty())
            {
                writeLog(QString("  跳过词 (%1个):").arg(skippedWordsInfo.size()));
                for (const QString& s : skippedWordsInfo)
                    writeLog("    " + s);
            }
        }

        // ----- 自适应配对半径：过滤后候选单字的最近邻P90分位数 -----
        double pairRadius = 2000.0;
        QMap<int, QList<int>> nearNeighbors;
        {
            struct NnItem { int idx; double d2; };
            QList<double> nnDistances;
            for (int i = 0; i < filteredSingleIndices.size(); ++i)
            {
                const QgsPointXY& pi = allFeatures[filteredSingleIndices[i]].pos;
                QList<NnItem> items;
                for (int j = 0; j < filteredSingleIndices.size(); ++j)
                {
                    if (i == j) continue;
                    double dx = pi.x() - allFeatures[filteredSingleIndices[j]].pos.x();
                    double dy = pi.y() - allFeatures[filteredSingleIndices[j]].pos.y();
                    double d2 = dx * dx + dy * dy;
                    items.append({filteredSingleIndices[j], d2});
                }
                std::sort(items.begin(), items.end(),
                          [](const NnItem& a, const NnItem& b) { return a.d2 < b.d2; });
                if (!items.isEmpty())
                {
                    nnDistances.append(sqrt(items[0].d2));
                    QList<int> top3;
                    for (int k = 0; k < qMin(3, (int)items.size()); ++k)
                        top3.append(items[k].idx);
                    nearNeighbors[filteredSingleIndices[i]] = top3;
                }
            }
            if (!nnDistances.isEmpty())
            {
                std::sort(nnDistances.begin(), nnDistances.end());
                int p90Idx = qMin((int)(nnDistances.size() * 0.9), nnDistances.size() - 1);
                pairRadius = qBound(500.0, nnDistances[p90Idx], 10000.0);
            }
        }
        const double PAIR_RADIUS_SQ = pairRadius * pairRadius;
        writeLog(QString("Step 2.5: 自适应配对半径 = %1 (P90分位数, 范围[500,10000])").arg(pairRadius, 0, 'f', 1));
        writeLog("");

        // ----- 字体一致性辅助函数 -----
        auto isFontConsistent = [&](int idxA, int idxB) -> bool {
            const FlatFeature& a = allFeatures[idxA];
            const FlatFeature& b = allFeatures[idxB];
            if (a.fontSize == 0.0 && b.fontSize == 0.0
                && a.fontFamil.isEmpty() && b.fontFamil.isEmpty()
                && a.fontColor.isEmpty() && b.fontColor.isEmpty())
                return true;
            if (a.fontSize > 0.0 && b.fontSize > 0.0)
            {
                if (std::abs(a.fontSize - b.fontSize) > 2.0)
                    return false;
            }
            if (!a.fontFamil.isEmpty() && !b.fontFamil.isEmpty())
            {
                if (a.fontFamil.compare(b.fontFamil, Qt::CaseInsensitive) != 0)
                    return false;
            }
            if (!a.fontColor.isEmpty() && !b.fontColor.isEmpty())
            {
                if (a.fontColor.compare(b.fontColor, Qt::CaseInsensitive) != 0)
                    return false;
            }
            return true;
        };

        std::vector<TextErrorInfo> errors;
        QMap<QString, TextErrorInfo> errorByFid;
        int hitCount = 0;

        // ----- 2a: 对每个有效敏感词做链式配对（char₁→char₂→...→charₙ） -----
        std::sort(activeWords.begin(), activeWords.end(),
            [](const QString& a, const QString& b) { return a.length() > b.length(); });

        QSet<int> usedInHit;

        writeLog("--- 2a: 链式拼词扫描 ---");
        writeLog("");

        for (const QString& word : activeWords)
        {
            QList<QChar> wordChars;
            for (int i = 0; i < word.length(); ++i)
                wordChars.append(word[i]);

            QList<int> startCandidates = charIndex[wordChars[0]];
            QString wordHeader = QString("[词=\"%1\" 长度=%2]").arg(word).arg(wordChars.size());
            writeLog(wordHeader + QString(" 起点候选数=%1 已用=%2")
                     .arg(startCandidates.size()).arg(
                         [&](){ int c=0; for(int s:startCandidates) if(usedInHit.contains(s)) c++; return c; }()));

            int startAttempt = 0;
            for (int startIdx : startCandidates)
            {
                ++startAttempt;
                if (usedInHit.contains(startIdx))
                {
                    writeLog(QString("  [起点#%1] idx=%2 text=\"%3\" → 已被占用，跳过")
                             .arg(startAttempt).arg(startIdx).arg(allFeatures[startIdx].text));
                    continue;
                }

                writeLog(QString("  [起点#%1] idx=%2 text=\"%3\" pos=(%4,%5) group=\"%6\"")
                         .arg(startAttempt).arg(startIdx)
                         .arg(allFeatures[startIdx].text)
                         .arg(allFeatures[startIdx].pos.x(), 0, 'f', 1)
                         .arg(allFeatures[startIdx].pos.y(), 0, 'f', 1)
                         .arg(allFeatures[startIdx].annoGroupId));

                const int MAX_CANDIDATES = 5;
                std::function<bool(QList<int>&, int, int, const QString&)> tryBuildChain;
                tryBuildChain = [&](QList<int>& chain, int wc, int prevIdx, const QString& curGroupId) -> bool {
                    if (wc >= (int)wordChars.size())
                        return true;

                    struct Cand { int idx; double d2; bool sameGroup; };
                    QList<Cand> candidates;
                    const QgsPointXY& prevPos = allFeatures[prevIdx].pos;
                    QString effectiveGroup = curGroupId.isEmpty() ? QString() : curGroupId;
                    int totalInRange = 0;
                    int fontRejected = 0;
                    for (int candIdx : charIndex[wordChars[wc]])
                    {
                        if (usedInHit.contains(candIdx)) continue;
                        if (chain.contains(candIdx)) continue;
                        double dx = prevPos.x() - allFeatures[candIdx].pos.x();
                        double dy = prevPos.y() - allFeatures[candIdx].pos.y();
                        double d2 = dx * dx + dy * dy;
                        if (d2 <= PAIR_RADIUS_SQ)
                        {
                            totalInRange++;
                            if (!isFontConsistent(prevIdx, candIdx))
                            {
                                fontRejected++;
                                continue;
                            }
                            bool sg = !effectiveGroup.isEmpty()
                                   && allFeatures[candIdx].annoGroupId == effectiveGroup;
                            candidates.append({candIdx, d2, sg});
                        }
                    }
                    std::sort(candidates.begin(), candidates.end(),
                              [](const Cand& a, const Cand& b) {
                                  if (a.sameGroup != b.sameGroup) return a.sameGroup;
                                  return a.d2 < b.d2;
                              });

                    QString stepIndent = QString(chain.size() * 4, ' ');
                    writeLog(QString("%1[步%2→\"%3\"] 范围内=%4 字体拒=%5 有效候选=%6 effectiveGroup=\"%7\"")
                             .arg(stepIndent)
                             .arg(wc + 1)
                             .arg(wordChars[wc])
                             .arg(totalInRange)
                             .arg(fontRejected)
                             .arg(candidates.size())
                             .arg(effectiveGroup.isEmpty() ? "(无)" : effectiveGroup));

                    int showCount = qMin((int)candidates.size(), MAX_CANDIDATES + 3);
                    for (int ci = 0; ci < qMin((int)candidates.size(), showCount); ++ci)
                    {
                        const Cand& c = candidates[ci];
                        const FlatFeature& cf = allFeatures[c.idx];
                        bool willTry = ci < MAX_CANDIDATES;
                        writeLog(QString("%1  [%2%3] idx=%4 text=\"%5\" dist=%6 group=\"%7\" %8 pos=(%9,%10)")
                                 .arg(stepIndent)
                                 .arg(willTry ? "→" : " ")
                                 .arg(ci + 1)
                                 .arg(c.idx)
                                 .arg(cf.text)
                                 .arg(sqrt(c.d2), 0, 'f', 1)
                                 .arg(cf.annoGroupId.isEmpty() ? "(无)" : cf.annoGroupId)
                                 .arg(c.sameGroup ? "同组" : "不同组")
                                 .arg(cf.pos.x(), 0, 'f', 1)
                                 .arg(cf.pos.y(), 0, 'f', 1));
                    }
                    if (candidates.size() > showCount)
                        writeLog(QString("%1  ... (共%2个，仅显示前%3)")
                                 .arg(stepIndent).arg(candidates.size()).arg(showCount));

                    int tried = 0;
                    for (int ci = 0; ci < candidates.size() && tried < MAX_CANDIDATES; ++ci)
                    {
                        chain.append(candidates[ci].idx);
                        QString nextGroup = candidates[ci].sameGroup ? effectiveGroup
                                         : (allFeatures[candidates[ci].idx].annoGroupId.isEmpty() ? effectiveGroup : QString());

                        bool subOk = tryBuildChain(chain, wc + 1, candidates[ci].idx, nextGroup);
                        if (subOk)
                            return true;

                        chain.removeLast();
                        ++tried;
                    }

                    if (candidates.isEmpty())
                        writeLog(QString("%1  → 无有效候选，此路不通").arg(stepIndent));
                    else if (tried > 0)
                        writeLog(QString("%1  → 所有候选回溯失败").arg(stepIndent));

                    return false;
                };

                QList<int> chain;
                chain.append(startIdx);
                QString startGroupId = hasAnnoGroupId ? allFeatures[startIdx].annoGroupId : QString();
                if (!tryBuildChain(chain, 1, startIdx, startGroupId))
                {
                    writeLog("    [结果] 跳过 → 无法拼成完整链");
                    writeLog("");
                    continue;
                }

                {
                    QStringList chainInfo;
                    for (int idx : chain)
                        chainInfo << QString("%1(idx=%2)").arg(allFeatures[idx].text).arg(idx);
                    writeLog("    [链完整] " + chainInfo.join(" → ") + QString(" (链长=%1)").arg(chain.size()));
                }

                // 保护②：步长一致性
                if (chain.size() >= 3)
                {
                    QList<double> steps;
                    for (int k = 1; k < chain.size(); ++k)
                    {
                        double dx = allFeatures[chain[k-1]].pos.x() - allFeatures[chain[k]].pos.x();
                        double dy = allFeatures[chain[k-1]].pos.y() - allFeatures[chain[k]].pos.y();
                        steps.append(sqrt(dx * dx + dy * dy));
                    }
                    std::sort(steps.begin(), steps.end());
                    double medianStep = steps[steps.size() / 2];
                    double maxStep = steps.last();
                    writeLog(QString("    保护② 步长一致性: 步长=%1 中位数=%2 最大=%3")
                             .arg([&](){QStringList sl; for(double s:steps) sl<<QString::number(s,'f',1); return sl.join(",");}())
                             .arg(medianStep, 0, 'f', 1).arg(maxStep, 0, 'f', 1));
                    if (medianStep > 1.0 && maxStep > medianStep * 3.0)
                    {
                        writeLog("    [结果] 跳过 → 步长异常 (maxStep > medianStep × 3)");
                        writeLog("");
                        continue;
                    }
                    writeLog("    保护② → 通过");
                }

                // 保护③：注释组一致性
                if (hasAnnoGroupId)
                {
                    QString groupId;
                    for (int i = 0; i < chain.size(); ++i)
                    {
                        groupId = allFeatures[chain[i]].annoGroupId;
                        if (!groupId.isEmpty()) break;
                    }
                    if (!groupId.isEmpty())
                    {
                        bool sameGroup = true;
                        QStringList groupDetails;
                        for (int i = 0; i < chain.size(); ++i)
                        {
                            QString g = allFeatures[chain[i]].annoGroupId;
                            groupDetails << (g.isEmpty() ? "(无)" : g);
                            if (!g.isEmpty() && g != groupId)
                            {
                                sameGroup = false;
                            }
                        }
                        writeLog(QString("    保护③ 注释组一致性: 各组=%1 基准=\"%2\"")
                                 .arg(groupDetails.join(", ")).arg(groupId));
                        if (!sameGroup)
                        {
                            writeLog("    [结果] 跳过 → 注记组不一致 (不同原始注记的单字不应配对)");
                            writeLog("");
                            continue;
                        }
                        writeLog("    保护③ → 通过 (全同组)");
                    }
                    else
                    {
                        writeLog("    保护③ 注释组一致性: 所有链元素分组为空 → 跳过校验");
                    }
                }
                else
                {
                    writeLog("    保护③ 注释组一致性: 图层无分组字段 → 跳过");
                }

                // 保护④：双向近邻校验
                if (chain.size() == 2)
                {
                    int a = chain[0], b = chain[1];
                    bool sameGroupPair = hasAnnoGroupId
                        && !allFeatures[a].annoGroupId.isEmpty()
                        && allFeatures[a].annoGroupId == allFeatures[b].annoGroupId;

                    if (sameGroupPair)
                    {
                        writeLog("    保护④ 双向近邻: 同组对 → 跳过校验 (分组已确认归属)");
                    }
                    else
                    {
                        auto itA = nearNeighbors.find(a);
                        auto itB = nearNeighbors.find(b);
                        if (itA != nearNeighbors.end() && itB != nearNeighbors.end())
                        {
                            bool aInBtop3 = itB.value().contains(a);
                            bool bInAtop3 = itA.value().contains(b);

                            QStringList top3a, top3b;
                            for (int t : itA.value()) top3a << QString::number(t);
                            for (int t : itB.value()) top3b << QString::number(t);
                            writeLog(QString("    保护④ 双向近邻:")
                                     + QString(" a(%1)top3=[%2] b(%3)top3=[%4]")
                                     .arg(a).arg(top3a.join(","))
                                     .arg(b).arg(top3b.join(",")));
                            writeLog(QString("             a在b的top3?%1  b在a的top3?%2")
                                     .arg(aInBtop3 ? "是" : "否")
                                     .arg(bInAtop3 ? "是" : "否"));

                            if (!aInBtop3 || !bInAtop3)
                            {
                                writeLog("    [结果] 跳过 → 非双向前3近邻");
                                writeLog("");
                                continue;
                            }
                            writeLog("    保护④ → 通过 (互为前3近邻)");
                        }
                    }
                }

                // 链式配对成功
                QString chainStr;
                QVector<QPointF> chainPositions;
                for (int idx : chain)
                {
                    chainStr += allFeatures[idx].text;
                    chainPositions.append(QPointF(allFeatures[idx].pos.x(), allFeatures[idx].pos.y()));
                }

                hitCount++;

                TextErrorInfo chainErr;
                chainErr.featureId = allFeatures[chain[0]].fid;
                chainErr.actualText = chainStr;
                chainErr.expectedText = QString();
                chainErr.position = chainPositions[0];
                chainErr.allPositions = chainPositions;
                chainErr.sensitiveCategory = wordToCategory.value(word, QString());
                chainErr.errorDescription = QString("敏感文字合规性检查_注记[%1]包含敏感词[%2]")
                    .arg(chainStr, word);
                if (!chainErr.sensitiveCategory.isEmpty())
                    chainErr.errorDescription += QString(" (类别:%1)").arg(chainErr.sensitiveCategory);
                errors.push_back(chainErr);

                for (int idx : chain)
                {
                    usedInHit.insert(idx);
                    const FlatFeature& ff = allFeatures[idx];
                    TextErrorInfo shpErr;
                    shpErr.featureId = ff.fid;
                    shpErr.actualText = chainStr;
                    shpErr.expectedText = QString();
                    shpErr.position = QPointF(ff.pos.x(), ff.pos.y());
                    shpErr.sensitiveCategory = chainErr.sensitiveCategory;
                    shpErr.errorDescription = chainErr.errorDescription;
                    errorByFid.insert(ff.fid, shpErr);
                }

                QString chainCat = wordToCategory.value(word);
                writeLog(QString("    [结果] ✗ 命中! \"%1\" 匹配敏感词\"%2\" (链长=%3, 命中累计=%4, 类别=%5)")
                         .arg(chainStr, word).arg(chain.size()).arg(hitCount).arg(chainCat.isEmpty() ? QStringLiteral("-") : chainCat));
                writeLog("");
            }
        }

        // ----- 2b: 多字符文本（≥2字）直接子串检查 -----
        writeLog("--- 2b: 多字符文本直接匹配 ---");
        QSet<QString> checkedMultiTexts;
        for (int multiIdx : multiCharIndices)
        {
            const QString& multiText = allFeatures[multiIdx].text;

            if (checkedMultiTexts.contains(multiText))
                continue;
            checkedMultiTexts.insert(multiText);

            QString multiTextNormalized = NormalizeTextForMatch(multiText);

            QStringList matchedWords;
            for (const QString& word : sensitiveWords)
            {
                if (multiTextNormalized.contains(word))
                    matchedWords.append(word);
            }

            if (!matchedWords.isEmpty())
            {
                hitCount++;

                QString joinedWords = matchedWords.join(',');
                auto ti = textIndex.constFind(multiText);
                if (ti != textIndex.constEnd())
                {
                    for (const AnnoFeatureInfo& info : ti.value())
                    {
                        QStringList matchedCats;
                        for (const QString& mw : matchedWords)
                        {
                            QString cat = wordToCategory.value(mw);
                            if (!cat.isEmpty()) matchedCats.append(cat);
                        }
                        matchedCats.removeDuplicates();

                        QPointF pos(info.position.x(), info.position.y());
                        TextErrorInfo err;
                        err.featureId = info.fid;
                        err.actualText = multiText;
                        err.expectedText = QString();
                        err.position = pos;
                        err.allPositions.append(pos);
                        err.sensitiveCategory = matchedCats.join(',');
                        err.errorDescription = QString("敏感文字合规性检查_注记[%1]包含敏感词[%2]")
                            .arg(multiText, joinedWords);
                        if (!err.sensitiveCategory.isEmpty())
                            err.errorDescription += QString(" (类别:%1)").arg(err.sensitiveCategory);
                        errors.push_back(err);
                        errorByFid.insert(info.fid, err);
                    }
                }

                QStringList mcCats;
                for (const QString& mw : matchedWords) { QString c = wordToCategory.value(mw); if (!c.isEmpty()) mcCats.append(c); }
                mcCats.removeDuplicates();
                writeLog(QString("✗ \"%1\" 包含敏感词[%2] (类别:%3)")
                         .arg(multiText, joinedWords, mcCats.isEmpty() ? QStringLiteral("-") : mcCats.join(',')));
            }
        }
        writeLog("");

        // ========== 结果汇总 ==========
        int totalAnnotations = 0;
        for (auto ti = textIndex.constBegin(); ti != textIndex.constEnd(); ++ti)
        {
            if (ti.key().length() >= 2)
                ++totalAnnotations;
        }

        writeLog("============================================");
        writeLog("  结果汇总");
        writeLog("============================================");
        writeLog(QString("  敏感词总数: %1").arg(sensitiveWords.size()));
        writeLog(QString("  有效敏感词: %1 (排除整词后)").arg(activeWords.size()));
        writeLog(QString("  总注记: %1").arg(totalAnnotations));
        writeLog(QString("  单CJK字符: %1 (过滤后: %2)").arg(singleCjkIndices.size()).arg(filteredSingleIndices.size()));
        writeLog(QString("  多字符文本: %1").arg(multiCharIndices.size()));
        writeLog(QString("  自适应配对半径: %1").arg(pairRadius, 0, 'f', 1));
        writeLog(QString("  分组字段: %1").arg(hasAnnoGroupId ? "存在" : "不存在"));
        writeLog(QString("  命中数: %1").arg(hitCount));
        writeLog("============================================");

        // ========== 写入 JSON 报告 ==========
        SE_Error jsonErr = WriteSensitiveCheckJsonReport(
            errors, szOutputJsonPath, totalAnnotations, hitCount);
        if (jsonErr != SE_ERROR_NONE)
        {
            writeLog("[错误] JSON写入失败");
            if (logFile.isOpen())
                logFile.close();
            delete layer;
            return jsonErr;
        }

        // ========== 写入错误 SHP ==========
        if (!errorByFid.isEmpty())
        {
            QMap<QString, const TextErrorInfo*> errPtrMap;
            for (auto et = errorByFid.constBegin(); et != errorByFid.constEnd(); ++et)
                errPtrMap.insert(et.key(), &et.value());

            SE_Error shpErr = WriteErrorShp(errPtrMap, layer, szOutputShpPath);
            if (shpErr != SE_ERROR_NONE)
                qWarning() << "[SensitiveContentCheck] 写入错误SHP失败";
        }

        delete layer;

        writeLog("");
        writeLog("日志结束: " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
        if (logFile.isOpen())
            logFile.close();

        return SE_ERROR_NONE;
    }
    catch (const std::exception& e)
    {
        auto writeLogEx = [&](const QString& msg) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << msg << "\n";
        };
        writeLogEx("");
        writeLogEx(QString("异常: %1").arg(e.what()));
        writeLogEx("日志结束: " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
        if (logFile.isOpen())
            logFile.close();
        if (layer) delete layer;
        std::string errMsg = std::string("算法处理异常: ") + e.what();
        return WriteFailureJsonAndReturn212(SE_ERROR_FAILURE, szOutputJsonPath, errMsg.c_str());
    }
    catch (...)
    {
        auto writeLogEx = [&](const QString& msg) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << msg << "\n";
        };
        writeLogEx("");
        writeLogEx("异常: 未知异常");
        writeLogEx("日志结束: " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
        if (logFile.isOpen())
            logFile.close();
        if (layer) delete layer;
        return WriteFailureJsonAndReturn212(SE_ERROR_FAILURE, szOutputJsonPath, "算法处理时发生未知异常");
    }
}
