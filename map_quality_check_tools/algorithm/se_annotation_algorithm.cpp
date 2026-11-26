#include "se_annotation_algorithm.h"
#include "se_font_mapping.h" // 包含内置字体映射
#include <QDomDocument>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QColor>
#include <QRegularExpression>
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsFeatureIterator.h>
#include <qgsmessagelog.h>
#include <QgsPointXY.h>
#include <QStringList>
#include <QgsCoordinateTransform.h>
#include <QgsProject.h>
#include <qgsexception.h>

namespace
{
QStringList splitRegionRuleValues(const QString &value)
{
    return value.split(QRegularExpression(QStringLiteral("[;；]")), Qt::KeepEmptyParts);
}

QStringList splitRuleValues(const QString &value)
{
    QStringList values;
    for (const QString &part : value.split(QRegularExpression(QStringLiteral("[;；]")), Qt::SkipEmptyParts))
    {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty())
            values.append(trimmed);
    }
    return values;
}
}

SE_AnnotationAlgorithm::SE_AnnotationAlgorithm()
    : m_fontCheck(true), m_fontSizeCheck(true), m_colorCheck(true), m_italicCheck(true), m_currentScale("100000")
{
    // 加载内置字体映射
    m_fontMappings = SE_FontMapping::getBuiltinFontMappings();
    QgsMessageLog::logMessage(QString("内置字体映射已加载，共 %1 项映射").arg(m_fontMappings.size()),
                              "AnnotationCheck", Qgis::Info);
}

SE_AnnotationAlgorithm::~SE_AnnotationAlgorithm()
{
}

bool SE_AnnotationAlgorithm::loadCheckRules(const QString &xmlFilePath, const QString &scale)
{
    QFile file(xmlFilePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QgsMessageLog::logMessage(QString("无法打开质检规则文件: %1").arg(xmlFilePath),
                                  "AnnotationCheck", Qgis::Critical);
        return false;
    }

    QString xmlContent = file.readAll();
    file.close();

    m_currentScale = scale;
    m_lastError.clear();
    m_regionCheckEnabled = false;
    m_featureRegions.clear();
    return parseXmlRules(xmlContent, scale);
}

bool SE_AnnotationAlgorithm::parseXmlRules(const QString &xmlContent, const QString &scale)
{
    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;

    if (!doc.setContent(xmlContent, &errorMsg, &errorLine, &errorColumn))
    {
        QgsMessageLog::logMessage(QString("XML解析错误: %1 (行%2, 列%3)")
                                      .arg(errorMsg)
                                      .arg(errorLine)
                                      .arg(errorColumn),
                                  "AnnotationCheck", Qgis::Critical);
        return false;
    }

    QDomElement root = doc.documentElement();
    QDomNodeList scaleNodes = root.elementsByTagName("scale");

    QList<ScaleConfig> scaleConfigs;

    // 解析所有比例尺配置
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        ScaleConfig config;

        // 获取比例尺值
        QDomNodeList valueNodes = scaleElement.elementsByTagName("value");
        if (!valueNodes.isEmpty())
        {
            config.value = valueNodes.at(0).toElement().text().trimmed(); // 只存分母字符串
        }

        // 解析检查项
        QDomNodeList checkItems = scaleElement.elementsByTagName("check_item");
        for (int j = 0; j < checkItems.count(); ++j)
        {
            QDomElement itemElement = checkItems.at(j).toElement();
            CheckItem item;

            // 新增：获取检查项ID
            QDomNodeList idNodes = itemElement.elementsByTagName("id");
            if (!idNodes.isEmpty())
            {
                bool ok = false;
                item.id = idNodes.at(0).toElement().text().trimmed().toInt(&ok);
                if (!ok)
                    item.id = -1;
            }

            // 获取检查项名称
            QDomNodeList nameNodes = itemElement.elementsByTagName("name");
            if (!nameNodes.isEmpty())
            {
                item.name = nameNodes.at(0).toElement().text().trimmed();
            }

            // 解析规则
            QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
            for (int k = 0; k < ruleNodes.count(); ++k)
            {
                QDomElement ruleElement = ruleNodes.at(k).toElement();
                CheckRule rule;

                // 获取规则的各个属性
                QDomNodeList ruleNameNodes = ruleElement.elementsByTagName("name");
                if (!ruleNameNodes.isEmpty())
                {
                    rule.name = ruleNameNodes.at(0).toElement().text().trimmed();
                }

                QDomNodeList descNodes = ruleElement.elementsByTagName("description");
                if (!descNodes.isEmpty())
                {
                    rule.description = descNodes.at(0).toElement().text().trimmed();
                }

                QDomNodeList valueNodes = ruleElement.elementsByTagName("value");
                if (!valueNodes.isEmpty())
                {
                    rule.value = valueNodes.at(0).toElement().text().trimmed();
                }

                QDomNodeList unitNodes = ruleElement.elementsByTagName("unit");
                if (!unitNodes.isEmpty())
                {
                    rule.unit = unitNodes.at(0).toElement().text().trimmed();
                }

                item.rules.append(rule);

                QgsMessageLog::logMessage(QString("加载规则: %1/%2 -> %3")
                                              .arg(item.name)
                                              .arg(rule.name)
                                              .arg(rule.value),
                                          "AnnotationCheck", Qgis::Info);
            }

            config.items.append(item);
        }

        scaleConfigs.append(config);
    }

    // 根据输入比例尺选择合适的配置
    m_activeConfig = selectScaleConfig(scaleConfigs, scale);

    QgsMessageLog::logMessage(QString("为比例尺 %1 选择了配置，包含 %2 个检查项")
                                  .arg(scale)
                                  .arg(m_activeConfig.items.size()),
                              "AnnotationCheck", Qgis::Info);

    return !m_activeConfig.items.isEmpty();
}

