#pragma once
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QtGlobal>
#include <cmath>

// Engineering configuration, not an assertion of measured map accuracy.
namespace CrossScaleTolerance {
inline QStringList numericKeys() {
    return {QStringLiteral("small_accuracy_mm"),QStringLiteral("large_accuracy_mm"),QStringLiteral("generalization_mm"),
        QStringLiteral("registration_m"),QStringLiteral("position_mm"),QStringLiteral("search_mm"),
        QStringLiteral("expanded_search_mm"),QStringLiteral("gap_mm")};
}
inline QJsonObject profile(const QJsonObject& profiles,const QString& businessClass) {
    QJsonObject result=profiles.value(QStringLiteral("default")).toObject();
    const auto custom=profiles.value(businessClass).toObject();
    for(auto it=custom.constBegin();it!=custom.constEnd();++it) result[it.key()]=it.value();
    return result;
}
inline QStringList validate(const QJsonObject& profiles,const QSet<QString>& classes) {
    QStringList errors;
    if(!profiles.value(QStringLiteral("default")).isObject()) errors << QStringLiteral("tolerance_profiles.default");
    const auto keys=numericKeys();
    for(auto it=profiles.constBegin();it!=profiles.constEnd();++it) {
        const QString prefix=QStringLiteral("tolerance_profiles.%1").arg(it.key());
        if(it.key()!=QStringLiteral("default") && !classes.contains(it.key())) errors << prefix;
        if(!it.value().isObject()) { errors << prefix;continue; }
        const auto custom=it.value().toObject();
        for(auto field=custom.constBegin();field!=custom.constEnd();++field)
            if(!keys.contains(field.key()) && field.key()!=QStringLiteral("source")) errors << prefix+QChar('.')+field.key();
        const auto merged=profile(profiles,it.key());
        for(const QString& key:keys) {
            const auto value=merged.value(key);
            if(!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble()<0) errors << prefix+QChar('.')+key;
        }
        if(!merged.value(QStringLiteral("source")).isString() || merged.value(QStringLiteral("source")).toString().trimmed().isEmpty())
            errors << prefix+QStringLiteral(".source");
        if(merged.value(QStringLiteral("small_accuracy_mm")).toDouble()+merged.value(QStringLiteral("large_accuracy_mm")).toDouble()<=0
            || merged.value(QStringLiteral("position_mm")).toDouble()<=0
            || merged.value(QStringLiteral("search_mm")).toDouble()<merged.value(QStringLiteral("position_mm")).toDouble()
            || merged.value(QStringLiteral("expanded_search_mm")).toDouble()<merged.value(QStringLiteral("search_mm")).toDouble())
            errors << prefix+QStringLiteral(".budget_order");
    }
    return errors;
}
struct Budget {
    double position=0,support=0,search=0,expanded=0,gap=0;
    QJsonObject evidence;
};
inline Budget resolve(const QString& model,const QJsonObject& profiles,const QString& businessClass,int smallScale,int largeScale,
    double legacyPositionMm,double legacyGeometryM,double legacySearchMm,double legacyGapMm,double legacyGapM) {
    const auto ground=[](double mm,int scale) {return mm*scale/1000.0;};
    Budget result;
    if(model!=QStringLiteral("scale_pair_v1")) {
        result.position=qMax(.001,ground(legacyPositionMm,smallScale));
        result.support=qMax(.001,legacyGeometryM);
        result.search=qMax(qMax(result.position,result.support),ground(legacySearchMm,smallScale));
        result.expanded=result.search;
        result.gap=legacyGapMm>=0?qMax(.001,ground(legacyGapMm,smallScale)):(legacyGapM>=0?legacyGapM:result.support);
        result.evidence={{QStringLiteral("model"),QStringLiteral("legacy_fixed_m")},
            {QStringLiteral("source"),QStringLiteral("legacy_xml_explicit_values")}};
    } else {
        const auto p=profile(profiles,businessClass);
        const double smallBudget=ground(p.value(QStringLiteral("small_accuracy_mm")).toDouble(),smallScale);
        const double largeBudget=ground(p.value(QStringLiteral("large_accuracy_mm")).toDouble(),largeScale);
        const double generalization=ground(p.value(QStringLiteral("generalization_mm")).toDouble(),smallScale);
        const double registration=p.value(QStringLiteral("registration_m")).toDouble();
        result.support=qMax(.001,smallBudget+largeBudget+generalization+registration);
        result.position=qMax(result.support,ground(p.value(QStringLiteral("position_mm")).toDouble(),smallScale));
        result.search=qMax(result.position,ground(p.value(QStringLiteral("search_mm")).toDouble(),smallScale));
        result.expanded=qMax(result.search,ground(p.value(QStringLiteral("expanded_search_mm")).toDouble(),smallScale));
        result.gap=qMax(.001,ground(p.value(QStringLiteral("gap_mm")).toDouble(),smallScale));
        result.evidence={{QStringLiteral("model"),model},{QStringLiteral("source"),p.value(QStringLiteral("source"))},
            {QStringLiteral("profile"),profiles.contains(businessClass)?businessClass:QStringLiteral("default")},
            {QStringLiteral("parameters"),p},{QStringLiteral("small_accuracy_budget_m"),smallBudget},
            {QStringLiteral("large_accuracy_budget_m"),largeBudget},{QStringLiteral("generalization_budget_m"),generalization},
            {QStringLiteral("registration_budget_m"),registration},{QStringLiteral("combination"),QStringLiteral("conservative_sum")}};
    }
    result.evidence[QStringLiteral("business_class")]=businessClass;
    result.evidence[QStringLiteral("small_scale")]=smallScale;
    result.evidence[QStringLiteral("large_scale")]=largeScale;
    result.evidence[QStringLiteral("position_m")]=result.position;
    result.evidence[QStringLiteral("geometry_m")]=result.support;
    result.evidence[QStringLiteral("search_m")]=result.search;
    result.evidence[QStringLiteral("expanded_search_m")]=result.expanded;
    result.evidence[QStringLiteral("gap_m")]=result.gap;
    result.evidence[QStringLiteral("expanded_candidates_can_bypass_position_gate")]=false;
    return result;
}
}
