#define _HAS_STD_BYTE 0
#include "se_admin_color_style_check_algorithm.h"

#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <QtXml/QDomDocument>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QMap>
#include <QColor>

struct AdminColorCheckConfig
{
    int maxColorCount = 4;
    double maxSingleColorRatio = 0.35;
    double maxSingleAreaRatio = 0.45;
    double colorBalanceThreshold = 0.20;
    double largeAreaThreshold = 500000.0;
    int colorTolerance = 15;
    QString fillColorField = "fill_color";
    
    // RGB 浅色判断下限
    int minLightR = 150;
    int minLightG = 150;
    int minLightB = 150;
    bool useRgbThreshold = false; // 标记是否启用了 RGB 阈值
};

static AdminColorCheckConfig g_AdminColorConfig;

static QString toLocalEncoding(const char* utf8Str)
{
    return QString::fromUtf8(utf8Str);
}

static void appendLogLine(QString* pLogText, const QString& text)
{
    if (!pLogText || text.isEmpty())
        return;

    if (!pLogText->isEmpty())
        pLogText->append('\n');

    pLogText->append(text);
}

static QColor parseColorValue(const QString& rawValue)
{
    QString value = rawValue.trimmed();
    if (value.isEmpty())
        return QColor();

    QRegularExpression cmykRe(R"(cmyk\(\s*([0-9]+(?:\.[0-9]+)?)\s*,\s*([0-9]+(?:\.[0-9]+)?)\s*,\s*([0-9]+(?:\.[0-9]+)?)\s*,\s*([0-9]+(?:\.[0-9]+)?)\s*\))",
                              QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch cmykMatch = cmykRe.match(value);
    if (cmykMatch.hasMatch()) {
        double c = cmykMatch.captured(1).toDouble();
        double m = cmykMatch.captured(2).toDouble();
        double y = cmykMatch.captured(3).toDouble();
        double k = cmykMatch.captured(4).toDouble();
        int r = qRound(255 * (1 - c / 100.0) * (1 - k / 100.0));
        int g = qRound(255 * (1 - m / 100.0) * (1 - k / 100.0));
        int b = qRound(255 * (1 - y / 100.0) * (1 - k / 100.0));
        return QColor(r, g, b);
    }

    QRegularExpression rgbRe(R"(rgb\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))",
                             QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch rgbMatch = rgbRe.match(value);
    if (rgbMatch.hasMatch()) {
        int r = rgbMatch.captured(1).toInt();
        int g = rgbMatch.captured(2).toInt();
        int b = rgbMatch.captured(3).toInt();
        return QColor(r, g, b);
    }

    if (value.startsWith("#")) {
        QColor c(value);
        if (c.isValid())
            return c;
    }

    QColor namedColor(value);
    if (namedColor.isValid())
        return namedColor;

    return QColor();
}

static QString normalizeColorKey(const QString& rawValue)
{
    QColor c = parseColorValue(rawValue);
    if (c.isValid()) {
        return QString("#%1%2%3")
            .arg(c.red(), 2, 16, QLatin1Char('0'))
            .arg(c.green(), 2, 16, QLatin1Char('0'))
            .arg(c.blue(), 2, 16, QLatin1Char('0'))
            .toUpper();
    }
    return rawValue.trimmed().toLower();
}

static bool isSameBaseColor(const QColor& c1, const QColor& c2, int tolerance)
{
    if (!c1.isValid() || !c2.isValid())
        return false;

    return qAbs(c1.red() - c2.red()) <= tolerance &&
           qAbs(c1.green() - c2.green()) <= tolerance &&
           qAbs(c1.blue() - c2.blue()) <= tolerance;
}

static bool isLightColor(const QColor& c)
{
    if (!c.isValid())
        return false;
    return c.lightness() >= 200 || c.value() >= 220;
}
// ==== 在这里插入新的 RGB 判断函数 ====
static bool isLightColorByRgbThreshold(const QColor& c, int minR, int minG, int minB)
{
    if (!c.isValid()) return false;
    // 浅色在 RGB 中表现为数值较大（越接近 255 越白/越浅）
    return (c.red() >= minR && c.green() >= minG && c.blue() >= minB);
}
// =====================================

static QString normalizeNameKey(const QString& raw)
{
    QString value = raw.trimmed();
    value.remove(QRegularExpression(QStringLiteral("\\s+")));
    return value.toLower();
}

static QStringList buildNameVariants(const QString& raw)
{
    QStringList variants;
    QString value = raw.trimmed();
    if (value.isEmpty())
        return variants;

    variants << value;

    QString noPrefix = value;
    while (noPrefix.startsWith(QStringLiteral("属"))) {
        noPrefix.remove(0, 1);
    }
    if (!noPrefix.isEmpty() && noPrefix != value) {
        variants << noPrefix;
    }

    return variants;
}

static double geometryArea(OGRGeometry* geom)
{
    if (!geom)
        return 0.0;

    OGRwkbGeometryType type = geom->getGeometryType();
    if (type == wkbPolygon || type == wkbPolygon25D) {
        return static_cast<OGRPolygon*>(geom)->get_Area();
    }
    else if (type == wkbMultiPolygon || type == wkbMultiPolygon25D) {
        OGRMultiPolygon* mp = static_cast<OGRMultiPolygon*>(geom);
        double area = 0.0;
        for (int i = 0; i < mp->getNumGeometries(); ++i) {
            OGRGeometry* sub = mp->getGeometryRef(i);
            if (sub && (sub->getGeometryType() == wkbPolygon || sub->getGeometryType() == wkbPolygon25D)) {
                area += static_cast<OGRPolygon*>(sub)->get_Area();
            }
        }
        return area;
    }

    return 0.0;
}

static bool featuresAreAdjacent(OGRGeometry* a, OGRGeometry* b)
{
    if (!a || !b)
        return false;
    return a->Touches(b) || (a->Intersects(b) && !a->Equals(b));
}

static QString lookupGdbNameByCentroid(OGRLayer* poGdbLayer, OGRGeometry* geom, int gdbNameFieldIndex, QString* pLogText, int shpFid)
{
    if (!poGdbLayer || !geom || gdbNameFieldIndex < 0) return QString();

    // 1. 获取 SHP 当前面的外接矩形范围 (Envelope)
    OGREnvelope envelope;
    geom->getEnvelope(&envelope);
    
    // 2. 用这个范围作为空间过滤器，这样能抓到重叠的所有面，而不只是一个点
    poGdbLayer->SetSpatialFilterRect(envelope.MinX, envelope.MinY, envelope.MaxX, envelope.MaxY);
    poGdbLayer->ResetReading();

    QString bestName;
    double shpArea = geometryArea(geom);
    double minAreaDiff = 1e30; 

    OGRFeature* poGdbFeature = nullptr;
    while ((poGdbFeature = poGdbLayer->GetNextFeature()) != nullptr) {
        OGRGeometry* gdbGeom = poGdbFeature->GetGeometryRef();
        if (gdbGeom && gdbGeom->Intersects(geom)) { // 进一步确认确实有空间相交
            double gdbArea = geometryArea(gdbGeom);
            double diff = qAbs(shpArea - gdbArea);
            
            const char* nameValueC = poGdbFeature->GetFieldAsString(gdbNameFieldIndex);
            QString currentName = nameValueC ? QString::fromUtf8(nameValueC) : QStringLiteral("未知");

            // 只有面积差异非常小（例如小于 SHP 面积的 20%），才认为是真匹配
            if (diff < shpArea * 0.2 && diff < minAreaDiff) {
                minAreaDiff = diff;
                bestName = currentName;
            }
        }
        OGRFeature::DestroyFeature(poGdbFeature);
    }

    poGdbLayer->SetSpatialFilter(nullptr);
    return bestName;
}


static void WriteJsonResult(
    const char* szOutputJsonPath,
    bool success,
    bool passed,
    const QJsonArray& errorDetails = QJsonArray(),
    const QJsonObject& extraInfo = QJsonObject())
{
    QJsonObject item;
    item["id"] = "306";
    item["name"] = QStringLiteral("行政区划颜色检查");
    item["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");

    if (passed) {
        item["info"] = QStringLiteral("行政区划颜色分布符合设色规范");
    } else {
        item["info"] = QStringLiteral("行政区划颜色存在设色冲突或不均衡");
    }

    if (!errorDetails.isEmpty()) {
        QJsonObject errors;
        for (int i = 0; i < errorDetails.size(); ++i) {
            QJsonValue detail = errorDetails.at(i);
            errors[QString("error%1").arg(i + 1)] = detail;
        }
        item["errors"] = errors;
    } else {
        item["errors"] = QJsonObject();
    }

    for (auto it = extraInfo.begin(); it != extraInfo.end(); ++it) {
        item[it.key()] = it.value();
    }

    QJsonObject root;
    root["check_item"] = item;

    QJsonDocument doc(root);
    QFile jsonFile(szOutputJsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
}

static void parseLightColorList(const QString& rawList, QList<QColor>& outColors)
{
    QStringList parts = rawList.split(';', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        QColor c = parseColorValue(part);
        if (c.isValid())
            outColors.append(c);
    }
}

SE_Error ParseXmlForAdminColorStyle(const char* szSchemaXmlPath, int& nMaxColorCount)
{
    g_AdminColorConfig = AdminColorCheckConfig();

    QFile file(szSchemaXmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn)) {
        file.close();
        return SE_ERROR_XML_PARSE_FAILED;
    }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int i = 0; i < checkItems.count(); ++i) {
        QDomElement item = checkItems.at(i).toElement();
        QDomNodeList rules = item.elementsByTagName("rule");
        for (int j = 0; j < rules.count(); ++j) {
            QDomElement rule = rules.at(j).toElement();
            QDomElement nameElem = rule.firstChildElement("name");
            QDomElement valueElem = rule.firstChildElement("value");
            if (nameElem.isNull() || valueElem.isNull())
                continue;

            QString ruleName = nameElem.text().trimmed();
            QString ruleValue = valueElem.text().trimmed();

            if (ruleName.contains(QStringLiteral("最大颜色数")) ||
                ruleName.contains(QStringLiteral("颜色数量"))) {
                bool ok = false;
                int v = ruleValue.toInt(&ok);
                if (ok && v > 0)
                    g_AdminColorConfig.maxColorCount = v;
            }
            else if (ruleName.contains(QStringLiteral("最大单色")) ||
                     ruleName.contains(QStringLiteral("单色占比"))) {
                bool ok = false;
                double v = ruleValue.toDouble(&ok);
                if (ok && v > 0.0 && v <= 1.0)
                    g_AdminColorConfig.maxSingleColorRatio = v;
            }
            else if (ruleName.contains(QStringLiteral("最大单色面积占比")) ||
                     ruleName.contains(QStringLiteral("单色面积占比"))) {
                bool ok = false;
                double v = ruleValue.toDouble(&ok);
                if (ok && v > 0.0 && v <= 1.0)
                    g_AdminColorConfig.maxSingleAreaRatio = v;
            }
            else if (ruleName.contains(QStringLiteral("颜色均匀度"))) {
                bool ok = false;
                double v = ruleValue.toDouble(&ok);
                if (ok && v >= 0.0 && v <= 1.0)
                    g_AdminColorConfig.colorBalanceThreshold = v;
            }
            else if (ruleName.contains(QStringLiteral("大面积")) ||
                     ruleName.contains(QStringLiteral("面积阈值"))) {
                bool ok = false;
                double v = ruleValue.toDouble(&ok);
                if (ok && v >= 0.0)
                    g_AdminColorConfig.largeAreaThreshold = v;
            }
            else if (ruleName.contains(QStringLiteral("浅色")) &&
                    ruleName.contains(QStringLiteral("RGB"))) {
                QStringList parts = ruleValue.split(',', Qt::SkipEmptyParts);
                if (parts.size() >= 3) {
                    g_AdminColorConfig.minLightR = parts[0].toInt();
                    g_AdminColorConfig.minLightG = parts[1].toInt();
                    g_AdminColorConfig.minLightB = parts[2].toInt();
                    g_AdminColorConfig.useRgbThreshold = true;
                }
            }
            else if (ruleName.contains(QStringLiteral("颜色容差")) ||
                     ruleName.contains(QStringLiteral("对比阈值"))) {
                bool ok = false;
                int v = ruleValue.toInt(&ok);
                if (ok && v >= 0)
                    g_AdminColorConfig.colorTolerance = v;
            }
            else if (ruleName.contains(QStringLiteral("填充颜色字段")) ||
                     ruleName.contains(QStringLiteral("fill_color"))) {
                if (!ruleValue.isEmpty())
                    g_AdminColorConfig.fillColorField = ruleValue;
            }
        }
    }

    nMaxColorCount = g_AdminColorConfig.maxColorCount;
    return SE_ERROR_NONE;
}

SE_Error AdminColorStyleCheck(
    const char* szShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed,
    const char* szGdbShpPath,
    QString* pLogText,
    const char* szGdbLayerName)
{
    bPassed = false;

    int maxColorCount = 0;
    SE_Error error = ParseXmlForAdminColorStyle(szSchemaXmlPath, maxColorCount);
    if (error != SE_ERROR_NONE) {
        WriteJsonResult(szOutputJsonPath, false, false);
        return error;
    }

    QString qPath = QString::fromUtf8(szShpPath);
    QFileInfo fileInfo(qPath);
    if (!fileInfo.exists()) {
        WriteJsonResult(szOutputJsonPath, false, false);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QString basePath = fileInfo.path() + "/" + fileInfo.completeBaseName();
    QFileInfo shxInfo(basePath + ".shx");
    QFileInfo dbfInfo(basePath + ".dbf");
    if (!shxInfo.exists() || !dbfInfo.exists()) {
        WriteJsonResult(szOutputJsonPath, false, false);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    GDALAllRegister();
#ifdef _WIN32
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "YES");
    CPLSetConfigOption("SHAPE_ENCODING", "UTF-8");
#else
    CPLSetConfigOption("GDAL_FILENAME_IS_UTF8", "NO");
    CPLSetConfigOption("SHAPE_ENCODING", "");
#endif

    std::string localPath = qPath.toUtf8().constData();
    GDALDataset* poDS = (GDALDataset*)GDALOpenEx(
        localPath.c_str(),
        GDAL_OF_VECTOR | GDAL_OF_READONLY,
        nullptr, nullptr, nullptr
    );
    if (!poDS) {
        const char* errMsg = CPLGetLastErrorMsg();
        QString detail = QStringLiteral("打开 shapefile 失败: %1").arg(errMsg ? QString::fromUtf8(errMsg) : QString());
        QJsonArray errArray;
        errArray.append(detail);
        WriteJsonResult(szOutputJsonPath, false, false, errArray);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer) {
        GDALClose(poDS);
        QJsonArray errArray;
        errArray.append(QStringLiteral("找不到图层"));
        WriteJsonResult(szOutputJsonPath, false, false, errArray);
        return SE_ERROR_LAYER_INVALID;
    }

    OGRFeatureDefn* poFDefn = poLayer->GetLayerDefn();
    int colorFieldIndex = poFDefn->GetFieldIndex(g_AdminColorConfig.fillColorField.toUtf8().constData());
    if (colorFieldIndex == -1) {
        GDALClose(poDS);
        QJsonArray errArray;
        QString msg = QStringLiteral("找不到字段：") + g_AdminColorConfig.fillColorField;
        errArray.append(msg);
        WriteJsonResult(szOutputJsonPath, false, false, errArray);
        return SE_ERROR_FIELD_NOT_FOUND;
    }

    auto findFieldIndex = [](OGRFeatureDefn* defn, const QStringList& candidates) -> int {
        for (const QString& candidate : candidates) {
            int idx = defn->GetFieldIndex(candidate.toUtf8().constData());
            if (idx != -1)
                return idx;
        }
        return -1;
    };

    int shpNameFieldIndex = findFieldIndex(poFDefn, { QStringLiteral("NAME"), QStringLiteral("Name"), QStringLiteral("name") });

    OGRLayer* poGdbLayer = nullptr;
    int gdbNameFieldIndex = -1;
    if (szGdbShpPath && QString::fromUtf8(szGdbShpPath).trimmed().isEmpty() == false) {
        QString gdbPath = QString::fromUtf8(szGdbShpPath);
        std::string gdbLocalPath = gdbPath.toUtf8().constData();
        GDALDataset* poGdbDS = (GDALDataset*)GDALOpenEx(
            gdbLocalPath.c_str(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY,
            nullptr, nullptr, nullptr);
        if (!poGdbDS) {
            appendLogLine(pLogText, QStringLiteral("无法打开 GDB/OGR 数据源: %1").arg(gdbPath));
        } else {
            // 使用CSV传入的GDB图层名，未提供则用默认BOUA6
            const char* layerName = (szGdbLayerName && szGdbLayerName[0] != '\0') ? szGdbLayerName : "BOUA6";
            poGdbLayer = poGdbDS->GetLayerByName(layerName);
            if (!poGdbLayer) {
                appendLogLine(pLogText, QStringLiteral("无法读取 GDB 图层: %1").arg(QString::fromUtf8(layerName)));
            } else {
                gdbNameFieldIndex = findFieldIndex(poGdbLayer->GetLayerDefn(),
                    { QStringLiteral("NAME"), QStringLiteral("Name"), QStringLiteral("name") });
                if (gdbNameFieldIndex == -1) {
                    appendLogLine(pLogText, QStringLiteral("GDB 图层 %1 缺少 NAME 字段").arg(QString::fromUtf8(layerName)));
                }
            }
        }
        if (!poGdbLayer && poGdbDS) {
            GDALClose(poGdbDS);
            poGdbDS = nullptr;
        }
    }

    std::map<QString, int> colorCount;
    std::map<QString, double> colorArea;
    std::map<QString, QColor> colorValueMap;
    std::vector<OGRFeature*> features;
    std::map<OGRFeature*, QString> featureColorKey;
    std::map<OGRFeature*, QString> featureNameMap;

    poLayer->ResetReading();
        OGRFeature* poFeature = nullptr;
        while ((poFeature = poLayer->GetNextFeature()) != nullptr) {
            const char* colorValueC = poFeature->GetFieldAsString(colorFieldIndex);
            QString colorValue = QString::fromUtf8(colorValueC ? colorValueC : "");
            QString colorKey = normalizeColorKey(colorValue);
            QColor parsedColor = parseColorValue(colorValue);

            QString gdbName;
            OGRFeature* clonedFeature = poFeature->Clone();
            if (clonedFeature) {
                if (gdbNameFieldIndex != -1) {
                    gdbName = lookupGdbNameByCentroid(poGdbLayer, clonedFeature->GetGeometryRef(), gdbNameFieldIndex, pLogText, poFeature->GetFID());                }
                if (gdbName.isEmpty() && shpNameFieldIndex != -1) {
                    const char* shpNameC = poFeature->GetFieldAsString(shpNameFieldIndex);
                    gdbName = QString::fromUtf8(shpNameC ? shpNameC : "");
                }

                // ================= 增加的具体诊断日志 =================
                if (gdbName.isEmpty()) {
                    appendLogLine(pLogText, QStringLiteral("[空间连接失败] SHP 面 (FID=%1) 在 GDB 中未能匹配到任何名称！").arg(poFeature->GetFID()));
                } else {
                    appendLogLine(pLogText, QStringLiteral("[提取成功] SHP 面 (FID=%1) 匹配到名称: '%2'，颜色: %3")
                                        .arg(poFeature->GetFID())
                                        .arg(gdbName)
                                        .arg(colorKey));
                }
                // ======================================================

                featureColorKey[clonedFeature] = colorKey;
                featureNameMap[clonedFeature] = gdbName;
                colorCount[colorKey] += 1;
                double area = geometryArea(clonedFeature->GetGeometryRef());
                colorArea[colorKey] += area;
                if (parsedColor.isValid())
                    colorValueMap[colorKey] = parsedColor;
                features.push_back(clonedFeature);
            }
            OGRFeature::DestroyFeature(poFeature);
        }

    int totalFeatures = static_cast<int>(features.size());
    if (totalFeatures == 0) {
        GDALClose(poDS);
        QJsonArray errArray;
        errArray.append(QStringLiteral("图层没有要素"));
        WriteJsonResult(szOutputJsonPath, false, false, errArray);
        return SE_ERROR_LAYER_INVALID;
    }

    QJsonArray errors;
    QSet<QString> uniqueColors;

    // ------------------------------------------------------------------
    // 基于 NAME 的主区 / 飞地颜色一致性检查
    // ------------------------------------------------------------------
    QMap<QString, QString> mainAreaColors;
    QMap<QString, QString> mainAreaColorsByKey;
    QList<std::pair<QString, QString>> enclaveEntries;

    for (OGRFeature* f : features) {
        QString name = featureNameMap[f].trimmed();
        if (name.isEmpty())
            continue;

        QString colorKey = featureColorKey[f];
        if (colorKey.isEmpty())
            continue;

        if (name.startsWith(QStringLiteral("属"))) {
            QString parentName = name.mid(1).trimmed();
            if (!parentName.isEmpty()) {
                enclaveEntries.append({name, parentName});
            }
        } else {
            mainAreaColors[name] = colorKey;
            mainAreaColorsByKey[normalizeNameKey(name)] = colorKey;
        }
    }

    for (const auto& entry : enclaveEntries) {
        QString enclaveName = entry.first;
        QString parentName = entry.second;
        QString parentKey = normalizeNameKey(parentName);

        appendLogLine(pLogText, QStringLiteral("[飞地比对] 开始检查飞地：'%1'，准备寻找主区：'%2' (处理后的Key为:'%3')")
                              .arg(enclaveName).arg(parentName).arg(parentKey));

        QString enclaveColor;
        for (OGRFeature* f : features) {
            if (normalizeNameKey(featureNameMap[f]) == normalizeNameKey(enclaveName)) {
                enclaveColor = featureColorKey[f];
                break;
            }
        }

        if (enclaveColor.isEmpty()) {
            errors.append(QStringLiteral("飞地要素 %1 未找到填充颜色").arg(enclaveName));
            continue;
        }

        if (!mainAreaColorsByKey.contains(parentKey)) {
            // ================= 增加的致命错误追踪日志 =================
            QString existKeys = mainAreaColorsByKey.keys().join(", ");
            appendLogLine(pLogText, QStringLiteral("[致命错误] 字典中未找到主区Key '%1'。")
                                      .arg(parentKey));
            appendLogLine(pLogText, QStringLiteral("[提示] 目前系统成功识别到的主区有: %1").arg(existKeys));
            // ==========================================================

            errors.append(QStringLiteral("未找到主区：%1 对应 %2").arg(parentName).arg(enclaveName));
            continue;
        }

        QString parentColor = mainAreaColorsByKey.value(parentKey);
        if (parentColor != enclaveColor) {
            errors.append(QStringLiteral("飞地颜色不一致：%1 颜色=%2，主区 %3 颜色=%4")
                          .arg(enclaveName)
                          .arg(enclaveColor)
                          .arg(parentName)
                          .arg(parentColor));
        } else {
             appendLogLine(pLogText, QStringLiteral("[检查通过] 飞地 '%1' 与主区 '%2' 颜色一致。").arg(enclaveName).arg(parentName));
        }
    }

    for (auto& pair : colorCount) {
        uniqueColors.insert(pair.first);
    }

    if (uniqueColors.size() != maxColorCount) {
        if (uniqueColors.size() > maxColorCount) {
            errors.append(QStringLiteral("颜色数量超出限制: 实际 %1 种, 允许 %2 种").arg(uniqueColors.size()).arg(maxColorCount));
        } else {
            errors.append(QStringLiteral("颜色种类不足: 实际 %1 种, 期望 %2 种").arg(uniqueColors.size()).arg(maxColorCount));
        }
    }

    int maxCount = 0;
    int minCount = totalFeatures;
    for (const auto& pair : colorCount) {
        int count = pair.second;
        maxCount = qMax(maxCount, count);
        minCount = qMin(minCount, count);
        double ratio = double(count) / double(totalFeatures);
        if (ratio > g_AdminColorConfig.maxSingleColorRatio) {
            errors.append(QStringLiteral("颜色 '%1' 过多，占比 %2% 超过阈值 %3%")
                          .arg(pair.first)
                          .arg(ratio * 100.0, 0, 'f', 1)
                          .arg(g_AdminColorConfig.maxSingleColorRatio * 100.0, 0, 'f', 1));
        }
    }

    if (errors.isEmpty() && minCount >= 0) {
        double maxRatio = double(maxCount) / double(totalFeatures);
        double minRatio = double(minCount) / double(totalFeatures);
        if ((maxRatio - minRatio) > g_AdminColorConfig.colorBalanceThreshold) {
            errors.append(QStringLiteral("颜色分布不均匀: 最大占比 %1%, 最小占比 %2%, 差值超过阈值 %3%")
                          .arg(maxRatio * 100.0, 0, 'f', 1)
                          .arg(minRatio * 100.0, 0, 'f', 1)
                          .arg(g_AdminColorConfig.colorBalanceThreshold * 100.0, 0, 'f', 1));
        }
    }

    // ------------------------------------------------------------------
    // 基于面积占比的颜色均匀度检查（双轨制第二轨）
    // ------------------------------------------------------------------
    double totalArea = 0.0;
    for (const auto& pair : colorArea) {
        totalArea += pair.second;
    }

    if (totalArea > 0.0) {
        for (const auto& pair : colorArea) {
            double areaRatio = pair.second / totalArea;
            if (areaRatio > g_AdminColorConfig.maxSingleAreaRatio) {
                errors.append(QStringLiteral("颜色 '%1' 面积覆盖过大: 面积占比 %2% 超过阈值 %3%")
                              .arg(pair.first)
                              .arg(areaRatio * 100.0, 0, 'f', 1)
                              .arg(g_AdminColorConfig.maxSingleAreaRatio * 100.0, 0, 'f', 1));
            }
        }
    }

    for (OGRFeature* f : features) {
            double area = geometryArea(f->GetGeometryRef());
            if (area >= g_AdminColorConfig.largeAreaThreshold) {
                QString key = featureColorKey[f];
                QColor c = parseColorValue(key);
                auto it = colorValueMap.find(key);
                if (it != colorValueMap.end()) {
                    c = it->second;
                }
                
                bool ok = false;
                if (c.isValid()) {
                    if (g_AdminColorConfig.useRgbThreshold) {
                        // 使用 XML 中配置的 RGB 下限阈值判断
                        ok = isLightColorByRgbThreshold(c, 
                                                        g_AdminColorConfig.minLightR, 
                                                        g_AdminColorConfig.minLightG, 
                                                        g_AdminColorConfig.minLightB);
                    } else {
                        // 如果 XML 里没写 RGB 阈值，兼容原有的明度判断逻辑
                        ok = isLightColor(c);
                    }
                }
                
                if (!ok) {
                    errors.append(QStringLiteral("大面积政区 FID=%1 未使用浅色系 (颜色值: %2)").arg(f->GetFID()).arg(key));
                }
            }
        }

    for (size_t i = 0; i < features.size(); ++i) {
        OGRFeature* f1 = features[i];
        OGRGeometry* g1 = f1->GetGeometryRef();
        QString key1 = featureColorKey[f1];
        QColor c1;
        auto it1 = colorValueMap.find(key1);
        if (it1 != colorValueMap.end()) {
            c1 = it1->second;
        } else {
            c1 = parseColorValue(key1);
        }

        for (size_t j = i + 1; j < features.size(); ++j) {
            OGRFeature* f2 = features[j];
            OGRGeometry* g2 = f2->GetGeometryRef();
            if (!featuresAreAdjacent(g1, g2))
                continue;

            QString key2 = featureColorKey[f2];

            QColor c2;
            auto it2 = colorValueMap.find(key2);
            if (it2 != colorValueMap.end()) {
                c2 = it2->second;
            } else {
                c2 = parseColorValue(key2);
            }

            if (key1 == key2 || isSameBaseColor(c1, c2, g_AdminColorConfig.colorTolerance)) {
                errors.append(QStringLiteral("相邻政区 FID=%1 与 FID=%2 颜色冲突: %3 / %4")
                              .arg(f1->GetFID())
                              .arg(f2->GetFID())
                              .arg(key1)
                              .arg(key2));
            }
        }
    }

    bool passed = errors.isEmpty();
    WriteJsonResult(szOutputJsonPath, passed, passed, errors);
    bPassed = passed;

    for (OGRFeature* f : features) {
        OGRFeature::DestroyFeature(f);
    }
    GDALClose(poDS);

    return SE_ERROR_NONE;
}