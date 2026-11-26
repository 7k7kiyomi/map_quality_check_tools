#include "map_check_algorithms.h"
#include "point_isolation_output.h"
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextCodec>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_quad_tree.h>
#include <cpl_string.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
struct GeometryDelete { void operator()(OGRGeometry* p) const { OGRGeometryFactory::destroyGeometry(p); } };
struct DatasetDelete { void operator()(GDALDataset* p) const { if (p) GDALClose(p); } };
struct FeatureDelete { void operator()(OGRFeature* p) const { OGRFeature::DestroyFeature(p); } };
struct SrsDelete { void operator()(OGRSpatialReference* p) const { if (p) p->Release(); } };
struct TransformDelete { void operator()(OGRCoordinateTransformation* p) const { if (p) OCTDestroyCoordinateTransformation(p); } };
using Geometry = std::unique_ptr<OGRGeometry, GeometryDelete>;
using Dataset = std::unique_ptr<GDALDataset, DatasetDelete>;
using Srs = std::unique_ptr<OGRSpatialReference, SrsDelete>;

void fail(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }

QString canonical(const QString& path)
{
    QFileInfo f(path);
    return QDir::cleanPath(f.exists() ? f.canonicalFilePath() : f.absoluteFilePath()).toLower();
}

QString family(const QString& path)
{
    QString base = QFileInfo(path).completeBaseName();
    base.remove(QRegularExpression(QStringLiteral("_(Point|LineString|Polygon)$"), QRegularExpression::CaseInsensitiveOption));
    return canonical(QFileInfo(path).absolutePath() + QLatin1Char('/') + base);
}

QStringList list(const QString& text)
{
    QStringList out;
    for (QString part : text.split(QRegularExpression(QStringLiteral("[;；,，、|]+")), QString::SkipEmptyParts)) {
        part = part.trimmed();
        if (!part.isEmpty() && !out.contains(part)) out << part;
    }
    return out;
}

QStringList csvCells(const QString& line)
{
    QStringList cells;
    QString cell;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line[i];
        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size() && line[i + 1] == QLatin1Char('"')) { cell += ch; ++i; }
            else quoted = !quoted;
        } else if (ch == QLatin1Char(',') && !quoted) { cells << cell.trimmed(); cell.clear(); }
        else cell += ch;
    }
    if (quoted) fail(QStringLiteral("图层配置 CSV 含未闭合引号"));
    cells << cell.trimmed();
    return cells;
}

struct MappingRow { QStringList actual, gdb; };

QMap<QString, MappingRow> readMapping(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取图层配置 CSV：%1").arg(path));
    QByteArray bytes = file.readAll();
    QTextCodec::ConverterState state;
    QString text = QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars) text = QTextCodec::codecForName("GB18030")->toUnicode(bytes);
    if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
    QMap<QString, MappingRow> result;
    for (const QString& row : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), QString::SkipEmptyParts)) {
        if (row.trimmed().startsWith(QLatin1Char('#'))) continue;
        const auto cells = csvCells(row);
        if (cells.size() < 3 || cells[0] == QStringLiteral("标准图层")) continue;
        MappingRow mapping{list(cells[2]), list(cells[1])};
        if (result.contains(cells[0]) && (result[cells[0]].actual != mapping.actual || result[cells[0]].gdb != mapping.gdb))
            fail(QStringLiteral("图层配置存在冲突的标准图层：%1").arg(cells[0]));
        result[cells[0]] = mapping;
    }
    if (result.isEmpty()) fail(QStringLiteral("图层配置未包含可用的标准图层映射"));
    return result;
}

struct Config {
    QString xml, target, grouping = QStringLiteral("symbol_id"), severity;
    QStringList associations, expectedGdb;
    double toleranceMetres = 0;
};

Config readConfig(const AlgorithmFilePaths& paths, int scale)
{
    QStringList xmls = paths.xmlPaths;
    std::stable_sort(xmls.begin(), xmls.end(), [&](const QString& a, const QString& b) {
        return (QFileInfo(a).completeBaseName() == paths.standardLayer)
             > (QFileInfo(b).completeBaseName() == paths.standardLayer);
    });
    for (const QString& xml : xmls) {
        QFile file(xml);
        if (!file.open(QIODevice::ReadOnly)) fail(QStringLiteral("无法读取 XML：%1").arg(xml));
        QDomDocument doc;
        QString error; int line = 0, column = 0;
        if (!doc.setContent(&file, &error, &line, &column))
            fail(QStringLiteral("XML 解析失败：%1，第 %2 行：%3").arg(xml).arg(line).arg(error));
        if (doc.documentElement().tagName() != QStringLiteral("check_list"))
            fail(QStringLiteral("XML 根节点必须是 check_list"));
        QDomElement chosen;
        int bestRank = -1;
        const auto scales = doc.documentElement().elementsByTagName(QStringLiteral("scale"));
        for (int i = 0; i < scales.size(); ++i) {
            const auto block = scales.at(i).toElement();
            const QString value = block.firstChildElement(QStringLiteral("value")).text().trimmed();
            bool ok = false;
            const int denominator = value.toInt(&ok);
            int rank = value.isEmpty() ? 0 : ((ok && denominator == scale && scale > 0) ? 1 : -1);
            if (rank < 0) continue;
            for (auto item = block.firstChildElement(QStringLiteral("check_item")); !item.isNull();
                 item = item.nextSiblingElement(QStringLiteral("check_item"))) {
                const QString id = item.firstChildElement(QStringLiteral("id")).text().trimmed();
                const QString name = item.firstChildElement(QStringLiteral("name")).text().trimmed();
                if (id != QStringLiteral("118") && name != QStringLiteral("点要素孤立检查")) continue;
                if (id != QStringLiteral("118") || name != QStringLiteral("点要素孤立检查"))
                    fail(QStringLiteral("点要素孤立检查的名称和 ID=118 不一致"));
                if (rank == bestRank) fail(QStringLiteral("同一比例尺下存在重复的点要素孤立检查规则"));
                if (rank > bestRank) { chosen = item; bestRank = rank; }
            }
        }
        if (chosen.isNull()) continue;
        QMap<QString, QString> values, units;
        for (auto rule = chosen.firstChildElement(QStringLiteral("rule")); !rule.isNull();
             rule = rule.nextSiblingElement(QStringLiteral("rule"))) {
            QString key = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
            if (values.contains(key)) fail(QStringLiteral("XML 存在重复参数：%1").arg(key));
            values[key] = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
            units[key] = rule.firstChildElement(QStringLiteral("unit")).text().trimmed().toLower();
        }
        Config cfg;
        cfg.xml = xml;
        cfg.target = values.value(QStringLiteral("检查图层"));
        cfg.associations = list(values.value(QStringLiteral("关联图层")));
        cfg.expectedGdb = list(values.value(QStringLiteral("对应GDB图层")));
        cfg.severity = chosen.firstChildElement(QStringLiteral("severity")).text().trimmed();
        if (values.contains(QStringLiteral("要素分组字段"))) cfg.grouping = values[QStringLiteral("要素分组字段")];
        if (cfg.target.isEmpty() || cfg.associations.isEmpty()) fail(QStringLiteral("XML 缺少检查图层或关联图层"));
        const QString position = values.value(QStringLiteral("连接位置"), QStringLiteral("任意位置"));
        if (position != QStringLiteral("任意位置")) fail(QStringLiteral("当前算法仅支持连接位置为任意位置"));
        const QString mode = values.value(QStringLiteral("几何处理方式"), QStringLiteral("面优先并恢复闭合填充路径"));
        if (mode != QStringLiteral("面优先并恢复闭合填充路径")) fail(QStringLiteral("不支持的几何处理方式：%1").arg(mode));
        bool ok = false;
        const double tolerance = values.value(QStringLiteral("连接容差")).toDouble(&ok);
        if (!ok || !std::isfinite(tolerance) || tolerance < 0) fail(QStringLiteral("连接容差必须为有限的非负数"));
        const QString unit = units[QStringLiteral("连接容差")];
        if (unit == QStringLiteral("mm") || unit == QStringLiteral("毫米")) {
            if (scale <= 0) fail(QStringLiteral("图上毫米容差需要有效的任务比例尺"));
            cfg.toleranceMetres = tolerance * scale / 1000.0;
        } else if (unit == QStringLiteral("m") || unit == QStringLiteral("米")) cfg.toleranceMetres = tolerance;
        else fail(QStringLiteral("不支持的连接容差单位：%1").arg(unit));
        if (!std::isfinite(cfg.toleranceMetres)) fail(QStringLiteral("连接容差换算后超出数值范围"));
        return cfg;
    }
    fail(QStringLiteral("未找到适用于当前比例尺的点要素孤立检查（118）规则"));
    return Config{};
}

