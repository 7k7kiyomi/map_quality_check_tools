#ifndef SE_MAP_STYLE_CHECK_ALGORITHM_H
#define SE_MAP_STYLE_CHECK_ALGORITHM_H

// 错误码定义
typedef int SE_Error;
#define SE_ERROR_NONE 0
#define SE_ERROR_FAILURE 1
#define SE_ERROR_FILE_NOT_FOUND 2
#define SE_ERROR_LAYER_INVALID 3
#define SE_ERROR_FIELD_NOT_FOUND 4
#define SE_ERROR_XML_PARSE_FAILED 5
#define SE_ERROR_STYLE_MISMATCH 6

/**
 * @brief 图名样式规则结构
 */
struct MapStyleRules {
    char szFont[256];           // 字体要求
    double dFontSizeMM;         // 字号要求(mm)
    double dStrokeWidthMM;      // 描边宽度(mm)
    char szColor[64];           // 颜色要求(名称)
    char szColorCMYK[64];       // 颜色CMYK值
    bool bHasShadow;            // 是否需要立体阴影
    
    MapStyleRules() : 
        dFontSizeMM(0.0), 
        dStrokeWidthMM(0.0), 
        bHasShadow(false) 
    {
        szFont[0] = '\0';
        szColor[0] = '\0';
        szColorCMYK[0] = '\0';
    }
};

/**
 * @brief XML解析函数：获取图名样式检查的配置参数
 * @param szSchemaXmlPath XML规则文件路径
 * @param pRules 输出的样式规则结构
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForMapStyle(const char* szSchemaXmlPath, MapStyleRules* pRules);

/**
 * @brief 图名样式检查主函数（输出JSON）
 * @param szMapNameShpPath 图名图层文件路径（SHP文件路径）"D:\Desktop\项目\甘肃地图成果质检系统\数据部分\永登县_shp\永登县_shp\图名.shp"
 * @param szSchemaXmlPath XML规则文件路径"D:\Desktop\xml测试\图名内容准确性.xml"
 * @param szOutputJsonPath 输出JSON文件路径
 * @param bPassed 输出参数：是否通过检查
 * @return SE_Error 错误码
 */
SE_Error MapStyleCheck(
    const char* szMapNameShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    bool& bPassed);

#endif // SE_MAP_STYLE_CHECK_ALGORITHM_H
