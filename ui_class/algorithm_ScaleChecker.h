#pragma once
#ifndef ALGORITHM_SCALE_CHECKER_H
#define ALGORITHM_SCALE_CHECKER_H

#include "map_check_common.h"
#include "algorithm_file_paths.h"
#include <QList>
#include <qgspointxy.h> 
#include <qobject.h>

// 比例尺检查算法类
class ScaleCheckerAlgorithm : public QObject {
        Q_OBJECT
public:
    // 执行比例尺检查算法
    static void execute(
        const QString& checkItemName,       // 质检项名称
        const AlgorithmFilePaths& filePaths, // 输入文件路径
        int /*scale*/,                      // 比例尺（当前算法暂不用，保留参数位）
        const QString& outputDir,           // 输出目录（与JSON同目录）
        CheckItemResult& result             // 结果输出
    );

private:
    // 从内图廓SHP提取顶点信息
    static bool extractMapFramePointsFromShp(const QString& shpPath, QList<QgsPointXY>& points, double& attrPixelWidth, double& attrPixelHeight, double& mapFrameWidth);
    // 计算内图廓宽度
    static double calculateMapFrameWidth(const QList<QgsPointXY>& points);
    // 验证比例尺文本格式
    static bool isValidScaleText(const QString& text, QString* scaleValue);
    // 计算实际地图比例尺
    static QString calculateActualMapScale(double mapFrameWidth, double attrPixelWidth);
    // 从图例SHP提取比例尺
    static bool extractScaleFromLegendShp(const QString& shpPath, QString& scaleValue, int& errorFeatureId);
};

// 算法函数声明（符合统一接口）
/*void algorithm_ScaleValueCheck(
    const QString& checkItemName,
    const AlgorithmFilePaths& filePaths,
    int scale,
    const QString& outputDir,
    CheckItemResult& result
);*/

#endif // ALGORITHM_SCALE_CHECKER_H
