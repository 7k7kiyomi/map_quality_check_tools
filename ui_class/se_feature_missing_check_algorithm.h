#pragma once

#include <string>
#include <memory>
#include "se_error_define.h"
#include "spdlog/spdlog.h"

using namespace std;
// 定义一个宏来控制导出和导入
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

/**
 * @brief 要素缺失检查参数
 */
struct FeatureMissingCheckParams
{
    double dBufferRadius;            // 缓冲区半径（毫米）- 必须参数
    double dMinArea;                 // 最小面积阈值（平方毫米）- 可选参数
    double dMinLength;               // 最小长度阈值（毫米）- 可选参数
    double dMinWidth;                // 最小宽度阈值（毫米）- 可选参数，用于面要素过滤
    std::string strAttributeField;   // 属性项名称 - 可选参数
    std::string strAttributeValue;   // 属性值 - 可选参数
    std::string strLayerName;        // 图层名称（从XML中获取）
    bool bHasMinArea;                // 是否有最小面积过滤
    bool bHasMinLength;              // 是否有最小长度过滤
    bool bHasMinWidth;               // 是否有最小宽度过滤
    bool bHasAttributeFilter;        // 是否有属性过滤
};

// 解析XML配置文件，获取指定比例尺下的要素缺失检查参数（C++ 接口）
FeatureMissingCheckParams ParseXmlForFeatureMissingParams(const std::string& xmlFilePath,
                                                          int scale,
                                                          std::shared_ptr<spdlog::logger> logger);

#ifdef __cplusplus
extern "C" {
#endif

    /**
    * @brief 要素缺失检查（一对多关系版本）
    * 
    * 该函数用于检查解析后的数据中是否存在要素缺失的情况。
    * 支持库体数据要素层和解析后要素层之间的一对多关系。
    * 
    * 算法步骤：
    * 1. 预处理原始数据：过滤小面积要素，构建缓冲区
    * 2. 处理解析后数据：读取所有图层，进行几何清理
    * 3. 跨图层空间查询：检查每个原始要素是否在解析后数据中存在
    * 4. 判断缺失情况：只要有要素缺失就判定为要素缺失
    * 5. 生成JSON结果和错误图层
    * 
    * @param szSourceShpFilePath 源数据shp文件全路径（单个库体数据）
    * @param szParsedDataPath 解析后数据shp文件全路径（单个或多个shp文件，用分号分隔）
    * @param szSchemaXmlPath 质检方案xml文件全路径（算法解析质检项及规则）
    * @param scale 比例尺
    * @param szOutputCheckResultLayerPath 存在问题的要素保存为质检问题数据（shp格式），如果无问题，不输出shp数据
    * @param szOutputJsonPath JSON格式检查结果文件路径
    * @param szOutputLogFilePath 日志文件路径，使用spdlog实现日志输出
    * 
    * @return SE_Error错误编码
    */
    MY_PLUGIN_API SE_Error FeatureMissingCheck(
        const char* szSourceShpFilePath,
        const char* szParsedDataPath,
        const char* szSchemaXmlPath,
        int scale,
        const char* szOutputCheckResultLayerPath,
        const char* szOutputJsonPath,
        const char* szOutputLogFilePath);

#ifdef __cplusplus
}
#endif
