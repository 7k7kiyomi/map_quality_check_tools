#pragma once

#include <QtWidgets/QDialog>
#include <QString>
#include <QStringList>
#include <QJsonArray>
#include <QJsonObject>

#include <functional>
#include <memory>
#include <vector>

#include "ogrsf_frmts.h"

QT_BEGIN_NAMESPACE
namespace Ui {
    class se_boundary_jump_draw_check_algorithm;
}
QT_END_NAMESPACE

// XML 中 ID=116 的“境界线缺失和跳绘检查”规则。
// 前半部分保留 XML 原始图上值，后半部分保存按比例尺换算后的图层坐标值。
struct BoundaryJumpRule
{
    // XML 原始图上值。
    double overlapLengthCm = 0.0;
    double topologyToleranceMm = 0.0;
    double generalizedLineMatchDistanceMm = 0.0;
    double boundaryCandidateSearchDistanceMm = 0.0;
    double drawDistanceMinMm = 0.0;
    double drawDistanceMaxMm = 0.0;
    double nonJumpOffsetDistanceMm = 0.0;
    double directionAngleThresholdDeg = 0.0;
    double missingBoundaryBufferRadiusMm = 0.0;
    double boundarySymbolLengthMm = 0.0;
    double maxJumpStepCoefficient = 0.0;
    QString overlapLengthUnit;
    QString topologyToleranceUnit;
    QString generalizedLineMatchDistanceUnit;
    QString boundaryCandidateSearchDistanceUnit;
    QString drawDistanceUnit;
    QString nonJumpOffsetDistanceUnit;
    QString directionAngleThresholdUnit;
    QString missingBoundaryBufferRadiusUnit;
    QString boundarySymbolLengthUnit;
    QString maxJumpStepCoefficientUnit;
    QString missingBoundaryAttributeName;
    QString missingBoundaryAttributeValue;

    // 换算值：OGR 的 Distance/Buffer 使用图层坐标单位。
    double scaleDenominator = 0.0;
    double overlapLength = 0.0;
    // XML“绘制距离”的闭区间上下限。
    double drawDistanceMin = 0.0;
    double drawDistanceMax = 0.0;
    double topologyTolerance = 0.0; // XML“重叠拓扑容差”换算后的值
    // XML“匹配缓冲区半径”换算后的值，仅用于原始线状要素匹配综合后线状要素。
    double generalizedLineMatchDistance = 0.0;
    // XML“偏移距离”换算后的值，用于非跳绘区成果境界线相对库体数据的检查。
    double nonJumpOffsetDistance = 0.0;
    // XML“搜索缓冲区半径”换算后的值，不参与合规距离判定。
    double boundaryCandidateSearchDistance = 0.0;
    // XML“缓冲区半径”换算后的值，用于未参与跳绘的 GDB 境界缺失检查。
    double missingBoundaryBufferRadius = 0.0;
    // XML“境界线符号长度”换算后的值，用于跳绘区理论分段数计算。
    double boundarySymbolLength = 0.0;
    // 最大跳绘步长，等于“最大跳绘步长系数”乘以境界线符号长度。
    double maxJumpStep = 0.0;
};

// GDALDataset 必须用 GDALClose 关闭，不能直接 delete。
struct JumpGdalDatasetCloser
{
    void operator()(GDALDataset* dataset) const
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};

using JumpGdalDatasetPtr = std::unique_ptr<GDALDataset, JumpGdalDatasetCloser>;

// OGRGeometry 交回 GDAL 销毁，避免 Windows 下跨 DLL 释放内存。
struct JumpOgrGeometryDestroyer
{
    void operator()(OGRGeometry* geometry) const
    {
        if (geometry != nullptr)
            OGRGeometryFactory::destroyGeometry(geometry);
    }
};

using JumpOgrGeometryPtr = std::unique_ptr<OGRGeometry, JumpOgrGeometryDestroyer>;

