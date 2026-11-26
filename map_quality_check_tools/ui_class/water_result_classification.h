#pragma once

#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeatureRequest.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace WaterResultClassification {

struct Match {
    QString category;
    QString rawValue;
    QgsFeatureId referenceId = -1;
    double coverage = 0.0;
    QMap<QString, QgsFeatureId> candidates;
    QString evidence;
};

inline QString categoryFor(const QString &field, const QString &value)
{
    const QString v = value.trimmed();
    if (field == QStringLiteral("LEVEL")) {
        static const QStringList levels{QStringLiteral("一级"), QStringLiteral("二级"),
            QStringLiteral("三级"), QStringLiteral("四级"), QStringLiteral("五级"),
            QStringLiteral("六级"), QStringLiteral("等外")};
        return levels.contains(v) ? v + QStringLiteral("河流") : QString();
    }
    if (field == QStringLiteral("TYPE")) {
        static const QStringList types{QStringLiteral("总干渠"), QStringLiteral("干渠"),
            QStringLiteral("支渠"), QStringLiteral("沟渠")};
        return types.contains(v) ? v : QString();
    }
    return {};
}

inline Match classify(const QgsGeometry &geometry, const QgsCoordinateReferenceSystem &crs,
                      QgsVectorLayer *reference, double tolerance)
{
    Match unmatched;
    if (!reference || !reference->isValid() || geometry.isNull() || geometry.isEmpty() ||
        geometry.type() != QgsWkbTypes::LineGeometry || geometry.length() <= 0.0 ||
        !std::isfinite(tolerance) || tolerance <= 0.0) return unmatched;

    const int levelIndex = reference->fields().indexFromName(QStringLiteral("LEVEL"));
    const int typeIndex = reference->fields().indexFromName(QStringLiteral("TYPE"));
    const QString field = levelIndex >= 0 ? QStringLiteral("LEVEL")
        : typeIndex >= 0 ? QStringLiteral("TYPE") : QString();
    if (field.isEmpty()) return unmatched;

    // The configured distance is in the parsed CRS. Do not apply it in a
    // different coordinate system where its units may differ.
    if (!crs.isValid() || !reference->crs().isValid() || crs != reference->crs())
        return unmatched;
    QgsGeometry query = geometry;
    const double length = query.length();
    if (length <= 0.0) return unmatched;
    QgsRectangle bounds = query.boundingBox();
    bounds.grow(tolerance);
    QgsFeatureIterator it = reference->getFeatures(QgsFeatureRequest().setFilterRect(bounds));
    QgsFeature candidate;
    QMap<QString, QgsGeometry> coverageByCategory;
    QMap<QString, Match> matches;
    QMap<QString, double> bestDistances;
    QMap<QString, double> secondDistances;
    QMap<QString, int> bestAlignment;
    QMap<QString, int> secondAlignment;
    while (it.nextFeature(candidate)) {
        if (!candidate.hasGeometry()) continue;
        const QgsGeometry ref = candidate.geometry();
        if (ref.isNull() || ref.isEmpty() || ref.type() != QgsWkbTypes::LineGeometry) continue;
        const QString raw = candidate.attribute(field).toString().trimmed();
        QString category = categoryFor(field, raw);
        if (category.isEmpty()) category = QStringLiteral("__unknown__");
        const QgsGeometry covered = query.intersection(ref.buffer(tolerance, 8));
        if (covered.isNull() || covered.isEmpty() || covered.length() <= 0.0) continue;
        if (coverageByCategory.contains(category))
            coverageByCategory[category] = coverageByCategory[category].combine(covered);
        else
            coverageByCategory[category] = covered;
        double distanceSum = 0.0;
        bool validSamples = true;
        int alignment = 0;
        for (int n = 0; n < 21; ++n) {
            const double position = length * (n + 0.5) / 21.0;
            const QgsGeometry sample = query.interpolate(position);
            const double distance = sample.distance(ref);
            if (!std::isfinite(distance) || distance < 0.0) {
                validSamples = false;
                break;
            }
            distanceSum += distance;
            QgsPointXY nearest;
            int vertexAfter = -1;
            if (ref.closestSegmentWithContext(sample.asPoint(), nearest, vertexAfter) >= 0 && vertexAfter >= 1) {
                const QgsPointXY before = query.interpolate(std::max(0.0, position - length / 210.0)).asPoint();
                const QgsPointXY after = query.interpolate(std::min(length, position + length / 210.0)).asPoint();
                const QgsPoint a = ref.vertexAt(vertexAfter - 1);
                const QgsPoint b = ref.vertexAt(vertexAfter);
                const double dx = after.x() - before.x(), dy = after.y() - before.y();
                const double rx = b.x() - a.x(), ry = b.y() - a.y();
                const double denominator = std::hypot(dx, dy) * std::hypot(rx, ry);
                if (denominator > 0 && std::abs(dx * rx + dy * ry) / denominator >= 0.707106781)
                    ++alignment;
            }
        }
        if (!validSamples) continue;
        const double meanDistance = distanceSum / 21.0;
        const double separation = std::max(0.1, tolerance * 0.05);
        const bool betterDirection = matches.contains(category) &&
            std::abs(meanDistance - bestDistances.value(category)) < separation &&
            alignment >= 13 && bestAlignment.value(category) < 13;
        const bool worseDirection = matches.contains(category) &&
            std::abs(meanDistance - bestDistances.value(category)) < separation &&
            alignment < 13 && bestAlignment.value(category) >= 13;
        if (!matches.contains(category) || betterDirection ||
            (!worseDirection && meanDistance < bestDistances.value(category))) {
            if (matches.contains(category) &&
                (!secondDistances.contains(category) || bestDistances.value(category) < secondDistances.value(category))) {
                secondDistances[category] = bestDistances.value(category);
                secondAlignment[category] = bestAlignment.value(category);
            }
            Match match;
            match.category = category;
            match.rawValue = raw;
            match.referenceId = candidate.id();
            match.coverage = covered.length();
            matches[category] = match;
            bestDistances[category] = meanDistance;
            bestAlignment[category] = alignment;
        } else if (!secondDistances.contains(category) || meanDistance < secondDistances.value(category)) {
            secondDistances[category] = meanDistance;
            secondAlignment[category] = alignment;
        }
    }
    Match best;
    double second = 0.0;
    for (auto i = coverageByCategory.cbegin(); i != coverageByCategory.cend(); ++i) {
        const double ratio = i.value().length() / length;
        if (ratio > best.coverage) {
            second = best.coverage;
            best = matches.value(i.key());
            best.coverage = ratio;
        } else if (ratio > second) second = ratio;
    }
    // Require most of the line to be supported and avoid assigning a near tie.
    if (best.category == QStringLiteral("__unknown__") || best.coverage < 0.6) return unmatched;
    if (best.coverage - second < 0.1) {
        QMap<QString, QgsGeometry> candidateGeometries;
        for (auto i = coverageByCategory.cbegin(); i != coverageByCategory.cend(); ++i) {
            if (i.key() != QStringLiteral("__unknown__") && i.value().length() / length >= 0.6) {
                unmatched.candidates.insert(i.key(), matches.value(i.key()).referenceId);
                candidateGeometries.insert(i.key(), reference->getFeature(matches.value(i.key()).referenceId).geometry());
            }
        }
        // Compare the whole source line. A crossing at one endpoint must not
        // score like a reference line that follows the source throughout.
        QString closestCategory;
        double closest = std::numeric_limits<double>::infinity();
        double runnerUp = closest;
        for (auto i = candidateGeometries.cbegin(); i != candidateGeometries.cend(); ++i) {
            if (i.value().isNull() || i.value().isEmpty()) continue;
            double sum = 0.0;
            bool valid = true;
            for (int n = 0; n < 21; ++n) {
                const QgsGeometry point = query.interpolate(length * (n + 0.5) / 21.0);
                const double distance = point.distance(i.value());
                if (!std::isfinite(distance) || distance < 0.0) { valid = false; break; }
                sum += distance;
            }
            if (!valid) continue;
            const double mean = sum / 21.0;
            if (mean < closest) { runnerUp = closest; closest = mean; closestCategory = i.key(); }
            else if (mean < runnerUp) runnerUp = mean;
        }
        if (!closestCategory.isEmpty() && std::isfinite(runnerUp) &&
            closest <= tolerance && runnerUp - closest >= std::max(0.1, tolerance * 0.05)
            && runnerUp >= closest * 1.5) {
            const QgsGeometry referenceGeometry = candidateGeometries.value(closestCategory);
            int aligned = 0;
            for (int n = 0; n < 21; ++n) {
                const double position = length * (n + 0.5) / 21.0;
                const double delta = length / 210.0;
                const QgsPointXY before = query.interpolate(std::max(0.0, position - delta)).asPoint();
                const QgsPointXY after = query.interpolate(std::min(length, position + delta)).asPoint();
                const QgsPointXY sample = query.interpolate(position).asPoint();
                QgsPointXY nearest;
                int vertexAfter = -1;
                if (referenceGeometry.closestSegmentWithContext(sample, nearest, vertexAfter) < 0 || vertexAfter < 1)
                    continue;
                const QgsPoint a = referenceGeometry.vertexAt(vertexAfter - 1);
                const QgsPoint b = referenceGeometry.vertexAt(vertexAfter);
                const double dx = after.x() - before.x(), dy = after.y() - before.y();
                const double rx = b.x() - a.x(), ry = b.y() - a.y();
                const double denominator = std::hypot(dx, dy) * std::hypot(rx, ry);
                if (denominator > 0 && std::abs(dx * rx + dy * ry) / denominator >= 0.707106781)
                    ++aligned;
            }
            if (aligned < 13) return unmatched;
            Match resolved = matches.value(closestCategory);
            resolved.coverage = coverageByCategory.value(closestCategory).length() / length;
            resolved.evidence = QStringLiteral("沿成果线采样比较，平均距离 %1 米，次优类别 %2 米，方向一致 %3/21，库体FID %4")
                .arg(closest, 0, 'f', 3).arg(runnerUp, 0, 'f', 3).arg(aligned).arg(resolved.referenceId);
            if (secondDistances.contains(closestCategory) &&
                std::abs(secondDistances.value(closestCategory) - bestDistances.value(closestCategory)) < std::max(0.1, tolerance * 0.05) &&
                (bestAlignment.value(closestCategory) >= 13) == (secondAlignment.value(closestCategory) >= 13))
                resolved.referenceId = -1;
            if (matches.value(closestCategory).coverage / length < 0.6)
                resolved.referenceId = -1;
            return resolved;
        }
        return unmatched;
    }
    if (secondDistances.contains(best.category) &&
        std::abs(secondDistances.value(best.category) - bestDistances.value(best.category)) < std::max(0.1, tolerance * 0.05) &&
        (bestAlignment.value(best.category) >= 13) == (secondAlignment.value(best.category) >= 13))
        best.referenceId = -1;
    if (matches.value(best.category).coverage / length < 0.6)
        best.referenceId = -1;
    return best;
}

