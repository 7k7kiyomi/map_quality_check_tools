#pragma once
#include "series_layer_mapping.h"
#include <QTreeWidget>
#include <QMap>
#include <QSet>
#include <functional>

namespace SeriesMappingTreeGroups {
inline bool isMappingRow(const QTreeWidgetItem* item)
{
    const QString type = item->data(0, Qt::UserRole).toString();
    return type == QStringLiteral("MappingRow") || type == QStringLiteral("FamilyCandidateRow");
}

inline QString detailName(const QString& left, const QString& right)
{
    return QFileInfo(left.isEmpty() ? right : left).fileName();
}

inline QList<QTreeWidgetItem*> mappingRows(QTreeWidgetItem* root)
{
    QList<QTreeWidgetItem*> rows;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* node) {
        if (isMappingRow(node)) { rows << node; return; }
        for (int i = 0; i < node->childCount(); ++i) visit(node->child(i));
    };
    visit(root);
    return rows;
}

// Build display-only groups before attaching cell widgets. The checked rows and
// their path/target roles are left intact, so grouping never merges relations.
inline void groupTask(QTreeWidgetItem* task, const QStringList& targetOrder,
                      int labelColumn, int leftColumn, int rightColumn, int statusColumn,
                      int leftPathRole, int rightPathRole)
{
    QList<QTreeWidgetItem*> rows;
    std::function<void(QTreeWidgetItem*)> detach = [&](QTreeWidgetItem* node) {
        if (isMappingRow(node)) { rows << node; return; }
        for (QTreeWidgetItem* child : node->takeChildren()) detach(child);
        delete node;
    };
    for (QTreeWidgetItem* node : task->takeChildren()) detach(node);

    QStringList order = targetOrder;
    QMap<QString, QList<QTreeWidgetItem*>> categories;
    for (QTreeWidgetItem* row : rows) {
        const QString target = row->data(0, Qt::UserRole + 2).toString();
        categories[target] << row;
        if (!order.contains(target)) order << target;
    }
    for (const QString& target : order) {
        if (!categories.contains(target)) continue;
        auto* category = new QTreeWidgetItem(task);
        category->setData(0, Qt::UserRole, QStringLiteral("CategoryGroupRow"));
        category->setData(0, Qt::UserRole + 1, task->data(0, Qt::UserRole + 1));
        category->setData(0, Qt::UserRole + 2, target);
        category->setText(labelColumn, target);
        category->setText(statusColumn, QStringLiteral("检查类别"));
        QSet<QString> leftPaths, rightPaths;
        QMap<QString, QList<QTreeWidgetItem*>> names;
        QStringList nameOrder;
        for (QTreeWidgetItem* row : categories.value(target)) {
            const QString left = row->data(0, leftPathRole).toString();
            const QString right = row->data(0, rightPathRole).toString();
            if (!left.isEmpty()) leftPaths.insert(left);
            if (!right.isEmpty()) rightPaths.insert(right);
            const QString path = left.isEmpty() ? right : left;
            QString name = SeriesLayerMapping::layerBaseName(path);
            if (name.isEmpty()) name = target;
            const QString key = name.toCaseFolded();
            if (!names.contains(key)) nameOrder << key;
            names[key] << row;
            const QString fullName = detailName(left, right);
            row->setText(labelColumn, fullName.isEmpty() ? QStringLiteral("未选择图层") : fullName);
            row->setToolTip(labelColumn, fullName);
        }
        category->setText(leftColumn, QStringLiteral("%1 个图层").arg(leftPaths.size()));
        category->setText(rightColumn, QStringLiteral("%1 个图层").arg(rightPaths.size()));
        for (const QString& key : nameOrder) {
            const auto entries = names.value(key);
            QTreeWidgetItem* parent = category;
            if (entries.size() > 1) {
                parent = new QTreeWidgetItem(category);
                parent->setData(0, Qt::UserRole, QStringLiteral("LayerNameGroupRow"));
                parent->setData(0, Qt::UserRole + 1, task->data(0, Qt::UserRole + 1));
                parent->setData(0, Qt::UserRole + 2, target);
                const QString left = entries.first()->data(0, leftPathRole).toString();
                const QString right = entries.first()->data(0, rightPathRole).toString();
                parent->setText(labelColumn, SeriesLayerMapping::layerBaseName(left.isEmpty() ? right : left));
                parent->setText(statusColumn, QStringLiteral("%1 条对应，展开查看").arg(entries.size()));
                parent->setExpanded(false);
            }
            for (QTreeWidgetItem* row : entries) parent->addChild(row);
        }
        category->setExpanded(true);
    }
}
} // namespace SeriesMappingTreeGroups
