#pragma once

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>

namespace SeriesMapConfigurationDeduplication {

inline QString normalizedPathIdentity(const QString& path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty()) return QString();

    const QFileInfo fileInfo(trimmedPath);
    const QString canonicalPath = fileInfo.canonicalFilePath();
    QString identity = QDir::cleanPath(QDir::fromNativeSeparators(
        canonicalPath.isEmpty() ? fileInfo.absoluteFilePath() : canonicalPath));
#if defined(Q_OS_WIN)
    identity = identity.toCaseFolded();
#endif
    return identity;
}

inline QStringList normalizedPathSet(const QStringList& paths)
{
    QStringList identities;
    for (const QString& path : paths) {
        const QString identity = normalizedPathIdentity(path);
        if (!identity.isEmpty() && !identities.contains(identity)) {
            identities << identity;
        }
    }
    std::sort(identities.begin(), identities.end());
    return identities;
}

inline QString mapGroupIdentityKey(const QString& mainPath, const QStringList& associatedPaths)
{
    const QString mainIdentity = normalizedPathIdentity(mainPath);
    if (mainIdentity.isEmpty()) return QString();

    QString key;
    const auto appendSegment = [&key](const QString& segment) {
        key += QString::number(segment.size());
        key += QLatin1Char(':');
        key += segment;
    };
    appendSegment(mainIdentity);
    for (const QString& associatedIdentity : normalizedPathSet(associatedPaths)) {
        appendSegment(associatedIdentity);
    }
    return key;
}

inline bool sameMapGroup(const QString& mainPathA, const QStringList& associatedPathsA,
    const QString& mainPathB, const QStringList& associatedPathsB)
{
    const QString mainIdentityA = normalizedPathIdentity(mainPathA);
    if (mainIdentityA.isEmpty() || mainIdentityA != normalizedPathIdentity(mainPathB)) {
        return false;
    }
    return normalizedPathSet(associatedPathsA) == normalizedPathSet(associatedPathsB);
}

inline QString displayName(const QString& path)
{
    const QString baseName = QFileInfo(path.trimmed()).completeBaseName();
    return baseName.isEmpty() ? path.trimmed() : baseName;
}

inline QString groupDisplayName(const QString& mainPath, const QStringList& associatedPaths)
{
    QStringList associatedNames;
    for (const QString& identity : normalizedPathSet(associatedPaths)) {
        QString name;
        for (const QString& path : associatedPaths) {
            if (normalizedPathIdentity(path) == identity) {
                name = displayName(path);
                if (name.isEmpty()) name = path.trimmed();
                break;
            }
        }
        if (!name.isEmpty()) associatedNames << name;
    }
    const QString mainName = displayName(mainPath);
    if (associatedNames.isEmpty()) return mainName;
    return QStringLiteral("%1（关联：%2）").arg(mainName, associatedNames.join(QStringLiteral("、")));
}

} // namespace SeriesMapConfigurationDeduplication