struct BoundaryJumpIssue
{
    QString type;                  // DRAW_DIST_ERR / WATER_AREA_OUTSIDE_ERR / JUMP_BREAK_ERR / WATER_TOPO_ERR / BOUNDARY_MISSING_ERR / NON_JUMP_OFFSET_ERR
    QString boundaryLayerName;     // 境界线来源图层
    GIntBig boundaryFid = -1;       // 境界线要素 FID
    QString otherLayerName;        // 冲突图面要素来源图层
    GIntBig otherFid = -1;          // 冲突图面要素 FID
    // 仅供内部汇总使用。不同跳绘区不可因原始关联链 FID 相同而合并几何。
    QString mergeKey;
    QString message;               // 输出到日志和错误图层的问题说明
    JumpOgrGeometryPtr geometry;    // 问题定位几何，最终写入错误图层

    BoundaryJumpIssue() = default;
    BoundaryJumpIssue(BoundaryJumpIssue&&) noexcept = default;
    BoundaryJumpIssue& operator=(BoundaryJumpIssue&&) noexcept = default;

    BoundaryJumpIssue(const BoundaryJumpIssue&) = delete;
    BoundaryJumpIssue& operator=(const BoundaryJumpIssue&) = delete;
};

// 行政界线与参与境界表达的线状要素之间的有效关联关系。
struct BoundaryLineRelation
{
    // relations 数组中的连续原始关联链编号。综合表达匹配后保留该编号，
    // 用于合并同一原始链与同一水系面产生的重复局部关联。
    int originalRelationIndex = -1;
    QString lineLayerName;
    GIntBig lineFid = -1;
    double overlapLength = 0.0;
    JumpOgrGeometryPtr overlapGeometry; // 行政界线上的有效重叠段
    // 原始线状要素完整几何：仅用于以红绿线本体求交确定 P1、P2。
    JumpOgrGeometryPtr sourceLineGeometry;
    JumpOgrGeometryPtr lineGeometry;    // 原始线状要素局部段：仅用于搜索成果境界线
    QString generalizedLineLayerName;
    GIntBig generalizedLineFid = -1;
    // 综合后水系面：成果境界线落在面内即为合格。
    bool generalizedIsArea = false;
    // 水系面是否覆盖整条理论跳绘区。局部相交仅开展水面内绘制距离/连通检查，
    // 不得把远处无水面的同一连续链也当作水系面跳绘区。
    bool waterAreaCoversJumpZone = false;
    // 水面进出交点仅用于把关联线分段，不替换原始跳绘区 P1/P2。每个
    // 子段保留其父跳绘区端点，输出时按点去重；交点则作为独立辅助点输出。
    bool hasOriginalJumpEndpoints = false;
    OGRPoint originalJumpStartPoint;
    OGRPoint originalJumpEndPoint;
    double originalJumpZoneLength = 0.0;
    std::vector<OGRPoint> nearbyWaterTransitionPoints;
    JumpOgrGeometryPtr generalizedAreaGeometry;
    // 匹配后的综合线对应路径：线状要素时由原始跳绘区映射到完整综合平滑线，
    // 用于候选搜索、绘制距离判定和 P1/P2；水系面保存实际面内关联段。
    JumpOgrGeometryPtr generalizedLineGeometry;

    BoundaryLineRelation() = default;
    BoundaryLineRelation(BoundaryLineRelation&&) noexcept = default;
    BoundaryLineRelation& operator=(BoundaryLineRelation&&) noexcept = default;

    BoundaryLineRelation(const BoundaryLineRelation&) = delete;
    BoundaryLineRelation& operator=(const BoundaryLineRelation&) = delete;
};

// 跳绘区端点为辅助排查成果，不属于错误要素。每个有效原始重叠段输出 P1、P2 两点。
struct DetectedJumpEndpoint
{
    QString endpointName;          // P1 / P2
    QString endpointSource;        // 原始理论端点
    QString lineLayerName;         // 原始线状要素图层
    GIntBig lineFid = -1;          // 原始线状要素 FID
    double overlapLength = 0.0;    // 对应有效重叠段长度
    OGRPoint point;                // 行政界线有效重叠段端点
};

// 供质检任务管理器工作线程使用的无界面检查结果。
struct BoundaryJumpCheckResult
{
    int problemCount = 0;
    QString message;
    QJsonArray problemDetails;
    QStringList outputPaths;
};

