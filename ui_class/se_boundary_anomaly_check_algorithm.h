#ifndef SE_BOUNDARY_ANOMALY_CHECK_ALGORITHM_H
#define SE_BOUNDARY_ANOMALY_CHECK_ALGORITHM_H

#include <vector>
#include <string>
#include <opencv2/core.hpp>
#include "se_error_define.h"

// ============================================================
// 境界线异常检测模块
// 将图片提取出的境界线与标准境界线对比，检测地图绘制异常
// 技术方案：标准境界线缓冲区分析 + 与提取境界线求交/求差
//   - 对标准境界线建立 bufferRadius 米缓冲区（多边形）
//   - 提取境界线 ∩ 缓冲区 = 正常段（落在容差范围内）
//   - 提取境界线 − 缓冲区 = 异常段（偏离标准线超过容差）
// ============================================================

// ---- 异常类型 ----
enum class BoundaryAnomalyType {
    Normal = 0,        // 正常：偏移在容差范围内
    PositionOffset,    // 位置偏移：匹配成功但偏差过大
    ShapeAnomaly,      // 形状异常：匹配成功但形状差异显著
    Missing,           // 缺失：标准境界线在提取结果中找不到对应
    Redundant          // 多余：提取境界线在标准数据中找不到对应
};

// ---- 单个异常段的检测结果 ----
struct BoundaryAnomalySegment {
    int              segmentId;           // 段编号
    BoundaryAnomalyType type;             // 异常类型
    double           avgOffset;           // 平均偏移距离（米）
    double           maxOffset;           // 最大偏移距离（米）
    double           directionDiff;       // 方向变化（度，保留兼容）
    int              matchRefIdx;         // 匹配的标准境界线索引（-1表示无匹配）
    double           matchOverlapRatio;   // 正常段长度占该线总长度的比例（0~1）
    std::vector<std::vector<cv::Point2d>> normalParts;  // 正常段（与缓冲区相交的部分，地理坐标）
    std::vector<std::vector<cv::Point2d>> anomalyParts; // 异常段（未与缓冲区相交的部分，地理坐标）
    std::string      description;         // 异常描述
};

// ---- 整体检测结果 ----
struct BoundaryAnomalyResult {
    bool success;                               // 检测是否成功执行
    std::string errorMsg;                       // 错误信息（失败时）

    int totalExtractedSegments;                 // 提取境界线段总数
    int totalStandardSegments;                  // 标准境界线段总数
    int matchedCount;                           // 匹配成功段数
    int normalCount;                            // 正常段数
    int positionOffsetCount;                    // 位置偏移段数
    int shapeAnomalyCount;                      // 形状异常段数
    int missingCount;                           // 缺失段数
    int redundantCount;                         // 多余段数

    double overallAvgOffset;                    // 整体平均偏移
    double overallMaxOffset;                    // 整体最大偏移

    std::vector<BoundaryAnomalySegment> segments; // 各段详细结果
};

// ---- 检测参数配置 ----
struct BoundaryCheckConfig {
    // 缓冲区分析参数
    double bufferRadius = 1000.0;        // 标准境界线缓冲区半径（米），超过此距离视为异常偏离
    int    bufferSegments = 16;          // 缓冲区象限分段数（QgsGeometry::buffer 第二参数）

    // 重叠比例阈值（保留兼容）
    double minOverlapRatio = 0.3;        // 最小正常段长度占比，低于此值视为完全异常

    // 保留兼容字段（当前算法未使用，避免破坏外部调用）
    double maxAvgOffsetNormal = 20.0;
    double maxOffsetNormal = 50.0;
    double maxAvgOffsetSuspicious = 50.0;
    double maxOffsetSuspicious = 100.0;
    double maxDirectionDiffDeg = 30.0;
    double sampleInterval = 50.0;
    double segmentSplitLength = 1000.0;
};

// ============================================================
// 主检测 API
// ============================================================

