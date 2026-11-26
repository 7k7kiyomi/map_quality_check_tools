#include "QualityCheckConfig.h"
#include <QRegularExpression>

namespace {
    const QString kSeriesCheckName = QStringLiteral("图幅接边与系列化表达");

    QStringList splitSeriesFields(const QString& text)
    {
        return text.split(QRegularExpression(QStringLiteral("[;,；，、]+")), Qt::SkipEmptyParts);
    }

    bool toConfigBool(const QVariant& value)
    {
        if (value.type() == QVariant::Bool) return value.toBool();
        const QString text = value.toString().trimmed().toLower();
        return text == QStringLiteral("true") || text == QStringLiteral("1") || text == QStringLiteral("是");
    }
}

QualityCheckConfig::QualityCheckConfig(QObject* parent)
    : QObject(parent)
{
}

QualityCheckConfig::~QualityCheckConfig()
{
}

bool QualityCheckConfig::loadFromFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QString("无法打开配置文件: %1").arg(filePath);
        return false;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorColumn;

    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn)) {
        m_lastError = QString("XML解析错误 (行%1,列%2): %3")
            .arg(errorLine).arg(errorColumn).arg(errorMsg);
        file.close();
        return false;
    }

    file.close();

    // 清空之前的配置
    m_checkItems.clear();

    // 解析根元素
    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list") {  // 改为 check_list
        m_lastError = "无效的XML根元素，应为 'check_list'";
        return false;
    }

    // 解析所有检查项
    QDomNodeList checkItems = root.elementsByTagName("check_item");
    for (int i = 0; i < checkItems.count(); ++i) {
        QDomElement element = checkItems.at(i).toElement();
        if (!parseCheckItem(element)) {
            return false;
        }
    }

    qDebug() << QString("成功加载 %1 个质量检查项").arg(m_checkItems.size());
    return true;
}

