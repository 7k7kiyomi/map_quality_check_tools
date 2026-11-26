#pragma once

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace PointIsolationOutput {
inline QString prefix(QString label)
{
    if (label.isEmpty()) label = QStringLiteral("水库");
    for (int i = 0; i < label.size(); ++i)
        if (QStringLiteral("\\/:*?\"<>|").contains(label.at(i))) label[i] = QLatin1Char('_');
    return QStringLiteral("点要素孤立_%1").arg(label);
}

inline QStringList directories(const QString& outputDir)
{
    const QDir root(QDir(outputDir).absoluteFilePath(QStringLiteral("质检错误结果")));
    return {root.absolutePath(), root.filePath(QStringLiteral("高严重程度")),
            root.filePath(QStringLiteral("中严重程度")), root.filePath(QStringLiteral("低严重程度"))};
}

// Only this check's exact output name and numeric archive copies belong to the current run.
inline bool ownsFile(const QString& path, const QString& outputDir, const QString& label)
{
    const QFileInfo file(path);
    bool inOutput = false;
    for (const QString& directory : directories(outputDir))
        if (file.absolutePath().compare(directory, Qt::CaseInsensitive) == 0) inOutput = true;
    if (!inOutput) return false;
    const QRegularExpression name(QStringLiteral("^%1_errors(?:_[0-9]+)?\\.(?:shp|shx|dbf|prj|cpg|qix|sbn|sbx|shp\\.xml)$")
        .arg(QRegularExpression::escape(prefix(label))), QRegularExpression::CaseInsensitiveOption);
    return name.match(file.fileName()).hasMatch();
}

inline QStringList previousFiles(const QString& outputDir, const QString& label)
{
    QStringList files;
    for (const QString& directory : directories(outputDir)) {
        for (const QFileInfo& file : QDir(directory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
            if (ownsFile(file.absoluteFilePath(), outputDir, label)) files << file.absoluteFilePath();
    }
    return files;
}
} // namespace PointIsolationOutput
