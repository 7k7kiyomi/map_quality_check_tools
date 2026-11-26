// 无UI接口版：光滑度阈值控制
#pragma once

#include <string>
#include "../commontype/se_error_define.h"

// 导出/导入宏（与其他算法保持一致）
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

/**
 * 接口：光滑度阈值控制（无界面）
 * 函数名：SmoothnessThresholdCheck
 * 输入：
 *  - szSchemaXmlPath        质检方案XML（全路径）
 *  - szParsedDataPath       待检查数据（Shapefile，全路径）
 *  - scale                  比例尺（如100000表示1:100000）
 * 输出：
 *  - szOutputErrorPointsLayerPath  不合格点输出（Shapefile）
 *  - szOutputJsonPath       JSON报告路径
 *  - szOutputLogFilePath    日志文件路径（spdlog）
 * 返回：
 *  - SE_Error（SE_ERROR_NONE / SE_ERROR_FAILURE）
 * 说明：
 *  - 从XML按输入比例尺读取“光滑度阈值控制”的阈值（图上毫米），
 *    按比例尺换算为实地米，并转换为图层单位；对线/面外环顶点序列计算三点圆半径，
 *    半径小于阈值判为错误点，输出错误点图层与JSON报告。
 */
#ifdef __cplusplus
extern "C"
{
#endif

    MY_PLUGIN_API SE_Error SmoothnessThresholdCheck(
        const char *szSchemaXmlPath,
        const char *szParsedDataPath,
        int scale,
        const char *szOutputErrorPointsLayerPath,
        const char *szOutputJsonPath,
        const char *szOutputLogFilePath);

#ifdef __cplusplus
}
#endif