bool QualityCheckConfig::parseCheckItem(const QDomElement& element)
{
    QString checkName = element.firstChildElement("name").text();

    if (checkName.isEmpty()) {
        return true; // 跳过无效项
    }

    CheckItemConfig config;
    config.name = checkName;
    config.type = checkName;
    config.enabled = true;
    config.description = QString("%1检查").arg(checkName);

    // 解析所有rule元素
    QDomNodeList rules = element.elementsByTagName("rule");
    for (int i = 0; i < rules.count(); ++i) {
        QDomElement ruleElement = rules.at(i).toElement();
        QString ruleName = ruleElement.firstChildElement("name").text();
        QString description = ruleElement.firstChildElement("description").text();
        QString value = ruleElement.firstChildElement("value").text();
        QString unit = ruleElement.firstChildElement("unit").text();

        if (ruleName.isEmpty() || value.isEmpty()) {
            continue;
        }

        // 根据规则名称确定参数名
        QString paramName;

        if (ruleName.contains("面要素重叠容差")) {
            paramName = "overlap_tolerance";
        }
        else if (ruleName.contains("面要素间隙容差")) {
            paramName = "gap_tolerance";
        }
        else if (ruleName.contains("最小重叠面积")) {
            paramName = "min_overlap_area";
        }
        else if (ruleName == QStringLiteral("道路连接缓冲距离")) {
            paramName = "road_connection_buffer";
        }
        else if (ruleName == QStringLiteral("居民点缓冲距离")) {
            paramName = "road_settlement_buffer";
        }
        else if (ruleName.contains("悬挂线容差")) {
            paramName = "dangling_tolerance";
        }
        else if (ruleName.contains("孤立距离")) {
            paramName = "isolation_distance";
        }
        else if (ruleName.contains("最小线段长度") && checkName.contains("线要素拓扑检查")) {
            paramName = "min_segment_length";
        }
        else if (ruleName.contains("连通性容差")) {
            paramName = "connectivity_tolerance";
        }
        else if (ruleName.contains("网络缓冲距离")) {
            paramName = "network_buffer_distance";
        }
        else if (ruleName.contains("连通性阈值")) {
            paramName = "connectivity_threshold";
        }
        else if (ruleName.contains("水系要素连接容差")) {
            paramName = "water_system_connection_tolerance";
        }
        else if (ruleName.contains("河网重叠检查容差")) {
            paramName = "river_overlap_tolerance";
        }
        else if (ruleName.contains("河网交叉检查容差")) {
            paramName = "river_intersection_tolerance";
        }
        else if (ruleName.contains("河网超出检查容差")) {
            paramName = "river_exceeds_tolerance";
        }
        else if (ruleName.contains("库体归类补判容差") && checkName.contains("几何走向异常检查")) {
            paramName = "direction_classification_fallback_tolerance";
        }
        else if (ruleName.contains("库体归类补判容差") && checkName.contains("线要素拓扑检查")) {
            paramName = "topology_classification_fallback_tolerance";
        }
        else if (ruleName.contains("库体数据匹配容差") && checkName.contains("几何走向异常检查")) {
            paramName = "direction_database_match_tolerance";
        }
        else if (ruleName.contains("库体数据匹配容差") && checkName.contains("直角化")) {
            paramName = "right_angle_database_match_tolerance";
        }
        else if (ruleName.contains("库体数据匹配容差")) {
            paramName = "database_match_tolerance";
        }
        else if (ruleName.contains("直角化角度下限")) {
            paramName = "right_angle_lower_limit";
        }
        else if (ruleName.contains("直角化角度上限")) {
            paramName = "right_angle_upper_limit";
        }
        else if (ruleName.contains("检查对象图层")) {
            paramName = "series_target_layers";
        }
        else if (ruleName.contains("境界线要素图层")) {
            paramName = "series_boundary_line_layer";
        }
        else if (ruleName.contains("政区面要素图层")) {
            paramName = "series_admin_area_layer";
        }
        else if (ruleName.contains("道路要素图层")) {
            paramName = "series_road_layer";
        }
        else if (ruleName.contains("水系面要素图层") || ruleName.contains("面状水体要素图层") || ruleName.contains("面状水体图层")) {
            paramName = "series_water_area_layer";
        }
        else if (ruleName.contains("水系要素图层")) {
            paramName = "series_water_layer";
        }
        else if (ruleName.contains("同类要素名称扩展") || ruleName.contains("图层名称扩展") || ruleName.contains("图层别名")) {
            paramName = "series_layer_name_aliases";
        }
        else if (ruleName.contains("接边缓冲距离")) {
            paramName = "series_edge_buffer_distance";
        }
        else if (ruleName.contains("线要素接边容差")) {
            paramName = "series_line_edge_tolerance";
        }
        else if (ruleName.contains("端点闭合容差")) {
            paramName = "series_endpoint_closure_tolerance";
        }
        else if (ruleName.contains("特征点接边容差") || ruleName.contains("节点接边容差") || ruleName.contains("特征点容差")) {
            paramName = "series_feature_point_tolerance";
        }
        else if (ruleName.contains("跨比例尺覆盖阈值") || ruleName.contains("覆盖比例阈值")) {
            paramName = "series_cross_scale_overlap_threshold";
        }
        else if (ruleName.contains("跨比例尺弱支撑分数阈值") || ruleName.contains("弱支撑分数阈值")) {
            paramName = "series_cross_scale_weak_support_score";
        }
        else if (ruleName.contains("图幅关系拓扑容差") || ruleName.contains("相邻识别容差")) {
            paramName = "series_index_adjacency_tolerance";
        }
        else if (ruleName.contains("属性一致性字段")) {
            paramName = "series_attribute_fields";
        }
        else if (ruleName.contains("线要素样式一致性字段")) {
            paramName = "series_line_style_fields";
        }
        else if (ruleName.contains("面要素样式一致性字段")) {
            paramName = "series_polygon_style_fields";
        }
        else if (ruleName.contains("点要素样式一致性字段")) {
            paramName = "series_point_style_fields";
        }
        else if (ruleName.contains("河流注记分级规则")) {
            paramName = "series_river_annotation_grade_rules";
        }
        else if (ruleName.contains("河流分级注记规则来源") || ruleName.contains("河流注记分级规则来源")) {
            paramName = "series_river_annotation_grade_rule_source";
        }
        else if (ruleName.contains("河流等级字段") || ruleName.contains("河流分级字段")) {
            paramName = "series_river_level_fields";
        }
        else if (ruleName.contains("山脉山岭注记分级规则")) {
            paramName = "series_mountain_annotation_grade_rules";
        }
        else if (ruleName.contains("山脉山岭注记规则来源") || ruleName.contains("山脉山岭注记分级规则来源")) {
            paramName = "series_mountain_annotation_grade_rule_source";
        }
        else if (ruleName.contains("注记名称字段") || ruleName.contains("注记文本字段") || ruleName.contains("注记内容字段")) {
            paramName = "series_annotation_text_field";
        }
        else if (ruleName.contains("注记字号字段") || ruleName.contains("注记大小字段") || ruleName.contains("注记尺寸字段")) {
            paramName = "series_annotation_font_size_field";
        }
        else if (ruleName==QStringLiteral("项目图式标识")) {
            paramName="series_project_style_id";
        }
        else if (ruleName==QStringLiteral("项目图式来源")) {
            paramName="series_project_style_source";
        }
        else if (ruleName==QStringLiteral("主区角色图层")) {
            paramName="series_main_role_layers";
        }
        else if (ruleName==QStringLiteral("外区角色图层")) {
            paramName="series_external_role_layers";
        }
        else if (ruleName.contains("线状符号样式规范") || ruleName.contains("点状符号样式规范") || ruleName.contains("面状符号样式规范") || ruleName.contains("符号样式规范规则") || ruleName.contains("图式规则")) {
            paramName = "series_cartographic_symbol_rules";
        }
        else if (ruleName.contains("符号样式规范来源") || ruleName.contains("图式规则来源")) {
            paramName = "series_cartographic_symbol_rule_source";
        }
        else if (ruleName.contains("名称字段")) {
            paramName = "series_name_field";
        }
        else if (ruleName.contains("等级字段")) {
            paramName = "series_level_field";
        }
        else if (ruleName.contains("编码字段")) {
            paramName = "series_code_field";
        }
        else if (ruleName.contains("符号编码字段")) {
            paramName = "series_symbol_id_field";
        }
        else if (ruleName.contains("颜色字段")) {
            paramName = "series_color_field";
        }
        else if (ruleName.contains("线宽字段")) {
            paramName = "series_line_width_field";
        }
        else if (ruleName.contains("错误输出等级")) {
            paramName = "series_output_severity";
        }
        else if (ruleName.contains("几何相似度阈值")) {
            paramName = "right_angle_geometry_similarity_threshold";
        }
        else if (ruleName.contains("转折点检测容差")) {
            paramName = "right_angle_turning_point_tolerance";
        }
        else if (ruleName.contains("转折点匹配容差")) {
            paramName = "right_angle_turning_point_match_tolerance";
        }
        else if (ruleName.contains("最小线段长度") && checkName.contains("直角化")) {
            paramName = "right_angle_min_segment_length";
        }


        else if (ruleName.contains("最小河流长度")) {
            paramName = "min_river_length";
        }
        else if (ruleName.contains("交叉角度允许差值") && checkName.contains("几何走向异常检查")) {
            paramName = "intersection_angle_tolerance";
        }
        else if (ruleName.contains("交叉点检测容差")) {
            paramName = "intersection_detection_tolerance";
        }
        else if (ruleName.contains("悬挂线检测容差") && checkName.contains("几何走向异常检查")) {
            paramName = "direction_dangling_detection_tolerance";
        }
        else if (ruleName.contains(QStringLiteral("跨比例尺面聚合规则"))) {
            paramName = "series_cross_scale_polygon_aggregation_allowed";
        }
        else if (ruleName.contains(QStringLiteral("跨比例尺最大未支撑连续线段"))) {
            paramName = "series_cross_scale_max_uncovered_segment_length";
        }
        else if (ruleName.contains(QStringLiteral("跨比例尺稳定属性字段"))) {
            paramName = "series_cross_scale_attribute_fields";
        }
        else {
            paramName = ruleName.toLower().replace(" ", "_");
        }

        // 转换值
        bool ok;
        double doubleValue = value.toDouble(&ok);
        if (ok) {
            config.parameters[paramName] = doubleValue;
        }
        else if (value.compare("true", Qt::CaseInsensitive) == 0 || value.compare("false", Qt::CaseInsensitive) == 0) {
            config.parameters[paramName] = (value.compare("true", Qt::CaseInsensitive) == 0);
        }
        else {
            config.parameters[paramName] = value;
        }
        // 新增：保存单位字符串，供比例尺转换使用
        if (!unit.isEmpty()) {
            config.parameters[paramName + QStringLiteral("_unit")] = unit;
        }
    }

    m_checkItems[checkName] = config;
    return true;
}



