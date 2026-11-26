#pragma once

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <qgsfeature.h>

namespace SeriesCartographicPolicy {
inline QString normalizedRole(QString value)
{
    value=value.trimmed().toLower();
    if(value==QStringLiteral("main") || value==QStringLiteral("主区")) return QStringLiteral("main");
    if(value==QStringLiteral("external") || value==QStringLiteral("外区") || value==QStringLiteral("非主区")) return QStringLiteral("external");
    return QString();
}

inline bool expressionField(QString field)
{
    field=field.trimmed().toLower();
    static const QSet<QString> names{QStringLiteral("original_c"),QStringLiteral("closed"),QStringLiteral("stroked"),
        QStringLiteral("filled"),QStringLiteral("is_dashed"),QStringLiteral("分级"),QStringLiteral("表达等级"),
        QStringLiteral("字号"),QStringLiteral("颜色"),QStringLiteral("线宽"),QStringLiteral("符号编码")};
    return names.contains(field) || field.startsWith(QStringLiteral("stroke_")) || field.startsWith(QStringLiteral("fill_"))
        || field.startsWith(QStringLiteral("font_")) || field.startsWith(QStringLiteral("text_size"))
        || field.startsWith(QStringLiteral("style_")) || field.startsWith(QStringLiteral("symbol_"));
}

inline QString pathKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}

inline QString featureRole(const QgsFeature& feature,const QString& path,const QJsonObject& context,
    const QStringList& mainLayers,const QStringList& externalLayers)
{
    // An explicit per-feature confirmation takes precedence over layer defaults.
    QString explicitRole;
    bool hasContext=false;
    for(const auto& value:context.value(QStringLiteral("feature_contexts")).toArray()) {
        const auto item=value.toObject();
        if(pathKey(item.value(QStringLiteral("shp_path")).toString())!=pathKey(path)
            || item.value(QStringLiteral("fid")).toVariant().toString()!=QString::number(feature.id())) continue;
        const QString role=normalizedRole(item.value(QStringLiteral("role")).toString());
        if(role.isEmpty()) {
            if(!item.value(QStringLiteral("role")).toString().trimmed().isEmpty()) return QString();
            continue;
        }
        if(hasContext && explicitRole!=role) return QString();
        hasContext=true;explicitRole=role;
    }
    if(hasContext) return explicitRole;
    QString attributeRole;
    for(const QString& field:{QStringLiteral("map_role"),QStringLiteral("series_role"),QStringLiteral("area_role")}) {
        const QString value=feature.attribute(field).toString().trimmed();
        if(value.isEmpty()) continue;
        const QString role=normalizedRole(value);
        if(role.isEmpty() || (!attributeRole.isEmpty() && attributeRole!=role)) return QString();
        attributeRole=role;
    }
    if(!attributeRole.isEmpty()) return attributeRole;
    QString layer=QFileInfo(path).completeBaseName();
    for(const QString& suffix:{QStringLiteral("_MultiPolygon"),QStringLiteral("_Polygon"),QStringLiteral("_MultiLineString"),
        QStringLiteral("_LineString"),QStringLiteral("_MultiPoint"),QStringLiteral("_Point"),QStringLiteral("_线"),QStringLiteral("_面"),QStringLiteral("_点")})
        if(layer.endsWith(suffix,Qt::CaseInsensitive)) {layer.chop(suffix.size());break;}
    const bool main=mainLayers.contains(layer,Qt::CaseInsensitive),external=externalLayers.contains(layer,Qt::CaseInsensitive);
    if(main==external) return QString();
    return main?QStringLiteral("main"):QStringLiteral("external");
}

inline bool applicable(const QString& ruleKey,const QString& scope,int denominator,const QString& project)
{
    // A scale-specific key must never be broadened by a trailing scope column.
    QString key=ruleKey;
    if(key.startsWith(QStringLiteral("role:"))) key=key.section(':',2);
    if(key.startsWith(QStringLiteral("scale:")) && !key.startsWith(QStringLiteral("scale:%1:").arg(denominator))) return false;
    if(scope.startsWith(QStringLiteral("project:"))) return !project.isEmpty() && scope.mid(8)==project;
    if(!scope.isEmpty() && scope!=QStringLiteral("all_scales")) return false;
    if(key.startsWith(QStringLiteral("scale:"))) return true;
    return scope==QStringLiteral("all_scales");
}
} // namespace SeriesCartographicPolicy
