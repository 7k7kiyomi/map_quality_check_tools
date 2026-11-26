#include "map_check_algorithms.h"
#include "map_check_task_manager.h"
#include "json_result_parser.h"
#include "../algorithm/se_feature_conflict_check.h"

#include <QDir>
#include <QDateTime>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>

namespace {

void appendConflictTrace(const QString& logPath, const QString& text, bool truncate = false)
{
    QFile file(logPath);
    const QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text |
        (truncate ? QIODevice::Truncate : QIODevice::Append);
    if (!file.open(mode)) return;
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODate) +
        QStringLiteral(" [123封装] ") + text + QLatin1Char('\n');
    file.write(line.toUtf8());
    file.flush();
}

void failConflict(const QString& name, const QString& jsonPath,
                  const QString& reason, CheckItemResult& result)
{
    result.id = QStringLiteral("123");
    result.name = name;
    result.status = QStringLiteral("质检失败");
    result.err_exist = true;
    result.info = QJsonObject{{QStringLiteral("description"), reason}};
    result.errors = QJsonObject{{QStringLiteral("error1"), reason}};
    const QJsonObject item{{QStringLiteral("id"), result.id},
                           {QStringLiteral("name"), result.name},
                           {QStringLiteral("status"), result.status},
                           {QStringLiteral("err_exist"), true},
                           {QStringLiteral("info"), reason},
                           {QStringLiteral("errors"), result.errors}};
    QFile file(jsonPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("check_item"), item}})
                       .toJson(QJsonDocument::Indented));
}

bool readRules(const QString& xmlPath, int scale, QMap<QString, QString>& values, QString& error)
{
    QFile file(xmlPath);
    QDomDocument doc;
    if (xmlPath.isEmpty() || !file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) {
        error = QStringLiteral("无法读取123规则 XML：%1").arg(xmlPath);
        return false;
    }
    if (doc.documentElement().tagName() != QStringLiteral("check_list")) {
        error = QStringLiteral("123规则 XML 根节点不是 check_list：%1").arg(xmlPath);
        return false;
    }
    QDomElement exact, generic;
    const QDomNodeList scales = doc.documentElement().elementsByTagName(QStringLiteral("scale"));
    for (int i = 0; i < scales.size(); ++i) {
        const QDomElement scaleElement = scales.at(i).toElement();
        const QString value = scaleElement.firstChildElement(QStringLiteral("value")).text().trimmed();
        bool ok = false;
        const int configured = value.toInt(&ok);
        const bool isExact = ok && configured == scale;
        const bool isGeneric = value.isEmpty() || (ok && configured == 0);
        if (!isExact && !isGeneric) continue;
        const QDomNodeList items = scaleElement.elementsByTagName(QStringLiteral("check_item"));
        for (int j = 0; j < items.size(); ++j) {
            const QDomElement item = items.at(j).toElement();
            if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != QStringLiteral("123") &&
                item.firstChildElement(QStringLiteral("name")).text().trimmed() !=
                    QStringLiteral("要素表示冲突检查")) continue;
            QDomElement& selected = isExact ? exact : generic;
            if (!selected.isNull()) {
                error = QStringLiteral("123规则 XML 同一比例尺有多个检查项：%1").arg(xmlPath);
                return false;
            }
            selected = item;
        }
    }
    const QDomElement selected = exact.isNull() ? generic : exact;
    if (selected.isNull()) {
        error = QStringLiteral("123规则 XML 没有匹配比例尺 1:%1 的检查项：%2").arg(scale).arg(xmlPath);
        return false;
    }
    const QDomNodeList rules = selected.elementsByTagName(QStringLiteral("rule"));
    for (int i = 0; i < rules.size(); ++i) {
        const QDomElement rule = rules.at(i).toElement();
        const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
        if (values.contains(name)) {
            error = QStringLiteral("123规则 XML 有重复规则：%1").arg(name);
            return false;
        }
        values.insert(name, rule.firstChildElement(QStringLiteral("value")).text().trimmed());
    }
    return true;
}

bool needsSource(const QMap<QString, QString>& rules, const QString& prefix)
{
    const QString fieldPrefix = prefix + QStringLiteral("属性筛选字段");
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it)
        if (it.key().startsWith(fieldPrefix) && !it.value().trimmed().isEmpty()) return true;
    return false;
}

QStringList standardNames(QString text)
{
    text.replace(QChar(0xff1b), QChar(';'));
    QStringList names;
    for (QString name : text.split(';', Qt::SkipEmptyParts)) {
        name = name.trimmed();
        if (!name.isEmpty() && !names.contains(name)) names << name;
    }
    return names;
}

