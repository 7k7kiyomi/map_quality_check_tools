#ifndef SE_CONTENT_FRAME_MISALIGNMENT_CHECK_ALGORITHM_H
#define SE_CONTENT_FRAME_MISALIGNMENT_CHECK_ALGORITHM_H

#include "se_error_define.h"

#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 内容与图廓错位检查（ID:306）
 * 输入：
 * - szAdminAreaShpFilePath   行政区划面图层（Shapefile）
 * - szInnerFrameShpFilePath  内图廓线图层（Shapefile）
 * - szSchemaXmlPath          规则XML（最大居中错位容差，单位毫米）
 * - szOutputJsonPath         JSON输出
 * - szOutputLogFilePath      日志输出（spdlog）
 * - nScale                   比例尺（默认100000）
 */
MY_PLUGIN_API SE_Error ContentFrameMisalignmentCheck(
    const char* szAdminAreaShpFilePath,
    const char* szInnerFrameShpFilePath,
    const char* szSchemaXmlPath,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath,
    int nScale);

#ifdef __cplusplus
}
#endif

#endif
