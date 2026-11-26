#pragma once
#include <QString>
#include <QVector>
#include <QPointF>
#include <QJsonObject>
#include <QMutex>

// ================================================================
// @brief 特征点限差控制算法接口（无界面调用版）
// 输入：规则 XML、库体 SHP、缩编 SHP
// 输出：JSON 报告路径 / 错误点 SHP 文件路径
// ================================================================
class IFeaturePointToleranceChecker
{
public:
    // 检查结果结构
    struct CheckResult {
        QString featureId;
        double x;
        double y;
        double distance;   // 实际偏移距离（米）
        QString status;    // "合格" 或 "不合格"
    };

    virtual ~IFeaturePointToleranceChecker() = default;

    // ------------------------------------------------------------
    // @brief 执行检查主流程
    // @param xmlRulePath   XML 规则文件路径"D:\Desktop\xmll\河流.xml"
    // @param baseShpPath   库体 Shapefile（原始数据）D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_gdb\永登县_gdb\LFCL.shp
    // @param genShpPath    缩编/解析 Shapefile（检查对象）D:\Desktop\项目\甘肃地图成果质检系统\数据部分\最新_永登县新版数据\永登县shp新版\铁路隧道.shp
    // @param outputJson    输出 JSON 报告路径
    // @return true 表示成功执行
    // ------------------------------------------------------------
    virtual bool runCheck(const QString& xmlRulePath,
        const QString& baseShpPath,
        const QString& genShpPath,
        const QString& outputJson) = 0;

    // ------------------------------------------------------------
    // @brief 导出错误点 SHP（仅导出不合格点）
    // @param outputShpPath 输出 Shapefile 路径
    // @return true 表示成功
    // ------------------------------------------------------------
    virtual bool exportErrorShapefile(const QString& outputShpPath) = 0;

    // ------------------------------------------------------------
    // @brief 获取检查结果（内存结构）
    // ------------------------------------------------------------
    virtual QVector<CheckResult> getResults() const = 0;

    // ------------------------------------------------------------
    // @brief 获取日志信息（按时间顺序）
    // ------------------------------------------------------------
    virtual QStringList getLog() const = 0;
};

// ================================================================
// @brief 创建特征点限差检查器实例
// @return 检查器实例指针（调用者负责释放）
// ================================================================
IFeaturePointToleranceChecker* createFeaturePointToleranceChecker();