QStringList mappedFiles(const QStringList& standards, const QMap<QString, CsvLayerInfo>& layerMap,
                        const QString& root, bool sourceFiles, QString& error)
{
    QStringList paths;
    if (standards.isEmpty() || root.trimmed().isEmpty()) {
        error = sourceFiles ? QStringLiteral("缺少源图层名称或 GDB_SHP 目录")
                            : QStringLiteral("缺少解析图层名称或解析目录");
        return paths;
    }
    for (const QString& standard : standards) {
        if (!layerMap.contains(standard)) {
            error = QStringLiteral("CSV 没有标准图层：%1").arg(standard);
            return QStringList();
        }
        const CsvLayerInfo info = layerMap.value(standard);
        const QList<QString> names = sourceFiles ? info.gdbLayerNames : info.actualLayerNames;
        if (names.isEmpty()) {
            error = QStringLiteral("标准图层 %1 没有%2映射")
                        .arg(standard, sourceFiles ? QStringLiteral("源数据") : QStringLiteral("解析图层"));
            return QStringList();
        }
        for (const QString& name : names) {
            QStringList found;
            const QStringList suffixes = sourceFiles ? QStringList{QString()} :
                QStringList{QString(), QStringLiteral("_Point"), QStringLiteral("_LineString"),
                            QStringLiteral("_Polygon")};
            for (const QString& suffix : suffixes) {
                const QString path = QDir(root).filePath(name + suffix + QStringLiteral(".shp"));
                if (QFileInfo::exists(path) && !found.contains(path, Qt::CaseInsensitive)) found << path;
            }
            if (found.isEmpty()) {
                error = QStringLiteral("找不到标准图层 %1 的%2文件：%3")
                            .arg(standard, sourceFiles ? QStringLiteral("源数据") : QStringLiteral("解析"), name);
                return QStringList();
            }
            for (const QString& path : found)
                if (!paths.contains(path, Qt::CaseInsensitive)) paths << path;
        }
    }
    return paths;
}

} // namespace

void FeatureConflictCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths,
                                 int scale, const QString& outputDir, CheckItemResult& result)
{
    const QString layerName = filePaths.standardLayer.trimmed();
    const QDir output(outputDir);
    const QString jsonDir = output.filePath(QStringLiteral("质检json文件"));
    const QString logDir = output.filePath(QStringLiteral("质检日志文件"));
    const QString errorDir = output.filePath(QStringLiteral("质检错误结果"));
    QDir().mkpath(jsonDir);
    QDir().mkpath(logDir);
    QDir().mkpath(errorDir);
    const QString base = QStringLiteral("要素表示冲突检查_") + layerName;
    const QString jsonPath = QDir(jsonDir).filePath(base + QStringLiteral(".json"));
    const QString logPath = QDir(logDir).filePath(base + QStringLiteral(".log"));
    const QString errorPath = QDir(errorDir).filePath(
        QStringLiteral("要素表示冲突检查错误_") + layerName + QStringLiteral(".shp"));
    appendConflictTrace(logPath,
        QStringLiteral("开始；标准图层=%1；比例尺=%2；XML=%3")
            .arg(layerName).arg(scale).arg(filePaths.conflictXmlPath), true);
    if (QFileInfo::exists(jsonPath) && !QFile::remove(jsonPath)) {
        appendConflictTrace(logPath, QStringLiteral("无法清除上次运行的JSON"));
        failConflict(checkItemName, jsonPath,
                     QStringLiteral("无法清除上次运行的 JSON：%1").arg(jsonPath), result);
        return;
    }
    if (scale <= 0 || layerName.isEmpty()) {
        appendConflictTrace(logPath, QStringLiteral("缺少有效比例尺或待检标准图层名称"));
        failConflict(checkItemName, jsonPath, QStringLiteral("缺少有效比例尺或待检标准图层名称"), result);
        return;
    }
    QMap<QString, QString> rules;
    QString inputError;
    appendConflictTrace(logPath, QStringLiteral("开始读取规则XML"));
    if (!readRules(filePaths.conflictXmlPath, scale, rules, inputError)) {
        appendConflictTrace(logPath, QStringLiteral("规则XML读取失败：%1").arg(inputError));
        failConflict(checkItemName, jsonPath, inputError, result);
        return;
    }
    appendConflictTrace(logPath, QStringLiteral("规则XML读取完成；规则数=%1").arg(rules.size()));
    appendConflictTrace(logPath, QStringLiteral("开始解析图面配置CSV"));
    const QMap<QString, CsvLayerInfo> layerMap =
        map_check_task_manager::parseCsvToLayerMap(filePaths.csvPath);
    if (layerMap.isEmpty()) {
        appendConflictTrace(logPath, QStringLiteral("图面配置CSV缺失或无法解析：%1").arg(filePaths.csvPath));
        failConflict(checkItemName, jsonPath, QStringLiteral("图面配置 CSV 缺失或无法解析"), result);
        return;
    }
    appendConflictTrace(logPath, QStringLiteral("图面配置CSV解析完成；标准图层数=%1").arg(layerMap.size()));
    const QStringList target{layerName};
    appendConflictTrace(logPath, QStringLiteral("开始映射待检解析图层"));
    const QStringList targetParsed = mappedFiles(target, layerMap, filePaths.shpPath, false, inputError);
    if (!inputError.isEmpty()) {
        appendConflictTrace(logPath, QStringLiteral("待检解析图层映射失败：%1").arg(inputError));
        failConflict(checkItemName, jsonPath, inputError, result);
        return;
    }
    appendConflictTrace(logPath, QStringLiteral("待检解析图层映射完成；文件数=%1").arg(targetParsed.size()));
    QStringList targetSources;
    if (needsSource(rules, QString())) {
        appendConflictTrace(logPath, QStringLiteral("开始映射待检源数据图层"));
        targetSources = mappedFiles(target, layerMap, filePaths.gdbShpPath, true, inputError);
        if (!inputError.isEmpty()) {
            appendConflictTrace(logPath, QStringLiteral("待检源数据图层映射失败：%1").arg(inputError));
            failConflict(checkItemName, jsonPath, inputError, result);
            return;
        }
        appendConflictTrace(logPath, QStringLiteral("待检源数据图层映射完成；文件数=%1").arg(targetSources.size()));
    }

    QJsonObject associations;
    QStringList mappingWarnings;
    appendConflictTrace(logPath, QStringLiteral("开始映射关联图层"));
    const QRegularExpression groupRule(QStringLiteral("^关联图层([1-9][0-9]*)$"));
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        const QRegularExpressionMatch match = groupRule.match(it.key());
        if (!match.hasMatch()) continue;
        const QString group = match.captured(1);
        const QStringList names = standardNames(it.value());
        QString error;
        QStringList parsed = mappedFiles(names, layerMap, filePaths.shpPath, false, error);
        QStringList sources;
        if (error.isEmpty() && needsSource(rules, it.key()))
            sources = mappedFiles(names, layerMap, filePaths.gdbShpPath, true, error);
        if (!error.isEmpty()) {
            mappingWarnings << QStringLiteral("关联图层%1[%2]：%3").arg(group, it.value(), error);
            parsed.clear();
            sources.clear();
        }
        associations.insert(group, QJsonObject{
            {QStringLiteral("parsed_paths"), parsed.join(';')},
            {QStringLiteral("source_paths"), sources.join(';')},
            {QStringLiteral("source_layers"), QString()}});
    }
    appendConflictTrace(logPath,
        QStringLiteral("关联图层映射完成；组数=%1；警告数=%2")
            .arg(associations.size()).arg(mappingWarnings.size()));
    const QByteArray xml = filePaths.conflictXmlPath.toUtf8();
    const QByteArray parsed = targetParsed.join(';').toUtf8();
    const QByteArray source = targetSources.join(';').toUtf8();
    const QByteArray associationJson = QJsonDocument(associations).toJson(QJsonDocument::Compact);
    const QByteArray outputBytes = errorPath.toUtf8();
    const QByteArray jsonBytes = jsonPath.toUtf8();
    const QByteArray logBytes = logPath.toUtf8();
    appendConflictTrace(logPath, QStringLiteral("准备调用123核心算法"));
    const int code = FeatureConflictCheck(xml.constData(), parsed.constData(), source.constData(), "",
                                          associationJson.constData(), scale, outputBytes.constData(),
                                          jsonBytes.constData(), logBytes.constData(),
                                          filePaths.conflictSourceMatchToleranceMm);
    if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
        failConflict(checkItemName, jsonPath,
                     QStringLiteral("123未生成可解析的本次 JSON，错误码：%1").arg(code), result);
        return;
    }
    if (code != 0 || result.status != QStringLiteral("质检成功")) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info[QStringLiteral("return_code")] = code;
    }
    const QString description = result.info.value(QStringLiteral("description")).toString();
    const QRegularExpressionMatch skipped = QRegularExpression(QStringLiteral("跳过([0-9]+)组"))
                                                .match(description);
    if (skipped.hasMatch()) {
        const int count = skipped.captured(1).toInt();
        result.info[QStringLiteral("skipped_groups")] = count;
        result.info[QStringLiteral("partial_check")] = count > 0;
    }
    if (!mappingWarnings.isEmpty())
        result.info[QStringLiteral("association_mapping_warnings")] = QJsonArray::fromStringList(mappingWarnings);
}
