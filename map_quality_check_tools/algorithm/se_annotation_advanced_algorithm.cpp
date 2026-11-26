#include "se_annotation_advanced_algorithm.h"
#include <QFile>
#include <QDomDocument>
#include <QXmlStreamReader>
#include <QMap>
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsSpatialIndex.h>
#include <QgsPointXY.h>
#include <QgsRectangle.h>
#include <qgsunittypes.h>
#include <QtCore/qfileinfo.h>
#include <cmath>
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <QSet>

// 单位转换常量：点(point) 转 毫米(mm)
const double POINT_TO_MM = 25.4 / 72.0;

SE_AnnotationAdvancedAlgorithm::SE_AnnotationAdvancedAlgorithm(std::shared_ptr<spdlog::logger> logger)
    : m_logger(logger)
{
}

SE_AnnotationAdvancedAlgorithm::~SE_AnnotationAdvancedAlgorithm()
{
    // 析构函数中可以添加清理逻辑
}

bool SE_AnnotationAdvancedAlgorithm::parseCheckRules(
    const QString &schemaPath,
    double mapScale,
    const QString &checkItemId,
    QMap<QString, QString> &rules)
{
    rules.clear();

    QFile file(schemaPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        m_logger->error("无法打开规则文件: {}", schemaPath.toUtf8().constData());
        return false;
    }

    QDomDocument document;
    QString errorMessage;
    int errorLine = 0;
    int errorColumn = 0;
    if (!document.setContent(&file, false, &errorMessage, &errorLine, &errorColumn))
    {
        m_logger->error("规则文件解析失败: {} (行 {}, 列 {})",
                        errorMessage.toStdString(), errorLine, errorColumn);
        return false;
    }

    QDomNodeList scaleNodes = document.documentElement().elementsByTagName("scale");
    if (scaleNodes.isEmpty())
    {
        m_logger->error("规则文件中没有 scale 节点: {}", schemaPath.toStdString());
        return false;
    }

    QDomElement selectedScale;
    QDomElement emptyScale;
    QString currentScaleValueStr;
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElement = scaleNodes.at(i).toElement();
        QString scaleValue = scaleElement.firstChildElement("value").text().trimmed();
        if (scaleValue.isEmpty() && emptyScale.isNull())
            emptyScale = scaleElement;

        bool scaleOk = false;
        double configuredScale = scaleValue.toDouble(&scaleOk);
        if (mapScale > 0.0 && scaleOk && qFuzzyCompare(configuredScale + 1.0, mapScale + 1.0))
        {
            selectedScale = scaleElement;
            currentScaleValueStr = scaleValue;
            break;
        }
    }

    if (selectedScale.isNull())
        selectedScale = !emptyScale.isNull() ? emptyScale : scaleNodes.at(0).toElement();

    for (QDomElement checkItem = selectedScale.firstChildElement("check_item");
         !checkItem.isNull();
         checkItem = checkItem.nextSiblingElement("check_item"))
    {
        if (checkItem.firstChildElement("id").text().trimmed() != checkItemId)
            continue;

        for (QDomElement rule = checkItem.firstChildElement("rule");
             !rule.isNull();
             rule = rule.nextSiblingElement("rule"))
        {
            QString ruleName = rule.firstChildElement("name").text().trimmed();
            if (!ruleName.isEmpty())
                rules[ruleName] = rule.firstChildElement("value").text().trimmed();
        }
        break;
    }

    bool found = !rules.isEmpty();

    file.close();

    if (!found)
    {
        m_logger->warn("在规则文件中未找到匹配比例尺 {} 的规则，也未找到空比例尺的回退规则",
                       currentScaleValueStr.isEmpty() ? "空比例尺" : currentScaleValueStr.toStdString());
        return false;
    }

    m_logger->info("成功解析检查项 {} 的 {} 条规则", checkItemId.toStdString(), rules.size());
    return true;
}

bool SE_AnnotationAdvancedAlgorithm::loadOverlapRules(const QString &schemaPath, double mapScale)
{
    QMap<QString, QString> rules;
    if (!parseCheckRules(schemaPath, mapScale, "206", rules))
    {
        return false;
    }

    // 解析具体规则
    if (rules.contains("允许重叠"))
    {
        m_allowOverlap = (rules["允许重叠"].toLower() == "true");
    }

    double toleranceInMm = 0.0;
    if (rules.contains("重叠容差"))
    {
        toleranceInMm = rules["重叠容差"].toDouble();
    }

    // 如果规则配置为允许重叠，则直接返回
    if (m_allowOverlap)
    {
        m_logger->info("规则配置为允许重叠，跳过检查");
        return true;
    }

    // 将毫米单位的容差转换为地图单位
    if (mapScale > 0)
    {
        m_overlapTolerance = (toleranceInMm / 1000.0) * mapScale;
        m_logger->info("重叠检查规则加载成功。容差: {} mm → 实地{}米 → 地图单位{}",
                       toleranceInMm, m_overlapTolerance, m_overlapTolerance);
    }
    else
    {
        m_overlapTolerance = toleranceInMm;
        m_logger->warn("无比例尺信息，直接使用容差值: {} (假设为地图单位)", toleranceInMm);
    }

    // 保存比例尺用于后续转换
    m_mapScale = mapScale;

    return true;
}

bool SE_AnnotationAdvancedAlgorithm::loadSymbolAnnotationMatchRules(const QString &schemaPath, double mapScale,
                                                                    const QString &checkItemId)
{
    QMap<QString, QString> rules;
    if (!parseCheckRules(schemaPath, mapScale, checkItemId, rules))
    {
        m_logger->warn("未找到id={}的check_item，无法加载符号注记匹配规则", checkItemId.toStdString());
        return false;
    }

    // 解析具体规则
    if (rules.contains("允许无注记"))
    {
        m_allowNoAnnotation = (rules["允许无注记"].toLower() == "true");
    }

    if (rules.contains("允许无符号"))
    {
        m_allowNoSymbol = (rules["允许无符号"].toLower() == "true");
    }

    if (rules.contains("最小间距"))
    {
        m_minDistanceMm = rules["最小间距"].toDouble();
    }

    if (rules.contains("最大间距"))
    {
        m_maxDistanceMm = rules["最大间距"].toDouble();
    }

    // 保存比例尺用于后续转换
    m_mapScale = mapScale;

    m_logger->info("符号注记匹配规则加载成功：允许无注记={}, 允许无符号={}, 最小间距={}mm, 最大间距={}mm",
                   m_allowNoAnnotation, m_allowNoSymbol, m_minDistanceMm, m_maxDistanceMm);

    m_matchRulesLoaded = false;
    m_matchLevelAttr.clear();
    m_matchLevelValuesCsv.clear();
    m_matchLabelField.clear();

    QMap<QString, QString> matchRules;
    if (parseCheckRules(schemaPath, mapScale, "200", matchRules))
    {
        m_matchRulesLoaded = true;
        for (auto it = matchRules.constBegin(); it != matchRules.constEnd(); ++it)
        {
            const QString ruleName = it.key().trimmed();
            const QString ruleValue = it.value().trimmed();
            if (ruleName.contains(QStringLiteral("属性项")))
            {
                m_matchLevelAttr = ruleValue;
            }
            else if (ruleName.contains(QStringLiteral("属性值")))
            {
                m_matchLevelValuesCsv = ruleValue;
            }
            else if (ruleName.contains(QStringLiteral("标注字段")))
            {
                m_matchLabelField = ruleValue;
            }
        }

        m_logger->info("已加载注记匹配规则（ID=200）：分层字段={}, 分层值={}, 标注字段={}",
                       m_matchLevelAttr.toStdString(),
                       m_matchLevelValuesCsv.toStdString(),
                       m_matchLabelField.toStdString());
    }
    else
    {
        m_logger->warn("未加载到注记匹配规则（ID=200），线/面GDB缺失注记检查将被跳过");
    }

    return true;
}

void SE_AnnotationAdvancedAlgorithm::setMatchParams(bool allowNoAnnotation, bool allowNoSymbol,
                                                    double minDistanceMm, double maxDistanceMm)
{
    m_allowNoAnnotation = allowNoAnnotation;
    m_allowNoSymbol = allowNoSymbol;
    m_minDistanceMm = minDistanceMm;
    m_maxDistanceMm = maxDistanceMm;
    m_logger->info("手动覆盖匹配参数：允许无注记={}, 允许无符号={}, 最小间距={}mm, 最大间距={}mm",
                   m_allowNoAnnotation, m_allowNoSymbol, m_minDistanceMm, m_maxDistanceMm);
}

bool SE_AnnotationAdvancedAlgorithm::loadCharSpacingRules(const QString &schemaPath, double mapScale)
{
    QMap<QString, QString> rules;
    if (!parseCheckRules(schemaPath, mapScale, "204", rules))
    {
        return false;
    }

    // 兼容多种规则命名：优先使用 "字体最小间距"，回退到 "最小字距"
    if (rules.contains("字体最小间距"))
    {
        m_minCharSpacingMm = rules["字体最小间距"].toDouble();
    }
    else if (rules.contains("最小字距"))

    {
        m_minCharSpacingMm = rules["最小字距"].toDouble();
    }

    if (rules.contains("字体最大间距"))
    {
        m_maxCharSpacingMm = rules["字体最大间距"].toDouble();
    }
    else if (rules.contains("最大字距"))
    {
        m_maxCharSpacingMm = rules["最大字距"].toDouble();
    }

    m_checkCharEqualSpacing = false;
    m_charSpacingDeviation = 0.10;
    if (rules.contains("检查等间距"))
    {
        const QString enabled = rules["检查等间距"].trimmed().toLower();
        if (enabled != "true" && enabled != "false")
        {
            m_logger->error("检查等间距必须为 true 或 false");
            return false;
        }
        m_checkCharEqualSpacing = (enabled == "true");
    }
    if (rules.contains("等间距允许偏差"))
    {
        bool ok = false;
        const double deviation = rules["等间距允许偏差"].toDouble(&ok);
        if (!ok || !std::isfinite(deviation) || deviation < 0.0 || deviation >= 1.0)
        {
            m_logger->error("等间距允许偏差必须为 [0, 1) 内的小数比例，例如 0.10 表示10%");
            return false;
        }
        m_charSpacingDeviation = deviation;
    }

    m_mapScale = mapScale;

    m_logger->info("字距检查规则加载成功：最小字距={}mm, 最大字距={}mm, 检查线要素={}, 检查面要素={}, 检查等间距={}, 等间距允许偏差={}",
                   m_minCharSpacingMm, m_maxCharSpacingMm, m_checkLineFeatures, m_checkPolygonFeatures,
                   m_checkCharEqualSpacing, m_charSpacingDeviation);

    return true;
}

bool SE_AnnotationAdvancedAlgorithm::loadGroupSpacingRules(const QString &schemaPath, double mapScale)
{
    QMap<QString, QString> rules;
    if (!parseCheckRules(schemaPath, mapScale, "207", rules))
    {
        return false;
    }

    if (rules.contains("注记最小间距"))
    {
        m_minGroupSpacingMm = rules["注记最小间距"].toDouble();
    }

    if (rules.contains("注记最大间距"))
    {
        m_maxGroupSpacingMm = rules["注记最大间距"].toDouble();
    }

    m_mapScale = mapScale;

    m_logger->info("注记组距检查规则加载成功：最小组距={}mm, 最大组距={}mm",
                   m_minGroupSpacingMm, m_maxGroupSpacingMm);

    return true;
}

bool SE_AnnotationAdvancedAlgorithm::loadEqualSpacingRules(const QString &schemaPath, double mapScale)
{
    QMap<QString, QString> rules;
    if (!parseCheckRules(schemaPath, mapScale, "208", rules))
    {
        return false;
    }

    if (rules.contains("最小标注数量"))
    {
        m_minAnnotationCount = rules["最小标注数量"].toInt();
    }

    if (rules.contains("允许偏差"))
    {
        m_allowedDeviation = rules["允许偏差"].toDouble();
    }

    m_mapScale = mapScale;

    m_logger->info("面要素等间距标注检查规则加载成功：最小标注数量={}, 允许偏差={}",
                   m_minAnnotationCount, m_allowedDeviation);

    return true;
}

QgsGeometry *SE_AnnotationAdvancedAlgorithm::calculateEffectiveTextGeometry(
    QgsGeometry *originalGeometry,
    double fontSizePt,
    const QString &textContent)
{
    // --- 步骤 0: 输入验证 ---
    if (!originalGeometry || originalGeometry->isEmpty() || fontSizePt <= 0 || textContent.isEmpty() || m_mapScale <= 0)
    {
        return originalGeometry ? new QgsGeometry(*originalGeometry) : nullptr;
    }
    if (originalGeometry->type() != QgsWkbTypes::PolygonGeometry)
    {
        return new QgsGeometry(*originalGeometry);
    }

    // --- 【核心修复】: 计算原始几何的最小外接矩形 ---
    // 这一步将任何复杂的、多顶点的注记多边形，都统一成一个标准的、带旋转的5点矩形。
    // 这使得后续的角点提取和尺寸计算变得可靠和安全。
    QgsGeometry ombbGeometry = originalGeometry->orientedMinimumBoundingBox();

    if (ombbGeometry.isEmpty() || ombbGeometry.type() != QgsWkbTypes::PolygonGeometry)
    {
        // 如果计算失败，则安全地回退到使用原始几何
        m_logger->warn("  无法计算注记 '{}' 的最小外接矩形，将使用原始几何进行收缩。", textContent.toStdString());
        // 在这种情况下，我们不能依赖角点，但仍然可以尝试基于质心和包络盒进行一个不太精确的轴对齐收缩
        // (为了简化，这里我们直接返回原始几何的副本)
        return new QgsGeometry(*originalGeometry);
    }

    QgsPolygonXY exteriorRing = ombbGeometry.asPolygon();
    // 经过OMBB计算后，得到的几何应该是非常标准的
    if (exteriorRing.isEmpty() || exteriorRing.isEmpty() || exteriorRing[0].size() != 5)
    {
        m_logger->warn("  计算出的最小外接矩形不是一个标准的5点多边形，回退到原始几何。");
        return new QgsGeometry(*originalGeometry);
    }

    // --- 步骤 1: 提取标准化的OMBB矩形的关键信息 ---
    QgsPointXY p0 = exteriorRing[0][0];
    QgsPointXY p1 = exteriorRing[0][1];
    QgsPointXY p2 = exteriorRing[0][2];

    QgsVector v_width_orig(p1.x() - p0.x(), p1.y() - p0.y());
    QgsVector v_height_orig(p2.x() - p1.x(), p2.y() - p1.y());
    if (v_width_orig.length() < v_height_orig.length())
    {
        std::swap(v_width_orig, v_height_orig);
    }

    // 使用OMBB的几何中心，这是最准确的
    QgsPointXY center = ombbGeometry.centroid().asPoint();
    if (center.isEmpty())
    {
        center = QgsPointXY((p0.x() + p2.x()) / 2.0, (p0.y() + p2.y()) / 2.0);
    }

    // --- 步骤 2: 基于 font_size 和字数计算有效尺寸 (逻辑不变) ---
    const double charAspectRatio = 1.1;
    int charCount = textContent.length();
    double effectiveHeightPt = fontSizePt;
    double effectiveWidthPt_estimated = charCount * fontSizePt * charAspectRatio;
    double effectiveHeightMU_base = mmToMapUnits(effectiveHeightPt * POINT_TO_MM);
    double effectiveWidthMU_base = mmToMapUnits(effectiveWidthPt_estimated * POINT_TO_MM);
    double finalEffectiveWidthMU = effectiveWidthMU_base * m_textShrinkFactor;
    double finalEffectiveHeightMU = effectiveHeightMU_base * m_textShrinkFactor;

    // --- 步骤 3 & 4: 重构和创建新几何 (逻辑不变) ---
    QgsVector u_width = v_width_orig.normalized();
    QgsVector u_height = v_height_orig.normalized();

    auto move = [](const QgsPointXY &pt, const QgsVector &v)
    {
        return QgsPointXY(pt.x() + v.x(), pt.y() + v.y());
    };

    QgsVector width_half(u_width.x() * (finalEffectiveWidthMU / 2.0), u_width.y() * (finalEffectiveWidthMU / 2.0));
    QgsVector height_half(u_height.x() * (finalEffectiveHeightMU / 2.0), u_height.y() * (finalEffectiveHeightMU / 2.0));
    QgsVector neg_width_half(-width_half.x(), -width_half.y());
    QgsVector neg_height_half(-height_half.x(), -height_half.y());

    QgsPointXY new_p0 = move(move(center, neg_width_half), neg_height_half);
    QgsPointXY new_p1 = move(move(center, width_half), neg_height_half);
    QgsPointXY new_p2 = move(move(center, width_half), height_half);
    QgsPointXY new_p3 = move(move(center, neg_width_half), height_half);

    QgsPolylineXY newPoints;
    newPoints << new_p0 << new_p1 << new_p2 << new_p3 << new_p0;
    QgsPolygonXY newRing;
    newRing << newPoints;

    return new QgsGeometry(QgsGeometry::fromPolygonXY(newRing));
}