CheckItemConfig QualityCheckConfig::getCheckItemConfig(const QString& type) const
{
    return m_checkItems.value(type);
}

QList<CheckItemConfig> QualityCheckConfig::getAllCheckItems() const
{
    return m_checkItems.values();
}

bool QualityCheckConfig::isCheckItemEnabled(const QString& type) const
{
    return m_checkItems.contains(type) && m_checkItems[type].enabled;
}

double QualityCheckConfig::getParameterDouble(const QString& checkType, const QString& paramName, double defaultValue) const
{
    if (!m_checkItems.contains(checkType)) {
        return defaultValue;
    }

    const auto& params = m_checkItems[checkType].parameters;
    if (!params.contains(paramName)) {
        return defaultValue;
    }

    return params[paramName].toDouble();
}

int QualityCheckConfig::getParameterInt(const QString& checkType, const QString& paramName, int defaultValue) const
{
    if (!m_checkItems.contains(checkType)) {
        return defaultValue;
    }

    const auto& params = m_checkItems[checkType].parameters;
    if (!params.contains(paramName)) {
        return defaultValue;
    }

    return params[paramName].toInt();
}

QString QualityCheckConfig::getParameterString(const QString& checkType, const QString& paramName, const QString& defaultValue) const
{
    if (!m_checkItems.contains(checkType)) {
        return defaultValue;
    }

    const auto& params = m_checkItems[checkType].parameters;
    if (!params.contains(paramName)) {
        return defaultValue;
    }

    return params[paramName].toString();
}

