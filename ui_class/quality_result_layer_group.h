#pragma once

#include <QDir>
#include <QFileInfo>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgslayertreeregistrybridge.h>

// Follow the host's folder identity so its result watcher reuses these groups.
inline bool placeQualityResultLayer(QgsProject* project, QgsVectorLayer* layer, const QString& destinationPath)
{
    if (!project || !project->layerTreeRoot() || !layer) return false;
    const QDir destination = QFileInfo(destinationPath).dir();
    QDir errorDir = destination;
    if (errorDir.dirName() != QStringLiteral("质检错误结果") && !errorDir.cdUp()) return false;
    if (errorDir.dirName() != QStringLiteral("质检错误结果")) return false;
    QDir mapDir = errorDir;
    if (!mapDir.cdUp()) return false;

    auto ensureGroup = [](QgsLayerTreeGroup* parent, const QDir& directory) {
        const QString path = QDir::fromNativeSeparators(QDir::cleanPath(directory.absolutePath()));
        QgsLayerTreeGroup* fallback = nullptr;
        for (QgsLayerTreeNode* node : parent->children()) {
            if (!node || node->nodeType() != QgsLayerTreeNode::NodeGroup) continue;
            auto* group = static_cast<QgsLayerTreeGroup*>(node);
            const QString existing = QDir::fromNativeSeparators(QDir::cleanPath(
                group->customProperty(QStringLiteral("ltzk/qualityResultFolderPath")).toString()));
            if (group->customProperty(QStringLiteral("ltzk/qualityResultFolder")).toBool() &&
                existing.compare(path, Qt::CaseInsensitive) == 0) return group;
            if (!group->customProperty(QStringLiteral("ltzk/qualityResultFolder")).toBool() &&
                group->name() == directory.dirName()) fallback = group;
        }
        QgsLayerTreeGroup* group = fallback ? fallback : parent->addGroup(directory.dirName());
        group->setCustomProperty(QStringLiteral("ltzk/qualityResultFolder"), true);
        group->setCustomProperty(QStringLiteral("ltzk/qualityResultFolderPath"), path);
        group->setExpanded(true);
        return group;
    };
    QgsLayerTreeGroup* target = ensureGroup(project->layerTreeRoot(), mapDir);
    target = ensureGroup(target, errorDir);
    if (destination != errorDir) target = ensureGroup(target, destination);

    QgsLayerTreeLayer* node = project->layerTreeRoot()->findLayer(layer->id());
    if (!node) { target->addLayer(layer); return true; }
    auto* parent = dynamic_cast<QgsLayerTreeGroup*>(node->parent());
    if (!parent) return false;
    if (parent == target) return true;
    QgsLayerTreeRegistryBridge* bridge = project->layerTreeRegistryBridge();
    const bool enabled = bridge && bridge->isEnabled();
    if (bridge) bridge->setEnabled(false);
    const bool detached = parent->takeChild(node);
    if (detached) target->addChildNode(node);
    if (bridge) bridge->setEnabled(enabled);
    return detached;
}

// Reconcile actual final files at task completion, including layers already loaded by the host.
inline int ensureQualityResultLayers(QgsProject* project, const QStringList& paths)
{
    if (!project) return 0;
    int available = 0;
    for (const QString& path : paths) {
        const QFileInfo file(path);
        if (!file.isFile()) continue;
        const QString expected = QDir::cleanPath(file.absoluteFilePath());
        QgsVectorLayer* result = nullptr;
        const auto layers = project->mapLayers();
        for (QgsMapLayer* layer : layers) {
            auto* candidate = qobject_cast<QgsVectorLayer*>(layer);
            if (!candidate) continue;
            const QString source = candidate->source().section(QLatin1Char('|'), 0, 0);
            if (QDir::cleanPath(QFileInfo(source).absoluteFilePath()).compare(expected, Qt::CaseInsensitive) == 0) {
                result = candidate;
                break;
            }
        }
        if (result) {
            if (result->isValid() && placeQualityResultLayer(project, result, expected)) ++available;
            continue;
        }
        result = new QgsVectorLayer(expected, file.completeBaseName(), QStringLiteral("ogr"));
        if (!result->isValid()) { delete result; continue; }
        project->addMapLayer(result, false);
        if (placeQualityResultLayer(project, result, expected)) ++available;
        else project->removeMapLayer(result->id());
    }
    return available;
}
