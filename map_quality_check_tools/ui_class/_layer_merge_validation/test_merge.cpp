#include "quality_result_merge.h"
#include <qgsapplication.h>
#include <iostream>
#include <cstdlib>
#define REQUIRE(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << std::endl; std::exit(1); } } while (0)
using namespace QualityResultMerge;
static void write(const QString& path, const QByteArray& data) {
    QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly)); REQUIRE(file.write(data) == data.size());
}
static Dataset open(const QString& path) {
    return Dataset(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr)), GDALClose);
}
int main(int argc, char** argv) {
    QgsApplication app(argc, argv, false);
    QgsApplication::initQgis();
    GDALAllRegister();
    QTemporaryDir run(QDir::current().filePath(QStringLiteral("merge_test_XXXXXX")));
    REQUIRE(run.isValid());
    const QString a = run.filePath(QStringLiteral("甲错误.geojson"));
    const QString b = run.filePath(QStringLiteral("乙错误.geojson"));
    const QString c = run.filePath(QStringLiteral("混合错误.geojson"));
    write(a, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"detail":"中文错误描述","chk_name":"原始字段","only_a":12},"geometry":{"type":"LineString","coordinates":[[0,0],[1,1]]}}]})");
    write(b, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"detail":"第二项","only_b":"B"},"geometry":{"type":"MultiLineString","coordinates":[[[2,2],[3,3]],[[4,4],[5,5]]]}}]})");
    write(c, R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"detail":"混合"},"geometry":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[1,2]},{"type":"MultiPoint","coordinates":[[2,3],[3,4]]},{"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,0]]]}]}}]})");
    const QString root = run.filePath(QStringLiteral("地图/质检错误结果"));
    QVector<Source> sources{{a, QStringLiteral("道路"), QStringLiteral("重叠检查"), QStringLiteral("高")},
        {b, QStringLiteral("道路"), QStringLiteral("自相交检查"), QStringLiteral("高")},
        {c, QStringLiteral("道路"), QStringLiteral("混合检查"), QStringLiteral("高")},
        {a, QStringLiteral("道路"), QStringLiteral("中级检查"), QStringLiteral("中")},
        {a, QStringLiteral("水系"), QStringLiteral("水系检查"), QStringLiteral("高")}};
    auto result = merge(root, sources, QgsProject::instance());
    for (const auto& error : result.errors) std::cerr << error.toStdString() << std::endl;
    REQUIRE(result.errors.isEmpty()); REQUIRE(result.paths.size() == 5);
    const QString highLine = QDir(root).filePath(QStringLiteral("高严重程度/道路_错误汇总_线.shp"));
    {
        auto dataset = open(highLine); REQUIRE(dataset); auto* layer = dataset->GetLayer(0);
        REQUIRE(layer->GetFeatureCount() == 2); REQUIRE(family(layer->GetGeomType()) == 1);
        REQUIRE(layer->GetSpatialRef()); REQUIRE(layer->GetLayerDefn()->GetFieldIndex("only_a") >= 0);
        REQUIRE(layer->GetLayerDefn()->GetFieldIndex("only_b") >= 0);
        Feature feature(layer->GetNextFeature(), OGRFeature::DestroyFeature);
        REQUIRE(QString::fromUtf8(feature->GetFieldAsString("chk_name")) == QStringLiteral("重叠检查"));
        REQUIRE(QString::fromUtf8(feature->GetFieldAsString("detail")) == QStringLiteral("中文错误描述"));
        REQUIRE(QString::fromUtf8(feature->GetFieldAsString("chk_na1")) == QStringLiteral("原始字段"));
    }
    {
        auto dataset = open(QDir(root).filePath(QStringLiteral("高严重程度/道路_错误汇总_点.shp")));
        REQUIRE(dataset); auto* layer = dataset->GetLayer(0); REQUIRE(layer->GetFeatureCount() == 1);
        Feature feature(layer->GetNextFeature(), OGRFeature::DestroyFeature);
        REQUIRE(feature->GetGeometryRef()->toGeometryCollection()->getNumGeometries() == 3);
    }
    REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(), result.paths) == 5);
    REQUIRE(QgsProject::instance()->mapLayers().size() == 5);
    sources.removeLast(); sources.removeLast(); sources.removeLast();
    result = merge(root, sources, QgsProject::instance());
    REQUIRE(result.errors.isEmpty()); REQUIRE(result.paths.size() == 1);
    REQUIRE(!QFileInfo::exists(QDir(root).filePath(QStringLiteral("高严重程度/道路_错误汇总_点.shp"))));
    REQUIRE(QgsProject::instance()->mapLayers().isEmpty());
    // A failed rerun preserves the last valid summary.
    auto failed = merge(root, {{run.filePath(QStringLiteral("missing.shp")), QStringLiteral("道路"), QStringLiteral("失败"), QStringLiteral("高")}}, QgsProject::instance());
    REQUIRE(!failed.errors.isEmpty()); REQUIRE(QFileInfo::exists(highLine));
    {
        auto dataset = open(highLine); REQUIRE(dataset); REQUIRE(dataset->GetLayer(0)->GetFeatureCount() == 2);
    }
    result = merge(root, {}, QgsProject::instance());
    REQUIRE(result.errors.isEmpty()); REQUIRE(result.paths.isEmpty()); REQUIRE(!QFileInfo::exists(highLine));
    result = merge(root, {{c, QStringLiteral("特殊/图层"), QStringLiteral("低级检查"), QStringLiteral("低")},
                          {a, QStringLiteral("未配置图层"), QStringLiteral("未配置检查"), QString()}}, QgsProject::instance());
    REQUIRE(result.errors.isEmpty()); REQUIRE(result.paths.size() == 3);
    for (const QString& path : result.paths) {
        REQUIRE(QFileInfo::exists(path)); REQUIRE(!QFileInfo(path).fileName().contains(QLatin1Char('/')));
    }
    const QStringList lastPaths = result.paths;
    result = merge(root, {}, QgsProject::instance());
    REQUIRE(result.errors.isEmpty());
    for (const QString& path : lastPaths) REQUIRE(!QFileInfo::exists(path));
    // Production cleanup: explicit per-check SHP/GeoJSON outputs inside the
    // result directory disappear, including loaded layers and SHP companions.
    const QString detailShp = QDir(root).filePath(QStringLiteral("高严重程度/分项错误.shp"));
    const QString detailJson = QDir(root).filePath(QStringLiteral("高严重程度/第二项错误.geojson"));
    {
        Dataset dataset(GetGDALDriverManager()->GetDriverByName("ESRI Shapefile")->Create(
            detailShp.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr), GDALClose);
        REQUIRE(dataset);
        OGRSpatialReference srs; srs.importFromEPSG(4326);
        auto* layer = dataset->CreateLayer("errors", &srs, wkbPoint, nullptr); REQUIRE(layer);
        Feature feature(OGRFeature::CreateFeature(layer->GetLayerDefn()), OGRFeature::DestroyFeature);
        OGRPoint point(1, 2); feature->SetGeometry(&point); REQUIRE(layer->CreateFeature(feature.get()) == OGRERR_NONE);
    }
    REQUIRE(QFile::copy(b, detailJson));
    write(QFileInfo(detailShp).dir().filePath(QStringLiteral("分项错误.qml")), QByteArray("style"));
    REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(), {detailShp, detailJson}) == 2);
    result = merge(root, {{detailShp, QStringLiteral("道路"), QStringLiteral("点检查"), QStringLiteral("高")},
                          {detailJson, QStringLiteral("道路"), QStringLiteral("线检查"), QStringLiteral("高")},
                          {a, QStringLiteral("外部输入"), QStringLiteral("外部测试"), QStringLiteral("低")}}, QgsProject::instance());
    REQUIRE(result.errors.isEmpty()); REQUIRE(result.paths.size() == 3);
    REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(), result.paths) == 3);
    REQUIRE(QgsProject::instance()->mapLayers().size() == 5);
    auto bad = merge(root, {{run.filePath(QStringLiteral("missing.shp")), QStringLiteral("道路"), QStringLiteral("失败"), QStringLiteral("高")}}, QgsProject::instance());
    REQUIRE(removeMergedSources(root, bad, QgsProject::instance()).isEmpty());
    REQUIRE(QFileInfo::exists(detailShp)); REQUIRE(QFileInfo::exists(detailJson));
    const auto removed = removeMergedSources(root, result, QgsProject::instance());
    REQUIRE(result.errors.isEmpty()); REQUIRE(removed.size() == 2);
    REQUIRE(!QFileInfo::exists(detailShp)); REQUIRE(!QFileInfo::exists(detailJson));
    REQUIRE(sidecars(detailShp).isEmpty()); REQUIRE(QgsProject::instance()->mapLayers().size() == 3);
    for (const QString& path : result.paths) {
        auto dataset = open(path); REQUIRE(dataset); REQUIRE(dataset->GetLayer(0)->GetFeatureCount() == 1);
    }
    REQUIRE(result.replacements.value(detailShp).first().endsWith(QStringLiteral("_点.shp")));
    REQUIRE(result.replacements.value(detailJson).first().endsWith(QStringLiteral("_线.shp")));
    // New water checks first classify by library category. Both check types
    // must then join that category's severity/geometry summary; pending stays
    // in its own category rather than being attributed to a source task.
    const QString waterRoot = run.filePath(QStringLiteral("水系分类/质检错误结果"));
    QDir().mkpath(waterRoot);
    const QString topology = QDir(waterRoot).filePath(QStringLiteral("线要素拓扑检查_等外河流_errors_lines.geojson"));
    const QString direction = QDir(waterRoot).filePath(QStringLiteral("几何走向异常检查_等外河流_errors_lines.geojson"));
    const QString pending = QDir(waterRoot).filePath(QStringLiteral("线要素拓扑检查_水系待归类_errors_points.geojson"));
    REQUIRE(QFile::copy(a, topology)); REQUIRE(QFile::copy(b, direction)); REQUIRE(QFile::copy(c, pending));
    auto water = merge(waterRoot, {
        {topology, QStringLiteral("等外河流"), QStringLiteral("线要素拓扑检查"), QStringLiteral("中")},
        {direction, QStringLiteral("等外河流"), QStringLiteral("几何走向异常检查"), QStringLiteral("中")},
        {pending, QStringLiteral("水系待归类"), QStringLiteral("线要素拓扑检查"), QStringLiteral("中")}}, QgsProject::instance());
    REQUIRE(water.errors.isEmpty()); REQUIRE(water.paths.size() == 3);
    const QString waterLine = QDir(waterRoot).filePath(QStringLiteral("中严重程度/等外河流_错误汇总_线.shp"));
    {
        auto dataset = open(waterLine); REQUIRE(dataset); REQUIRE(dataset->GetLayer(0)->GetFeatureCount() == 2);
        QSet<QString> checks; Feature feature(nullptr, OGRFeature::DestroyFeature);
        while (feature.reset(dataset->GetLayer(0)->GetNextFeature()), feature)
            checks.insert(QString::fromUtf8(feature->GetFieldAsString("chk_name")));
        REQUIRE(checks.contains(QStringLiteral("线要素拓扑检查")));
        REQUIRE(checks.contains(QStringLiteral("几何走向异常检查")));
    }
    REQUIRE(QFileInfo::exists(QDir(waterRoot).filePath(QStringLiteral("中严重程度/水系待归类_错误汇总_点.shp"))));
    REQUIRE(removeMergedSources(waterRoot, water, QgsProject::instance()).size() == 3);
    REQUIRE(!QFileInfo::exists(topology)); REQUIRE(!QFileInfo::exists(direction)); REQUIRE(!QFileInfo::exists(pending));
    // One broken source/group must not suppress all other layers, and only
    // successfully merged sources may be removed.
    const QString partialRoot = run.filePath(QStringLiteral("部分失败/质检错误结果"));
    QDir().mkpath(partialRoot);
    const QString good = QDir(partialRoot).filePath(QStringLiteral("有效错误.geojson"));
    REQUIRE(QFile::copy(b, good));
    const QString empty = QDir(partialRoot).filePath(QStringLiteral("有效错误.shp"));
    {
        Dataset dataset(GetGDALDriverManager()->GetDriverByName("ESRI Shapefile")->Create(
            empty.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr), GDALClose);
        REQUIRE(dataset); REQUIRE(dataset->CreateLayer("errors", nullptr, wkbPoint, nullptr));
    }
    auto partial = merge(partialRoot, {
        {good, QStringLiteral("有效图层"), QStringLiteral("有效检查"), QStringLiteral("高")},
        {empty, QStringLiteral("有效图层"), QStringLiteral("空检查"), QStringLiteral("高")},
        {run.filePath(QStringLiteral("missing.shp")), QStringLiteral("损坏图层"), QStringLiteral("损坏检查"), QStringLiteral("高")}}, QgsProject::instance());
    REQUIRE(!partial.errors.isEmpty()); REQUIRE(partial.paths.size() == 1);
    REQUIRE(partial.replacements.size() == 2);
    REQUIRE(removeMergedSources(partialRoot, partial, QgsProject::instance()).size() == 2);
    REQUIRE(!QFileInfo::exists(empty));
    REQUIRE(!QFileInfo::exists(good)); REQUIRE(QFileInfo::exists(partial.paths.first()));
    if (argc > 1) {
        const QDir real(QString::fromLocal8Bit(argv[1]));
        const QString realRoot = run.filePath(QStringLiteral("真实结果验证/质检错误结果"));
        auto actual = merge(realRoot, {
            {real.filePath(QStringLiteral("高严重程度/注记字体_地理名称_标注_errors.shp")), QStringLiteral("地理名称_标注"), QStringLiteral("注记字体"), QStringLiteral("高")},
            {real.filePath(QStringLiteral("高严重程度/注记颜色_地理名称_标注_errors.shp")), QStringLiteral("地理名称_标注"), QStringLiteral("注记颜色"), QStringLiteral("高")},
            {real.filePath(QStringLiteral("境界线色带_晕带_inner_errors.shp")), QStringLiteral("晕带"), QStringLiteral("境界线色带"), QStringLiteral("高")},
            {real.filePath(QStringLiteral("高严重程度/符号方向性检查_隧道口_errors.shp")), QStringLiteral("隧道口"), QStringLiteral("方向"), QStringLiteral("高")},
            {real.filePath(QStringLiteral("高严重程度/关系检查_隧道口_errors.shp")), QStringLiteral("隧道口"), QStringLiteral("关系"), QStringLiteral("高")}}, QgsProject::instance());
        for (const QString& error : actual.errors) std::cerr << error.toUtf8().constData() << std::endl;
        REQUIRE(!actual.errors.isEmpty()); REQUIRE(!actual.paths.isEmpty());
        for (const QString& path : actual.paths) {
            auto dataset = open(path); REQUIRE(dataset); REQUIRE(dataset->GetLayer(0)->GetFeatureCount() > 0);
        }
        REQUIRE(removeMergedSources(realRoot, actual, QgsProject::instance()).isEmpty());
        std::cout << "PASS: actual Yongdeng outputs reproduced unreadable/CRS failures; valid annotation layer still merged; original real files unchanged" << std::endl;
    }
    REQUIRE(QFileInfo::exists(a)); REQUIRE(QFileInfo::exists(b)); REQUIRE(QFileInfo::exists(c));
    std::cout << "PASS: high/medium/low/unconfigured severity and layer isolation, point/line/polygon splitting, single/multi and collections, Unicode and safe filenames, union attributes, provenance, loaded-file replacement, stale cleanup, zero errors, failed-run preservation" << std::endl;
    std::cout << "PASS: summary-only final output, source-to-summary references, loaded detail-layer removal, SHP sidecar and GeoJSON cleanup, outside-root input protection, failed-merge source preservation" << std::endl;
    std::cout << "PASS: classified water topology and direction results merge by category, severity and geometry; pending category is separate" << std::endl;
    QgsProject::instance()->clear();
    QgsApplication::exitQgis();
}
