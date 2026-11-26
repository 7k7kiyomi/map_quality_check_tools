#include "se_symbol_direction_algorithm.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsAbstractGeometry.h>
#include <QgsSpatialIndex.h>
#include <QgsPointXY.h>
#include <QgsExpression.h>
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

static double calculateSpringDirection(
    QgsVectorLayer *symbolLayer,
    const QString &symbolId,
    const QString &symbolIdField,
    std::shared_ptr<spdlog::logger> logger)
{
    QgsFeature headFeature;
    QgsFeature tailFeature;
    double maxAspectRatio = -1.0;
    double minAspectRatio = std::numeric_limits<double>::max();

    QgsFeatureIterator it = symbolLayer->getFeatures(QgsFeatureRequest().setFilterExpression(
        QgsExpression::createFieldEqualityExpression(symbolIdField, symbolId)));
    QgsFeature feature;
    while (it.nextFeature(feature))
    {
        // 使用旋转外包框识别圆头和尾部，避免版面宽高属性随旋转变化。
        QgsGeometry box = feature.geometry().orientedMinimumBoundingBox();
        QgsPolygonXY polygon = box.asPolygon();
        if (polygon.isEmpty() || polygon.front().size() < 4)
            continue;
        const auto &ring = polygon.front();
        double width = ring[0].distance(ring[1]);
        double height = ring[1].distance(ring[2]);
        if (std::min(width, height) <= std::numeric_limits<double>::epsilon())
            continue;
        double aspectRatio = std::max(width, height) / std::min(width, height);
        if (!std::isfinite(aspectRatio))
            continue;
        if (aspectRatio > maxAspectRatio)
        {
            maxAspectRatio = aspectRatio;
            tailFeature = feature;
        }
        if (aspectRatio < minAspectRatio)
        {
            minAspectRatio = aspectRatio;
            headFeature = feature;
        }
    }

    if (!headFeature.isValid() || !tailFeature.isValid() || headFeature.id() == tailFeature.id())
    {
        logger->warn("未能为泉符号 {} 识别独立的圆头和尾部，方向不可判定", symbolId.toStdString());
        return std::numeric_limits<double>::quiet_NaN();
    }

    QgsGeometry headCenterGeom = headFeature.geometry().centroid();
    if (headCenterGeom.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();
    QgsPointXY headCenter = headCenterGeom.asPoint();
    QgsPointXY tailTip;
    double maxDistance = 0.0;
    QgsGeometry tailGeom = tailFeature.geometry();
    auto vertices = tailGeom.vertices();
    while (vertices.hasNext())
    {
        QgsPointXY vertex(vertices.next());
        double distance = headCenter.distance(vertex);
        if (std::isfinite(distance) && distance > maxDistance)
        {
            maxDistance = distance;
            tailTip = vertex;
        }
    }
    if (maxDistance <= std::numeric_limits<double>::epsilon())
        return std::numeric_limits<double>::quiet_NaN();

    double angle = std::atan2(tailTip.y() - headCenter.y(), tailTip.x() - headCenter.x()) * 180.0 / M_PI;
    return angle < 0.0 ? angle + 360.0 : angle;
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

static double calculateBridgeDirection(
    QgsVectorLayer *symbolLayer, const QString &symbolId, const QString &symbolIdField)
{
    const int strokedIdx = symbolLayer->fields().indexOf("stroked");
    double sumX = 0.0;
    double sumY = 0.0;
    double totalLength = 0.0;
    QgsFeatureIterator it = symbolLayer->getFeatures(QgsFeatureRequest().setFilterExpression(
        QgsExpression::createFieldEqualityExpression(symbolIdField, symbolId)));
    QgsFeature feature;
    while (it.nextFeature(feature))
    {
        // 白色填充遮罩和端部折线不代表桥轴，取每条描边侧线的最长线段。
        if (strokedIdx >= 0 && !feature.attribute(strokedIdx).toBool())
            continue;
        QList<QgsPolylineXY> lines;
        collectCandidateLines(feature.geometry(), lines);
        for (const auto &line : lines)
        {
            double longest = 0.0;
            double direction = 0.0;
            for (int i = 1; i < line.size(); ++i)
            {
                double length = line[i - 1].distance(line[i]);
                if (std::isfinite(length) && length > longest)
                {
                    longest = length;
                    direction = std::atan2(line[i].y() - line[i - 1].y(), line[i].x() - line[i - 1].x());
                }
            }
            sumX += longest * std::cos(2.0 * direction);
            sumY += longest * std::sin(2.0 * direction);
            totalLength += longest;
        }
    }
    if (totalLength <= std::numeric_limits<double>::epsilon() ||
        std::hypot(sumX, sumY) <= totalLength * 1e-6)
        return std::numeric_limits<double>::quiet_NaN();
    double angle = std::atan2(sumY, sumX) * 90.0 / M_PI;
    return angle < 0.0 ? angle + 180.0 : angle;
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
            if (a == b)
                continue;
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
        return std::numeric_limits<double>::quiet_NaN();

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
        return std::numeric_limits<double>::quiet_NaN();
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
    QgsGeometry pointA = a.nearestPoint(b);
    QgsGeometry pointB = b.nearestPoint(a);
    if (pointA.isEmpty() || pointB.isEmpty())
        return false;
    pa = pointA.asPoint();
    pb = pointB.asPoint();
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
            if (std::isfinite(dist) && dist >= 0.0 && dist < minDistance && dist <= searchRadius)
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
        QgsExpression::createFieldEqualityExpression(symbolIdField, symbolId)));
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

static double getLocalOverallDirection(const QgsGeometry &lineGeom, const QgsPointXY &referencePoint, double length)
{
    const double undefined = std::numeric_limits<double>::quiet_NaN();
    if (!std::isfinite(length) || length <= 0.0)
        return undefined;
    QList<QgsPolylineXY> lines;
    if (!collectCandidateLines(lineGeom, lines))
        return undefined;

    QgsGeometry pointGeom = QgsGeometry::fromPointXY(referencePoint);
    QgsGeometry nearestPart;
    double bestDistance = std::numeric_limits<double>::max();
    for (const auto &line : lines)
    {
        QgsGeometry part = QgsGeometry::fromPolylineXY(line);
        if (part.length() <= std::numeric_limits<double>::epsilon())
            continue;
        double distance = part.distance(pointGeom);
        if (distance >= 0.0 && distance < bestDistance)
        {
            bestDistance = distance;
            nearestPart = part;
        }
    }
    if (nearestPart.isEmpty())
        return undefined;

    double position = nearestPart.lineLocatePoint(pointGeom);
    double totalLength = nearestPart.length();
    if (!std::isfinite(position) || position < 0.0)
        return undefined;
    double window = std::min(length, totalLength);
    // 按线长插值，在线端点处向可用的一侧补足窗口，避免依赖顶点密度。
    double start = std::max(0.0, std::min(position - window / 2.0, totalLength - window));
    QgsGeometry startGeom = nearestPart.interpolate(start);
    QgsGeometry endGeom = nearestPart.interpolate(start + window);
    if (startGeom.isEmpty() || endGeom.isEmpty())
        return undefined;
    QgsPointXY a = startGeom.asPoint();
    QgsPointXY b = endGeom.asPoint();
    if (a.distance(b) <= std::numeric_limits<double>::epsilon())
        return undefined;
    double angle = std::atan2(b.y() - a.y(), b.x() - a.x()) * 180.0 / M_PI;
    return angle < 0.0 ? angle + 180.0 : angle;
}

double SE_SymbolDirectionAlgorithm::calculateAssociatedDirection(
    const QgsGeometry &symbolGeometry,
    const QgsGeometry &associatedGeometry,
    const DirectionCheckRule &rule)
{
    if (rule.symbolType == "泉" || rule.symbolType == "桥梁" ||
        rule.symbolType == "道路代码" || rule.symbolType == "隧道口")
    {
        QgsGeometry center = symbolGeometry.centroid();
        if (center.isEmpty())
            return std::numeric_limits<double>::quiet_NaN();
        const QgsRectangle bounds = symbolGeometry.boundingBox();
        double window = std::max(mmToMapUnits(2.0), std::max(bounds.width(), bounds.height()));
        return getLocalOverallDirection(associatedGeometry, center.asPoint(), window);
    }

    QgsPointXY ps, pf;
    if (!computeConnectingPoints(symbolGeometry, associatedGeometry, ps, pf))
        return std::numeric_limits<double>::quiet_NaN();
    return localTangentDirectionAt(associatedGeometry, pf);
}

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
    int checkedCount = 0;
    int unmatchedCount = 0;
    int undefinedCount = 0;
    while (it.nextFeature(feature))
    {
        // 新增：过滤 element_ty = text 的要素
        if (elementTyIdx >= 0 && feature.attribute(elementTyIdx).toString() == "text")
            continue;

        featureCount++;
        QgsGeometry featureGeom = feature.geometry();
        if (featureGeom.isEmpty())
        {
            ++undefinedCount;
            continue;
        }

        QgsPointXY featureCenter = featureGeom.centroid().asPoint();

        // 计算当前要素的方向
        // 对于渠流向(箭杆)和道路代码(外框)，通用的OMBB方法都适用
        double featureDirection = calculateGeometryDirection(featureGeom);
        if (!std::isfinite(featureDirection))
        {
            ++undefinedCount;
            logger->debug("要素 {} 的方向不可判定，跳过方向检查", static_cast<long long>(feature.id()));
            continue;
        }

        // --- 后续检查逻辑与 Symbol Mode 相同 ---
        QString foundLayerName;
        QgsGeometry nearestFeature = findNearestAssociatedFeature(
            featureCenter, associatedLayers, rule.searchRadius, foundLayerName);
        if (nearestFeature.isEmpty())
        {
            ++unmatchedCount;
            continue;
        }

        double associatedDirection = calculateAssociatedDirection(featureGeom, nearestFeature, rule);
        if (!std::isfinite(associatedDirection))
        {
            ++undefinedCount;
            continue;
        }
        ++checkedCount;

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

    logger->info("要素模式检查完成：共 {} 个要素，实际判定 {} 个，未关联 {} 个，不可判定 {} 个，发现 {} 个问题",
                 featureCount, checkedCount, unmatchedCount, undefinedCount, results.size());
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
        int checkedCount = 0;
        int unmatchedCount = 0;
        int undefinedCount = 0;
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
            {
                ++undefinedCount;
                continue;
            }
            QgsPointXY symbolCenter = symbolGeom.centroid().asPoint();

            double symbolDirection = 0.0;
            if (rule.symbolType == "泉")
            {
                symbolDirection = calculateSpringDirection(symbolLayer, symbolId, rule.symbolIdField, m_logger);
            }
            else if (rule.symbolType == "桥梁")
            {
                symbolDirection = calculateBridgeDirection(symbolLayer, symbolId, rule.symbolIdField);
            }
            else
            {
                // 其他未特化的符号，使用通用OMBB方法
                symbolDirection = calculateGeometryDirection(symbolGeom);
            }

            if (!std::isfinite(symbolDirection))
            {
                ++undefinedCount;
                m_logger->debug("符号 {} 的方向不可判定，跳过方向检查", symbolId.toStdString());
                continue;
            }

            // --- 后续检查逻辑 (与之前版本完全相同) ---
            QString foundLayerName;
            QgsGeometry nearestFeature = findNearestAssociatedFeature(
                symbolCenter, associatedLayers, rule.searchRadius, foundLayerName);
            if (nearestFeature.isEmpty())
            {
                ++unmatchedCount;
                continue;
            }

            double featureTangent = calculateAssociatedDirection(symbolGeom, nearestFeature, rule);
            if (!std::isfinite(featureTangent))
            {
                ++undefinedCount;
                continue;
            }
            ++checkedCount;

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

        m_logger->info("符号模式检查完成：共 {} 个符号，实际判定 {} 个，未关联 {} 个，不可判定 {} 个，发现 {} 个问题",
                       processedSymbolIds.size(), checkedCount, unmatchedCount, undefinedCount, results.size());
        return results;
    }
}