ScaleConfig SE_AnnotationAlgorithm::selectScaleConfig(const QList<ScaleConfig> &configs, const QString &scale)
{
    ScaleConfig defaultConfig; // 空值配置作为默认
    ScaleConfig matchedConfig; // 匹配的配置

    for (const ScaleConfig &config : configs)
    {
        if (config.value.isEmpty())
        {
            // 记录空值配置作为默认选项
            defaultConfig = config;
        }
        else if (config.value == scale) // 直接用分母字符串匹配
        {
            // 找到完全匹配的配置
            matchedConfig = config;
            QgsMessageLog::logMessage(QString("找到匹配的比例尺配置: %1").arg(scale),
                                      "AnnotationCheck", Qgis::Info);
            return matchedConfig;
        }
    }

    // 如果没有找到匹配的，使用默认配置（空值配置）
    if (!defaultConfig.items.isEmpty())
    {
        QgsMessageLog::logMessage(QString("使用默认配置（与比例尺无关）"),
                                  "AnnotationCheck", Qgis::Info);
        return defaultConfig;
    }

    // 如果都没有，返回第一个配置或空配置
    if (!configs.isEmpty())
    {
        QgsMessageLog::logMessage(QString("使用第一个可用配置"),
                                  "AnnotationCheck", Qgis::Warning);
        return configs.first();
    }

    QgsMessageLog::logMessage(QString("未找到任何可用配置"),
                              "AnnotationCheck", Qgis::Critical);
    return ScaleConfig();
}

// 新增：按ID获取规则值
QString SE_AnnotationAlgorithm::getRuleValueById(int itemId, const QString &ruleName) const
{
    for (const CheckItem &item : m_activeConfig.items)
    {
        if (item.id == itemId)
        {
            if (ruleName.isEmpty())
            {
                return item.rules.isEmpty() ? QString() : item.rules.first().value;
            }
            for (const CheckRule &rule : item.rules)
            {
                if (rule.name == ruleName)
                    return rule.value;
            }
            return QString();
        }
    }
    return QString();
}

// 新增：按ID判断是否存在检查项
bool SE_AnnotationAlgorithm::hasCheckItemById(int itemId) const
{
    for (const CheckItem &item : m_activeConfig.items)
    {
        if (item.id == itemId)
            return true;
    }
    return false;
}

QString SE_AnnotationAlgorithm::getRuleValue(const QString &itemName, const QString &ruleName) const
{
    for (const CheckItem &item : m_activeConfig.items)
    {
        if (item.name == itemName)
        {
            if (ruleName.isEmpty() && !item.rules.isEmpty())
            {
                // 如果没有指定规则名，返回第一个规则的值
                return item.rules.first().value;
            }

            for (const CheckRule &rule : item.rules)
            {
                if (ruleName.isEmpty() || rule.name == ruleName)
                {
                    return rule.value;
                }
            }
        }
    }
    return QString();
}

void SE_AnnotationAlgorithm::setCheckOptions(bool fontCheck, bool fontSizeCheck, bool colorCheck, bool italicCheck)
{
    m_fontCheck = fontCheck;
    m_fontSizeCheck = fontSizeCheck;
    m_colorCheck = colorCheck;
    m_italicCheck = italicCheck;
}

bool SE_AnnotationAlgorithm::isCheckRequired(const QString &checkType) const
{
    return !getRuleValue(checkType).isEmpty();
}

QString SE_AnnotationAlgorithm::getFieldName(const QString &fullName) const
{
    // shp格式限制字段名最多10个字符
    return fullName.left(10);
}

void SE_AnnotationAlgorithm::setRegionLayerPaths(const QString &mainRegionPath, const QString &provincePath)
{
    m_mainRegionPath = mainRegionPath;
    m_provincePath = provincePath;
}

int SE_AnnotationAlgorithm::regionIndex(const QgsFeature &feature) const
{
    return m_regionCheckEnabled ? m_featureRegions.value(feature.id(), -1) : -1;
}

QString SE_AnnotationAlgorithm::regionName(const QgsFeature &feature) const
{
    const int index = regionIndex(feature);
    const QStringList names = {QStringLiteral("主区"), QStringLiteral("外区省内"), QStringLiteral("外区省外")};
    return index >= 0 ? names.at(index) : QString();
}