QString QualityCheckConfig::getSeriesBoundaryLineLayer() const { return getParameterString(kSeriesCheckName, "series_boundary_line_layer"); }
QString QualityCheckConfig::getSeriesAdminAreaLayer() const { return getParameterString(kSeriesCheckName, "series_admin_area_layer"); }
bool QualityCheckConfig::isSeriesSymmetricDifferenceEnabled() const
{
    return toConfigBool(m_checkItems.value(kSeriesCheckName).parameters.value("series_enable_symmetric_difference"));
}
double QualityCheckConfig::getSeriesFeaturePointTolerance() const { return getParameterDouble(kSeriesCheckName, "series_feature_point_tolerance", 0.0); }
QString QualityCheckConfig::getSeriesFeaturePointToleranceUnit() const { return getParameterString(kSeriesCheckName, "series_feature_point_tolerance_unit", QStringLiteral("毫米")); }
double QualityCheckConfig::getSeriesMinAreaTolerance() const { return getParameterDouble(kSeriesCheckName, "series_min_area_tolerance", 0.0); }
QString QualityCheckConfig::getSeriesMinAreaToleranceUnit() const { return getParameterString(kSeriesCheckName, "series_min_area_tolerance_unit", QStringLiteral("平方米")); }
QStringList QualityCheckConfig::getSeriesAttributeConsistencyFields() const { return splitSeriesFields(getParameterString(kSeriesCheckName, "series_attribute_fields")); }
QStringList QualityCheckConfig::getSeriesLineStyleConsistencyFields() const { return splitSeriesFields(getParameterString(kSeriesCheckName, "series_line_style_fields")); }
QStringList QualityCheckConfig::getSeriesPolygonStyleConsistencyFields() const { return splitSeriesFields(getParameterString(kSeriesCheckName, "series_polygon_style_fields")); }
QStringList QualityCheckConfig::getSeriesPointStyleConsistencyFields() const { return splitSeriesFields(getParameterString(kSeriesCheckName, "series_point_style_fields")); }
bool QualityCheckConfig::validateSeriesMapTemplate(QStringList* errors) const
{
    // 501 图幅接边与跨比例尺检查使用的规则完整性校验。
    QStringList localErrors;
    if (!m_checkItems.contains(kSeriesCheckName)) localErrors << QStringLiteral("缺少检查项：图幅接边与系列化表达");
    if (getParameterString(kSeriesCheckName, "series_target_layers").trimmed().isEmpty()) localErrors << QStringLiteral("缺少规则：检查对象图层");
    if (getSeriesBoundaryLineLayer().trimmed().isEmpty()) localErrors << QStringLiteral("缺少规则：境界线要素图层");
    if (getSeriesAdminAreaLayer().trimmed().isEmpty()) localErrors << QStringLiteral("缺少规则：政区面要素图层");
    if (getParameterDouble(kSeriesCheckName, "series_edge_buffer_distance", 0.0) <= 0.0) localErrors << QStringLiteral("规则“接边缓冲距离”必须为正数");
    if (getParameterDouble(kSeriesCheckName, "series_line_edge_tolerance", 0.0) <= 0.0) localErrors << QStringLiteral("规则“线要素接边容差”必须为正数");
    if (getParameterDouble(kSeriesCheckName, "series_endpoint_closure_tolerance", 0.0) <= 0.0) localErrors << QStringLiteral("规则“端点闭合容差”必须为正数");
    if (getSeriesLineStyleConsistencyFields().isEmpty()) localErrors << QStringLiteral("缺少规则：线要素样式一致性字段");
    if (getSeriesPolygonStyleConsistencyFields().isEmpty()) localErrors << QStringLiteral("缺少规则：面要素样式一致性字段");
    if (getSeriesPointStyleConsistencyFields().isEmpty()) localErrors << QStringLiteral("缺少规则：点要素样式一致性字段");
    if (getSeriesMinAreaTolerance() < 0.0) localErrors << QStringLiteral("规则“面积容差下限”不能小于0");
    if (errors) *errors = localErrors;
    return localErrors.isEmpty();
}

