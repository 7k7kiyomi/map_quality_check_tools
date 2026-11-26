#pragma once
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

// Each confirmed row is an allowed relation, not two independent layer pools.
namespace SeriesLayerMapping {
// Known names within the four existing 501 categories; no new check categories.
inline QStringList supplementalAliases(const QString& category)
{
    if (category == QStringLiteral("境界线")) return {QStringLiteral("市州级行政区界线")};
    if (category == QStringLiteral("道路")) return {QStringLiteral("在建高速公路"), QStringLiteral("专用道连接道")};
    if (category == QStringLiteral("水系线")) return {QStringLiteral("河流")};
    if (category == QStringLiteral("水系面")) return {QStringLiteral("湖泊水库面")};
    return {};
}
inline QString pathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath()).toCaseFolded();
}
inline bool containsPath(const QJsonArray& paths, const QString& path)
{
    if (path.trimmed().isEmpty()) return false;
    const QString key = pathKey(path);
    for (const QJsonValue& value : paths)
        if (!value.toString().trimmed().isEmpty() && pathKey(value.toString()) == key) return true;
    return false;
}
inline void appendRow(QJsonObject& layer, const QStringList& left, const QStringList& right)
{
    QJsonArray rows = layer.value(QStringLiteral("rows")).toArray();
    rows.append(QJsonObject{{QStringLiteral("left"), QJsonArray::fromStringList(left)},
                            {QStringLiteral("right"), QJsonArray::fromStringList(right)}});
    layer[QStringLiteral("rows")] = rows;
}
inline QString layerBaseName(const QString& path)
{
    QString name = QFileInfo(path.trimmed()).fileName();
    if (name.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive)) name.chop(4);
    name.remove(QRegularExpression(QStringLiteral("_(?:MultiLineStrings?|LineStrings?|MultiPolygons?|Polygons?|MultiPoints?|Points?|线|面|点)$"),
                                   QRegularExpression::CaseInsensitiveOption));
    return name.trimmed();
}
inline QString legacyLayerKey(const QString& path)
{
    return layerBaseName(path).toCaseFolded();
}

inline bool allowsPair(const QJsonObject& layer, const QString& leftPath, const QString& rightPath)
{
    if (layer.contains(QStringLiteral("rows"))) {
        for (const QJsonValue& value : layer.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject row = value.toObject();
            if (containsPath(row.value(QStringLiteral("left")).toArray(), leftPath)
                && containsPath(row.value(QStringLiteral("right")).toArray(), rightPath)) return true;
        }
        return false;
    }
    // Legacy files lost row pairing. Only same-name relations can be recovered.
    return containsPath(layer.value(QStringLiteral("left")).toArray(), leftPath)
        && containsPath(layer.value(QStringLiteral("right")).toArray(), rightPath)
        && legacyLayerKey(leftPath) == legacyLayerKey(rightPath);
}
inline QJsonArray selectedPaths(const QJsonObject& layer, const QString& side)
{
    if (!layer.contains(QStringLiteral("rows"))) return layer.value(side).toArray();
    QJsonArray paths;
    for (const QJsonValue& value : layer.value(QStringLiteral("rows")).toArray()) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("left")).toArray().isEmpty()
            || row.value(QStringLiteral("right")).toArray().isEmpty()) continue;
        for (const QJsonValue& path : row.value(side).toArray())
            if (!path.toString().trimmed().isEmpty() && !containsPath(paths, path.toString())) paths.append(path);
    }
    return paths;
}
}