enum class Role { Symbol, Line, Area, Any };
struct Source { QString path; Role role; bool target = false; bool gdb = false; };

QStringList resolve(const QString& root, const QString& actual, Role role)
{
    QStringList paths;
    const QStringList suffixes = {QString(), QStringLiteral("_LineString"), QStringLiteral("_Polygon")};
    for (const auto& suffix : suffixes) {
        if (role == Role::Line && suffix == QStringLiteral("_Polygon")) continue;
        const QString path = QDir(root).filePath(actual + suffix + QStringLiteral(".shp"));
        if (QFileInfo::exists(path)) paths << path;
    }
    return paths;
}

struct Part {
    QString path, key, symbol, elemId;
    GIntBig fid = -1;
    Geometry geom;
    bool filled = false;
};
struct Object {
    QString key, symbol, elemId;
    QStringList paths;
    QJsonArray fids;
    Geometry geom;
    OGREnvelope bounds;
    bool boundsKnown = false;
    bool symbolFromFilledLines = false;
    QString problem;
};

bool finiteBounds(const OGREnvelope& e)
{
    return std::isfinite(e.MinX) && std::isfinite(e.MinY)
        && std::isfinite(e.MaxX) && std::isfinite(e.MaxY);
}

std::vector<Part> readSource(const Source& source, const Config& cfg, Srs& working)
{
    Dataset ds(static_cast<GDALDataset*>(GDALOpenEx(source.path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                                  nullptr, nullptr, nullptr)));
    if (!ds || ds->GetLayerCount() != 1) fail(QStringLiteral("无法打开单图层成果 SHP：%1").arg(source.path));
    OGRLayer* layer = ds->GetLayer(0);
    auto* rawSrs = layer->GetSpatialRef();
    if (!rawSrs) fail(QStringLiteral("成果图层缺少坐标系：%1").arg(source.path));
    Srs local(rawSrs->Clone());
    local->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (!working) {
        if (!local->IsProjected() && !local->IsLocal()) fail(QStringLiteral("待检图层需要采用具有长度单位的投影坐标系"));
        working.reset(local->Clone());
        if (!std::isfinite(working->GetLinearUnits()) || working->GetLinearUnits() <= 0)
            fail(QStringLiteral("待检图层的坐标单位无效"));
    }
    std::unique_ptr<OGRCoordinateTransformation, TransformDelete> transform;
    if (!local->IsSame(working.get())) {
        transform.reset(OGRCreateCoordinateTransformation(local.get(), working.get()));
        if (!transform) fail(QStringLiteral("无法将图层转换到待检坐标系：%1").arg(source.path));
    }
    std::vector<Part> parts;
    layer->ResetReading();
    CPLErrorReset();
    while (auto* rawFeature = layer->GetNextFeature()) {
        std::unique_ptr<OGRFeature, FeatureDelete> f(rawFeature);
        Part p;
        p.path = source.path;
        p.fid = f->GetFID();
        int groupIndex = source.gdb || cfg.grouping.isEmpty()
            ? -1 : f->GetFieldIndex(cfg.grouping.toUtf8().constData());
        if (groupIndex >= 0 && f->IsFieldSetAndNotNull(groupIndex))
            p.symbol = QString::fromUtf8(f->GetFieldAsString(groupIndex)).trimmed();
        if (p.symbol.isEmpty()) {
            p.key = canonical(p.path);
            p.key.append(QStringLiteral("#FID:%1").arg(p.fid));
        } else {
            p.key = family(p.path);
            p.key.append(QStringLiteral("#SYMBOL:"));
            p.key.append(p.symbol);
        }
        const int filled = f->GetFieldIndex("filled");
        if (filled >= 0) {
            const QString v = QString::fromUtf8(f->GetFieldAsString(filled)).trimmed().toLower();
            p.filled = v == QStringLiteral("true") || v == QStringLiteral("1") || v == QStringLiteral("yes");
        }
        const int elem = f->GetFieldIndex("ELEMID");
        if (elem >= 0) p.elemId = QString::fromUtf8(f->GetFieldAsString(elem)).trimmed();
        if (f->GetGeometryRef()) {
            p.geom.reset(f->GetGeometryRef()->clone());
            p.geom->flattenTo2D();
            if (transform && p.geom->transform(transform.get()) != OGRERR_NONE)
                fail(QStringLiteral("要素坐标转换失败：%1，FID=%2").arg(p.path).arg(p.fid));
            p.geom->assignSpatialReference(working.get());
        }
        parts.push_back(std::move(p));
    }
    if (CPLGetLastErrorType() >= CE_Failure) fail(QStringLiteral("读取要素失败：%1").arg(source.path));
    return parts;
}

bool polygonal(const OGRGeometry* geom)
{
    const auto t = wkbFlatten(geom->getGeometryType());
    return t == wkbPolygon || t == wkbMultiPolygon;
}

Geometry filledSurface(const OGRGeometry* geom)
{
    const auto type = wkbFlatten(geom->getGeometryType());
    if (type == wkbLineString) {
        const auto* line = static_cast<const OGRLineString*>(geom);
        OGRLinearRing ring;
        for (int i = 0; i < line->getNumPoints(); ++i) {
            if (i && line->getX(i) == line->getX(i - 1) && line->getY(i) == line->getY(i - 1)) continue;
            ring.addPoint(line->getX(i), line->getY(i));
        }
        const int n = ring.getNumPoints();
        if (n < 4 || ring.getX(0) != ring.getX(n - 1) || ring.getY(0) != ring.getY(n - 1)) return Geometry();
        auto polygon = std::unique_ptr<OGRPolygon>(new OGRPolygon());
        if (polygon->addRing(&ring) != OGRERR_NONE || !polygon->IsValid() || polygon->get_Area() <= 0) return Geometry();
        return Geometry(polygon.release());
    }
    // Nested linear rings require a known fill rule; never guess which one is a hole.
    return Geometry();
}