inline bool endpoints(const QgsGeometry &geometry, QgsPointXY &start, QgsPointXY &end)
{
    QgsPolylineXY line;
    if (geometry.isMultipart()) {
        const QgsMultiPolylineXY parts = geometry.asMultiPolyline();
        if (parts.size() != 1) return false;
        line = parts.front();
    } else {
        line = geometry.asPolyline();
    }
    if (line.size() < 2) return false;
    start = line.front();
    end = line.back();
    return true;
}

inline Match classifyDirectionFallback(const QgsGeometry &geometry,
    const QgsCoordinateReferenceSystem &crs, QgsVectorLayer *reference,
    double endpointTolerance, double fallbackTolerance)
{
    Match unmatched;
    QgsPointXY start, end;
    if (!reference || !reference->isValid() || !crs.isValid() || crs != reference->crs() ||
        !endpoints(geometry, start, end) || geometry.length() <= 0 ||
        endpointTolerance <= 0 || fallbackTolerance <= endpointTolerance)
        return unmatched;
    const int levelIndex = reference->fields().indexFromName(QStringLiteral("LEVEL"));
    const int typeIndex = reference->fields().indexFromName(QStringLiteral("TYPE"));
    const QString field = levelIndex >= 0 ? QStringLiteral("LEVEL")
        : typeIndex >= 0 ? QStringLiteral("TYPE") : QString();
    if (field.isEmpty()) return unmatched;
    QgsRectangle bounds = geometry.boundingBox();
    bounds.grow(fallbackTolerance);
    QgsFeatureIterator it = reference->getFeatures(QgsFeatureRequest().setFilterRect(bounds));
    QgsFeature candidate;
    Match best;
    double bestMean = std::numeric_limits<double>::infinity();
    double secondMean = bestMean;
    QString secondCategory;
    const double length = geometry.length();
    while (it.nextFeature(candidate)) {
        if (!candidate.hasGeometry()) continue;
        const QgsGeometry ref = candidate.geometry();
        if (ref.isNull() || ref.isEmpty() || ref.type() != QgsWkbTypes::LineGeometry) continue;
        const double startDistance = QgsGeometry::fromPointXY(start).distance(ref);
        const double endDistance = QgsGeometry::fromPointXY(end).distance(ref);
        if (startDistance < 0 || endDistance < 0 || startDistance > endpointTolerance ||
            endDistance > endpointTolerance) continue;
        const QgsGeometry covered = geometry.intersection(ref.buffer(fallbackTolerance, 8));
        if (covered.isNull() || covered.length() / length < 0.6) continue;
        int close[3] = {0, 0, 0};
        double distanceSum = 0;
        bool valid = true;
        for (int n = 0; n < 99; ++n) {
            const double distance = geometry.interpolate(length * (n + 0.5) / 99.0).distance(ref);
            if (!std::isfinite(distance) || distance < 0) { valid = false; break; }
            distanceSum += distance;
            if (distance <= fallbackTolerance) ++close[n / 33];
        }
        if (!valid || close[0] < 9 || close[1] < 9 || close[2] < 9) continue;
        const QString raw = candidate.attribute(field).toString().trimmed();
        const QString category = categoryFor(field, raw);
        const double mean = distanceSum / 99.0;
        if (mean < bestMean) {
            secondMean = bestMean;
            secondCategory = best.category;
            best.category = category;
            best.rawValue = raw;
            best.referenceId = candidate.id();
            best.coverage = covered.length() / length;
            bestMean = mean;
        } else if (mean < secondMean) {
            secondMean = mean;
            secondCategory = category;
        }
    }
    if (best.category.isEmpty()) return unmatched;
    if (std::isfinite(secondMean) &&
        (secondMean - bestMean < std::max(0.1, fallbackTolerance * 0.05) ||
         secondMean < bestMean * 1.5)) {
        if (secondCategory != best.category) return unmatched;
        best.referenceId = -1;
    }
    best.evidence = QStringLiteral("整段河道补判，覆盖率 %1%，平均距离 %2 米，库体FID %3")
        .arg(best.coverage * 100, 0, 'f', 1).arg(bestMean, 0, 'f', 3).arg(best.referenceId);
    return best;
}

