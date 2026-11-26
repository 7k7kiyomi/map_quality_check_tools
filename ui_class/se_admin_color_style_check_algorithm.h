#ifndef SE_ADMIN_COLOR_STYLE_CHECK_ALGORITHM_H
#define SE_ADMIN_COLOR_STYLE_CHECK_ALGORITHM_H

// 错误码定义
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_LAYER_INVALID 3
#define SE_ERROR_FIELD_NOT_FOUND 4
#define SE_ERROR_XML_PARSE_FAILED 5
#define SE_ERROR_COLOR_COUNT_EXCEEDED 6
#define SE_ERROR_ADJACENT_SAME_COLOR 7
#define SE_ERROR_COLOR_DISTRIBUTION_UNBALANCED 8
#define SE_ERROR_LARGE_AREA_NOT_LIGHT 9
#include <QString>

/**
 * @brief XML解析函数：获取行政区划设色规范检查的配置参数
 * @param szSchemaXmlPath XML规则文件路径
 * @param nMaxColorCount 输出的最大颜色数量限制
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForAdminColorStyle(
    const char* szSchemaXmlPath,
    int& nMaxColorCount);

/**
 * @brief 行政区划设色规范检查主函数（输出JSON）
 * @param szShpPath 行政区划图层文件路径
 * @param szSchemaXmlPath XML规则文件路径
 * @param szOutputJsonPath 输出JSON文件路径
 * @param bPassed 输出参数：是否通过检查
 * @param szGdbShpPath GDB参考路径（可选，用于名称反查）
 * @param pLogText 日志文本输出（可选）
 * @param szGdbLayerName GDB图层名称（来自CSV第2列，用于反查时指定图层）
 * @return SE_Error 错误码
 */
SE_Error AdminColorStyleCheck(
    const char* szShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed,
    const char* szGdbShpPath = nullptr,
    QString* pLogText = nullptr,
    const char* szGdbLayerName = nullptr);

#endif // SE_ADMIN_COLOR_STYLE_CHECK_ALGORITHM_H