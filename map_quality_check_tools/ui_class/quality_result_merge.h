#pragma once

#include "quality_result_output.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <ogrsf_frmts.h>
#include <memory>
#include <map>

// Only explicit final outputs from the current run are inputs. Never scan old
// severity folders: that would merge previous runs (or summaries) a second time.
namespace QualityResultMerge {
struct Source {
    QString path, layer, check, severity;
};
struct Result {
    QStringList paths, errors;
    QHash<QString, QStringList> replacements;
    bool readyForCleanup = false;
};
using Dataset = std::unique_ptr<GDALDataset, decltype(&GDALClose)>;
using Feature = std::unique_ptr<OGRFeature, decltype(&OGRFeature::DestroyFeature)>;
using Geometry = std::unique_ptr<OGRGeometry>;

inline QString safeName(QString name)
{
    const QString original = name;
    name.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]")), QStringLiteral("_"));
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    if (name.isEmpty()) name = QStringLiteral("未命名");
    if (name != original || name.size() > 100) {
        name = name.left(100) + QLatin1Char('_') + QString::fromLatin1(
            QCryptographicHash::hash(original.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
    }
    return name;
}

inline int family(OGRwkbGeometryType type)
{
    switch (wkbFlatten(type)) {
    case wkbPoint: case wkbMultiPoint: return 0;
    case wkbLineString: case wkbMultiLineString: return 1;
    case wkbPolygon: case wkbMultiPolygon: return 2;
    default: return -1;
    }
}

// Normalize single/multi geometries and split heterogeneous collections by
// family, keeping all parts of one finding together within each family.
inline void collectGeometry(const OGRGeometry* geometry, OGRGeometryCollection* parts[3])
{
    if (!geometry || geometry->IsEmpty()) return;
    const auto type = wkbFlatten(geometry->getGeometryType());
    const int kind = family(type);
    if (type == wkbPoint || type == wkbLineString || type == wkbPolygon) {
        parts[kind]->addGeometry(geometry);
    } else if (OGR_GT_IsSubClassOf(type, wkbGeometryCollection)) {
        const auto* collection = geometry->toGeometryCollection();
        for (int i = 0; i < collection->getNumGeometries(); ++i)
            collectGeometry(collection->getGeometryRef(i), parts);
    }
}

inline QStringList sidecars(const QString& path)
{
    const QFileInfo file(path);
    QStringList paths;
    // Restrict deletion/copying to known Shapefile companions.
    for (const QString& ext : {QStringLiteral("shp"), QStringLiteral("shx"), QStringLiteral("dbf"),
             QStringLiteral("prj"), QStringLiteral("cpg"), QStringLiteral("qix"), QStringLiteral("qml"),
             QStringLiteral("sbn"), QStringLiteral("sbx"), QStringLiteral("shp.xml")}) {
        const QString candidate = file.dir().filePath(file.completeBaseName() + QLatin1Char('.') + ext);
        if (QFileInfo::exists(candidate)) paths << candidate;
    }
    return paths;
}

inline Result merge(const QString& errorRoot, const QVector<Source>& sources, QgsProject* project)
{
    Result result;
    GDALAllRegister();
    QDir root(errorRoot);
    if (!root.mkpath(QStringLiteral("."))) {
        result.errors << QStringLiteral("无法创建错误汇总目录：%1").arg(errorRoot);
        return result;
    }
    QTemporaryDir staging;
    if (!staging.isValid()) {
        result.errors << QStringLiteral("无法创建错误汇总临时目录");
        return result;
    }
    // Manifest owns only this feature's generated files, including zero-error
    // reruns. Do not delete arbitrary SHPs with similar names.
    const QString manifestPath = root.filePath(QStringLiteral("图层错误汇总清单.json"));
    QStringList previous;
    QFile manifest(manifestPath);
    if (manifest.open(QIODevice::ReadOnly)) {
        for (const auto& value : QJsonDocument::fromJson(manifest.readAll()).array()) {
            const QString relative = value.toString();
            const QString path = root.absoluteFilePath(relative);
            const QString folder = QFileInfo(path).dir().dirName();
            if (!relative.contains(QStringLiteral("..")) && !QDir::isAbsolutePath(relative)
                && (QFileInfo(path).dir().absolutePath() == root.absolutePath()
                    || QFileInfo(path).dir().absolutePath() == root.absoluteFilePath(folder))
                && (folder == QStringLiteral("高严重程度") || folder == QStringLiteral("中严重程度")
                    || folder == QStringLiteral("低严重程度") || folder == root.dirName())
                && QFileInfo(path).fileName().contains(QStringLiteral("_错误汇总_"))
                && path.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive)) previous << path;
        }
        manifest.close();
    }

    QMap<QString, QVector<Source>> groups;
    for (const Source& source : sources) {
        if (source.path.isEmpty() || source.layer.isEmpty()) continue;
        groups[QualityResultOutput::normalizedSeverity(source.severity) + QChar(0x1f) + source.layer] << source;
    }
    struct Output {
        Dataset dataset{nullptr, GDALClose};
        OGRLayer* layer = nullptr;
        QString temporary, destination;
        qint64 count = 0;
    };
    QStringList stagedPaths, destinations, preserved;
    for (auto group = groups.constBegin(); group != groups.constEnd(); ++group) {
        // Union attributes by name; strings accommodate differing source types.
        // Reserve short ASCII provenance fields to avoid DBF name truncation.
        QMap<QString, QString> names;
        QSet<QString> used{QStringLiteral("src_layer"), QStringLiteral("chk_name"), QStringLiteral("severity"),
                           QStringLiteral("src_file"), QStringLiteral("src_fid")};
        QSet<QString> seen;
        for (const Source& source : group.value()) {
            Dataset input(static_cast<GDALDataset*>(GDALOpenEx(source.path.toUtf8().constData(),
                GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
            if (!input || !input->GetLayer(0)) {
                result.errors << QStringLiteral("无法读取错误图层：%1").arg(source.path);
                continue;
            }
            OGRFeatureDefn* definition = input->GetLayer(0)->GetLayerDefn();
            for (int i = 0; i < definition->GetFieldCount(); ++i) {
                const QString key = QString::fromUtf8(definition->GetFieldDefn(i)->GetNameRef()).toCaseFolded();
                if (names.contains(key)) continue;
                QString name = key;
                name.replace(QRegularExpression(QStringLiteral("[^a-z0-9_]")), QStringLiteral("_"));
                name = name.left(10);
                if (name.isEmpty()) name = QStringLiteral("attr");
                const QString base = name.left(6);
                for (int n = 1; used.contains(name); ++n) name = base + QString::number(n);
                used.insert(name);
                names.insert(key, name);
            }
        }
        Output outputs[3];
        const QStringList labels{QStringLiteral("点"), QStringLiteral("线"), QStringLiteral("面")};
        const OGRwkbGeometryType types[3]{wkbMultiPoint, wkbMultiLineString, wkbMultiPolygon};
        OGRSpatialReference targetSrs;
        bool haveSrs = false, srsChosen = false;
        bool failed = false;
        for (const Source& source : group.value()) {
            const QString key = QualityResultOutput::pathKey(source.path);
            if (seen.contains(key)) continue;
            seen.insert(key);
            Dataset input(static_cast<GDALDataset*>(GDALOpenEx(source.path.toUtf8().constData(),
                GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
            if (!input || !input->GetLayer(0)) { failed = true; break; }
            OGRLayer* layer = input->GetLayer(0);
            result.replacements.insert(QDir::cleanPath(source.path), {});
            // Empty outputs do not contribute coordinates and must not select
            // (or conflict with) the CRS used by actual findings.
            if (layer->GetFeatureCount() == 0) continue;
            OGRSpatialReference* srs = layer->GetSpatialRef();
            if (!srsChosen) {
                haveSrs = srs != nullptr;
                if (srs) targetSrs = *srs;
                srsChosen = true;
            }
            if (haveSrs != (srs != nullptr)) {
                result.errors << QStringLiteral("图层 %1 的错误结果坐标系缺失情况不一致，未生成该图层汇总").arg(source.layer);
                failed = true; break;
            }
            std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)> transform(
                nullptr, OCTDestroyCoordinateTransformation);
            if (srs && !srs->IsSame(&targetSrs)) {
                transform.reset(OGRCreateCoordinateTransformation(srs, &targetSrs));
                if (!transform) { failed = true; break; }
            }
            Feature feature(nullptr, OGRFeature::DestroyFeature);
            while (feature.reset(layer->GetNextFeature()), feature) {
                // An unlocated finding cannot be represented in these geometry
                // summaries; retain its original file instead of discarding it.
                if (!feature->GetGeometryRef() || feature->GetGeometryRef()->IsEmpty()) {
                    failed = true; break;
                }
                Geometry geometry(feature->GetGeometryRef()->getLinearGeometry());
                if (!geometry || (transform && geometry->transform(transform.get()) != OGRERR_NONE)) {
                    failed = true; break;
                }
                OGRMultiPoint points;
                OGRMultiLineString lines;
                OGRMultiPolygon polygons;
                OGRGeometryCollection* parts[3]{&points, &lines, &polygons};
                collectGeometry(geometry.get(), parts);
                if (points.IsEmpty() && lines.IsEmpty() && polygons.IsEmpty()) { failed = true; break; }
                for (int kind = 0; kind < 3; ++kind) {
                    if (parts[kind]->IsEmpty()) continue;
                    Output& output = outputs[kind];
                    if (!output.layer) {
                        const QString name = safeName(source.layer) + QStringLiteral("_错误汇总_") + labels[kind];
                        const QString severity = QualityResultOutput::normalizedSeverity(source.severity);
                        const QDir destination(severity == QStringLiteral("未配置") ? root.path()
                            : root.filePath(severity + QStringLiteral("严重程度")));
                        if (!QDir().mkpath(destination.path())) { failed = true; break; }
                        output.destination = destination.filePath(name + QStringLiteral(".shp"));
                        output.temporary = QDir(staging.path()).filePath(QString::number(stagedPaths.size())
                            + QLatin1Char('_') + QString::number(kind) + QStringLiteral(".shp"));
                        auto* driver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
                        if (!driver) { failed = true; break; }
                        output.dataset.reset(driver->Create(output.temporary.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr));
                        if (!output.dataset) { failed = true; break; }
                        char* options[]{const_cast<char*>("ENCODING=UTF-8"), nullptr};
                        auto type = types[kind];
                        if (geometry->Is3D()) type = wkbSetZ(type);
                        if (geometry->IsMeasured()) type = wkbSetM(type);
                        output.layer = output.dataset->CreateLayer("errors", haveSrs ? &targetSrs : nullptr, type, options);
                        if (!output.layer) { failed = true; break; }
                        QStringList fields{QStringLiteral("src_layer"), QStringLiteral("chk_name"), QStringLiteral("severity"),
                                           QStringLiteral("src_file"), QStringLiteral("src_fid")};
                        fields.append(names.values());
                        for (const QString& name : fields) {
                            OGRFieldDefn field(name.toUtf8().constData(), OFTString);
                            field.SetWidth(254);
                            if (output.layer->CreateField(&field) != OGRERR_NONE) { failed = true; break; }
                        }
                        if (failed) break;
                    }
                    Feature merged(OGRFeature::CreateFeature(output.layer->GetLayerDefn()), OGRFeature::DestroyFeature);
                    const QStringList provenance{source.layer, source.check, source.severity,
                        QFileInfo(source.path).fileName(), QString::number(feature->GetFID())};
                    for (int i = 0; i < provenance.size(); ++i) merged->SetField(i, provenance.at(i).toUtf8().constData());
                    for (int i = 0; i < feature->GetFieldCount(); ++i) {
                        if (!feature->IsFieldSetAndNotNull(i)) continue;
                        const QString key = QString::fromUtf8(feature->GetDefnRef()->GetFieldDefn(i)->GetNameRef()).toCaseFolded();
                        merged->SetField(names.value(key).toUtf8().constData(), feature->GetFieldAsString(i));
                    }
                    merged->SetGeometry(parts[kind]);
                    if (output.layer->CreateFeature(merged.get()) != OGRERR_NONE) { failed = true; break; }
                    ++output.count;
                    QStringList& paths = result.replacements[QDir::cleanPath(source.path)];
                    if (!paths.contains(output.destination)) paths << output.destination;
                }
                if (failed) break;
            }
            if (failed) break;
        }
        for (Output& output : outputs) {
            output.dataset.reset();
            if (!failed && output.count > 0) {
                stagedPaths << output.temporary;
                destinations << output.destination;
            }
        }
        if (failed) {
            result.errors << QStringLiteral("图层错误汇总失败：%1（%2），保留该图层分项结果，其余图层继续汇总").arg(
                group.value().first().layer, group.value().first().severity);
            for (const Source& source : group.value()) result.replacements.remove(QDir::cleanPath(source.path));
            const Source& source = group.value().first();
            const QString severity = QualityResultOutput::normalizedSeverity(source.severity);
            const QDir destination(severity == QStringLiteral("未配置") ? root.path()
                : root.filePath(severity + QStringLiteral("严重程度")));
            for (const QString& label : labels) {
                const QString path = destination.filePath(safeName(source.layer) + QStringLiteral("_错误汇总_") + label + QStringLiteral(".shp"));
                if (previous.contains(path)) preserved << path;
            }
        }
    }
    // Failures are isolated by severity and source layer. Previously published
    // summaries of failed groups remain untouched and stay in the manifest.
    if (!result.errors.isEmpty() && result.replacements.isEmpty()) return result;
    const int inputErrorCount = result.errors.size();
    QStringList toRelease = previous;
    for (const QString& path : preserved) toRelease.removeAll(path);
    toRelease.append(destinations);
    QualityResultOutput::releaseLayers(project, toRelease);
    for (const QString& path : toRelease) {
        for (const QString& companion : sidecars(path)) {
            if (!QFile::remove(companion)) result.errors << QStringLiteral("无法清理旧汇总：%1").arg(companion);
        }
    }
    if (result.errors.size() != inputErrorCount) return result;
    for (int i = 0; i < stagedPaths.size(); ++i) {
        const QFileInfo temporary(stagedPaths.at(i)), destination(destinations.at(i));
        QStringList copied;
        for (const QString& companion : sidecars(temporary.filePath())) {
            const QString suffix = QFileInfo(companion).fileName().mid(temporary.completeBaseName().size());
            const QString path = destination.dir().filePath(destination.completeBaseName() + suffix);
            if (!QFile::copy(companion, path)) {
                result.errors << QStringLiteral("无法保存汇总文件：%1").arg(path);
                break;
            }
            copied << path;
        }
        if (result.errors.size() != inputErrorCount) {
            for (const QString& path : copied) QFile::remove(path);
            break;
        }
        result.paths << destination.filePath();
    }
    result.paths.append(preserved);
    QJsonArray files;
    for (const QString& path : result.paths) files.append(root.relativeFilePath(path));
    QSaveFile saved(manifestPath);
    if (!saved.open(QIODevice::WriteOnly) || saved.write(QJsonDocument(files).toJson()) < 0 || !saved.commit())
        result.errors << QStringLiteral("无法保存图层错误汇总清单");
    else if (result.errors.size() == inputErrorCount) result.readyForCleanup = true;
    return result;
}

// Remove only explicitly merged outputs inside the error-result directory.
// Failed groups/publication/verification leave their originals intact.
inline QStringList removeMergedSources(const QString& errorRoot, Result& result, QgsProject* project)
{
    QStringList removed;
    if (!result.readyForCleanup) return removed;
    for (const QString& path : result.paths) {
        Dataset summary(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(),
            GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr)), GDALClose);
        if (!summary || !summary->GetLayer(0) || summary->GetLayer(0)->GetFeatureCount() <= 0) {
            result.errors << QStringLiteral("汇总文件验证失败，保留原始错误结果：%1").arg(path);
            return removed;
        }
    }
    const QString root = QFileInfo(errorRoot).canonicalFilePath();
    if (root.isEmpty()) return removed;
    QStringList candidates;
    for (auto it = result.replacements.constBegin(); it != result.replacements.constEnd(); ++it) {
        const QFileInfo source(it.key());
        if (!source.exists()) continue;
        const QString canonical = source.canonicalFilePath();
        const QString extension = source.suffix().toLower();
        if (!canonical.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)
            || (extension != QStringLiteral("shp") && extension != QStringLiteral("geojson"))
            || source.fileName().contains(QStringLiteral("_错误汇总_"))) continue;
        candidates << source.absoluteFilePath();
    }
    QualityResultOutput::releaseLayers(project, candidates);
    for (const QString& path : candidates) {
        QStringList files;
        if (QFileInfo(path).suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) == 0)
            files = sidecars(path);
        else {
            files << path;
            for (const QString& extension : {QStringLiteral(".qml"), QStringLiteral(".qmd")}) {
                const QFileInfo source(path);
                const QString companion = source.dir().filePath(source.completeBaseName() + extension);
                if (QFileInfo::exists(companion)) files << companion;
            }
        }
        bool ok = true;
        // Keep the primary file until its companions have been removed.
        files.removeAll(path);
        files << path;
        for (const QString& file : files) {
            if (!QFile::remove(file)) {
                result.errors << QStringLiteral("无法清理已汇总的错误结果：%1").arg(file);
                ok = false; break;
            }
        }
        if (ok) removed << QDir::cleanPath(path);
    }
    return removed;
}
} // namespace QualityResultMerge