// Report connected reference candidates even when they do not cover the whole source.
inline QString partialReferenceEvidence(const QgsGeometry &geometry,
    const QgsCoordinateReferenceSystem &crs, QgsVectorLayer *reference, double tolerance)
{
    QgsPointXY start, end;
    if (!reference || !reference->isValid() || crs != reference->crs() ||
        !endpoints(geometry, start, end) || geometry.length() <= 0 || tolerance <= 0)
        return {};
    const QString field = reference->fields().indexFromName(QStringLiteral("LEVEL")) >= 0
        ? QStringLiteral("LEVEL") : QStringLiteral("TYPE");
    if (reference->fields().indexFromName(field) < 0) return {};
    struct Candidate { QgsFeature feature; QString category; QgsGeometry buffer; };
    QVector<Candidate> candidates;
    QgsRectangle bounds = geometry.boundingBox();
    bounds.grow(tolerance);
    QgsFeature feature;
    auto iterator = reference->getFeatures(QgsFeatureRequest().setFilterRect(bounds));
    while (iterator.nextFeature(feature)) {
        if (!feature.hasGeometry()) continue;
        const QString category = categoryFor(field, feature.attribute(field).toString());
        if (category.isEmpty()) continue;
        const QgsGeometry buffer = feature.geometry().buffer(tolerance, 8);
        if (buffer.isNull() || geometry.intersection(buffer).length() / geometry.length() < 0.01) continue;
        candidates.push_back({feature, category, buffer});
    }
    QVector<bool> visited(candidates.size(), false);
    double bestCoverage = 0;
    QString bestEvidence;
    for (int i = 0; i < candidates.size(); ++i) {
        if (visited[i]) continue;
        QVector<int> group{i};
        visited[i] = true;
        for (int n = 0; n < group.size(); ++n) {
            const auto current = candidates[group[n]].feature.geometry();
            for (int j = 0; j < candidates.size(); ++j) {
                if (visited[j] || candidates[j].category != candidates[i].category) continue;
                const double distance = current.distance(candidates[j].feature.geometry());
                if (distance >= 0 && distance <= 0.01) {
                    visited[j] = true;
                    group.push_back(j);
                }
            }
        }
        QVector<QgsGeometry> buffers, lines;
        QStringList ids;
        for (int index : group) {
            buffers.push_back(candidates[index].buffer);
            lines.push_back(candidates[index].feature.geometry());
            ids.append(QString::number(candidates[index].feature.id()));
        }
        const QgsGeometry covered = geometry.intersection(QgsGeometry::unaryUnion(buffers));
        const double coverage = covered.length() / geometry.length();
        if (coverage <= bestCoverage) continue;
        const QgsGeometry combined = QgsGeometry::unaryUnion(lines);
        const double first = QgsGeometry::fromPointXY(start).distance(combined);
        const double last = QgsGeometry::fromPointXY(end).distance(combined);
        bestCoverage = coverage;
        bestEvidence = QStringLiteral("候选%1，库体FID %2，覆盖率%3%，起终点距候选%4/%5米，整段待确认")
            .arg(candidates[i].category, ids.join(QStringLiteral(",")))
            .arg(coverage * 100, 0, 'f', 1).arg(first, 0, 'f', 1).arg(last, 0, 'f', 1);
    }
    return bestEvidence;
}