void SE_AnnotationAdvancedAlgorithm::loadAndPreprocessFeatures(
    const QStringList &layerPaths,
    const QString &elementType,
    QList<ElementInfo> &allElements,
    QList<QgsVectorLayer *> &loadedLayers)
{
    for (const QString &path : layerPaths)
    {
        const QString layerName = QFileInfo(path).baseName();
        if (layerName.contains(QStringLiteral("图例")))
        {
            m_logger->info("跳过图例图层: {}", path.toStdString());
            continue;
        }

        QgsVectorLayer *layer = new QgsVectorLayer(path, layerName, "ogr");
        if (!layer || !layer->isValid())
        {
            m_logger->warn("无法加载图层: {}", path.toUtf8().constData());
            delete layer;
            continue;
        }
        loadedLayers.append(layer);

        // 预处理符号要素：按 symbol_id 分组并计算外包框
        if (elementType == "symbol")
        {
            QMap<QVariant, QList<QgsFeature>> symbolGroups;
            int symbolIdIndex = layer->fields().indexOf("symbol_id");
            if (symbolIdIndex == -1)
            {
                m_logger->warn("图层 {} 缺少 symbol_id 字段，跳过符号处理", path.toStdString());
                continue;
            }

            QgsFeatureIterator iterator = layer->getFeatures();
            QgsFeature f;
            while (iterator.nextFeature(f))
            {
                symbolGroups[f.attribute(symbolIdIndex)].append(f);
            }

            for (auto it = symbolGroups.constBegin(); it != symbolGroups.constEnd(); ++it)
            {
                QgsRectangle totalBounds;
                bool first = true;

                ElementInfo info;
                info.id = allElements.size();
                info.layerName = layer->name();
                info.elementType = "symbol";
                info.originalFeatureId = it.key(); // symbol_id

                for (const QgsFeature &part : it.value())
                {
                    QgsRectangle bbox = part.geometry().boundingBox();
                    if (first)
                    {
                        totalBounds = bbox;
                        first = false;
                    }
                    else
                    {
                        totalBounds.setXMinimum(qMin(totalBounds.xMinimum(), bbox.xMinimum()));
                        totalBounds.setYMinimum(qMin(totalBounds.yMinimum(), bbox.yMinimum()));
                        totalBounds.setXMaximum(qMax(totalBounds.xMaximum(), bbox.xMaximum()));
                        totalBounds.setYMaximum(qMax(totalBounds.yMaximum(), bbox.yMaximum()));
                    }
                    info.componentFeatureIds.append(part.id()); // 记录组成要素ID
                }

                info.geometry = new QgsGeometry(QgsGeometry::fromRect(totalBounds));
                allElements.append(info);
            }
        }
        // 处理注记要素：提取文本尺寸信息并计算有效几何
        else if (elementType == "text")
        {
            // 获取必要的字段索引
            int textContentIdx = layer->fields().indexOf("text_conte"); // 获取 "text_conte" 字段
            int textWidthIdx = layer->fields().indexOf("text_width");
            int textHeightIdx = layer->fields().indexOf("text_heigh");
            int fontSizeIdx = layer->fields().indexOf("font_size");
            int elementTyIdx = layer->fields().indexOf("element_ty"); // 新增

            QgsFeatureIterator featureIterator = layer->getFeatures();
            QgsFeature feature;

            while (featureIterator.nextFeature(feature))
            {
                // 新增：排除 element_ty 不为 text 的面要素
                if (elementTyIdx >= 0)
                {
                    QVariant ety = feature.attribute(elementTyIdx);
                    if (feature.geometry().type() == QgsWkbTypes::PolygonGeometry && ety.isValid() && ety.toString().toLower() != "text")
                    {
                        continue;
                    }
                }

                ElementInfo info;
                info.id = allElements.size();
                info.layerName = layer->name();
                info.geometry = new QgsGeometry(feature.geometry());
                info.elementType = "text";
                info.originalFeatureId = feature.id();
                info.textWidth = textWidthIdx >= 0 ? feature.attribute(textWidthIdx).toDouble() : 0.0;
                info.textHeight = textHeightIdx >= 0 ? feature.attribute(textHeightIdx).toDouble() : 0.0;
                info.fontSize = fontSizeIdx >= 0 ? feature.attribute(fontSizeIdx).toDouble() : 0.0;
                if (textContentIdx >= 0)
                {
                    info.textContent = feature.attribute(textContentIdx).toString(); // 填充文本内容
                }
                else
                {
                    info.textContent = " "; // 避免空字符串
                }
                info.effectiveGeometry = calculateEffectiveTextGeometry(
                    info.geometry,
                    info.fontSize,
                    info.textContent);

                allElements.append(info);
            }
        }
    }
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkOverlap(
    const QStringList &symbolLayerPaths,
    const QStringList &annotationLayerPaths,
    QList<ElementInfo> &allElements,
    QList<QgsVectorLayer *> &loadedLayers)
{
    QList<AnnotationCheckResult> results;
    if (m_allowOverlap)
    {
        return results; // 规则允许重叠，不执行检查
    }

    // 1. 加载并预处理所有要素
    m_logger->info("加载并预处理符号要素...");
    loadAndPreprocessFeatures(symbolLayerPaths, "symbol", allElements, loadedLayers);
    m_logger->info("加载并预处理注记要素...");
    loadAndPreprocessFeatures(annotationLayerPaths, "text", allElements, loadedLayers);
    m_logger->info("总共加载 {} 个待查元素（符号+注记）。", allElements.size());

    if (allElements.size() < 2)
    {
        m_logger->info("待查元素不足2个，无需检查。");
        return results;
    }

    // 2. 构建空间索引（基于原始几何，用于粗筛选）
    m_logger->info("构建空间索引...");
    QgsSpatialIndex spatialIndex;
    for (const auto &element : allElements)
    {
        spatialIndex.addFeature(element.id, element.geometry->boundingBox());
    }
    m_logger->info("空间索引构建完成。");

    // 3. 遍历每个要素，查找重叠
    m_logger->info("开始遍历要素并查找重叠...");

    // 计算最小重叠面积阈值（基于容差）
    double minOverlapArea = m_overlapTolerance * m_overlapTolerance;

    for (int i = 0; i < allElements.size(); ++i)
    {
        const auto &currentElement = allElements[i];

        // 使用原始几何进行粗筛选（保持搜索范围）
        QgsRectangle searchRect = currentElement.geometry->boundingBox();
        searchRect.grow(m_overlapTolerance);

        // 使用空间索引查找候选者
        QList<qint64> candidateIds = spatialIndex.intersects(searchRect);

        for (qint64 candidateId : candidateIds)
        {
            // 避免与自身比较，以及避免重复比较
            if (candidateId <= currentElement.id)
            {
                continue;
            }

            const auto &candidateElement = allElements[candidateId];

            // 精确几何计算：使用有效几何进行判断
            // 对于文本要素，使用收缩后的effectiveGeometry
            // 对于符号要素，使用原始geometry
            QgsGeometry *geom1 = currentElement.effectiveGeometry ? currentElement.effectiveGeometry : currentElement.geometry;
            QgsGeometry *geom2 = candidateElement.effectiveGeometry ? candidateElement.effectiveGeometry : candidateElement.geometry;

            // 判断是否相交
            if (geom1->intersects(*geom2))
            {
                // 计算重叠面积
                QgsGeometry intersection = geom1->intersection(*geom2);
                double overlapArea = intersection.area();

                // 只有重叠面积超过阈值才报错
                if (overlapArea > minOverlapArea)
                {
                    // 记录错误
                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::OverlapError;
                    result.featureId = QString::number(currentElement.id);
                    result.actualValue = QString::number(candidateElement.id);
                    result.errorMessage = QString("元素重叠: %1 '%2' 与 %3 '%4' 重叠 (面积: %5)")
                                              .arg(currentElement.elementType)
                                              .arg(currentElement.originalFeatureId.toString())
                                              .arg(candidateElement.elementType)
                                              .arg(candidateElement.originalFeatureId.toString())
                                              .arg(overlapArea, 0, 'f', 6);
                    result.layerName = QString("%1, %2")
                                           .arg(currentElement.layerName)
                                           .arg(candidateElement.layerName);

                    // 使用重叠区域的中心点作为错误位置
                    QgsPointXY center = intersection.centroid().asPoint();
                    result.position = QPointF(center.x(), center.y());

                    results.append(result);
                }
            }
        }
    }

    m_logger->info("查找重叠完成，发现 {} 处有效重叠。", results.size());
    return results;
}

QgsPointXY SE_AnnotationAdvancedAlgorithm::calculateSymbolCenter(QgsVectorLayer *actualLayer, const QString &symbolId)
{
    if (!actualLayer || symbolId.isEmpty())
        return QgsPointXY();

    int symbolIdIdx = actualLayer->fields().indexOf("symbol_id");
    if (symbolIdIdx < 0)
        return QgsPointXY();

    double sumX = 0.0, sumY = 0.0;
    int count = 0;

    QgsFeatureIterator it = actualLayer->getFeatures();
    QgsFeature f;
    while (it.nextFeature(f))
    {
        if (f.attribute(symbolIdIdx).toString() == symbolId)
        {
            QgsPointXY center = f.geometry().centroid().asPoint();
            sumX += center.x();
            sumY += center.y();
            count++;
        }
    }

    if (count > 0)
        return QgsPointXY(sumX / count, sumY / count);

    return QgsPointXY();
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkSymbolAnnotationMatch(
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *actualLayer,
    QgsVectorLayer *gdbLayer,
    QString *errorMessage,
    QgsVectorLayer *annoPolygonLayer)
{
    QList<AnnotationCheckResult> results;

    if (errorMessage)
        errorMessage->clear();

    if (!matchResultLayer || !matchResultLayer->isValid())
    {
        m_logger->error("匹配结果图层无效");
        return results;
    }

    // 【修改】如果 actualLayer 为空，记录警告并继续执行不依赖它的检查
    if (!actualLayer || !actualLayer->isValid())
    {
        m_logger->warn("实际图层无效或未提供，将跳过点符号相关检查（如符号缺失注记、注记缺失符号、间距等）");
    }

    m_logger->info("开始执行符号注记匹配检查（ID=210）");

    // 获取字段索引
    auto fieldIndex = [&](std::initializer_list<const char *> names) -> int
    {
        for (const char *name : names)
        {
            int idx = matchResultLayer->fields().indexOf(name);
            if (idx >= 0)
                return idx;
        }
        return -1;
    };

    int matchStatIdx = fieldIndex({"match_stat", "match_status"});
    int matchScorIdx = fieldIndex({"match_scor", "match_score"});
    int symbolIdIdx = matchResultLayer->fields().indexOf("symbol_id");
    int actualFidIdx = fieldIndex({"actual_fid", "actual_fids"});
    // 兼容原单字段结果，按数值顺序读回200生成的全部编号续段。
    QMap<int, int> actualContinuationFields;
    const QgsFields &matchFields = matchResultLayer->fields();
    for (int index = 0; index < matchFields.count(); ++index) {
        const QString name = matchFields.at(index).name().toLower();
        if (!name.startsWith("act_fid")) continue;
        bool ok = false;
        const int part = name.mid(7).toInt(&ok);
        if (ok && part >= 2) actualContinuationFields.insert(part, index);
    }
    QVector<int> actualFidIndices;
    if (actualFidIdx >= 0) actualFidIndices.append(actualFidIdx);
    for (auto index = actualContinuationFields.constBegin(); index != actualContinuationFields.constEnd(); ++index)
        actualFidIndices.append(index.value());
    int annoFidIdx = matchResultLayer->fields().indexOf("anno_fid");
    int textConteIdx = matchResultLayer->fields().indexOf("text_conte");
    int assocNameIdx = matchResultLayer->fields().indexOf("assoc_name");
    int gdbFidIdx = matchResultLayer->fields().indexOf("gdb_fid");
    int gdbFidsIdx = matchResultLayer->fields().indexOf("gdb_fids");

    if (matchStatIdx < 0 || symbolIdIdx < 0 || actualFidIdx < 0 || annoFidIdx < 0)
    {
        if (errorMessage)
            *errorMessage = QStringLiteral("匹配结果图层缺少必要字段（match_status、symbol_id、actual_fids 或 anno_fid）");
        return results;
    }

    // 从actual图层构建符号中心点映射和符号ID集合
    m_logger->info("构建符号中心点映射...");
    QMap<QString, QgsPointXY> symbolCenters;
    QSet<QString> existingSymbolIds; // 实际图层中存在的所有符号ID
    bool actualLayerIsPointSymbol = false;

    if (actualLayer && actualLayer->isValid())
    {
        int actualSymbolIdIdx = actualLayer->fields().indexOf("symbol_id");
        if (actualSymbolIdIdx < 0)
        {
            m_logger->info("实际图层不包含 symbol_id 字段，判定为非点符号图层，跳过点符号相关检查");
        }
        else
        {
            // 按 symbol_id 聚合整个符号范围，输出点位使用符号整体中心，而不是组件线的逐个中心。
            QMap<QString, QgsRectangle> symbolBounds;
            QgsFeatureIterator actualIt = actualLayer->getFeatures();
            QgsFeature actualFeature;
            while (actualIt.nextFeature(actualFeature))
            {
                QString symbolId = actualFeature.attribute(actualSymbolIdIdx).toString();
                if (!symbolId.isEmpty() && actualFeature.hasGeometry())
                {
                    existingSymbolIds.insert(symbolId);
                    QgsRectangle bbox = actualFeature.geometry().boundingBox();
                    if (symbolBounds.contains(symbolId))
                    {
                        symbolBounds[symbolId].combineExtentWith(bbox);
                    }
                    else
                    {
                        symbolBounds.insert(symbolId, bbox);
                    }
                }
            }

            actualLayerIsPointSymbol = !existingSymbolIds.isEmpty();
            if (!actualLayerIsPointSymbol)
            {
                m_logger->info("实际图层包含 symbol_id 字段但没有有效符号ID，跳过点符号相关检查");
            }

            // 计算每个符号的整体中心点
            for (auto it = symbolBounds.constBegin(); it != symbolBounds.constEnd(); ++it)
            {
                symbolCenters[it.key()] = it.value().center();
            }
            m_logger->info("实际图层点符号识别结果：{}，符号数量={}", actualLayerIsPointSymbol, existingSymbolIds.size());
        }
    }
    else
    {
        m_logger->info("未提供有效实际图层，跳过符号中心点构建。");
    }

    // 构建 symbol_id → GDB要素名称 映射（通过空间查询，用于"符号缺失注记"展示真实名称而非cached_symbol_*）
    // 核心思路：实际符号图层没有GRADE字段，无法直接过滤。因此先在GDB侧按XML配置的等级
    // （如3A）过滤，只把目标等级的GDB要素加入空间索引。符号只有命中这些3A GDB要素，
    // 才被认定为目标等级符号，参与后续"符号缺失注记"检查。
    QMap<QString, QString> symbolIdToGdbName;
    QMap<QString, QString> symbolIdToGdbLevel; // 同时记录GDB等级，用于日志/调试
    if (actualLayerIsPointSymbol && gdbLayer && gdbLayer->isValid() && !m_matchLabelField.isEmpty())
    {
        int gdbNameIdx2 = gdbLayer->fields().indexOf(m_matchLabelField);
        int gdbLevelIdx2 = m_matchLevelAttr.isEmpty() ? -1 : gdbLayer->fields().indexOf(m_matchLevelAttr);
        if (gdbNameIdx2 >= 0)
        {
            // 先解析XML配置的等级过滤（如3A），只索引目标等级的GDB要素
            QSet<QString> spatialFilterLevels;
            if (gdbLevelIdx2 >= 0 && !m_matchLevelValuesCsv.trimmed().isEmpty())
            {
                QString norm = m_matchLevelValuesCsv;
                norm.replace(QChar(u'，'), ',');
                norm.replace(QChar(u'；'), ';');
                norm.replace(';', ',');
                for (const QString &part : norm.split(',', Qt::SkipEmptyParts))
                {
                    QString v = part;
                    v.replace(" ", "");
                    v = v.trimmed().toLower();
                    if (!v.isEmpty())
                        spatialFilterLevels.insert(v);
                }
            }

            // 缓存GDB要素，建立空间索引（仅目标等级的GDB要素参与）
            QgsSpatialIndex gdbSpatialIndex;
            QMap<qint64, QgsFeature> gdbFeatureCache;
            QgsFeatureIterator gdbIt2 = gdbLayer->getFeatures();
            QgsFeature gdbFeat2;
            while (gdbIt2.nextFeature(gdbFeat2))
            {
                if (!gdbFeat2.hasGeometry())
                    continue;

                // GRADE过滤：只把目标等级的GDB要素加入空间索引
                if (!spatialFilterLevels.isEmpty() && gdbLevelIdx2 >= 0)
                {
                    QString lv = gdbFeat2.attribute(gdbLevelIdx2).toString();
                    lv.replace(" ", "");
                    lv = lv.trimmed().toLower();
                    bool lvMatched = false;
                    for (const QString &f : spatialFilterLevels)
                    {
                        if ((f.endsWith('*') && lv.startsWith(f.left(f.length() - 1))) || lv == f)
                        {
                            lvMatched = true;
                            break;
                        }
                    }
                    if (!lvMatched)
                        continue; // 非目标等级（如4A），不加入空间索引
                }

                gdbSpatialIndex.addFeature(gdbFeat2);
                gdbFeatureCache[gdbFeat2.id()] = gdbFeat2;
            }

            for (auto itSym = symbolCenters.constBegin(); itSym != symbolCenters.constEnd(); ++itSym)
            {
                QString symId = itSym.key();
                QgsPointXY center = itSym.value();

                // 用符号中心点的小范围矩形做空间相交查询
                double eps = 1.0; // 1米容差
                QgsRectangle queryRect(center.x() - eps, center.y() - eps,
                                       center.x() + eps, center.y() + eps);
                QList<qint64> nearbyIds = gdbSpatialIndex.intersects(queryRect);

                // 分组：点要素和面要素分开处理，优先匹配点要素
                QgsGeometry centerGeom = QgsGeometry::fromPointXY(center);
                struct Candidate
                {
                    qint64 fid;
                    double area;     // 面积（面要素），点要素为0
                    double distance; // 距离（点要素），面要素为0
                };
                QList<Candidate> pointCandidates, polygonCandidates;

                for (qint64 nid : nearbyIds)
                {
                    QgsFeature nFeat = gdbFeatureCache.value(nid);
                    if (!nFeat.isValid() || !nFeat.hasGeometry())
                        continue;
                    QString gdbNm = nFeat.attribute(gdbNameIdx2).toString().trimmed();
                    if (gdbNm.isEmpty())
                        continue;

                    QgsWkbTypes::GeometryType geomType = nFeat.geometry().type();
                    if (geomType == QgsWkbTypes::PointGeometry)
                    {
                        // 点要素：记录到符号中心点的距离
                        double dist = nFeat.geometry().asPoint().distance(center);
                        pointCandidates.append({nid, 0.0, dist});
                    }
                    else if (geomType == QgsWkbTypes::PolygonGeometry)
                    {
                        // 面要素：检查符号中心点是否在面内，记录面积
                        if (nFeat.geometry().contains(centerGeom))
                        {
                            double area = nFeat.geometry().area();
                            polygonCandidates.append({nid, area, 0.0});
                        }
                    }
                }

                // 优先级：点要素（最近距离） > 面要素（最小面积）
                Candidate best{-1, 0.0, 0.0};
                bool found = false;
                if (!pointCandidates.isEmpty())
                {
                    std::sort(pointCandidates.begin(), pointCandidates.end(),
                              [](const Candidate &a, const Candidate &b)
                              { return a.distance < b.distance; });
                    best = pointCandidates.first();
                    found = true;
                }
                else if (!polygonCandidates.isEmpty())
                {
                    std::sort(polygonCandidates.begin(), polygonCandidates.end(),
                              [](const Candidate &a, const Candidate &b)
                              { return a.area < b.area; });
                    best = polygonCandidates.first();
                    found = true;
                }

                if (found)
                {
                    QgsFeature bestFeat = gdbFeatureCache.value(best.fid);
                    QString gdbNm = bestFeat.attribute(gdbNameIdx2).toString().trimmed();
                    symbolIdToGdbName[symId] = gdbNm;
                    if (gdbLevelIdx2 >= 0)
                        symbolIdToGdbLevel[symId] = bestFeat.attribute(gdbLevelIdx2).toString().trimmed();
                }
            }
            m_logger->info("symbol_id→GDB名称映射完成：{}/{}个符号找到对应的GDB名称",
                           symbolIdToGdbName.size(), symbolCenters.size());
        }
    }

    // 构建 GDB 名称→几何类型缓存（用于 UNMATCHED 结果标记面/线要素）
    QMap<QString, int> gdbNameGeomTypeCache;
    if (gdbLayer && gdbLayer->isValid() && !m_matchLabelField.isEmpty())
    {
        int gdbLabelIdx = gdbLayer->fields().indexOf(m_matchLabelField);
        if (gdbLabelIdx >= 0)
        {
            QgsFeatureIterator cacheIt = gdbLayer->getFeatures();
            QgsFeature cacheFeat;
            while (cacheIt.nextFeature(cacheFeat))
            {
                if (!cacheFeat.isValid() || !cacheFeat.hasGeometry())
                    continue;
                QString label = cacheFeat.attribute(gdbLabelIdx).toString().trimmed();
                if (label.isEmpty())
                    continue;
                int geomType = cacheFeat.geometry().type();
                if (!gdbNameGeomTypeCache.contains(label))
                    gdbNameGeomTypeCache[label] = geomType;
            }
        }
    }

    // 收集匹配结果中的信息
    QSet<QString> symbolsInMatchResult; // 匹配结果中出现的所有符号ID（已有注记配对）
    QSet<qint64> annosInMatchResult;    // 匹配结果中出现的所有注记FID
    QMap<qint64, QString> annoToSymbol; // anno_fid -> symbol_id
    QMap<qint64, QString> annoToText;
    QMap<qint64, QString> annoToAssocName;
    QMap<qint64, QString> annoToMatchStat;
    QMap<qint64, QString> annoToMatchScor;
    QMap<qint64, QgsPointXY> annoToPosition;
    QSet<qint64> matchedGdbFids;

    // 【新增】统计未匹配注记数量
    int unmatchedAnnoCount = 0;

    QgsFeatureIterator it = matchResultLayer->getFeatures();
    QgsFeature feature;

    // 第一遍遍历：收集所有信息
    while (it.nextFeature(feature))
    {
        QString symbolId = feature.attribute(symbolIdIdx).toString();
        QStringList componentParts;
        for (int index : actualFidIndices) {
            const QString part = feature.attribute(index).toString().trimmed();
            if (!part.isEmpty()) componentParts.append(part);
        }
        QString actualFidStr = componentParts.join(',');
        QVariant annoFidVariant = feature.attribute(annoFidIdx);
        bool annoFidOk = !annoFidVariant.isNull();
        qint64 annoFid = annoFidOk ? annoFidVariant.toLongLong(&annoFidOk) : -1;
        QString textConte = textConteIdx >= 0 ? feature.attribute(textConteIdx).toString() : "";
        QString assocName = assocNameIdx >= 0 ? feature.attribute(assocNameIdx).toString() : "";
        QString matchStat = feature.attribute(matchStatIdx).toString();
        QString matchScor = matchScorIdx >= 0 ? feature.attribute(matchScorIdx).toString() : "";
        QgsPointXY annoPos = feature.geometry().centroid().asPoint();

        {
            if (gdbFidsIdx >= 0)
            {
                QStringList gdbFidParts = feature.attribute(gdbFidsIdx).toString().split(',', Qt::SkipEmptyParts);
                for (const QString &part : gdbFidParts)
                {
                    bool ok = false;
                    qint64 fid = part.trimmed().toLongLong(&ok);
                    if (ok && fid >= 0)
                        matchedGdbFids.insert(fid);
                }
            }

            if (gdbFidIdx >= 0)
            {
                bool ok = false;
                qint64 fid = feature.attribute(gdbFidIdx).toLongLong(&ok);
                if (ok && fid >= 0)
                    matchedGdbFids.insert(fid);
            }
        }

        if (annoFidOk && annoFid >= 0)
        {
            annosInMatchResult.insert(annoFid);
            annoToText[annoFid] = textConte;
            annoToAssocName[annoFid] = assocName;
            annoToMatchStat[annoFid] = matchStat;
            annoToMatchScor[annoFid] = matchScor;
            annoToPosition[annoFid] = annoPos;

            // 【新增】统计未匹配注记
            if (matchStat == "UNMATCHED")
            {
                unmatchedAnnoCount++;
            }

            // 记录注记对应的符号（只有当symbol_id和actual_fid都有效时才算有符号）
            if (!symbolId.isEmpty() && !actualFidStr.isEmpty())
            {
                annoToSymbol[annoFid] = symbolId;
                symbolsInMatchResult.insert(symbolId); // 这个符号已有注记配对
            }
        }
    }

    m_logger->info("匹配结果统计：");
    m_logger->info("  匹配结果中的注记总数: {}", annosInMatchResult.size());
    m_logger->info("  匹配结果中有注记配对的符号数: {}", symbolsInMatchResult.size());
    m_logger->info("  有符号配对的注记数: {}", annoToSymbol.size());
    m_logger->info("  未匹配的注记数: {}", unmatchedAnnoCount);
    m_logger->info("  已匹配GDB要素数: {}", matchedGdbFids.size());

    // 转换间距阈值到地图单位
    double minDistanceMapUnits = mmToMapUnits(m_minDistanceMm);
    double maxDistanceMapUnits = mmToMapUnits(m_maxDistanceMm);

    // 开始检查
    QSet<qint64> checkedAnnos;
    int skippedUnmatchedOrphanCount = 0; // 被过滤的空间兜底无文本关联UNMATCHED注记数

    // ===== 面要素UNMATCHED+PARTIALLY_MATCHED预处理：注记全部在面内 → 面内路径匹配 =====
    // 对于面要素的UNMATCHED和PARTIALLY_MATCHED注记，先判断注记字是否全部落在面要素内部：
    //   - 全部在面内 → 直接收字→按X排序→拼词→对照GDB名称（面内简化路径）
    //   - 不全在面内 → 留在下方常规循环中逐条处理（沿线阈值匹配路径）
    // 从源头避免面内散字被拆成多条碎片错误，不再需要事后跨簇合并。
    // 【2026-07-23扩展】原来只处理UNMATCHED，但PARTIALLY_MATCHED也会被逐字拆散
    //   （如"天祝藏族自治县"7个字在面内但顺序不对 → 7条content_mismatch），
    //   现在一并纳入面内路径：收齐全部字后统一判断顺序/内容错误。
    QSet<qint64> polygonPathCheckedAnnos;
    {
        // 按assoc_name分组UNMATCHED/PARTIALLY_MATCHED注记（仅面要素）
        QMap<QString, QList<qint64>> unmatchedByAssoc;
        for (qint64 annoFid : annosInMatchResult)
        {
            QString matchStat = annoToMatchStat.value(annoFid);
            if (matchStat != "UNMATCHED" && matchStat != "PARTIALLY_MATCHED")
                continue;
            QString assocName = annoToAssocName.value(annoFid);
            QString textConte = annoToText.value(annoFid);
            if (assocName.isEmpty() || textConte.isEmpty())
                continue;
            int geomType = gdbNameGeomTypeCache.value(assocName, 0);
            if (geomType != QgsWkbTypes::PolygonGeometry)
                continue;
            unmatchedByAssoc[assocName].append(annoFid);
        }

        // 对每个面要素名称组，尝试面内路径
        for (auto it = unmatchedByAssoc.constBegin(); it != unmatchedByAssoc.constEnd(); ++it)
        {
            const QString &assocName = it.key();
            const QList<qint64> &annoFids = it.value();

            // 从gdbLayer中找到该名称的面要素几何
            QgsGeometry polyGeom;
            if (gdbLayer && gdbLayer->isValid() && !m_matchLabelField.isEmpty())
            {
                int labelIdx = gdbLayer->fields().indexOf(m_matchLabelField);
                if (labelIdx >= 0)
                {
                    QgsFeatureIterator polyIt = gdbLayer->getFeatures();
                    QgsFeature polyFeat;
                    while (polyIt.nextFeature(polyFeat))
                    {
                        if (!polyFeat.isValid() || !polyFeat.hasGeometry())
                            continue;
                        if (polyFeat.geometry().type() != QgsWkbTypes::PolygonGeometry)
                            continue;
                        QString label = polyFeat.attribute(labelIdx).toString().trimmed();
                        if (label == assocName)
                        {
                            polyGeom = polyFeat.geometry();
                            break;
                        }
                    }
                }
            }
            if (polyGeom.isEmpty())
                continue;

            // 判断：这些注记字是否全部在这个面要素内？
            bool allInside = true;
            for (qint64 fid : annoFids)
            {
                QgsPointXY pos = annoToPosition.value(fid);
                QgsGeometry pointGeom = QgsGeometry::fromPointXY(pos);
                if (!polyGeom.contains(pointGeom))
                {
                    allInside = false;
                    break;
                }
            }

            if (!allInside)
                continue; // 注记不全在面内 → 保留给下方常规UNMATCHED处理（沿线阈值匹配）

            // ===== 面内路径：收字 → 空间分组 → 按X排序 → 逐组拼词对照GDB名称 =====
            // 同一个面要素内可能存在多组重复注记（如青海省面内3组"青海省"标注），
            // 需要先按空间间距分成独立组，再逐组匹配。
            struct CharItem
            {
                QString ch;
                QgsPointXY pos;
                qint64 fid;
            };
            QList<CharItem> chars;
            for (qint64 fid : annoFids)
            {
                CharItem ci;
                ci.ch = annoToText.value(fid);
                ci.pos = annoToPosition.value(fid);
                ci.fid = fid;
                chars.append(ci);
            }

            // 按位置排序：主键X，辅键Y
            std::sort(chars.begin(), chars.end(),
                      [](const CharItem &a, const CharItem &b)
                      {
                          if (std::abs(a.pos.x() - b.pos.x()) < 1.0)
                              return a.pos.y() < b.pos.y();
                          return a.pos.x() < b.pos.x();
                      });

            // 计算相邻字间距，用于空间分组
            QList<double> gaps;
            for (int i = 0; i < chars.size() - 1; ++i)
            {
                double dx = chars[i + 1].pos.x() - chars[i].pos.x();
                double dy = chars[i + 1].pos.y() - chars[i].pos.y();
                gaps.append(std::sqrt(dx * dx + dy * dy));
            }

            // 按间距拆分成独立注记组（每组对应一个注记文本实例）
            QList<QList<CharItem>> spatialGroups;
            if (gaps.isEmpty())
            {
                spatialGroups.append(chars);
            }
            else
            {
                // 取中位间距×3作为分组合并阈值，下限5000地图单位
                QList<double> sortedGaps = gaps;
                std::sort(sortedGaps.begin(), sortedGaps.end());
                double medianGap = sortedGaps[sortedGaps.size() / 2];
                double splitThreshold = std::max(medianGap * 3.0, 5000.0);

                QList<CharItem> curGroup;
                curGroup.append(chars[0]);
                for (int i = 0; i < gaps.size(); ++i)
                {
                    if (gaps[i] > splitThreshold)
                    {
                        spatialGroups.append(curGroup);
                        curGroup.clear();
                    }
                    curGroup.append(chars[i + 1]);
                }
                if (!curGroup.isEmpty())
                    spatialGroups.append(curGroup);
            }

            // 逐组匹配
            int groupIdx = 0;
            for (const QList<CharItem> &group : spatialGroups)
            {
                groupIdx++;

                // 组内按X排序（各字可能有微小X偏差）
                QList<CharItem> sortedGroup = group;
                std::sort(sortedGroup.begin(), sortedGroup.end(),
                          [](const CharItem &a, const CharItem &b)
                          {
                              return a.pos.x() < b.pos.x();
                          });

                // 拼词
                QString formedWord;
                for (const CharItem &ci : sortedGroup)
                    formedWord += ci.ch;

                // 计算组内几何中心
                double avgX = 0, avgY = 0;
                for (const CharItem &ci : sortedGroup)
                {
                    avgX += ci.pos.x();
                    avgY += ci.pos.y();
                }
                avgX /= sortedGroup.size();
                avgY /= sortedGroup.size();

                // 对照GDB名称
                if (formedWord == assocName)
                {
                    // 匹配通过
                    for (const CharItem &ci : group)
                    {
                        polygonPathCheckedAnnos.insert(ci.fid);
                        checkedAnnos.insert(ci.fid);
                    }
                    if (spatialGroups.size() > 1)
                    {
                        m_logger->info("面要素'{}'面内路径[组{}/{}]匹配通过: 收字{}个→排序拼词'{}'==源名称",
                                       assocName.toStdString(), groupIdx, spatialGroups.size(),
                                       sortedGroup.size(), formedWord.toStdString());
                    }
                    else
                    {
                        m_logger->info("面要素'{}'面内路径匹配通过: {}字全部在面内，排序拼词'{}'==源名称",
                                       assocName.toStdString(), sortedGroup.size(), formedWord.toStdString());
                    }
                }
                else
                {
                    // 不匹配 → 判断是顺序错误还是内容错误
                    QStringList actualChars;
                    for (const CharItem &ci : sortedGroup)
                        actualChars.append(ci.ch);

                    // 逐字符拆分 sortedActual，而非按注记块分组
                    // 否则单块多字注记如["区古红"]会和逐字符拆分的["古","红","区"]不匹配
                    QStringList sortedActual;
                    for (const QChar &c : formedWord)
                        sortedActual.append(QString(c));
                    sortedActual.sort();
                    QStringList sortedExpected;
                    for (const QChar &c : assocName)
                        sortedExpected.append(QString(c));
                    sortedExpected.sort();

                    bool isOrderError = (sortedActual.join("") == sortedExpected.join(""));

                    // 反转检查：如果排序后拼词是源名称的完全反转（逐字符），视为正确
                    // 例如"区古红" vs "红古区" → 只是X排序方向相反，不算顺序错误
                    // 多字注记不检查反转：如"土族自治县民和回族"属于顺序问题
                    bool isReversed = false;
                    if (isOrderError && formedWord.size() == assocName.size() && formedWord.size() >= 2)
                    {
                        QString reversed;
                        reversed.reserve(assocName.size());
                        for (int i = assocName.size() - 1; i >= 0; --i)
                            reversed.append(assocName.at(i));
                        if (formedWord == reversed)
                            isReversed = true;
                    }

                    QString groupErrorMessage;

                    if (isReversed)
                    {
                        // 排序后拼词是源名称的完全反转 → 视为正确（只是X排序方向相反）
                        for (const CharItem &ci : group)
                        {
                            polygonPathCheckedAnnos.insert(ci.fid);
                            checkedAnnos.insert(ci.fid);
                        }
                        if (spatialGroups.size() > 1)
                        {
                            m_logger->info("面要素'{}'面内路径[组{}/{}]反转匹配通过: 收字{}个→排序拼词'{}'==反转源名称'{}'",
                                           assocName.toStdString(), groupIdx, spatialGroups.size(),
                                           sortedGroup.size(), formedWord.toStdString(), assocName.toStdString());
                        }
                        else
                        {
                            m_logger->info("面要素'{}'面内路径反转匹配通过: {}字全部在面内，排序拼词'{}'==反转源名称'{}'",
                                           assocName.toStdString(), sortedGroup.size(), formedWord.toStdString(), assocName.toStdString());
                        }
                    }
                    else if (isOrderError)
                    {
                        // 字符全集匹配但顺序不对（非反转）→ order_mismatch
                        if (spatialGroups.size() > 1)
                        {
                            groupErrorMessage = QString::fromUtf8(
                                                    "【注记顺序错误】面要素'%1'的第%2组注记按空间排列拼出'%3'，与源名称'%4'字符相同但顺序不一致（%5字已找到，位置需调整）")
                                                    .arg(assocName)
                                                    .arg(groupIdx)
                                                    .arg(formedWord, assocName)
                                                    .arg(sortedGroup.size());
                        }
                        else
                        {
                            groupErrorMessage = QString::fromUtf8(
                                                    "【注记顺序错误】面要素'%1'的注记按空间排列拼出'%2'，与源名称'%3'字符相同但顺序不一致（全部%4字已找到，位置需调整）")
                                                    .arg(assocName, formedWord, assocName)
                                                    .arg(sortedGroup.size());
                        }

                        // 计算空间组质心，所有字共用同一坐标
                        double gCentroidX = 0, gCentroidY = 0;
                        for (const CharItem &ci : sortedGroup)
                        {
                            gCentroidX += ci.pos.x();
                            gCentroidY += ci.pos.y();
                        }
                        gCentroidX /= sortedGroup.size();
                        gCentroidY /= sortedGroup.size();

                        for (const CharItem &ci : sortedGroup)
                        {
                            AnnotationCheckResult result;
                            result.featureId = QString::number(ci.fid);
                            result.position = QPointF(gCentroidX, gCentroidY);
                            result.layerName = matchResultLayer->name();
                            result.geometryType = QgsWkbTypes::PolygonGeometry;
                            result.expectedValue = assocName;
                            result.actualValue = ci.ch;
                            result.errorType = AnnotationErrorType::FontError;
                            result.errorMessage = groupErrorMessage;
                            results.append(result);
                        }

                        for (const CharItem &ci : group)
                        {
                            polygonPathCheckedAnnos.insert(ci.fid);
                            checkedAnnos.insert(ci.fid);
                        }
                        m_logger->info("面要素'{}'面内路径[组{}/{}]顺序不匹配: 收字{}个→排序拼词'{}'!=源名称'{}'",
                                       assocName.toStdString(), groupIdx, spatialGroups.size(),
                                       sortedGroup.size(), formedWord.toStdString(), assocName.toStdString());
                    }
                    else
                    {
                        // 内容不匹配 → content_mismatch，列出差异
                        QSet<QString> actualSet(actualChars.begin(), actualChars.end());
                        QSet<QString> expectedSet(sortedExpected.begin(), sortedExpected.end());
                        QSet<QString> missing = expectedSet - actualSet;
                        QSet<QString> extra = actualSet - expectedSet;
                        if (spatialGroups.size() > 1)
                        {
                            groupErrorMessage = QString::fromUtf8(
                                                    "【注记内容不匹配】面要素'%1'的第%2组注记拼出'%3'，与源名称'%4'不一致")
                                                    .arg(assocName)
                                                    .arg(groupIdx)
                                                    .arg(formedWord, assocName);
                        }
                        else
                        {
                            groupErrorMessage = QString::fromUtf8(
                                                    "【注记内容不匹配】面要素'%1'的注记拼出'%2'，与源名称'%3'不一致")
                                                    .arg(assocName, formedWord, assocName);
                        }
                        if (!missing.isEmpty())
                            groupErrorMessage += QString::fromUtf8("（缺失: %1）").arg(QStringList(missing.begin(), missing.end()).join(", "));
                        if (!extra.isEmpty())
                            groupErrorMessage += QString::fromUtf8("（多余: %1）").arg(QStringList(extra.begin(), extra.end()).join(", "));

                        // 计算空间组质心，所有字共用同一坐标
                        double gCentroidX = 0, gCentroidY = 0;
                        for (const CharItem &ci : sortedGroup)
                        {
                            gCentroidX += ci.pos.x();
                            gCentroidY += ci.pos.y();
                        }
                        gCentroidX /= sortedGroup.size();
                        gCentroidY /= sortedGroup.size();

                        for (const CharItem &ci : sortedGroup)
                        {
                            AnnotationCheckResult result;
                            result.featureId = QString::number(ci.fid);
                            result.position = QPointF(gCentroidX, gCentroidY);
                            result.layerName = matchResultLayer->name();
                            result.geometryType = QgsWkbTypes::PolygonGeometry;
                            result.expectedValue = assocName;
                            result.actualValue = ci.ch;
                            result.errorType = AnnotationErrorType::FontError;
                            result.errorMessage = groupErrorMessage;
                            results.append(result);
                        }

                        for (const CharItem &ci : group)
                        {
                            polygonPathCheckedAnnos.insert(ci.fid);
                            checkedAnnos.insert(ci.fid);
                        }
                        if (spatialGroups.size() > 1)
                        {
                            m_logger->info("面要素'{}'面内路径[组{}/{}]不匹配: 收字{}个→排序拼词'{}'!=源名称'{}'",
                                           assocName.toStdString(), groupIdx, spatialGroups.size(),
                                           sortedGroup.size(), formedWord.toStdString(), assocName.toStdString());
                        }
                        else
                        {
                            m_logger->info("面要素'{}'面内路径不匹配: 收字{}个→排序拼词'{}'!=源名称'{}'",
                                           assocName.toStdString(), sortedGroup.size(), formedWord.toStdString(),
                                           assocName.toStdString());
                        }
                    }
                }
            }
        }
    }

    // === 错误类型1：检查所有注记的匹配状态和符号配对情况 ===
    for (qint64 annoFid : annosInMatchResult)
    {
        QString textConte = annoToText.value(annoFid);
        QString assocName = annoToAssocName.value(annoFid);
        QString matchStat = annoToMatchStat.value(annoFid);
        double matchScor = annoToMatchScor.value(annoFid).toDouble();
        QgsPointXY annoPos = annoToPosition.value(annoFid);
        QString symbolId = annoToSymbol.value(annoFid);

        // 1.0 未匹配的注记（UNMATCHED）
        // 已被面内路径处理的注记直接跳过；剩余走原有逐条处理逻辑（线要素/面外注记）
        if (matchStat == "UNMATCHED")
        {
            // 面内路径已处理 → 跳过
            if (polygonPathCheckedAnnos.contains(annoFid))
            {
                continue;
            }

            // 【2026-07-23新增】GDB名称缺失疑似标记：
            //   该注记落在GDB要素内但NAME字段为空，无法自动校验 → 标记为疑似供人工复查
            //   expectedValue设为空，让generateGroupedJsonReport将其归入otherErrors单独输出，
            //   避免被按expectedValue分组+空间聚类拼成乱码（如不同路的字拼到一起）
            static const QString SUSPICIOUS_GDB_NULL_NAME = QString::fromUtf8("【GDB名称缺失-疑似】");
            if (assocName == SUSPICIOUS_GDB_NULL_NAME)
            {
                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::FontError;
                result.featureId = QString::number(annoFid);
                result.actualValue = textConte;
                result.expectedValue.clear(); // 空值 → otherErrors 单独输出，不参与分组
                result.errorMessage = QString::fromUtf8("【GDB名称缺失-疑似】注记'%1'附近GDB要素名称字段为空，无法自动校验，请人工复查").arg(textConte);
                result.position = QPointF(annoPos.x(), annoPos.y());
                results.append(result);
                checkedAnnos.insert(annoFid);

                m_logger->info("[疑似] GDB名称缺失-疑似: annoFID={} text='{}' pos=({},{})",
                               annoFid, textConte.toStdString(), annoPos.x(), annoPos.y());
                continue;
            }

            // 注记文字与源名称是否有包含关系
            bool hasTextRelation = false;
            if (!assocName.isEmpty() && !textConte.isEmpty())
            {
                // 注记文字在源名称中出现，或源名称在注记文字中出现
                hasTextRelation = assocName.contains(textConte) || textConte.contains(assocName);
            }

            if (!hasTextRelation)
            {
                // 无文本关联的空间兜底注记：不输出到报告，直接跳过
                skippedUnmatchedOrphanCount++;
                checkedAnnos.insert(annoFid);
                continue;
            }

            AnnotationCheckResult result;
            result.errorType = AnnotationErrorType::FontError;
            result.featureId = QString::number(annoFid);
            result.actualValue = textConte;
            if (!assocName.isEmpty())
            {
                // 使用 assocName 作为 expectedValue，用于在JSON中显示源名称
                // 但加上FID后缀确保每条UNMATCHED条目都唯一，不会被generateGroupedJsonReport的空间聚类串在一起
                result.expectedValue = QString("%1 [UNMATCHED_FID:%2]").arg(assocName).arg(annoFid);
                result.errorMessage = QString::fromUtf8("【注记不匹配】注记'%1'(FID=%2)与源名称'%3'不匹配")
                                          .arg(textConte)
                                          .arg(annoFid)
                                          .arg(assocName);
            }
            else
            {
                result.expectedValue = QString("应匹配到GDB要素 [UNMATCHED_FID:%1]").arg(annoFid);
                result.errorMessage = QString::fromUtf8("【注记未匹配】注记 '%1' (FID=%2) 未能匹配到任何GDB要素")
                                          .arg(textConte)
                                          .arg(annoFid);
            }
            result.position = QPointF(annoPos.x(), annoPos.y());
            result.layerName = matchResultLayer->name();
            // 标记几何类型：面要素(QgsWkbTypes::PolygonGeometry=3)、线要素(2)、未知(0)
            result.geometryType = gdbNameGeomTypeCache.value(assocName, 0);
            results.append(result);
            checkedAnnos.insert(annoFid);
            continue; // 未匹配的注记不需要进行后续检查
        }

        // 1.1 注记内容错误（PARTIALLY_MATCHED）
        // 已被面内路径处理的注记直接跳过（面内路径已统一判断顺序/内容错误）
        if (polygonPathCheckedAnnos.contains(annoFid))
        {
            checkedAnnos.insert(annoFid);
            continue;
        }
        if ((matchStat == "PARTIALLY_MATCHED" && matchScor < 0.99) || (matchStat == "PARTIALLY_MATCHED" && matchScor > 1 && matchScor < 1.99))
        {
            AnnotationCheckResult result;
            result.errorType = AnnotationErrorType::FontError;
            result.featureId = QString::number(annoFid);
            result.actualValue = textConte;
            result.expectedValue = assocName;
            result.errorMessage = QString("【注记内容错误】注记内容 '%1' 与源名称 '%2' 不完全匹配")
                                      .arg(textConte, assocName);
            result.position = QPointF(annoPos.x(), annoPos.y());
            result.layerName = matchResultLayer->name();
            results.append(result);
            checkedAnnos.insert(annoFid);
        }

        // 1.2 注记缺失符号（仅当实际图层被识别为点符号图层时检查）
        if (actualLayerIsPointSymbol && !m_allowNoSymbol && symbolId.isEmpty())
        {
            AnnotationCheckResult result;
            result.errorType = AnnotationErrorType::FontError;
            result.featureId = QString::number(annoFid);
            result.actualValue = textConte;                                     // 注记实际文本，而非占位符
            result.expectedValue = assocName.isEmpty() ? textConte : assocName; // 关联的GDB要素名
            result.errorMessage = QString("【注记缺失符号】注记 '%1' (FID=%2) 没有对应的符号")
                                      .arg(textConte)
                                      .arg(annoFid);
            result.position = QPointF(annoPos.x(), annoPos.y());
            result.layerName = matchResultLayer->name();
            results.append(result);
            checkedAnnos.insert(annoFid);
        }

        // 1.3 间距检查（只检查完全匹配且有符号的注记）
        if (!symbolId.isEmpty() && matchStat == "FULLY_MATCHED")
        {
            if (!existingSymbolIds.contains(symbolId))
            {
                m_logger->warn("注记 {} 引用的符号 {} 在实际图层中不存在", annoFid, symbolId.toStdString());
                continue;
            }

            QgsPointXY symbolCenter = symbolCenters.value(symbolId);
            if (!symbolCenter.isEmpty())
            {
                double distance = annoPos.distance(symbolCenter);

                if (distance < minDistanceMapUnits)
                {
                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::OverlapError;
                    result.featureId = QString::number(annoFid);
                    result.actualValue = QString::number(distance * 1000.0 / m_mapScale, 'f', 2) + "mm";
                    result.expectedValue = QString("≥%1mm").arg(m_minDistanceMm);
                    result.errorMessage = QString("【间距过小】注记 '%1' 与符号 '%2' 间距 %3mm < 最小间距 %4mm")
                                              .arg(textConte)
                                              .arg(symbolId)
                                              .arg(distance * 1000.0 / m_mapScale, 0, 'f', 2)
                                              .arg(m_minDistanceMm);
                    result.position = QPointF(annoPos.x(), annoPos.y());
                    result.layerName = matchResultLayer->name();
                    results.append(result);
                }
                else if (distance > maxDistanceMapUnits)
                {
                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::OverlapError;
                    result.featureId = QString::number(annoFid);
                    result.actualValue = QString::number(distance * 1000.0 / m_mapScale, 'f', 2) + "mm";
                    result.expectedValue = QString("≤%1mm").arg(m_maxDistanceMm);
                    result.errorMessage = QString("【间距过大】注记 '%1' 与符号 '%2' 间距 %3mm > 最大间距 %4mm")
                                              .arg(textConte)
                                              .arg(symbolId)
                                              .arg(distance * 1000.0 / m_mapScale, 0, 'f', 2)
                                              .arg(m_maxDistanceMm);
                    result.position = QPointF(annoPos.x(), annoPos.y());
                    result.layerName = matchResultLayer->name();
                    results.append(result);
                }
            }
        }
    }

    // === 错误类型2：符号缺失注记 ===
    // 检查实际图层中存在但在匹配结果中没有注记配对的符号
    // GRADE过滤已前置：构建symbolIdToGdbName时只索引了目标等级(如3A)的GDB要素，
    // 因此只有能空间匹配到3A GDB的符号才会进入symbolIdToGdbName，直接用它判断即可
    if (actualLayerIsPointSymbol && !m_allowNoAnnotation)
    {
        m_logger->info("检查符号缺失注记...");
        int missingAnnoCount = 0;

        for (const QString &symbolId : existingSymbolIds)
        {
            // 如果符号ID没有出现在匹配结果的symbol_id字段中，说明缺失注记
            if (!symbolsInMatchResult.contains(symbolId))
            {
                // 只有能空间匹配到目标等级GDB要素的符号才参与检查（消除4A等混入）
                if (!symbolIdToGdbName.contains(symbolId))
                    continue;

                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::FontError;
                result.featureId = symbolId; // 使用symbol_id作为featureId

                // 优先从预先构建的空间映射获取GDB要素名称
                QString symbolFeatureName = symbolIdToGdbName.value(symbolId);

                // 回退：遍历actualLayer按symbol_id字段匹配查找symbol_nam字段
                if (symbolFeatureName.isEmpty())
                {
                    int symNamIdx = actualLayer->fields().indexOf("symbol_nam");
                    if (symNamIdx >= 0)
                    {
                        QgsFeatureIterator symIt = actualLayer->getFeatures();
                        QgsFeature symFeat;
                        while (symIt.nextFeature(symFeat))
                        {
                            int sidIdx = actualLayer->fields().indexOf("symbol_id");
                            if (sidIdx >= 0 && symFeat.attribute(sidIdx).toString() == symbolId)
                            {
                                QString val = symFeat.attribute(symNamIdx).toString().trimmed();
                                if (!val.isEmpty())
                                {
                                    symbolFeatureName = val;
                                    break;
                                }
                            }
                        }
                    }
                }

                result.actualValue = "无注记"; // 保留用于classifyError识别为missing_annotation
                // 如果找不到真实名称，用符号组编号替代cached_symbol_*（更可读）
                result.expectedValue = symbolFeatureName.isEmpty()
                                           ? QString::fromUtf8("符号组（%1）").arg(symbolId.section('_', -1))
                                           : symbolFeatureName;
                result.errorMessage = QString::fromUtf8("【符号缺失注记】符号'%1'%2在实际图层中存在但没有对应的注记")
                                          .arg(symbolId)
                                          .arg(symbolFeatureName.isEmpty() ? QString() : QString::fromUtf8("（%1）").arg(symbolFeatureName));

                QgsPointXY symbolCenter = symbolCenters.value(symbolId, QgsPointXY());
                result.position = QPointF(symbolCenter.x(), symbolCenter.y());
                result.layerName = actualLayer->name();
                results.append(result);
                missingAnnoCount++;
            }
        }

        m_logger->info("发现 {} 个符号缺失注记", missingAnnoCount);
    }

    // === 错误类型3：GDB线/面要素缺失注记 ===
    if (gdbLayer && gdbLayer->isValid() && !m_allowNoAnnotation)
    {
        if (!m_matchRulesLoaded)
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("质检规则中缺少当前比例尺可用的ID=200注记匹配规则");
            return results;
        }
        else if (m_matchLabelField.isEmpty())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("ID=200注记匹配规则未配置标注字段");
            return results;
        }
        else if (m_matchLevelAttr.isEmpty() && !m_matchLevelValuesCsv.trimmed().isEmpty())
        {
            if (errorMessage)
                *errorMessage = QStringLiteral("ID=200注记匹配规则配置了属性值，但未配置属性项");
            return results;
        }
        else
        {
            m_logger->info("检查GDB线/面要素缺失注记；点要素仅按实际图层中的符号检查...");

            int labelIdx = gdbLayer->fields().indexOf(m_matchLabelField);
            int levelIdx = m_matchLevelAttr.isEmpty() ? -1 : gdbLayer->fields().indexOf(m_matchLevelAttr);
            if (labelIdx < 0)
            {
                if (errorMessage)
                    *errorMessage = QString("GDB图层缺少ID=200配置的标注字段：%1").arg(m_matchLabelField);
                return results;
            }
            else if (!m_matchLevelAttr.isEmpty() && levelIdx < 0)
            {
                if (errorMessage)
                    *errorMessage = QString("GDB图层缺少ID=200配置的分层字段：%1").arg(m_matchLevelAttr);
                return results;
            }
            else
            {
                QString normalizedValues = m_matchLevelValuesCsv;
                normalizedValues.replace(QChar(u'，'), ',');
                normalizedValues.replace(QChar(u'；'), ';');
                normalizedValues.replace(';', ',');

                QSet<QString> allowedLevelValues;
                for (const QString &part : normalizedValues.split(',', Qt::SkipEmptyParts))
                {
                    QString value = part;
                    value.replace(" ", "");
                    value = value.trimmed().toLower();
                    if (!value.isEmpty())
                        allowedLevelValues.insert(value);
                }

                struct NamedGdbGroup
                {
                    QString name;
                    QList<qint64> fids;
                    QgsGeometry geometry;
                    QgsWkbTypes::GeometryType geometryType;
                };

                QMap<QString, QList<qint64>> nameToFids;
                QMap<qint64, QgsGeometry> gdbGeomCache;
                int checkedGdbFeatureCount = 0;
                QgsFeatureIterator gdbIt = gdbLayer->getFeatures();
                QgsFeature gdbFeature;
                while (gdbIt.nextFeature(gdbFeature))
                {
                    if (!gdbFeature.isValid() || !gdbFeature.hasGeometry())
                        continue;

                    QgsGeometry geom = gdbFeature.geometry();
                    QgsWkbTypes::GeometryType geomType = geom.type();
                    if (geomType != QgsWkbTypes::LineGeometry &&
                        geomType != QgsWkbTypes::PolygonGeometry)
                        continue;

                    QString label = gdbFeature.attribute(labelIdx).toString().trimmed();
                    if (label.isEmpty())
                        continue;

                    if (levelIdx >= 0 && !allowedLevelValues.isEmpty())
                    {
                        QString levelValue = gdbFeature.attribute(levelIdx).toString();
                        levelValue.replace(" ", "");
                        levelValue = levelValue.trimmed().toLower();

                        bool levelMatched = false;
                        for (const QString &filter : allowedLevelValues)
                        {
                            if ((filter.endsWith('*') && levelValue.startsWith(filter.left(filter.length() - 1))) ||
                                levelValue == filter)
                            {
                                levelMatched = true;
                                break;
                            }
                        }

                        if (!levelMatched)
                            continue;
                    }

                    checkedGdbFeatureCount++;
                    nameToFids[label].append(gdbFeature.id());
                    gdbGeomCache.insert(gdbFeature.id(), geom);
                }

                QList<NamedGdbGroup> mergedGroups;
                double proximityThreshold = mmToMapUnits(5.0);
                for (auto nameIt = nameToFids.constBegin(); nameIt != nameToFids.constEnd(); ++nameIt)
                {
                    const QList<qint64> &fids = nameIt.value();
                    QSet<qint64> processed;

                    for (qint64 seedFid : fids)
                    {
                        if (processed.contains(seedFid))
                            continue;

                        QList<qint64> groupFids;
                        QList<qint64> queue;
                        queue.append(seedFid);
                        processed.insert(seedFid);

                        int queueIndex = 0;
                        while (queueIndex < queue.size())
                        {
                            qint64 currentFid = queue.at(queueIndex++);
                            groupFids.append(currentFid);

                            for (qint64 otherFid : fids)
                            {
                                if (processed.contains(otherFid))
                                    continue;

                                if (gdbGeomCache.value(currentFid).distance(gdbGeomCache.value(otherFid)) < proximityThreshold)
                                {
                                    queue.append(otherFid);
                                    processed.insert(otherFid);
                                }
                            }
                        }

                        QList<QgsGeometry> geomsToMerge;
                        for (qint64 fid : groupFids)
                            geomsToMerge.append(gdbGeomCache.value(fid));

                        QgsGeometry mergedGeom = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(geomsToMerge));
                        if (mergedGeom.isEmpty() && !geomsToMerge.isEmpty())
                            mergedGeom = geomsToMerge.first();
                        if (mergedGeom.isEmpty())
                            continue;

                        NamedGdbGroup group;
                        group.name = nameIt.key();
                        group.fids = groupFids;
                        group.geometry = mergedGeom;
                        group.geometryType = mergedGeom.type();
                        mergedGroups.append(group);
                    }
                }

                int missingGdbAnnoCount = 0;
                for (const NamedGdbGroup &group : mergedGroups)
                {
                    bool groupMatched = false;
                    for (qint64 fid : group.fids)
                    {
                        if (matchedGdbFids.contains(fid))
                        {
                            groupMatched = true;
                            break;
                        }
                    }
                    if (groupMatched)
                        continue;

                    QStringList fidParts;
                    for (qint64 fid : group.fids)
                        fidParts.append(QString::number(fid));
                    QString gdbFids = fidParts.join(",");

                    QgsPointXY pos = group.geometry.centroid().asPoint();
                    QString geomTypeName;
                    if (group.geometryType == QgsWkbTypes::PointGeometry)
                        geomTypeName = QStringLiteral("点");
                    else if (group.geometryType == QgsWkbTypes::LineGeometry)
                        geomTypeName = QStringLiteral("线");
                    else
                        geomTypeName = QStringLiteral("面");

                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::FontError;
                    result.featureId = gdbFids;
                    result.actualValue = "无注记";
                    result.expectedValue = group.name;
                    result.errorMessage = QString("【GDB要素缺失注记】%1要素 '%2' (GDB FIDs=%3) 名称不为空但未标注")
                                              .arg(geomTypeName)
                                              .arg(group.name)
                                              .arg(gdbFids);
                    result.position = QPointF(pos.x(), pos.y());
                    result.layerName = gdbLayer->name();
                    results.append(result);
                    missingGdbAnnoCount++;
                }

                m_logger->info("GDB缺失注记检查完成：检查 {} 个命名线/面要素，合并为 {} 个空间组，发现 {} 个未标注组",
                               checkedGdbFeatureCount, mergedGroups.size(), missingGdbAnnoCount);
            }
        }
    }

    // === 后处理：多字注记回扫 ===
    // 针对"缺失注记"的结果，扫描附近 UNMATCHED 多字注记（len>1）。
    // 如果与GDB名称的字符重叠率 ≥ 50%，说明可能是注记写了错别字，
    // 导致200匹配失败，实际应为"内容不匹配"而非"缺失注记"。
    if (!m_allowNoAnnotation)
    {
        m_logger->info("后处理：多字注记回扫...");

        // 第一步：收集所有 UNMATCHED 多字注记（文字长度>1）
        struct UnmatchedMultiAnno
        {
            qint64 fid;
            QString text;
            QgsPointXY pos;
        };
        QList<UnmatchedMultiAnno> unmatchedMultiCharAnnos;
        for (qint64 annoFid : annosInMatchResult)
        {
            if (annoToMatchStat.value(annoFid) == "UNMATCHED")
            {
                QString text = annoToText.value(annoFid);
                if (text.length() > 1)
                {
                    unmatchedMultiCharAnnos.append({annoFid, text, annoToPosition.value(annoFid)});
                }
            }
        }

        if (!unmatchedMultiCharAnnos.isEmpty())
        {
            m_logger->info("  收集到 {} 个UNMATCHED多字注记候选", unmatchedMultiCharAnnos.size());

            // 搜索半径：50mm 对应的地图单位（足够覆盖近邻注记）
            double searchRadius = mmToMapUnits(50.0);
            int reclassifiedCount = 0;

            for (int ri = 0; ri < results.size(); ++ri)
            {
                AnnotationCheckResult &result = results[ri];

                // 只处理"缺失注记"类型
                if (result.actualValue != QString::fromUtf8("无注记"))
                    continue;

                QString gdbName = result.expectedValue;
                if (gdbName.isEmpty())
                    continue;

                QPointF rpos = result.position;
                if (rpos.isNull())
                    continue;

                QgsPointXY resultPos(rpos.x(), rpos.y());

                // 第二步：搜附近 UNMATCHED 多字注记，计算字符重叠率
                qint64 bestFid = -1;
                QString bestText;
                double bestOverlap = 0.0;

                // 拆 GDB 名称单字集合
                QSet<QChar> gdbCharSet;
                for (const QChar &c : gdbName)
                {
                    if (!c.isSpace())
                        gdbCharSet.insert(c);
                }
                if (gdbCharSet.isEmpty())
                    continue;

                for (const UnmatchedMultiAnno &ua : unmatchedMultiCharAnnos)
                {
                    double dist = ua.pos.distance(resultPos);
                    if (dist > searchRadius)
                        continue;

                    // 拆注记文字单字集合
                    QSet<QChar> annoCharSet;
                    for (const QChar &c : ua.text)
                    {
                        if (!c.isSpace())
                            annoCharSet.insert(c);
                    }
                    if (annoCharSet.isEmpty())
                        continue;

                    // 计算重叠率 = 交集字数 / max(GDB字数, 注记字数)
                    QSet<QChar> overlap = gdbCharSet;
                    overlap.intersect(annoCharSet);

                    int maxLen = qMax(gdbCharSet.size(), annoCharSet.size());
                    double ratio = (double)overlap.size() / maxLen;

                    // 阈值：≥ 50%（对3字词来说就是 ≥ 2/3 重叠）
                    if (ratio >= 0.5 && ratio > bestOverlap)
                    {
                        bestOverlap = ratio;
                        bestFid = ua.fid;
                        bestText = ua.text;
                    }
                }

                // 第三步：找到匹配 → 将"缺失注记"改为"内容不匹配"
                if (bestFid >= 0)
                {
                    result.actualValue = bestText;
                    result.featureId = QString::number(bestFid);
                    result.errorMessage = QString::fromUtf8("【内容不匹配-回扫】注记'%1'与源名称'%2'不匹配，字符重叠率%.0f%%（疑似注记内部错别字）")
                                              .arg(bestText, gdbName)
                                              .arg(bestOverlap * 100.0);
                    reclassifiedCount++;
                }
            }

            if (reclassifiedCount > 0)
                m_logger->info("  多字注记回扫：{} 条\"缺失注记\"被重新识别为\"内容不匹配\"", reclassifiedCount);
            else
                m_logger->info("  多字注记回扫：未发现可重新分类的条目");
        }
        else
        {
            m_logger->info("  无UNMATCHED多字注记，跳过多字回扫");
        }
    }

    // === 错误类型4：孤儿注记检测 ===
    // 已禁用。孤儿注记检测容易引入误报（如将其他图层的注记误关联到当前GDB要素），
    // 且主要需求关注的是已匹配注记的内容正确性，而非周边遗漏注记。如需重新启用，取消下方注释即可。
