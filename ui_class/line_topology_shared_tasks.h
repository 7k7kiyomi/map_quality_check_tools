#pragma once

#include "algorithm_file_paths.h"
#include "map_check_common.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace LineTopologySharedTasks {

inline QJsonObject elementSignature(const QDomElement& element)
{
    QJsonObject signature;
    signature["tag"] = element.tagName();
    QJsonObject attributes;
    const QDomNamedNodeMap attributeNodes = element.attributes();
    for (int i = 0; i < attributeNodes.size(); ++i) {
        const QDomAttr attribute = attributeNodes.item(i).toAttr();
        if (attribute.name().compare(QStringLiteral("severity"), Qt::CaseInsensitive) == 0) continue;
        attributes[attribute.name()] = attribute.value().trimmed();
    }
    signature["attributes"] = attributes;
    QJsonArray children;
    for (QDomElement child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        // Descriptions and severity affect reporting, not the computation itself.
        if (child.tagName() != QStringLiteral("description")
            && child.tagName().compare(QStringLiteral("severity"), Qt::CaseInsensitive) != 0)
            children.append(elementSignature(child));
    }
    if (children.isEmpty()) signature["text"] = element.text().trimmed();
    else signature["children"] = children;
    return signature;
}

inline QJsonArray inputSignature(const QString& semicolonPaths)
{
    QStringList paths = semicolonPaths.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QStringList signatures;
    for (QString path : paths) {
        path = path.trimmed();
        if (path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"'))) path = path.mid(1, path.size() - 2);
        const QFileInfo file(path);
        if (!file.isFile()) return {};
        QJsonObject signature;
        signature["path"] = QDir::fromNativeSeparators(file.canonicalFilePath()).toCaseFolded();
        QJsonArray parts;
        QStringList files{file.absoluteFilePath()};
        if (file.suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) == 0) {
            for (const QString& suffix : {QStringLiteral("dbf"), QStringLiteral("shx"), QStringLiteral("prj"), QStringLiteral("cpg")})
                files.append(file.dir().filePath(file.completeBaseName() + QLatin1Char('.') + suffix));
        }
        for (const QString& part : files) {
            const QFileInfo info(part);
            QJsonObject item;
            item["exists"] = info.isFile();
            item["size"] = QString::number(info.size());
            item["modified"] = QString::number(info.lastModified().toMSecsSinceEpoch());
            parts.append(item);
        }
        signature["parts"] = parts;
        signatures.append(QString::fromUtf8(QJsonDocument(signature).toJson(QJsonDocument::Compact)));
    }
    signatures.removeDuplicates();
    std::sort(signatures.begin(), signatures.end());
    QJsonArray result;
    for (const QString& signature : signatures) result.append(signature);
    return result;
}

inline QString waterTaskKey(const QString& checkName, const AlgorithmFilePaths& paths,
                            const QString& xmlPath, int scale)
{
    const QString normalizedCheckName = checkName.trimmed();
    const QString checkId = normalizedCheckName == QStringLiteral("线要素拓扑检查")
        ? QStringLiteral("112")
        : normalizedCheckName == QStringLiteral("几何走向异常检查")
            ? QStringLiteral("105") : QString();
    if (checkId.isEmpty() || scale <= 0) return {};
    const QString standard = paths.standardLayer.trimmed();
    const auto isWaterName = [](const QString& value) {
        return value.contains(QStringLiteral("水系")) || value.contains(QStringLiteral("河"))
            || value.contains(QStringLiteral("渠"));
    };
    bool isWaterNetwork = isWaterName(standard);
    for (const QString& path : paths.actualShpSemicolon.split(QLatin1Char(';'), Qt::SkipEmptyParts))
        isWaterNetwork = isWaterNetwork || isWaterName(QFileInfo(path.trimmed()).completeBaseName());
    if (!isWaterNetwork) return {};
    const QJsonArray parsed = inputSignature(paths.actualShpSemicolon);
    const QJsonArray reference = inputSignature(paths.gdbShpSemicolon);
    if (parsed.isEmpty() || reference.isEmpty()) return {};
    QFile xml(xmlPath);
    QDomDocument document;
    if (!xml.open(QIODevice::ReadOnly) || !document.setContent(&xml)) return {};
    QJsonArray rules;
    const QDomNodeList items = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < items.size(); ++i) {
        const QDomElement item = items.at(i).toElement();
        if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != checkId
            || item.firstChildElement(QStringLiteral("name")).text().trimmed() != normalizedCheckName) continue;
        QJsonObject signature;
        signature["scale"] = item.parentNode().toElement().firstChildElement(QStringLiteral("value")).text().trimmed();
        signature["item"] = elementSignature(item);
        rules.append(signature);
    }
    if (rules.isEmpty()) return {};
    QJsonObject request;
    request["check_id"] = checkId;
    request["parsed"] = parsed;
    request["reference"] = reference;
    request["scale"] = scale;
    request["rules"] = rules;
    if (!paths.csvPath.trimmed().isEmpty()) {
        QFile csv(paths.csvPath);
        if (!csv.open(QIODevice::ReadOnly)) return {};
        request["csv"] = QString::fromLatin1(QCryptographicHash::hash(csv.readAll(), QCryptographicHash::Sha256).toHex());
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(request).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

} // namespace LineTopologySharedTasks