double QualityCheckConfig::getDirectionDatabaseMatchTolerance() const
{
    return getParameterDouble("几何走向异常检查", "direction_database_match_tolerance", 10.0);
}


double QualityCheckConfig::getMinRiverLength() const
{
    return getParameterDouble("几何走向异常检查", "min_river_length", 100.0);
}

double QualityCheckConfig::getIntersectionDetectionTolerance() const
{
    return getParameterDouble("几何走向异常检查", "intersection_detection_tolerance", 5.0);
}

double QualityCheckConfig::getDirectionDanglingDetectionTolerance() const
{
    return getParameterDouble("几何走向异常检查", "direction_dangling_detection_tolerance", 10.0);
}
double QualityCheckConfig::getOverlapTolerance() const
{
    return getParameterDouble("面要素重叠/间隙检查", "overlap_tolerance");
}

double QualityCheckConfig::getGapTolerance() const
{
    return getParameterDouble("面要素重叠/间隙检查", "gap_tolerance");
}

double QualityCheckConfig::getMinOverlapArea() const
{
    return getParameterDouble("面要素重叠/间隙检查", "min_overlap_area");
}

double QualityCheckConfig::getDanglingTolerance() const
{
    return getParameterDouble("空间拓扑关系错误检查", "dangling_tolerance");
}

