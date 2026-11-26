#pragma once
#ifndef JSON_RESULT_PARSER_H
#define JSON_RESULT_PARSER_H

#include <QString>
#include "map_check_common.h"

/**
 * @brief 从算法生成的JSON文件中解析并填充CheckItemResult
 * @param jsonFilePath JSON文件路径
 * @param result 输出的CheckItemResult结构
 * @param checkItemName 可选的检查项名称（如果为空则使用JSON中的name）
 * @return true 解析成功, false 解析失败
 */
bool ParseJsonToCheckItemResult(
    const QString& jsonFilePath,
    CheckItemResult& result,
    const QString& checkItemName = QString()
);

#endif // JSON_RESULT_PARSER_H
