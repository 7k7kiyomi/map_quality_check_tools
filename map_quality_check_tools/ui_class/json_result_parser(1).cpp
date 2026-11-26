#include "json_result_parser.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

bool ParseJsonToCheckItemResult(
    const QString& jsonFilePath,
    CheckItemResult& result,
    const QString& checkItemName)
{
    // 如果没有提供要查找的名称，则无法进行精确匹配，直接失败
    if (checkItemName.isEmpty()) {
        return false;
    }

    QFile jsonFile(jsonFilePath);
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray jsonData = jsonFile.readAll();
    jsonFile.close();

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (doc.isNull() || !doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();

    // --- 新逻辑：遍历所有顶级键，寻找名称匹配的 check_item ---
    for (const QString& key : root.keys()) {
        QJsonValue value = root[key];

        // 只处理值为对象的顶级键
        if (value.isObject()) {
            QJsonObject potentialCheckItem = value.toObject();

            // 检查该对象是否包含 "name" 字段，并且其值是否与我们寻找的名称匹配
            if (potentialCheckItem.contains("name") && potentialCheckItem["name"].toString() == checkItemName) {

                // --- 找到了匹配的 check_item，开始填充结果 ---
                result.id = potentialCheckItem["id"].toString();
                result.name = checkItemName; // 名称已经匹配，直接使用
                result.status = potentialCheckItem["status"].toString();
                // ******** 修改点 ********
                // --- 健壮地处理 "err_exist" 字段，兼容布尔值和字符串 ---
                QJsonValue errExistValue = potentialCheckItem["err_exist"];
                if (errExistValue.isBool()) {
                    // 1. 如果是标准的布尔值 (true/false)，直接转换
                    result.err_exist = errExistValue.toBool();
                }
                else if (errExistValue.isString()) {
                    // 2. 如果是字符串 ("true"/"false")，进行不区分大小写的比较
                    result.err_exist = (errExistValue.toString().toLower() == "true");
                }
                else {
                    // 3. 对于其他任何类型或缺失该字段，默认为 false
                    result.err_exist = false;
                }
                // ******** 修改结束 ********

                // 正确填充 info 字段 (QJsonObject)
                QJsonValue infoValue = potentialCheckItem["info"];
                if (infoValue.isObject()) {
                    result.info = infoValue.toObject();
                }
                else if (infoValue.isString()) {
                    result.info["description"] = infoValue.toString();
                }
                else {
                    result.info["value"] = infoValue.toVariant().toString();
                }

                // 正确填充 errors 字段 (QJsonObject)
                if (potentialCheckItem.contains("errors")) {
                    QJsonValue errorsValue = potentialCheckItem["errors"];
                    if (errorsValue.isObject()) {
                        result.errors = errorsValue.toObject();
                    }
                    else if (errorsValue.isArray()) {
                        QJsonArray errorsArray = errorsValue.toArray();
                        int errorIndex = 1;
                        for (const QJsonValue& errorVal : errorsArray) {
                            QString errorKey = QString("error%1").arg(errorIndex++);
                            result.errors[errorKey] = errorVal;
                        }
                    }
                }

                return true; // 成功找到并解析，立即返回
            }
        }
    }

    // --- 如果遍历完所有键都没有找到匹配的，则返回失败 ---
    return false;
}