double QualityCheckConfig::getMinLineLength() const
{
    return getParameterDouble("空间拓扑关系错误检查", "min_line_length");
}

double QualityCheckConfig::getConnectivityTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "connectivity_tolerance");

}
double QualityCheckConfig::getLineTopologyDanglingTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "dangling_tolerance");
}

double QualityCheckConfig::getRiverOverlapToleranceNew() const
{
    return getParameterDouble("线要素拓扑检查", "river_overlap_tolerance");
}
// 河流检查专用参数获取方法实现
double QualityCheckConfig::getRiverDanglingTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "dangling_tolerance");
}

double QualityCheckConfig::getRiverIsolationDistance() const
{
    return getParameterDouble("线要素拓扑检查", "isolation_distance");
}

double QualityCheckConfig::getRiverMinLineLength() const
{
    return getParameterDouble("线要素拓扑检查", "min_segment_length");
}
// 添加河网通达性检查相关参数获取方法
double QualityCheckConfig::getRiverOverlapTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "river_overlap_tolerance");
}

double QualityCheckConfig::getRiverIntersectionTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "river_intersection_tolerance");
}

double QualityCheckConfig::getRiverExceedsTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "river_exceeds_tolerance");
}

double QualityCheckConfig::getDatabaseMatchTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "database_match_tolerance");
}

double QualityCheckConfig::getWaterSystemConnectionTolerance() const
{
    return getParameterDouble("线要素拓扑检查", "water_system_connection_tolerance");
}
// 添加缺少的函数实现
double QualityCheckConfig::getLineTopologyMinLineLength() const
{
    return getParameterDouble("线要素拓扑检查", "min_segment_length");
}

double QualityCheckConfig::getLineTopologyIsolationDistance() const
{
    return getParameterDouble("线要素拓扑检查", "isolation_distance");
}

double QualityCheckConfig::getWaterSystemConnectionToleranceNew() const
{
    return getParameterDouble("线要素拓扑检查", "water_system_connection_tolerance");
}

double QualityCheckConfig::getRiverIntersectionToleranceNew() const
{
    return getParameterDouble("线要素拓扑检查", "river_intersection_tolerance");
}

double QualityCheckConfig::getRiverExceedsToleranceNew() const
{
    return getParameterDouble("线要素拓扑检查", "river_exceeds_tolerance");
}

double QualityCheckConfig::getDatabaseMatchToleranceNew() const
{
    return getParameterDouble("线要素拓扑检查", "database_match_tolerance");
}
// 实现参数获取方法
double QualityCheckConfig::getRightAngleLowerLimit() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_lower_limit");
}

double QualityCheckConfig::getRightAngleUpperLimit() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_upper_limit");
}

double QualityCheckConfig::getRightAngleDatabaseMatchTolerance() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_database_match_tolerance");
}

double QualityCheckConfig::getRightAngleGeometrySimilarityThreshold() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_geometry_similarity_threshold");
}

double QualityCheckConfig::getRightAngleTurningPointTolerance() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_turning_point_tolerance");
}
double QualityCheckConfig::getRightAngleTurningPointMatchTolerance() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_turning_point_match_tolerance");
}
double QualityCheckConfig::getRightAngleMinSegmentLength() const
{
    return getParameterDouble("直角化角度阈值控制", "right_angle_min_segment_length");
}
