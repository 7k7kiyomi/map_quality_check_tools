#ifndef SE_ANNOTATION_CONTENT_CHECK_ALGORITHM_H
#define SE_ANNOTATION_CONTENT_CHECK_ALGORITHM_H

#include "../commontype/se_error_define.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <QMap>
#include <memory>
#include <spdlog/logger.h>

class QgsVectorLayer;
class QgsPointXY;

/**
 * @brief 注记内容合规性检查算法（ID=213）
 *
 * 基于200（注记匹配）的输出结果，检查注记内容是否与GDB源名称一致。
 * 只关注内容层面的错误（内容不匹配、内容错误），不涉及符号相关检查。
 *
 * 输入：200算法的匹配结果SHP（含match_status/assoc_name/text_conte等字段）
 * 输出：按源名称分组的内容错误结果列表
 */
class SE_AnnotationContentCheckAlgorithm
{
public:
    explicit SE_AnnotationContentCheckAlgorithm(std::shared_ptr<spdlog::logger> logger);
    ~SE_AnnotationContentCheckAlgorithm();

    /**
     * @brief 加载检查规则（ID=213）
     * @param schemaPath 规则文件路径
     * @param mapScale 地图比例尺分母
     * @return 是否加载成功
     */
    bool loadRules(const QString &schemaPath, double mapScale);

    /**
     * @brief 执行注记内容合规性检查
     * @param matchResultLayer 200算法的匹配结果SHP图层
     * @param gdbLayer GDB图层（可选，用于间距检查）
     * @return 检查结果列表
     */
    QList<AnnotationCheckResult> checkContent(
        QgsVectorLayer *matchResultLayer,
        QgsVectorLayer *gdbLayer = nullptr);

private:
    // 规则参数
    double m_minDistanceMm = 2.0;  // 最小间距（毫米）
    double m_maxDistanceMm = 5.0;  // 最大间距（毫米）
    double m_mapScale = 0.0;       // 当前比例尺分母

    // 日志记录器
    std::shared_ptr<spdlog::logger> m_logger;

    /**
     * @brief 将毫米转换为地图单位
     */
    inline double mmToMapUnits(double mm) const
    {
        return (m_mapScale > 0.0) ? (mm * m_mapScale / 1000.0) : mm;
    }
};

#endif // SE_ANNOTATION_CONTENT_CHECK_ALGORITHM_H
