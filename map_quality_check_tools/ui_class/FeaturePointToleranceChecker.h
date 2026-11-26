#pragma once

#include <string>
#include <memory>
#include "../commontype/se_error_define.h"
#include "spdlog/spdlog.h"
#include <QPointF>
#include <QString>
#include <QStringList>

// 导出/导入宏（与其他算法保持一致）
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

/**
 * @brief 特征点限差控制检查参数（从XML解析）
 * - dMapThresholdMM: 图上阈值（毫米）
 * - strLayerName: 图层名称（取自XML文件名，不含扩展名）
 * - bValid: 是否解析到有效阈值
 */
struct FeaturePointToleranceParams
{
    // 图上阈值（毫米）— 距离容差 T
    double dMapThresholdMM = 0.0;
    // 角度阈值（度）— 判定特征点的大弯曲显著性
    double angleThresholdDeg = 0.0;
    // 最小有效边长（毫米）— 排除微小弯曲
    double minEdgeLenMM = 0.0;
    // 特征角容差（度）— 形状保留验证时的角度差容忍
    double featureAngleToleranceDeg = 0.0;
    // XML文件名（不含扩展名）作为图层名
    std::string strLayerName;
    // 是否解析到有效参数
    bool bValid = false;
};

/**
 * @brief 解析XML

 */
FeaturePointToleranceParams ParseXmlForFeaturePointToleranceParams(
    const std::string& xmlFilePath,
    int scale,
    std::shared_ptr<spdlog::logger> logger);

// 单个特征点结构（用于输出与判定）
struct FeaturePointCandidate
{
    QPointF pos;
    double sourceAngleDeg; // 原始库体角度 c
    int order;             // 顺序索引（沿库体遍历顺序）
};

// 预处理解析后数据：端点捕捉与基于几何连通的融合
void* PreprocessParsedData(const QStringList& parsedPaths, double snapToleranceUnits, std::shared_ptr<spdlog::logger> logger);

// 提取库体特征点：按角度阈值与最小有效边长筛选
std::vector<FeaturePointCandidate> ExtractSourceFeaturePoints(const QString& sourcePath,
                                                             double angleThresholdDeg,
                                                             double minEdgeLenUnits,
                                                             std::shared_ptr<spdlog::logger> logger);

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * @brief 特征点限差控制（接口版，无界面）
     *
     * 输入：
     * - szSourceShpFilePath   库体数据（Shapefile，全路径）
     * - szParsedDataPath      解析/缩编数据（Shapefile，全路径）
     * - szSchemaXmlPath       质检方案XML（全路径）
     * - scale                 比例尺（如100000表示1:100000）
     * 输出：
     * - szOutputErrorPointsLayerPath  不合格点输出（Shapefile）
     * - szOutputJsonPath      JSON报告路径
     * - szOutputLogFilePath   日志文件路径（spdlog）
     *
     * 算法概要：
     * 1. 解析XML获取图上阈值（毫米）与图层名称（XML文件名）
     * 2. 将图上毫米按输入比例尺换算为实地米，再转换为图层单位
     * 3. 从库体/解析数据中提取所有顶点（支持点/线/面/多*）
     * 4. 逐库体顶点计算到解析数据最近点的距离，超阈值判为不合格
     * 5. 导出错误点图层（若存在）、输出JSON报告
     */
    MY_PLUGIN_API SE_Error FeaturePointToleranceCheck(
        const char* szSourceShpFilePath,
        const char* szParsedDataPath,
        const char* szSchemaXmlPath,
        int scale,
        const char* szOutputErrorPointsLayerPath,
        const char* szOutputJsonPath,
        const char* szOutputLogFilePath);

#ifdef __cplusplus
}
#endif
