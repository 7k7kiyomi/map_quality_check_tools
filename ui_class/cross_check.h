#pragma once
#ifndef CROSS_CHECK_H
#define CROSS_CHECK_H

#include <QString>
#include <QStringList>
#include <QMap>
#include <QSet>
#include <QJsonObject>
#include <QJsonArray>

// -------------------------------------------------------
// 数据结构
// -------------------------------------------------------

// 单个FID在某个type下的检查记录
struct CrossCheckFidRecord
{
    QString fid;           // anno_fid
    QString name;          // 注记文字
    QString matchStat;     // FULLY_MATCHED / PARTIALLY_MATCHED / UNMATCHED
    QString errorType;     // error_type from SHP (CONTENT_ERROR / MATCH_ERROR / ...)
    QString shpPath;       // 输出SHP路径
    QString jsonPath;      // 输出JSON路径
};

// 中间文件的数据结构
struct CrossCheckData
{
    QString annotLayer;                                 // 注记图层标识名
    QMap<QString, QList<CrossCheckFidRecord>> types;   // type名 → 记录列表
};

// -------------------------------------------------------
// 公共接口
// -------------------------------------------------------

// 执行跨类型交叉验证
// outputDir:        错误结果输出目录（中间文件存放位置）
// annotLayerName:   注记图层标识名（用于中间文件命名）
// currentType:      当前检查的type名（如"等外河流_标注"）
// currentShpPath:   当前type的错误SHP路径
// currentJsonPath:  当前type的JSON报告路径
// logDetails:       输出参数，记录被移除的条目详情（供UI展示）
// 返回值：本次是否修正了任何输出
bool performCrossTypeCheck(
    const QString& outputDir,
    const QString& annotLayerName,
    const QString& currentType,
    const QString& currentShpPath,
    const QString& currentJsonPath,
    QString& logDetails
);

// 从200匹配结果SHP获取该type下所有FID，减去210 temp SHP中的错误FID，
// 差值即为被210判定正确的FID，写入中间文件供跨类型交叉验证使用。
// 必须在temp文件被清理前、performCrossTypeCheck之前调用。
// matchResultShpPath: 200匹配结果SHP路径（含该type所有注记FID）
// tempShpPath:        210全量temp SHP路径（含所有错误FID）
void saveFullMatchedToCrossCheck(
    const QString& outputDir,
    const QString& annotLayerName,
    const QString& currentType,
    const QString& finalJsonPath,
    const QString& matchResultShpPath,
    const QString& tempShpPath
);

#endif // CROSS_CHECK_H
