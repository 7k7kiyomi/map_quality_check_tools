#ifndef CONTAINMENT_CHECK_INTERFACE_H
#define CONTAINMENT_CHECK_INTERFACE_H

#include "se_error_define.h"

#ifndef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 260
#endif

/**
 * @param szXmlConfigPath           XML配置文件路径（必须，包含检查规则和参数配置）
 * @param szBasePolygonPath         基础面数据路径（库体面，行政面检查必需）
 * @param szBasePointPath           基础点数据路径（库体点，行政面检查必需）
 * @param szParsedPolygonPath       解析面数据路径（行政面检查必需）
 * @param szParsedPointPath         解析点数据路径（行政面检查、河流包含检查必需）
 * @param szRiverPolygonPath        河流面数据路径（桥梁检查、河流包含检查必需）
 * @param szDitchFlowPaths          渠流向数据路径（多文件用分号分隔，渠流向检查必需）
 * @param szDitchPaths              沟渠数据路径（多文件用分号分隔，渠流向检查必需）
 * @param szBridgePaths             桥梁数据路径（多文件用分号分隔，桥梁检查必需）
 * @param szRoadPaths               道路数据路径（多文件用分号分隔，桥梁检查检查必需）
 * @param szTunnelEntrancePaths     隧道口数据路径（多文件用分号分隔，隧道检查必需）
 * @param szTunnelPaths             隧道数据路径（多文件用分号分隔，隧道检查必需）
 * @param szRoadPathsForTunnel      隧道相关道路数据路径（多文件用分号分隔，隧道检查必需）
 * @param szOutputLogPath           运行日志完整路径（必须，包含文件名）
 * @param szOutputErrorPath         错误要素SHP文件完整路径（必须）
 * @param szOutputJsonPath          输出JSON报告完整路径（必须）
 * @param scale                     比例尺参数（必须，用于匹配XML中对应的检查规则和缓冲区距离）
 */

SE_Error RelationshipCheck(
    const char *szXmlConfigPath,
    const char *szBasePolygonPath,
    const char *szBasePointPath,
    const char *szParsedPolygonPath,
    const char *szParsedPointPath,
    const char *szRiverPolygonPath,
    const char *szDitchFlowPaths,
    const char *szDitchPaths,
    const char *szBridgePaths,
    const char *szRoadPaths,
    const char *szTunnelEntrancePaths,
    const char *szTunnelPaths,
    const char *szRoadPathsForTunnel,
    const char *szOutputLogPath,
    const char *szOutputErrorPath,
    const char *szOutputJsonPath,
    int scale);

#endif