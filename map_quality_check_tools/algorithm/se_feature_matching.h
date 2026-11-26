#pragma once
#include <QString>
#include <QList>
#include <QHash>
#include <QSet>
#include <QVariant>
#include <QPointF>
#include <QgsPointXY.h>   // 使用QgsPointXY
#include <QgsGeometry.h>  // 使用QgsGeometry
#include <QgsRectangle.h> // 显式包含
#include <qgsspatialindex.h>
#include <algorithm>

class QgsVectorLayer;

enum MatchStatus
{
    NOT_MATCHED,
    FULLY_MATCHED,     // 完全匹配
    PARTIALLY_MATCHED, // 部分匹配 (注记与GDB名称不完全一致)
    UNMATCHED          // 新增：未匹配的注记
};

/**
 * @brief 单个要素的三方匹配结果 (结构体不变)
 */
struct FeatureMatchLink
{
    qint64 annoFid = -1; // 注记要素ID
    qint64 gdbFid = -1;
    QList<qint64> gdbFids; // 新增：构成该逻辑要素的所有原始 GDB FID
    QString assoc_name;
    QList<qint64> actualFids;
    QString symbolId;

    // 用于符号一致性检查的额外信息
    QString annoText;       // 注记文本内容
    double annoAngle = 0.0; // 注记角度
    QPointF annoPosition;   // 注记位置
    QPointF gdbCentroid;    // GDB要素中心点

    MatchStatus status = FULLY_MATCHED; // 默认是完全匹配
    double matchScore = 1.0;            // 匹配得分, 1.0代表完美

    // 新增：匹配到的完整 GDB 名称

    // 添加默认构造函数确保成员初始化
    FeatureMatchLink()
        : annoFid(-1), gdbFid(-1), annoAngle(0.0)
    {
    }
};

struct ActualMatchResult
{
    QList<qint64> fids;
    QString symbolId; // 只有当匹配到符号时才有效
};

// -- NEW: 用于预处理和缓存注记信息 --
struct AnnotationInfo
{
    qint64 fid;
    QString text;
    QgsGeometry geom;
    QgsPointXY center;
    double fontSize;  // 用于计算搜索半径
    QString fontFamily; // 字体族（如"微软雅黑"），用于防止跨河拼组
};

// -- 注记组结构体，修正了 calculateMetrics 方法 --
struct AnnotationGroup
{
    QList<qint64> fids;
    QList<AnnotationInfo> members;
    QString combinedText;
    QgsGeometry bounds;
    QgsPointXY center;

    void calculateMetrics()
    {
        if (members.isEmpty())
            return;

        // FIX: 使用 std::sort 对成员进行排序
        std::sort(members.begin(), members.end(), [](const AnnotationInfo &a, const AnnotationInfo &b)
                  {
                      if (qAbs(a.center.x() - b.center.x()) > qAbs(a.center.y() - b.center.y()))
                      {
                          return a.center.x() < b.center.x(); // 主要沿X轴排列
                      }
                      return a.center.y() < b.center.y(); // 主要沿Y轴排列
                  });

        QgsRectangle groupBox;
        double sumX = 0, sumY = 0;
        combinedText.clear();
        fids.clear(); // 清空以重新填充

        for (const auto &member : members)
        {
            fids.append(member.fid);
            if (groupBox.isEmpty())
            {
                groupBox = member.geom.boundingBox();
            }
            else
            {
                // FIX: 使用 combineExtentWith 来合并两个 QgsRectangle（QgsRectangle 没有 combine 成员）
                groupBox.combineExtentWith(member.geom.boundingBox());
            }
            sumX += member.center.x();
            sumY += member.center.y();
            combinedText.append(member.text);
        }

        // FIX: 使用 QgsGeometry::fromRect 将矩形转为几何
        bounds = QgsGeometry::fromRect(groupBox);
        center = QgsPointXY(sumX / members.size(), sumY / members.size());
    }
};

// --- NEW: 定义一个新的返回结构体，包含匹配结果和分组信息 ---
struct MatchResult
{
    QList<FeatureMatchLink> links;       // <-- 恢复为值类型
    QMap<qint64, qint64> annoToGroupMap; // annoFid -> groupId
};

/**
 * @brief 要素匹配器 - 单规则处理版本
 */
class FeatureMatcher
{
public:
    static FeatureMatcher &instance();

    /**
     * @brief 执行单行规则的要素匹配
     * @param standardLayer 标准图层名称
     * @param gdbLayerPath GDB图层文件路径
     * @param levelAttr 分层属性字段名（可空）
     * @param &levelValuesCsv 外部传入的允许级别值（逗号或分号分隔
     * @param actualLayerPath 实际要素图层路径
     * @param actualAnnoLayerPath 实际注记图层路径
     * @param gdbLabelField GDB标注字段名
     * @param mapScale 地图比例尺（如"1:100000"）
     * @param errMsg 错误信息输出
     * @return 匹配结果列表，空列表表示失败
     */
    MatchResult matchSingleRule(
        const QString &standardLayer,
        const QString &gdbLayerPath,
        const QString &levelAttr,
        const QString &levelValuesCsv,
        const QString &actualLayerPath,
        const QString &actualAnnoLayerPath,
        const QString &gdbLabelField,
        const QString &mapScale,
        QString &errMsg,
        const QSet<qint64> &excludedAnnoFids = QSet<qint64>());

    /**
     * @brief 根据匹配结果生成临时标准图层
     * @param matches 匹配结果
     * @param annoToGroupMap 注记FID到组ID的映射
     * @param annoLayer 注记图层
     * @param gdbLayer GDB图层
     * @param actualLayer 实际图层
     * @param outputPath 输出临时图层路径
     * @param outputUnmatchedAnnotations 是否输出未匹配的注记（默认false）
     * @param errMsg 错误消息
     * @return 是否成功
     */
    // 改为按值传参，避免悬空引用
    bool createTempStandardLayer(
        const QList<FeatureMatchLink> &matches,
        const QMap<qint64, qint64> &annoToGroupMap,
        QgsVectorLayer *annoLayer,
        QgsVectorLayer *gdbLayer,
        QgsVectorLayer *actualLayer,
        const QString &outputPath,
        bool outputUnmatchedAnnotations,
        double nearbyRadius,
        int gdbLevelIdx,
        const QSet<QString> &levelValues,
        QString &errMsg,
        const QSet<qint64> &excludedAnnoFids = QSet<qint64>());

    void clear();

    ActualMatchResult matchActualBySpatial(
        QgsVectorLayer *gdbLayer, qint64 gdbFid,
        QgsVectorLayer *actualLayer, const QString &mapScale,
        // -- 新增参数 --
        const QMap<QString, QList<qint64>> &symbolIdToFidsMap,
        const QgsSpatialIndex &symbolComponentIndex,
        const QMap<QgsFeatureId, QString> &componentFidToSymbolIdMap,
        const QMap<QString, QgsPointXY> &symbolIdToCenterMap);

private:
    FeatureMatcher() = default;

    // -- REFACTORED: 内部辅助函数 --
    QgsVectorLayer *loadLayer(const QString &path, QString &errMsg);
    QSet<QString> inferLevelValues(const QString &standardLayer, const QString &levelAttr);

private:
    QList<QgsVectorLayer *> m_tempLayers; // 临时加载的图层，需要清理
};
