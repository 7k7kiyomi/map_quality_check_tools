// ================================================================
// @file ScaleChecker.h
// @brief 投影参数准确性检查（算法接口导出，无 UI）
// ================================================================
#ifndef SCALECHECKER_H
#define SCALECHECKER_H

#include <string>
#include <memory>
#include <limits>
#include <QString>
#include "se_error_define.h"
#include "spdlog/spdlog.h"

// 导出/导入控制，与其他算法接口保持一致
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

// 规则结构（从 XML 解析得到）
struct ProjectionRule
{
    std::string id;                    // 检查项 ID，例如 "401"
    std::string name;                  // 检查项名称，例如 "投影参数准确性"
    std::string projectionTypeCN;      // 投影类型（中文），如 "正轴等积割圆锥投影"
    double centralMeridian = std::numeric_limits<double>::quiet_NaN();
    double latitudeOfOrigin = std::numeric_limits<double>::quiet_NaN();
    double standardParallel1 = std::numeric_limits<double>::quiet_NaN();
    double standardParallel2 = std::numeric_limits<double>::quiet_NaN();
};

// 解析 XML 规则文件，按比例尺匹配并返回 ProjectionRule
ProjectionRule ParseXmlForProjectionRule(const std::string& xmlFilePath,
                                         int scale,
                                         std::shared_ptr<spdlog::logger> logger);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 投影参数准确性检查
 *
 * 输入：解析后数据（shp 文件）、XML 规则文件、比例尺
 * 输出：JSON 文件、日志报告
 *
 * @param szShpFilePath 解析后数据 shp 文件全路径
 * @param szSchemaXmlPath 质检方案 xml 文件全路径（包含投影参数规则）
 * @param scale 比例尺（用于选择 XML 中匹配的 <scale>/<value>）
 * @param szOutputJsonPath JSON 结果输出路径
 * @param szOutputLogFilePath 日志文件输出路径（spdlog）
 * @return SE_Error 错误码，成功返回 SE_ERROR_SUCCESS
 */
MY_PLUGIN_API SE_Error ProjectionParameterCheck(
    const char* szShpFilePath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath);

#ifdef __cplusplus
}
#endif

#endif // SCALECHECKER_H