bool SE_AnnotationAlgorithm::prepareRegionCheck(int itemId, QgsVectorLayer *layer, const QList<QStringList> &values)
{
    m_lastError.clear();
    m_regionCheckEnabled = false;
    m_featureRegions.clear();
    const QStringList ruleNames = {QStringLiteral("关联主区"), QStringLiteral("关联省内"),
                                   QStringLiteral("省内筛选字段"), QStringLiteral("省内筛选值")};
    bool hasRegionRules = false;
    for (const CheckItem &item : m_activeConfig.items)
    {
        if (item.id != itemId)
            continue;
        for (const CheckRule &rule : item.rules)
            hasRegionRules = hasRegionRules || ruleNames.contains(rule.name);
    }
    if (!hasRegionRules)
        return true;

    auto fail = [this, itemId](const QString &message) {
        m_lastError = QString("检查项 %1 区域检查失败：%2").arg(itemId).arg(message);
        QgsMessageLog::logMessage(m_lastError, "AnnotationCheck", Qgis::Critical);
        return false;
    };
    for (const QString &name : ruleNames)
    {
        if (getRuleValueById(itemId, name).isEmpty())
            return fail(QString("缺少规则“%1”").arg(name));
    }
    if (!layer || !layer->isValid() || !layer->crs().isValid())
        return fail("注记图层或坐标系无效");
    for (const QStringList &list : values)
    {
        if (list.size() != 1 && list.size() != 3)
            return fail("区域取值必须为一个共用值或主区、外区省内、外区省外三个值，且不允许空项");
        for (const QString &value : list)
        {
            const QString text = value.trimmed();
            if (text.isEmpty())
                return fail("区域取值不允许空项");
            if (itemId == 202)
            {
                const QRegularExpression format(R"(^(\d+\.?\d*)(?:\*(\d+\.?\d*))?$)");
                const QRegularExpressionMatch match = format.match(text);
                if (!match.hasMatch() || match.captured(1).toDouble() <= 0 ||
                    (!match.captured(2).isEmpty() && match.captured(2).toDouble() <= 0))
                    return fail(QString("无法解析区域字号：%1").arg(text));
            }
            if (itemId == 203)
            {
                const QRegularExpression format(R"(^(?:C(\d+))?(?:M(\d+))?(?:Y(\d+))?(?:K(\d+))?$)");
                const QRegularExpressionMatch match = format.match(text);
                if (!match.hasMatch())
                    return fail(QString("无法解析区域 CMYK 颜色：%1").arg(text));
                for (int component = 1; component <= 4; ++component)
                {
                    bool ok = false;
                    const int number = match.captured(component).toInt(&ok);
                    if (!match.captured(component).isEmpty() && (!ok || number > 100))
                        return fail(QString("区域 CMYK 分量必须在 0～100 之间：%1").arg(text));
                }
            }
        }
    }
    if (m_mainRegionPath.isEmpty() || m_provincePath.isEmpty())
        return fail("必须传入主区实际面图层路径和省内 GDB 面图层路径");

    QgsVectorLayer mainLayer(m_mainRegionPath, "main_region", "ogr");
    QgsVectorLayer provinceLayer(m_provincePath, "province_region", "ogr");
    for (QgsVectorLayer *regionLayer : {&mainLayer, &provinceLayer})
    {
        if (!regionLayer->isValid() || regionLayer->geometryType() != QgsWkbTypes::PolygonGeometry ||
            !regionLayer->crs().isValid())
            return fail(QString("关联图层或面几何、坐标系无效：%1").arg(regionLayer->source()));
    }
    const QString fieldName = getRuleValueById(itemId, "省内筛选字段");
    const int fieldIndex = provinceLayer.fields().lookupField(fieldName);
    if (fieldIndex < 0)
        return fail(QString("省内 GDB 图层缺少筛选字段：%1").arg(fieldName));
    const QString fieldValue = getRuleValueById(itemId, "省内筛选值");

    auto validArea = [](QgsGeometry geometry) {
        try
        {
            if (!geometry.isEmpty() && !geometry.isGeosValid())
                geometry = geometry.makeValid();
        }
        catch (const QgsException &)
        {
            return QgsGeometry();
        }
        return geometry;
    };
    auto merge = [](const QVector<QgsGeometry> &geometries) {
        return geometries.size() == 1 ? geometries.first() : QgsGeometry::unaryUnion(geometries);
    };
    QVector<QgsGeometry> mainParts, provinceParts;
    try
    {
        for (QgsVectorLayer *regionLayer : {&mainLayer, &provinceLayer})
        {
            QgsCoordinateTransform transform(regionLayer->crs(), layer->crs(), QgsProject::instance()->transformContext());
            QgsFeatureIterator iterator = regionLayer->getFeatures();
            QgsFeature feature;
            while (iterator.nextFeature(feature))
            {
                if (regionLayer == &provinceLayer &&
                    (feature.attribute(fieldIndex).isNull() || feature.attribute(fieldIndex).toString() != fieldValue))
                    continue;
                QgsGeometry geometry = feature.geometry();
                if (regionLayer->crs() != layer->crs() && geometry.transform(transform) != Qgis::GeometryOperationResult::Success)
                    return fail("关联图层坐标转换失败");
                geometry = validArea(geometry);
                if (geometry.isEmpty() || !geometry.isGeosValid() || geometry.area() <= 0)
                    return fail(QString("关联图层存在无法修复的面：%1，FID=%2")
                                    .arg(regionLayer->source()).arg(feature.id()));
                if (regionLayer == &mainLayer)
                    mainParts.append(geometry);
                else
                    provinceParts.append(geometry);
            }
        }
    }
    catch (const QgsCsException &exception)
    {
        return fail(QString("关联图层坐标转换失败：%1").arg(exception.what()));
    }
    if (mainParts.isEmpty() || provinceParts.isEmpty())
        return fail("主区面为空或省内筛选条件未命中任何面");
    const QgsGeometry mainGeometry = merge(mainParts);
    const QgsGeometry provinceGeometry = merge(provinceParts);
    if (mainGeometry.isEmpty() || !mainGeometry.isGeosValid() ||
        provinceGeometry.isEmpty() || !provinceGeometry.isGeosValid())
        return fail("关联区域合并失败");

    QMap<QString, QVector<QgsGeometry>> groups;
    QMap<QString, QList<qint64>> members;
    const int groupIndex = layer->fields().lookupField("group_id");
    QgsFeatureIterator iterator = layer->getFeatures();
    QgsFeature feature;
    while (iterator.nextFeature(feature))
    {
        QString key = QString("feature:%1").arg(feature.id());
        if (groupIndex >= 0 && !feature.attribute(groupIndex).isNull())
        {
            bool ok = false;
            const qlonglong groupId = feature.attribute(groupIndex).toLongLong(&ok);
            if (!ok)
                return fail(QString("注记 FID=%1 的 group_id 无效").arg(feature.id()));
            if (groupId >= 0)
                key = QString("group:%1").arg(groupId);
        }
        const QgsGeometry geometry = validArea(feature.geometry());
        if (geometry.isEmpty() || !geometry.isGeosValid() || geometry.area() <= 0)
            return fail(QString("注记 FID=%1 缺少有效面几何").arg(feature.id()));
        groups[key].append(geometry);
        members[key].append(feature.id());
    }
    for (auto group = groups.constBegin(); group != groups.constEnd(); ++group)
    {
        const QgsGeometry geometry = merge(group.value());
        if (geometry.isEmpty() || !geometry.isGeosValid())
            return fail(QString("注记组 %1 合并失败").arg(group.key()));
        QgsGeometry remainder = geometry;
        double areas[3] = {0, 0, 0};
        const QgsGeometry regions[2] = {mainGeometry, provinceGeometry};
        for (int index = 0; index < 2; ++index)
        {
            if (remainder.isEmpty() || regions[index].isEmpty())
                continue;
            const QgsGeometry overlap = remainder.intersection(regions[index]);
            if (!overlap.lastError().isEmpty())
                return fail(QString("注记组 %1 与区域求交失败").arg(group.key()));
            areas[index] = overlap.area();
            remainder = remainder.difference(regions[index]);
            if (!remainder.lastError().isEmpty())
                return fail(QString("注记组 %1 与区域求差失败").arg(group.key()));
        }
        areas[2] = remainder.area();
        const double tolerance = geometry.area() * 1e-8;
        int best = 0;
        for (int index = 1; index < 3; ++index)
        {
            if (areas[index] > areas[best])
                best = index;
        }
        if (areas[best] <= tolerance)
            return fail(QString("注记组 %1 未落入有效区域").arg(group.key()));
        for (int index = 0; index < 3; ++index)
        {
            if (index != best && qAbs(areas[index] - areas[best]) <= tolerance)
                return fail(QString("注记组 %1 跨区域覆盖比例相同，无法可靠归类").arg(group.key()));
        }
        for (qint64 id : members.value(group.key()))
            m_featureRegions.insert(id, best);
    }
    m_regionCheckEnabled = true;
    return true;
}

