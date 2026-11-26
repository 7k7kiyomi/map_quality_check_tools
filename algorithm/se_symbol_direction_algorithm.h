#ifndef SE_SYMBOL_DIRECTION_ALGORITHM_H
#define SE_SYMBOL_DIRECTION_ALGORITHM_H

#include "../commontype/se_error_define.h"
#include <QList>
#include <QMap>
#include <QString>
#include <memory>
#include <spdlog/logger.h>

class QgsVectorLayer;
class QgsGeometry;
class QgsPointXY;

// 符号方向检查类型枚举
enum class DirectionCheckType
{
    PERPENDICULAR,  // 垂直关系
    PARALLEL,       // 平行关系
    DIRECTION_MATCH // 方向一致（原"指向"）
};

enum class DirectionCheckMode
{
    SYMBOL,
    FEATURE
};

// 符号方向检查规则
struct DirectionCheckRule
{
    QString symbolType;                                        // 符号类型名称
    DirectionCheckType checkType;                              // 检查类型
    QStringList associatedLayers;                              // 关联图层名称列表
    double searchRadius;                                       // 搜索半径（地图单位）
    double angleThreshold;                                     // 角度阈值（度）
    QString symbolIdField;                                     // 符号ID字段名（默认"symbol_id"）
    DirectionCheckMode checkMode = DirectionCheckMode::SYMBOL; // 默认是符号模式

    DirectionCheckRule()
        : checkType(DirectionCheckType::PERPENDICULAR), searchRadius(50.0), angleThreshold(15.0), symbolIdField("symbol_id")
    {
    }
};

class SE_SymbolDirectionAlgorithm
{
public:
    explicit SE_SymbolDirectionAlgorithm(std::shared_ptr<spdlog::logger> logger);
    ~SE_SymbolDirectionAlgorithm();

    /**
     * @brief 加载符号方向检查规则（ID=209）
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @param rule [out] 解析出的检查规则
     * @return 是否加载成功
     */
    bool loadDirectionCheckRules(
        const QString &schemaPath,
        double mapScale,
        DirectionCheckRule &rule);

    /**
     * @brief 执行符号方向检查
     * @param symbolLayer 待检查的符号图层
     * @param associatedLayers 关联图层列表（键为图层名称）
     * @param rule 检查规则
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkSymbolDirection(
        QgsVectorLayer *symbolLayer,
        const QMap<QString, QgsVectorLayer *> &associatedLayers,
        const DirectionCheckRule &rule);

private:
    std::shared_ptr<spdlog::logger> m_logger;
    double m_mapScale = 0.0;

    // 辅助函数：计算几何要素的主方向（0-180度）
    double calculateGeometryDirection(const QgsGeometry &geometry);

    QList<AnnotationCheckResult> checkFeaturesDirection(
        QgsVectorLayer* targetLayer,
        const QMap<QString, QgsVectorLayer*>& associatedLayers,
        const DirectionCheckRule& rule,
        std::shared_ptr<spdlog::logger> logger);

    // 辅助函数：计算点到线/面要素最近点的切线方向
    double calculateTangentDirection(
        const QgsPointXY &point,
        const QgsGeometry &lineOrPolygon);

    // 辅助函数：计算两个角度之间的最小夹角（0-90度）
    double calculateAngleDifference(double angle1, double angle2);

    // 辅助函数：判断角度是否接近垂直（接近90度）
    bool isNearPerpendicular(double angleDiff, double threshold);

    // 辅助函数：判断角度是否接近平行（接近0度）
    bool isNearParallel(double angleDiff, double threshold);

    // 辅助函数：查找符号周围最近的关联要素
    QgsGeometry findNearestAssociatedFeature(
        const QgsPointXY &symbolCenter,
        const QMap<QString, QgsVectorLayer *> &associatedLayers,
        double searchRadius,
        QString &foundLayerName);

    // 辅助函数：计算符号的几何中心（基于symbol_id聚合）
    QgsPointXY calculateSymbolCenter(
        QgsVectorLayer *symbolLayer,
        const QString &symbolId,
        const QString &symbolIdField);

    // 辅助函数：获取符号的代表性几何（用于方向计算）
    // 新增：针对道路代码标注，获取其外包框（element_ty='path'）
    QgsGeometry getSymbolRepresentativeGeometry(
        QgsVectorLayer *symbolLayer,
        const QString &symbolId,
        const QString &symbolIdField);

    // 将毫米转换为地图单位
    inline double mmToMapUnits(double mm) const
    {
        return (m_mapScale > 0.0) ? (mm * m_mapScale / 1000.0) : mm;
    }
};

#endif // SE_SYMBOL_DIRECTION_ALGORITHM_H