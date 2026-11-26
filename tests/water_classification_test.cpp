#include "ui_class/water_result_classification.h"
#include "ui_class/water_reference_chain.h"
#include <qgsapplication.h>
#include <qgsvectordataprovider.h>
#include <stdexcept>
#include <iostream>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static void add(QgsVectorLayer &layer, const QString &wkt, const QString &level) {
    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromWkt(wkt));
    f.setAttribute(0, level);
    QgsFeatureList features{f};
    check(layer.dataProvider()->addFeatures(features), "add feature");
    layer.updateExtents();
}
int main(int argc, char **argv) {
    QgsApplication app(argc, argv, false);
    QgsApplication::setPrefixPath(QStringLiteral("E:/LTZK/QGIS_32815"), true);
    QgsApplication::initQgis();
    QgsVectorLayer ref("LineString?crs=EPSG:3857&field=LEVEL:string", "ref", "memory");
    add(ref, "LINESTRING (0 0,100 0)", QStringLiteral("三级"));
    add(ref, "LINESTRING (50 -50,50 50)", QStringLiteral("等外"));
    auto a = WaterResultClassification::classify(QgsGeometry::fromWkt("LINESTRING (0 1,100 1)"), ref.crs(), &ref, 2.0);
    check(a.category == QStringLiteral("三级河流"), "crossing must not supersede aligned line");
    auto b = WaterResultClassification::classify(QgsGeometry::fromWkt("LINESTRING (200 0,300 0)"), ref.crs(), &ref, 2.0);
    check(b.category.isEmpty(), "unmatched line must remain unclassified");
    add(ref, "LINESTRING (0 -1,100 -1)", QStringLiteral("六级"));
    auto c = WaterResultClassification::classify(QgsGeometry::fromWkt("LINESTRING (0 -0.5,100 -0.5)"), ref.crs(), &ref, 2.0);
    check(c.category.isEmpty(), "conflicting equally good categories must remain unclassified");
    QgsVectorLayer network("LineString?crs=EPSG:3857&field=LEVEL:string", "network", "memory");
    QgsVectorLayer library("LineString?crs=EPSG:3857&field=LEVEL:string", "library", "memory");
    add(network, "LINESTRING (-100 0,0 0,0 -100)", QStringLiteral("三级"));
    add(network, "LINESTRING (0 0,100 0)", QStringLiteral("三级"));
    add(network, "LINESTRING (100 0,200 0)", QStringLiteral("三级"));
    add(library, "LINESTRING (-100 0,0 0,0 -100)", QStringLiteral("三级"));
    add(library, "LINESTRING (0 0,100 0)", QStringLiteral("三级"));
    add(library, "LINESTRING (100 0,200 0)", QStringLiteral("三级"));
    add(library, "LINESTRING (0 0,100 0)", QStringLiteral("六级"));
    QgsFeature target;
    auto networkIt = network.getFeatures();
    networkIt.nextFeature(target);
    networkIt.nextFeature(target);
    auto tie = WaterResultClassification::classify(target.geometry(), network.crs(), &library, 2.0);
    check(tie.category.isEmpty() && tie.candidates.size() == 2, "candidate tie retained");
    auto resolved = WaterResultClassification::classifyWithNeighbors(target, &network, &library, 2.0, tie);
    check(resolved.category == QStringLiteral("三级河流") && !resolved.evidence.isEmpty(), "two anchors resolve tie");
    add(network, "LINESTRING (0 0,0 -100)", QStringLiteral("六级"));
    auto branch = WaterResultClassification::classifyWithNeighbors(target, &network, &library, 2.0, tie);
    check(branch.category.isEmpty(), "branch must remain unclassified");
    QgsVectorLayer shortRef("LineString?crs=EPSG:3857&field=LEVEL:string", "short", "memory");
    add(shortRef, "LINESTRING (0 0,10 0)", QStringLiteral("等外"));
    add(shortRef, "LINESTRING (0 -100,0 100)", QStringLiteral("六级"));
    auto shortMatch = WaterResultClassification::classify(
        QgsGeometry::fromWkt("LINESTRING (0 0.02,10 0.02)"), shortRef.crs(), &shortRef, 29.0);
    check(shortMatch.category == QStringLiteral("等外河流"), "short line must match aligned river, not endpoint crossing");
    QgsVectorLayer sameClass("LineString?crs=EPSG:3857&field=LEVEL:string", "sameClass", "memory");
    add(sameClass, "LINESTRING (0 0,-100 0)", QStringLiteral("等外"));
    add(sameClass, "LINESTRING (0 0,24 0)", QStringLiteral("等外"));
    add(sameClass, "LINESTRING (0 -100,0 100)", QStringLiteral("六级"));
    auto sameMatch = WaterResultClassification::classify(
        QgsGeometry::fromWkt("LINESTRING (0 0.02,24 0.02)"), sameClass.crs(), &sameClass, 29.0);
    check(sameMatch.category == QStringLiteral("等外河流") && sameMatch.referenceId == 2,
          "must rank all same-category candidates before selecting the reference");
    QgsVectorLayer fallbackRef("LineString?crs=EPSG:3857&field=LEVEL:string", "fallback", "memory");
    add(fallbackRef, "LINESTRING (0 0,25 20,50 0,75 20,100 0)", QStringLiteral("等外"));
    auto fallback = WaterResultClassification::classifyDirectionFallback(
        QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"), fallbackRef.crs(), &fallbackRef, 2.0, 25.0);
    check(fallback.category == QStringLiteral("等外河流"), "whole-river fallback must match a generalized river");
    add(fallbackRef, "LINESTRING (0 0,25 -20,50 0,75 -20,100 0)", QStringLiteral("六级"));
    fallback = WaterResultClassification::classifyDirectionFallback(
        QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"), fallbackRef.crs(), &fallbackRef, 2.0, 25.0);
    check(fallback.category.isEmpty(), "equally supported fallback categories must remain pending");
    QgsVectorLayer crossingRef("LineString?crs=EPSG:3857&field=LEVEL:string", "crossing", "memory");
    add(crossingRef, "LINESTRING (0 -100,0 100)", QStringLiteral("六级"));
    fallback = WaterResultClassification::classifyDirectionFallback(
        QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"), crossingRef.crs(), &crossingRef, 2.0, 25.0);
    check(fallback.category.isEmpty(), "a crossing at one endpoint must not match the entire river");
    QgsVectorLayer sameTie("LineString?crs=EPSG:3857&field=LEVEL:string", "sameTie", "memory");
    add(sameTie, "LINESTRING (0 1,100 1)", QStringLiteral("等外"));
    add(sameTie, "LINESTRING (0 -1,100 -1)", QStringLiteral("等外"));
    auto ambiguousId = WaterResultClassification::classify(
        QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"), sameTie.crs(), &sameTie, 2.0);
    check(ambiguousId.category == QStringLiteral("等外河流") && ambiguousId.referenceId < 0,
          "same category can be known while its reference FID remains ambiguous");
    QgsVectorLayer partial("LineString?crs=EPSG:3857&field=TYPE:string", "partial", "memory");
    add(partial, "LINESTRING (20 0,50 0)", QStringLiteral("支渠"));
    add(partial, "LINESTRING (50 0,80 0)", QStringLiteral("支渠"));
    const auto partialEvidence = WaterResultClassification::partialReferenceEvidence(
        QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"), partial.crs(), &partial, 2.0);
    check(partialEvidence.contains(QStringLiteral("支渠")) &&
          partialEvidence.contains(QStringLiteral("1,2")) &&
          partialEvidence.contains(QStringLiteral("整段待确认")),
          "connected partial references should be reported without inventing a full match");
    QgsVectorLayer split("LineString?crs=EPSG:3857&field=LEVEL:string", "split", "memory");
    add(split, "LINESTRING (0 0,50 0)", QStringLiteral("三级"));
    add(split, "LINESTRING (50 0,100 0)", QStringLiteral("三级"));
    auto chain = WaterReferenceChain::find(QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"),
        split.crs(), &split, QStringLiteral("三级河流"), 2.0);
    check(chain.features.size() == 2, "one source line should match connected reference segments");
    QgsVectorLayer disconnected("LineString?crs=EPSG:3857&field=LEVEL:string", "disconnected", "memory");
    add(disconnected, "LINESTRING (0 0,40 0)", QStringLiteral("三级"));
    add(disconnected, "LINESTRING (60 0,100 0)", QStringLiteral("三级"));
    chain = WaterReferenceChain::find(QgsGeometry::fromWkt("LINESTRING (0 0,100 0)"),
        disconnected.crs(), &disconnected, QStringLiteral("三级河流"), 2.0);
    check(chain.features.isEmpty(), "disconnected references must not form a chain");
    std::cout << "PASS water classification" << std::endl;
    return 0;
}