/**
 * @brief 境界线异常检测
 *
 * 检测流程：
 *   1. 将所有标准境界线合并，建立 bufferRadius 米缓冲区（多边形）
 *   2. 对每条提取境界线执行：
 *      - normalGeom = extractedLine ∩ combinedBuffer  （与缓冲区相交的部分）
 *      - anomalyGeom = extractedLine − combinedBuffer  （未与缓冲区相交的部分）
 *   3. 若 anomalyGeom 为空 → 正常；否则 → 异常
 *   4. normalParts / anomalyParts 输出按多段折线形式返回
 *
 * 坐标系统说明：
 * - extractedBoundaries: 地理坐标系（投影坐标，单位米），由图片提取后经仿射变换逆变换得到
 * - standardBoundaries:  地理坐标系（投影坐标，单位米），即 m_boundaryFeatures
 * - 所有距离计算均在地理坐标系下进行，单位为米
 *
 * @param extractedBoundaries  图片提取的境界线（地理坐标，CV_64F）
 * @param standardBoundaries   标准境界线（地理坐标，CV_64F）
 * @param config               检测参数配置（bufferRadius 单位米）
 * @param outResult            输出检测结果
 * @return SE_Error 错误码
 */
SE_Error CheckBoundaryAnomalies(
    const std::vector<std::vector<cv::Point2d>>& extractedBoundaries,
    const std::vector<std::vector<cv::Point2d>>& standardBoundaries,
    const BoundaryCheckConfig& config,
    BoundaryAnomalyResult& outResult);

// ============================================================
// 辅助 API（供外部调用或单元测试）
// ============================================================

/**
 * @brief 构建标准境界线的空间索引
 * @param standardBoundaries 标准境界线
 * @param gridCellSize       网格单元大小（像素）
 * @return 空间网格索引：grid[y][x] = {线段索引列表}
 */
std::vector<std::vector<std::vector<size_t>>> BuildSpatialIndex(
    const std::vector<std::vector<cv::Point2f>>& standardBoundaries,
    double gridCellSize);

/**
 * @brief 计算点到折线的最短距离
 * @param pt   查询点
 * @param poly 折线
 * @return 最短距离，以及对应的最近线段索引
 */
double PointToPolylineDist(const cv::Point2f& pt,
                           const std::vector<cv::Point2f>& poly,
                           size_t& outNearestSegIdx);

/**
 * @brief 计算提取线段与标准线段的匹配度（缓冲区重叠比例）
 * @param extracted  提取境界线段
 * @param standard   标准境界线段
 * @param bufferRadius 缓冲区半径
 * @return 重叠比例（落在缓冲区内点数 / 总采样点数）
 */
double ComputeOverlapRatio(const std::vector<cv::Point2f>& extracted,
                           const std::vector<cv::Point2f>& standard,
                           double bufferRadius);

/**
 * @brief 对折线等间隔采样
 * @param poly       原始折线
 * @param interval   采样间隔（像素）
 * @return 采样点列表
 */
std::vector<cv::Point2f> SamplePolyline(const std::vector<cv::Point2f>& poly,
                                         double interval);

/**
 * @brief 计算两段折线的方向差异
 * @param poly1 折线1
 * @param poly2 折线2
 * @return 平均方向偏差（度）
 */
double ComputeDirectionDifference(const std::vector<cv::Point2f>& poly1,
                                  const std::vector<cv::Point2f>& poly2);

/**
 * @brief 将长折线拆分为等长的小段
 * @param poly          原始折线
 * @param segmentLength 段长度（像素）
 * @return 拆分后的子线段列表
 */
std::vector<std::vector<cv::Point2f>> SplitPolyline(
    const std::vector<cv::Point2f>& poly,
    double segmentLength);

/**
 * @brief 生成异常检测报告（JSON 格式字符串）
 * @param result 检测结果
 * @return JSON 格式报告字符串
 */
std::string GenerateAnomalyReport(const BoundaryAnomalyResult& result);

#ifdef MAKING_MY_PLUGIN_DLL
#define MY_PLUGIN_API __declspec(dllexport)
#else
#define MY_PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C 接口：境界线异常检测（通过文件路径调用）
 */
MY_PLUGIN_API SE_Error BoundaryAnomalyCheck(
    const char* szExtractedShpPath,
    const char* szStandardShpPath,
    const char* szOutputErrorLayerPath,
    const char* szOutputJsonPath,
    const char* szOutputLogFilePath);

#ifdef __cplusplus
}
#endif

#endif // SE_BOUNDARY_ANOMALY_CHECK_ALGORITHM_H
