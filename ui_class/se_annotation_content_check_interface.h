#ifndef SE_ANNOTATION_CONTENT_CHECK_INTERFACE_H
#define SE_ANNOTATION_CONTENT_CHECK_INTERFACE_H

#include "../commontype/se_error_define.h"

// -------------------------------------------------------
// 注记内容合规性检查（ID=213）对外接口
// 依赖：210 符号注记匹配引擎（se_annotation_advanced_algorithm.h/.cpp）
//       需要链接 QGIS Core/Gui/Analysis 库、spdlog 日志库
// -------------------------------------------------------

/**
 * @brief 对单组注记类型执行内容合规性检查
 *
 * 流程：
 *   1. 加载并标准化 200 匹配结果 SHP
 *   2. 调用 210 引擎 checkSymbolAnnotationMatch 做基础检查
 *   3. 从结果中筛选 ContentError（内容错误）和注记缺失符号错误
 *   4. 过滤孤立通用类别词
 *   5. 写入 JSON（groups 格式）+ SHP
 *
 * @param sz200MatchShp    200 匹配结果 SHP 路径（含 match_stat / symbol_id 等字段）
 * @param szActualShp      实际符号图层路径（用于符号存在性校验）
 * @param szGdbLayer       GDB图层路径（用于源名称比对，可为空 ""）
 * @param szAnnotLayer     注记多边形图层路径（用于孤儿注记检测，可为空 ""）
 * @param szSchemaXmlPath  规则文件路径（可为空 ""，使用内置默认规则）
 * @param scale            地图比例尺分母（只传给 210 引擎，213 自身不用）
 * @param szOutputShpPath  输出 SHP 路径（213 错误要素）
 * @param szOutputJsonPath 输出 JSON 路径（213 结果，groups 格式）
 * @param szOutputLogPath  输出日志路径（可为空 ""）
 *
 * @return SE_Error 错误码：0=成功，非0=失败
 */
SE_Error AnnotationContentCheck(
    const char *sz200MatchShp,
    const char *szActualShp,
    const char *szGdbLayer,
    const char *szAnnotLayer,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputShpPath,
    const char *szOutputJsonPath,
    const char *szOutputLogPath);

#endif // SE_ANNOTATION_CONTENT_CHECK_INTERFACE_H
