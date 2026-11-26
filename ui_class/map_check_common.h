#pragma once
#ifndef MAP_CHECK_COMMON_H
#define MAP_CHECK_COMMON_H

#include <QString>
#include <QJsonObject> // <-- 替换
#include <QMetaType>

// 与全局JSON的check_item节点结构一致
struct CheckItemResult {
    QString id;          // 质检项唯一标识
    QString name;        // 质检项名称（用于匹配）
    QString status;      // 状态："未质检"/"质检成功"/"质检失败"
    bool err_exist;      // 是否存在错误
    QJsonObject info;   // <-- 替换
    QJsonObject errors; // <-- 替换
};

Q_DECLARE_METATYPE(CheckItemResult) // 支持Qt信号传递

#endif // MAP_CHECK_COMMON_H