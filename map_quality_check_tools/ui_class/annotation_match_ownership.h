#pragma once
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QVector>

// 完整标注实例是不可拆分的归属单元；group_id只在本次任务内有效。
struct AnnotationOwnershipGroup
{
    int task = -1;
    QString instance;
    QSet<qint64> fids;
    bool complete = false;
};

inline QMap<int, QSet<qint64>> planAnnotationOwnership(
    const QVector<AnnotationOwnershipGroup> &groups, QStringList &ambiguities)
{
    QMap<qint64, QSet<int>> fullOwners;
    for (int i = 0; i < groups.size(); ++i)
        if (groups[i].complete)
            for (qint64 fid : groups[i].fids) fullOwners[fid].insert(i);
    QSet<int> unreliable;
    for (auto it = fullOwners.constBegin(); it != fullOwners.constEnd(); ++it)
        if (it.value().size() > 1) {
            unreliable.unite(it.value());
            ambiguities.append(QStringLiteral("原注记FID=%1参与多个完整实例，保留归属冲突供复核").arg(it.key()));
        }
    QMap<qint64, int> reserved;
    for (int i = 0; i < groups.size(); ++i)
        if (groups[i].complete && !unreliable.contains(i))
            for (qint64 fid : groups[i].fids) reserved[fid] = groups[i].task;
    QSet<int> affected;
    for (const auto &group : groups)
        if (!group.complete)
            for (qint64 fid : group.fids)
                if (reserved.contains(fid) && reserved[fid] != group.task) affected.insert(group.task);
    QMap<int, QSet<qint64>> exclusions;
    for (int task : affected)
        for (auto it = reserved.constBegin(); it != reserved.constEnd(); ++it)
            if (it.value() != task) exclusions[task].insert(it.key());
    return exclusions;
}
