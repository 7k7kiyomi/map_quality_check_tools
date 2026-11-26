#ifndef SE_ANNOTATION_ADVANCED_ALGORITHM_H
#define SE_ANNOTATION_ADVANCED_ALGORITHM_H

#include "../commontype/se_error_define.h"
#include <QList>
#include <QMap>
#include <QStringList>
#include <QVariant>
#include <memory>
#include <spdlog/logger.h>

class QgsVectorLayer;
class QgsGeometry;
class QgsFeature;
class QgsPointXY;

// 内部使用的要素数据结构
struct ElementInfo
{
    qint64 id;
    QString layerName;
    QgsGeometry *geometry = nullptr;          // 原始几何（可能包含冗余空间）
    QgsGeometry *effectiveGeometry = nullptr; // 有效几何（收缩后的精确范围）
    QString elementType;                      // "symbol" 或 "text"
    QVariant originalFeatureId;               // 原始要素ID或symbol_id
    QList<QVariant> componentFeatureIds;      // 符号组成要素ID列表（注记为空）

    // 新增：用于存储文本实际尺寸信息
    double textWidth = 0.0;
    double textHeight = 0.0;
    double fontSize = 0.0;
    QString textContent; // <-- 新增
};

class SE_AnnotationAdvancedAlgorithm
{
public:
    explicit SE_AnnotationAdvancedAlgorithm(std::shared_ptr<spdlog::logger> logger);
    ~SE_AnnotationAdvancedAlgorithm();

    /**
     * @brief 加载重叠检查规则
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @return 是否加载成功
     */
    bool loadOverlapRules(const QString &schemaPath, double mapScale);

    /**
     * @brief 加载符号注记匹配检查规则
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @param checkItemId check_item的id，默认"210"；213调用时传"213"
     * @return 是否加载成功
     */
    bool loadSymbolAnnotationMatchRules(const QString &schemaPath, double mapScale,
                                        const QString &checkItemId = "210");

    /**
     * @brief 手动设置匹配参数（用于213等不依赖210XML的调用方）
     * @param allowNoAnnotation 是否允许符号没有注记
     * @param allowNoSymbol 是否允许注记没有符号
     * @param minDistanceMm 最小间距（毫米）
     * @param maxDistanceMm 最大间距（毫米）
     */
    void setMatchParams(bool allowNoAnnotation, bool allowNoSymbol,
                        double minDistanceMm, double maxDistanceMm);

    /**
     * @brief 设置地图比例尺分母（用于213等跳过210直接设参的场景）
     */
    void setMapScale(double mapScale) { m_mapScale = mapScale; }

    /**
     * @brief 加载字距检查规则（ID=204）
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @return 是否加载成功
     */
    bool loadCharSpacingRules(const QString &schemaPath, double mapScale);

    /**
     * @brief 加载注记组距检查规则（ID=207）
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @return 是否加载成功
     */
    bool loadGroupSpacingRules(const QString &schemaPath, double mapScale);

    /**
     * @brief 加载面要素等间距标注检查规则（ID=208）
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @return 是否加载成功
     */
    bool loadEqualSpacingRules(const QString &schemaPath, double mapScale);

    /**
     * @brief 执行全局重叠检查
     * @param symbolLayerPaths 符号图层路径列表
     * @param annotationLayerPaths 注记图层路径列表
     * @param allElements [out] 所有加载和预处理过的要素列表
     * @param loadedLayers [out] 所有加载的QGIS图层指针列表
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkOverlap(
        const QStringList &symbolLayerPaths,
        const QStringList &annotationLayerPaths,
        QList<ElementInfo> &allElements,
        QList<QgsVectorLayer *> &loadedLayers);

    /**
     * @brief 执行符号注记匹配检查（ID=210）
     * @param matchResultLayer 注记-要素匹配结果图层
     * @param actualLayer 实际符号图层（用于验证符号存在性和计算间距）
     * @param gdbLayer GDB图层（用于线/面要素缺失注记检查，可为空）
     * @param errorMessage 检查无法执行时的错误信息（可为空）
     * @param annoPolygonLayer 原始注记Polygon图层（用于孤儿注记检测，可为空）
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkSymbolAnnotationMatch(
        QgsVectorLayer *matchResultLayer,
        QgsVectorLayer *actualLayer,
        QgsVectorLayer *gdbLayer = nullptr,
        QString *errorMessage = nullptr,
        QgsVectorLayer *annoPolygonLayer = nullptr);

    /**
     * @brief 执行字距上下限及可选的散列注记等间距检查（ID=204）
     * @param matchResultLayer 注记-要素匹配结果图层
     * @param gdbLayer GDB图层（用于获取几何信息）
     * @param statistics [out] 等间距检查覆盖情况及无法判定的组（可为空）
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkCharSpacing(
        QgsVectorLayer *matchResultLayer,
        QgsVectorLayer *gdbLayer,
        QMap<QString, QString> *statistics = nullptr);

    /**
     * @brief 执行注记组距检查（ID=207）
     * @param matchResultLayer 注记-要素匹配结果图层
     * @param statistics [out] 统计信息（河流/道路的注记组分布）
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkGroupSpacing(
        QgsVectorLayer *matchResultLayer,
        QMap<QString, QString> &statistics);

    /**
     * @brief 执行面要素等间距标注检查（ID=208）
     * @param matchResultLayer 注记-要素匹配结果图层
     * @param gdbLayer GDB图层（用于获取面要素几何）
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkEqualSpacing(
        QgsVectorLayer *matchResultLayer,
        QgsVectorLayer *gdbLayer);

    /**
     * @brief 通用规则解析函数
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @param checkItemId 检查项ID（如"206"、"210"、"213"）
     * @param rules [out] 解析出的规则键值对
     * @return 是否解析成功
     */
    bool parseCheckRules(
        const QString &schemaPath,
        double mapScale,
        const QString &checkItemId,
        QMap<QString, QString> &rules);

private:
    // 规则参数
    bool m_allowOverlap = false;
    double m_overlapTolerance = 0.0; // 地图单位的容差（用于判断最小重叠面积阈值）
    double m_textShrinkFactor = 0.8; // 文本框收缩系数，默认87.5%（可配置）
    double m_mapScale = 0.0;         // 当前比例尺分母（如 145000）

