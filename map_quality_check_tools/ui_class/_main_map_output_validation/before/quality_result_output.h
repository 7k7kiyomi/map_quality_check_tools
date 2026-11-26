#pragma once
#include "quality_result_layer_group.h"
#include <QDomDocument>
#include <QFile>
#include <QSet>
#include <QThread>
#include <QMetaObject>
#include <qgsvectordataprovider.h>
#include <functional>

// Shared by task management and the series checks. Call only after SHP/DBF
// writers have closed; use explicit output paths, never scan cache/input dirs.
namespace QualityResultOutput {
inline QString pathKey(const QString& path)
{
    return QDir::fromNativeSeparators(QDir::cleanPath(
        QFileInfo(path.section(QLatin1Char('|'), 0, 0)).absoluteFilePath())).toCaseFolded();
}

inline QString normalizedSeverity(const QString& text)
{
    const QString value = text.trimmed();
    return value == QStringLiteral("高") || value == QStringLiteral("中") || value == QStringLiteral("低")
        ? value : QStringLiteral("未配置");
}

inline QString severityFromXml(const QString& xmlPath, const QString& checkId)
{
    QFile file(xmlPath);
    QDomDocument document;
    if (!file.open(QIODevice::ReadOnly) || !document.setContent(&file)) return QStringLiteral("未配置");
    const auto checks = document.elementsByTagName(QStringLiteral("check_item"));
    for (int i = 0; i < checks.size(); ++i) {
        const auto check = checks.at(i).toElement();
        if (check.firstChildElement(QStringLiteral("id")).text().trimmed() == checkId)
            return normalizedSeverity(check.firstChildElement(QStringLiteral("severity")).text());
    }
    return QStringLiteral("未配置");
}

inline QString directory(const QString& mapRoot, const QString& severity = QString())
{
    const QDir root(QDir(mapRoot).filePath(QStringLiteral("质检错误结果")));
    const QString level = normalizedSeverity(severity);
    return level == QStringLiteral("未配置") ? root.path() : root.filePath(level + QStringLiteral("严重程度"));
}

inline bool ensureDirectory(const QString& errorRoot)
{
    QDir root(errorRoot);
    if (!root.mkpath(QStringLiteral("."))) return false;
    for (const QString& level : {QStringLiteral("高"), QStringLiteral("中"), QStringLiteral("低")})
        if (!root.mkpath(level + QStringLiteral("严重程度"))) return false;
    return true;
}

inline bool onProjectThread(QgsProject* project, const std::function<void()>& operation)
{
    if (!project) return false;
    if (QThread::currentThread() == project->thread()) { operation(); return true; }
    return QMetaObject::invokeMethod(project, operation, Qt::BlockingQueuedConnection);
}

// Removing only this check's explicit result layers releases Windows file locks
// before overwriting them. Inputs and other checks' layers remain untouched.
inline int releaseLayers(QgsProject* project, const QStringList& paths)
{
    QSet<QString> expected;
    for (const QString& path : paths) if (!path.isEmpty()) expected.insert(pathKey(path));
    int removed = 0;
    onProjectThread(project, [&]() {
        QStringList ids;
        for (QgsMapLayer* layer : project->mapLayers()) {
            auto* vector = qobject_cast<QgsVectorLayer*>(layer);
            if (vector && expected.contains(pathKey(vector->source()))) ids << vector->id();
        }
        removed = ids.size();
        project->removeMapLayers(ids);
    });
    return removed;
}

inline int loadLayers(QgsProject* project, const QStringList& paths, QStringList* failedPaths = nullptr)
{
    QStringList failures;
    int available = 0;
    const bool dispatched = onProjectThread(project, [&]() {
        QSet<QString> seen;
        for (const QString& path : paths) {
            if (path.isEmpty() || seen.contains(pathKey(path))) continue;
            seen.insert(pathKey(path));
            if (!QFileInfo(path).isFile()) { failures << path; continue; }
            // The host watcher may have opened a file before the writer closed.
            for (QgsMapLayer* layer : project->mapLayers()) {
                auto* vector = qobject_cast<QgsVectorLayer*>(layer);
                if (!vector || pathKey(vector->source()) != pathKey(path)) continue;
                if (!vector->isValid()) vector->setDataSource(path, vector->name(), QStringLiteral("ogr"), true);
                else if (vector->dataProvider()) {
                    vector->dataProvider()->reloadData();
                    vector->updateFields();
                    vector->updateExtents(true);
                    vector->triggerRepaint();
                }
            }
            if (ensureQualityResultLayers(project, QStringList{path}) == 1) ++available;
            else failures << path;
        }
    });
    if (!dispatched) failures = paths;
    if (failedPaths) *failedPaths = failures;
    return available;
}
} // namespace QualityResultOutput
