#pragma once

#include "series_layer_mapping.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <cmath>

// Human-readable evidence for the independent 502 result exporter. This does not change matching decisions.
namespace CrossScaleFindingPresentation {
inline QString scaleText(int scale)
{
    return scale>0 ? QStringLiteral("1:%1").arg(scale):QStringLiteral("未记录");
}

inline QString listText(const QJsonValue& value)
{
    QStringList items;
    for (const auto& item:value.toArray()) {
        const QString text=item.isString()?item.toString():item.toVariant().toString();
        if (!text.isEmpty() && !items.contains(text)) items << text;
    }
    return items.join(QStringLiteral("、"));
}

inline QJsonValue number(const QJsonObject& record,const QString& key)
{
    const auto value=record.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble()>=0
        ? value:QJsonValue(QJsonValue::Null);
}

inline QString issueName(const QString& type)
{
    if (type==QStringLiteral("cross_scale_support_missing")) return QStringLiteral("未找到对应候选");
    if (type==QStringLiteral("cross_scale_weak_support")) return QStringLiteral("局部支撑不足");
    if (type==QStringLiteral("cross_scale_candidate_ambiguous")) return QStringLiteral("对应关系不唯一");
    if (type==QStringLiteral("cross_scale_candidate_outside_position_tolerance")) return QStringLiteral("候选位置超差");
    if (type==QStringLiteral("cross_scale_annotation_text_no_match")) return QStringLiteral("未找到同名标注");
    if (type==QStringLiteral("cross_scale_annotation_text_variant")) return QStringLiteral("标注名称尾缀不同");
    if (type==QStringLiteral("cross_scale_annotation_spatial_no_match")) return QStringLiteral("标注位置对应未确定");
    if (type==QStringLiteral("cross_scale_mandatory_retention_missing")) return QStringLiteral("强制保留对象待核对");
    return QStringLiteral("检查条件不足");
}

inline QJsonObject decorate(QJsonObject record,const QJsonObject& task,const QJsonObject& summary)
{
    const QString type=record.value(QStringLiteral("error_type")).toString();
    const QString status=record.value(QStringLiteral("decision_status")).toString();
    const bool annotation=record.contains(QStringLiteral("parsed_text_identity"));
    const bool assessed=record.value(QStringLiteral("best_relation_group_candidate_count")).toInt()>0
        && number(record,QStringLiteral("coverage_ratio")).isDouble();
    const bool lineAssessed=assessed && record.value(QStringLiteral("checked_length")).toDouble()>0;
    const int smallScale=task.value(QStringLiteral("small_scale")).toInt();
    const int largeScale=task.value(QStringLiteral("large_scale")).toInt();
    record[QStringLiteral("small_scale")]=smallScale;
    record[QStringLiteral("large_scale")]=largeScale;
    record[QStringLiteral("small_scale_text")]=scaleText(smallScale);
    record[QStringLiteral("large_scale_text")]=scaleText(largeScale);
    record[QStringLiteral("decision_label")]=status==QStringLiteral("confirmed_error")?QStringLiteral("程序确认错误"):
        status==QStringLiteral("not_evaluable")?QStringLiteral("无法评估"):QStringLiteral("待人工复核");
    record[QStringLiteral("finding_name")]=issueName(type);
    record[QStringLiteral("responsibility_label")]=status==QStringLiteral("confirmed_error")
        ? (record.value(QStringLiteral("right_is_error")).toBool()?QStringLiteral("程序判定小比例尺侧"):
            record.value(QStringLiteral("left_is_error")).toBool()?QStringLiteral("程序判定大比例尺侧"):QStringLiteral("按具体错误要素核对"))
        :QStringLiteral("尚未确定，核对两侧原图");
    record[QStringLiteral("support_coverage_evaluated")]=assessed;
    record[QStringLiteral("support_coverage_percent")]=assessed
        ? QJsonValue(record.value(QStringLiteral("coverage_ratio")).toDouble()*100):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("display_gap_m")]=lineAssessed?number(record,QStringLiteral("longest_uncovered_length")):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("display_gap_limit_m")]=lineAssessed?number(record,QStringLiteral("max_uncovered_segment_length")):QJsonValue(QJsonValue::Null);
    const QJsonValue threshold=number(summary,QStringLiteral("support_coverage_threshold"));
    record[QStringLiteral("support_coverage_threshold")]=threshold;
    record[QStringLiteral("display_coverage_min_percent")]=assessed && threshold.isDouble()
        ? QJsonValue(threshold.toDouble()*100):QJsonValue(QJsonValue::Null);
    const auto budget=record.value(QStringLiteral("tolerance_budget")).toObject();
    const QMap<QString,QString> budgetKeys{{QStringLiteral("candidate_search_radius"),QStringLiteral("search_m")},
        {QStringLiteral("expanded_candidate_search_radius"),QStringLiteral("expanded_search_m")},
        {QStringLiteral("position_acceptance_tolerance"),QStringLiteral("position_m")},{QStringLiteral("support_tolerance"),QStringLiteral("geometry_m")}};
    for(auto it=budgetKeys.constBegin();it!=budgetKeys.constEnd();++it)
        if(!number(record,it.key()).isDouble()) record[it.key()]=number(budget,it.value()).isDouble()?number(budget,it.value()):number(summary,it.key());
    // Outside-position records use the legacy tolerance key; retain the actual executed value.
    if (!number(record,QStringLiteral("position_acceptance_tolerance")).isDouble())
        record[QStringLiteral("position_acceptance_tolerance")]=number(record,QStringLiteral("tolerance"));
    QJsonValue distance=number(record,QStringLiteral("best_distance"));
    if (!distance.isDouble() && annotation) distance=number(record,QStringLiteral("annotation_best_distance"));
    if (!distance.isDouble() && annotation) distance=number(record,QStringLiteral("annotation_nearest_same_text_distance"));
    if (!distance.isDouble() && type==QStringLiteral("cross_scale_annotation_text_variant"))
        distance=number(record,QStringLiteral("annotation_nearest_text_variant_distance"));
    record[QStringLiteral("display_candidate_distance_m")]=distance;
    const QJsonValue search=number(record,QStringLiteral("candidate_search_radius"));
    const QString model=budget.value(QStringLiteral("model")).toString(summary.value(QStringLiteral("tolerance_model")).toString());
    record[QStringLiteral("tolerance_model_label")]=annotation?QStringLiteral("标注独立对应"):
        model==QStringLiteral("scale_pair_v1")?QStringLiteral("按两图比例尺和类别换算"):QStringLiteral("旧版固定米制缓冲");
    record[QStringLiteral("tolerance_source_label")]=annotation?QStringLiteral("同文本匹配及重复名称消歧"):
        budget.value(QStringLiteral("source")).toString()==QStringLiteral("engineering_initial_unvalidated")
            ?QStringLiteral("工程初始参数，待真实样本校准"):budget.value(QStringLiteral("source")).toString(QStringLiteral("旧XML显式参数"));
    record[QStringLiteral("display_expanded_search_m")]=!annotation?number(record,QStringLiteral("expanded_candidate_search_radius")):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("tolerance_business_class")]=budget.value(QStringLiteral("business_class"));
    const auto directionCoverage=number(record,QStringLiteral("line_direction_coverage_ratio"));
    record[QStringLiteral("display_direction_coverage_percent")]=directionCoverage.isDouble()
        ?QJsonValue(directionCoverage.toDouble()*100):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("display_direction_max_deg")]=record.value(QStringLiteral("line_direction_check_applicable")).toBool()
        ?number(record,QStringLiteral("line_direction_max_angle_deg")):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("display_search_radius_m")]=!annotation?search:QJsonValue(QJsonValue::Null);
    record[QStringLiteral("display_position_tolerance_m")]=annotation?number(record,QStringLiteral("annotation_tolerance")):
        number(record,QStringLiteral("position_acceptance_tolerance"));
    record[QStringLiteral("display_geometry_tolerance_m")]=!annotation?number(record,QStringLiteral("support_tolerance")):
        QJsonValue(QJsonValue::Null);
    record[QStringLiteral("tolerance_application_note")]=annotation
        ?QStringLiteral("同文本标注在确认图层内检索；位置仅辅助重名消歧；几何支撑缓冲不适用。")
        :QStringLiteral("空间初选、位置接受及几何支撑为不同环节；同编码/同名语义候选可在空间初选范围外检索。");
    record[QStringLiteral("search_on_small_mm")]=!annotation && search.isDouble() && smallScale>0
        ? QJsonValue(search.toDouble()*1000/smallScale):QJsonValue(QJsonValue::Null);
    record[QStringLiteral("search_on_large_mm")]=!annotation && search.isDouble() && largeScale>0
        ? QJsonValue(search.toDouble()*1000/largeScale):QJsonValue(QJsonValue::Null);

    QString referencePath=record.value(QStringLiteral("best_large_shp")).toString();
    QJsonValue referenceFids=record.value(QStringLiteral("best_large_source_fids"));
    if (referencePath.isEmpty()) {
        referencePath=record.value(QStringLiteral("annotation_best_large_shp")).toString();
        referenceFids=record.value(QStringLiteral("annotation_best_large_source_fids"));
    }
    if (referencePath.isEmpty() && type==QStringLiteral("cross_scale_annotation_text_variant")) {
        referencePath=record.value(QStringLiteral("annotation_nearest_text_variant_shp")).toString();
        referenceFids=record.value(QStringLiteral("annotation_nearest_text_variant_source_fids"));
    }
    QStringList referenceLayers;
    const QJsonObject mapping=summary.value(QStringLiteral("confirmed_layer_mapping")).toObject();
    const QString smallPath=record.value(QStringLiteral("small_shp")).toString();
    for (const auto& value:summary.value(QStringLiteral("large_paths")).toArray()) {
        const QString path=value.toString();
        if (!mapping.isEmpty() && !SeriesLayerMapping::allowsPair(mapping,path,smallPath)) continue;
        const QString file=QFileInfo(path).fileName();
        if (!referenceLayers.contains(file)) referenceLayers << file;
    }
    record[QStringLiteral("reference_confirmed_layers")]=QJsonArray::fromStringList(referenceLayers);
    record[QStringLiteral("reference_candidate_layer")]=QFileInfo(referencePath).fileName();
    QString fidText=listText(referenceFids);
    if (fidText.isEmpty() && record.value(QStringLiteral("best_large_feature_id")).isDouble())
        fidText=record.value(QStringLiteral("best_large_feature_id")).toVariant().toString();
    QStringList candidateLayers,sourceDescriptions;
    for(const auto& source:record.value(QStringLiteral("best_support_sources")).toArray()) {
        const auto object=source.toObject();
        const QString file=QFileInfo(object.value(QStringLiteral("path")).toString()).fileName();
        if(!candidateLayers.contains(file)) candidateLayers << file;
        sourceDescriptions << file+QStringLiteral(": ")+listText(object.value(QStringLiteral("source_fids")));
    }
    if(!candidateLayers.isEmpty()) record[QStringLiteral("reference_candidate_layer")]=candidateLayers.join(QStringLiteral("、"));
    if(candidateLayers.size()>1) fidText=sourceDescriptions.join(QStringLiteral("；"));
    record[QStringLiteral("reference_candidate_fids")]=fidText;

    const QString text=record.value(QStringLiteral("parsed_text_identity")).toString();
    const QStringList gates=[&]() { QStringList items; for(const auto& v:record.value(QStringLiteral("support_failure_gates")).toArray()) items << v.toString(); return items; }();
    QString reason,advice;
    if (type==QStringLiteral("cross_scale_support_missing")) {
        const int hits=record.value(QStringLiteral("candidate_stage_counts")).toObject().value(QStringLiteral("spatial_index_bbox_hits")).toInt();
        reason=hits==0?QStringLiteral("%1米空间初选未命中候选；语义扩展后也无可用支撑；覆盖率和缺段未计算。")
                .arg(search.isDouble()?QString::number(search.toDouble(),'f',1):QStringLiteral("未记录"))
            :QStringLiteral("空间初选命中%1个候选；经对应图层、类别、标识及几何筛选和语义扩展后，可用候选为0；支撑未计算。").arg(hits);
        advice=QStringLiteral("对照两侧原图核实该对象是否存在、是否改线或概括；同时检查版本和已确认图层。");
        const auto expanded=number(record,QStringLiteral("expanded_candidate_search_radius"));
        if(expanded.isDouble() && search.isDouble() && expanded.toDouble()>search.toDouble())
            reason=QStringLiteral("初选%1米、扩展%2米及身份/名称补查后，仍无可接受支撑；不能据此确认地图遗漏。覆盖率和缺段未计算。")
                .arg(search.toDouble(),0,'f',1).arg(expanded.toDouble(),0,'f',1);
    } else if (type==QStringLiteral("cross_scale_candidate_outside_position_tolerance")) {
        reason=QStringLiteral("找到候选，最近距离%1米，超过位置容差%2米；尚未确认是同一地物。")
            .arg(distance.isDouble()?QString::number(distance.toDouble(),'f',1):QStringLiteral("未计算"))
            .arg(record.value(QStringLiteral("position_acceptance_tolerance")).toDouble(),0,'f',1);
        advice=QStringLiteral("核对候选名称、位置与邻接关系，区分制图移位、版本变化和错误匹配。");
    } else if (type==QStringLiteral("cross_scale_weak_support") || (assessed && !annotation)) {
        QStringList facts;
        facts << QStringLiteral("支撑覆盖%1%").arg(record.value(QStringLiteral("support_coverage_percent")).toDouble(),0,'f',1);
        if(gates.contains(QStringLiteral("coverage_below_threshold")) && threshold.isDouble())
            facts << QStringLiteral("低于要求%1%").arg(threshold.toDouble()*100,0,'f',1);
        if(gates.contains(QStringLiteral("significant_uncovered_segment")) && lineAssessed)
            facts << QStringLiteral("最长连续未支撑%1米，允许%2米")
                .arg(record.value(QStringLiteral("display_gap_m")).toDouble(),0,'f',1)
                .arg(record.value(QStringLiteral("display_gap_limit_m")).toDouble(),0,'f',1);
        if(gates.contains(QStringLiteral("candidate_not_unique"))) facts << QStringLiteral("多个候选关系未能唯一确定");
        if(gates.contains(QStringLiteral("local_direction_coverage_below_threshold"))) facts << QStringLiteral("局部方向一致覆盖%1%，不足要求")
            .arg(record.value(QStringLiteral("line_direction_coverage_ratio")).toDouble()*100,0,'f',1);
        if(gates.contains(QStringLiteral("local_direction_evaluation_failed"))) facts << QStringLiteral("局部方向未能可靠计算");
        if(gates.contains(QStringLiteral("point_entity_reconstruction_uncertain"))) facts << QStringLiteral("点符号实体重建依据不足");
        if(gates.contains(QStringLiteral("semantic_class_mismatch"))) facts << QStringLiteral("业务类别未确认一致");
        if(gates.contains(QStringLiteral("aggregation_not_allowed"))) facts << QStringLiteral("当前规则不允许该类候选聚合");
        if(gates.contains(QStringLiteral("multiple_point_support_groups"))) facts << QStringLiteral("存在多个点支撑组");
        reason=facts.join(QStringLiteral("；"))+QStringLiteral("。");
        advice=QStringLiteral("核对标记区段与候选原图的走向和位置；未支撑不等同于实地断线或遗漏。");
    } else if (type==QStringLiteral("cross_scale_annotation_text_no_match")) {
        reason=QStringLiteral("大比例尺对应标注层未找到与“%1”规范化文本相同的标注。 ").arg(text);
        advice=QStringLiteral("核对两侧文字、简称、分字解析和版本；不能据此直接认定标注遗漏。");
    } else if (type==QStringLiteral("cross_scale_annotation_text_variant")) {
        const QString variant=record.value(QStringLiteral("annotation_nearest_text_variant_text")).toString();
        reason=variant.isEmpty()?QStringLiteral("“%1”存在去除行政名称后缀后同名的标注候选，文字对应尚未确认。").arg(text)
            :QStringLiteral("小图“%1”与候选“%2”名称后缀不同，尚未确认简称关系。").arg(text,variant);
        advice=QStringLiteral("确认是否为同一地名及允许的简称，或是否存在行政区划/版本变化。");
    } else if (annotation) {
        const int count=record.value(QStringLiteral("annotation_candidate_count")).toInt();
        reason=QStringLiteral("标注“%1”：大比例尺同文本候选%2个；%3，尚未确定一一对应。")
            .arg(text).arg(count).arg(!record.value(QStringLiteral("annotation_small_text_unique")).toBool(true)
                ?QStringLiteral("小比例尺存在重复名称"):QStringLiteral("空间证据不足以消歧"));
        advice=QStringLiteral("结合标注对应的地物、行政归属和邻近对象确认；排版位置变化本身不直接判错。");
    } else if (type==QStringLiteral("cross_scale_candidate_ambiguous")) {
        reason=QStringLiteral("存在候选，但标识、名称或空间关系不足以唯一确定对应；详见候选证据。");
        advice=QStringLiteral("核对两侧候选的名称、类别、位置与拓扑关系，确认应对应哪个对象。");
    } else {
        reason=record.value(QStringLiteral("description")).toString();
        advice=QStringLiteral("查看详细证据，补齐检查条件后复核。");
    }
    record[QStringLiteral("technical_description")]=record.value(QStringLiteral("description"));
    record[QStringLiteral("description")]=reason;
    record[QStringLiteral("review_advice")]=advice;
    return record;
}

inline QString shapefileText(QString value,int maxBytes)
{
    if(value.toUtf8().size()<=maxBytes) return value;
    while(!value.isEmpty() && value.toUtf8().size()>maxBytes-3) value.chop(1);
    return value+QStringLiteral("…");
}
}
