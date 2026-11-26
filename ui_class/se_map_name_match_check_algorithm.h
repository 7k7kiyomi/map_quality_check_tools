#ifndef SE_MAP_NAME_MATCH_CHECK_ALGORITHM_H
#define SE_MAP_NAME_MATCH_CHECK_ALGORITHM_H

// 错误码定义
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_LAYER_INVALID 3
#define SE_ERROR_FIELD_NOT_FOUND 4
#define SE_ERROR_NO_MAP_NAME 5
#define SE_ERROR_NO_ADMIN_MATCH 6
#define SE_ERROR_XML_PARSE_FAILED 7

/**
 * @brief XML解析函数：获取图名匹配检查的配置参数
 * @param szSchemaXmlPath XML规则文件路径
 * @param szFieldName 输出的字段名（用于匹配）
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForMapNameMatch(const char* szSchemaXmlPath, char* szFieldName);

/**
 * @brief 图名与行政区划匹配检查主函数（输出JSON）
 * @param szMapNameShpPath 图名图层文件路径（SHP文件路径）"D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_shp\永登县_shp\图名.shp"
 * @param szAdminColorShpPath 行政区划面普色图层文件路径"D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_shp\永登县_shp\行政区划面普色.shp"
 * @param szBouaDataPath BOUA标准数据文件路径（SHP或GDB路径）"D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_gdb\永登县_gdb\BOUA5.shp"
 * @param szSchemaXmlPath XML规则文件路径"D:\Desktop\xml\图名与行政区划匹配检查.xml"
 * @param szOutputJsonPath 输出JSON文件路径
 * @param bMatched 输出参数：是否匹配成功
 * @return SE_Error 错误码
 */
SE_Error MapNameMatchCheck(
    const char* szMapNameShpPath,
    const char* szAdminColorShpPath,
    const char* szBouaDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bMatched);

#endif // SE_MAP_NAME_MATCH_CHECK_ALGORITHM_H