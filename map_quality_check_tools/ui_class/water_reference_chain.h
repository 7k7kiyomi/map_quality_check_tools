#pragma once
#include "water_result_classification.h"

namespace WaterReferenceChain {
struct Result {
    QString category;
    QVector<QgsFeature> features;
};

inline Result find(const QgsGeometry& source, const QgsCoordinateReferenceSystem& crs,
                   QgsVectorLayer* reference, const QString& category, double tolerance)
{
    Result result;
    if (!reference || !reference->isValid() || !crs.isValid() || crs != reference->crs() ||
        category.isEmpty() || source.isEmpty() || source.length() <= 0 || tolerance <= 0)
        return result;
    const QString field = reference->fields().indexFromName(QStringLiteral("LEVEL")) >= 0
        ? QStringLiteral("LEVEL") : QStringLiteral("TYPE");
    if (reference->fields().indexFromName(field) < 0) return result;
    struct Candidate { QgsFeature feature; QgsGeometry buffer; };
    QVector<Candidate> candidates;
    QgsRectangle bounds = source.boundingBox();
    bounds.grow(tolerance);
    auto iterator = reference->getFeatures(QgsFeatureRequest().setFilterRect(bounds));
    QgsFeature feature;
    while (iterator.nextFeature(feature)) {
        if (!feature.hasGeometry() ||
            WaterResultClassification::categoryFor(field, feature.attribute(field).toString()) != category)
            continue;
        const auto geometry = feature.geometry();
        if (geometry.isEmpty() || geometry.type() != QgsWkbTypes::LineGeometry) continue;
        const auto buffer = geometry.buffer(tolerance, 8);
        if (buffer.isEmpty() || source.intersection(buffer).length() / source.length() < 0.01)
            continue;
        candidates.append({feature, buffer});
    }
    QVector<bool> visited(candidates.size(), false);
    double bestCoverage = 0, secondCoverage = 0;
    QVector<QgsFeature> best;
    for (int i = 0; i < candidates.size(); ++i) {
        if (visited[i]) continue;
        QVector<int> group{i};
        visited[i] = true;
        for (int n = 0; n < group.size(); ++n) {
            const auto current = candidates[group[n]].feature.geometry();
            for (int j = 0; j < candidates.size(); ++j) {
                if (visited[j]) continue;
                const double distance = current.distance(candidates[j].feature.geometry());
                if (distance >= 0 && distance <= 0.01) {
                    visited[j] = true;
                    group.append(j);
                }
            }
        }
        QVector<QgsGeometry> buffers;
        QVector<QgsFeature> features;
        for (int index : group) {
            buffers.append(candidates[index].buffer);
            features.append(candidates[index].feature);
        }
        const double coverage = source.intersection(QgsGeometry::unaryUnion(buffers)).length() / source.length();
        // A connected network can contain branches. Follow the nearest supported
        // segment along the source and reject a sequence that returns to a
        // previously departed segment.
        if (group.size() > 1) {
            QSet<int> departed;
            int previous = -1;
            bool ordered = true;
            for (int sample = 0; sample < 99 && ordered; ++sample) {
                const QgsGeometry point = source.interpolate(source.length() * (sample + 0.5) / 99.0);
                double nearest = tolerance;
                int current = -1;
                for (int index : group) {
                    const double distance = point.distance(candidates[index].feature.geometry());
                    if (distance >= 0 && distance <= nearest) {
                        nearest = distance;
                        current = index;
                    }
                }
                if (current < 0 || current == previous) continue;
                if (departed.contains(current)) ordered = false;
                if (previous >= 0) departed.insert(previous);
                previous = current;
            }
            if (!ordered) continue;
        }
        if (coverage > bestCoverage) {
            secondCoverage = bestCoverage;
            bestCoverage = coverage;
            best = features;
        } else if (coverage > secondCoverage) secondCoverage = coverage;
    }
    if (bestCoverage >= 0.6 && (secondCoverage < 0.6 || bestCoverage - secondCoverage >= 0.1)) {
        result.category = category;
        result.features = best;
    }
    return result;
}
}
