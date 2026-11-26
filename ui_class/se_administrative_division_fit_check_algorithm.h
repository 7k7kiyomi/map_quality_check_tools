#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

#include "ogrsf_frmts.h"

/// XML 中 ID=115 的“行政区划面套合检查”规则
// “容差阈值”是图上距离，需要按比例尺换算成实地米值后用于判错。
// “候选搜索区半径”也是图上距离，需要按比例尺换算成实地米值后用于有效线段筛选和附近边界搜索。
struct AdministrativeFitRule
{
    double mapTolerance = 0.0;       // XML 中的图上容差，例如 0.1
    QString unit;                    // XML 中的图上单位，例如 mm
    double scaleDenominator = 0.0;   // 比例尺分母，例如 1:145000 记录为 145000
    double tolerance = 0.0;          // 换算后的图层坐标容差；当前投影单位为米
    double candidateSearchMapTolerance = 0.0; // XML 中的候选搜索区图上半径，例如 1.4
    QString candidateSearchUnit;              // XML 中的候选搜索区图上单位，例如 mm
    double candidateSearchTolerance = 0.0;    // 换算后的候选搜索区半径；当前投影单位为米
};

// GDALDataset 必须用 GDALClose 关闭，不能直接 delete。
struct FitGdalDatasetCloser
{
    void operator()(GDALDataset* dataset) const
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};

using FitGdalDatasetPtr = std::unique_ptr<GDALDataset, FitGdalDatasetCloser>;

// OGRGeometry 由 GDAL 创建，也交回 GDAL 释放，避免 Windows 下跨 DLL 释放内存的问题。
struct FitOgrGeometryDestroyer
{
    void operator()(OGRGeometry* geometry) const
    {
        if (geometry != nullptr)
            OGRGeometryFactory::destroyGeometry(geometry);
    }
};

using FitOgrGeometryPtr = std::unique_ptr<OGRGeometry, FitOgrGeometryDestroyer>;

// 单条线面不套合问题。geometry 是最终输出到问题 SHP 的定位几何。
struct FitTopologyIssue
{
    QString type;              // 问题类型：LINE_OUT_TOL
    QString sourceLayerName;   // 来源图层名
    GIntBig sourceFid = -1;     // 来源要素 FID；整体边界问题为 -1
    double distance = 0.0;      // 超限线段到附近面边界的最大采样距离
    double area = 0.0;          // 面积字段，线问题默认保留为 0
    QString message;           // 问题说明
    FitOgrGeometryPtr geometry;    // 问题位置几何

    FitTopologyIssue() = default;
    FitTopologyIssue(FitTopologyIssue&&) noexcept = default;
    FitTopologyIssue& operator=(FitTopologyIssue&&) noexcept = default;

    FitTopologyIssue(const FitTopologyIssue&) = delete;
    FitTopologyIssue& operator=(const FitTopologyIssue&) = delete;
};

// 供质检管理器工作线程使用的无界面检查结果。
struct AdministrativeFitCheckResult
{
    int problemCount = 0;
    QString message;
    QJsonArray problemDetails;
    QStringList outputPaths;
};

// 行政区划面套合检查的无界面接口，仅供质检任务管理器调用。
class se_administrative_division_fit_check_algorithm
{
public:
    // 不创建或访问界面对象，可由 map_check_task_manager 的工作线程调用。
    // polygonShpPath 为行政区划面，lineShpPaths 为分号分隔的行政界线数据。
    static bool RunAdministrativeFitCheck(
        const QString& polygonShpPath,
        const QString& lineShpPaths,
        const QString& xmlPath,
        int scale,
        const QString& outputBaseShpPath,
        AdministrativeFitCheckResult& result,
        const std::function<void(const QString&)>& log = {});

private:
    se_administrative_division_fit_check_algorithm() = delete;

    // 从 XML 中读取 ID=115 的规则容差和候选搜索半径；两者均按图上距离和比例尺换算成实地米值。
    static bool runCore(
        const QString& polygonShpPath,
        const QString& lineShpPaths,
        const QString& xmlPath,
        int scale,
        const QString& outputBaseShpPath,
        bool writeOutput,
        AdministrativeFitCheckResult& result,
        const std::function<void(const QString&)>& log);

    static bool readRuleFromXml(
        const QString& xmlPath,
        int scale,
        AdministrativeFitRule& rule,
        QString& error);

    // 收集一个或多个行政界线 SHP 文件中距离行政区面边界超过规则容差的有效线段问题。
    static bool collectTopologyIssues(
        const QString& polygonShpPath,
        const QString& lineShpPaths,
        const AdministrativeFitRule& rule,
        std::vector<FitTopologyIssue>& issues,
        QString& error,
        const std::function<void(const QString&)>& log);

    // 逐要素提取面边界并组成边界集合，避免全图 Union 因无效几何失败。
    static FitOgrGeometryPtr loadBoundaryGeometry(
        const QString& shpPath,
        QString& error);

    // 读取面 SHP 的空间参考，用于输出结果 SHP。
    static std::unique_ptr<OGRSpatialReference> readSpatialReference(
        const QString& shpPath);

    // 添加问题几何；Multi* / GeometryCollection 会自动拆成单几何，便于写 SHP。
    static void addIssue(
        const QString& type,
        const QString& sourceLayerName,
        GIntBig sourceFid,
        double distance,
        double area,
        const QString& message,
        const OGRGeometry* geometry,
        std::vector<FitTopologyIssue>& issues);

    // 拆分超限线段，过滤搜索范围边缘截断段，并逐段计算到附近面边界的最大采样距离。
    static void addLineOutToleranceIssues(
        const QString& sourceLayerName,
        GIntBig sourceFid,
        const OGRGeometry* geometry,
        const OGRGeometry* nearbyBoundary,
        const OGRGeometry* toleranceBoundary,
        const OGRGeometry* boundarySearchOuterBoundary,
        double tolerance,
        std::vector<FitTopologyIssue>& issues);

    // 按问题几何类型输出 point / line / polygon 三类 SHP。
    static bool writeProblemShapefiles(
        const QString& polygonShpPath,
        const QString& outputBaseShpPath,
        const std::vector<FitTopologyIssue>& issues,
        double tolerance,
        QStringList& outputPaths,
        QString& error,
        const std::function<void(const QString&)>& log);

    // Qt 路径转 GDAL 可用的 UTF-8 路径。
    static QByteArray toGdalPath(const QString& path);
};


