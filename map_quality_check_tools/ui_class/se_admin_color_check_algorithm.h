#ifndef SE_ADMIN_COLOR_CHECK_ALGORITHM_H
#define SE_ADMIN_COLOR_CHECK_ALGORITHM_H

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

/**
 * @brief XML解析函数：获取行政区划颜色检查的配置参数
 * @param szSchemaXmlPath XML规则文件路径
 * @param nMaxColorCount 输出的最大颜色数量限制
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForAdminColor(const char* szSchemaXmlPath, int& nMaxColorCount);

/**
 * @brief 行政区划颜色检查主函数（输出JSON）
 * @param szShpPath 行政区划图层文件路径（SHP文件路径）"D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_shp\永登县_shp\行政区划面普色.shp"
 * @param szSchemaXmlPath XML规则文件路径 "D:\Desktop\行政区划面颜色检查.xml"
 * @param szOutputJsonPath 输出JSON文件路径 
 * @param bPassed 输出参数：是否通过检查
 * @return SE_Error 错误码
 */
SE_Error AdminColorCheck(
    const char* szShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed);

#endif // SE_ADMIN_COLOR_CHECK_ALGORITHM_H
