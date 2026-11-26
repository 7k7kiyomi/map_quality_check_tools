#pragma once
#ifndef ALGORITHM_FILE_PATHS_H
#define ALGORITHM_FILE_PATHS_H

#include <QString>
#include <QStringList>

// 算法所需文件路径封装（统一传递给所有算法）
struct AlgorithmFilePaths {
    QString pdfPath;     // 地图PDF路径
    QString shpPath;     // SHP文件夹路径
    QString seriesLeftShpPath;  // 501 左侧成果 SHP 目录；跨比例尺时为大比例尺成果目录，多个目录用分号分隔
    QString seriesRightShpPath; // 501 右侧成果 SHP 目录；跨比例尺时为小比例尺成果目录，多个目录用分号分隔
    QString seriesLayerMappingJson; // 501 用户确认后的左右图层对应关系 JSON
    QString gdbShpPath;  // GDB_SHP文件夹路径
    QString legendDir;   // 标准图例文件夹路径
    QString csvPath;     // 图面配置CSV路径

    // 拼接后的具体shp路径
    QString gdbShp;       // gdb来源的具体shp路径
    QString actualShp;    // 实际图层的具体shp路径
    QString annotShp;     // 实际注记图层的具体shp路径

    QString standardLayer;  // 第1列：标准图层（与子任务名称匹配）
    QString gdbLayer;       // 第2列：gdb图层
    QString actualLayer;    // 第3列：实际图层
    QString actualAnnotLayer; // 第4列：实际注记图层
    QString correspondingAnnotLayer; // 第5列：对应注记层
    QString legendField;    // 第6列：图例字段
    QString labelField;     // 第7列：标注字段（可选，保留当前版本已有能力）

    // 分号拼接的最终路径（供算法直接使用）
    QString gdbShpSemicolon;      // gdb图层路径，格式："path1;path2"
    QString actualShpSemicolon;   // 实际图层路径，格式："path1;path2"
    QString annotShpSemicolon;    // 注记图层路径，格式："path1;path2"

    QStringList xmlPaths;      // 质检规则文件路径
    QStringList actualShpList; // 实际图层的点/线/面路径列表
    QStringList annotShpList;  // 实际注记图层的点/线/面路径列表

    // 122 全局空白检查的任务严重程度；其他输入由本图目录、CSV 和 XML 解析。
    QString blankSeverity;

    // 123 的任务 XML 由任务树精确选择；源匹配容差为图上毫米。
    QString conflictXmlPath;
    double conflictSourceMatchToleranceMm = 5.0;

    // 121 的规则及本幅图本次运行的 200 匹配结果。
    QString densityXmlPath;
    QString densitySeverity;
    QString densityMatchesJson;
    QString annotationMatchResultPath; // 本图当前标准图层最终的 200 结果（可为交叉验证轮次文件）
    double densitySourceMatchToleranceMm = 5.0;

    // 系列图图幅对执行上下文（501）
    QString seriesPairName;
    QString seriesLeftSheetName;
    QString seriesRightSheetName;
    QString seriesLeftSheetWkt;
    QString seriesRightSheetWkt;
    int seriesLeftScale = 0;
    int seriesRightScale = 0;

    // 跨比例尺检查模式字段
    QString checkMode; // "SameScale" 或 "CrossScale"
    QString seriesSmallScaleSheetName;
    QString seriesSmallScaleSheetWkt;
    QStringList seriesLargeScaleSheetNames;
    QStringList seriesLargeScaleSheetWkts;

};

#endif // ALGORITHM_FILE_PATHS_H
