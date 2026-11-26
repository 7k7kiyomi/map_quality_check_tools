#include "ui_class/ReadShp.h"
#include <qgsapplication.h>
#include <qgsvectordataprovider.h>
#include <QTemporaryFile>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
static void add(QgsVectorLayer &layer, const QString &wkt) {
    QgsFeature feature(layer.fields());
    feature.setGeometry(QgsGeometry::fromWkt(wkt));
    QgsFeatureList features{feature};
    require(layer.dataProvider()->addFeatures(features), "could not add feature");
    layer.updateExtents();
}
int main(int argc, char **argv) {
    try {
    QgsApplication app(argc, argv, false);
    QgsApplication::setPrefixPath(QStringLiteral("E:/LTZK/QGIS_32815"), true);
    QgsApplication::setPluginPath(QStringLiteral("E:/LTZK/QGIS_32815/plugins"));
    QgsApplication::initQgis();
    ReadShpDialog dialog;
    QTemporaryFile config;
    require(config.open(), "config open");
    config.write(u8R"(<check_list><check_item><name>线要素拓扑检查</name>
      <rule><name>最小线段长度</name><value>1</value><unit>米</unit></rule>
      <rule><name>孤立距离</name><value>2</value><unit>米</unit></rule>
      <rule><name>悬挂线容差</name><value>2</value><unit>米</unit></rule>
      <rule><name>水系要素连接容差</name><value>2</value><unit>米</unit></rule>
      </check_item></check_list>)");
    config.flush();
    require(dialog.m_config->loadFromFile(config.fileName()), "config load");
    dialog.m_mapScale = 1000;
    if (argc > 1) {
        require(dialog.m_config->loadFromFile(QStringLiteral("D:/AAAGSZJTESTMAP/11.10测试XML/三级河流.xml")),
                "actual rule could not be loaded");
        const QString data = QString::fromLocal8Bit(argv[1]);
        QgsVectorLayer actual(data, QStringLiteral("水系_LineString"), QStringLiteral("ogr"));
        require(actual.isValid(), "actual river layer could not be loaded");
        dialog.parseFilePaths = QStringList{data};
        const auto actualResults = dialog.checkRiverIsolation(&actual, data);
        bool found28 = false;
        for (const auto &result : actualResults) {
            if (result.featureId == 28) {
                found28 = true;
                std::cout << "FID28: " << result.description.toUtf8().constData() << std::endl;
            }
        }
        std::cout << "isolated count: " << actualResults.size() << "; FID28 found: " << found28 << std::endl;
        return 0;
    }
    QgsVectorLayer lines("LineString?crs=EPSG:3857", "water", "memory");
    add(lines, "LINESTRING (0 0,100 0)");
    add(lines, "LINESTRING (200 0,300 0)");
    add(lines, "LINESTRING (250 -30,250 30)");
    const auto results = dialog.checkRiverIsolation(&lines, QStringLiteral("水系_LineString.shp"));
    const auto hanging = dialog.checkRiverDangling(&lines, QStringLiteral("水系_LineString.shp"));
    require(!hanging.empty() && hanging.front().feature.isValid() && hanging.front().feature.hasGeometry(),
            "dangling errors must retain the source feature for classification");
    require(results.size() == 1, "long isolated line must be found; lines crossing in the middle must not");
    require(results.front().featureId == 1, "incorrect isolated feature");
    QgsVectorLayer shortLine("LineString?crs=EPSG:3857", "short", "memory");
    add(shortLine, "LINESTRING (0 0,0.5 0)");
    auto single = dialog.checkRiverIsolation(&shortLine, QStringLiteral("水系_LineString.shp"));
    require(single.size() == 1 && single.front().calculatedValue == -1,
            "a single short isolated line should have no invented nearest distance");
    QgsFeature area;
    area.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POLYGON ((40 -5,60 -5,60 5,40 5,40 -5))")));
    require(dialog.filterConnectedToWaterSystemStrict(results, {area}).empty(),
            "waterbody intersecting the middle must exclude an isolated result");
    QualityCheckResult dangling{};
    dangling.passed = false;
    dangling.checkType = CheckType::LINE_TOPOLOGY_ERROR;
    dangling.sourceFile = QStringLiteral("水系_LineString.shp");
    dangling.featureId = 1;
    dangling.errorType = QStringLiteral("河流悬挂");
    dangling.description = QStringLiteral("起点和终点都悬挂");
    auto merged = results;
    merged.front().referenceCategory = QStringLiteral("三级河流");
    merged.front().referenceSource = QStringLiteral("HYDL_HL.shp");
    merged.front().referenceFeatureId = 17;
    merged.insert(merged.begin(), dangling);
    mergeDuplicateLineTopologyErrorRecords(merged);
    require(merged.size() == 1 && merged[0].errorType == QStringLiteral("河流独立") &&
            merged[0].description.contains(QStringLiteral("起点和终点都悬挂")),
            "isolated and two-ended dangling results must merge without losing details");
    require(merged[0].referenceCategory == QStringLiteral("三级河流") &&
            merged[0].referenceFeatureId == 17,
            "merged error must retain reference classification from another issue");
    QualityCheckResult networkDangling = dangling;
    networkDangling.checkType = CheckType::NETWORK_CONNECTIVITY_ERROR;
    networkDangling.description = dangling.description;
    auto duplicateHanging = std::vector<QualityCheckResult>{dangling, networkDangling};
    mergeDuplicateLineTopologyErrorRecords(duplicateHanging);
    require(duplicateHanging.size() == 1, "same hanging issue from both checks must merge");
    QualityCheckResult firstJunction = networkDangling;
    firstJunction.description = QStringLiteral("成果接点Point (0 0)，要素 10");
    QualityCheckResult secondJunction = networkDangling;
    secondJunction.description = QStringLiteral("成果接点Point (100 0)，要素 20");
    auto junctions = std::vector<QualityCheckResult>{firstJunction, secondJunction};
    mergeDuplicateLineTopologyErrorRecords(junctions);
    require(junctions.size() == 2, "different junction issues must remain separate");
    std::cout << "PASS river isolation" << std::endl;
    return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
