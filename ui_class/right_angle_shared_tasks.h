#pragma once

#include "line_topology_shared_tasks.h"
#include "QualityCheckConfig.h"
#include <cmath>

namespace RightAngleSharedTasks {

inline QString taskKey(const QString& checkName, const AlgorithmFilePaths& paths,
                       const QString& xmlPath, int scale)
{
    if (checkName.trimmed() != QStringLiteral("直角化角度阈值控制") || scale <= 0) return {};
    const QJsonArray parsed = LineTopologySharedTasks::inputSignature(paths.actualShpSemicolon);
    const QJsonArray reference = LineTopologySharedTasks::inputSignature(paths.gdbShpSemicolon);
    if (parsed.isEmpty() || reference.isEmpty()) return {};
    QFile xml(xmlPath);
    QDomDocument document;
    if (!xml.open(QIODevice::ReadOnly) || !document.setContent(&xml)) return {};
    bool hasRule = false;
    const QDomNodeList items = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < items.size(); ++i) {
        const QDomElement item = items.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("id")).text().trimmed() == QStringLiteral("109") &&
            item.firstChildElement(QStringLiteral("name")).text().trimmed() == checkName.trimmed())
            hasRule = true;
    }
    QualityCheckConfig config;
    if (!hasRule || !config.loadFromFile(xmlPath)) return {};
    // Use the values actually read by 109, rather than the XML filename,
    // comments, descriptions, formatting, or unrelated checks.
    QJsonArray parameters;
    for (double value : {config.getRightAngleLowerLimit(), config.getRightAngleUpperLimit(),
        config.getRightAngleDatabaseMatchTolerance(), config.getRightAngleGeometrySimilarityThreshold(),
        config.getRightAngleTurningPointTolerance(), config.getRightAngleTurningPointMatchTolerance(),
        config.getRightAngleMinSegmentLength()}) {
        if (!std::isfinite(value)) return {};
        parameters.append(value);
    }
    // Minimum edge length is scaled according to its configured unit. Keep
    // that unit distinct as well as the numeric value (e.g. 10m versus 10mm).
    const QString lengthUnit = config.getParameterString(checkName.trimmed(),
        QStringLiteral("right_angle_min_segment_length_unit")).trimmed();
    const QJsonObject request{{"id", "109"}, {"parsed", parsed}, {"reference", reference},
        {"scale", scale}, {"parameters", parameters}, {"minimum_segment_length_unit", lengthUnit}};
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(request).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

inline bool writeReusedOutput(const QString& outputRoot, const QString& layer, const CheckItemResult& result)
{
    const QString jsonDir = QDir(outputRoot).filePath(QStringLiteral("质检json文件"));
    const QString logDir = QDir(outputRoot).filePath(QStringLiteral("质检日志文件"));
    if (!QDir().mkpath(jsonDir) || !QDir().mkpath(logDir)) return false;
    const QString name = QStringLiteral("直角化角度阈值_%1").arg(layer);
    const QJsonObject item{{"id", result.id}, {"name", result.name}, {"status", result.status},
        {"err_exist", result.err_exist}, {"errors", result.errors}, {"info", result.info}};
    QSaveFile json(QDir(jsonDir).filePath(name + QStringLiteral(".json")));
    const QByteArray data = QJsonDocument(QJsonObject{{"check_item4", item}}).toJson(QJsonDocument::Indented);
    if (!json.open(QIODevice::WriteOnly) || json.write(data) != data.size() || !json.commit()) return false;
    QSaveFile log(QDir(logDir).filePath(name + QStringLiteral(".log")));
    const QByteArray message = result.info.value("description").toString().toUtf8() + '\n';
    return log.open(QIODevice::WriteOnly) && log.write(message) == message.size() && log.commit();
}

// One instance per map run; never retain results across maps or restarts.
class Cache {
public:
    bool find(const QString& key, const QString& checkName, CheckItemResult& result) const
    {
        const auto found = m_results.constFind(key);
        if (key.isEmpty() || found == m_results.constEnd()) return false;
        const Entry& owner = found.value();
        result = owner.result;
        result.name = checkName;
        result.errors = QJsonObject{};
        result.err_exist = false;
        result.info.remove("error_layer_paths");
        result.info["error_count"] = 0;
        result.info["reused_shared_check"] = true;
        result.info["duplicate_of_layer"] = owner.layer;
        result.info["shared_result_has_errors"] = owner.result.err_exist;
        result.info["shared_error_count"] = owner.result.info.value("error_count").toInt(owner.result.errors.size());
        result.info["shared_summary"] = owner.result.info.value("summary");
        result.info["shared_result_json"] = QStringLiteral("质检json文件/直角化角度阈值_%1.json").arg(owner.layer);
        result.info["algorithm_executed"] = false;
        const QString description = QStringLiteral("同一数据及直角化规则已由“%1”完成检查；本任务引用该结果，错误不重复统计。共享结果错误记录数：%2。")
            .arg(owner.layer).arg(result.info.value("shared_error_count").toInt());
        result.info["description"] = description;
        result.info["summary"] = description;
        return true;
    }

    void remember(const QString& key, const QString& layer, const CheckItemResult& result)
    {
        if (key.isEmpty() || result.id != QStringLiteral("109") || result.info.value("reused_shared_check").toBool() ||
            (result.status != QStringLiteral("质检成功") && result.status != QStringLiteral("质检完成"))) return;
        m_results.insert(key, Entry{layer, result});
    }

private:
    struct Entry { QString layer; CheckItemResult result; };
    QHash<QString, Entry> m_results;
};

} // namespace RightAngleSharedTasks
