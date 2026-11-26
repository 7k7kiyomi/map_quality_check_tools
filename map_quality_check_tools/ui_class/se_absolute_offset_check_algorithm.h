#ifndef SE_ABSOLUTE_OFFSET_CHECK_ALGORITHM_H
#define SE_ABSOLUTE_OFFSET_CHECK_ALGORITHM_H

#include <vector>
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
// 前向声明
class OGRGeometry;

/**
 * @brief XML解析函数：获取绝对偏移检查的缓冲区半径
 * @param szSchemaXmlPath XML规则文件路径
 * @param dBufferRadius 输出的缓冲区半径值（毫米）
 * @param nScale 地图比例尺（默认100000）
 * @return SE_Error 错误码
 */
SE_Error ParseXmlForAbsoluteOffsetBufferRadius(const char* szSchemaXmlPath, double& dBufferRadius, int nScale = 100000);

/**
 * @brief 绝对偏移检查算法
 * 
 * 比例尺处理说明：
 * - 缓冲区半径基于解析后地图的比例尺计算
 * - nScale为解析后地图比例尺，用于XML解析和JSON报告生成
 * 
 * 算法逻辑：
 * - 使用叠加擦除处理，计算解析后要素与缓冲区的差集
 * - 要素落在缓冲区内部的部分视为正确
 * - 要素落在缓冲区外部的部分视为错误，作为结果输出
 * - 只有超出缓冲区的几何体部分会被保存到错误图层中
 * 
 * @param szSourceShpFilePath 库体数据shp文件全路径（固定1:10000比例尺）
 * @param szParsedDataPath 解析后数据路径（支持单个shp文件或分号分隔的多个shp文件）
 * @param szSchemaXmlPath 绝对偏移质检方案xml文件全路径（算法解析质检项及规则）
 * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式），如果无问题，不输出shp数据
 * @param szOutputJsonPath JSON格式检查结果文件路径
 * @param szOutputLogFilePath 日志文件路径，使用spdlog实现日志输出
 * @param nScale 解析后地图的比例尺（默认100000），用于XML解析和JSON报告生成
 * 
 * @return SE_Error错误编码
 */
MY_PLUGIN_API SE_Error AbsoluteOffsetCheck(
    const char* szSourceShpFilePath,
    const char* szParsedDataPath,
    const char* szSchemaXmlPath,
    const char* szOutputCheckResultLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath,
    int nScale = 100000);

/**
 * @brief 将图上毫米单位的缓冲区半径转换为实地米
 * @param dBufferRadiusMM 缓冲区半径（毫米）
 * @param nParsedMapScale 解析后地图的比例尺
 * @return 实地米单位的缓冲区半径
*/
double ConvertMMToGeoUnits(double dBufferRadiusMM, int nParsedMapScale);

/**
 * @brief 检查几何体是否完全在缓冲区内
 * @param pGeometry 待检查的几何体
 * @param pBufferGeometry 缓冲区几何体
 * @return true表示完全在缓冲区内，false表示超出缓冲区
 */
bool IsGeometryWithinBuffer(void* pGeometry, void* pBufferGeometry);

/**
 * @brief 计算几何体超出缓冲区的部分（差集运算）
 * @param pGeometry 待检查的几何体
 * @param buffers 缓冲区几何体列表
 * @return 超出缓冲区的几何体部分，如果完全在缓冲区内则返回nullptr，调用者负责释放返回的几何体内存
 * @deprecated 将被新的整体缓冲区差集运算函数替代
 */
// 已弃用，使用ComputeLayerDifference替代
// void* GetGeometryOutsideBuffers(void* pGeometry, const std::vector<OGRGeometry*>& buffers);

/**
 * @brief 合并缓冲区几何体为一个整体
 * @param buffers 缓冲区几何体列表
 * @return 合并后的缓冲区几何体，调用者负责释放返回的几何体内存
 */
void* MergeBuffers(const std::vector<OGRGeometry*>& buffers);

/**
 * @brief 计算解析后图层与整体缓冲区的差集
 * @param pParsedLayer 解析后图层
 * @param pBufferGeometry 整体缓冲区几何体
 * @param outErrorCount 输出参数，返回超出缓冲区的要素数量
 * @param outTotalCount 输出参数，返回总要素数量
 * @return 差集结果图层，如果完全在缓冲区内则返回nullptr，调用者负责释放返回的图层内存
 */
void* ComputeLayerDifference(void* pParsedLayer, void* pBufferGeometry, int& outErrorCount, int& outTotalCount);

#ifdef __cplusplus
}
#endif

#endif // SE_ABSOLUTE_OFFSET_CHECK_ALGORITHM_H