Object buildObject(const std::vector<Part*>& parts, Role role)
{
    Object out;
    out.key = parts[0]->key; out.symbol = parts[0]->symbol; out.elemId = parts[0]->elemId;
    bool hasPolygon = false;
    for (const auto* p : parts) {
        out.fids.append(static_cast<double>(p->fid));
        if (!out.paths.contains(p->path)) out.paths << p->path;
        if (p->geom && !p->geom->IsEmpty()) {
            OGREnvelope box; p->geom->getEnvelope(&box);
            if (finiteBounds(box)) {
                if (!out.boundsKnown) { out.bounds = box; out.boundsKnown = true; }
                else out.bounds.Merge(box);
            }
            hasPolygon |= polygonal(p->geom.get());
        }
    }
    out.symbolFromFilledLines = role == Role::Symbol && !hasPolygon;
    std::vector<Geometry> faces;
    for (const auto* p : parts) {
        if (!p->geom || p->geom->IsEmpty()) { out.problem = QStringLiteral("要素几何为空"); break; }
        Geometry face;
        if (role == Role::Any) {
            face.reset(p->geom->clone());
        } else if (role == Role::Line) {
            const auto t = wkbFlatten(p->geom->getGeometryType());
            if (t != wkbLineString && t != wkbMultiLineString) { out.problem = QStringLiteral("水系线图层含非线几何"); break; }
            face.reset(p->geom->clone());
        } else if (polygonal(p->geom.get())) {
            if (!p->geom->IsValid()) { out.problem = QStringLiteral("面几何无效"); break; }
            face.reset(p->geom->clone());
        } else if (!hasPolygon && p->filled) {
            face = filledSurface(p->geom.get());
            if (!face) { out.problem = QStringLiteral("填充路径未闭合、退化或无法可靠恢复为面"); break; }
        } else continue; // A symbol's outline never becomes another logical reservoir.
        if (!out.boundsKnown) { out.problem = QStringLiteral("几何坐标不是有限数值"); break; }
        if (role != Role::Line && role != Role::Any) {
            for (const auto& existing : faces) {
                Geometry delta(existing->SymDifference(face.get()));
                if (!delta) { out.problem = QStringLiteral("面几何比较失败"); break; }
                if (!delta->IsEmpty() && (existing->Contains(face.get()) || face->Contains(existing.get())) && !hasPolygon) {
                    out.problem = QStringLiteral("多个嵌套填充路径的孔洞语义不明确"); break;
                }
            }
            if (!out.problem.isEmpty()) break;
        }
        faces.push_back(std::move(face));
    }
    if (out.problem.isEmpty() && faces.empty()) out.problem = QStringLiteral("没有可用面几何或闭合填充路径");
    if (out.problem.isEmpty()) {
        out.geom = std::move(faces[0]);
        for (size_t i = 1; i < faces.size(); ++i) {
            Geometry merged(out.geom->Union(faces[i].get()));
            if (!merged || merged->IsEmpty()) { out.problem = QStringLiteral("同一对象的几何合并失败"); break; }
            out.geom = std::move(merged);
        }
        if (out.problem.isEmpty() && (!out.geom->IsValid()
            || (role != Role::Line && role != Role::Any && !polygonal(out.geom.get()))))
            out.problem = QStringLiteral("恢复后的对象几何无效");
    }
    if (!out.problem.isEmpty()) out.geom.reset();
    else out.geom->getEnvelope(&out.bounds);
    return out;
}

std::vector<Object> objects(std::vector<Part>& parts, Role role)
{
    QMap<QString, std::vector<Part*>> groups;
    for (auto& p : parts) groups[p.key].push_back(&p);
    std::vector<Object> result;
    for (auto it = groups.begin(); it != groups.end(); ++it) result.push_back(buildObject(it.value(), role));
    return result;
}

// Parsed water surfaces can export their stroke into a separate file without
// carrying a symbol ID. Remove only a non-filled line wholly covered by a
// valid native surface boundary. An overlapping/contained filled face is kept.
int removeKnownBoundaryStrokes(std::vector<Part>& parts)
{
    std::vector<const Part*> surfaces;
    for (const auto& part : parts)
        if (part.geom && polygonal(part.geom.get()) && part.geom->IsValid()) surfaces.push_back(&part);
    QSet<QString> duplicates;
    // Materialize before returning: QStringBuilder otherwise retains temporary QString references.
    auto recordId = [](const Part& part) -> QString {
        return canonical(part.path) + QStringLiteral("#FID:%1").arg(part.fid);
    };
    for (const auto& part : parts) {
        if (!part.geom || part.filled || polygonal(part.geom.get()) || part.geom->IsEmpty()) continue;
        const auto type = wkbFlatten(part.geom->getGeometryType());
        if (type != wkbLineString && type != wkbMultiLineString) continue;
        for (const auto* surface : surfaces) {
            Geometry boundary(surface->geom->Boundary());
            if (!boundary) fail(QStringLiteral("水体面边界提取失败"));
            Geometry remainder(part.geom->Difference(boundary.get()));
            if (!remainder) fail(QStringLiteral("水体面描边比较失败"));
            if (remainder->IsEmpty()) { duplicates.insert(recordId(part)); break; }
        }
    }
    const auto original = parts.size();
    parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const Part& part) {
        return !part.filled && part.geom && !polygonal(part.geom.get()) && duplicates.contains(recordId(part));
    }), parts.end());
    return static_cast<int>(original - parts.size());
}

CPLRectObj rect(const OGREnvelope& e, double margin = 0)
{
    return CPLRectObj{e.MinX - margin, e.MinY - margin, e.MaxX + margin, e.MaxY + margin};
}

double envelopeDistance(const OGREnvelope& a, const OGREnvelope& b)
{
    return std::hypot(std::max({a.MinX - b.MaxX, b.MinX - a.MaxX, 0.0}),
                      std::max({a.MinY - b.MaxY, b.MinY - a.MaxY, 0.0}));
}

struct Index {
    CPLQuadTree* tree = nullptr;
    explicit Index(const std::vector<Object*>& candidates)
    {
        if (candidates.empty()) return;
        OGREnvelope box = candidates[0]->bounds;
        for (const auto* c : candidates) box.Merge(c->bounds);
        auto bounds = rect(box, 1e-8);
        tree = CPLQuadTreeCreate(&bounds, nullptr);
        if (!tree) fail(QStringLiteral("无法创建空间索引"));
        CPLQuadTreeSetMaxDepth(tree, CPLQuadTreeGetAdvisedMaxDepth(static_cast<int>(candidates.size())));
        for (auto* c : candidates) { const auto r = rect(c->bounds); CPLQuadTreeInsertWithBounds(tree, c, &r); }
    }
    ~Index() { if (tree) CPLQuadTreeDestroy(tree); }
};

QJsonObject identity(const Object& object)
{
    QJsonObject json;
    json[QStringLiteral("symbol_id")] = object.symbol;
    json[QStringLiteral("source_fids")] = object.fids;
    json[QStringLiteral("source_files")] = QJsonArray::fromStringList(object.paths);
    if (!object.fids.isEmpty()) json[QStringLiteral("feature_id")] = object.fids[0];
    if (!object.elemId.isEmpty()) json[QStringLiteral("elem_id")] = object.elemId;
    return json;
}

constexpr double kMinimumCorrespondenceSupport = 0.80;
constexpr double kMinimumCorrespondenceMargin = 0.05;
constexpr double kSymbolLineCorridorFraction = 0.10;
constexpr double kMinimumLineCoverage = 0.60;
constexpr double kMinimumSourceCoverageForMergedLine = 0.80;

double geometryMeasure(const OGRGeometry* geometry, int dimension)
{
    if (!geometry || geometry->IsEmpty()) return 0.0;
    const auto type = wkbFlatten(geometry->getGeometryType());
    if (dimension == 2 && type == wkbPolygon)
        return static_cast<const OGRPolygon*>(geometry)->get_Area();
    if (dimension == 1 && (type == wkbLineString || type == wkbLinearRing))
        return static_cast<const OGRLineString*>(geometry)->get_Length();
    if (type == wkbMultiPolygon || type == wkbMultiLineString || type == wkbGeometryCollection) {
        const auto* collection = dynamic_cast<const OGRGeometryCollection*>(geometry);
        if (!collection) return 0.0;
        double total = 0.0;
        for (int i = 0; i < collection->getNumGeometries(); ++i)
            total += geometryMeasure(collection->getGeometryRef(i), dimension);
        return total;
    }
    return 0.0;
}

std::vector<Object*> search(const Index& index, const OGREnvelope& bounds)
{
    std::vector<Object*> result;
    if (!index.tree) return result;
    auto query = rect(bounds);
    int count = 0;
    void** found = CPLQuadTreeSearch(index.tree, &query, &count);
    std::unique_ptr<void*, decltype(&CPLFree)> hits(found, CPLFree);
    result.reserve(count);
    for (int i = 0; i < count; ++i) result.push_back(static_cast<Object*>(found[i]));
    return result;
}

