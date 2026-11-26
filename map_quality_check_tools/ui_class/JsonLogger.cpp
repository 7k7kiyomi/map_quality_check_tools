#include "JsonLogger.h"

JsonLogger::JsonLogger(QObject* parent)
    : QObject(parent)
{
    // 不需要初始化其他内容
}

void JsonLogger::addCheckItem(const QString& checkId, const QString& checkItemName,
    const QString& status, bool errExist, const QString& info,
    const QJsonObject& errors)
{
    QJsonObject checkItem;
    checkItem["id"] = checkId;
    checkItem["name"] = checkItemName;
    checkItem["status"] = status;
    checkItem["err_exist"] = errExist;
    checkItem["info"] = info;

    // 只有当存在错误时才添加errors字段
    if (errExist && !errors.isEmpty()) {
        checkItem["errors"] = errors;
    }

    m_jsonLog["check_item"] = checkItem;  // 使用固定的键名 "check_item"
}

QString JsonLogger::createErrorDescription(int featureId, const QString& description)
{
    return description;
}

// 改为静态方法
QJsonObject JsonLogger::createErrorObject(int featureId, const QString& description)
{
    QJsonObject error;
    error["feature_id"] = featureId;
    error["description"] = description;
    error["timestamp"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    return error;
}

bool JsonLogger::saveToFile(const QString& filePath)
{
    // 确保目录存在
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        return false;
    }

    QFile jsonFile(filePath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QJsonDocument doc(m_jsonLog);
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        return true;
    }

    return false;
}

bool JsonLogger::saveJsonToFile(const QJsonObject& jsonObject, const QString& filePath)
{
    // 确保目录存在
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        return false;
    }

    QFile jsonFile(filePath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QJsonDocument doc(jsonObject);
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        return true;
    }

    return false;
}