#ifndef SE_MAP_ANNOTATION_CHECK_ALGORITHM_H
#define SE_MAP_ANNOTATION_CHECK_ALGORITHM_H

// 错误码定义
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_LAYER_INVALID 3
#define SE_ERROR_FIELD_NOT_FOUND 4
#define SE_ERROR_XML_PARSE_FAILED 5

#include <cstring>

/**
 * @brief 地图落款检查配置结构
 */
struct MapAnnotationRules
{
    char szFontName[256];      // 字体名称
    double dFontSizeMM;        // 字号（mm）
    char szFrameColor[64];     // 图廓颜色
    double dDistance;          // 制图落款位置-距离（mm）
    char szAlignment[64];      // 制图落款位置-对齐
    char szLayout[64];         // 制图落款位置-排布

    MapAnnotationRules()
    {
        memset(szFontName, 0, sizeof(szFontName));
        dFontSizeMM = 0.0;
        memset(szFrameColor, 0, sizeof(szFrameColor));
        dDistance = 0.0;
        memset(szAlignment, 0, sizeof(szAlignment));
        memset(szLayout, 0, sizeof(szLayout));
    }
};

/**
 * @brief XML解析函数：获取地图落款检查的配置参数
 * @param szSchemaXmlPath XML规则文件路径
 * @param pRules 输出的配置规则
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForMapAnnotation(const char* szSchemaXmlPath, MapAnnotationRules* pRules);

/**
 * @brief 地图落款检查主函数（输出JSON）
 * @param szDecorationLinePath 整饰层线图层文件路径（SHP文件路径）
 * @param szDecorationPolyPath 整饰层面图层文件路径（SHP文件路径）
 * @param szInnerFramePath 内图廓图层文件路径（SHP文件路径）
 * @param szSchemaXmlPath XML规则文件路径
 * @param szOutputJsonPath 输出JSON文件路径
 * @param bPassed 输出参数：是否通过检查
 * @return SE_Error 错误码
 */
SE_Error MapAnnotationCheck(
    const char* szDecorationLinePath,
    const char* szDecorationPolyPath,
    const char* szInnerFramePath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed);

#endif // SE_MAP_ANNOTATION_CHECK_ALGORITHM_H
