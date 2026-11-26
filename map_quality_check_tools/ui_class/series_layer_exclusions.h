#pragma once
#include "series_layer_mapping.h"
#include <QMap>

namespace SeriesLayerExclusions {
inline QString nameKey(const QString& path)
{
    QString name = SeriesLayerMapping::layerBaseName(path);
    name.remove(QRegularExpression(QStringLiteral("[\\s_\\-—()（）\\[\\]【】]+")));
    return name.toCaseFolded();
}

inline bool countyMap(QString path)
{
    // Inspect the map name, never a parent folder containing e.g. "分县地图".
    QString name = QFileInfo(path.trimmed()).completeBaseName();
    if (name.endsWith(QStringLiteral("_shp"), Qt::CaseInsensitive)) name.chop(4);
    name.remove(QRegularExpression(QStringLiteral("(?:[—_\\-（(]|新版|修订版).*$")));
    name.remove(QRegularExpression(QStringLiteral("(?:行政区划图|地形图|政区图|地图|挂图|全图|图)$")));
    if (name.contains(QStringLiteral("分县")) || name.endsWith(QStringLiteral("政区"))) return false;
    return name.endsWith(QStringLiteral("县")) || name.endsWith(QStringLiteral("区"))
        || name.endsWith(QStringLiteral("旗"));
}

inline bool countyMapInRoots(const QString& roots)
{
    for (const QString& root : roots.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (countyMap(root)) return true;
    return false;
}

inline bool excluded(const QString& path, bool crossScale, bool countyEdge = false)
{
    const QString name = nameKey(path);
    if (countyEdge && (name.contains(QStringLiteral("晕带"))
        || ((name.contains(QStringLiteral("乡镇")) || name.contains(QStringLiteral("乡级"))
             || name.contains(QStringLiteral("镇级")) || name.contains(QStringLiteral("乡界"))
             || name.contains(QStringLiteral("镇界"))) && name.contains(QStringLiteral("界"))))) return true;
    if (!crossScale) return false;
    for (const QString& token : {QStringLiteral("代码标注"), QStringLiteral("代码注记"),
         QStringLiteral("引线"), QStringLiteral("快速路"), QStringLiteral("整饰"), QStringLiteral("图例"),
         QStringLiteral("渠流向"), QStringLiteral("渠道流向"), QStringLiteral("表面注记"),
         QStringLiteral("行政区划面普色"), QStringLiteral("行政区划普色"), QStringLiteral("行政区画面普色"),
         QStringLiteral("位置图")})
        if (name.contains(token)) return true;
    return false;
}

inline QStringList filterPaths(const QStringList& paths, bool crossScale, bool countyEdge = false)
{
    QStringList kept;
    for (const QString& path : paths) if (!excluded(path, crossScale, countyEdge)) kept << path;
    return kept;
}

inline void filterGroups(QMap<QString, QStringList>& groups, bool crossScale, bool countyEdge)
{
    for (auto it = groups.begin(); it != groups.end();) {
        it.value() = filterPaths(it.value(), crossScale, countyEdge);
        if (it.value().isEmpty()) it = groups.erase(it); else ++it;
    }
}

// Also protect execution from older saved/manual mappings. Removing an excluded
// side removes that entire relation, rather than turning it into a missing layer.
inline QJsonObject filterMapping(QJsonObject root, bool crossScale, bool countyEdge = false)
{
    const QJsonObject layers = root.value(QStringLiteral("layers")).toObject();
    QJsonObject keptLayers;
    for (auto it = layers.begin(); it != layers.end(); ++it) {
        if (excluded(it.key(), crossScale, countyEdge)) continue;
        QJsonObject layer = it.value().toObject();
        if (layer.value(QStringLiteral("rows")).isArray()) {
            QJsonArray rows, left, right;
            for (const QJsonValue& value : layer.value(QStringLiteral("rows")).toArray()) {
                QJsonObject row = value.toObject();
                const QJsonArray oldLeft = row.value(QStringLiteral("left")).toArray();
                const QJsonArray oldRight = row.value(QStringLiteral("right")).toArray();
                QJsonArray newLeft, newRight;
                for (const QJsonValue& path : oldLeft) if (!excluded(path.toString(), crossScale, countyEdge)) newLeft.append(path);
                for (const QJsonValue& path : oldRight) if (!excluded(path.toString(), crossScale, countyEdge)) newRight.append(path);
                if ((!oldLeft.isEmpty() && newLeft.isEmpty()) || (!oldRight.isEmpty() && newRight.isEmpty())) continue;
                row[QStringLiteral("left")] = newLeft; row[QStringLiteral("right")] = newRight;
                rows.append(row);
                for (const QJsonValue& path : newLeft) if (!left.contains(path)) left.append(path);
                for (const QJsonValue& path : newRight) if (!right.contains(path)) right.append(path);
            }
            if (rows.isEmpty()) continue;
            layer[QStringLiteral("rows")] = rows;
            layer[QStringLiteral("left")] = left; layer[QStringLiteral("right")] = right;
            if (layer.contains(QStringLiteral("large"))) layer[QStringLiteral("large")] = left;
            if (layer.contains(QStringLiteral("small"))) layer[QStringLiteral("small")] = right;
        } else {
            for (const QString& side : {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("large"), QStringLiteral("small")}) {
                if (!layer.value(side).isArray()) continue;
                QJsonArray kept;
                for (const QJsonValue& path : layer.value(side).toArray())
                    if (!excluded(path.toString(), crossScale, countyEdge)) kept.append(path);
                if (!layer.value(side).toArray().isEmpty() && kept.isEmpty()) { layer = QJsonObject(); break; }
                layer[side] = kept;
            }
            if (layer.isEmpty()) continue;
        }
        keptLayers[it.key()] = layer;
    }
    root[QStringLiteral("layers")] = keptLayers;
    return root;
}
} // namespace SeriesLayerExclusions
