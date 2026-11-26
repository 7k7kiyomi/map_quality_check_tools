#ifndef SE_LEGEND_MATCH_CHECK_ALGORITHM_H
#define SE_LEGEND_MATCH_CHECK_ALGORITHM_H

// 错误码定义（复用通用错误码）
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_LAYER_INVALID 3
#define SE_ERROR_FIELD_NOT_FOUND 4
#define SE_ERROR_XML_PARSE_FAILED 7

/**
 * @brief 标准图例匹配检查主函数（输出JSON）
 * @param szStandardLayerDir 标准图例文件夹路径（包含多个标准图例SHP文件）
 * @param szMapLayerPath 待检查的地图图层文件路径（SHP文件）
 * @param szElementNaFilter 要检查的element_na值（如“国道”、“河流”等）
 * @param szOutputShpPath 输出错误要素Shapefile路径（可为nullptr）
 * @param szSchemaXmlPath XML规则文件路径（可选，传NULL则使用默认配置）
 * @param szOutputJsonPath 输出JSON文件路径
 * @param szOutputLogPath 输出日志文件路径（可为nullptr）
 * @param bPassed 输出参数：检查是否通过
 * @return SE_Error 错误码
 */
SE_Error LegendMatchCheck(
    const char* szStandardLayerDir,
    const char* szMapLayerPath,
    const char* szElementNaFilter,
    const char* szOutputShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szOutputLogPath,
    bool& bPassed);

#endif // SE_LEGEND_MATCH_CHECK_ALGORITHM_H
