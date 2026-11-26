#include "se_symbol_direction_algorithm.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsSpatialIndex.h>
#include <QgsPointXY.h>
#include <QgsDistanceArea.h>
#include <cmath>
#include <QgsFeatureRequest.h>
#include <QgsRectangle.h>
#include <QgsWkbTypes.h>
#include <limits>
#include <algorithm> // for std::max/min

SE_SymbolDirectionAlgorithm::SE_SymbolDirectionAlgorithm(std::shared_ptr<spdlog::logger> logger)
    : m_logger(logger)
{
}

SE_SymbolDirectionAlgorithm::~SE_SymbolDirectionAlgorithm()
{
}

bool SE_SymbolDirectionAlgorithm::loadDirectionCheckRules(
    const QString &schemaPath,
    double mapScale,
    DirectionCheckRule &rule)
{
    m_mapScale = mapScale;

    QFile file(schemaPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        m_logger->error("无法打开规则文件: {}", schemaPath.toUtf8().constData());
        return false;
    }

    QXmlStreamReader xml(&file);
    bool matched = false;

    while (!xml.atEnd() && !xml.hasError())
    {
        auto token = xml.readNext();

        if (token == QXmlStreamReader::StartElement && xml.name() == "check_item")
        {
            QMap<QString, QString> itemRules;
            QString itemId;

            while (!(xml.isEndElement() && xml.name() == "check_item") && !xml.atEnd())
            {
                xml.readNext();
                if (xml.isStartElement())
                {
                    if (xml.name() == "id")
                    {
                        itemId = xml.readElementText().trimmed();
                    }
                    else if (xml.name() == "rule")
                    {
                        QString rName, rVal;
                        while (!(xml.isEndElement() && xml.name() == "rule") && !xml.atEnd())
                        {
                            xml.readNext();
                            if (xml.isStartElement())
                            {
                                if (xml.name() == "name")
                                    rName = xml.readElementText().trimmed();
                                else if (xml.name() == "value")
                                    rVal = xml.readElementText().trimmed();
                            }
                        }
                        if (!rName.isEmpty())
                            itemRules[rName] = rVal;
                    }
                }
            }

            if (itemId == "209")
            {
                matched = true;

                if (itemRules.contains("符号类型"))
                    rule.symbolType = itemRules["符号类型"];

                if (itemRules.contains("关联图层"))
                    rule.associatedLayers = itemRules["关联图层"].split(';', Qt::SkipEmptyParts);

                const QString chk = itemRules.value("检查类型");
                if (chk == "垂直")
                    rule.checkType = DirectionCheckType::PERPENDICULAR;
                else if (chk == "平行")
                    rule.checkType = DirectionCheckType::PARALLEL;
                else if (chk == "方向一致")
                    rule.checkType = DirectionCheckType::DIRECTION_MATCH;

                if (itemRules.contains("搜索半径"))
                    rule.searchRadius = mmToMapUnits(itemRules["搜索半径"].toDouble());

                if (itemRules.contains("角度阈值"))
                    rule.angleThreshold = itemRules["角度阈值"].toDouble();

                break;
            }
        }
    }

    file.close();

    if (!matched)
    {
        m_logger->error("未找到ID=209的符号方向检查规则");
        return false;
    }

    m_logger->info("加载规则成功: 符号类型='{}', 检查类型={}, 搜索半径(MU)={}, 角度阈值={}度",
                   rule.symbolType.toStdString(),
                   static_cast<int>(rule.checkType),
                   rule.searchRadius,
                   rule.angleThreshold);
    return true;
}

