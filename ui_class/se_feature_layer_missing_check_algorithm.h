#ifndef SE_FEATURE_LAYER_MISSING_CHECK_ALGORITHM_H
#define SE_FEATURE_LAYER_MISSING_CHECK_ALGORITHM_H
#include "se_error_define.h"

// 定义一个宏来控制导出和导入
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

    /**
    * @brief XML解析函数：获取要素层缺失检查的缓冲区半径
    * @param szSchemaXmlPath XML规则文件路径
    * @param dBufferRadius 输出的缓冲区半径值（毫米）
    * @param nScale 地图比例尺（默认100000）
    * @return SE_Error 错误码
    */
    SE_Error ParseXmlForLayerMissingBufferRadius(const char* szSchemaXmlPath, double& dBufferRadius, int nScale = 100000);

    /**
    * @brief 要素层缺失检查主函数（输出JSON）
    * @param szSourceShpFilePath 库体数据文件路径（SHP数据源路径）
    * @param szParsedDataPath 解析后SHP数据路径（支持分号分隔多个文件）
    * @param szSchemaXmlPath XML规则文件路径
    * @param szOutputJsonPath 输出JSON文件路径
    * @param nScale 地图比例尺（输入接口，默认100000）
    * @return SE_Error 错误码，成功时返回SE_ERROR_NONE
    */
    MY_PLUGIN_API SE_Error FeatureLayerMissingCheck(
        const char* szSourceShpFilePath,
        const char* szParsedDataPath,
        const char* szSchemaXmlPath,
        const char* szOutputJsonPath,
        int nScale = 100000);

#ifdef __cplusplus
}
#endif

#endif