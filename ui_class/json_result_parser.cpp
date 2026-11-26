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
    
    // 尝试多种可能的格式
    QJsonObject checkItem;
    
    // 辅助函数：标准化检查项名称（去除空格、斜杠等）
    auto normalizeCheckItemName = [](const QString& name) -> QString {
        QString normalized = name;
        normalized.remove(' ');       // 去除空格
        normalized.remove('/');       // 去除斜杠
        normalized.remove('\\');      // 去除反斜杠
        normalized.remove('-');       // 去除连字符
        return normalized.toLower();  // 转小写
    };
    
    // 格式1: 如果提供了 checkItemName,先尝试按名称匹配（使用标准化比较）
    if (!checkItemName.isEmpty()) {
        QString normalizedCheckName = normalizeCheckItemName(checkItemName);
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (it.key().startsWith("check_item")) {
                QJsonObject item = it.value().toObject();
                QString itemName = item["name"].toString();
                if (normalizeCheckItemName(itemName) == normalizedCheckName) {
                    checkItem = item;
                    break;
                }
            }
        }
    }
    
    // 格式2: 如果按名称没找到,尝试查找嵌套的 check_item, check_item1, check_item2 等
    if (checkItem.isEmpty()) {
        if (root.contains("check_item")) {
            checkItem = root["check_item"].toObject();
        } else if (root.contains("check_item1")) {
            checkItem = root["check_item1"].toObject();
        } else if (root.contains("check_item2")) {
            checkItem = root["check_item2"].toObject();
        } else if (root.contains("check_item3")) {
            checkItem = root["check_item3"].toObject();
        } else {
            // 尝试找第一个包含check_item前缀的键
            bool found = false;
            for (auto it = root.begin(); it != root.end(); ++it) {
                if (it.key().startsWith("check_item")) {
                    checkItem = it.value().toObject();
                    found = true;
                    break;
                }
            }
            
            // 格式3: 如果没找到嵌套结构,检查根对象是否直接包含必需字段
            // (例如算法直接输出的扁平JSON格式)
            if (!found && root.contains("id") && root.contains("status")) {
                checkItem = root;  // 使用根对象本身
            }
        }
    }
    
    if (checkItem.isEmpty()) {
        return false;
    }

    // 直接从JSON填充CheckItemResult
    result.id = checkItem["id"].toString();
    result.name = checkItemName.isEmpty() ? checkItem["name"].toString() : checkItemName;
    result.status = checkItem["status"].toString();
    
    // 健壮地处理 err_exist 字段
    QJsonValue errExistValue = checkItem["err_exist"];
    if (errExistValue.isBool()) {
        result.err_exist = errExistValue.toBool();
    } else if (errExistValue.isString()) {
        result.err_exist = (errExistValue.toString().toLower() == "true");
    } else {
        result.err_exist = false;
    }

    // 填充info字段 (QJsonObject)
    QJsonValue infoValue = checkItem["info"];
    if (infoValue.isString()) {
        // 如果info是字符串，包装成对象
        result.info = QJsonObject();
        result.info["description"] = infoValue.toString();
    } else if (infoValue.isObject()) {
        // 如果info是对象，直接赋值
        result.info = infoValue.toObject();
    } else {
        result.info = QJsonObject();
    }

    // 填充errors字段 (QJsonObject)
    if (checkItem.contains("errors")) {
        QJsonValue errorsValue = checkItem["errors"];
        if (errorsValue.isObject()) {
            // errors是对象格式 {"error1": "描述1", "error2": "描述2"}
            result.errors = errorsValue.toObject();
        } else if (errorsValue.isArray()) {
            // errors是数组格式，转换为对象
            QJsonArray errorsArray = errorsValue.toArray();
            result.errors = QJsonObject();
            int errorIndex = 1;
            for (const QJsonValue& errorVal : errorsArray) {
                QString errorKey = QString("error%1").arg(errorIndex++);
                result.errors[errorKey] = errorVal;
            }
        } else {
            result.errors = QJsonObject();
        }
    } else {
        result.errors = QJsonObject();
    }

    return true;
}