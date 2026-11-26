#ifndef SE_REQUIRED_LAYERS_CHECK_ALGORITHM_H
#define SE_REQUIRED_LAYERS_CHECK_ALGORITHM_H

// 错误码定义（复用通用错误码）
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_XML_PARSE_FAILED 7

/**
 * @brief 必需图层检查算法（输出JSON）
 * 
 * 检查指定目录下是否存在XML配置文件中要求的所有图层
 * 
 * @param szSchemaXmlPath XML规则文件路径（包含必需图层配置）
 * @param szDirectoryPath 要检查的目录路径（包含shp文件）
 * @param szOutputJsonPath 输出JSON文件路径
 * @param bPassed 输出参数：检查是否通过（所有必需图层都存在）
 * @return SE_Error 错误码
 */
SE_Error RequiredLayersCheck(
    const char* szSchemaXmlPath,
    const char* szDirectoryPath,
    const char* szOutputJsonPath,
    bool& bPassed);

#endif // SE_REQUIRED_LAYERS_CHECK_ALGORITHM_H