QList<AnnotationCheckResult> SE_AnnotationAlgorithm::checkAnnotationLayer(QgsVectorLayer *layer)
{
    QList<AnnotationCheckResult> results;
    m_lastError.clear();

    if (!layer || !layer->isValid())
    {
        QgsMessageLog::logMessage("图层无效", "AnnotationCheck", Qgis::Critical);
        return results;
    }

    // 201 字体
    if (m_fontCheck && (hasCheckItemById(201) || isCheckRequired("字体")))
        results.append(checkFont(layer));
    if (!m_lastError.isEmpty())
        return {};

    // 202 字大
    if (m_fontSizeCheck && (hasCheckItemById(202) || isCheckRequired("字大")))
        results.append(checkFontSize(layer));
    if (!m_lastError.isEmpty())
        return {};

    // 203 颜色
    if (m_colorCheck && (hasCheckItemById(203) || isCheckRequired("字体颜色")))
        results.append(checkColor(layer));
    if (!m_lastError.isEmpty())
        return {};

    // 205 斜体
    if (m_italicCheck && (hasCheckItemById(205) || isCheckRequired("字体斜体")))
        results.append(checkItalic(layer));

    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAlgorithm::checkFont(QgsVectorLayer *layer)
{
    QList<AnnotationCheckResult> results;

    if (!hasCheckItemById(201) && !isCheckRequired("字体"))
        return results;

    QString expectedFont = getRuleValueById(201, "字体名");
    if (expectedFont.isEmpty())
        expectedFont = getRuleValue("字体"); // 兼容旧规则

    auto normalizeFontName = [](const QString &name) -> QString {
        QString normalized = name;
        static QRegularExpression encodingRe(R"([_\-\s]*(?:GB2312|GBK|GB18030|BIG5|UTF[\-\d]*))",
                                             QRegularExpression::CaseInsensitiveOption);
        normalized.remove(encodingRe);
        if (normalized.startsWith("方正"))
            normalized = normalized.mid(2);
        if (normalized.endsWith("简体") || normalized.endsWith("繁体"))
            normalized.chop(2);
        return normalized.trimmed();
    };

    if (!prepareRegionCheck(201, layer, {splitRegionRuleValues(expectedFont)}))
        return results;

    QStringList expectedFonts;
    for (const QString &font : splitRuleValues(expectedFont))
        expectedFonts.append(normalizeFontName(font));

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature))
    {
        // 获取字体字段值（完整名称：font_family，截取后：font_famil）
        QString actualFont;
        QString fontFieldName = getFieldName("font_family");
        int fieldIndex = layer->fields().indexFromName(fontFieldName);

        if (fieldIndex >= 0)
        {
            actualFont = feature.attribute(fieldIndex).toString();
        }

        if (actualFont.isEmpty())
        {
            continue;
        }

        // 处理字体名，去除 "--" 及 "_" 后缀（用于映射查找，如 FZFSJW_GB12345 → FZFSJW）
        QString fontForMapping = actualFont;
        int dashIndex = fontForMapping.indexOf("--");
        int underlineIndex = fontForMapping.indexOf("_");
        if (dashIndex != -1)
        {
            fontForMapping = fontForMapping.left(dashIndex);
        }
        else if (underlineIndex != -1)
        {
            fontForMapping = fontForMapping.left(underlineIndex);
        }

        // 转换字体名称（英文→中文映射）
        QString convertedFont = convertFontName(fontForMapping);

        QString normalizedConvertedFont = normalizeFontName(convertedFont);

        // 同时用原始名称做一遍归一化（防止映射后名称变了，但原始名也能匹配的情况）
        QString normalizedActualFont = normalizeFontName(actualFont);

        const int index = regionIndex(feature);
        const QStringList allowedFonts = index >= 0 && expectedFonts.size() == 3
            ? QStringList{expectedFonts.at(index)} : expectedFonts;
        bool fontMatch = allowedFonts.contains(normalizedConvertedFont)
                       || allowedFonts.contains(normalizedActualFont);

        if (!fontMatch)
        {
            AnnotationCheckResult result;
            result.featureId = QString::number(feature.id());
            result.errorType = AnnotationErrorType::FontError;
            result.errorMessage = index >= 0 ? QString("字体不符合规范（%1）").arg(regionName(feature)) : QString("字体不符合规范");
            // ACTUAL_VAL 保留原始解析的字体名称，便于排查
            result.actualValue = actualFont;
            const QStringList fontValues = splitRuleValues(expectedFont);
            result.expectedValue = index >= 0 && fontValues.size() == 3 ? fontValues.at(index) : expectedFont;
            result.layerName = layer->name();

            // 获取要素位置
            if (feature.hasGeometry())
            {
                QgsGeometry geom = feature.geometry();
                QgsPointXY centroid = geom.centroid().asPoint();
                result.position = QPointF(centroid.x(), centroid.y());
            }

            results.append(result);
        }
    }

    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAlgorithm::checkFontSize(QgsVectorLayer *layer)
{
    QList<AnnotationCheckResult> results;

    if (!hasCheckItemById(202) && !isCheckRequired("字大"))
        return results;

    // “字体大小”支持候选值，“最小字大”/“最大字大”支持按位置配对的范围
    QString expectedSizeStr = getRuleValueById(202, "字体大小");
    if (expectedSizeStr.isEmpty())
        expectedSizeStr = getRuleValue("字大", "字体大小");

    QString minSizeStr = getRuleValueById(202, "最小字大");
    if (minSizeStr.isEmpty())
        minSizeStr = getRuleValue("字大", "最小字大");

    QString maxSizeStr = getRuleValueById(202, "最大字大");
    if (maxSizeStr.isEmpty())
        maxSizeStr = getRuleValue("字大", "最大字大");

    QList<QStringList> regionValues;
    if (!minSizeStr.isEmpty() || !maxSizeStr.isEmpty())
    {
        if (!minSizeStr.isEmpty())
            regionValues.append(splitRegionRuleValues(minSizeStr));
        if (!maxSizeStr.isEmpty())
            regionValues.append(splitRegionRuleValues(maxSizeStr));
    }
    else
        regionValues.append(splitRegionRuleValues(expectedSizeStr));
    if (!prepareRegionCheck(202, layer, regionValues))
        return results;

    auto parseSizes = [this](const QString &value) {
        QList<QSizeF> sizes;
        for (const QString &part : splitRuleValues(value))
            sizes.append(parseFontSize(part));
        return sizes;
    };
    const QList<QSizeF> expectedSizes = parseSizes(expectedSizeStr);
    const QList<QSizeF> minSizes = parseSizes(minSizeStr);
    const QList<QSizeF> maxSizes = parseSizes(maxSizeStr);
    const QStringList minSizeValues = splitRuleValues(minSizeStr);
    const QStringList maxSizeValues = splitRuleValues(maxSizeStr);
    const bool hasRanges = !minSizes.isEmpty() || !maxSizes.isEmpty();
    const int rangeCount = qMax(minSizes.size(), maxSizes.size());

    if (minSizes.size() > 1 && maxSizes.size() > 1 && minSizes.size() != maxSizes.size())
    {
        QgsMessageLog::logMessage(QString("字号范围的最小值与最大值数量不匹配: %1 / %2")
                                      .arg(minSizeStr).arg(maxSizeStr),
                                  "AnnotationCheck", Qgis::Warning);
        return results;
    }

    // 单位（默认mm），优先从ID=202项读取；兼容单值或范围规则的 unit 字段
    QString unit = "mm";
    if (hasCheckItemById(202))
    {
        for (const CheckItem &item : m_activeConfig.items)
        {
            if (item.id == 202)
            {
                for (const CheckRule &rule : item.rules)
                {
                    if ((rule.name == "字体大小" || rule.name == "最小字大" || rule.name == "最大字大") && !rule.unit.isEmpty())
                    {
                        unit = rule.unit;
                        break;
                    }
                }
                break;
            }
        }
    }
    else
    {
        // 兼容旧逻辑（按名称查找）
        for (const CheckItem &item : m_activeConfig.items)
        {
            if (item.name == "字大")
            {
                for (const CheckRule &rule : item.rules)
                {
                    if ((rule.name == "字体大小" || rule.name == "最小字大" || rule.name == "最大字大") && !rule.unit.isEmpty())
                    {
                        unit = rule.unit;
                        break;
                    }
                }
                break;
            }
        }
    }

    auto hasInvalidSize = [](const QList<QSizeF> &sizes) {
        for (const QSizeF &size : sizes)
        {
            if (size.isEmpty())
                return true;
        }
        return false;
    };
    if ((expectedSizes.isEmpty() && !hasRanges) ||
        (hasRanges && (hasInvalidSize(minSizes) || hasInvalidSize(maxSizes))) ||
        (!hasRanges && hasInvalidSize(expectedSizes)))
    {
        if (m_regionCheckEnabled)
            m_lastError = "区域字号规则包含无法解析的值";
        QgsMessageLog::logMessage(QString("无法解析期望字号（候选值或范围）: %1 / %2 / %3")
                                      .arg(expectedSizeStr)
                                      .arg(minSizeStr)
                                      .arg(maxSizeStr),
                                  "AnnotationCheck", Qgis::Warning);
        return results;
    }

    if (m_regionCheckEnabled && hasRanges)
    {
        for (int i = 0; i < rangeCount; ++i)
        {
            const QSizeF minimum = minSizes.isEmpty() ? QSizeF() : minSizes.at(minSizes.size() == 1 ? 0 : i);
            const QSizeF maximum = maxSizes.isEmpty() ? QSizeF() : maxSizes.at(maxSizes.size() == 1 ? 0 : i);
            if (!minimum.isEmpty() && !maximum.isEmpty() &&
                (minimum.width() > maximum.width() || minimum.height() > maximum.height()))
            {
                m_lastError = "区域字号规则的最小字大大于最大字大";
                QgsMessageLog::logMessage(m_lastError, "AnnotationCheck", Qgis::Critical);
                return results;
            }
        }
    }

    QString expectedSizeDisplay;
    QStringList ranges;
    if (hasRanges)
    {
        for (int i = 0; i < rangeCount; ++i)
        {
            const QString minimum = minSizeValues.isEmpty() ? QString() : minSizeValues.at(minSizeValues.size() == 1 ? 0 : i);
            const QString maximum = maxSizeValues.isEmpty() ? QString() : maxSizeValues.at(maxSizeValues.size() == 1 ? 0 : i);
            if (!minimum.isEmpty() && !maximum.isEmpty())
                ranges.append(QString("%1-%2%3").arg(minimum).arg(maximum).arg(unit));
            else if (!minimum.isEmpty())
                ranges.append(QString(">=%1%2").arg(minimum).arg(unit));
            else
                ranges.append(QString("<=%1%2").arg(maximum).arg(unit));
        }
        expectedSizeDisplay = ranges.join(";");
    }
    else
    {
        expectedSizeDisplay = expectedSizeStr + unit;
    }

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    // 假设目标数据的字号字段为点数（pt），需转换为mm，1pt = 0.3528mm
    const double ptToMm = 0.3528;
    double tolerance = 0.05; // 容差，可根据实际需求调整

    while (iter.nextFeature(feature))
    {
        // 获取字号字段值（完整名称：font_size，截取后：font_size）
        QString actualSizeStr;
        QString sizeFieldName = getFieldName("font_size");
        int fieldIndex = layer->fields().indexFromName(sizeFieldName);

        if (fieldIndex >= 0)
        {
            actualSizeStr = feature.attribute(fieldIndex).toString();
        }

        if (actualSizeStr.isEmpty())
        {
            continue;
        }

        // 目标数据通常为单一数值（点数），如 12.76
        bool ok = false;
        double actualPt = actualSizeStr.toDouble(&ok);
        QSizeF actualSizeMm;
        if (ok)
        {
            double mmValue = actualPt * ptToMm;
            actualSizeMm = QSizeF(mmValue, mmValue);
        }
        else
        {
            // 若不是纯数字，尝试解析为宽高
            actualSizeMm = parseFontSize(actualSizeStr);
        }

        // 如果解析失败，跳过该要素
        if (actualSizeMm.isEmpty())
            continue;

        bool outOfSpec = true;

        const int index = regionIndex(feature);
        // 区域模式只比较对应档位；普通模式继续接受任一候选值。
        if (hasRanges)
        {
            for (int i = 0; i < rangeCount; ++i)
            {
                if (index >= 0 && rangeCount == 3 && i != index)
                    continue;
                const QSizeF minimum = minSizes.isEmpty() ? QSizeF() : minSizes.at(minSizes.size() == 1 ? 0 : i);
                const QSizeF maximum = maxSizes.isEmpty() ? QSizeF() : maxSizes.at(maxSizes.size() == 1 ? 0 : i);
                const bool aboveMinimum = minimum.isEmpty() ||
                    ((actualSizeMm.width() + tolerance) >= minimum.width() &&
                     (actualSizeMm.height() + tolerance) >= minimum.height());
                const bool belowMaximum = maximum.isEmpty() ||
                    ((actualSizeMm.width() - tolerance) <= maximum.width() &&
                     (actualSizeMm.height() - tolerance) <= maximum.height());
                if (aboveMinimum && belowMaximum)
                {
                    outOfSpec = false;
                    break;
                }
            }
        }
        else
        {
            for (int i = 0; i < expectedSizes.size(); ++i)
            {
                if (index >= 0 && expectedSizes.size() == 3 && i != index)
                    continue;
                if (compareFontSize(actualSizeMm, expectedSizes.at(i), tolerance))
                {
                    outOfSpec = false;
                    break;
                }
            }
        }

        if (outOfSpec)
        {
            AnnotationCheckResult result;
            result.featureId = QString::number(feature.id());
            result.errorType = AnnotationErrorType::FontSizeError;
            // 根据是否为范围选择不同的提示
            if (hasRanges)
                result.errorMessage = QString("字号不在规范范围");
            else
                result.errorMessage = QString("字号不符合规范");

            result.actualValue = QString("%1").arg(actualSizeStr.toDouble(), 0, 'f', 2) + " (" +
                                 QString("%1x%2").arg(actualSizeMm.width(), 0, 'f', 2).arg(actualSizeMm.height(), 0, 'f', 2) + unit + ")";

            result.expectedValue = expectedSizeDisplay;
            if (index >= 0)
            {
                result.errorMessage += QString("（%1）").arg(regionName(feature));
                if (hasRanges && rangeCount == 3)
                    result.expectedValue = ranges.at(index);
                else if (!hasRanges && expectedSizes.size() == 3)
                    result.expectedValue = splitRuleValues(expectedSizeStr).at(index) + unit;
            }

            result.layerName = layer->name();

            if (feature.hasGeometry())
            {
                QgsGeometry geom = feature.geometry();
                QgsPointXY centroid = geom.centroid().asPoint();
                result.position = QPointF(centroid.x(), centroid.y());
            }

            results.append(result);
        }
    }

    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAlgorithm::checkColor(QgsVectorLayer *layer)
{
    QList<AnnotationCheckResult> results;

    if (!hasCheckItemById(203) && !isCheckRequired("字体颜色"))
        return results;

    QString expectedColorStr = getRuleValueById(203, "颜色代码");
    if (expectedColorStr.isEmpty())
        expectedColorStr = getRuleValue("字体颜色", "颜色代码");

    if (!prepareRegionCheck(203, layer, {splitRegionRuleValues(expectedColorStr)}))
        return results;

    const QStringList expectedColors = splitRuleValues(expectedColorStr);
    bool validColors = !expectedColors.isEmpty();
    for (const QString &color : expectedColors)
    {
        if (!parseCMYKColor(color).isValid())
        {
            validColors = false;
            break;
        }
    }

    if (!validColors)
    {
        if (m_regionCheckEnabled)
            m_lastError = "区域颜色规则包含无法解析的值";
        QgsMessageLog::logMessage(QString("无法解析期望颜色: %1").arg(expectedColorStr),
                                  "AnnotationCheck", Qgis::Warning);
        return results;
    }

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    int tolerance = 5; // 颜色容差（CMYK百分比），可根据实际需求调整

    while (iter.nextFeature(feature))
    {
        // 获取颜色字段值（完整名称：font_color，截取后：font_color）
        QString actualColorStr;
        QString colorFieldName = getFieldName("font_color");
        int fieldIndex = layer->fields().indexFromName(colorFieldName);

        if (fieldIndex >= 0)
        {
            actualColorStr = feature.attribute(fieldIndex).toString();
        }

        if (actualColorStr.isEmpty())
        {
            continue;
        }

        // 质检数据为cmyk(0.00,0.00,0.00,70.00)或#1abc9c格式
        QString compareColorStr = actualColorStr;

        // 兼容hex格式：转换为cmyk格式统一比较
        if (actualColorStr.startsWith('#'))
        {
            compareColorStr = hexToCMYKString(actualColorStr);
            if (compareColorStr.isEmpty())
            {
                QgsMessageLog::logMessage(QString("无法解析hex颜色: %1").arg(actualColorStr),
                                          "AnnotationCheck", Qgis::Warning);
                continue;
            }
        }

        QColor actualColor = parseStandardCMYKColor(compareColorStr);

        if (!actualColor.isValid())
        {
            QgsMessageLog::logMessage(QString("无法解析实际颜色: %1").arg(actualColorStr),
                                      "AnnotationCheck", Qgis::Warning);
            continue;
        }

        const int index = regionIndex(feature);
        bool colorMatch = false;
        for (int i = 0; i < expectedColors.size(); ++i)
        {
            if (index >= 0 && expectedColors.size() == 3 && i != index)
                continue;
            if (compareCMYKColor(compareColorStr, expectedColors.at(i), tolerance))
            {
                colorMatch = true;
                break;
            }
        }
        if (!colorMatch)
        {
            AnnotationCheckResult result;
            result.featureId = QString::number(feature.id());
            result.errorType = AnnotationErrorType::ColorError;
            result.errorMessage = index >= 0 ? QString("颜色不符合规范（%1）").arg(regionName(feature)) : QString("颜色不符合规范");
            result.actualValue = actualColorStr;
            result.expectedValue = index >= 0 && expectedColors.size() == 3 ? expectedColors.at(index) : expectedColorStr;
            result.layerName = layer->name();

            if (feature.hasGeometry())
            {
                QgsGeometry geom = feature.geometry();
                QgsPointXY centroid = geom.centroid().asPoint();
                result.position = QPointF(centroid.x(), centroid.y());
            }

            results.append(result);
        }
    }

    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAlgorithm::checkItalic(QgsVectorLayer *layer)
{
    QList<AnnotationCheckResult> results;

    if (!hasCheckItemById(205) && !isCheckRequired("字体斜体"))
    {
        QgsMessageLog::logMessage("未找到斜体规则配置", "AnnotationCheck", Qgis::Warning);
        return results;
    }

    // 优先按ID=205获取规则
    QString expectedDirection = getRuleValueById(205, "倾斜方向");
    QString minAngleStr = getRuleValueById(205, "最小斜角");
    QString maxAngleStr = getRuleValueById(205, "最大斜角");

    // 回落到按名称
    if (expectedDirection.isEmpty())
        expectedDirection = getRuleValue("字体斜体", "倾斜方向");
    if (minAngleStr.isEmpty())
        minAngleStr = getRuleValue("字体斜体", "最小斜角");
    if (maxAngleStr.isEmpty())
        maxAngleStr = getRuleValue("字体斜体", "最大斜角");

    // 获取角度范围
    double minAngle = 5.0;  // 默认值
    double maxAngle = 30.0; // 默认值

    bool ok;
    if (!minAngleStr.isEmpty())
    {
        double parsed = minAngleStr.toDouble(&ok);
        if (ok)
            minAngle = parsed;
    }

    if (!maxAngleStr.isEmpty())
    {
        double parsed = maxAngleStr.toDouble(&ok);
        if (ok)
            maxAngle = parsed;
    }

    QgsFeatureIterator iter = layer->getFeatures();
    QgsFeature feature;

    while (iter.nextFeature(feature))
    {
        // 获取斜体角度字段值（完整名称：shear_angle，截取后：shear_angl）
        QString actualAngleStr;
        QString angleFieldName = getFieldName("shear_angle");
        int fieldIndex = layer->fields().indexFromName(angleFieldName);

        if (fieldIndex >= 0)
        {
            actualAngleStr = feature.attribute(fieldIndex).toString();
        }

        if (actualAngleStr.isEmpty())
        {
            continue;
        }

        double actualAngle = parseShearAngle(actualAngleStr);

        // 将"左斜"转换为"left"进行比较
        QString directionForComparison = expectedDirection;
        if (expectedDirection == "左斜")
            directionForComparison = "left";
        else if (expectedDirection == "右斜")
            directionForComparison = "right";

        if (!compareItalicAngle(actualAngle, directionForComparison, minAngle, maxAngle))
        {
            AnnotationCheckResult result;
            result.featureId = QString::number(feature.id());
            result.errorType = AnnotationErrorType::ItalicError;
            result.errorMessage = QString("斜体角度不符合规范");
            result.actualValue = QString("%1°").arg(actualAngle, 0, 'f', 3);
            result.expectedValue = QString("%1 (%2°-%3°)")
                                       .arg(expectedDirection)
                                       .arg(minAngle, 0, 'f', 1)
                                       .arg(maxAngle, 0, 'f', 1);
            result.layerName = layer->name();

            if (feature.hasGeometry())
            {
                QgsGeometry geom = feature.geometry();
                QgsPointXY centroid = geom.centroid().asPoint();
                result.position = QPointF(centroid.x(), centroid.y());
            }

            results.append(result);
        }
    }

    return results;
}

double SE_AnnotationAlgorithm::parseShearAngle(const QString &angleString)
{
    bool ok = false;
    double angle = angleString.toDouble(&ok);
    return ok ? angle : 0.0;
}

bool SE_AnnotationAlgorithm::compareItalicAngle(double actualAngle, const QString &expectedDirection,
                                                double minAngle, double maxAngle)
{
    double absAngle = qAbs(actualAngle);

    // 检查角度范围
    if (absAngle < minAngle || absAngle > maxAngle)
    {
        return false;
    }

    // 检查方向
    if (expectedDirection == "left")
    {
        // 左斜：负值（逆时针）
        return actualAngle < 0;
    }
    else if (expectedDirection == "right")
    {
        // 右斜：正值（顺时针）
        return actualAngle > 0;
    }

    return false;
}

QString SE_AnnotationAlgorithm::convertFontName(const QString &englishName)
{
    return SE_FontMapping::convertEnglishToChinese(englishName);
}

QSizeF SE_AnnotationAlgorithm::parseFontSize(const QString &sizeString)
{
    // 解析类似 "4.5*4.5" 或 "4.5" 的字符串
    QRegularExpression re(R"((\d+\.?\d*)\*?(\d+\.?\d*)?)");
    QRegularExpressionMatch match = re.match(sizeString);

    if (match.hasMatch())
    {
        double width = match.captured(1).toDouble();
        double height = match.captured(2).isEmpty() ? width : match.captured(2).toDouble();
        return QSizeF(width, height);
    }

    return QSizeF();
}

bool SE_AnnotationAlgorithm::compareFontSize(const QSizeF &actual, const QSizeF &expected, double tolerance)
{
    return (qAbs(actual.width() - expected.width()) <= tolerance) &&
           (qAbs(actual.height() - expected.height()) <= tolerance);
}

QColor SE_AnnotationAlgorithm::parseStandardCMYKColor(const QString &cmykString)
{
    // 解析类似 "cmyk(0.00,0.00,0.00,70.00)" 的CMYK颜色字符串
    QRegularExpression re(R"(cmyk\((\d+\.?\d*),(\d+\.?\d*),(\d+\.?\d*),(\d+\.?\d*)\))");
    QRegularExpressionMatch match = re.match(cmykString);

    if (match.hasMatch())
    {
        double c = match.captured(1).toDouble();
        double m = match.captured(2).toDouble();
        double y = match.captured(3).toDouble();
        double k = match.captured(4).toDouble();

        // 将CMYK转换为RGB（标准算法）
        double r = 255 * (1 - c / 100.0) * (1 - k / 100.0);
        double g = 255 * (1 - m / 100.0) * (1 - k / 100.0);
        double b = 255 * (1 - y / 100.0) * (1 - k / 100.0);

        return QColor(qRound(r), qRound(g), qRound(b));
    }

    return QColor();
}

QString SE_AnnotationAlgorithm::hexToCMYKString(const QString &hexColor)
{
    // 解析 #RGB, #RRGGBB, #RRGGBBAA 格式
    QString hex = hexColor.trimmed();
    if (hex.startsWith('#'))
        hex = hex.mid(1);

    if (hex.length() == 3)
    {
        // 短格式 #RGB -> #RRGGBB
        hex = QString("%1%1%2%2%3%3").arg(hex[0]).arg(hex[1]).arg(hex[2]);
    }
    else if (hex.length() == 4)
    {
        // 短格式带alpha #RGBA -> #RRGGBB (忽略alpha)
        hex = QString("%1%1%2%2%3%3").arg(hex[0]).arg(hex[1]).arg(hex[2]);
    }

    if (hex.length() < 6)
        return QString();

    bool ok;
    int r = hex.mid(0, 2).toInt(&ok, 16);
    if (!ok) return QString();
    int g = hex.mid(2, 2).toInt(&ok, 16);
    if (!ok) return QString();
    int b = hex.mid(4, 2).toInt(&ok, 16);
    if (!ok) return QString();

    // RGB 转 CMYK
    double rPrime = r / 255.0;
    double gPrime = g / 255.0;
    double bPrime = b / 255.0;

    double k = 1.0 - qMax(qMax(rPrime, gPrime), bPrime);
    if (qFuzzyCompare(k, 1.0))
    {
        // 纯黑色
        return QString("cmyk(0.00,0.00,0.00,100.00)");
    }

    double c = (1.0 - rPrime - k) / (1.0 - k) * 100.0;
    double m = (1.0 - gPrime - k) / (1.0 - k) * 100.0;
    double y = (1.0 - bPrime - k) / (1.0 - k) * 100.0;
    double kPct = k * 100.0;

    return QString("cmyk(%1,%2,%3,%4)")
        .arg(c, 0, 'f', 2)
        .arg(m, 0, 'f', 2)
        .arg(y, 0, 'f', 2)
        .arg(kPct, 0, 'f', 2);
}

bool SE_AnnotationAlgorithm::compareCMYKColor(const QString &actualCMYK, const QString &expectedCMYK, int tolerance)
{
    // 解析实际CMYK值 cmyk(0.00,0.00,0.00,70.00)
    QRegularExpression actualRe(R"(cmyk\((\d+\.?\d*),(\d+\.?\d*),(\d+\.?\d*),(\d+\.?\d*)\))");
    QRegularExpressionMatch actualMatch = actualRe.match(actualCMYK);

    if (!actualMatch.hasMatch())
    {
        return false;
    }

    double actualC = actualMatch.captured(1).toDouble();
    double actualM = actualMatch.captured(2).toDouble();
    double actualY = actualMatch.captured(3).toDouble();
    double actualK = actualMatch.captured(4).toDouble();

    // 解析期望CMYK值 M40Y100K55
    QRegularExpression expectedRe(R"((?:C(\d+))?(?:M(\d+))?(?:Y(\d+))?(?:K(\d+))?)");
    QRegularExpressionMatch expectedMatch = expectedRe.match(expectedCMYK);

    if (!expectedMatch.hasMatch())
    {
        return false;
    }

    double expectedC = expectedMatch.captured(1).isEmpty() ? 0 : expectedMatch.captured(1).toDouble();
    double expectedM = expectedMatch.captured(2).isEmpty() ? 0 : expectedMatch.captured(2).toDouble();
    double expectedY = expectedMatch.captured(3).isEmpty() ? 0 : expectedMatch.captured(3).toDouble();
    double expectedK = expectedMatch.captured(4).isEmpty() ? 0 : expectedMatch.captured(4).toDouble();

    // 比较各个分量
    return (qAbs(actualC - expectedC) <= tolerance) &&
           (qAbs(actualM - expectedM) <= tolerance) &&
           (qAbs(actualY - expectedY) <= tolerance) &&
           (qAbs(actualK - expectedK) <= tolerance);
}

QColor SE_AnnotationAlgorithm::parseCMYKColor(const QString &cmykString)
{
    // 解析类似 "M40Y100K55" 的CMYK颜色字符串
    QRegularExpression re(R"((?:C(\d+))?(?:M(\d+))?(?:Y(\d+))?(?:K(\d+))?)");
    QRegularExpressionMatch match = re.match(cmykString);

    if (match.hasMatch())
    {
        int c = match.captured(1).isEmpty() ? 0 : match.captured(1).toInt();
        int m = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt();
        int y = match.captured(3).isEmpty() ? 0 : match.captured(3).toInt();
        int k = match.captured(4).isEmpty() ? 0 : match.captured(4).toInt();

        // 将CMYK转换为RGB（标准算法）
        double r = 255 * (1 - c / 100.0) * (1 - k / 100.0);
        double g = 255 * (1 - m / 100.0) * (1 - k / 100.0);
        double b = 255 * (1 - y / 100.0) * (1 - k / 100.0);

        return QColor(qRound(r), qRound(g), qRound(b));
    }

    return QColor();
}