    // 日志记录器
    std::shared_ptr<spdlog::logger> m_logger;

    // 新增：符号注记匹配规则参数（ID=210）
    bool m_allowNoAnnotation = false; // 是否允许符号没有注记
    bool m_allowNoSymbol = false;     // 是否允许注记没有符号
    double m_minDistanceMm = 0.2;     // 最小间距（毫米）
    double m_maxDistanceMm = 0.5;     // 最大间距（毫米）
    QString m_matchLevelAttr;         // ID=200：GDB分层属性字段
    QString m_matchLevelValuesCsv;     // ID=200：GDB分层属性值列表
    QString m_matchLabelField;         // ID=200：GDB标注字段
    bool m_matchRulesLoaded = false;    // 是否成功加载ID=200规则

    // 新增：字距检查规则参数（ID=204）
    double m_minCharSpacingMm = 0.1;    // 最小字距（毫米）
    double m_maxCharSpacingMm = 2.0;    // 最大字距（毫米）
    bool m_checkLineFeatures = true;    // 是否检查线要素注记
    bool m_checkPolygonFeatures = true; // 是否检查面要素注记
    bool m_checkCharEqualSpacing = false;
    double m_charSpacingDeviation = 0.10;

    QList<AnnotationCheckResult> checkCharEqualSpacing(
        QgsVectorLayer *matchResultLayer, QMap<QString, QString> *statistics);

    // 新增：注记组距检查规则参数（ID=207）
    double m_minGroupSpacingMm = 0.3; // 最小组距（毫米）
    double m_maxGroupSpacingMm = 0.5; // 最大组距（毫米）

    // 新增：面要素等间距标注检查规则参数（ID=208）
    int m_minAnnotationCount = 3;    // 最小标注数量
    double m_allowedDeviation = 0.2; // 允许偏差比例

    // 辅助函数
    /**
     * @brief 从图层加载并预处理要素
     * @param layerPaths 图层路径列表
     * @param elementType 要标记的元素类型 ("symbol" 或 "text")
     * @param allElements [out] 用于存储所有加载要素的列表
     * @param loadedLayers [out] 用于存储已加载的图层指针，便于后续释放
     */
    void loadAndPreprocessFeatures(const QStringList &layerPaths, const QString &elementType, QList<ElementInfo> &allElements, QList<QgsVectorLayer *> &loadedLayers);

    /**
     * @brief 计算文本有效几何（收缩冗余矩形框）
     * @param originalGeometry 原始几何对象
     * @param fontSizePt 文本尺寸（点单位）
     * @param textContent 文本内容（点单位）
     * @return 收缩后的有效几何对象
     */
    QgsGeometry *calculateEffectiveTextGeometry(
        QgsGeometry *originalGeometry,
        double fontSizePt,         // <-- 改为 font_size
        const QString &textContent // <-- 改为 textContent
    );

    /**
     * @brief 将毫米转换为地图单位（内联工具函数）
     */
    inline double mmToMapUnits(double mm) const
    {
        return (m_mapScale > 0.0) ? (mm * m_mapScale / 1000.0) : mm;
    }

    /**
     * @brief 计算符号的几何中心（基于symbol_id聚合）
     * @param actualLayer 实际要素图层
     * @param symbolId 符号ID
     * @return 符号中心点
     */
    QgsPointXY calculateSymbolCenter(QgsVectorLayer *actualLayer, const QString &symbolId);
};

#endif // SE_ANNOTATION_ADVANCED_ALGORITHM_H
