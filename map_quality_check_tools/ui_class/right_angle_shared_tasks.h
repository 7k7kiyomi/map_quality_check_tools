#pragma once

#include "line_topology_shared_tasks.h"
#include <QHash>
#include <QJsonDocument>
#include <QSaveFile>

namespace RightAngleSharedTasks {

inline QString taskKey(const QString& checkName, const AlgorithmFilePaths& paths,
                       const QString& xmlPath, int scale)
{
    if (checkName.trimmed() != QStringLiteral("直角化角度阈值控制") || scale <= 0)
        return {};
    const QJsonArray actual = LineTopologySharedTasks::inputSignature(paths.actualShpSemicolon);
    const QJsonArray reference = LineTopologySharedTasks::inputSignature(paths.gdbShpSemicolon);
    if (actual.isEmpty() || reference.isEmpty()) return {};
    QFile xml(xmlPath);
    if (!xml.open(QIODevice::ReadOnly)) return {};
    QJsonObject signature;
    signature[QStringLiteral("actual")] = actual;
    signature[QStringLiteral("reference")] = reference;
    signature[QStringLiteral("xml_hash")] = QString::fromLatin1(
        QCryptographicHash::hash(xml.readAll(), QCryptographicHash::Sha256).toHex());
    signature[QStringLiteral("scale")] = scale;
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(signature).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

class Cache {
public:
    bool find(const QString& key, const QString& checkName, CheckItemResult& result) const
    {
        if (key.isEmpty() || !m_results.contains(key)) return false;
        result = m_results.value(key);
        result.name = checkName;
        result.info.insert(QStringLiteral("reused_shared_right_angle_check"), true);
        return true;
    }

    void remember(const QString& key, const QString& layerName, const CheckItemResult& result)
    {
        if (key.isEmpty() || (result.status != QStringLiteral("质检成功")
                              && result.status != QStringLiteral("质检完成"))) return;
        CheckItemResult saved = result;
        saved.info.insert(QStringLiteral("shared_check_owner_layer"), layerName);
        m_results.insert(key, saved);
    }

private:
    QHash<QString, CheckItemResult> m_results;
};

inline bool writeReusedOutput(const QString& outputDir, const QString& layerName,
                              const CheckItemResult& result)
{
    const QString jsonDir = QDir(outputDir).filePath(QStringLiteral("质检json文件"));
    if (!QDir().mkpath(jsonDir)) return false;
    const QString path = QDir(jsonDir).filePath(
        QStringLiteral("直角化角度阈值_%1.json").arg(layerName));
    QJsonObject item;
    item[QStringLiteral("id")] = result.id;
    item[QStringLiteral("name")] = result.name;
    item[QStringLiteral("status")] = result.status;
    item[QStringLiteral("err_exist")] = result.err_exist;
    item[QStringLiteral("info")] = result.info;
    item[QStringLiteral("errors")] = result.errors;
    QSaveFile output(path);
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("check_item"), item}})
        .toJson(QJsonDocument::Indented);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size()
        && output.commit();
}

} // namespace RightAngleSharedTasks