double correspondenceSupport(const OGRGeometry* a, const OGRGeometry* b)
{
    if (!a || !b || a->IsEmpty() || b->IsEmpty()) return 0.0;
    const int da = a->getDimension(), db = b->getDimension();
    if (da < 0 || db < 0) return 0.0;
    if (!a->Intersects(b)) return 0.0;
    if (da == 0 || db == 0) return 1.0;

    CPLErrorReset();
    Geometry overlap(a->Intersection(b));
    if (!overlap) return std::numeric_limits<double>::quiet_NaN();
    if (overlap->IsEmpty()) return 0.0;

    double numerator = 0.0, denominator = 0.0;
    if (da == 2 && db == 2) {
        numerator = geometryMeasure(overlap.get(), 2);
        denominator = std::min(geometryMeasure(a, 2), geometryMeasure(b, 2));
    } else if (da == 1 && db == 1) {
        numerator = geometryMeasure(overlap.get(), 1);
        denominator = std::min(geometryMeasure(a, 1), geometryMeasure(b, 1));
    } else if ((da == 2 && db == 1) || (da == 1 && db == 2)) {
        const OGRGeometry* line = da == 1 ? a : b;
        numerator = geometryMeasure(overlap.get(), 1);
        denominator = geometryMeasure(line, 1);
    } else {
        return 0.0;
    }
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    return std::max(0.0, std::min(1.0, numerator / denominator));
}

struct Correspondence {
    const Object* object = nullptr;
    double support = 0.0;
    bool ambiguous = false;
    int candidateCount = 0;
    double sourceCoverage = 0.0;
    double parsedCoverage = 0.0;
};

Correspondence uniqueCorrespondence(const Object& source, const std::vector<Object*>& nearby,
                                   const std::function<bool(const Object&)>& allowed = {})
{
    std::vector<std::pair<const Object*, double>> scored;
    bool geometryFailure = false;
    double bestSupport = 0.0;
    int candidateCount = 0;
    for (const auto* candidate : nearby) {
        if (!candidate || !candidate->geom || !candidate->boundsKnown || (allowed && !allowed(*candidate))) continue;
        const double support = correspondenceSupport(source.geom.get(), candidate->geom.get());
        if (!std::isfinite(support)) { geometryFailure = true; continue; }
        if (support > 0.0) ++candidateCount;
        bestSupport = std::max(bestSupport, support);
        if (support >= kMinimumCorrespondenceSupport) scored.push_back({candidate, support});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (geometryFailure) return Correspondence{nullptr, bestSupport, true, candidateCount};
    if (scored.empty()) return Correspondence{nullptr, bestSupport, false, candidateCount};
    if (scored.size() > 1 && scored[0].second - scored[1].second < kMinimumCorrespondenceMargin)
        return Correspondence{nullptr, scored[0].second, true, candidateCount};
    return Correspondence{scored[0].first, scored[0].second, false, candidateCount};
}

// A drawn point-reservoir symbol is a cartographic shape, not the original
// shoreline. Require one positive-area source overlap and that source's
// geometric centre inside the symbol; never break multiple overlaps by FID.
Correspondence uniqueReservoirSymbolCorrespondence(const Object& symbol,
                                                   const std::vector<Object*>& nearby)
{
    const Object* candidate = nullptr;
    double bestSupport = 0.0;
    int candidateCount = 0;
    bool geometryFailure = false;
    for (const auto* possible : nearby) {
        if (!possible || !possible->geom || possible->geom->getDimension() != 2) continue;
        const double support = correspondenceSupport(symbol.geom.get(), possible->geom.get());
        if (!std::isfinite(support)) { geometryFailure = true; continue; }
        if (support <= 0.0) continue;
        ++candidateCount;
        bestSupport = std::max(bestSupport, support);
        candidate = possible;
    }
    if (geometryFailure || candidateCount > 1)
        return Correspondence{nullptr, bestSupport, true, candidateCount};
    if (!candidate) return Correspondence{nullptr, bestSupport, false, candidateCount};
    OGRPoint centre;
    if (candidate->geom->Centroid(&centre) != OGRERR_NONE || centre.IsEmpty())
        return Correspondence{nullptr, bestSupport, true, candidateCount};
    CPLErrorReset();
    const bool containsCentre = symbol.geom->Intersects(&centre);
    if (CPLGetLastErrorType() >= CE_Failure)
        return Correspondence{nullptr, bestSupport, true, candidateCount};
    return Correspondence{containsCentre ? candidate : nullptr, bestSupport, false, candidateCount};
}

// Compare the full line shapes within a small symbol-size-relative corridor.
// Requiring substantial source coverage rejects a short crossing/neighboring
// fragment; source segments can still correspond to a longer merged line.
Correspondence uniqueSymbolRelatedLineCorrespondence(
    const Object& source, const std::vector<Object*>& nearby, double corridor,
    const std::function<bool(const Object&)>& allowed)
{
    const double sourceLength = geometryMeasure(source.geom.get(), 1);
    if (!std::isfinite(sourceLength) || sourceLength <= 0.0 || !std::isfinite(corridor) || corridor <= 0.0)
        return Correspondence{nullptr, 0.0, true};
    CPLErrorReset();
    Geometry sourceBuffer(source.geom->Buffer(corridor));
    if (!sourceBuffer || CPLGetLastErrorType() >= CE_Failure)
        return Correspondence{nullptr, 0.0, true};

    const Object* accepted = nullptr;
    double bestScore = 0.0, bestSource = 0.0, bestParsed = 0.0;
    double acceptedScore = 0.0, acceptedSource = 0.0, acceptedParsed = 0.0;
    int count = 0;
    bool geometryFailure = false;
    for (const auto* candidate : nearby) {
        if (!candidate || !candidate->geom || candidate->geom->getDimension() != 1
            || !candidate->boundsKnown || (allowed && !allowed(*candidate))) continue;
        const double parsedLength = geometryMeasure(candidate->geom.get(), 1);
        if (!std::isfinite(parsedLength) || parsedLength <= 0.0) { geometryFailure = true; continue; }
        CPLErrorReset();
        Geometry parsedBuffer(candidate->geom->Buffer(corridor));
        Geometry coveredSource(parsedBuffer ? source.geom->Intersection(parsedBuffer.get()) : nullptr);
        Geometry coveredParsed(candidate->geom->Intersection(sourceBuffer.get()));
        if (!parsedBuffer || !coveredSource || !coveredParsed || CPLGetLastErrorType() >= CE_Failure) {
            geometryFailure = true;
            continue;
        }
        const double sourceCoverage = std::min(1.0, geometryMeasure(coveredSource.get(), 1) / sourceLength);
        const double parsedCoverage = std::min(1.0, geometryMeasure(coveredParsed.get(), 1) / parsedLength);
        if (!std::isfinite(sourceCoverage) || !std::isfinite(parsedCoverage)) { geometryFailure = true; continue; }
        const double score = std::min(sourceCoverage, parsedCoverage);
        if (score > bestScore) { bestScore = score; bestSource = sourceCoverage; bestParsed = parsedCoverage; }
        if (sourceCoverage < kMinimumLineCoverage
            || (parsedCoverage < kMinimumLineCoverage && sourceCoverage < kMinimumSourceCoverageForMergedLine)) continue;
        ++count;
        accepted = candidate;
        acceptedScore = score;
        acceptedSource = sourceCoverage;
        acceptedParsed = parsedCoverage;
    }
    if (geometryFailure || count > 1)
        return Correspondence{nullptr, bestScore, true, count, bestSource, bestParsed};
    if (accepted)
        return Correspondence{accepted, acceptedScore, false, count, acceptedSource, acceptedParsed};
    return Correspondence{nullptr, bestScore, false, count, bestSource, bestParsed};
}

QString actualLayerName(const QString& path)
{
    QString name = QFileInfo(path).completeBaseName();
    name.remove(QRegularExpression(QStringLiteral("_(Point|LineString|Polygon)$"),
                                   QRegularExpression::CaseInsensitiveOption));
    return name;
}

QString objectLayerName(const Object& object)
{
    return object.paths.isEmpty() ? QString() : actualLayerName(object.paths.first());
}

bool sameGdbFeature(const Object& a, const Object& b)
{
    // This key identifies one feature inside the loaded GDB dataset. ELEMID
    // may be absent or duplicated and is not a cross-dataset correspondence key.
    return !a.key.isEmpty() && a.key == b.key;
}

bool parsedObjectAllowedForGdbLayer(const Object& object, const QString& gdbLayer,
                                    const QStringList& associationClasses,
                                    const QMap<QString, MappingRow>& mapping)
{
    for (const auto& standardClass : associationClasses) {
        if (!mapping.contains(standardClass) || !mapping[standardClass].gdb.contains(gdbLayer)) continue;
        for (const auto& path : object.paths)
            if (mapping[standardClass].actual.contains(actualLayerName(path))) return true;
    }
    return false;
}

void save(const QString& path, const QByteArray& data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        fail(QStringLiteral("无法写入输出文件：%1").arg(path));
}

void exportProblems(const QString& path, OGRSpatialReference* srs,
                    const std::vector<std::pair<const Object*, QJsonObject>>& problems)
{
    auto* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!driver) fail(QStringLiteral("缺少 ESRI Shapefile 驱动"));
    if (QFileInfo::exists(path) && driver->Delete(path.toUtf8().constData()) != CE_None)
        fail(QStringLiteral("无法替换旧问题图层：%1").arg(path));
    Dataset ds(driver->Create(path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr));
    if (!ds) fail(QStringLiteral("无法创建问题图层：%1").arg(path));
    char* options[] = {const_cast<char*>("ENCODING=UTF-8"), nullptr};
    auto* layer = ds->CreateLayer(QFileInfo(path).completeBaseName().toUtf8().constData(), srs, wkbPolygon, options);
    if (!layer) fail(QStringLiteral("无法创建问题面图层"));
    for (const char* name : {"SYMBOL_ID", "SRC_FIDS", "SRC_FILE", "ERR_TYPE"}) {
        OGRFieldDefn field(name, OFTString); field.SetWidth(254);
        if (layer->CreateField(&field) != OGRERR_NONE) fail(QStringLiteral("无法创建问题图层字段"));
    }
    for (const char* name : {"NEAR_M", "TOL_M"}) {
        OGRFieldDefn field(name, OFTReal);
        if (layer->CreateField(&field) != OGRERR_NONE) fail(QStringLiteral("无法创建距离字段"));
    }
    for (const auto& problem : problems) {
        std::unique_ptr<OGRFeature, FeatureDelete> f(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        f->SetField("SYMBOL_ID", problem.first->symbol.toUtf8().constData());
        QStringList fids;
        for (const auto& fid : problem.first->fids) fids << QString::number(static_cast<qint64>(fid.toDouble()));
        f->SetField("SRC_FIDS", fids.join(QLatin1Char(';')).toUtf8().constData());
        f->SetField("SRC_FILE", problem.first->paths.join(QLatin1Char(';')).toUtf8().constData());
        const QString errorType = problem.second.value(QStringLiteral("error_type")).toString();
        const QString errorLabel = errorType == QStringLiteral("connection_lost")
            ? QStringLiteral("GDB连接未保留") : QStringLiteral("GDB源关系孤立");
        f->SetField("ERR_TYPE", errorLabel.toUtf8().constData());
        if (problem.second[QStringLiteral("nearest_distance_m")].isDouble())
            f->SetField("NEAR_M", problem.second[QStringLiteral("nearest_distance_m")].toDouble());
        f->SetField("TOL_M", problem.second[QStringLiteral("tolerance_m")].toDouble());
        if (f->SetGeometry(problem.first->geom.get()) != OGRERR_NONE || layer->CreateFeature(f.get()) != OGRERR_NONE)
            fail(QStringLiteral("写入问题要素失败"));
    }
    if (layer->SyncToDisk() != OGRERR_NONE) fail(QStringLiteral("问题图层落盘失败"));
}
} // namespace

void PointIsolationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& paths,
                               int scale, const QString& outputDir, CheckItemResult& result)
{
    result = CheckItemResult{};
    result.id = QStringLiteral("118");
    result.name = checkItemName;
    result.status = QStringLiteral("质检失败");
    result.err_exist = true;
    result.info[QStringLiteral("total_count")] = 0;
    result.info[QStringLiteral("raw_feature_count")] = 0;
    result.info[QStringLiteral("connected_count")] = 0;
    result.info[QStringLiteral("isolated_count")] = 0;
    result.info[QStringLiteral("invalid_count")] = 0;
    result.info[QStringLiteral("undetermined_count")] = 0;
    QString jsonPath, logPath, shpPath;
    QStringList log;
    log << QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" 点要素孤立检查（118）开始");
    try {
        if (outputDir.trimmed().isEmpty()) fail(QStringLiteral("质检输出目录为空"));
        const QString label = paths.standardLayer.isEmpty() ? QStringLiteral("水库") : paths.standardLayer;
        const QString prefix = PointIsolationOutput::prefix(label);
        auto output = [&](const QString& folder, const QString& suffix) {
            const QString dir = QDir(outputDir).filePath(folder);
            if (!QDir().mkpath(dir)) fail(QStringLiteral("无法创建输出目录：%1").arg(dir));
            return QDir(dir).filePath(prefix + suffix);
        };
        jsonPath = output(QStringLiteral("质检json文件"), QStringLiteral(".json"));
        logPath = output(QStringLiteral("质检日志文件"), QStringLiteral(".log"));
        shpPath = output(QStringLiteral("质检错误结果"), QStringLiteral("_errors.shp"));
        result.info[QStringLiteral("json_path")] = jsonPath;
        result.info[QStringLiteral("log_path")] = logPath;
        for (const QString& previous : PointIsolationOutput::previousFiles(outputDir, label)) {
            if (!QFile::remove(previous)) fail(QStringLiteral("无法清理上次点要素孤立检查结果：%1").arg(previous));
        }
        const Config cfg = readConfig(paths, scale);
        result.info[QStringLiteral("xml_path")] = cfg.xml;
        result.info[QStringLiteral("tolerance_m")] = cfg.toleranceMetres;
        result.info[QStringLiteral("severity")] = cfg.severity;
        const auto mapping = readMapping(paths.csvPath);
        if (!paths.standardLayer.isEmpty()) {
            if (!mapping.contains(paths.standardLayer) || !mapping[paths.standardLayer].actual.contains(cfg.target))
                fail(QStringLiteral("当前标准图层与 XML 检查图层的 CSV 映射不一致"));
            for (const auto& expected : cfg.expectedGdb) {
                if (!mapping[paths.standardLayer].gdb.contains(expected))
                    fail(QStringLiteral("XML 对应 GDB 图层与 CSV 配置不一致：%1").arg(expected));
            }
        }
        static std::once_flag once;
        std::call_once(once, [] { GDALAllRegister(); });
        QStringList targetPaths = list(paths.actualShpSemicolon);
        if (targetPaths.isEmpty()) targetPaths = paths.actualShpList;
        if (targetPaths.isEmpty()) targetPaths = resolve(paths.shpPath, cfg.target, Role::Symbol);
        QSet<QString> loadedTargets;
        std::vector<Part> targetParts;
        Srs working;
        for (const auto& path : targetPaths) {
            const QString base = QFileInfo(path).completeBaseName();
            if (base.endsWith(QStringLiteral("_Point"), Qt::CaseInsensitive)) continue;
            if (loadedTargets.contains(canonical(path))) continue;
            loadedTargets.insert(canonical(path));
            auto parts = readSource(Source{path, Role::Symbol, true}, cfg, working);
            for (auto& p : parts) targetParts.push_back(std::move(p));
        }
        if (loadedTargets.isEmpty()) fail(QStringLiteral("未找到待检水库成果图层"));
        auto targets = objects(targetParts, Role::Symbol);
        result.info[QStringLiteral("raw_feature_count")] = static_cast<int>(targetParts.size());
        result.info[QStringLiteral("total_count")] = static_cast<int>(targets.size());
        result.info[QStringLiteral("grouping_field")] = cfg.grouping;
        const double metresPerUnit = working->GetLinearUnits();
        const double tolerance = cfg.toleranceMetres / metresPerUnit;
        if (!std::isfinite(tolerance)) fail(QStringLiteral("连接容差无法换算为图层长度单位"));
        std::vector<Object> associated;
        std::vector<Part> associatedSymbols, associatedLines, associatedAreas;
        QSet<QString> loadedAssociations;
        QStringList missing;
        bool includeOtherTargets = false;
        for (const auto& name : cfg.associations) {
            if (!mapping.contains(name) || mapping[name].actual.isEmpty()) { missing << name; continue; }
            for (const auto& actual : mapping[name].actual) {
                Role role = actual == cfg.target ? Role::Symbol
                    : ((name == QStringLiteral("河流面") || name == QStringLiteral("湖泊水库面")
                        || actual == QStringLiteral("水系面")) ? Role::Area : Role::Line);
                const auto actualPaths = resolve(paths.shpPath, actual, role);
                if (actualPaths.isEmpty()) { missing << name + QLatin1Char(':') + actual; continue; }
                for (const auto& path : actualPaths) {
                    const QString key = canonical(path);
                    if (loadedAssociations.contains(key)) continue;
                    loadedAssociations.insert(key);
                    if (loadedTargets.contains(key)) { includeOtherTargets = true; continue; }
                    auto raw = readSource(Source{path, role, false}, cfg, working);
                    auto& pool = role == Role::Symbol ? associatedSymbols : (role == Role::Area ? associatedAreas : associatedLines);
                    for (auto& part : raw) pool.push_back(std::move(part));
                }
            }
        }
        result.info[QStringLiteral("removed_boundary_stroke_count")] = removeKnownBoundaryStrokes(associatedAreas);
        for (auto role : {Role::Symbol, Role::Line, Role::Area}) {
            auto& pool = role == Role::Symbol ? associatedSymbols : (role == Role::Area ? associatedAreas : associatedLines);
            auto normalized = objects(pool, role);
            for (auto& object : normalized) associated.push_back(std::move(object));
        }
        result.info[QStringLiteral("associated_file_count")] = loadedAssociations.size();
        result.info[QStringLiteral("missing_associated_layers")] = QJsonArray::fromStringList(missing);
        std::vector<Object*> damaged;
        for (auto& object : associated) if (!object.geom) damaged.push_back(&object);
        if (includeOtherTargets) for (auto& object : targets) if (!object.geom) damaged.push_back(&object);
        result.info[QStringLiteral("associated_geometry_warning_count")] = static_cast<int>(damaged.size());
        QJsonArray warnings;
        for (const auto* object : damaged) {
            auto detail = identity(*object); detail[QStringLiteral("description")] = object->problem;
            warnings.append(detail);
        }
        result.info[QStringLiteral("geometry_warnings")] = warnings;

        struct GdbLayerData {
            QString name, path, problem;
            bool loaded = false;
            std::vector<Object> objects;
        };
        QStringList targetGdbLayers, sourceGdbLayers, requiredGdbLayers, missingGdbMappings;
        auto appendUnique = [](QStringList& values, const QString& value) {
            if (!value.isEmpty() && !values.contains(value)) values << value;
        };
        for (const auto& name : cfg.expectedGdb) appendUnique(targetGdbLayers, name);
        if (targetGdbLayers.isEmpty()) missingGdbMappings << QStringLiteral("待检图层未配置对应GDB图层");
        for (const auto& standardClass : cfg.associations) {
            if (!mapping.contains(standardClass) || mapping[standardClass].gdb.isEmpty()) {
                missingGdbMappings << standardClass;
                continue;
            }
            for (const auto& gdbName : mapping[standardClass].gdb) appendUnique(sourceGdbLayers, gdbName);
        }
        for (const auto& name : targetGdbLayers) appendUnique(requiredGdbLayers, name);
        for (const auto& name : sourceGdbLayers) appendUnique(requiredGdbLayers, name);

        std::vector<GdbLayerData> gdbLayers;
        QStringList semicolonGdbPaths = list(paths.gdbShpSemicolon);
        for (const auto& name : requiredGdbLayers) {
            GdbLayerData layer;
            layer.name = name;
            const QString directPath = QDir(paths.gdbShpPath).filePath(name + QStringLiteral(".shp"));
            if (QFileInfo::exists(directPath)) layer.path = directPath;
            if (layer.path.isEmpty()) {
                for (const auto& candidatePath : semicolonGdbPaths) {
                    if (QFileInfo(candidatePath).completeBaseName().compare(name, Qt::CaseInsensitive) == 0) {
                        layer.path = candidatePath;
                        break;
                    }
                }
            }
            if (layer.path.isEmpty()) {
                layer.problem = QStringLiteral("缺少GDB成果图层：%1").arg(name);
            } else {
                try {
                    auto raw = readSource(Source{layer.path, Role::Any, false, true}, cfg, working);
                    layer.objects = objects(raw, Role::Any);
                    layer.loaded = true;
                } catch (const std::exception& e) {
                    layer.problem = QString::fromUtf8(e.what());
                }
            }
            gdbLayers.push_back(std::move(layer));
        }
        auto findGdbLayer = [&](const QString& name) -> GdbLayerData* {
            for (auto& layer : gdbLayers)
                if (layer.name.compare(name, Qt::CaseInsensitive) == 0) return &layer;
            return nullptr;
        };
        auto findGdbLayerIndex = [&](const QString& name) -> int {
            for (size_t i = 0; i < gdbLayers.size(); ++i)
                if (gdbLayers[i].name.compare(name, Qt::CaseInsensitive) == 0) return static_cast<int>(i);
            return -1;
        };
        std::vector<std::unique_ptr<Index>> gdbLayerIndexes;
        std::vector<bool> unboundedGdbGeometry(gdbLayers.size(), false);
        gdbLayerIndexes.reserve(gdbLayers.size());
        for (size_t layerIndex = 0; layerIndex < gdbLayers.size(); ++layerIndex) {
            std::vector<Object*> indexObjects;
            for (auto& object : gdbLayers[layerIndex].objects) {
                if (object.boundsKnown) indexObjects.push_back(&object);
                else if (!object.geom) unboundedGdbGeometry[layerIndex] = true;
            }
            gdbLayerIndexes.emplace_back(new Index(indexObjects));
        }
        bool targetGdbIncomplete = targetGdbLayers.isEmpty();
        for (const auto& name : targetGdbLayers) {
            const auto* layer = findGdbLayer(name);
            if (!layer || !layer->loaded) targetGdbIncomplete = true;
        }
        bool sourceGdbIncomplete = !missingGdbMappings.isEmpty();
        for (const auto& name : sourceGdbLayers) {
            const auto* layer = findGdbLayer(name);
            if (!layer || !layer->loaded) sourceGdbIncomplete = true;
        }
        QJsonArray gdbLayerDiagnostics, missingGdbLayersJson;
        for (const auto& layer : gdbLayers) {
            QJsonObject diagnostic;
            diagnostic[QStringLiteral("gdb_layer")] = layer.name;
            diagnostic[QStringLiteral("path")] = layer.path;
            diagnostic[QStringLiteral("loaded")] = layer.loaded;
            diagnostic[QStringLiteral("feature_count")] = static_cast<int>(layer.objects.size());
            if (!layer.problem.isEmpty()) {
                diagnostic[QStringLiteral("description")] = layer.problem;
                missingGdbLayersJson.append(layer.name);
            }
            gdbLayerDiagnostics.append(diagnostic);
        }
        result.info[QStringLiteral("gdb_layer_diagnostics")] = gdbLayerDiagnostics;
        result.info[QStringLiteral("missing_gdb_layers")] = missingGdbLayersJson;
        result.info[QStringLiteral("missing_gdb_mapping_classes")] = QJsonArray::fromStringList(missingGdbMappings);
        result.info[QStringLiteral("target_gdb_layers")] = QJsonArray::fromStringList(targetGdbLayers);
        result.info[QStringLiteral("associated_gdb_layers")] = QJsonArray::fromStringList(sourceGdbLayers);
        result.info[QStringLiteral("minimum_correspondence_support")] = kMinimumCorrespondenceSupport;
        result.info[QStringLiteral("minimum_correspondence_margin")] = kMinimumCorrespondenceMargin;
        result.info[QStringLiteral("symbol_line_corridor_fraction")] = kSymbolLineCorridorFraction;
        result.info[QStringLiteral("minimum_line_source_coverage")] = kMinimumLineCoverage;
        result.info[QStringLiteral("minimum_line_parsed_coverage")] = kMinimumLineCoverage;
        result.info[QStringLiteral("minimum_merged_line_source_coverage")] = kMinimumSourceCoverageForMergedLine;

        std::vector<Object*> validGdbTargets, invalidGdbTargets;
        for (const auto& layerName : targetGdbLayers) {
            auto* layer = findGdbLayer(layerName);
            if (!layer || !layer->loaded) continue;
            for (auto& object : layer->objects) {
                (object.geom ? validGdbTargets : invalidGdbTargets).push_back(&object);
            }
        }
        Index gdbTargetIndex(validGdbTargets);
        std::vector<const Object*> mappedGdbTargets(targets.size(), nullptr);
        std::vector<double> targetSupports(targets.size(), 0.0);
        std::vector<bool> targetMappingAmbiguous(targets.size(), false);
        std::vector<int> targetCandidateCounts(targets.size(), 0);
        std::vector<QString> targetMappingStatuses(targets.size(), QStringLiteral("unmatched"));
        std::vector<QString> targetMappingMethods(targets.size(), QStringLiteral("geometry_overlap"));
        QMap<QString, int> gdbTargetMatchCounts;
        for (size_t i = 0; i < targets.size(); ++i) {
            const auto& target = targets[i];
            if (!target.geom || !target.boundsKnown) continue;
            const bool symbolLocation = cfg.target == QStringLiteral("点状水库") && target.symbolFromFilledLines;
            if (symbolLocation) targetMappingMethods[i] = QStringLiteral("unique_reservoir_centre_in_symbol");
            const auto candidates = search(gdbTargetIndex, target.bounds);
            const auto match = symbolLocation ? uniqueReservoirSymbolCorrespondence(target, candidates)
                                              : uniqueCorrespondence(target, candidates);
            targetCandidateCounts[i] = match.candidateCount;
            targetSupports[i] = match.support;
            if (match.ambiguous) {
                targetMappingAmbiguous[i] = true;
                targetMappingStatuses[i] = QStringLiteral("ambiguous");
            }
            else if (match.object) {
                mappedGdbTargets[i] = match.object;
                targetMappingStatuses[i] = QStringLiteral("matched");
                ++gdbTargetMatchCounts[match.object->key];
            }
            for (const auto* possible : invalidGdbTargets) {
                if (!possible->boundsKnown || envelopeDistance(target.bounds, possible->bounds) == 0.0) {
                    targetMappingAmbiguous[i] = true;
                    targetMappingStatuses[i] = QStringLiteral("ambiguous_invalid_gdb_geometry");
                }
            }
        }
        for (size_t i = 0; i < mappedGdbTargets.size(); ++i)
            if (mappedGdbTargets[i] && gdbTargetMatchCounts.value(mappedGdbTargets[i]->key) > 1) {
                targetMappingAmbiguous[i] = true;
                targetMappingStatuses[i] = QStringLiteral("ambiguous_many_to_one");
            }

        std::vector<Object*> parsedCandidates;
        for (auto& object : associated) if (object.geom && object.boundsKnown) parsedCandidates.push_back(&object);
        if (includeOtherTargets)
            for (auto& object : targets) if (object.geom && object.boundsKnown) parsedCandidates.push_back(&object);
        Index parsedAssociationIndex(parsedCandidates);

        int connected = 0, isolated = 0, sourceIsolated = 0, connectionLost = 0, invalid = 0, uncertain = 0;
        QJsonArray connections, relationshipResults;
        std::vector<std::pair<const Object*, QJsonObject>> problems;
        auto addError = [&](QJsonObject error) {
            result.errors[QStringLiteral("error%1").arg(result.errors.size() + 1)] = error;
        };
        for (size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            const auto& target = targets[targetIndex];
            QJsonObject detail = identity(target);
            detail[QStringLiteral("tolerance_m")] = cfg.toleranceMetres;
            if (!target.geom) {
                ++invalid;
                detail[QStringLiteral("error_type")] = QStringLiteral("几何异常");
                detail[QStringLiteral("relationship_status")] = QStringLiteral("undetermined");
                detail[QStringLiteral("description")] = target.problem;
                relationshipResults.append(detail);
                addError(detail);
                continue;
            }
            detail[QStringLiteral("target_mapping_status")] = targetMappingStatuses[targetIndex];
            detail[QStringLiteral("target_mapping_method")] = targetMappingMethods[targetIndex];
            detail[QStringLiteral("target_mapping_candidate_count")] = targetCandidateCounts[targetIndex];
            detail[QStringLiteral("target_mapping_best_support")] = targetSupports[targetIndex];
            auto markUndetermined = [&](const QString& description) {
                ++uncertain;
                detail[QStringLiteral("error_type")] = QStringLiteral("undetermined");
                detail[QStringLiteral("relationship_status")] = QStringLiteral("undetermined");
                detail[QStringLiteral("description")] = description;
                relationshipResults.append(detail);
                addError(detail);
            };
            if (!missing.isEmpty() || !missingGdbMappings.isEmpty() || targetGdbIncomplete || sourceGdbIncomplete) {
                markUndetermined(QStringLiteral("规则要求的解析图层、GDB图层或映射不完整，无法判定源关系是否保留"));
                continue;
            }
            if (targetMappingAmbiguous[targetIndex] || !mappedGdbTargets[targetIndex]) {
                markUndetermined(QStringLiteral("解析待检对象无法通过空间匹配唯一对应到GDB待检对象"));
                continue;
            }
            const Object* gdbTarget = mappedGdbTargets[targetIndex];
            detail[QStringLiteral("gdb_target")] = identity(*gdbTarget);
            detail[QStringLiteral("target_correspondence_support")] = targetSupports[targetIndex];

            bool sourceGeometryIncomplete = false;
            std::vector<std::pair<QString, const Object*>> gdbEdges;
            for (const auto& sourceLayerName : sourceGdbLayers) {
                const auto* sourceLayer = findGdbLayer(sourceLayerName);
                if (!sourceLayer || !sourceLayer->loaded) { sourceGeometryIncomplete = true; continue; }
                const int layerIndex = findGdbLayerIndex(sourceLayerName);
                if (layerIndex < 0) { sourceGeometryIncomplete = true; continue; }
                if (unboundedGdbGeometry[static_cast<size_t>(layerIndex)]) sourceGeometryIncomplete = true;
                for (const auto* sourceObject : search(*gdbLayerIndexes[static_cast<size_t>(layerIndex)], gdbTarget->bounds)) {
                    if (!sourceObject->geom) { sourceGeometryIncomplete = true; continue; }
                    if (sameGdbFeature(*gdbTarget, *sourceObject)) continue;
                    CPLErrorReset();
                    const bool touches = gdbTarget->geom->Intersects(sourceObject->geom.get());
                    if (CPLGetLastErrorType() >= CE_Failure) {
                        sourceGeometryIncomplete = true;
                        CPLErrorReset();
                    } else if (touches) {
                        gdbEdges.push_back({sourceLayerName, sourceObject});
                    }
                }
            }
            detail[QStringLiteral("gdb_source_relation_count")] = static_cast<int>(gdbEdges.size());
            if (sourceGeometryIncomplete) {
                markUndetermined(QStringLiteral("GDB待检范围内存在无法读取或验证的关联几何，不能据此判定孤立或断连"));
                continue;
            }
            if (gdbEdges.empty()) {
                ++isolated;
                ++sourceIsolated;
                detail[QStringLiteral("error_type")] = QStringLiteral("source_isolated");
                detail[QStringLiteral("relationship_status")] = QStringLiteral("source_isolated");
                detail[QStringLiteral("description")] = QStringLiteral("GDB待检对象未与规则指定的任何关联要素相交");
                problems.push_back({&target, detail});
                relationshipResults.append(detail);
                addError(detail);
                continue;
            }

            QJsonArray edgeResults, preservedEdges;
            bool anyPreserved = false, anyUnmapped = false;
            double nearestDistance = std::numeric_limits<double>::infinity();
            for (const auto& edge : gdbEdges) {
                const QString gdbLayerName = edge.first;
                const Object* gdbSource = edge.second;
                QJsonObject edgeDetail;
                edgeDetail[QStringLiteral("gdb_layer")] = gdbLayerName;
                edgeDetail[QStringLiteral("gdb_source")] = identity(*gdbSource);
                const bool useLineCorridor = cfg.target == QStringLiteral("点状水库")
                    && target.symbolFromFilledLines && gdbSource->geom->getDimension() == 1;
                const double symbolDiagonal = std::hypot(target.bounds.MaxX - target.bounds.MinX,
                                                         target.bounds.MaxY - target.bounds.MinY);
                const double corridor = useLineCorridor ? symbolDiagonal * kSymbolLineCorridorFraction : 0.0;
                OGREnvelope searchBounds = gdbSource->bounds;
                searchBounds.MinX -= corridor; searchBounds.MaxX += corridor;
                searchBounds.MinY -= corridor; searchBounds.MaxY += corridor;
                const auto nearby = search(parsedAssociationIndex, searchBounds);
                const auto allowed = [&](const Object& candidate) {
                    return candidate.key != target.key
                        && parsedObjectAllowedForGdbLayer(candidate, gdbLayerName, cfg.associations, mapping);
                };
                const auto correspondence = useLineCorridor
                    ? uniqueSymbolRelatedLineCorrespondence(*gdbSource, nearby, corridor, allowed)
                    : uniqueCorrespondence(*gdbSource, nearby, allowed);
                edgeDetail[QStringLiteral("mapping_method")] = useLineCorridor
                    ? QStringLiteral("symbol_scaled_line_corridor") : QStringLiteral("geometry_overlap");
                if (useLineCorridor) {
                    edgeDetail[QStringLiteral("correspondence_corridor_m")] = corridor * metresPerUnit;
                    edgeDetail[QStringLiteral("candidate_count")] = correspondence.candidateCount;
                    edgeDetail[QStringLiteral("source_line_coverage")] = correspondence.sourceCoverage;
                    edgeDetail[QStringLiteral("parsed_line_coverage")] = correspondence.parsedCoverage;
                }
                if (correspondence.ambiguous || !correspondence.object) {
                    edgeDetail[QStringLiteral("mapping_status")] = correspondence.ambiguous
                        ? QStringLiteral("ambiguous") : QStringLiteral("unmatched");
                    if (correspondence.support > 0.0)
                        edgeDetail[QStringLiteral("best_support")] = correspondence.support;
                    anyUnmapped = true;
                    edgeResults.append(edgeDetail);
                    continue;
                }
                const Object* parsedSource = correspondence.object;
                const double distance = target.geom->Distance(parsedSource->geom.get());
                if (!std::isfinite(distance) || distance < 0.0) {
                    edgeDetail[QStringLiteral("mapping_status")] = QStringLiteral("connection_distance_failed");
                    anyUnmapped = true;
                    edgeResults.append(edgeDetail);
                    continue;
                }
                const bool preserved = distance <= tolerance;
                edgeDetail[QStringLiteral("mapping_status")] = QStringLiteral("matched");
                edgeDetail[QStringLiteral("parsed_source")] = identity(*parsedSource);
                edgeDetail[QStringLiteral("correspondence_support")] = correspondence.support;
                edgeDetail[QStringLiteral("parsed_distance_m")] = distance * metresPerUnit;
                edgeDetail[QStringLiteral("parsed_connection_preserved")] = preserved;
                edgeResults.append(edgeDetail);
                nearestDistance = std::min(nearestDistance, distance * metresPerUnit);
                if (preserved) {
                    anyPreserved = true;
                    preservedEdges.append(edgeDetail);
                }
            }
            detail[QStringLiteral("gdb_source_relations")] = edgeResults;
            detail[QStringLiteral("preserved_source_relation_count")] = preservedEdges.size();
            if (anyPreserved) {
                ++connected;
                detail[QStringLiteral("relationship_status")] = QStringLiteral("connection_preserved");
                detail[QStringLiteral("preserved_source_relations")] = preservedEdges;
                detail[QStringLiteral("description")] = QStringLiteral("至少一条GDB源关系在解析成果中保留连接");
                connections.append(detail);
            } else if (anyUnmapped) {
                ++uncertain;
                detail[QStringLiteral("error_type")] = QStringLiteral("undetermined");
                detail[QStringLiteral("relationship_status")] = QStringLiteral("undetermined");
                detail[QStringLiteral("description")] = QStringLiteral("GDB存在源关系，但至少一条关系无法唯一匹配到解析关联要素");
                addError(detail);
            } else {
                ++isolated;
                ++connectionLost;
                detail[QStringLiteral("error_type")] = QStringLiteral("connection_lost");
                detail[QStringLiteral("relationship_status")] = QStringLiteral("connection_lost");
                detail[QStringLiteral("description")] = QStringLiteral("GDB源关系存在，但对应解析关联要素未与待检对象满足XML连接容差");
                if (std::isfinite(nearestDistance)) detail[QStringLiteral("nearest_distance_m")] = nearestDistance;
                problems.push_back({&target, detail});
                addError(detail);
            }
            relationshipResults.append(detail);
        }
        result.info[QStringLiteral("connected_count")] = connected;
        result.info[QStringLiteral("isolated_count")] = isolated;
        result.info[QStringLiteral("source_isolated_count")] = sourceIsolated;
        result.info[QStringLiteral("connection_lost_count")] = connectionLost;
        result.info[QStringLiteral("invalid_count")] = invalid;
        result.info[QStringLiteral("undetermined_count")] = uncertain;
        result.info[QStringLiteral("connections")] = connections;
        result.info[QStringLiteral("relationship_results")] = relationshipResults;
        if (!problems.empty()) {
            exportProblems(shpPath, working.get(), problems);
            result.info[QStringLiteral("result_shp")] = shpPath;
        }
        result.status = (invalid || uncertain) ? QStringLiteral("质检失败") : QStringLiteral("质检成功");
        result.err_exist = !result.errors.isEmpty();
        result.info[QStringLiteral("description")] = QStringLiteral("检查 %1 个对象（%2 条原始记录）：保留连接 %3，GDB无源关系 %4，解析关系未保留 %5，几何异常 %6，无法判定 %7")
            .arg(targets.size()).arg(targetParts.size()).arg(connected).arg(sourceIsolated).arg(connectionLost).arg(invalid).arg(uncertain);
        if (targets.empty()) result.info[QStringLiteral("skipped")] = true;
    } catch (const std::exception& e) {
        const QString message = QString::fromUtf8(e.what());
        result.status = QStringLiteral("质检失败"); result.err_exist = true;
        result.info[QStringLiteral("description")] = message;
        QJsonObject error;
        error[QStringLiteral("error_type")] = QStringLiteral("执行异常");
        error[QStringLiteral("description")] = message;
        result.errors[QStringLiteral("error%1").arg(result.errors.size() + 1)] = error;
    }
    log << result.info[QStringLiteral("description")].toString();
    log << QString::fromUtf8(QJsonDocument(result.info).toJson(QJsonDocument::Indented));
    log << QString::fromUtf8(QJsonDocument(result.errors).toJson(QJsonDocument::Indented));
    try {
        if (!logPath.isEmpty()) save(logPath, log.join(QLatin1Char('\n')).toUtf8());
        QJsonObject item;
        item[QStringLiteral("id")] = result.id; item[QStringLiteral("name")] = result.name;
        item[QStringLiteral("status")] = result.status; item[QStringLiteral("err_exist")] = result.err_exist;
        item[QStringLiteral("info")] = result.info; item[QStringLiteral("errors")] = result.errors;
        QJsonObject root; root[QStringLiteral("check_item")] = item;
        if (!jsonPath.isEmpty()) save(jsonPath, QJsonDocument(root).toJson(QJsonDocument::Indented));
    } catch (const std::exception& e) {
        result.status = QStringLiteral("质检失败"); result.err_exist = true;
        result.info[QStringLiteral("output_error")] = QString::fromUtf8(e.what());
    }
}
