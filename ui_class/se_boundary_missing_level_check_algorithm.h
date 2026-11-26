#pragma once

#include <QJsonArray>
#include <QString>

#include <memory>
#include <functional>

#include "ogrsf_frmts.h"

// XML 中读取出来的境界线越级/符号等级检查规则。
// 线宽属性项、颜色属性项用于定位 SHP 属性表字段；
// expectedWidthMm、expectedColor 是 XML 中配置的标准符号参数。
struct BoundarySymbolRule
{
    QString widthFieldName;
    QString colorFieldName;
    double expectedWidthMm = 0.0;
    QString widthUnit;
    QString expectedColor;
};

// GDALDataset 必须用 GDALClose 关闭，不能直接 delete。
struct GdalDatasetCloser
{
    void operator()(GDALDataset* dataset) const
    {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};

using GdalDatasetPtr = std::unique_ptr<GDALDataset, GdalDatasetCloser>;

// OGRGeometry 由 GDAL 创建，也交回 GDAL 释放，避免 Windows 下跨 DLL 释放内存的问题。
struct OgrGeometryDestroyer
{
    void operator()(OGRGeometry* geometry) const
    {
        if (geometry != nullptr)
            OGRGeometryFactory::destroyGeometry(geometry);
    }
};

using OgrGeometryPtr = std::unique_ptr<OGRGeometry, OgrGeometryDestroyer>;

// Result returned by the non-UI entry point used by the task manager.
// `problemDetails` is intentionally kept as JSON so it can be written to the
// same error-report schema as the other batch checks.
struct BoundaryMissingLevelCheckResult
{
    int problemCount = 0;
    QString message;
    QJsonArray problemDetails;
};

// 境界线越级检查的无界面接口，仅供质检任务管理器调用。
class se_boundary_missing_level_check_algorithm
{
public:
    // 不创建或访问界面对象，可由 map_check_task_manager 的工作线程调用。
    static bool RunBoundaryMissingLevelCheck(
        const QString& shpPath,
        const QString& xmlPath,
        const QString& outputShpPath,
        BoundaryMissingLevelCheckResult& result);

private:
    se_boundary_missing_level_check_algorithm() = delete;

    static bool runCore(
        const QString& shpPath,
        const QString& xmlPath,
        const QString& outputShpPath,
        bool writeOutput,
        BoundaryMissingLevelCheckResult& result,
        const std::function<void(const QString&)>& log);

    // 从 XML 中读取 ID=114 的境界线越级检查规则。
    static bool readRuleFromXml(const QString& xmlPath, BoundarySymbolRule& rule, QString& error);

    // 检查一个 SHP 图层。outputLayer 为空时只统计问题，不写结果 SHP
    static bool checkLayerSymbol(
        OGRLayer* inputLayer,
        OGRLayer* outputLayer,
        const BoundarySymbolRule& rule,
        int& problemCount,
        QString& error,
        QJsonArray& problemDetails,
        const std::function<void(const QString&)>& log);

    // 将一个不合规要素写入结果 SHP。
    static bool writeProblemFeature(
        OGRLayer* outputLayer,
        const QString& sourceLayerName,
        GIntBig sourceFid,
        OGRGeometry* geometry,
        const QString& actualWidthText,
        double actualWidthMm,
        double expectedWidthMm,
        const QString& actualColor,
        const QString& expectedColor,
        const QString& issueText);

    // 解析 SHP 属性表中的线宽值,属性表单位为 pt，这里统一换算成 mm
    static bool parseWidthPointToMm(
        const QString& widthText,
        double& widthPoint,
        double& widthMm);

    // 颜色比较前做规范化，避免大小写和空格差异造成误判。
    static QString normalizeColor(const QString& colorText);

    // Qt 路径转 GDAL 可用的 UTF-8 路径。
    static QByteArray toGdalPath(const QString& path);
};