#if 0
    if (annoPolygonLayer && annoPolygonLayer->isValid() && gdbLayer && gdbLayer->isValid())
    {
        m_logger->info("开始孤儿注记检测（扫描已匹配GDB要素周边的遗漏注记）...");

        int annoTextIdx = annoPolygonLayer->fields().indexOf("text_conte");
        int annoFontSizeIdx = annoPolygonLayer->fields().indexOf("font_size");
        int annoFontFamilIdx = annoPolygonLayer->fields().indexOf("font_famil");
        int annoElemTyIdx = annoPolygonLayer->fields().indexOf("element_ty");

        if (annoTextIdx >= 0 && annoFontSizeIdx >= 0 && annoFontFamilIdx >= 0)
        {
            double searchRadiusMU = mmToMapUnits(20.0);
            double fontSizeTolerance = 2.0;

            struct GdbMatchedGroup
            {
                QString sourceName;
                QSet<qint64> matchedAnnoFids;
                QString fontFamil;
                double fontSize;
                QgsGeometry geom;
            };
            QMap<QString, GdbMatchedGroup> gdbGroups;

            QgsFeatureIterator mrit = matchResultLayer->getFeatures();
            QgsFeature mrf;
            while (mrit.nextFeature(mrf))
            {
                QString gdbFidsStr;
                if (gdbFidsIdx >= 0)
                    gdbFidsStr = mrf.attribute(gdbFidsIdx).toString().trimmed();
                if (gdbFidsStr.isEmpty() && gdbFidIdx >= 0)
                {
                    QVariant fidVar = mrf.attribute(gdbFidIdx);
                    if (!fidVar.isNull())
                        gdbFidsStr = fidVar.toString();
                }
                if (gdbFidsStr.isEmpty() || gdbFidsStr == "-1")
                    continue;

                QVariant afVar = mrf.attribute(annoFidIdx);
                bool ok = false;
                qint64 afid = afVar.toLongLong(&ok);
                if (!ok || afid < 0)
                    continue;

                QString matchStat = mrf.attribute(matchStatIdx).toString();
                // 【修改】不再跳过UNMATCHED条目，让其进入主流程与正确GDB名称(assoc_name)关联，
                // 避免落入孤儿注记检测时丢失sourceName导致显示"应匹配到GDB要素"

                QString fontFamil = matchResultLayer->fields().indexOf("font_famil") >= 0
                    ? mrf.attribute(matchResultLayer->fields().indexOf("font_famil")).toString() : "";
                double fontSize = matchResultLayer->fields().indexOf("font_size") >= 0
                    ? mrf.attribute(matchResultLayer->fields().indexOf("font_size")).toDouble() : 0.0;
                QString assocName = assocNameIdx >= 0 ? mrf.attribute(assocNameIdx).toString() : "";

                if (!gdbGroups.contains(gdbFidsStr))
                {
                    GdbMatchedGroup grp;
                    grp.sourceName = assocName;
                    grp.fontFamil = fontFamil;
                    grp.fontSize = fontSize;
                    grp.matchedAnnoFids.insert(afid);
                    gdbGroups.insert(gdbFidsStr, grp);
                }
                else
                {
                    GdbMatchedGroup &grp = gdbGroups[gdbFidsStr];
                    grp.matchedAnnoFids.insert(afid);
                    if (fontSize > 0 && grp.fontSize <= 0)
                        grp.fontSize = fontSize;
                    if (fontFamil.isEmpty() && !grp.fontFamil.isEmpty())
                        grp.fontFamil = grp.fontFamil;
                }
            }

            for (auto git = gdbGroups.begin(); git != gdbGroups.end(); ++git)
            {
                GdbMatchedGroup &grp = git.value();
                QStringList fidParts = git.key().split(',', Qt::SkipEmptyParts);
                QList<QgsGeometry> geomsToMerge;
                for (const QString &part : fidParts)
                {
                    bool fidOk = false;
                    qint64 fid = part.trimmed().toLongLong(&fidOk);
                    if (!fidOk || fid < 0)
                        continue;
                    QgsFeature gf = gdbLayer->getFeature(fid);
                    if (gf.isValid() && gf.hasGeometry())
                        geomsToMerge.append(gf.geometry());
                }
                if (geomsToMerge.isEmpty())
                    continue;
                if (geomsToMerge.size() == 1)
                    grp.geom = geomsToMerge.first();
                else
                    grp.geom = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(geomsToMerge));
            }

            int orphanCount = 0;
            QgsFeatureIterator annoIt = annoPolygonLayer->getFeatures();
            QgsFeature annoFeat;
            while (annoIt.nextFeature(annoFeat))
            {
                if (!annoFeat.hasGeometry())
                    continue;

                if (annoElemTyIdx >= 0)
                {
                    QVariant ety = annoFeat.attribute(annoElemTyIdx);
                    if (annoFeat.geometry().type() == QgsWkbTypes::PolygonGeometry
                        && ety.isValid() && ety.toString().toLower() != "text")
                        continue;
                }

                qint64 annoFid = annoFeat.id();
                QString textConte = annoFeat.attribute(annoTextIdx).toString().trimmed();
                double fontSize = annoFeat.attribute(annoFontSizeIdx).toDouble();
                QString fontFamil = annoFeat.attribute(annoFontFamilIdx).toString();

                if (textConte.isEmpty() || fontSize <= 0)
                    continue;

                if (annosInMatchResult.contains(annoFid))
                    continue;

                QgsPointXY annoCenter = annoFeat.geometry().centroid().asPoint();

                for (auto git = gdbGroups.constBegin(); git != gdbGroups.constEnd(); ++git)
                {
                    const GdbMatchedGroup &grp = git.value();
                    if (grp.geom.isEmpty())
                        continue;

                    if (grp.fontFamil.isEmpty() || fontFamil.isEmpty())
                        continue;
                    QString ff1 = grp.fontFamil.trimmed();
                    QString ff2 = fontFamil.trimmed();
                    if (ff1.compare(ff2, Qt::CaseInsensitive) != 0)
                        continue;
                    if (grp.fontSize > 0 && std::abs(fontSize - grp.fontSize) > fontSizeTolerance)
                        continue;

                    double distMU = grp.geom.distance(QgsGeometry::fromPointXY(annoCenter));
                    if (distMU > searchRadiusMU)
                        continue;

                    QMap<QString, AnnotationErrorType> dummy;
                    Q_UNUSED(dummy);

                    QString errDesc;
                    if (textConte.length() == 1 && !grp.sourceName.isEmpty() && !grp.sourceName.contains(textConte))
                    {
                        errDesc = QString("【孤儿注记-内容错误】在要素'%1'附近发现单字注记'%2'(FID=%3)，字体与要素注记一致(%4, %.1fpt)，但该字不在源名称'%1'中")
                            .arg(grp.sourceName, textConte).arg(annoFid).arg(fontFamil).arg(fontSize);
                    }
                    else
                    {
                        errDesc = QString("【孤儿注记-未关联】在已匹配要素附近发现未关联注记'%1'(FID=%2)，字体(%3, %.1fpt)与周边注记一致但未被200算法匹配")
                            .arg(textConte).arg(annoFid).arg(fontFamil).arg(fontSize);
                    }

                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::FontError;
                    result.featureId = QString::number(annoFid);
                    result.actualValue = textConte;
                    result.expectedValue = grp.sourceName.isEmpty() ? "应匹配到GDB要素" : grp.sourceName;
                    result.errorMessage = errDesc;
                    result.position = QPointF(annoCenter.x(), annoCenter.y());
                    result.layerName = annoPolygonLayer->name();
                    results.append(result);
                    orphanCount++;

                    m_logger->info("孤儿注记: FID={} text='{}' 源名称='{}' 字体={} size={:.1f}pt 距GDB线={:.0f}m",
                        annoFid, textConte.toStdString(), grp.sourceName.toStdString(),
                        fontFamil.toStdString(), fontSize, distMU);
                    break;
                }
            }
            m_logger->info("孤儿注记检测完成，发现 {} 个孤儿注记", orphanCount);
        }
        else
        {
            m_logger->warn("原始注记图层缺少必要字段(text_conte, font_size, font_famil)，跳过孤儿注记检测");
        }
    }
