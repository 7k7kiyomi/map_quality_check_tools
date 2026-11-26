#ifndef SE_MIN_BEND_THRESHOLD_ALGORITHM_H
#define SE_MIN_BEND_THRESHOLD_ALGORITHM_H

#include <string>
#include <vector>
#include <iosfwd>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include "se_error_define.h"

// 定义一个宏来控制导出和导入
#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

// 前向声明（C++ 类型）
class OGRGeometry;
class GDALDataset;
class OGRLayer;
class OGRPoint;
class OGRSpatialReference;

// 问题点信息结构体（C++ 类型）
struct ProblemPointInfo
{
    double x;           // 点的X坐标
    double y;           // 点的Y坐标
    std::string layerName;    // 图层名称
    long long featureID;      // 要素ID
    std::string errorType;    // 错误类型
    double area;        // 计算得到的面积值
};

#ifdef __cplusplus
extern "C" {
#endif

    // 解析函数已内部化：请使用 MinBendThresholdCheck 作为唯一对外接口

    /**
     * @brief 检查单个要素的弯曲是否符合阈值要求
     * @param poGeometry 要素几何对象
     * @param dThresholdValue 最小弯曲阈值（平方米）
     * @param dSlopeAngleThreshold 斜率判断阈值（度）
     * @param layerName 图层名称
     * @param featureID 要素ID
     * @param problemPoints 输出的问题点信息列表
     * @return bool 是否存在问题（true表示存在问题）
     */
    bool CheckFeatureBends(OGRGeometry* poGeometry, double dThresholdValue, double dSlopeAngleThreshold,
                          const std::string& layerName, long long featureID,
                          std::vector<ProblemPointInfo>& problemPoints);

    /**
     * @brief 处理单个SHP文件
   
     * @param dThresholdValue 阈值（实地面积，平方米）
     * @param dSlopeAngleThreshold 斜率判断阈值（度）
     * @param poErrorDS 错误数据集
     * @param poErrorLayer 错误图层
     * @param poBendPointLayer 弯曲点子图层
     * @param bHasErrors 是否存在错误（引用）
     * @param nTotalFeatures 总要素数（引用）
     * @param nErrorFeatures 错误要素数（引用）
     * @param allProblemPoints 所有问题点集合（引用）
     * @return SE_Error 错误码
     */
    SE_Error ProcessShpFile(const char* szShpFilePath, double dThresholdValue, double dSlopeAngleThreshold,
                            GDALDataset* poErrorDS, OGRLayer* poErrorLayer, OGRLayer* poBendPointLayer,
                            bool& bHasErrors, int& nTotalFeatures, int& nErrorFeatures,
                            std::vector<ProblemPointInfo>& allProblemPoints);

    /**
     * @brief 创建问题点图层
     * @param problemPoints 问题点信息集合
     * @param szOutputPath 输出图层路径
     * @return SE_Error 错误码
     */
    SE_Error CreateProblemPointsLayer(const std::vector<ProblemPointInfo>& problemPoints, 
                                     const char* szOutputPath,
                                     OGRSpatialReference* poSRS);

    /**
     * @brief 最小弯曲阈值控制检查主函数
     * @param szParsedDataPath 解析后数据路径（支持多个SHP文件用分号分隔  ）
     * @param szSchemaXmlPath XML规则文件路径
     * @param szOutputCheckReportPath 输出JSON格式检查报告文件路径
     * @param szOutputCheckResultLayerPath 输出错误图层文件路径
     * @param szOutputLogFilePath 输出日志文件路径
     * @param nScale 地图比例尺（用于选择XML中对应的质检项和计算实际面积阈值，默认100000）
     * @note 内部解析XML获取图上阈值（mm²），并按1:nScale换算为实地面积阈值（m²）。
     * @note 算法流程：
     *       1. 基于斜率变化提取特征点（拐点）
     *       2. 计算相邻拐点之间形成的面域面积
     *       3. 将面积与阈值比较，标记过小弯曲
     *       4. 生成JSON格式检查报告，包含以下内容：
     *          - 检查结果统计（总要素数、错误要素数、错误率）
     *          - 详细错误信息（图层名称、要素ID、错误类型、问题点坐标、面积值）
     *          - 检查时间和执行状态
     * @return SE_Error 错误码
     */
    MY_PLUGIN_API SE_Error MinBendThresholdCheck(
        const char* szParsedDataPath,
        const char* szSchemaXmlPath,
        const char* szOutputCheckReportPath,
        const char* szOutputCheckResultLayerPath,
        const char* szOutputLogFilePath,
        int nScale = 100000);

    /**
     * @brief 计算三角形面积
     */
    double CalculateTriangleArea(double x1, double y1, double x2, double y2, double x3, double y3);

    /**
     * @brief 计算斜率变化角度（转折角）
     */
    double CalculateSlopeAngle(double x1, double y1, double x2, double y2, double x3, double y3);

    /**
     * @brief 提取特征点（拐点）
     */
    void ExtractBendPoints(const std::vector<OGRPoint*>& points, double angleThreshold, std::vector<int>& bendPoints, bool isClosedRing = false);

    /**
     * @brief 计算相邻拐点之间形成的面域面积
     */
    double CalculateBendArea(const std::vector<OGRPoint*>& points, int startIdx, int endIdx);

#ifdef __cplusplus
}
#endif

#endif