// Resolve only a genuine category tie supported independently at both ends.
inline Match classifyWithNeighbors(const QgsFeature &target, QgsVectorLayer *parsed,
                                   QgsVectorLayer *reference, double tolerance,
                                   const Match &ambiguous)
{
    if (!parsed || !reference || ambiguous.candidates.size() < 2) return ambiguous;
    QgsPointXY targetStart, targetEnd;
    if (!endpoints(target.geometry(), targetStart, targetEnd)) return ambiguous;
    constexpr double nodeTolerance = 0.01;
    QgsFeature anchors[2];
    Match anchorMatches[2];
    const QgsPointXY nodes[2] = {targetStart, targetEnd};
    for (int side = 0; side < 2; ++side) {
        QgsRectangle bounds(nodes[side].x() - nodeTolerance, nodes[side].y() - nodeTolerance,
                            nodes[side].x() + nodeTolerance, nodes[side].y() + nodeTolerance);
        QgsFeature neighbor;
        QgsFeatureIterator it = parsed->getFeatures(QgsFeatureRequest().setFilterRect(bounds));
        int count = 0;
        while (it.nextFeature(neighbor)) {
            if (neighbor.id() == target.id() || !neighbor.hasGeometry()) continue;
            const QgsGeometry node = QgsGeometry::fromPointXY(nodes[side]);
            if (node.distance(neighbor.geometry()) > nodeTolerance) continue;
            anchors[side] = neighbor;
            ++count;
        }
        if (count != 1) return ambiguous;
        anchorMatches[side] = classify(anchors[side].geometry(), parsed->crs(), reference, tolerance);
        if (anchorMatches[side].category.isEmpty()) return ambiguous;
    }
    if (anchors[0].id() == anchors[1].id() || anchorMatches[0].category != anchorMatches[1].category)
        return ambiguous;
    const QString category = anchorMatches[0].category;
    if (!ambiguous.candidates.contains(category)) return ambiguous;
    const QgsFeature candidate = reference->getFeature(ambiguous.candidates.value(category));
    if (!candidate.hasGeometry()) return ambiguous;
    QgsPointXY candidateStart, candidateEnd;
    if (!endpoints(candidate.geometry(), candidateStart, candidateEnd)) return ambiguous;
    const QgsPointXY candidateNodes[2] = {candidateStart, candidateEnd};
    bool connected[2][2] = {{false, false}, {false, false}};
    for (int side = 0; side < 2; ++side) {
        const QgsFeature anchor = reference->getFeature(anchorMatches[side].referenceId);
        if (!anchor.hasGeometry()) return ambiguous;
        if (anchor.id() == candidate.id()) { connected[side][0] = connected[side][1] = true; continue; }
        for (int node = 0; node < 2; ++node) {
            const double distance = QgsGeometry::fromPointXY(candidateNodes[node]).distance(anchor.geometry());
            if (distance >= 0 && distance <= nodeTolerance)
                connected[side][node] = true;
        }
    }
    if (!(connected[0][0] && connected[1][1]) && !(connected[0][1] && connected[1][0])) return ambiguous;
    Match resolved;
    resolved.category = category;
    resolved.referenceId = candidate.id();
    const QString field = reference->fields().indexFromName(QStringLiteral("LEVEL")) >= 0
        ? QStringLiteral("LEVEL") : QStringLiteral("TYPE");
    resolved.rawValue = candidate.attribute(field).toString().trimmed();
    resolved.evidence = QStringLiteral("相邻成果FID %1、%2；对应库体FID %3、%4；候选库体FID %5")
        .arg(anchors[0].id()).arg(anchors[1].id()).arg(anchorMatches[0].referenceId)
        .arg(anchorMatches[1].referenceId).arg(candidate.id());
    return resolved;
}

} // namespace WaterResultClassification
