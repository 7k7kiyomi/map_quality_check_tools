#ifndef SE_FEATURE_MISPLACEMENT_CHECK_ALGORITHM_H
#define SE_FEATURE_MISPLACEMENT_CHECK_ALGORITHM_H

#include "se_error_define.h"

// 定义一个宏来控制导出和导入（与其他算法一致）
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

/**
 * @brief 要素错绘检查算法
 * 
 * 算法描述：对库体数据的线要素和面要素进行缓冲区分析，检查解析后的GEOPDF数据是否落在相应的缓冲区内。
 * 线要素：若解析后的线要素数据落在库体线要素缓冲区中则符合要求，反之则是长度超限。
 * 面要素：若解析后的面要素数据落在库体面要素缓冲区中则符合要求，反之则是面积超限。
 * 缓冲区半径从XML文件的要素错绘质检项中获取。
 * 
 * @param szSourceShpFilePath 库体数据shp文件全路径
 * @param szParsedShpFilePath 解析后的GEOPDF数据shp文件全路径
 * @param szSchemaXmlPath 要素错绘质检方案xml文件全路径（算法解析质检项及规则）
 * @param scale 地图比例尺
 * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式），如果无问题，不输出shp数据
 * @param szOutputJsonPath JSON结果输出路径
 * @param szOutputLogFilePath 日志文件路径，使用spdlog实现日志输出
 * 
 * @return SE_Error错误编码
 */
MY_PLUGIN_API SE_Error FeatureMisplacementCheck(
    const char* szSourceShpFilePath,
    const char* szParsedShpFilePath,
    const char* szSchemaXmlPath,
    int scale,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath);

/**
 * @brief 从XML文件中解析缓冲区半径
 * 
 * @param szSchemaXmlPath XML文件路径
 * @param scale 地图比例尺
 * @param dBufferRadius 输出的缓冲区半径
 * @return SE_Error错误编码
 */
MY_PLUGIN_API SE_Error ParseXmlForBufferRadius(const char* szSchemaXmlPath, int scale, double& dBufferRadius);

#endif // SE_FEATURE_MISPLACEMENT_CHECK_ALGORITHM_H