class se_boundary_jump_draw_check_algorithm : public QDialog
{
    Q_OBJECT

public:
    explicit se_boundary_jump_draw_check_algorithm(
        const QString& boundaryShpPath = QString(),
        const QString& otherShpPath = QString(),
        QWidget* parent = nullptr,
        Qt::WindowFlags fl = Qt::WindowFlags());
    ~se_boundary_jump_draw_check_algorithm();

    // 无界面入口：可由 map_check_task_manager 的工作线程直接调用。
    // boundaryShpPath 为成果境界线，adminShpPath 为配置的 GDB 行政界线，
    // otherShpPaths 为关联图层对应的 GDB 线要素，generalizedShpPaths 为关联图层
    // 对应的解析后图面要素（可同时包含线和面）。
    static bool RunBoundaryJumpDrawCheck(
        const QString& boundaryShpPath,
        const QString& adminShpPath,
        const QString& otherShpPaths,
        const QString& generalizedShpPaths,
        const QString& xmlPath,
        int scale,
        const QString& outputBaseShpPath,
        BoundaryJumpCheckResult& result,
        const std::function<void(const QString&)>& log = {});

private slots:
    // 加载按钮名称与现有 UI 保持一致。
    void on_pushButton_jj_clicked();
    void on_pushButton_xzq_clicked();
    void on_pushButton_hldl_clicked();
    void on_pushButton_hldlshp_clicked();
    void on_pushButton_xml_clicked();
    // 如果 UI 中按钮名不同，可在构造函数中手动 connect 到这两个槽。
    void on_pushButton_start_clicked();
    void on_pushButton_shuchu_clicked();

private:
    Ui::se_boundary_jump_draw_check_algorithm* ui;

    QString m_boundaryPath;
    QString m_adminPath;
    QString m_otherPath;
    QString m_generalizedLinePath;
    QString m_xmlPath;

    // 记录上一轮检查输入，输出结果前判断是否需要重新检查。
    QString m_lastCheckedBoundaryPath;
    QString m_lastCheckedAdminPath;
    QString m_lastCheckedOtherPath;
    QString m_lastCheckedGeneralizedLinePath;
    QString m_lastCheckedXmlPath;
    bool m_lastCheckFinished = false;
    int m_lastProblemCount = 0;
    std::vector<DetectedJumpEndpoint> m_detectedJumpEndpoints;

private:
    void appendLog(const QString& message);
    void resetLogAfterReload(const QString& reloadMessage);

    // 主流程：writeOutput=false 只统计问题；writeOutput=true 时写出错误图层。
    bool runCheck(
        const QString& outputBaseShpPath,
        bool writeOutput,
        int& problemCount,
        QString& message);

    static bool runCore(
        const QString& boundaryShpPath,
        const QString& adminShpPath,
        const QString& otherShpPaths,
        const QString& generalizedShpPaths,
        const QString& xmlPath,
        int scale,
        const QString& outputBaseShpPath,
        bool writeOutput,
        BoundaryJumpCheckResult& result,
        const std::function<void(const QString&)>& log);

    static bool readRuleFromXml(
        const QString& xmlPath,
        const QString& boundaryShpPath,
        int scale,
        BoundaryJumpRule& rule,
        QString& error);

    // 收集境界线问题：先提取行政界线—线状要素有效关联关系，
    // 再以原始关联段搜索成果境界线，并用匹配后的综合线检查绘制距离。
    static bool collectJumpIssues(
        const QString& boundaryShpPath,
        const QString& adminShpPath,
        const QString& otherShpPaths,
        const QString& generalizedShpPaths,
        const BoundaryJumpRule& rule,
        std::vector<BoundaryJumpIssue>& issues,
        std::vector<DetectedJumpEndpoint>& detectedEndpoints,
        QString& error,
        const std::function<void(const QString&)>& log);

    static bool writeProblemShapefiles(
        const QString& outputBaseShpPath,
        const std::vector<BoundaryJumpIssue>& issues,
        const BoundaryJumpRule& rule,
        const QString& boundaryShpPath,
        QStringList& outputPaths,
        QString& error,
        const std::function<void(const QString&)>& log);

};
