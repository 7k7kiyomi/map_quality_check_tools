#pragma once
#ifndef MAP_CHECK_ALGORITHMS_H
#define MAP_CHECK_ALGORITHMS_H

#include <functional>
#include <QString>
#include <QStringList>
#include "algorithm_file_paths.h"
#include "map_check_common.h"
class map_check_task_manager;  // 前向声明，避免循环包含

// 算法函数接口定义
using AlgorithmFunc = std::function<void(
    const QString& checkItemName,        // 质检项名称
    const AlgorithmFilePaths& filePaths, // 输入文件路径
    int scale,                // 比例尺（从JSON提取）
    const QString& outputDir,           // 输出目录（与JSON同目录）
    CheckItemResult& result             // 结果输出
    )>;

// 所有算法声明
//比例尺数值检查    
void algorithm_ScaleValueCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//101 要素层缺失检查    
void FeatureLayerMissingCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//102 要素缺失检查   
void FeatureMissingCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//103 要素错绘   
void FeatureMisplacementCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//104 绝对偏移检查   
void AbsoluteOffsetCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//105 几何走向异常检查     
void checkWaterSystemDirection(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& result);
//106 最小弯曲阈值控制   
void MinBendThresholdCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//107 特征点限差控制   
void FeaturePointToleranceChecker(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//108 光滑度阈值控制   
void SmoothnessChecker(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//109  直角化角度阈值控制   
void checkRightAngleThreshold(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& result);
//110  削尖角度阈值控制   
void AngleCheckInterface(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& result);
//111  面要素重叠 / 间隙检查    
void checkPolygonOverlapGap(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& resultt);
//112  线要素拓扑检查    
void checkLineTopology(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& result);
//113  关系检查   
void ContainmentCheckInterface(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//501 图幅接边与系列化表达（含跨比例尺表达支撑检查）
void SeriesMapTemplateCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//114  境界线越级检查
void BoundaryMissingLevelCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//115  行政区划面套合检查
void AdministrativeDivisionFitCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

//116  境界线缺失和跳绘检查
void BoundaryJumpDrawCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

//117  飞地漏绘检查
void EnclaveMissingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// 118: Point-symbol isolation; normalize reservoir symbols before testing connections.
void PointIsolationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// 119: Feature selection redundancy (tunnels, stations, rivers and bridges).
void FeatureRedundancyCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// ========== 注记检查算法 ==========
//要素-注记匹配（ID=200）  
void AnnotationMatchFeaturesWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//注记字体检查（ID=201）  
void AnnotationFontCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//注记字号检查（ID=202）  
void AnnotationFontSizeCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//注记颜色检查（ID=203）   
void AnnotationColorCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//字距检查（ID=204）  
void CharSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//注记斜体检查（ID=205）
void AnnotationItalicCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//重叠检查（ID=206）   
void AnnotationOverlapCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//线要素注记组距检查（ID=207）   
void GroupSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//面要素等间距标注检查（ID=208）  
void EqualSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//符号方向性检查（ID=209）  
void SymbolDirectionCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//符号注记匹配检查（ID=210）  
void SymbolAnnotationMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//政区名称注记错误/缺失检查（ID=211）
void AdminNameAnnotationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//敏感文字合规性检查（ID=212）
void SensitiveContentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//注记内容合规性检查（ID=213）
void AnnotationContentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
// 境界线色带渲染合规性检查（ID=214）
void BoundaryBandRenderCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// ========== 图面整饰检查算法 ==========
//图名内容准确性检查（ID=304）
void MapStyleCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//制图说明检查检查（ID=305）
void MapAnnotationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//行政区划颜色检查（ID=306）
void AdminColorCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//要素超出边界检查（ID=307）
void FeatureOutOfBoundaryCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//内容与图廓错位检查（ID=311）
void ContentFrameMisalignmentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//标准图例匹配检查（ID=308）
void LegendMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//图名与行政区划匹配检查（ID=309）
void MapNameMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//必需图层检查（ID=310）
void RequiredLayersCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);
//行政区划设色检查（ID=312）
void AdminColorStyleCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// ========== 投影坐标检查算法 ==========
//投影参数准确性检查（ID=401）
void ProjectionParameterCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result);

// 工具函数：根据分号分隔的标准图层名，返回所有实际图层完整路径
QStringList getActualLayerPaths(const QString& stdLayerNames, AlgorithmFilePaths& filePaths);

// 根据图层名称匹配对应的XML文件
QString MatchXmlByLayerName(const QStringList& xmlPaths, const QString& standardLayer);

#endif // MAP_CHECK_ALGORITHMS_H
