#ifndef SENSITIVE_CONTENT_CHECK_ALGORITHM_H
#define SENSITIVE_CONTENT_CHECK_ALGORITHM_H

// ========== 以下依赖由集成方自行修改 ==========
// 如果你的编译环境不同，请将 MY_EXPORT_API 改为你自己的导出宏（如 __declspec(dllexport)），
// 或者直接去掉 MY_EXPORT_API 关键字即可（适用于静态链接或仅编译为 obj）
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_EXPORT_API __declspec(dllexport)
#else
#define MY_EXPORT_API __declspec(dllimport)
#endif
// ============================================

// ========== 错误码定义（自包含，无需外部头文件）==========
typedef int SE_Error;

#define SE_ERROR_NONE                  0   // 成功
#define SE_ERROR_NOT_ENOUGH_DATA       1   // 没有足够的反序列化数据
#define SE_ERROR_NOT_ENOUGH_MEMORY     2   // 内存不足
#define SE_ERROR_UNSUPPORTED_GEOMETRY  3   // 几何类型不支持
#define SE_ERROR_UNSUPPORTED_OPERATION 4   // 操作不支持
#define SE_ERROR_CORRUPT_DATA          5   // 错误数据
#define SE_ERROR_FAILURE               6   // 失败
#define SE_ERROR_UNSUPPORTED_SRS       7   // 空间参考系不支持
#define SE_ERROR_INVALID_HANDLE        8   // 不支持的句柄
#define SE_ERROR_FILE_NOT_FOUND        9   // 文件不存在
#define SE_ERROR_INVALID_PARAMETER     10  // 参数无效
#define SE_ERROR_WRITE_FILE_FAILED     11  // 写文件失败
#define SE_ERROR_BUFFER_TOO_SMALL      12  // 缓冲区太小
// =====================================================

/**
 * @brief 敏感文字合规性检查 —— 检查注记中是否包含敏感词
 *
 * 检查逻辑：
 *   1. 读取注记SHP文件，建立文本索引
 *   2. 敏感字符集过滤 —— 只保留在敏感词库中出现的单字要素
 *   3. 单CJK字符：链式配对扫描（自适应半径 + 字体一致性 + 步长保护）
 *   4. 多字符文本（≥2字）：直接子串匹配敏感词
 *   5. 包含敏感词 → 报错输出；不包含 → 通过
 *
 * @param szAnnoShpPath       注记SHP文件路径
 * @param szXmlConfigPath     XML配置文件路径，内含敏感词列表
 * @param szOutputJsonPath    输出JSON报告文件路径
 * @param szOutputShpPath     输出错误要素SHP文件路径
 * @param szOutputLogPath     输出日志文件路径
 * @return SE_Error 错误编码（0=成功，非0=失败，含义见上方宏定义）
 */
SE_Error SensitiveContentCheck(
    const char* szAnnoShpPath,
    const char* szXmlConfigPath,
    const char* szOutputJsonPath,
    const char* szOutputShpPath,
    const char* szOutputLogPath);

#endif // SENSITIVE_CONTENT_CHECK_ALGORITHM_H