#endif

    m_logger->info("符号注记匹配检查完成，发现 {} 个问题（其中未匹配注记 {} 个，已过滤无文本关联的空间兜底注记 {} 个）",
                   results.size(), unmatchedAnnoCount, skippedUnmatchedOrphanCount);
    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkCharEqualSpacing(
    QgsVectorLayer *matchResultLayer, QMap<QString, QString> *statistics)
{
    QList<AnnotationCheckResult> results;
    const QgsFields &fields = matchResultLayer->fields();
    const int groupIdx = fields.indexOf("group_id");
    const int fidIdx = fields.indexOf("anno_fid");
    const int textIdx = fields.indexOf("text_conte");
    const int nameIdx = fields.indexOf("assoc_name");
    int statusIdx = fields.indexOf("match_status");
    if (statusIdx < 0)
        statusIdx = fields.indexOf("match_stat");

    QMap<qint64, QList<QgsFeature>> groups;
    QgsFeature feature;
    auto iterator = matchResultLayer->getFeatures();
    int ungrouped = 0;
    while (iterator.nextFeature(feature))
    {
        const qint64 groupId = feature.attribute(groupIdx).toLongLong();
        if (groupId > 0)
            groups[groupId].append(feature);
        else
            ++ungrouped;
    }

    int checked = 0, failed = 0, indeterminate = 0, notApplicable = 0;
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
    {
        const auto &features = it.value();
        const QString name = nameIdx >= 0 ? features.first().attribute(nameIdx).toString().trimmed() : QString();
        QString reason;
        auto recordIndeterminate = [&](const QString &detail) {
            ++indeterminate;
            m_logger->warn("散列注记等间距无法判定：group_id={}, {}", it.key(), detail.toStdString());
            if (statistics)
                statistics->insert(QString("等间距无法判定 group_id=%1").arg(it.key()), detail);
        };

        struct Character
        {
            qint64 fid;
            QString text;
            QgsPointXY center;
        };
        QList<Character> characters;
        QSet<qint64> seen;
        bool allSingleCharacters = true;
        for (const QgsFeature &item : features)
        {
            const QString text = item.attribute(textIdx).toString();
            if (text.toUcs4().size() != 1)
                allSingleCharacters = false;
            bool fidOk = false;
            const qint64 fid = item.attribute(fidIdx).toLongLong(&fidOk);
            if (!fidOk || fid < 0 || seen.contains(fid))
                reason = "注记FID无效或重复";
            seen.insert(fid);
            if (nameIdx < 0 || item.attribute(nameIdx).toString().trimmed() != name || name.isEmpty())
                reason = "缺少一致的关联名称，不能验证注记完整性";
            if (statusIdx < 0 || item.attribute(statusIdx).toString().trimmed() != "FULLY_MATCHED")
                reason = "注记未完全匹配或缺少匹配状态，可能缺字或包含误匹配字符";
            const QgsGeometry geometry = item.geometry();
            const QgsGeometry centroid = geometry.centroid();
            if (geometry.isNull() || geometry.isEmpty() || centroid.isNull() || centroid.isEmpty())
            {
                reason = "注记几何为空或无法获取中心点";
                continue;
            }
            const QgsPointXY center = centroid.asPoint();
            if (!std::isfinite(center.x()) || !std::isfinite(center.y()))
            {
                reason = "注记中心坐标无效";
                continue;
            }
            characters.append({fid, text, center});
        }
        if (!allSingleCharacters)
        {
            if (features.size() == 1)
                ++notApplicable;
            else
                recordIndeterminate("组内包含多字文本，不能直接恢复逐字中心位置");
            continue;
        }
        if (reason.isEmpty())
        {
            const auto nameCharacters = name.toUcs4();
            QMap<QString, int> required, actual;
            for (uint codepoint : nameCharacters)
                ++required[QString::fromUcs4(&codepoint, 1)];
            for (const auto &character : characters)
                ++actual[character.text];
            if (required != actual)
                reason = "组内字符与关联名称不完整对应（含重复字数量）";
        }
        if (!reason.isEmpty())
        {
            recordIndeterminate(reason);
            continue;
        }
        if (characters.size() < 3)
        {
            ++notApplicable;
            continue;
        }

        // 名称限定阅读顺序；重复字按最短空间路径分配，等长解不强行判定。
        const auto nameCharacters = name.toUcs4();
        QList<QList<int>> candidates;
        for (uint codepoint : nameCharacters)
        {
            QList<int> indices;
            const QString text = QString::fromUcs4(&codepoint, 1);
            for (int i = 0; i < characters.size(); ++i)
                if (characters[i].text == text)
                    indices.append(i);
            candidates.append(indices);
        }
        QList<int> path, bestPath;
        QVector<bool> used(characters.size(), false);
        double bestLength = std::numeric_limits<double>::infinity();
        bool ambiguous = false, exhausted = false;
        int visited = 0;
        std::function<void(int, double)> search = [&](int index, double length) {
            // shortcut: 重复字排列搜索最多访问4096个节点，超限组报告无法判定。
            if (++visited > 4096)
            {
                exhausted = true;
                return;
            }
            if (index == candidates.size())
            {
                const double epsilon = 1e-9 * std::max(1.0, length);
                if (bestPath.isEmpty() || length < bestLength - epsilon)
                {
                    bestLength = length;
                    bestPath = path;
                    ambiguous = false;
                }
                else if (std::abs(length - bestLength) <= epsilon)
                {
                    QList<int> reversed = bestPath;
                    std::reverse(reversed.begin(), reversed.end());
                    if (path != reversed)
                        ambiguous = true;
                }
                return;
            }
            for (int candidate : candidates[index])
            {
                if (used[candidate])
                    continue;
                const double distance = path.isEmpty() ? 0.0 :
                    characters[path.last()].center.distance(characters[candidate].center);
                used[candidate] = true;
                path.append(candidate);
                search(index + 1, length + distance);
                path.removeLast();
                used[candidate] = false;
                if (exhausted)
                    return;
            }
        };
        if (characters.size() > 128)
        {
            recordIndeterminate("字符数量超过可靠排序上限128");
            continue;
        }
        search(0, 0.0);
        if (exhausted || ambiguous || bestPath.isEmpty())
        {
            recordIndeterminate("重复字空间排序有歧义或排列搜索超限");
            continue;
        }

        QList<double> distances;
        for (int i = 0; i + 1 < bestPath.size(); ++i)
            distances.append(characters[bestPath[i]].center.distance(characters[bestPath[i + 1]].center));
        const double mean = std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();
        if (!std::isfinite(mean))
        {
            recordIndeterminate("中心距离计算无效");
            continue;
        }

        QStringList abnormalPairs;
        double maxDeviation = 0.0;
        int worstPair = 0;
        for (int i = 0; i < distances.size(); ++i)
        {
            const double deviation = mean > 1e-9 ? std::abs(distances[i] - mean) / mean : 0.0;
            const bool coincident = distances[i] <= 1e-9;
            if (coincident || deviation > m_charSpacingDeviation + 1e-12)
            {
                const auto &first = characters[bestPath[i]];
                const auto &second = characters[bestPath[i + 1]];
                abnormalPairs.append(QString("'%1—%2'(anno_fid=%3,%4)：中心距%5mm，%6")
                    .arg(first.text, second.text).arg(first.fid).arg(second.fid)
                    .arg(distances[i] * 1000.0 / m_mapScale, 0, 'f', 3)
                    .arg(coincident ? QString("中心重合或距离接近零") :
                         QString("偏差%1%").arg(deviation * 100.0, 0, 'f', 2)));
                if (abnormalPairs.size() == 1 || deviation > maxDeviation)
                {
                    maxDeviation = deviation;
                    worstPair = i;
                }
            }
        }
        ++checked;
        if (abnormalPairs.isEmpty())
            continue;
        ++failed;
        const auto &first = characters[bestPath[worstPair]];
        const auto &second = characters[bestPath[worstPair + 1]];
        AnnotationCheckResult result;
        result.errorType = AnnotationErrorType::SizeError;
        result.featureId = QString::number(first.fid);
        result.layerName = matchResultLayer->name();
        result.position = QPointF((first.center.x() + second.center.x()) / 2.0,
                                  (first.center.y() + second.center.y()) / 2.0);
        result.actualValue = abnormalPairs.join("；");
        result.expectedValue = QString("相邻字符中心距相对组内平均值偏差≤%1%，且中心不重合")
            .arg(m_charSpacingDeviation * 100.0, 0, 'f', 2);
        result.errorMessage = QString("【散列注记字距不均】'%1' (group_id=%2)，平均中心距%3mm；%4")
            .arg(name).arg(it.key()).arg(mean * 1000.0 / m_mapScale, 0, 'f', 3).arg(result.actualValue);
        results.append(result);
    }
    const QString summary = QString("已判定%1组（通过%2组，异常%3组），不适用%4组，无法判定%5组，未分组注记%6条；无法判定及未分组记录不代表通过")
        .arg(checked).arg(checked - failed).arg(failed).arg(notApplicable).arg(indeterminate).arg(ungrouped);
    if (statistics)
        statistics->insert("散列注记等间距检查", summary);
    m_logger->info("{}", summary.toStdString());
    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkCharSpacing(
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *gdbLayer,
    QMap<QString, QString> *statistics)
{
    QList<AnnotationCheckResult> results;

    if (!matchResultLayer || !matchResultLayer->isValid())
    {
        m_logger->error("匹配结果图层无效");
        return results;
    }

    if (!gdbLayer || !gdbLayer->isValid())
    {
        m_logger->error("GDB图层无效");
        return results;
    }

    m_logger->info("开始执行字距检查（ID=204）");
    if (statistics)
        statistics->clear();

    // 获取字段索引
    int groupIdIdx = matchResultLayer->fields().indexOf("group_id");
    int annoFidIdx = matchResultLayer->fields().indexOf("anno_fid");
    int gdbFidIdx = matchResultLayer->fields().indexOf("gdb_fid");
    int gdbFidsIdx = matchResultLayer->fields().indexOf("gdb_fids"); // 新增：可能的多fid列表
    int textConteIdx = matchResultLayer->fields().indexOf("text_conte");
    int fontSizeIdx = matchResultLayer->fields().indexOf("font_size");

    if (groupIdIdx < 0 || annoFidIdx < 0 || gdbFidIdx < 0 || textConteIdx < 0 || fontSizeIdx < 0)
    {
        m_logger->error("匹配结果图层缺少必要字段 (group_id, anno_fid, gdb_fid, text_conte, font_size)");
        return results;
    }

    if (m_checkCharEqualSpacing)
    {
        if (matchResultLayer->crs().isGeographic() || matchResultLayer->crs().mapUnits() != QgsUnitTypes::DistanceMeters)
        {
            const QString reason = "散列注记等间距无法判定：输入必须使用以米为单位的投影坐标系";
            m_logger->warn("{}", reason.toStdString());
            if (statistics)
                statistics->insert("散列注记等间距检查", reason);
        }
        else
        {
            results.append(checkCharEqualSpacing(matchResultLayer, statistics));
        }
    }

    // 按 group_id 分组注记
    QMap<qint64, QList<QgsFeature>> groups;
    QgsFeatureIterator it = matchResultLayer->getFeatures();
    QgsFeature feature;

    while (it.nextFeature(feature))
    {
        qint64 groupId = feature.attribute(groupIdIdx).toLongLong();
        QString textConte = feature.attribute(textConteIdx).toString();

        // 只处理单字注记
        if (textConte.length() == 1 && groupId > 0)
        {
            groups[groupId].append(feature);
        }
    }

    m_logger->info("找到 {} 个单字注记组", groups.size());

    // 【修正】将固定的毫米阈值转换为地图单位
    double minSpacingMapUnits = mmToMapUnits(m_minCharSpacingMm);
    double maxSpacingMapUnits = mmToMapUnits(m_maxCharSpacingMm);

    // 检查每个组的字距
    int checkedGroups = 0;
    for (auto groupIt = groups.constBegin(); groupIt != groups.constEnd(); ++groupIt)
    {
        qint64 groupId = groupIt.key();
        const QList<QgsFeature> &annoFeatures = groupIt.value();

        if (annoFeatures.size() < 2)
        {
            continue;
        }

        // 获取GDB要素几何（用于投影计算）
        QgsGeometry gdbGeom;
        bool gotGdbGeom = false;

        if (gdbFidsIdx >= 0)
        {
            QString gdbFidsStr = annoFeatures.first().attribute(gdbFidsIdx).toString().trimmed();
            if (!gdbFidsStr.isEmpty())
            {
                QStringList parts = gdbFidsStr.split(',', Qt::SkipEmptyParts);
                for (QString p : parts)
                {
                    qint64 fid = p.trimmed().toLongLong();
                    if (fid <= 0)
                        continue;
                    QgsFeature partFeat = gdbLayer->getFeature(fid);
                    if (!partFeat.isValid())
                        continue;
                    if (!gotGdbGeom)
                    {
                        gdbGeom = partFeat.geometry();
                        gotGdbGeom = true;
                    }
                    else
                    {
                        // 合并多段构成的要素几何，作为整体代表几何
                        gdbGeom = gdbGeom.combine(partFeat.geometry());
                    }
                }
            }
        }

        // 回退：使用单个 gdb_fid 字段
        if (!gotGdbGeom && gdbFidIdx >= 0)
        {
            qint64 gdbFid = annoFeatures.first().attribute(gdbFidIdx).toLongLong();
            if (gdbFid > 0)
            {
                QgsFeature gdbFeature = gdbLayer->getFeature(gdbFid);
                if (gdbFeature.isValid())
                {
                    gdbGeom = gdbFeature.geometry();
                    gotGdbGeom = true;
                }
            }
        }

        if (!gotGdbGeom || gdbGeom.isEmpty())
        {
            m_logger->warn("无法获取 GDB 代表几何 (group_id={})，跳过该注记组", groupId);
            continue;
        }

        struct ProjectedChar
        {
            qint64 annoFid;
            QString text;
            QgsPointXY position;
            double fontSize;    // 点单位
            double charWidthMU; // 字符宽度（地图单位）
            double distanceAlong;
        };

        QList<ProjectedChar> projectedChars;

        // 【新增】字符宽度估算常量（中文字符宽高比约为 1:1）
        const double CHAR_ASPECT_RATIO = 1.0;

        for (const QgsFeature &annoFeature : annoFeatures)
        {
            const QgsGeometry centroid = annoFeature.geometry().centroid();
            if (centroid.isNull() || centroid.isEmpty())
                continue;
            QgsPointXY annoCenter = centroid.asPoint();
            if (!std::isfinite(annoCenter.x()) || !std::isfinite(annoCenter.y()))
                continue;
            double distAlong = 0.0;

            if (gdbGeom.type() == QgsWkbTypes::LineGeometry)
            {
                distAlong = gdbGeom.lineLocatePoint(QgsGeometry::fromPointXY(annoCenter));
            }
            else if (gdbGeom.type() == QgsWkbTypes::PolygonGeometry)
            {
                QgsPolygonXY poly = gdbGeom.asPolygon();
                if (!poly.isEmpty())
                {
                    QgsGeometry exterior = QgsGeometry::fromPolylineXY(poly[0]);
                    if (!exterior.isNull())
                    {
                        distAlong = exterior.lineLocatePoint(QgsGeometry::fromPointXY(annoCenter));
                    }
                }
            }
            else
            {
                continue;
            }

            ProjectedChar pc;
            pc.annoFid = annoFeature.attribute(annoFidIdx).toLongLong();
            pc.text = annoFeature.attribute(textConteIdx).toString();
            pc.position = annoCenter;
            pc.fontSize = annoFeature.attribute(fontSizeIdx).toDouble();

            // 【关键修正】计算字符宽度（地图单位）
            double fontSizeMm = pc.fontSize * POINT_TO_MM;
            double charWidthMm = fontSizeMm * CHAR_ASPECT_RATIO;
            pc.charWidthMU = mmToMapUnits(charWidthMm);

            pc.distanceAlong = distAlong;
            projectedChars.append(pc);
        }

        // std::sort(projectedChars.begin(), projectedChars.end(),
        //           [](const ProjectedChar &a, const ProjectedChar &b)
        //           {
        //               return a.distanceAlong < b.distanceAlong;
        //           });

        if (projectedChars.size() < 2)
            continue;

        // 使用注记自身的主方向轴（PCA）作为排序与测距轴，避免合并几何的分支干扰
        // 1) 计算质心
        double cx = 0.0, cy = 0.0;
        for (const auto &pc : projectedChars)
        {
            cx += pc.position.x();
            cy += pc.position.y();
        }
        cx /= projectedChars.size();
        cy /= projectedChars.size();
        // 2) 协方差矩阵
        double Sxx = 0.0, Syy = 0.0, Sxy = 0.0;
        for (const auto &pc : projectedChars)
        {
            double dx = pc.position.x() - cx;
            double dy = pc.position.y() - cy;
            Sxx += dx * dx;
            Syy += dy * dy;
            Sxy += dx * dy;
        }
        // 3) 主轴角度 theta（特征向量），参考二维PCA解析式
        double theta = 0.0;
        if (std::abs(Sxy) > 1e-12 || std::abs(Sxx - Syy) > 1e-12)
        {
            theta = 0.5 * std::atan2(2.0 * Sxy, (Sxx - Syy));
        } // 否则保持 0（水平轴）
        const double ux = std::cos(theta);
        const double uy = std::sin(theta);
        // 4) 以主轴为方向计算每个字符的轴向投影作为 distanceAlong
        for (auto &pc : projectedChars)
        {
            double dx = pc.position.x() - cx;
            double dy = pc.position.y() - cy;
            pc.distanceAlong = dx * ux + dy * uy; // 轴向标量投影
        }
        // 5) 按轴向投影排序
        std::sort(projectedChars.begin(), projectedChars.end(),
                  [](const ProjectedChar &a, const ProjectedChar &b)
                  {
                      return a.distanceAlong < b.distanceAlong;
                  });

        for (int i = 0; i < projectedChars.size() - 1; ++i)
        {
            const ProjectedChar &char1 = projectedChars[i];
            const ProjectedChar &char2 = projectedChars[i + 1];

            // 使用两点间直线距离作为字符中心距，然后减去两个字符半宽，得到字符边缘间距
            double centerDistance = char1.position.distance(char2.position);
            double actualCharSpacing = centerDistance - (char1.charWidthMU / 2.0) - (char2.charWidthMU / 2.0);

            // 防止出现负值（可能是字符重叠或测量误差）
            if (actualCharSpacing < 0)
            {
                actualCharSpacing = 0;
            }

            double actualCharSpacingMm = actualCharSpacing * 1000.0 / m_mapScale;

            QString combinedText;
            for (const auto &pc : projectedChars)
            {
                combinedText += pc.text;
            }

            if (actualCharSpacing < minSpacingMapUnits)
            {
                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::SizeError;
                result.featureId = QString::number(char1.annoFid);
                result.actualValue = QString::number(actualCharSpacingMm, 'f', 2) + "mm";
                result.expectedValue = QString("≥%1mm").arg(m_minCharSpacingMm);
                result.errorMessage = QString("【字距过小】注记组 '%1' (group_id=%2) 中字符 '%3' 与 '%4' 间距 %5 < 最小间距 %6")
                                          .arg(combinedText)
                                          .arg(groupId)
                                          .arg(char1.text)
                                          .arg(char2.text)
                                          .arg(result.actualValue)
                                          .arg(result.expectedValue);
                result.position = QPointF((char1.position.x() + char2.position.x()) / 2.0,
                                          (char1.position.y() + char2.position.y()) / 2.0);
                result.layerName = matchResultLayer->name();
                results.append(result);
            }
            else if (actualCharSpacing > maxSpacingMapUnits)
            {
                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::SizeError;
                result.featureId = QString::number(char1.annoFid);
                result.actualValue = QString::number(actualCharSpacingMm, 'f', 2) + "mm";
                result.expectedValue = QString("≤%1mm").arg(m_maxCharSpacingMm);
                result.errorMessage = QString("【字距过大】注记组 '%1' (group_id=%2) 中字符 '%3' 与 '%4' 间距 %5 > 最大间距 %6")
                                          .arg(combinedText)
                                          .arg(groupId)
                                          .arg(char1.text)
                                          .arg(char2.text)
                                          .arg(result.actualValue)
                                          .arg(result.expectedValue);
                result.position = QPointF((char1.position.x() + char2.position.x()) / 2.0,
                                          (char1.position.y() + char2.position.y()) / 2.0);
                result.layerName = matchResultLayer->name();
                results.append(result);
            }
        }

        checkedGroups++;
    }

    m_logger->info("字距检查完成，检查了 {} 个注记组，发现 {} 个问题", checkedGroups, results.size());
    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkGroupSpacing(
    QgsVectorLayer *matchResultLayer,
    QMap<QString, QString> &statistics)
{
    QList<AnnotationCheckResult> results;
    statistics.clear();

    if (!matchResultLayer || !matchResultLayer->isValid())
    {
        m_logger->error("匹配结果图层无效");
        return results;
    }

    m_logger->info("开始执行注记组距检查（ID=207）");

    // 获取字段索引
    int groupIdIdx = matchResultLayer->fields().indexOf("group_id");
    int gdbFidsIdx = matchResultLayer->fields().indexOf("gdb_fids");
    int assocNameIdx = matchResultLayer->fields().indexOf("assoc_name");
    int annoFidIdx = matchResultLayer->fields().indexOf("anno_fid");

    if (groupIdIdx < 0 || gdbFidsIdx < 0 || assocNameIdx < 0 || annoFidIdx < 0)
    {
        m_logger->error("匹配结果图层缺少必要字段 (group_id, gdb_fids, assoc_name, anno_fid)");
        return results;
    }

    // 结构：存储每个组的信息
    struct GroupInfo
    {
        qint64 groupId;
        QString gdbFids;        // 逗号分隔的fid列表
        QString assocName;      // 源名称
        QgsPointXY center;      // 组的中心点
        QList<qint64> annoFids; // 组内注记FID列表
    };

    // 按 group_id 收集组信息
    QMap<qint64, GroupInfo> groups;
    QgsFeatureIterator it = matchResultLayer->getFeatures();
    QgsFeature feature;

    while (it.nextFeature(feature))
    {
        qint64 groupId = feature.attribute(groupIdIdx).toLongLong();
        if (groupId <= 0)
            continue;

        QString gdbFids = feature.attribute(gdbFidsIdx).toString().trimmed();
        QString assocName = feature.attribute(assocNameIdx).toString();
        qint64 annoFid = feature.attribute(annoFidIdx).toLongLong();
        QgsPointXY pos = feature.geometry().centroid().asPoint();

        if (!groups.contains(groupId))
        {
            GroupInfo info;
            info.groupId = groupId;
            info.gdbFids = gdbFids;
            info.assocName = assocName;
            info.center = pos;
            info.annoFids.append(annoFid);
            groups[groupId] = info;
        }
        else
        {
            // 更新中心点（平均值）
            GroupInfo &info = groups[groupId];
            double newX = (info.center.x() * info.annoFids.size() + pos.x()) / (info.annoFids.size() + 1);
            double newY = (info.center.y() * info.annoFids.size() + pos.y()) / (info.annoFids.size() + 1);
            info.center = QgsPointXY(newX, newY);
            info.annoFids.append(annoFid);
        }
    }

    m_logger->info("找到 {} 个注记组", groups.size());

    // 按 gdbFids 分组（同一段要素）
    QMap<QString, QList<GroupInfo>> segmentGroups;
    for (const GroupInfo &info : groups.values())
    {
        if (!info.gdbFids.isEmpty())
        {
            segmentGroups[info.gdbFids].append(info);
        }
    }

    // 按 assocName 分组（同一名称的所有段）
    QMap<QString, QMap<QString, QList<GroupInfo>>> nameSegments;
    for (auto it = segmentGroups.constBegin(); it != segmentGroups.constEnd(); ++it)
    {
        QString gdbFids = it.key();
        const QList<GroupInfo> &groupList = it.value();

        if (!groupList.isEmpty())
        {
            QString assocName = groupList.first().assocName;
            nameSegments[assocName][gdbFids] = groupList;
        }
    }

    // 生成统计信息
    m_logger->info("=== 注记组分布统计 ===");
    for (auto nameIt = nameSegments.constBegin(); nameIt != nameSegments.constEnd(); ++nameIt)
    {
        QString name = nameIt.key();
        const QMap<QString, QList<GroupInfo>> &segments = nameIt.value();

        int totalGroups = 0;
        for (const QList<GroupInfo> &groupList : segments.values())
        {
            totalGroups += groupList.size();
        }

        // 只统计组数>=2的
        if (totalGroups >= 2)
        {
            QString statKey = QString("河流/道路[%1]").arg(name);
            QString statValue = QString("共%1段,总计%2组注记")
                                    .arg(segments.size())
                                    .arg(totalGroups);

            // 每段详情
            QStringList segmentDetails;
            int segIdx = 1;
            for (auto segIt = segments.constBegin(); segIt != segments.constEnd(); ++segIt)
            {
                const QList<GroupInfo> &groupList = segIt.value();
                if (groupList.size() >= 1)
                {
                    segmentDetails.append(QString("第%1段有%2组").arg(segIdx).arg(groupList.size()));
                }
                segIdx++;
            }

            if (!segmentDetails.isEmpty())
            {
                statValue += " (" + segmentDetails.join(", ") + ")";
            }

            statistics[statKey] = statValue;
            m_logger->info("  {}: {}", statKey.toStdString(), statValue.toStdString());
        }
    }

    // 转换组距阈值到地图单位
    double minSpacingMapUnits = mmToMapUnits(m_minGroupSpacingMm);
    double maxSpacingMapUnits = mmToMapUnits(m_maxGroupSpacingMm);

    // 检查每个段内的组距
    int checkedPairs = 0;
    for (auto segIt = segmentGroups.constBegin(); segIt != segmentGroups.constEnd(); ++segIt)
    {
        const QList<GroupInfo> &groupList = segIt.value();

        // 至少需要2组才能检查组距
        if (groupList.size() < 2)
            continue;

        // 两两检查相邻组的间距
        for (int i = 0; i < groupList.size() - 1; ++i)
        {
            for (int j = i + 1; j < groupList.size(); ++j)
            {
                const GroupInfo &group1 = groupList[i];
                const GroupInfo &group2 = groupList[j];

                double distance = group1.center.distance(group2.center);
                double distanceMm = distance * 1000.0 / m_mapScale;

                QString featureName = group1.assocName;

                if (distance < minSpacingMapUnits)
                {
                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::SizeError;
                    qint64 reprAnnoFid = group1.annoFids.isEmpty() ? 0 : group1.annoFids.first();
                    result.featureId = QString::number(reprAnnoFid);
                    result.actualValue = QString::number(distanceMm, 'f', 2) + "mm";
                    result.expectedValue = QString("≥%1mm").arg(m_minGroupSpacingMm);
                    result.errorMessage = QString("【组距过小】'%1' 的注记组 %2 与组 %3 间距 %4 < 最小组距 %5")
                                              .arg(featureName)
                                              .arg(group1.groupId)
                                              .arg(group2.groupId)
                                              .arg(result.actualValue)
                                              .arg(result.expectedValue);
                    result.position = QPointF((group1.center.x() + group2.center.x()) / 2.0,
                                              (group1.center.y() + group2.center.y()) / 2.0);
                    result.layerName = matchResultLayer->name();
                    results.append(result);
                }
                else if (distance > maxSpacingMapUnits)
                {
                    AnnotationCheckResult result;
                    result.errorType = AnnotationErrorType::SizeError;
                    qint64 reprAnnoFid = group1.annoFids.isEmpty() ? 0 : group1.annoFids.first();
                    result.featureId = QString::number(reprAnnoFid);
                    result.actualValue = QString::number(distanceMm, 'f', 2) + "mm";
                    result.expectedValue = QString("≤%1mm").arg(m_maxGroupSpacingMm);
                    result.errorMessage = QString("【组距过大】'%1' 的注记组 %2 与组 %3 间距 %4 > 最大组距 %5")
                                              .arg(featureName)
                                              .arg(group1.groupId)
                                              .arg(group2.groupId)
                                              .arg(result.actualValue)
                                              .arg(result.expectedValue);
                    result.position = QPointF((group1.center.x() + group2.center.x()) / 2.0,
                                              (group1.center.y() + group2.center.y()) / 2.0);
                    result.layerName = matchResultLayer->name();
                    results.append(result);
                }

                checkedPairs++;
            }
        }
    }

    m_logger->info("注记组距检查完成，检查了 {} 对组，发现 {} 个问题", checkedPairs, results.size());
    return results;
}

QList<AnnotationCheckResult> SE_AnnotationAdvancedAlgorithm::checkEqualSpacing(
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *gdbLayer)
{
    QList<AnnotationCheckResult> results;

    if (!matchResultLayer || !matchResultLayer->isValid())
    {
        m_logger->error("匹配结果图层无效");
        return results;
    }

    if (!gdbLayer || !gdbLayer->isValid())
    {
        m_logger->error("GDB图层无效");
        return results;
    }

    m_logger->info("开始执行面要素等间距标注检查（ID=208）");

    // 获取字段索引
    int textConteIdx = matchResultLayer->fields().indexOf("text_conte");
    int groupIdIdx = matchResultLayer->fields().indexOf("group_id");
    int gdbFidsIdx = matchResultLayer->fields().indexOf("gdb_fids");
    int gdbFidIdx = matchResultLayer->fields().indexOf("gdb_fid");
    int annoFidIdx = matchResultLayer->fields().indexOf("anno_fid");
    int assocNameIdx = matchResultLayer->fields().indexOf("assoc_name");

    if (groupIdIdx < 0 || annoFidIdx < 0 || assocNameIdx < 0)
    {
        m_logger->error("匹配结果图层缺少必要字段");
        return results;
    }

    // 结构：存储每个注记组的信息
    struct GroupAnnotation
    {
        qint64 groupId;
        qint64 annoFid;
        QString gdbFids;
        QString assocName;
        QString textConte;
        QgsPointXY position;
    };

    // 按 gdb_fids 分组注记
    QMap<QString, QList<GroupAnnotation>> gdbFidsGroups;
    QgsFeatureIterator it = matchResultLayer->getFeatures();
    QgsFeature feature;

    while (it.nextFeature(feature))
    {
        qint64 groupId = feature.attribute(groupIdIdx).toLongLong();
        if (groupId <= 0)
            continue;

        QString gdbFids;
        if (gdbFidsIdx >= 0)
        {
            gdbFids = feature.attribute(gdbFidsIdx).toString().trimmed();
        }
        if (gdbFids.isEmpty() && gdbFidIdx >= 0)
        {
            gdbFids = feature.attribute(gdbFidIdx).toString();
        }
        if (gdbFids.isEmpty())
            continue;

        GroupAnnotation anno;
        anno.groupId = groupId;
        anno.annoFid = feature.attribute(annoFidIdx).toLongLong();
        anno.gdbFids = gdbFids;
        anno.textConte = textConteIdx >= 0 ? feature.attribute(textConteIdx).toString() : "";
        anno.assocName = assocNameIdx >= 0 ? feature.attribute(assocNameIdx).toString() : "";
        anno.position = feature.geometry().centroid().asPoint();

        gdbFidsGroups[gdbFids].append(anno);
    }

    m_logger->info("找到 {} 个要素的注记组", gdbFidsGroups.size());

    // 对每个要素检查其注记分布
    int checkedFeatures = 0;
    for (auto gdbIt = gdbFidsGroups.constBegin(); gdbIt != gdbFidsGroups.constEnd(); ++gdbIt)
    {
        QString gdbFids = gdbIt.key();
        const QList<GroupAnnotation> &annotations = gdbIt.value();

        // 过滤：注记数量少于阈值则跳过
        if (annotations.size() < m_minAnnotationCount)
        {
            continue;
        }

        // 获取GDB要素几何并合并
        QgsGeometry gdbGeom;
        bool gotGeom = false;

        QStringList fidList = gdbFids.split(',', Qt::SkipEmptyParts);
        for (const QString &fidStr : fidList)
        {
            qint64 fid = fidStr.trimmed().toLongLong();
            if (fid <= 0)
                continue;

            QgsFeature gdbFeature = gdbLayer->getFeature(fid);
            if (!gdbFeature.isValid())
                continue;

            if (!gotGeom)
            {
                gdbGeom = gdbFeature.geometry();
                gotGeom = true;
            }
            else
            {
                gdbGeom = gdbGeom.combine(gdbFeature.geometry());
            }
        }

        if (!gotGeom || gdbGeom.isEmpty() || gdbGeom.type() != QgsWkbTypes::PolygonGeometry)
        {
            m_logger->warn("无法获取面要素几何 gdb_fids={}", gdbFids.toStdString());
            continue;
        }

        // 计算面要素的最小外接矩形（OMBB）
        QgsGeometry ombb = gdbGeom.orientedMinimumBoundingBox();
        if (ombb.isEmpty())
        {
            m_logger->warn("无法计算要素的最小外接矩形 gdb_fids={}", gdbFids.toStdString());
            continue;
        }

        // 提取OMBB的长轴方向
        QgsPolygonXY ombbPoly = ombb.asPolygon();
        if (ombbPoly.isEmpty() || ombbPoly[0].size() != 5)
        {
            continue;
        }

        QgsPointXY p0 = ombbPoly[0][0];
        QgsPointXY p1 = ombbPoly[0][1];
        QgsPointXY p2 = ombbPoly[0][2];

        double len1 = p0.distance(p1);
        double len2 = p1.distance(p2);

        // 确定长轴
        QgsVector longAxis;
        if (len1 >= len2)
        {
            longAxis = QgsVector(p1.x() - p0.x(), p1.y() - p0.y());
        }
        else
        {
            longAxis = QgsVector(p2.x() - p1.x(), p2.y() - p1.y());
        }
        longAxis = longAxis.normalized();

        // 计算OMBB中心点作为参考原点
        QgsPointXY ombbCenter = ombb.centroid().asPoint();

        // 将所有注记投影到长轴上
        struct ProjectedAnnotation
        {
            qint64 groupId;
            qint64 annoFid;
            double projection; // 在长轴上的投影值
            QgsPointXY position;
            QString textConte;
        };

        QList<ProjectedAnnotation> projectedAnnos;
        for (const GroupAnnotation &anno : annotations)
        {
            ProjectedAnnotation pa;
            pa.groupId = anno.groupId;
            pa.annoFid = anno.annoFid;
            pa.position = anno.position;
            pa.textConte = anno.textConte;

            // 计算相对于中心的向量
            QgsVector v(anno.position.x() - ombbCenter.x(),
                        anno.position.y() - ombbCenter.y());
            // 投影到长轴
            pa.projection = v.x() * longAxis.x() + v.y() * longAxis.y();

            projectedAnnos.append(pa);
        }

        // 按投影值排序
        std::sort(projectedAnnos.begin(), projectedAnnos.end(),
                  [](const ProjectedAnnotation &a, const ProjectedAnnotation &b)
                  {
                      return a.projection < b.projection;
                  });

        // 计算相邻间距
        QList<double> spacings;
        for (int i = 0; i < projectedAnnos.size() - 1; ++i)
        {
            double spacing = projectedAnnos[i + 1].projection - projectedAnnos[i].projection;
            spacings.append(spacing);
        }

        if (spacings.isEmpty())
        {
            continue;
        }

        // 计算平均间距
        double avgSpacing = std::accumulate(spacings.begin(), spacings.end(), 0.0) / spacings.size();
        double minAllowedSpacing = avgSpacing * (1.0 - m_allowedDeviation);
        double maxAllowedSpacing = avgSpacing * (1.0 + m_allowedDeviation);

        // 检查每个间距是否在允许范围内
        QString featureName = annotations.first().assocName;
        for (int i = 0; i < spacings.size(); ++i)
        {
            double spacing = spacings[i];
            double deviationRatio = std::abs(spacing - avgSpacing) / avgSpacing;

            if (spacing < minAllowedSpacing || spacing > maxAllowedSpacing)
            {
                const ProjectedAnnotation &anno1 = projectedAnnos[i];
                const ProjectedAnnotation &anno2 = projectedAnnos[i + 1];

                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::SizeError;
                result.featureId = QString::number(anno1.annoFid);
                result.actualValue = QString::number(deviationRatio * 100.0, 'f', 1) + "%";
                result.expectedValue = QString("±%1%").arg(m_allowedDeviation * 100.0, 0, 'f', 1);
                // 更清晰地同时输出 group_id 与代表性 anno_fid，便于定位
                result.errorMessage = QString("【间距不均】'%1' 注记组 %2 , %3 与 %4 ,间距偏差 %5 超出允许范围 %6")
                                          .arg(featureName)
                                          .arg(anno1.groupId)
                                          .arg(anno1.textConte)
                                          .arg(anno2.textConte)
                                          .arg(result.actualValue)
                                          .arg(result.expectedValue);
                result.position = QPointF((anno1.position.x() + anno2.position.x()) / 2.0,
                                          (anno1.position.y() + anno2.position.y()) / 2.0);
                result.layerName = matchResultLayer->name();
                results.append(result);
            }
        }

        checkedFeatures++;
    }

    m_logger->info("等间距标注检查完成，检查了 {} 个面要素，发现 {} 个问题",
                   checkedFeatures, results.size());
    return results;
}