// ==================== 辅助函数实现 ====================
double SE_SymbolDirectionAlgorithm::calculateGeometryDirection(const QgsGeometry &geometry)
{
    if (geometry.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();

    QgsWkbTypes::Type flatType = QgsWkbTypes::flatType(geometry.wkbType());

    if (flatType == QgsWkbTypes::LineString ||
        flatType == QgsWkbTypes::MultiLineString ||
        flatType == QgsWkbTypes::Polygon ||
        flatType == QgsWkbTypes::MultiPolygon)
    {
        QgsGeometry ombb = geometry.orientedMinimumBoundingBox();
        if (ombb.isEmpty())
            return std::numeric_limits<double>::quiet_NaN();

        QgsPolygonXY ombbPoly = ombb.asPolygon();
        if (ombbPoly.isEmpty() || ombbPoly.front().size() < 4)
            return std::numeric_limits<double>::quiet_NaN();

        QgsPointXY p0 = ombbPoly.front()[0];
        QgsPointXY p1 = ombbPoly.front()[1];
        QgsPointXY p2 = ombbPoly.front()[2];

        double len1 = p0.distance(p1);
        double len2 = p1.distance(p2);
        double longerSide = std::max(len1, len2);
        double shorterSide = std::min(len1, len2);

        if (longerSide <= std::numeric_limits<double>::epsilon())
            return std::numeric_limits<double>::quiet_NaN();

        if ((flatType == QgsWkbTypes::Polygon || flatType == QgsWkbTypes::MultiPolygon) &&
            (shorterSide <= std::numeric_limits<double>::epsilon() || longerSide / shorterSide < 1.15))
        {
            return std::numeric_limits<double>::quiet_NaN();
        }

        QgsPointXY start, end;
        if (len1 >= len2)
        {
            start = p0;
            end = p1;
        }
        else
        {
            start = p1;
            end = p2;
        }

        double dx = end.x() - start.x();
        double dy = end.y() - start.y();
        double angle = std::atan2(dy, dx) * 180.0 / M_PI;

        if (angle < 0.0)
            angle += 180.0;
        if (angle >= 180.0)
            angle = fmod(angle, 180.0);
        return angle;
    }

    return std::numeric_limits<double>::quiet_NaN();
}

// 【最终修正版】专门用于计算“泉”符号方向的函数
// 策略：一次遍历找到头/尾要素，然后遍历尾巴所有顶点找到最远点
static double calculateSpringDirection(
    QgsVectorLayer *symbolLayer,
    const QString &symbolId,
    const QString &symbolIdField,
    std::shared_ptr<spdlog::logger> logger)
{
    // 1. 获取所需字段的索引
    int symbolIdIdx = symbolLayer->fields().indexOf(symbolIdField);
    int widthIdx = symbolLayer->fields().indexOf("width");
    int heightIdx = symbolLayer->fields().indexOf("height");

    if (symbolIdIdx < 0 || widthIdx < 0 || heightIdx < 0)
    {
        logger->warn("泉符号图层缺少 'symbol_id', 'width' 或 'height' 字段，无法精确判断方向。");
        return 0.0;
    }

    QgsFeature headFeature;
    QgsFeature tailFeature;
    double maxAspectRatio = -1.0;
    double minAspectRatio = std::numeric_limits<double>::max();

    // 2. 一次遍历，同时记录具有最大和最小宽高比的要素
    QgsFeatureIterator it = symbolLayer->getFeatures(QgsFeatureRequest().setFilterExpression(
        QString("\"%1\" = '%2'").arg(symbolIdField, symbolId)));
    QgsFeature currentFeature;
    while (it.nextFeature(currentFeature))
    {
        bool ok_w, ok_h;
        double width = currentFeature.attribute(widthIdx).toDouble(&ok_w);
        double height = currentFeature.attribute(heightIdx).toDouble(&ok_h);

        if (!ok_w || !ok_h)
            continue;

        double max_dim = std::max(width, height);
        double min_dim = std::min(width, height);

        if (min_dim < 1e-6)
            continue;

        double aspectRatio = max_dim / min_dim;

        // 记录宽高比最大的要素（尾巴）
        if (aspectRatio > maxAspectRatio)
        {
            maxAspectRatio = aspectRatio;
            tailFeature = currentFeature;
        }

        // 记录宽高比最小的要素（头/圆）
        if (aspectRatio < minAspectRatio)
        {
            minAspectRatio = aspectRatio;
            headFeature = currentFeature;
        }
    }

    // 3. 必须成功找到头和尾，并且它们必须是不同的要素
    if (!headFeature.isValid() || !tailFeature.isValid() || headFeature.id() == tailFeature.id())
    {
        logger->warn("未能为泉符号 {} 准确识别出独立的头部和尾部要素。", symbolId.toStdString());
        return 0.0;
    }

    // 4. 获取头部的中心点
    QgsGeometry headGeom = headFeature.geometry();
    QgsPointXY headCenter = headGeom.centroid().asPoint();

    // 5. 遍历尾巴几何的所有顶点，找到距离圆心最远的点
    QgsGeometry tailGeom = tailFeature.geometry();
    const QgsAbstractGeometry *tailAbsGeom = tailGeom.constGet();
    if (!tailAbsGeom)
        return 0.0;

    QgsPointXY tailTipPoint;
    double maxDistFromHead = -1.0;

    for (int i = 0; i < tailAbsGeom->vertexCount(); ++i)
    {
        QgsPoint vertex = tailAbsGeom->vertexAt(QgsVertexId(0, 0, i));
        double dist = headCenter.distance(vertex);
        if (dist > maxDistFromHead)
        {
            maxDistFromHead = dist;
            tailTipPoint = vertex;
        }
    }

    if (maxDistFromHead < 0.0)
    {
        logger->warn("未能为泉符号 {} 的尾巴找到有效顶点。", symbolId.toStdString());
        return 0.0;
    }

    // 6. 构建从圆心指向尾巴最远端的向量，并计算其方向角 (0-360度)
    double dx = tailTipPoint.x() - headCenter.x();
    double dy = tailTipPoint.y() - headCenter.y();
    double angle = std::atan2(dy, dx) * 180.0 / M_PI;

    // 将角度归一化到 [0, 360) 范围
    if (angle < 0.0)
    {
        angle += 360.0;
    }

    return angle;
}

static bool collectCandidateLines(const QgsGeometry &geom, QList<QgsPolylineXY> &lines)
{
    lines.clear();
    if (geom.isEmpty())
        return false;

    const auto gtype = geom.type();
    if (gtype == QgsWkbTypes::LineGeometry)
    {
        QgsPolylineXY pl = geom.asPolyline();
        if (pl.size() >= 2)
            lines.append(pl);
        else
        {
            const QgsMultiPolylineXY mpl = geom.asMultiPolyline();
            for (const auto &l : mpl)
                if (l.size() >= 2)
                    lines.append(l);
        }
        return !lines.isEmpty();
    }
    else if (gtype == QgsWkbTypes::PolygonGeometry)
    {
        QgsPolygonXY poly = geom.asPolygon();
        if (!poly.isEmpty() && poly.front().size() >= 2)
        {
            lines.append(poly.front());
            return true;
        }
        const QgsMultiPolygonXY mpoly = geom.asMultiPolygon();
        for (const auto &p : mpoly)
        {
            if (!p.isEmpty() && p.front().size() >= 2)
                lines.append(p.front());
        }
        return !lines.isEmpty();
    }
    return false;
}

static double computeClosestSegmentDirection(const QgsPointXY &pt, const QList<QgsPolylineXY> &lines)
{
    double bestDist = std::numeric_limits<double>::max();
    QgsPointXY bestA, bestB;

    for (const auto &line : lines)
    {
        for (int i = 0; i < line.size() - 1; ++i)
        {
            const QgsPointXY &a = line[i];
            const QgsPointXY &b = line[i + 1];
            double d = QgsGeometry::fromPolylineXY({a, b}).distance(QgsGeometry::fromPointXY(pt));
            if (d < bestDist)
            {
                bestDist = d;
                bestA = a;
                bestB = b;
            }
        }
    }
    if (bestDist == std::numeric_limits<double>::max())
        return 0.0;

    double dx = bestB.x() - bestA.x();
    double dy = bestB.y() - bestA.y();
    double angle = std::atan2(dy, dx) * 180.0 / M_PI;
    if (angle < 0)
        angle += 180.0;
    if (angle >= 180.0)
        angle = fmod(angle, 180.0);
    return angle;
}

static double localTangentDirectionAt(const QgsGeometry &geom, const QgsPointXY &refPt)
{
    QList<QgsPolylineXY> lines;
    if (!collectCandidateLines(geom, lines))
        return 0.0;
    return computeClosestSegmentDirection(refPt, lines);
}

static bool computeConnectingPoints(const QgsGeometry &a, const QgsGeometry &b,
                                    QgsPointXY &pa, QgsPointXY &pb)
{
    QgsGeometry sl = a.shortestLine(b);
    QgsPolylineXY pl = sl.asPolyline();
    if (pl.size() >= 2)
    {
        pa = pl.front();
        pb = pl.back();
        return true;
    }
    pa = a.nearestPoint(b).asPoint();
    pb = b.nearestPoint(a).asPoint();
    return true;
}

double SE_SymbolDirectionAlgorithm::calculateAngleDifference(double angle1, double angle2)
{
    while (angle1 >= 180.0)
        angle1 -= 180.0;
    while (angle2 >= 180.0)
        angle2 -= 180.0;
    while (angle1 < 0.0)
        angle1 += 180.0;
    while (angle2 < 0.0)
        angle2 += 180.0;

    double diff = std::abs(angle1 - angle2);
    if (diff > 90.0)
        diff = 180.0 - diff;

    return diff;
}

bool SE_SymbolDirectionAlgorithm::isNearPerpendicular(double angleDiff, double threshold)
{
    // angleDiff is already min angle (0-90)
    return std::abs(angleDiff - 90.0) <= threshold;
}

bool SE_SymbolDirectionAlgorithm::isNearParallel(double angleDiff, double threshold)
{
    // angleDiff is already min angle (0-90)
    return angleDiff <= threshold;
}

QgsGeometry SE_SymbolDirectionAlgorithm::findNearestAssociatedFeature(
    const QgsPointXY &symbolCenter,
    const QMap<QString, QgsVectorLayer *> &associatedLayers,
    double searchRadius,
    QString &foundLayerName)
{
    QgsGeometry nearestGeom;
    double minDistance = std::numeric_limits<double>::max();
    foundLayerName.clear();

    QgsRectangle searchRect(
        symbolCenter.x() - searchRadius,
        symbolCenter.y() - searchRadius,
        symbolCenter.x() + searchRadius,
        symbolCenter.y() + searchRadius);

    for (auto it = associatedLayers.constBegin(); it != associatedLayers.constEnd(); ++it)
    {
        QgsVectorLayer *layer = it.value();
        if (!layer || !layer->isValid())
            continue;

        QgsFeatureIterator featIt = layer->getFeatures(QgsFeatureRequest().setFilterRect(searchRect));
        QgsFeature feature;

        while (featIt.nextFeature(feature))
        {
            QgsGeometry geom = feature.geometry();
            if (geom.isEmpty())
                continue;

            double dist = geom.distance(QgsGeometry::fromPointXY(symbolCenter));
            if (dist < minDistance && dist <= searchRadius)
            {
                minDistance = dist;
                nearestGeom = geom;
                foundLayerName = it.key();
            }
        }
    }

    return nearestGeom;
}

QgsPointXY SE_SymbolDirectionAlgorithm::calculateSymbolCenter(
    QgsVectorLayer *symbolLayer,
    const QString &symbolId,
    const QString &symbolIdField)
{
    int symbolIdIdx = symbolLayer->fields().indexOf(symbolIdField);
    if (symbolIdIdx < 0)
        return QgsPointXY();

    QgsGeometry combinedGeom;
    QgsFeatureIterator it = symbolLayer->getFeatures(QgsFeatureRequest().setFilterExpression(
        QString("\"%1\" = '%2'").arg(symbolIdField, symbolId)));
    QgsFeature feature;
    while (it.nextFeature(feature))
    {
        if (combinedGeom.isEmpty())
        {
            combinedGeom = feature.geometry();
        }
        else
        {
            combinedGeom = combinedGeom.combine(feature.geometry());
        }
    }

    if (combinedGeom.isEmpty())
        return QgsPointXY();
    return combinedGeom.centroid().asPoint();
}

QgsGeometry SE_SymbolDirectionAlgorithm::getSymbolRepresentativeGeometry(
    QgsVectorLayer *symbolLayer,
    const QString &symbolId,
    const QString &symbolIdField)
{
    int symbolIdIdx = symbolLayer->fields().indexOf(symbolIdField);
    if (symbolIdIdx < 0)
        return QgsGeometry();

    int elementTyIdx = symbolLayer->fields().indexOf("element_ty");

    QList<QgsGeometry> pathGeoms;
    QList<QgsGeometry> geoms;

    QgsFeatureIterator it = symbolLayer->getFeatures(QgsFeatureRequest().setFilterExpression(
        QString("\"%1\" = '%2'").arg(symbolIdField, symbolId)));
    QgsFeature feature;

    while (it.nextFeature(feature))
    {
        if (elementTyIdx >= 0 && feature.attribute(elementTyIdx).toString() == "path")
        {
            pathGeoms.append(feature.geometry());
        }
        else
        {
            geoms.append(feature.geometry());
        }
    }

    const QList<QgsGeometry> &representativeGeoms = pathGeoms.isEmpty() ? geoms : pathGeoms;
    if (representativeGeoms.isEmpty())
        return QgsGeometry();

    QgsGeometry combined = representativeGeoms.first();
    for (int i = 1; i < representativeGeoms.size(); ++i)
    {
        combined = combined.combine(representativeGeoms[i]);
    }

    return combined;
}

static double getLocalOverallDirection(const QgsGeometry &lineGeom, const QgsPointXY &pointOnLine, double length)
{
    if (lineGeom.isEmpty() || (lineGeom.type() != QgsWkbTypes::LineGeometry) || length <= 0.0)
        return 0.0;

    QList<QgsPolylineXY> lines;
    collectCandidateLines(lineGeom, lines);
    if (lines.isEmpty())
        return 0.0;

    double bestDist = std::numeric_limits<double>::max();
    int bestLineIdx = -1;
    int bestVertexIdx = -1;
    for (int li = 0; li < lines.size(); ++li)
    {
        const QgsPolylineXY &pl = lines[li];
        for (int vi = 0; vi < pl.size(); ++vi)
        {
            double d = pl[vi].distance(pointOnLine);
            if (d < bestDist)
            {
                bestDist = d;
                bestLineIdx = li;
                bestVertexIdx = vi;
            }
        }
    }

    if (bestLineIdx < 0 || bestVertexIdx < 0)
        return 0.0;
    const QgsPolylineXY &pts = lines[bestLineIdx];
    if (pts.size() < 2)
        return 0.0;

    double halfLen = length / 2.0;

    int startIdx = bestVertexIdx;
    double acc = 0.0;
    for (int i = bestVertexIdx; i > 0; --i)
    {
        double seg = pts[i].distance(pts[i - 1]);
        if (acc + seg > halfLen)
            break;
        acc += seg;
        startIdx = i - 1;
    }

    int endIdx = bestVertexIdx;
    acc = 0.0;
    for (int i = bestVertexIdx; i < pts.size() - 1; ++i)
    {
        double seg = pts[i + 1].distance(pts[i]);
        if (acc + seg > halfLen)
            break;
        acc += seg;
        endIdx = i + 1;
    }

    if (startIdx == endIdx)
    {
        if (endIdx < pts.size() - 1)
            endIdx++;
        else if (startIdx > 0)
            startIdx--;
        else
            return 0.0;
    }

    const QgsPointXY &pStart = pts[startIdx];
    const QgsPointXY &pEnd = pts[endIdx];
    if (pStart == pEnd)
        return 0.0;

    double dx = pEnd.x() - pStart.x();
    double dy = pEnd.y() - pStart.y();

    double angle = std::atan2(dy, dx) * 180.0 / M_PI;
    if (angle < 0.0)
        angle += 180.0;
    if (angle >= 180.0)
        angle = fmod(angle, 180.0);
    return angle;
}

// 【新增】辅助函数，专门处理无 symbol_id 的、逐要素检查的情况 (Feature Mode)
QList<AnnotationCheckResult> SE_SymbolDirectionAlgorithm::checkFeaturesDirection(
    QgsVectorLayer *targetLayer,
    const QMap<QString, QgsVectorLayer *> &associatedLayers,
    const DirectionCheckRule &rule,
    std::shared_ptr<spdlog::logger> logger)
{
    QList<AnnotationCheckResult> results;
    if (!targetLayer || !targetLayer->isValid())
    {
        logger->error("Feature Mode: 目标图层无效");
        return results;
    }

    int elementTyIdx = targetLayer->fields().indexOf("element_ty");

    QgsFeatureIterator it = targetLayer->getFeatures();
    QgsFeature feature;
    int featureCount = 0;
    while (it.nextFeature(feature))
    {
        // 新增：过滤 element_ty = text 的要素
        if (elementTyIdx >= 0 && feature.attribute(elementTyIdx).toString() == "text")
            continue;

        featureCount++;
        QgsGeometry featureGeom = feature.geometry();
        if (featureGeom.isEmpty())
            continue;

        QgsPointXY featureCenter = featureGeom.centroid().asPoint();

        // 计算当前要素的方向
        // 对于渠流向(箭杆)和道路代码(外框)，通用的OMBB方法都适用
        double featureDirection = calculateGeometryDirection(featureGeom);
        if (!std::isfinite(featureDirection))
        {
            logger->debug("要素 {} 的方向不可判定，跳过方向检查", static_cast<long long>(feature.id()));
            continue;
        }

        // --- 后续检查逻辑与 Symbol Mode 相同 ---
        QString foundLayerName;
        QgsGeometry nearestFeature = findNearestAssociatedFeature(
            featureCenter, associatedLayers, rule.searchRadius, foundLayerName);
        if (nearestFeature.isEmpty())
            continue;

        QgsPointXY ps, pf;
        computeConnectingPoints(featureGeom, nearestFeature, ps, pf);
        double associatedDirection = localTangentDirectionAt(nearestFeature, pf);

        bool hasError = false;
        QString errorDesc;
        double angleDiff;
        double finalFeatureAngleForCompare = featureDirection;       // [0, 180)
        double finalAssociatedAngleForCompare = associatedDirection; // [0, 180)

        // Feature Mode下，渠流向和道路代码都是"平行"检查
        if (rule.checkType == DirectionCheckType::PARALLEL)
        {
            angleDiff = std::abs(finalFeatureAngleForCompare - finalAssociatedAngleForCompare);
            angleDiff = std::min(angleDiff, 180.0 - angleDiff);
            hasError = (angleDiff > rule.angleThreshold);
            if (hasError)
            {
                errorDesc = QString("【方向错误】要素应平行于%1，实际偏差%2度")
                                .arg(foundLayerName)
                                .arg(angleDiff, 0, 'f', 1);
            }
        }
        else
        {
            // 可在此处为 Feature Mode 扩展其他检查类型
        }

        if (hasError)
        {
            AnnotationCheckResult result;
            result.errorType = AnnotationErrorType::FontError; // 复用
            result.featureId = QString::number(feature.id());  // 使用要素的 Feature ID
            result.actualValue = QString::number(featureDirection, 'f', 1);
            result.expectedValue = QString::number(associatedDirection, 'f', 1);
            result.errorMessage = errorDesc;
            result.position = QPointF(featureCenter.x(), featureCenter.y());
            result.layerName = targetLayer->name();
            results.append(result);
        }
    }

    logger->info("要素模式(Feature Mode)检查完成，检查了 {} 个要素，发现 {} 个问题", featureCount, results.size());
    return results;
}

QList<AnnotationCheckResult> SE_SymbolDirectionAlgorithm::checkSymbolDirection(
    QgsVectorLayer *symbolLayer, // 接口恢复为接收单个 QgsVectorLayer*
    const QMap<QString, QgsVectorLayer *> &associatedLayers,
    const DirectionCheckRule &rule)
{
    if (!symbolLayer || !symbolLayer->isValid())
    {
        m_logger->error("传入的目标图层无效");
        return QList<AnnotationCheckResult>();
    }

    m_logger->info("开始检查 '{}' 的方向性", rule.symbolType.toStdString());

    // ====================================================================
    // 模式分发：根据符号类型，决定进入 Feature Mode 还是 Symbol Mode
    // ====================================================================

    // 模式一：FEATURE MODE - 逐要素检查 (道路代码, 渠流向)
    if (rule.symbolType == "道路代码" || rule.symbolType == "渠流向")
    {
        // 调用新抽离的函数处理
        return checkFeaturesDirection(symbolLayer, associatedLayers, rule, m_logger);
    }

    // 模式二：SYMBOL MODE - 基于 symbol_id 检查 (泉等)
    else
    {
        QList<AnnotationCheckResult> results;
        int symbolIdIdx = symbolLayer->fields().indexOf(rule.symbolIdField);
        if (symbolIdIdx < 0)
        {
            m_logger->error("符号模式(Symbol Mode): 图层缺少 {} 字段", rule.symbolIdField.toStdString());
            return results;
        }

        QSet<QString> processedSymbolIds;
        QgsFeatureIterator it = symbolLayer->getFeatures();
        QgsFeature feature;
        while (it.nextFeature(feature))
        {
            QString symbolId = feature.attribute(symbolIdIdx).toString();
            if (symbolId.isEmpty() || processedSymbolIds.contains(symbolId))
                continue;

            processedSymbolIds.insert(symbolId);

            QgsGeometry symbolGeom = getSymbolRepresentativeGeometry(symbolLayer, symbolId, rule.symbolIdField);
            if (symbolGeom.isEmpty())
                continue;
            QgsPointXY symbolCenter = symbolGeom.centroid().asPoint();

            double symbolDirection = 0.0;
            if (rule.symbolType == "泉")
            {
                symbolDirection = calculateSpringDirection(symbolLayer, symbolId, rule.symbolIdField, m_logger);
            }
            else
            {
                // 其他未特化的符号，使用通用OMBB方法
                symbolDirection = calculateGeometryDirection(symbolGeom);
            }

            if (!std::isfinite(symbolDirection))
            {
                m_logger->debug("符号 {} 的方向不可判定，跳过方向检查", symbolId.toStdString());
                continue;
            }

            // --- 后续检查逻辑 (与之前版本完全相同) ---
            QString foundLayerName;
            QgsGeometry nearestFeature = findNearestAssociatedFeature(
                symbolCenter, associatedLayers, rule.searchRadius, foundLayerName);
            if (nearestFeature.isEmpty())
                continue;

            QgsPointXY ps, pf;
            computeConnectingPoints(symbolGeom, nearestFeature, ps, pf);
            double featureTangent = localTangentDirectionAt(nearestFeature, pf);

            bool hasError = false;
            QString errorDesc;
            double finalSymbolAngleForCompare = symbolDirection;
            double finalFeatureAngleForCompare = featureTangent;
            double angleDiff;

            switch (rule.checkType)
            {
            case DirectionCheckType::PERPENDICULAR:
                if (finalSymbolAngleForCompare >= 180.0)
                    finalSymbolAngleForCompare -= 180.0;
                angleDiff = std::abs(finalSymbolAngleForCompare - finalFeatureAngleForCompare);
                angleDiff = std::abs(angleDiff - 90.0);
                hasError = (angleDiff > rule.angleThreshold);
                if (hasError)
                    errorDesc = QString("【方向错误】符号应垂直于%1，实际偏差%2度")
                                    .arg(foundLayerName)
                                    .arg(angleDiff, 0, 'f', 1);
                break;
            case DirectionCheckType::PARALLEL:
                if (finalSymbolAngleForCompare >= 180.0)
                    finalSymbolAngleForCompare -= 180.0;
                angleDiff = std::abs(finalSymbolAngleForCompare - finalFeatureAngleForCompare);
                angleDiff = std::min(angleDiff, 180.0 - angleDiff);
                hasError = (angleDiff > rule.angleThreshold);
                if (hasError)
                    errorDesc = QString("【方向错误】符号应平行于%1，实际偏差%2度")
                                    .arg(foundLayerName)
                                    .arg(angleDiff, 0, 'f', 1);
                break;
            case DirectionCheckType::DIRECTION_MATCH:
            {
                double featureDir1 = featureTangent;
                double featureDir2 = featureTangent + 180.0;
                double diff1 = std::abs(symbolDirection - featureDir1);
                double diff2 = std::abs(symbolDirection - featureDir2);
                if (diff1 > 180.0)
                    diff1 = 360.0 - diff1;
                if (diff2 > 180.0)
                    diff2 = 360.0 - diff2;
                angleDiff = std::min(diff1, diff2);
                finalFeatureAngleForCompare = (diff1 < diff2) ? featureDir1 : featureDir2;
                hasError = (angleDiff > rule.angleThreshold);
                if (hasError)
                    errorDesc = QString("【方向错误】符号方向应与%1一致，实际偏差%2度")
                                    .arg(foundLayerName)
                                    .arg(angleDiff, 0, 'f', 1);
            }
            break;
            }

            if (hasError)
            {
                AnnotationCheckResult result;
                result.errorType = AnnotationErrorType::FontError; // 复用
                result.featureId = symbolId;
                result.actualValue = QString::number(symbolDirection, 'f', 1);
                result.expectedValue = QString::number(finalFeatureAngleForCompare, 'f', 1);
                result.errorMessage = errorDesc;
                result.position = QPointF(symbolCenter.x(), symbolCenter.y());
                result.layerName = symbolLayer->name();
                results.append(result);
            }
        }

        m_logger->info("符号模式(Symbol Mode)检查完成，检查了 {} 个符号，发现 {} 个问题", processedSymbolIds.size(), results.size());
        return results;
    }
}