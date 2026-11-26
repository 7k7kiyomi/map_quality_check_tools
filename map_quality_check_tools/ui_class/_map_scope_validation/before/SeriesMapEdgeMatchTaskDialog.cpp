#include "SeriesMapEdgeMatchTaskDialog.h"
#include "map_check_algorithms.h"
#include "series_layer_mapping.h"
#include "searchable_layer_combo_box.h"
#include "series_source_audit.h"
#include "map_check_task_manager.h"
#include "QualityCheckConfig.h"
#include "SeriesSheetOverview.h"
#include "SeriesMapConfigurationDeduplication.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QComboBox>
#include <QDateTime>
#include <QTextStream>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include <QMessageBox>
#include <QGroupBox>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <qscopeguard.h>
#include <QListWidget>
#include <QSet>
#include <QAbstractItemView>
#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariant>
#include <QJsonArray>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QUuid>
#include <QDialogButtonBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTreeWidget>
#include <QHeaderView>
#include <QFontInfo>
#include <QFontMetrics>
#include <QCoreApplication>
#include <QEvent>
#include <QDirIterator>
#include <algorithm>
#include <cmath>
#include <functional>
#include <exception>
#include <limits>
#include <qgisinterface.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsfields.h>
#include <qgspointxy.h>
#include <qgsvectorfilewriter.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgsunittypes.h>
#include <qgsvectorlayer.h>
#include <qgswkbtypes.h>

namespace {
constexpr int SeriesColMainMap = 0;
constexpr int SeriesColMainScale = 1;
constexpr int SeriesColAssocMap = 2;
constexpr int SeriesColAssocScale = 3;
constexpr int SeriesColRelationType = 4;
constexpr int SeriesColStatus = 5;
constexpr int SeriesTaskKeyRole = Qt::UserRole + 20;
constexpr int SeriesMainNameRole = Qt::UserRole + 21;
constexpr int SeriesAssocNameRole = Qt::UserRole + 22;

QString seriesTaskKey(const QString& leftName, int leftScale, const QString& rightName, int rightScale)
{
    QStringList ends{leftName.toCaseFolded() + QLatin1Char(':') + QString::number(leftScale),
                     rightName.toCaseFolded() + QLatin1Char(':') + QString::number(rightScale)};
    ends.sort();
    return ends.join(QChar(0x1f));
}

bool backupReportWorkingFile(const QString& filePath, const QString& backupPath, bool* hadOriginal)
{
    const bool exists = QFile::exists(filePath);
    if (hadOriginal) *hadOriginal = exists;
    if (!exists) return true;

    QFile::remove(backupPath);
    return QFile::copy(filePath, backupPath);
}

void restoreReportWorkingFile(const QString& filePath, const QString& backupPath, bool hadOriginal)
{
    QFile::remove(filePath);
    if (hadOriginal) QFile::copy(backupPath, filePath);
}
QString seriesReportMapNameFromPath(const QString& sourceJsonPath)
{
    QString mapName = QFileInfo(sourceJsonPath).completeBaseName();
    mapName.remove(QRegularExpression(QStringLiteral("_result_\\d+$")));
    return mapName.isEmpty() ? QStringLiteral("未命名地图") : mapName;
}

bool seriesReportJsonValueToBool(const QJsonValue& value)
{
    if (value.isBool()) return value.toBool();
    const QString text = value.toString().trimmed().toLower();
    return text == QStringLiteral("true") || text == QStringLiteral("1") || text == QStringLiteral("yes");
}

QString seriesReportCheckItemDetailText(const QJsonObject& checkItem)
{
    QString checkName = checkItem.value(QStringLiteral("name")).toString().trimmed();
    if (checkName.isEmpty()) checkName = QStringLiteral("图幅接边与系列化表达");

    const QJsonObject errors = checkItem.value(QStringLiteral("errors")).toObject();
    if (errors.isEmpty()) return QStringLiteral("未发现%1错误。").arg(checkName);

    QString example;
    for (auto it = errors.constBegin(); it != errors.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject err = it.value().toObject();
        example = err.value(QStringLiteral("description")).toString().trimmed();
        if (example.isEmpty()) example = err.value(QStringLiteral("summary")).toString().trimmed();
        if (!example.isEmpty()) break;
    }
    if (example.isEmpty()) example = QStringLiteral("存在错误但未给出详情");

    return QStringLiteral("发现%1个%2错误，例如：%3").arg(errors.size()).arg(checkName).arg(example);
}

void normalizeSeriesReportCheckItemInfo(QJsonObject& checkItem)
{
    if (!checkItem.contains(QStringLiteral("errors"))) return;

    QJsonObject info = checkItem.value(QStringLiteral("info")).toObject();
    const QString description = info.value(QStringLiteral("description")).toString().trimmed();
    const QString summary = info.value(QStringLiteral("summary")).toString().trimmed();
    if (!description.isEmpty() && !summary.isEmpty()) return;

    const QString detailText = seriesReportCheckItemDetailText(checkItem);
    if (description.isEmpty()) info[QStringLiteral("description")] = detailText;
    if (summary.isEmpty()) info[QStringLiteral("summary")] = detailText;
    checkItem[QStringLiteral("info")] = info;
}

void normalizeSeriesReportTaskNode(QJsonObject& taskNode)
{
    for (auto it = taskNode.begin(); it != taskNode.end(); ++it) {
        if (!it.key().startsWith(QStringLiteral("check_item")) || !it.value().isObject()) continue;
        QJsonObject checkItem = it.value().toObject();
        normalizeSeriesReportCheckItemInfo(checkItem);
        it.value() = checkItem;
    }
}

void normalizeSeriesReportGeopdfNode(QJsonObject& geopdfNode)
{
    QJsonObject tasks = geopdfNode.value(QStringLiteral("tasks")).toObject();
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        if (!it.key().startsWith(QStringLiteral("task")) || !it.value().isObject()) continue;
        QJsonObject taskNode = it.value().toObject();
        normalizeSeriesReportTaskNode(taskNode);
        it.value() = taskNode;
    }
    geopdfNode[QStringLiteral("tasks")] = tasks;
}
QString seriesReportLayerNameFromCheckItem(const QJsonObject& checkItem)
{
    const QJsonObject errors = checkItem.value(QStringLiteral("errors")).toObject();
    for (auto it = errors.constBegin(); it != errors.constEnd(); ++it) {
        if (!it.value().isObject()) continue;
        const QJsonObject err = it.value().toObject();
        const QString standardLayer = err.value(QStringLiteral("standard_layer")).toString().trimmed();
        if (!standardLayer.isEmpty()) return standardLayer;
        const QString layer = err.value(QStringLiteral("layer")).toString().trimmed();
        if (!layer.isEmpty()) return layer;
        const QString leftStandardLayer = err.value(QStringLiteral("left_standard_layer")).toString().trimmed();
        if (!leftStandardLayer.isEmpty()) return leftStandardLayer;
    }
    return QStringLiteral("图幅接边与系列化表达");
}

void appendSeriesReportSourceJson(const QJsonObject& sourceRoot, const QString& sourceJsonPath, QJsonObject& mergedRoot, int& geopdfCount)
{
    if (mergedRoot.isEmpty() && sourceRoot.contains(QStringLiteral("timing"))) {
        mergedRoot[QStringLiteral("timing")] = sourceRoot.value(QStringLiteral("timing"));
    }

    bool hasGeopdfNode = false;
    for (auto it = sourceRoot.constBegin(); it != sourceRoot.constEnd(); ++it) {
        if (it.key().startsWith(QStringLiteral("Geopdf")) && it.value().isObject()) {
            QJsonObject geopdfNode = it.value().toObject();
            normalizeSeriesReportGeopdfNode(geopdfNode);
            mergedRoot[geopdfCount++ == 0 ? QStringLiteral("Geopdf") : QStringLiteral("Geopdf%1").arg(geopdfCount)] = geopdfNode;
            hasGeopdfNode = true;
        }
    }
    if (hasGeopdfNode) return;

    if (sourceRoot.contains(QStringLiteral("tasks"))) {
        QJsonObject geopdfNode = sourceRoot;
        if (geopdfNode.value(QStringLiteral("name")).toString().trimmed().isEmpty()) {
            geopdfNode[QStringLiteral("name")] = seriesReportMapNameFromPath(sourceJsonPath);
        }
        normalizeSeriesReportGeopdfNode(geopdfNode);
        mergedRoot[geopdfCount++ == 0 ? QStringLiteral("Geopdf") : QStringLiteral("Geopdf%1").arg(geopdfCount)] = geopdfNode;
        return;
    }

    QJsonObject taskNode;
    int checkItemCount = 0;
    int errItemCount = 0;
    QString layerName;
    for (auto it = sourceRoot.constBegin(); it != sourceRoot.constEnd(); ++it) {
        if (!it.key().startsWith(QStringLiteral("check_item")) || !it.value().isObject()) continue;
        const QJsonObject checkItem = it.value().toObject();
        taskNode[QStringLiteral("check_item%1").arg(++checkItemCount)] = checkItem;
        if (seriesReportJsonValueToBool(checkItem.value(QStringLiteral("err_exist")))) ++errItemCount;
        if (layerName.isEmpty()) layerName = seriesReportLayerNameFromCheckItem(checkItem);
    }
    if (checkItemCount == 0 || sourceRoot.value(QStringLiteral("check_item1")).isUndefined()) return;

    taskNode[QStringLiteral("layer_name")] = layerName.isEmpty() ? QStringLiteral("图幅接边与系列化表达") : layerName;

    QJsonObject info;
    info[QStringLiteral("tasks_num")] = 1;
    info[QStringLiteral("completed_num")] = 1;
    info[QStringLiteral("err_num")] = errItemCount > 0 ? 1 : 0;
    info[QStringLiteral("uncompleted_num")] = 0;

    QJsonObject tasks;
    tasks[QStringLiteral("task1")] = taskNode;

    QJsonObject geopdfNode;
    geopdfNode[QStringLiteral("name")] = seriesReportMapNameFromPath(sourceJsonPath);
    geopdfNode[QStringLiteral("info")] = info;
    geopdfNode[QStringLiteral("tasks")] = tasks;
    normalizeSeriesReportGeopdfNode(geopdfNode);
    mergedRoot[geopdfCount++ == 0 ? QStringLiteral("Geopdf") : QStringLiteral("Geopdf%1").arg(geopdfCount)] = geopdfNode;
}

QStringList splitSeriesTableList(const QString& text)
{
    QStringList values = text.split(QRegularExpression(QStringLiteral("[;；、\\n\\r\\t]+")), Qt::SkipEmptyParts);
    for (QString& value : values) value = value.trimmed();
    values.removeAll(QString());
    return values;
}

QString normalizeSeriesLayerAliasTextForDialog(QString text)
{
    text = SeriesLayerMapping::layerBaseName(text);
    text.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）\\[\\]【】]+")));
    return text.toLower();
}

QString normalizeSeriesLayerFileBaseForDialog(const QString& text)
{
    return SeriesLayerMapping::layerBaseName(text);
}

QString seriesLayerGeometryKindFromSuffixForDialog(const QString& text)
{
    const QString base = QFileInfo(text.trimmed()).completeBaseName();
    if (base.endsWith(QStringLiteral("_LineStrings"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiLineStrings"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_LineString"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiLineString"), Qt::CaseInsensitive)) {
        return QStringLiteral("line");
    }
    if (base.endsWith(QStringLiteral("_Polygons"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiPolygons"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_Polygon"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiPolygon"), Qt::CaseInsensitive)) {
        return QStringLiteral("polygon");
    }
    if (base.endsWith(QStringLiteral("_Points"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiPoints"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_Point"), Qt::CaseInsensitive) ||
        base.endsWith(QStringLiteral("_MultiPoint"), Qt::CaseInsensitive)) {
        return QStringLiteral("point");
    }
    return QString();
}

QString seriesLayerGeometryKindForDialog(const QString& shpPath)
{
    const QString suffixKind = seriesLayerGeometryKindFromSuffixForDialog(shpPath);
    const QFileInfo info(shpPath);
    if (!info.isFile()) return suffixKind;

    static QMap<QString, QString> geometryKindCache;
    const QString cacheKey = info.absoluteFilePath();
    if (geometryKindCache.contains(cacheKey)) return geometryKindCache.value(cacheKey);

    QString geometryKind;
    QgsVectorLayer layer(info.absoluteFilePath(), info.completeBaseName(), QStringLiteral("ogr"));
    if (layer.isValid()) {
        const QgsWkbTypes::GeometryType geometryType = QgsWkbTypes::geometryType(layer.wkbType());
        if (geometryType == QgsWkbTypes::LineGeometry) geometryKind = QStringLiteral("line");
        else if (geometryType == QgsWkbTypes::PolygonGeometry) geometryKind = QStringLiteral("polygon");
        else if (geometryType == QgsWkbTypes::PointGeometry) geometryKind = QStringLiteral("point");
    }
    geometryKindCache.insert(cacheKey, geometryKind);
    return geometryKind;
}

bool seriesLayerGeometryCompatibleForDialog(const QString& leftPath, const QString& rightPath)
{
    const QString leftKind = seriesLayerGeometryKindForDialog(leftPath);
    const QString rightKind = seriesLayerGeometryKindForDialog(rightPath);
    return leftKind.isEmpty() || rightKind.isEmpty() || leftKind == rightKind;
}

void addSeriesLayerAliasGroupForDialog(QMap<QString, QString>& aliases, const QString& category, const QStringList& names)
{
    const QString categoryText = normalizeSeriesLayerFileBaseForDialog(category);
    if (categoryText.isEmpty()) return;
    QStringList allNames = names;
    allNames << categoryText;
    for (const QString& name : allNames) {
        const QString key = normalizeSeriesLayerAliasTextForDialog(name);
        if (!key.isEmpty()) aliases[key] = categoryText;
    }
}

QMap<QString, QString> parseSeriesLayerNameAliasesForDialog(const QString& text)
{
    QMap<QString, QString> aliases;
    const QStringList groups = text.split(QRegularExpression(QStringLiteral("[;；\\n\\r]+")), Qt::SkipEmptyParts);
    for (QString group : groups) {
        group = group.trimmed();
        if (group.isEmpty()) continue;
        const int equalPos = group.indexOf(QRegularExpression(QStringLiteral("[=＝:]")));
        if (equalPos < 0) continue;
        const QString category = group.left(equalPos).trimmed();
        const QString values = group.mid(equalPos + 1).trimmed();
        const QStringList names = values.split(QRegularExpression(QStringLiteral("[|,，、]+")), Qt::SkipEmptyParts);
        addSeriesLayerAliasGroupForDialog(aliases, category, names);
    }
    return aliases;
}

QString seriesLayerCategoryKeyForDialog(const QString& shpName, const QMap<QString, QString>& aliases)
{
    const QString fallback = normalizeSeriesLayerFileBaseForDialog(shpName);
    const QString key = normalizeSeriesLayerAliasTextForDialog(shpName);
    if (!key.isEmpty() && aliases.contains(key)) return aliases.value(key);
    return fallback;
}

QString seriesLayerAnnotationClassForDialog(const QString& shpName)
{
    const QString base = normalizeSeriesLayerFileBaseForDialog(shpName);
    QString compact = base;
    compact.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）\\[\\]【】]+")));

    QString annotationType;
    if (compact.endsWith(QStringLiteral("代码标注"), Qt::CaseInsensitive)) annotationType = QStringLiteral("代码标注");
    else if (compact.endsWith(QStringLiteral("名称标注"), Qt::CaseInsensitive)) annotationType = QStringLiteral("名称标注");
    else if (compact.endsWith(QStringLiteral("标注"), Qt::CaseInsensitive)) annotationType = QStringLiteral("标注");
    else if (compact.endsWith(QStringLiteral("注记"), Qt::CaseInsensitive)) annotationType = QStringLiteral("注记");
    if (annotationType.isEmpty()) return QString();

    QString subject = compact;
    subject.chop(annotationType.size());
    auto subjectHas = [&](const QString& token) { return subject.contains(token, Qt::CaseInsensitive); };

    if (subjectHas(QStringLiteral("道路")) || subjectHas(QStringLiteral("公路")) ||
        subjectHas(QStringLiteral("国道")) || subjectHas(QStringLiteral("省道")) ||
        subjectHas(QStringLiteral("县道")) || subjectHas(QStringLiteral("乡道")) ||
        subjectHas(QStringLiteral("村道")) || subjectHas(QStringLiteral("连接道")) ||
        subjectHas(QStringLiteral("快速路"))) {
        if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("road_annotation:代码标注");
        if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("road_annotation:名称标注");
        if (annotationType == QStringLiteral("注记")) return QStringLiteral("road_annotation:注记");
        return QStringLiteral("road_annotation:标注");
    }
    if (subjectHas(QStringLiteral("水系")) || subjectHas(QStringLiteral("河流")) ||
        subjectHas(QStringLiteral("时令河")) || subjectHas(QStringLiteral("沟渠")) ||
        subjectHas(QStringLiteral("干渠")) || subjectHas(QStringLiteral("支渠"))) {
        if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("hydrology_annotation:代码标注");
        if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("hydrology_annotation:名称标注");
        if (annotationType == QStringLiteral("注记")) return QStringLiteral("hydrology_annotation:注记");
        return QStringLiteral("hydrology_annotation:标注");
    }
    if (subjectHas(QStringLiteral("境界线")) || subjectHas(QStringLiteral("行政区界线")) ||
        subjectHas(QStringLiteral("国界线"))) {
        if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("boundary_annotation:代码标注");
        if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("boundary_annotation:名称标注");
        if (annotationType == QStringLiteral("注记")) return QStringLiteral("boundary_annotation:注记");
        return QStringLiteral("boundary_annotation:标注");
    }
    if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("annotation:代码标注");
    if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("annotation:名称标注");
    if (annotationType == QStringLiteral("注记")) return QStringLiteral("annotation:注记");
    return QStringLiteral("annotation:标注");
}

QString seriesLayerFamilyForDialog(const QString& shpName, const QMap<QString, QString>& aliases)
{
    const QString base = normalizeSeriesLayerFileBaseForDialog(shpName);
    const QString category = seriesLayerCategoryKeyForDialog(shpName, aliases);
    const QString text = base + QStringLiteral("|") + category;
    auto has = [&](const QString& token) { return text.contains(token, Qt::CaseInsensitive); };

    const QString annotationClass = seriesLayerAnnotationClassForDialog(shpName);
    if (!annotationClass.isEmpty()) return annotationClass.section(QLatin1Char(':'), 0, 0);

    if (has(QStringLiteral("公路隧道")) || has(QStringLiteral("公路隧道口"))) {
        return QStringLiteral("road_facility");
    }
    if (has(QStringLiteral("水系面")) || has(QStringLiteral("河流面")) || has(QStringLiteral("水域面")) ||
        has(QStringLiteral("水域")) || has(QStringLiteral("湖泊")) || has(QStringLiteral("坑塘")) ||
        (has(QStringLiteral("水库")) && !has(QStringLiteral("不依比例水库")))) {
        return QStringLiteral("hydrology_area");
    }
    if (has(QStringLiteral("道路")) || has(QStringLiteral("公路")) || has(QStringLiteral("国道")) ||
        has(QStringLiteral("省道")) || has(QStringLiteral("县道")) || has(QStringLiteral("乡道")) ||
        has(QStringLiteral("村道")) || has(QStringLiteral("连接道")) || has(QStringLiteral("快速路"))) {
        return QStringLiteral("road");
    }
    if (has(QStringLiteral("水系")) || has(QStringLiteral("河流")) || has(QStringLiteral("时令河")) ||
        has(QStringLiteral("沟渠")) || has(QStringLiteral("干渠")) || has(QStringLiteral("支渠"))) {
        return QStringLiteral("hydrology");
    }
    if (has(QStringLiteral("境界线")) || has(QStringLiteral("行政区界线")) || has(QStringLiteral("国界线"))) {
        return QStringLiteral("boundary");
    }
    if (has(QStringLiteral("行政区划面")) || has(QStringLiteral("政区面"))) {
        return QStringLiteral("admin_area");
    }
    return QString();
}

QString seriesLayerBusinessClassForDialog(const QString& shpName, const QMap<QString, QString>& aliases)
{
    const QString base = normalizeSeriesLayerFileBaseForDialog(shpName);
    const QString category = seriesLayerCategoryKeyForDialog(shpName, aliases);
    const QString text = base + QStringLiteral("|") + category;
    auto has = [&](const QString& token) { return text.contains(token, Qt::CaseInsensitive); };

    const QString annotationClass = seriesLayerAnnotationClassForDialog(shpName);
    if (!annotationClass.isEmpty()) return annotationClass;

    if (has(QStringLiteral("公路隧道口"))) return QStringLiteral("road_facility:公路隧道口");
    if (has(QStringLiteral("公路隧道"))) return QStringLiteral("road_facility:公路隧道");

    if (has(QStringLiteral("河流面"))) return QStringLiteral("hydrology_area:河流面");
    if (has(QStringLiteral("水域面")) || has(QStringLiteral("水域"))) return QStringLiteral("hydrology_area:水域");
    if (has(QStringLiteral("湖泊"))) return QStringLiteral("hydrology_area:湖泊");
    if (has(QStringLiteral("坑塘"))) return QStringLiteral("hydrology_area:坑塘");
    if (has(QStringLiteral("水库")) && !has(QStringLiteral("不依比例水库"))) return QStringLiteral("hydrology_area:水库");
    if (has(QStringLiteral("水系面"))) return QStringLiteral("hydrology_area:水系面");

    if (has(QStringLiteral("高速公路"))) return QStringLiteral("road:高速公路");
    if (has(QStringLiteral("快速路"))) return QStringLiteral("road:快速路");
    if (has(QStringLiteral("国道"))) return QStringLiteral("road:国道");
    if (has(QStringLiteral("省道"))) return QStringLiteral("road:省道");
    if (has(QStringLiteral("县道"))) return QStringLiteral("road:县道");
    if (has(QStringLiteral("乡道"))) return QStringLiteral("road:乡道");
    if (has(QStringLiteral("村道"))) return QStringLiteral("road:村道");
    if (has(QStringLiteral("专用公路"))) return QStringLiteral("road:专用公路");
    if (has(QStringLiteral("连接道"))) return QStringLiteral("road:连接道");

    if (has(QStringLiteral("乡镇级行政区界线"))) return QStringLiteral("boundary:乡镇级行政区界线");
    if (has(QStringLiteral("县级行政区界线"))) return QStringLiteral("boundary:县级行政区界线");
    if (has(QStringLiteral("市级行政区界线"))) return QStringLiteral("boundary:市级行政区界线");
    if (has(QStringLiteral("省级行政区界线"))) return QStringLiteral("boundary:省级行政区界线");
    if (has(QStringLiteral("国界线"))) return QStringLiteral("boundary:国界线");

    if (has(QStringLiteral("总干渠"))) return QStringLiteral("hydrology:总干渠");
    if (has(QStringLiteral("干渠"))) return QStringLiteral("hydrology:干渠");
    if (has(QStringLiteral("支渠"))) return QStringLiteral("hydrology:支渠");
    if (has(QStringLiteral("常年河"))) return QStringLiteral("hydrology:常年河");
    if (has(QStringLiteral("干涸河"))) return QStringLiteral("hydrology:干涸河");
    if (has(QStringLiteral("一级河流"))) return QStringLiteral("hydrology:一级河流");
    if (has(QStringLiteral("二级河流"))) return QStringLiteral("hydrology:二级河流");
    if (has(QStringLiteral("三级河流"))) return QStringLiteral("hydrology:三级河流");
    if (has(QStringLiteral("四级河流"))) return QStringLiteral("hydrology:四级河流");
    if (has(QStringLiteral("五级河流"))) return QStringLiteral("hydrology:五级河流");
    if (has(QStringLiteral("六级河流"))) return QStringLiteral("hydrology:六级河流");
    if (has(QStringLiteral("时令河"))) return QStringLiteral("hydrology:时令河");
    if (has(QStringLiteral("等外河流"))) return QStringLiteral("hydrology:等外河流");
    if (has(QStringLiteral("河流"))) return QStringLiteral("hydrology:河流");
    if (has(QStringLiteral("沟渠"))) return QStringLiteral("hydrology:沟渠");
    return QString();
}

bool seriesLayerPairDefaultCheckedForDialog(const QString& leftShpName, const QString& rightShpName,
                                            const QMap<QString, QString>& aliases, QString* statusText = nullptr)
{
    const QString leftFamily = seriesLayerFamilyForDialog(leftShpName, aliases);
    const QString rightFamily = seriesLayerFamilyForDialog(rightShpName, aliases);
    if (leftFamily.isEmpty() || rightFamily.isEmpty() || leftFamily != rightFamily) {
        if (statusText) *statusText = QStringLiteral("族群不一致，需确认");
        return false;
    }

    const QString leftClass = seriesLayerBusinessClassForDialog(leftShpName, aliases);
    const QString rightClass = seriesLayerBusinessClassForDialog(rightShpName, aliases);
    if (leftClass.isEmpty() || rightClass.isEmpty()) {
        if (statusText) *statusText = QStringLiteral("跨比例尺泛化候选");
        return true;
    }
    const QString leftDomain = leftClass.section(QLatin1Char(':'), 0, 0);
    const QString rightDomain = rightClass.section(QLatin1Char(':'), 0, 0);
    if (leftDomain != rightDomain) {
        if (statusText) *statusText = QStringLiteral("业务类型不一致，需确认");
        return false;
    }
    if (leftClass == rightClass) {
        if (statusText) *statusText = QStringLiteral("跨比例尺等级匹配");
        return true;
    }

    if (statusText) *statusText = QStringLiteral("等级不一致，需确认");
    return false;
}

// Only exact names, normalized names and configured aliases can establish a
// default relation. Substring matching is reserved for the search UI.
int seriesLayerNameMatchScoreForDialog(const QString& left, const QString& right,
                                      const QMap<QString, QString>& aliases)
{
    if (QFileInfo(left).fileName().compare(QFileInfo(right).fileName(), Qt::CaseInsensitive) == 0) return 3;
    if (normalizeSeriesLayerAliasTextForDialog(left) == normalizeSeriesLayerAliasTextForDialog(right)) return 2;
    const QString leftClass = seriesLayerBusinessClassForDialog(left, aliases);
    const QString rightClass = seriesLayerBusinessClassForDialog(right, aliases);
    if (!leftClass.isEmpty() && !rightClass.isEmpty() && leftClass != rightClass) return 0;
    return seriesLayerCategoryKeyForDialog(left, aliases).compare(
        seriesLayerCategoryKeyForDialog(right, aliases), Qt::CaseInsensitive) == 0 ? 1 : 0;
}

QStringList collectSeriesShpFilesForDialog(const QString& shpRootDir)
{
    QStringList paths;
    const QStringList dirs = shpRootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) continue;
        const QStringList shpFiles = dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files, QDir::Name);
        for (const QString& fileName : shpFiles) {
            if (QFileInfo(fileName).completeBaseName() == QStringLiteral("内图廓")) continue;
            const QString path = dir.filePath(fileName);
            if (!paths.contains(path)) paths << path;
        }
    }
    return paths;
}

QStringList collectSeriesShpFilesByAliasForDialog(const QString& category, const QMap<QString, QString>& aliases, const QString& shpRootDir)
{
    QStringList paths;
    const QString categoryKey = seriesLayerCategoryKeyForDialog(category, aliases);
    for (const QString& path : collectSeriesShpFilesForDialog(shpRootDir)) {
        const QString pathCategory = seriesLayerCategoryKeyForDialog(QFileInfo(path).fileName(), aliases);
        if (pathCategory.compare(categoryKey, Qt::CaseInsensitive) == 0 && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}

QStringList collectSeriesShpFilesByFamilyForDialog(const QString& category, const QMap<QString, QString>& aliases, const QString& shpRootDir)
{
    const QString targetFamily = seriesLayerFamilyForDialog(category, aliases);
    if (targetFamily.isEmpty()) return collectSeriesShpFilesByAliasForDialog(category, aliases, shpRootDir);

    QStringList paths;
    for (const QString& path : collectSeriesShpFilesForDialog(shpRootDir)) {
        const QString pathFamily = seriesLayerFamilyForDialog(QFileInfo(path).fileName(), aliases);
        if (pathFamily == targetFamily && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}
}

static QString seriesDebugLogPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).filePath(QStringLiteral("501_debug.log"));
}

static void appendSeriesDebugLog(const QString& message)
{
    QFile file(seriesDebugLogPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz ")) << message << "\n";
}

struct SeriesOutputPaths
{
    QString rootDir;
    QString resultDir;
    QString logDir;
    QString jsonDir;
    QString jsonPath;
    QString pointShpPath;
    QString lineShpPath;
    QString polygonShpPath;
    QString linkShpPath;
    QString logPath;
};

static SeriesOutputPaths buildSeriesOutputPaths(const QString& rootDir, const QString& layerName = QString())
{
    SeriesOutputPaths paths;
    paths.rootDir = rootDir;
    QDir root(rootDir);
    paths.resultDir = root.filePath(QStringLiteral("质检错误结果"));
    paths.logDir = root.filePath(QStringLiteral("质检日志"));
    paths.jsonDir = root.filePath(QStringLiteral("质检Json文件"));

    QString baseName = QStringLiteral("图幅接边与系列化表达");
    if (!layerName.isEmpty()) {
        baseName += QStringLiteral("_") + layerName;
    }

    paths.jsonPath = QDir(paths.jsonDir).filePath(baseName + QStringLiteral(".json"));
    paths.pointShpPath = QDir(paths.resultDir).filePath(baseName + QStringLiteral("_error_points.shp"));
    paths.lineShpPath = QDir(paths.resultDir).filePath(baseName + QStringLiteral("_error_lines.shp"));
    paths.polygonShpPath = QDir(paths.resultDir).filePath(baseName + QStringLiteral("_error_polygons.shp"));
    paths.linkShpPath = QDir(paths.resultDir).filePath(baseName + QStringLiteral("_error_links.shp"));
    paths.logPath = QDir(paths.logDir).filePath(baseName + QStringLiteral(".log"));
    return paths;
}

static bool ensureSeriesOutputDirs(const SeriesOutputPaths& paths)
{
    return QDir().mkpath(paths.rootDir)
        && QDir().mkpath(paths.resultDir)
        && QDir().mkpath(paths.logDir)
        && QDir().mkpath(paths.jsonDir);
}

static QJsonArray seriesRecordsToArray(const QVector<QJsonObject>& records)
{
    QJsonArray array;
    for (const QJsonObject& record : records) array.append(record);
    return array;
}

static bool writeSeriesJsonFile(const QString& path, const QJsonDocument& doc, QString* failedPath = nullptr)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (failedPath) *failedPath = path;
        return false;
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

static bool saveSeriesStageCacheFiles(const QString& rootDir,
    const QVector<QJsonObject>& autoConfirmedRecords,
    const QVector<QJsonObject>& originalErrorRecords,
    QString* failedPath = nullptr)
{
    const QString cacheDir = QDir(rootDir).filePath(QStringLiteral(".series_cache"));
    if (!QDir().mkpath(cacheDir)) {
        if (failedPath) *failedPath = cacheDir;
        return false;
    }

    auto writeDoc = [&](const QString& fileName, const QJsonDocument& doc) {
        return writeSeriesJsonFile(QDir(cacheDir).filePath(fileName), doc, failedPath);
    };

    return writeDoc(QStringLiteral("501_auto_confirmed.json"), QJsonDocument(seriesRecordsToArray(autoConfirmedRecords)))
        && writeDoc(QStringLiteral("501_original_errors.json"), QJsonDocument(seriesRecordsToArray(originalErrorRecords)));
}

static QString findPhaseOneResultJson(const QString& rootDir)
{
    QDir dir(rootDir);
    const QStringList files = dir.entryList(QStringList() << QStringLiteral("*_result_*.json"), QDir::Files, QDir::Time);
    for (const QString& name : files) {
        const QString path = dir.filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (!doc.isObject()) continue;
        const QJsonObject root = doc.object();
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            if (it.key().startsWith(QStringLiteral("Geopdf")) && it.value().isObject()) return path;
        }
    }
    return QString();
}

static bool mergeSeriesResultIntoPhaseOneJson(const QString& jsonPath, const QString& layerName, const QJsonObject& checkItem, QString* failedPath = nullptr)
{
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { if (failedPath) *failedPath = jsonPath; return false; }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) { if (failedPath) *failedPath = jsonPath; return false; }
    QJsonObject root = doc.object();
    QString geopdfKey;
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) if (it.key().startsWith(QStringLiteral("Geopdf")) && it.value().isObject()) { geopdfKey = it.key(); break; }
    if (geopdfKey.isEmpty()) { if (failedPath) *failedPath = jsonPath; return false; }
    QJsonObject geopdfNode = root.value(geopdfKey).toObject();
    QJsonObject tasks = geopdfNode.value(QStringLiteral("tasks")).toObject();
    QString taskKey;
    for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
        if (it.key().startsWith(QStringLiteral("task")) && it.value().toObject().value(QStringLiteral("layer_name")).toString() == layerName) {
            taskKey = it.key();
            break;
        }
    }
    if (taskKey.isEmpty()) taskKey = QStringLiteral("task%1").arg(tasks.size() + 1);
    QJsonObject taskNode = tasks.value(taskKey).toObject();
    taskNode[QStringLiteral("layer_name")] = layerName;
    
    // 查找是否已经存在 501 或 图幅接边与系列化表达 的 check_item
    QString existingCheckItemKey;
    for (auto it = taskNode.constBegin(); it != taskNode.constEnd(); ++it) {
        if (it.key().startsWith(QStringLiteral("check_item"))) {
            QJsonObject item = it.value().toObject();
            if (item.value(QStringLiteral("id")).toString() == QStringLiteral("501") || 
                item.value(QStringLiteral("name")).toString() == QStringLiteral("图幅接边与系列化表达")) {
                existingCheckItemKey = it.key();
                break;
            }
        }
    }

    if (!existingCheckItemKey.isEmpty()) {
        // 覆盖已有的
        taskNode[existingCheckItemKey] = checkItem;
    } else {
        // 新增
        int checkItemIndex = 1;
        while (taskNode.contains(QStringLiteral("check_item%1").arg(checkItemIndex))) {
            checkItemIndex++;
        }
        taskNode[QStringLiteral("check_item%1").arg(checkItemIndex)] = checkItem;
    }
    
    tasks[taskKey] = taskNode;
    geopdfNode[QStringLiteral("tasks")] = tasks;
    root[geopdfKey] = geopdfNode;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) { if (failedPath) *failedPath = jsonPath; return false; }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

static bool removeSeriesShapefile(const QString& shpPath)
{
    const QFileInfo fi(shpPath);
    const QString base = fi.path() + QStringLiteral("/") + fi.completeBaseName();
    const QStringList exts = {
        QStringLiteral("shp"), QStringLiteral("shx"), QStringLiteral("dbf"),
        QStringLiteral("prj"), QStringLiteral("cpg"), QStringLiteral("qpj"),
        QStringLiteral("sbn"), QStringLiteral("sbx")
    };
    for (const QString& ext : exts) {
        const QString path = base + QStringLiteral(".") + ext;
        if (QFile::exists(path) && !QFile::remove(path)) return false;
    }
    return true;
}

static bool jsonNumber(const QJsonObject& obj, const QString& key, double& out)
{
    const QJsonValue v = obj.value(key);
    if (!v.isDouble()) return false;
    out = v.toDouble();
    return std::isfinite(out);
}

static QVariant jsonDoubleVariant(const QJsonObject& obj, const QString& key)
{
    double value = 0.0;
    return jsonNumber(obj, key, value) ? QVariant(value) : QVariant();
}

static bool resolveSeriesErrorPoint(const QJsonObject& err, QgsPointXY& point, QString& pointSource)
{
    double x = 0.0, y = 0.0;
    if (jsonNumber(err, QStringLiteral("point_x"), x) && jsonNumber(err, QStringLiteral("point_y"), y)) {
        point = QgsPointXY(x, y);
        pointSource = err.value(QStringLiteral("point_source")).toString(QStringLiteral("point_x_y"));
        return true;
    }

    double lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;
    if (jsonNumber(err, QStringLiteral("left_x"), lx) && jsonNumber(err, QStringLiteral("left_y"), ly)
        && jsonNumber(err, QStringLiteral("right_x"), rx) && jsonNumber(err, QStringLiteral("right_y"), ry)) {
        point = QgsPointXY((lx + rx) / 2.0, (ly + ry) / 2.0);
        pointSource = QStringLiteral("nearest_midpoint");
        return true;
    }

    if (jsonNumber(err, QStringLiteral("left_x"), lx) && jsonNumber(err, QStringLiteral("left_y"), ly)) {
        point = QgsPointXY(lx, ly);
        pointSource = QStringLiteral("left_point");
        return true;
    }
    return false;
}

static bool resolveSeriesErrorLine(const QJsonObject& err, QgsPointXY& leftPoint, QgsPointXY& rightPoint)
{
    double lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;
    if (!jsonNumber(err, QStringLiteral("left_x"), lx) || !jsonNumber(err, QStringLiteral("left_y"), ly)
        || !jsonNumber(err, QStringLiteral("right_x"), rx) || !jsonNumber(err, QStringLiteral("right_y"), ry)) {
        return false;
    }
    if (qFuzzyCompare(lx, rx) && qFuzzyCompare(ly, ry)) return false;
    leftPoint = QgsPointXY(lx, ly);
    rightPoint = QgsPointXY(rx, ry);
    return true;
}

static QString seriesPhaseOnePythonPath()
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/apps/Python39/python.exe"));
}

static QString seriesPhaseOneScriptPath(const QString& scriptName)
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/preprocess_scripts/%1").arg(scriptName));
}

static bool runSeriesPhaseOneScript(const QString& scriptName, const QStringList& args, QString* message, const std::function<void(const QString&)>& logCallback = {})
{
    const QString pythonPath = seriesPhaseOnePythonPath();
    const QString scriptPath = seriesPhaseOneScriptPath(scriptName);
    if (!QFileInfo::exists(pythonPath)) { if (message) *message = QStringLiteral("一期 Python 不存在：%1").arg(pythonPath); return false; }
    if (!QFileInfo::exists(scriptPath)) { if (message) *message = QStringLiteral("一期脚本不存在：%1").arg(scriptPath); return false; }
    QProcess proc; QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8")); env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    const QString mpqisDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    if (!mpqisDir.isEmpty()) env.insert(QStringLiteral("PYTHONHOME"), QDir(mpqisDir).filePath(QStringLiteral("MPQIS/apps/Python39")));
    proc.setProcessEnvironment(env); proc.start(pythonPath, QStringList() << scriptPath << args);
    if (!proc.waitForStarted(5000)) { if (message) *message = QStringLiteral("无法启动脚本：%1").arg(scriptName); return false; }
    auto flushStream = [&](QByteArray data, const QString& prefix) { const QString text = QString::fromUtf8(data).trimmed(); if (text.isEmpty()) return; for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) if (logCallback) logCallback(prefix.isEmpty() ? line : QStringLiteral("%1%2").arg(prefix, line)); };
    while (proc.state() != QProcess::NotRunning) { proc.waitForFinished(200); flushStream(proc.readAllStandardOutput(), QString()); flushStream(proc.readAllStandardError(), QStringLiteral("[stderr] ")); QCoreApplication::processEvents(); }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) { if (message) *message = QStringLiteral("%1 执行失败").arg(scriptName); return false; }
    if (message) *message = QStringLiteral("%1 执行成功").arg(scriptName); return true;
}

static bool validateSeriesShpFiles(const QString& shpPath, QString* error)
{
    const QFileInfo fi(shpPath); if (!fi.isFile()) { if (error) *error = QStringLiteral("SHP 不存在：%1").arg(shpPath); return false; }
    const QString base = fi.path() + QStringLiteral("/") + fi.completeBaseName();
    for (const QString& ext : { QStringLiteral("shx"), QStringLiteral("dbf") }) if (!QFile::exists(base + QStringLiteral(".") + ext)) { if (error) *error = QStringLiteral("Shapefile 缺少 .%1 文件：%2").arg(ext, fi.fileName()); return false; }
    return true;
}

static bool validatePreprocessShpOutput(const QString& shpDir, QString* error)
{
    QDir dir(shpDir);
    if (!dir.exists()) {
        if (error) *error = QStringLiteral("预处理 SHP 目录不存在：%1").arg(shpDir);
        return false;
    }

    const QString framePath = dir.filePath(QStringLiteral("内图廓.shp"));
    if (!QFile::exists(framePath)) {
        if (error) *error = QStringLiteral("预处理结果缺少内图廓.shp：%1").arg(shpDir);
        return false;
    }

    QString fileError;
    if (!validateSeriesShpFiles(framePath, &fileError)) {
        if (error) *error = fileError;
        return false;
    }

    const QStringList shpFiles = dir.entryList(QStringList() << QStringLiteral("*.shp"), QDir::Files, QDir::Name);
    bool hasCheckableLayer = false;
    for (const QString& shpFile : shpFiles) {
        if (QFileInfo(shpFile).completeBaseName() == QStringLiteral("内图廓")) continue;
        const QString shpPath = dir.filePath(shpFile);
        if (!validateSeriesShpFiles(shpPath, &fileError)) {
            if (error) *error = fileError;
            return false;
        }
        hasCheckableLayer = true;
    }

    if (!hasCheckableLayer) {
        if (error) *error = QStringLiteral("预处理结果缺少可检查的 SHP 图层：%1").arg(shpDir);
        return false;
    }
    return true;
}

static bool ensureSeriesWritableDir(const QString& dirPath, QString* error)
{
    QDir dir(dirPath); if (!dir.exists() && !QDir().mkpath(dirPath)) { if (error) *error = QStringLiteral("输出目录不可创建：%1").arg(dirPath); return false; }
    QFile probe(dir.filePath(QStringLiteral(".series_write_test"))); if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = QStringLiteral("输出目录不可写：%1").arg(dirPath); return false; }
    probe.close(); probe.remove(); return true;
}

static QString resolvePreparedMainShp(const QString& outputRoot, const QString& pdfPath)
{
    const QString shpDir = QDir(outputRoot).filePath(QFileInfo(pdfPath).completeBaseName() + QStringLiteral("_shp"));
    const QString frameShp = QDir(shpDir).filePath(QStringLiteral("内图廓.shp"));
    if (QFile::exists(frameShp)) return frameShp;
    const QStringList shpFiles = QDir(shpDir).entryList(QStringList() << QStringLiteral("*.shp"), QDir::Files, QDir::Name);
    return shpFiles.isEmpty() ? QString() : QDir(shpDir).filePath(shpFiles.first());
}

static QJsonObject normalizeSeriesFindingStatus(QJsonObject err)
{
    const QString errorType = err.value(QStringLiteral("error_type")).toString().trimmed();
    if (errorType == QStringLiteral("cartographic_symbol_rule_mismatch")
        && (!err.value(QStringLiteral("source_evidence_verified")).toBool()
            || !err.value(QStringLiteral("rule_applicability_confirmed")).toBool()
            || !err.value(QStringLiteral("symbol_component_confirmed")).toBool())) {
        err[QStringLiteral("decision_status")]=QStringLiteral("not_evaluable");
        err[QStringLiteral("final_state")]=QStringLiteral("not_evaluable");
        err[QStringLiteral("evidence_note")]=QStringLiteral("旧结果缺少原图、规则适用性或符号分量证据，需要重新检查");
    }
    if (errorType == QStringLiteral("style_field_mismatch")
        && (!err.value(QStringLiteral("source_evidence_verified")).toBool()
            || !err.value(QStringLiteral("symbol_component_confirmed")).toBool()
            || !err.value(QStringLiteral("style_context_verified")).toBool())) {
        err[QStringLiteral("decision_status")]=QStringLiteral("not_evaluable");
        err[QStringLiteral("final_state")]=QStringLiteral("not_evaluable");
    }
    const bool reviewByType = errorType == QStringLiteral("cross_scale_weak_support")
        || errorType == QStringLiteral("cross_scale_candidate_ambiguous")
        || errorType == QStringLiteral("cross_scale_support_missing")
        || errorType == QStringLiteral("cross_scale_actual_layer_semantic_mismatch");
    const bool notEvaluableByType = errorType == QStringLiteral("cross_scale_support_not_evaluable");
    if (reviewByType || err.value(QStringLiteral("match_status")).toString() == QStringLiteral("weak_supported")) {
        err[QStringLiteral("decision_status")] = QStringLiteral("needs_review");
        const QString finalState = err.value(QStringLiteral("final_state")).toString().trimmed();
        if (finalState.isEmpty() || finalState == QStringLiteral("confirmed") || finalState == QStringLiteral("confirmed_error"))
            err[QStringLiteral("final_state")] = QStringLiteral("needs_review");
    } else if (notEvaluableByType) {
        err[QStringLiteral("decision_status")] = QStringLiteral("not_evaluable");
        const QString finalState = err.value(QStringLiteral("final_state")).toString().trimmed();
        if (finalState.isEmpty() || finalState == QStringLiteral("confirmed") || finalState == QStringLiteral("confirmed_error"))
            err[QStringLiteral("final_state")] = QStringLiteral("not_evaluable");
    }
    return err;
}

static bool isConfirmedSeriesError(const QJsonObject& err)
{
    const QString decisionStatus = normalizeSeriesFindingStatus(err).value(QStringLiteral("decision_status")).toString().trimmed();
    if (decisionStatus == QStringLiteral("not_error") || decisionStatus == QStringLiteral("skipped")) return false;
    return decisionStatus.isEmpty() || decisionStatus == QStringLiteral("confirmed_error");
}

static bool isSeriesReviewFinding(const QJsonObject& err)
{
    const QString status = normalizeSeriesFindingStatus(err).value(QStringLiteral("decision_status")).toString().trimmed();
    return status == QStringLiteral("needs_review") || status == QStringLiteral("not_evaluable");
}

static void assignUniqueSeriesOutputIds(QVector<QJsonObject>& records)
{
    QMap<QString, int> occurrences;
    QSet<QString> usedIds;
    for (QJsonObject& record : records) {
        const QString sourceId = record.value(QStringLiteral("err_id")).toString().trimmed();
        QJsonObject identity = record;
        identity.remove(QStringLiteral("err_id"));
        const QString baseId = QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex().left(32));
        int occurrence = occurrences.value(baseId, 0) + 1;
        occurrences[baseId] = occurrence;
        QString outputId = baseId;
        while (usedIds.contains(outputId)) {
            ++occurrence;
            occurrences[baseId] = occurrence;
            const QString suffix = QStringLiteral("_%1").arg(occurrence);
            outputId = baseId.left(40 - suffix.size()) + suffix;
        }
        usedIds.insert(outputId);
        if (!sourceId.isEmpty()) record[QStringLiteral("source_err_id")] = sourceId;
        record[QStringLiteral("err_id")] = outputId;
    }
}

SeriesMapEdgeMatchTaskDialog::SeriesMapEdgeMatchTaskDialog(QWidget* parent, Qt::WindowFlags f)
    : QDialog(parent, f)
{
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowSystemMenuHint);
    buildUi();
}

SeriesMapEdgeMatchTaskDialog::SeriesMapEdgeMatchTaskDialog(QgisInterface* iface, QWidget* parent, Qt::WindowFlags f)
    : QDialog(parent, f)
{
    Q_UNUSED(iface);
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowSystemMenuHint);
    buildUi();
}

void SeriesMapEdgeMatchTaskDialog::setReportExportHandler(const std::function<void(const QStringList&)>& handler)
{
    m_reportExportHandler = handler;
}

QString SeriesMapEdgeMatchTaskDialog::getPreprocessPythonPath() const
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/apps/Python39/python.exe"));
}

QString SeriesMapEdgeMatchTaskDialog::getPreprocessScriptPath(const QString& scriptName) const
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/preprocess_scripts/%1").arg(scriptName));
}

bool SeriesMapEdgeMatchTaskDialog::runPreprocessScript(const QString& scriptName, const QStringList& args, QString* message)
{
    const QString pythonPath = getPreprocessPythonPath();
    const QString scriptPath = getPreprocessScriptPath(scriptName);

    if (!QFileInfo::exists(pythonPath)) {
        if (message) *message = QStringLiteral("Python 不存在：%1").arg(pythonPath);
        return false;
    }
    if (!QFileInfo::exists(scriptPath)) {
        if (message) *message = QStringLiteral("脚本不存在：%1").arg(scriptPath);
        return false;
    }

    QProcess proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    const QString mpqisDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    if (!mpqisDir.isEmpty()) {
        env.insert(QStringLiteral("PYTHONHOME"), QDir(mpqisDir).filePath(QStringLiteral("MPQIS/apps/Python39")));
        QString path = env.value(QStringLiteral("PATH"));
        const QString mpqisBin = QDir(mpqisDir).filePath(QStringLiteral("MPQIS/bin"));
        env.insert(QStringLiteral("PATH"), mpqisBin + QStringLiteral(";") + path);
    }
    proc.setProcessEnvironment(env);

    proc.start(pythonPath, QStringList() << scriptPath << args);
    if (!proc.waitForStarted(5000)) {
        if (message) *message = QStringLiteral("无法启动脚本：%1").arg(scriptName);
        return false;
    }

    auto flushStream = [&](QByteArray data, const QString& prefix) {
        const QString text = QString::fromUtf8(data).trimmed();
        if (text.isEmpty()) return;
        for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
            m_outputEdit->append(prefix.isEmpty() ? line : QStringLiteral("%1%2").arg(prefix, line));
            QCoreApplication::processEvents();
        }
    };

    while (proc.state() != QProcess::NotRunning) {
        proc.waitForFinished(200);
        flushStream(proc.readAllStandardOutput(), QString());
        flushStream(proc.readAllStandardError(), QStringLiteral("[stderr] "));
        QCoreApplication::processEvents();
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (message) *message = QStringLiteral("%1 执行失败").arg(scriptName);
        return false;
    }

    if (message) *message = QStringLiteral("%1 执行成功").arg(scriptName);
    return true;
}

QString SeriesMapEdgeMatchTaskDialog::resolvePreprocessedShpDir(const QString& pdfPath, const QString& outputRootDir) const
{
    QFileInfo fi(pdfPath);
    QString baseName = fi.completeBaseName();
    QString pdfDir = fi.absolutePath();

    auto isValidShpDir = [](const QString& dirPath) -> bool {
        QString error;
        return validatePreprocessShpOutput(dirPath, &error);
    };

    // 1. 精确匹配：输出目录或 PDF 所在目录下的 baseName_shp
    QString exactPdf = QDir(pdfDir).filePath(baseName + QStringLiteral("_shp"));
    if (isValidShpDir(exactPdf)) return exactPdf;

    QString exactOut = QDir(outputRootDir).filePath(baseName + QStringLiteral("_shp"));
    if (isValidShpDir(exactOut)) return exactOut;

    // 2. 模糊匹配：查找包含或被包含的 _shp 目录（例如 "兰州市_shp" 匹配 "兰州市 分县政区.pdf"）
    QStringList searchDirs = {pdfDir, outputRootDir};
    QStringList fuzzyMatches;
    for (const QString& searchDir : searchDirs) {
        QDir dir(searchDir);
        const QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& subDir : subDirs) {
            if (!subDir.endsWith(QStringLiteral("_shp"), Qt::CaseInsensitive)) continue;
            const QString subBase = subDir.left(subDir.length() - 4);
            if (!baseName.contains(subBase, Qt::CaseInsensitive)
                && !subBase.contains(baseName, Qt::CaseInsensitive)) continue;
            const QString candidate = dir.filePath(subDir);
            if (isValidShpDir(candidate) && !fuzzyMatches.contains(candidate)) fuzzyMatches << candidate;
        }
    }
    if (fuzzyMatches.size() == 1) return fuzzyMatches.first();
    return QString();
}

SeriesMapEdgeMatchTaskDialog::PreprocessResult SeriesMapEdgeMatchTaskDialog::preprocessGeoPdfIfNeeded(const QString& pdfPath, const QString& outputRootDir)
{
    PreprocessResult result;
    result.pdfPath = pdfPath;

    QFileInfo fi(pdfPath);
    if (!fi.exists()) {
        result.success = false;
        result.errorMessage = QStringLiteral("文件不存在");
        return result;
    }

    QString pdfDir = fi.absolutePath();
    QString baseName = fi.completeBaseName();
    QString geojsonDir = QDir(outputRootDir).filePath(baseName + QStringLiteral("_geojson"));
    QString shpDir = QDir(outputRootDir).filePath(baseName + QStringLiteral("_shp"));

    result.shpDir = shpDir;

    QDir().mkpath(geojsonDir);
    QDir().mkpath(shpDir);

    QString msg;

    // 1. PDF -> GeoJSON
    m_outputEdit->append(QStringLiteral("开始解析 PDF: %1").arg(pdfPath));
    QCoreApplication::processEvents();

    const QString jsxTemplatePath = getPreprocessScriptPath(QStringLiteral("export_to_geojson.jsx"));
    if (!QFileInfo::exists(jsxTemplatePath)) {
        result.success = false;
        result.errorStage = QStringLiteral("pdf_to_geojson");
        result.errorMessage = QStringLiteral("JSX 模板不存在：%1").arg(jsxTemplatePath);
        m_outputEdit->append(QStringLiteral("PDF 解析失败: %1").arg(result.errorMessage));
        QCoreApplication::processEvents();
        return result;
    }

    m_outputEdit->append(QStringLiteral("PDF路径：%1").arg(pdfPath));
    m_outputEdit->append(QStringLiteral("GeoJSON父目录：%1").arg(pdfDir));
    m_outputEdit->append(QStringLiteral("预期GeoJSON目录：%1").arg(geojsonDir));
    m_outputEdit->append(QStringLiteral("SHP输出目录：%1").arg(shpDir));
    m_outputEdit->append(QStringLiteral("JSX模板：%1").arg(jsxTemplatePath));

    QStringList pdfToGeojsonArgs;
    pdfToGeojsonArgs << QStringLiteral("--pdf-files") << pdfPath
        << QStringLiteral("--output-dir") << outputRootDir
        << QStringLiteral("--jsx-template") << jsxTemplatePath;
    if (!runPreprocessScript(QStringLiteral("launch_ai2.py"), pdfToGeojsonArgs, &msg)) {
        result.success = false;
        result.errorStage = QStringLiteral("pdf_to_geojson");
        result.errorMessage = msg;
        m_outputEdit->append(QStringLiteral("PDF 解析失败: %1").arg(msg));
        QCoreApplication::processEvents();
        return result;
    }

    // 严格校验是否生成了 GeoJSON 文件，如果未生成，说明 launch_ai2.py 内部执行失败（即使它返回了 0）
    QStringList geojsonFiles = QDir(geojsonDir).entryList(QStringList() << QStringLiteral("*.geojson"), QDir::Files);
    if (geojsonFiles.isEmpty()) {
        QString nestedDir = QDir(geojsonDir).filePath(QFileInfo(geojsonDir).fileName());
        if (QDir(nestedDir).exists()) {
            geojsonFiles = QDir(nestedDir).entryList(QStringList() << QStringLiteral("*.geojson"), QDir::Files);
            if (!geojsonFiles.isEmpty()) {
                geojsonDir = nestedDir;
            }
        }
    }

    if (geojsonFiles.isEmpty()) {
        result.success = false;
        result.errorStage = QStringLiteral("pdf_to_geojson");
        result.errorMessage = QStringLiteral("PDF 已调用 Illustrator 解析，但未生成任何 GeoJSON 文件。可能是 Illustrator 2025 执行 JSX 失败。请检查 Illustrator/JSX 执行日志。");
        m_outputEdit->append(QStringLiteral("PDF 解析失败: %1").arg(result.errorMessage));
        QCoreApplication::processEvents();
        return result;
    }

    // 2. GeoJSON -> SHP
    m_outputEdit->append(QStringLiteral("开始转换 GeoJSON 为 SHP..."));
    QCoreApplication::processEvents();

    QStringList geojsonToShpArgs;
    geojsonToShpArgs << QStringLiteral("--geopdf") << pdfPath
        << QStringLiteral("--geojson-folder") << geojsonDir
        << QStringLiteral("--output-folder") << shpDir;
    if (!runPreprocessScript(QStringLiteral("geojson2shp2.py"), geojsonToShpArgs, &msg)) {
        result.success = false;
        result.errorStage = QStringLiteral("geojson_to_shp");
        result.errorMessage = msg;
        m_outputEdit->append(QStringLiteral("GeoJSON 转换失败: %1").arg(msg));
        QCoreApplication::processEvents();
        return result;
    }

    QString validationError;
    if (!validatePreprocessShpOutput(shpDir, &validationError)) {
        result.success = false;
        result.errorStage = QStringLiteral("validate_shp_output");
        result.errorMessage = validationError;
        m_outputEdit->append(QStringLiteral("SHP 输出验收失败: %1").arg(validationError));
        QCoreApplication::processEvents();
        return result;
    }

    // 删除中间生成的 geojson 文件夹
    QDir dirToRemove(geojsonDir);
    if (dirToRemove.exists()) {
        dirToRemove.removeRecursively();
    }

    result.success = true;
    m_outputEdit->append(QStringLiteral("预处理成功: %1").arg(shpDir));
    QCoreApplication::processEvents();
    return result;
}

void SeriesMapEdgeMatchTaskDialog::buildUi()
{
    setWindowTitle(QStringLiteral("系列图内容检查 - 图幅接边与系列化表达"));
    resize(960, 720);

    auto* mainLayout = new QVBoxLayout(this);

    auto* inputGroup = new QGroupBox(QStringLiteral("全局配置"), this);
    auto* globalLayout = new QHBoxLayout(inputGroup);
    auto* inputLayout = new QGridLayout();
    inputLayout->setColumnStretch(1, 1);
    inputLayout->setVerticalSpacing(12);
    globalLayout->addLayout(inputLayout, 2);

    inputLayout->addWidget(new QLabel(QStringLiteral("图幅索引："), inputGroup), 0, 0);
    m_indexFileEdit = new QLineEdit(inputGroup);
    m_indexFileEdit->setPlaceholderText(QStringLiteral("可选：外部图幅索引 SHP（留空则自动从 GeoPDF 提取）"));
    auto* indexButton = new QPushButton(QStringLiteral("选择 shp"), inputGroup);
    inputLayout->addWidget(m_indexFileEdit, 0, 1);
    inputLayout->addWidget(indexButton, 0, 2);

    inputLayout->addWidget(new QLabel(QStringLiteral("质检规则："), inputGroup), 1, 0);
    m_xmlEdit = new QLineEdit(inputGroup);
    m_xmlEdit->setPlaceholderText(QStringLiteral("请选择 XML 质检规则文件"));
    auto* xmlButton = new QPushButton(QStringLiteral("选择文件"), inputGroup);
    inputLayout->addWidget(m_xmlEdit, 1, 1); inputLayout->addWidget(xmlButton, 1, 2);

    inputLayout->addWidget(new QLabel(QStringLiteral("结果输出目录："), inputGroup), 2, 0);
    m_outputDirEdit = new QLineEdit(inputGroup);
    m_outputDirEdit->setPlaceholderText(QStringLiteral("请选择结果输出根目录"));
    auto* outputDirButton = new QPushButton(QStringLiteral("选择目录"), inputGroup);
    inputLayout->addWidget(m_outputDirEdit, 2, 1);
    inputLayout->addWidget(outputDirButton, 2, 2);

    auto* previewLayout = new QVBoxLayout();
    auto* previewTitle = new QHBoxLayout();
    previewTitle->addWidget(new QLabel(QStringLiteral("图幅分布预览"), inputGroup));
    previewTitle->addStretch();
    auto* showAllButton = new QPushButton(QStringLiteral("显示全部"), inputGroup);
    previewTitle->addWidget(showAllButton);
    previewLayout->addLayout(previewTitle);
    m_sheetOverview = new SeriesSheetOverview(inputGroup);
    previewLayout->addWidget(m_sheetOverview);
    auto* legend = new QLabel(QStringLiteral("蓝色：当前图幅  青色：关联图幅 · 滚轮缩放 / 拖动平移"), inputGroup);
    legend->setWordWrap(true);
    previewLayout->addWidget(legend);
    globalLayout->addLayout(previewLayout, 1);
    mainLayout->addWidget(inputGroup);
    connect(showAllButton, &QPushButton::clicked, this, [this]() {
        m_previewMode = PreviewMode::Configured;
        updateOverview(true);
    });
    m_sheetOverview->sheetClicked = [this](qint64 id) { locateOverviewSheet(id); };

    auto* tableGroup = new QGroupBox(QStringLiteral("地图配置"), this);
    auto* tableLayout = new QVBoxLayout(tableGroup);

    auto* tableToolbar = new QHBoxLayout();
    auto* addRowBtn = new QPushButton(QStringLiteral("+ 添加地图"), tableGroup);
    auto* removeRowBtn = new QPushButton(QStringLiteral("- 移除选中"), tableGroup);
    auto* clearAllRowsBtn = new QPushButton(QStringLiteral("全部删除"), tableGroup);
    tableToolbar->addWidget(addRowBtn);
    tableToolbar->addWidget(removeRowBtn);
    tableToolbar->addWidget(clearAllRowsBtn);
    tableToolbar->addStretch();
    tableLayout->addLayout(tableToolbar);

    m_configTree = new QTreeWidget(tableGroup);
    m_configTree->setColumnCount(6);
    m_configTree->setHeaderLabels({
        QStringLiteral("主检查地图"),
        QStringLiteral("主图比例尺"),
        QStringLiteral("关联地图"),
        QStringLiteral("关联图比例尺"),
        QStringLiteral("关系类型"),
        QStringLiteral("状态")
    });

    m_configTree->setRootIsDecorated(true);
    m_configTree->setItemsExpandable(true);
    m_configTree->setAllColumnsShowFocus(true);
    m_configTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_configTree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_configTree->setWordWrap(false);
    m_configTree->header()->setSectionResizeMode(QHeaderView::Stretch);
    m_configTree->header()->setMinimumSectionSize(95);
    m_configTree->setColumnWidth(SeriesColMainMap, 210);
    m_configTree->setColumnWidth(SeriesColMainScale, 115);
    m_configTree->setColumnWidth(SeriesColAssocMap, 250);
    m_configTree->setColumnWidth(SeriesColAssocScale, 140);
    m_configTree->setColumnWidth(SeriesColRelationType, 90);
    m_configTree->setColumnWidth(SeriesColStatus, 150);
    m_configTree->header()->setStretchLastSection(false);

    m_configTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableLayout->addWidget(m_configTree);

    mainLayout->addWidget(tableGroup, 1);

    connect(addRowBtn, &QPushButton::clicked, this, &SeriesMapEdgeMatchTaskDialog::addConfigRow);
    connect(removeRowBtn, &QPushButton::clicked, this, &SeriesMapEdgeMatchTaskDialog::removeConfigRow);
    connect(clearAllRowsBtn, &QPushButton::clicked, this, [this]() {
        if (m_configTree->topLevelItemCount() > 0 && QMessageBox::question(this, QStringLiteral("确认"), QStringLiteral("确定要清空所有地图配置吗？")) == QMessageBox::Yes) {
            m_configTree->clear();
            // Keep m_sheetInfos so the loaded index can be reused after the user reconfigures maps.
            m_adjacentPairs.clear();
            m_coverageGroups.clear();
            m_taskChoices.clear();
            m_pairSelection.clear();
            checkTableStatus();
            updateSelectedCount();
        }
    });

    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(QStringLiteral("一键全选"), this);
    m_clearAllButton = new QPushButton(QStringLiteral("取消全选"), this);
    m_startButton = new QPushButton(QStringLiteral("开始检查"), this);
    m_exportErrorButton = new QPushButton(QStringLiteral("重新生成输出"), this);
    m_exportReportButton = new QPushButton(QStringLiteral("质检报告导出"), this);
    m_exportErrorButton->setEnabled(false);
    m_exportReportButton->setEnabled(true);
    m_exportErrorButton->hide();
    m_taskCountLabel = new QLabel(QStringLiteral("已选：0 项检查任务"), this);

    buttonLayout->addWidget(m_selectAllButton);
    buttonLayout->addWidget(m_clearAllButton);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_exportErrorButton);
    buttonLayout->addWidget(m_exportReportButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_taskCountLabel);
    mainLayout->addLayout(buttonLayout);

    // 添加进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat(QStringLiteral("等待开始..."));
    mainLayout->addWidget(m_progressBar);

    auto* outputGroup = new QGroupBox(QStringLiteral("任务清单 / 占位结果"), this);
    auto* outputLayout = new QVBoxLayout(outputGroup);
    m_outputEdit = new QTextEdit(outputGroup);
    m_outputEdit->setReadOnly(true);
    m_outputEdit->setPlaceholderText(QStringLiteral("点击“开始检查”后，将在这里输出待检查任务清单。"));
    outputLayout->addWidget(m_outputEdit);
    mainLayout->addWidget(outputGroup, 1);
    outputGroup->hide(); // 隐藏该区域，保留功能

    connect(xmlButton, &QPushButton::clicked, this, [this]() { browseXmlRule(); });
    connect(indexButton, &QPushButton::clicked, this, [this]() { browseIndexFile(); });
    connect(outputDirButton, &QPushButton::clicked, this, [this]() { browseOutputDirectory(); });
    connect(m_selectAllButton, &QPushButton::clicked, this, [this]() { selectAllPairs(); });
    connect(m_clearAllButton, &QPushButton::clicked, this, [this]() { clearAllPairs(); });
    connect(m_startButton, &QPushButton::clicked, this, [this]() { startCheck(); });
    connect(m_exportErrorButton, &QPushButton::clicked, this, [this]() { exportErrorShp(); });
    connect(m_exportReportButton, &QPushButton::clicked, this, [this]() { exportReport(); });
    connect(m_configTree, &QTreeWidget::itemChanged, this, &SeriesMapEdgeMatchTaskDialog::setTaskChecked);
    connect(m_configTree, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* current, QTreeWidgetItem*) { highlightConfigItem(current); });
    connect(m_configTree, &QTreeWidget::itemClicked, this,
        [this](QTreeWidgetItem* item, int) {
            if (m_previewMode != PreviewMode::Selection) highlightConfigItem(item);
        });

    connect(m_indexFileEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        Q_UNUSED(text);
        clearLoadedSheetRelations();
    });

    updateSelectedCount();
}

void SeriesMapEdgeMatchTaskDialog::appendRuntimeLog(const QString& text)
{
    if (!m_outputEdit || text.trimmed().isEmpty()) return;
    m_outputEdit->append(text);
    QCoreApplication::processEvents();
}

void SeriesMapEdgeMatchTaskDialog::clearLoadedSheetRelations()
{
    m_sheetInfos.clear();
    m_verifiedPreviewSheets.clear();
    m_displayedPreviewSheets.clear();
    m_cachedPreviewSheets.clear();
    m_previewMode = PreviewMode::Index;
    m_previewRelationsVerified = false;
    m_adjacentPairs.clear();
    m_coverageGroups.clear();
    m_taskChoices.clear();
    if (m_sheetOverview) m_sheetOverview->setSheets({});
    m_lastErrorRecords.clear();
    m_originalErrorRecords.clear();
    m_pairRunContext.clear();
    m_checkedLayersByMap.clear();
    m_originalStats = QJsonObject();
    m_originalStats.insert(QStringLiteral("check_completed"), false);
    m_inputDataType = InputDataType::Unknown;
    m_sheetIndexCrs = QgsCoordinateReferenceSystem();

    if (m_progressBar) {
        m_progressBar->setValue(0);
        m_progressBar->setFormat(QStringLiteral("等待加载图幅关系..."));
    }
    updateSelectedCount();
}



void SeriesMapEdgeMatchTaskDialog::browseXmlRule()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择 XML 质检规则"),
        QString(),
        QStringLiteral("XML 文件 (*.xml);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_xmlEdit->setText(path);
    }
}

void SeriesMapEdgeMatchTaskDialog::browseIndexFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择图幅索引 / 图廓 shp"),
        QString(),
        QStringLiteral("SHP 文件 (*.shp);;所有文件 (*)"));
    if (!path.isEmpty()) {
        m_indexFileEdit->setText(path);
        appendRuntimeLog(QStringLiteral("已选择外部图幅索引，正在自动加载图幅关系..."));
        loadSheets(false);
    }
}

void SeriesMapEdgeMatchTaskDialog::browseOutputDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择结果输出根目录"),
        m_outputDirEdit ? m_outputDirEdit->text() : QString());
    if (!path.isEmpty() && m_outputDirEdit) {
        m_outputDirEdit->setText(path);
    }
}

QString SeriesMapEdgeMatchTaskDialog::phaseOneOutputRootDir() const
{
    QString root = m_outputDirEdit ? m_outputDirEdit->text().trimmed() : QString();
    if (root.isEmpty()) {
        root = QDir::current().filePath(QStringLiteral("output_results"));
    }
    return root;
}

QString SeriesMapEdgeMatchTaskDialog::resultOutputRootDir() const
{
    return phaseOneOutputRootDir();
}

QString SeriesMapEdgeMatchTaskDialog::seriesWorkDir() const
{
    return QDir(phaseOneOutputRootDir()).filePath(QStringLiteral("系列图检查"));
}

QString SeriesMapEdgeMatchTaskDialog::outputRootDir() const
{
    return seriesWorkDir();
}

QStringList SeriesMapEdgeMatchTaskDialog::configuredMainAssocPairs() const
{
    QStringList pairs;
    if (!m_configTree) return pairs;

    for (int row = 0; row < m_configTree->topLevelItemCount(); ++row) {
        const QLineEdit* mainEdit = configLineEdit(row, SeriesColMainMap);
        const QLineEdit* assocEdit = configLineEdit(row, SeriesColAssocMap);
        if (!mainEdit || !assocEdit) continue;

        const QString mainPdf = mainEdit->text().trimmed();
        if (mainPdf.isEmpty()) continue;
        const QString mainName = QFileInfo(mainPdf).completeBaseName();
        const QStringList assocPdfs = splitSeriesTableList(assocEdit->text().trimmed());
        for (const QString& assocPdf : assocPdfs) {
            const QString assocName = QFileInfo(assocPdf.trimmed()).completeBaseName();
            if (assocName.isEmpty()) continue;
            pairs << QStringLiteral("%1 - %2").arg(mainName, assocName);
        }
    }

    pairs.removeDuplicates();
    return pairs;
}

QStringList SeriesMapEdgeMatchTaskDialog::currentReportResultJsonPaths() const
{
    QStringList paths;
    QDir root(phaseOneOutputRootDir());
    if (!root.exists()) return paths;

    auto appendLatestInDir = [&paths](const QDir& dir) {
        const QFileInfoList files = dir.entryInfoList(QStringList() << QStringLiteral("*_result_*.json"), QDir::Files, QDir::Time);
        if (!files.isEmpty()) paths << files.first().absoluteFilePath();
    };

    appendLatestInDir(root);
    const QFileInfoList childDirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& childDir : childDirs) {
        appendLatestInDir(QDir(childDir.absoluteFilePath()));
    }
    paths.removeDuplicates();
    return paths;
}

QString SeriesMapEdgeMatchTaskDialog::resolveSheetName(const QgsFeature& feature, const QgsFields& fields) const
{
    QStringList preferredNames;
    preferredNames << QStringLiteral("图幅号")
        << QStringLiteral("图幅编号")
        << QStringLiteral("图幅名称")
        << QStringLiteral("图名")
        << QStringLiteral("NAME")
        << QStringLiteral("SHEET")
        << QStringLiteral("SHEET_ID")
        << QStringLiteral("MAP_ID")
        << QStringLiteral("TBH");

    for (const QString& fieldName : preferredNames) {
        const int idx = fields.indexOf(fieldName);
        if (idx >= 0) {
            const QString value = feature.attribute(idx).toString().trimmed();
            if (!value.isEmpty()) {
                return value;
            }
        }
    }

    for (int i = 0; i < fields.count(); ++i) {
        const QString value = feature.attribute(i).toString().trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }

    return QStringLiteral("图幅%1").arg(feature.id());
}

double SeriesMapEdgeMatchTaskDialog::readSeriesRuleDouble(const QStringList& ruleNames, double fallback) const
{
    const QString xmlPath = m_xmlEdit ? m_xmlEdit->text().trimmed() : QString();
    if (xmlPath.isEmpty()) return fallback;

    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return fallback;

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return fallback;
    }
    file.close();

    QDomNodeList rules = doc.documentElement().elementsByTagName(QStringLiteral("rule"));
    for (int i = 0; i < rules.count(); ++i) {
        QDomElement rule = rules.at(i).toElement();
        const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
        bool matched = false;
        for (const QString& candidate : ruleNames) {
            if (name.contains(candidate, Qt::CaseInsensitive)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        QString text = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
        text.replace(QStringLiteral("％"), QStringLiteral("%"));
        bool percent = text.endsWith(QStringLiteral("%"));
        if (percent) text.chop(1);

        bool ok = false;
        const double value = text.toDouble(&ok);
        if (!ok) continue;
        return percent ? value / 100.0 : value;
    }

    return fallback;
}

double SeriesMapEdgeMatchTaskDialog::requiredSeriesRuleDouble(const QStringList& ruleNames, bool* ok) const
{
    if (ok) *ok = false;
    const QString xmlPath = m_xmlEdit ? m_xmlEdit->text().trimmed() : QString();
    if (xmlPath.isEmpty()) return std::numeric_limits<double>::quiet_NaN();

    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return std::numeric_limits<double>::quiet_NaN();

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return std::numeric_limits<double>::quiet_NaN();
    }
    file.close();

    QDomNodeList rules = doc.documentElement().elementsByTagName(QStringLiteral("rule"));
    for (int i = 0; i < rules.count(); ++i) {
        QDomElement rule = rules.at(i).toElement();
        const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
        bool matched = false;
        for (const QString& candidate : ruleNames) {
            if (name.contains(candidate, Qt::CaseInsensitive)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        QString text = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
        text.replace(QStringLiteral("％"), QStringLiteral("%"));
        bool percent = text.endsWith(QStringLiteral("%"));
        if (percent) text.chop(1);

        bool parsed = false;
        const double value = text.toDouble(&parsed);
        if (!parsed) continue;
        if (ok) *ok = true;
        return percent ? value / 100.0 : value;
    }

    return std::numeric_limits<double>::quiet_NaN();
}

QString SeriesMapEdgeMatchTaskDialog::readSeriesRuleText(const QStringList& ruleNames, const QString& fallback, int scale) const
{
    Q_UNUSED(scale);
    const QString xmlPath = m_xmlEdit ? m_xmlEdit->text().trimmed() : QString();
    if (xmlPath.isEmpty()) return fallback;

    QualityCheckConfig cfg;
    if (cfg.loadFromFile(xmlPath)) {
        const QString checkName = QStringLiteral("图幅接边与系列化表达");
        for (const QString& ruleName : ruleNames) {
            const QString value = cfg.getParameterString(checkName, ruleName).trimmed();
            if (!value.isEmpty()) return value;
        }
    }

    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return fallback;

    QDomDocument doc;
    if (!doc.setContent(&file)) {
        file.close();
        return fallback;
    }
    file.close();

    QDomNodeList rules = doc.documentElement().elementsByTagName(QStringLiteral("rule"));
    for (int i = 0; i < rules.count(); ++i) {
        QDomElement rule = rules.at(i).toElement();
        const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
        bool matched = false;
        for (const QString& candidate : ruleNames) {
            if (name == candidate || name.contains(candidate, Qt::CaseInsensitive)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        const QString text = rule.firstChildElement(QStringLiteral("value")).text().trimmed();
        if (!text.isEmpty()) return text;
    }

    return fallback;
}

QStringList SeriesMapEdgeMatchTaskDialog::seriesCheckTargetLayers(const QString& xmlPath, int scale) const
{
    Q_UNUSED(xmlPath);
    QStringList targets = splitSeriesTableList(readSeriesRuleText(QStringList() << QStringLiteral("series_target_layers"), QString(), scale));
    const QStringList extraRuleNames = {
        QStringLiteral("series_boundary_line_layer"),
        QStringLiteral("series_road_layer"),
        QStringLiteral("series_water_layer"),
        QStringLiteral("series_water_area_layer")
    };
    for (const QString& ruleName : extraRuleNames) {
        const QStringList values = splitSeriesTableList(readSeriesRuleText(QStringList() << ruleName, QString(), scale));
        for (const QString& value : values) {
            if (!value.isEmpty() && !targets.contains(value)) targets << value;
        }
    }
    targets.removeDuplicates();
    return targets;
}

double SeriesMapEdgeMatchTaskDialog::seriesIndexAdjacencyTolerance() const
{
    return readSeriesRuleDouble(
        QStringList()
            << QStringLiteral("series_index_adjacency_tolerance")
            << QStringLiteral("图幅关系拓扑容差")
            << QStringLiteral("相邻识别容差"),
        1e-8);
}

double SeriesMapEdgeMatchTaskDialog::seriesCrossScaleOverlapThreshold() const
{
    return requiredSeriesRuleDouble(
        QStringList()
            << QStringLiteral("series_cross_scale_overlap_threshold")
            << QStringLiteral("跨比例尺覆盖阈值")
            << QStringLiteral("覆盖比例阈值"));
}

SeriesMapEdgeMatchTaskDialog::IndexRelationType SeriesMapEdgeMatchTaskDialog::indexRelationType(const QgsGeometry& left, const QgsGeometry& right) const
{
    if (left.isNull() || right.isNull() || left.isEmpty() || right.isEmpty()) {
        return IndexRelationType::None;
    }

    QgsGeometry common;
    try {
        common = left.intersection(right);
    } catch (...) {
        return IndexRelationType::None;
    }

    if (common.isNull() || common.isEmpty()) {
        return IndexRelationType::None;
    }

    const double edgeTolerance = qMax(seriesIndexAdjacencyTolerance(), 1e-12);
    constexpr double areaNumericalTolerance = 1e-8;
    if (common.area() <= areaNumericalTolerance && common.length() > edgeTolerance) {
        return IndexRelationType::Adjacent;
    }

    if (common.area() > areaNumericalTolerance) {
        return IndexRelationType::Overlap;
    }

    return IndexRelationType::IntersectsOnly;
}

bool SeriesMapEdgeMatchTaskDialog::isAdjacent(const QgsGeometry& left, const QgsGeometry& right) const
{
    return indexRelationType(left, right) == IndexRelationType::Adjacent;
}

bool SeriesMapEdgeMatchTaskDialog::isConfiguredSheetRelationValid(
    const SheetInfo& mainInfo,
    const SheetInfo& assocInfo,
    int mainScale,
    int assocScale) const
{
    return evaluateConfiguredFrameRelation(mainInfo.geometry, mainScale,
        assocInfo.geometry, assocScale, nullptr);
}

bool SeriesMapEdgeMatchTaskDialog::evaluateConfiguredFrameRelation(
    const QgsGeometry& mainGeometry,
    int mainScale,
    const QgsGeometry& assocGeometry,
    int assocScale,
    QString* reason) const
{
    const IndexRelationType relation = indexRelationType(mainGeometry, assocGeometry);
    if (relation == IndexRelationType::None) {
        if (reason) *reason = QStringLiteral("真实图廓无空间交集");
        return false;
    }
    if (relation == IndexRelationType::IntersectsOnly) {
        if (reason) *reason = QStringLiteral("真实图廓仅点状相交，不构成可检查的接边或面重叠");
        return false;
    }
    if (relation == IndexRelationType::Adjacent) {
        if (reason) *reason = QStringLiteral("真实图廓邻接");
        return true;
    }
    if (mainScale <= 0 || assocScale <= 0) {
        if (reason) *reason = QStringLiteral("存在真实图廓面重叠，但比例尺缺失，无法判断检查适用性");
        return false;
    }
    if (mainScale == assocScale) {
        if (reason) *reason = QStringLiteral("同比例尺真实图廓存在面重叠，不属于当前接边检查范围");
        return false;
    }

    const QgsGeometry& largerScaleGeometry = mainScale < assocScale ? mainGeometry : assocGeometry;
    const QgsGeometry intersection = mainGeometry.intersection(assocGeometry);
    const double largerScaleArea = largerScaleGeometry.area();
    const double overlapRatio = largerScaleArea > 0.0 ? intersection.area() / largerScaleArea : 0.0;
    const double overlapThreshold = qBound(0.0, seriesCrossScaleOverlapThreshold(), 1.0);
    if (overlapRatio >= overlapThreshold) {
        if (reason) *reason = QStringLiteral("跨比例尺真实图廓覆盖比例 %1").arg(overlapRatio, 0, 'f', 3);
        return true;
    }
    if (reason) {
        *reason = QStringLiteral("存在跨比例尺真实图廓面重叠，但覆盖比例 %1 未达到检查阈值 %2")
            .arg(overlapRatio, 0, 'f', 3).arg(overlapThreshold, 0, 'f', 3);
    }
    return false;
}

bool SeriesMapEdgeMatchTaskDialog::validateSeriesCrsAndUnits(const QStringList& shpDirs, QString* error) const
{
    if (!m_sheetIndexCrs.isValid()) {
        if (error) *error = QStringLiteral("图幅索引缺少有效坐标参考，无法保证接边距离单位正确。");
        return false;
    }
    if (m_sheetIndexCrs.mapUnits() != QgsUnitTypes::DistanceMeters) {
        if (error) *error = QStringLiteral("图幅索引坐标单位不是米，当前单位为：%1。请先统一转换到米制投影坐标系。")
            .arg(QgsUnitTypes::toString(m_sheetIndexCrs.mapUnits()));
        return false;
    }

    for (const QString& dirPath : shpDirs) {
        const QString framePath = QDir(dirPath.trimmed()).filePath(QStringLiteral("内图廓.shp"));
        QgsVectorLayer frame(framePath, QStringLiteral("series_frame_crs_check"), QStringLiteral("ogr"));
        if (!frame.isValid()) {
            if (error) *error = QStringLiteral("无法加载 GeoPDF 预处理内图廓用于 CRS 校验：%1").arg(framePath);
            return false;
        }
        const QgsCoordinateReferenceSystem crs = frame.crs();
        if (!crs.isValid()) {
            if (error) *error = QStringLiteral("GeoPDF 预处理内图廓缺少有效坐标参考：%1").arg(framePath);
            return false;
        }
        if (crs.mapUnits() != QgsUnitTypes::DistanceMeters) {
            if (error) *error = QStringLiteral("GeoPDF 预处理内图廓坐标单位不是米：%1。请先统一转换到米制投影坐标系。").arg(framePath);
            return false;
        }
        const bool sameCrs = (!m_sheetIndexCrs.authid().isEmpty() && m_sheetIndexCrs.authid() == crs.authid())
            || m_sheetIndexCrs.toWkt() == crs.toWkt();
        if (!sameCrs) {
            if (error) *error = QStringLiteral("图幅索引与 GeoPDF 预处理内图廓 CRS 不一致：%1（索引=%2，GeoPDF=%3）。")
                .arg(framePath, m_sheetIndexCrs.authid(), crs.authid());
            return false;
        }
    }

    return true;
}

static int parseScaleFromSheetName(const QString& name) {
    QRegularExpression re(QStringLiteral("^[A-Z][0-9]{2}([A-K])[0-9]{6}"));
    QRegularExpressionMatch match = re.match(name.toUpper());
    if (match.hasMatch()) {
        QString code = match.captured(1);
        if (code == QStringLiteral("A")) return 1000000;
        if (code == QStringLiteral("B")) return 500000;
        if (code == QStringLiteral("C")) return 250000;
        if (code == QStringLiteral("D")) return 100000;
        if (code == QStringLiteral("E")) return 50000;
        if (code == QStringLiteral("F")) return 25000;
        if (code == QStringLiteral("G")) return 10000;
        if (code == QStringLiteral("H")) return 5000;
        if (code == QStringLiteral("I")) return 2000;
        if (code == QStringLiteral("J")) return 1000;
        if (code == QStringLiteral("K")) return 500;
    }
    return 0;
}

static QStringList scaleFieldNames()
{
    return QStringList()
        << QStringLiteral("比例尺")
        << QStringLiteral("地图比例尺")
        << QStringLiteral("SCALE")
        << QStringLiteral("Scale")
        << QStringLiteral("scale")
        << QStringLiteral("MAP_SCALE")
        << QStringLiteral("MapScale")
        << QStringLiteral("map_scale")
        << QStringLiteral("SCAL")
        << QStringLiteral("SCALE_DEN")
        << QStringLiteral("SCALE_DENO")
        << QStringLiteral("DENOM")
        << QStringLiteral("DENOMINATR");
}

static bool isScaleFieldName(const QString& fieldName)
{
    const QString trimmed = fieldName.trimmed();
    for (const QString& candidate : scaleFieldNames()) {
        if (trimmed.compare(candidate, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static int parseScaleDenominator(const QString& rawValue)
{
    QString value = rawValue.trimmed();
    if (value.isEmpty()) {
        return 0;
    }

    value.replace(QStringLiteral("："), QStringLiteral(":"));
    value.replace(QStringLiteral("∶"), QStringLiteral(":"));
    value.replace(QStringLiteral("／"), QStringLiteral("/"));

    auto parsePositiveDenominator = [](QString numberText) -> int {
        numberText.remove(QRegularExpression(QStringLiteral("[,，\\s]")));
        bool ok = false;
        const int denom = numberText.toInt(&ok);
        return ok && denom >= 500 ? denom : 0;
    };

    QRegularExpression ratioRe(QStringLiteral("^\\s*1\\s*[:/]\\s*([0-9][0-9,，\\s]*)\\s*$"));
    QRegularExpressionMatch ratioMatch = ratioRe.match(value);
    if (ratioMatch.hasMatch()) {
        return parsePositiveDenominator(ratioMatch.captured(1));
    }

    QRegularExpression denominatorRe(QStringLiteral("^\\s*([0-9][0-9,，\\s]*)\\s*$"));
    QRegularExpressionMatch denominatorMatch = denominatorRe.match(value);
    if (denominatorMatch.hasMatch()) {
        return parsePositiveDenominator(denominatorMatch.captured(1));
    }

    return 0;
}

static int resolveScaleFromFeature(const QgsFeature& feature, const QgsFields& fields, const QString& sheetName)
{
    for (int i = 0; i < fields.count(); ++i) {
        if (!isScaleFieldName(fields.at(i).name())) {
            continue;
        }

        const int scale = parseScaleDenominator(feature.attribute(i).toString());
        if (scale > 0) {
            return scale;
        }
    }

    return parseScaleFromSheetName(sheetName);
}

static int parseSeriesScaleDenominatorText(const QString& text)
{
    QString t = text.trimmed();
    if (t.isEmpty()) return 0;
    
    if (t.startsWith(QStringLiteral("比例尺"))) {
        t = t.mid(3).trimmed();
    }
    
    t.replace(QStringLiteral("："), QStringLiteral(":"));
    t.remove(QRegularExpression(QStringLiteral("[,，\\s]")));
    
    QRegularExpression re(QStringLiteral("^(?:1:)?(\\d+)$"));
    QRegularExpressionMatch match = re.match(t);
    if (match.hasMatch()) {
        return match.captured(1).toInt();
    }
    return 0;
}

static int resolveSeriesScaleFromLegendPolygonShp(const QString& shpPath)
{
    QgsVectorLayer layer(shpPath, QStringLiteral("legend"), QStringLiteral("ogr"));
    if (!layer.isValid()) return 0;
    
    int textIdx = layer.fields().lookupField(QStringLiteral("text_conte"));
    if (textIdx < 0) return 0;
    
    QgsFeatureIterator it = layer.getFeatures();
    QgsFeature f;
    while (it.nextFeature(f)) {
        QString text = f.attribute(textIdx).toString();
        int scale = parseSeriesScaleDenominatorText(text);
        if (scale > 0) return scale;
    }
    return 0;
}

static int resolveSeriesScaleFromMapFrameShp(const QString& frameShpPath)
{
    QgsVectorLayer layer(frameShpPath, QStringLiteral("frame"), QStringLiteral("ogr"));
    if (!layer.isValid()) return 0;
    
    int widthIdx = layer.fields().lookupField(QStringLiteral("width"));
    if (widthIdx < 0) return 0;
    
    QgsFeatureIterator it = layer.getFeatures();
    QgsFeature f;
    if (it.nextFeature(f)) {
        double attrPixelWidth = f.attribute(widthIdx).toDouble();
        if (attrPixelWidth <= 0) return 0;
        
        QgsGeometry geom = f.geometry();
        if (geom.isNull()) return 0;
        
        QgsRectangle bbox = geom.boundingBox();
        double mapFrameWidth = bbox.width();
        if (mapFrameWidth <= 0) return 0;
        
        const double DPI = 72.0;
        const double METER_PER_INCH = 0.0254;
        double scaleDenominator = mapFrameWidth / (attrPixelWidth * (METER_PER_INCH / DPI));
        
        if (scaleDenominator > 0) {
            return qRound(scaleDenominator);
        }
    }
    return 0;
}

static int resolveSeriesScaleFromPreprocessedShpDir(const QString& shpDir, QString* source, QString* warning)
{
    if (source) source->clear();
    if (warning) warning->clear();
    
    int legendScale = 0;
    QDir dir(shpDir);
    QStringList legendFiles = dir.entryList(QStringList() << QStringLiteral("*_Polygon.shp"), QDir::Files);
    for (const QString& file : legendFiles) {
        legendScale = resolveSeriesScaleFromLegendPolygonShp(dir.filePath(file));
        if (legendScale > 0) break;
    }
    
    int frameScale = resolveSeriesScaleFromMapFrameShp(dir.filePath(QStringLiteral("内图廓.shp")));
    
    if (legendScale > 0 && frameScale > 0) {
        if (legendScale == frameScale) {
            if (source) *source = QStringLiteral("legend+frame");
            return legendScale;
        } else {
            if (warning) *warning = QStringLiteral("图例比例尺与内图廓计算比例尺不一致");
            return 0;
        }
    } else if (legendScale > 0) {
        if (source) *source = QStringLiteral("legend");
        if (warning) *warning = QStringLiteral("仅通过图例识别到比例尺");
        return legendScale;
    } else if (frameScale > 0) {
        if (source) *source = QStringLiteral("frame");
        if (warning) *warning = QStringLiteral("仅通过内图廓计算识别到比例尺");
        return frameScale;
    }
    
    return 0;
}

static QStringList collectGeoPdfFiles(const QString& inputPath)
{
    QStringList pdfFiles;
    QFileInfo fi(inputPath);

    if (fi.isFile() && fi.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
        pdfFiles << fi.absoluteFilePath();
    } else if (fi.isDir()) {
        QDirIterator it(inputPath, QStringList() << QStringLiteral("*.pdf"), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            pdfFiles << it.next();
        }
    }

    pdfFiles.removeDuplicates();
    return pdfFiles;
}

static QStringList collectCsvFiles(const QString& inputPath)
{
    QStringList csvFiles;
    QFileInfo fi(inputPath);

    if (fi.isFile() && fi.suffix().compare(QStringLiteral("csv"), Qt::CaseInsensitive) == 0) {
        csvFiles << fi.absoluteFilePath();
    } else if (fi.isDir()) {
        QDirIterator it(inputPath, QStringList() << QStringLiteral("*.csv"), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            csvFiles << it.next();
        }
    }

    csvFiles.removeDuplicates();
    return csvFiles;
}

static int parseScaleString(const QString& text)
{
    QString s = text.trimmed();
    if (s.isEmpty()) return 0;

    QRegularExpression re(QStringLiteral("^(?:比例尺\\s*)?(?:1\\s*[:：]\\s*)?([1-9]\\d*(?:\\s*\\d+)*)$"));
    QRegularExpressionMatch match = re.match(s);
    if (match.hasMatch()) {
        QString numStr = match.captured(1);
        numStr.remove(QChar(' '));
        return numStr.toInt();
    }
    return 0;
}

static int inferScaleDenominatorFromShpDir(const QString& shpDir)
{
    if (shpDir.isEmpty() || !QDir(shpDir).exists()) return 0;

    QStringList priorities = {
        QStringLiteral("图例_Polygon.shp"),
        QStringLiteral("比例尺_Polygon.shp"),
        QStringLiteral("注记_Polygon.shp"),
        QStringLiteral("整饰_Polygon.shp")
    };

    QDir dir(shpDir);
    QStringList allShps = dir.entryList(QStringList() << QStringLiteral("*.shp"), QDir::Files);
    
    QStringList candidateShps;
    for (const QString& p : priorities) {
        for (const QString& shp : allShps) {
            if (shp.compare(p, Qt::CaseInsensitive) == 0) {
                candidateShps << dir.absoluteFilePath(shp);
            }
        }
    }
    
    for (const QString& shp : allShps) {
        if (shp.endsWith(QStringLiteral("_Polygon.shp"), Qt::CaseInsensitive)) {
            QString absPath = dir.absoluteFilePath(shp);
            if (!candidateShps.contains(absPath)) {
                candidateShps << absPath;
            }
        }
    }
    
    for (const QString& shp : allShps) {
        QString absPath = dir.absoluteFilePath(shp);
        if (!candidateShps.contains(absPath)) {
            candidateShps << absPath;
        }
    }

    QSet<int> foundScales;
    QRegularExpression scaleRe(QStringLiteral("1\\s*[:：]\\s*([1-9]\\d*(?:\\s*\\d+)*)"));
    QRegularExpression fallbackRe(QStringLiteral("比例尺\\s*([1-9]\\d*(?:\\s*\\d+)*)"));

    for (const QString& shpPath : candidateShps) {
        QgsVectorLayer layer(shpPath, QStringLiteral("temp"), QStringLiteral("ogr"));
        if (!layer.isValid()) continue;

        int textFieldIndex = -1;
        const QgsFields fields = layer.fields();
        for (int i = 0; i < fields.count(); ++i) {
            QString fieldName = fields.at(i).name().toLower();
            if (fieldName == QStringLiteral("text_conte") || fieldName == QStringLiteral("text_content")) {
                textFieldIndex = i;
                break;
            }
        }

        if (textFieldIndex < 0) continue;

        QgsFeatureIterator it = layer.getFeatures();
        QgsFeature f;
        
        while (it.nextFeature(f)) {
            QString text = f.attribute(textFieldIndex).toString().trimmed();
            if (text.isEmpty()) continue;
            
            QRegularExpressionMatch match = scaleRe.match(text);
            if (match.hasMatch()) {
                QString denomStr = match.captured(1);
                denomStr.remove(QChar(' '));
                denomStr.remove(QChar(','));
                bool ok;
                int denom = denomStr.toInt(&ok);
                if (ok && denom > 0) {
                    foundScales.insert(denom);
                }
            } else if (text.contains(QStringLiteral("比例尺"))) {
                QRegularExpressionMatch fallbackMatch = fallbackRe.match(text);
                if (fallbackMatch.hasMatch()) {
                    QString denomStr = fallbackMatch.captured(1);
                    denomStr.remove(QChar(' '));
                    denomStr.remove(QChar(','));
                    bool ok;
                    int denom = denomStr.toInt(&ok);
                    if (ok && denom > 0) {
                        foundScales.insert(denom);
                    }
                }
            }
        }
        
        if (!foundScales.isEmpty()) {
            break; // Found in this SHP, no need to check lower priority SHPs
        }
    }

    if (foundScales.size() == 1) {
        return *foundScales.begin();
    }
    
    return foundScales.size() > 1 ? -1 : 0; // -1 for conflict, 0 for not found
}

static QString resolveShpDirForSheetName(
    const QString& sheetName,
    const QMap<QString, QString>& pdfBaseNameToShpDir
)
{
    QString normalizedSheet = sheetName.trimmed();

    if (pdfBaseNameToShpDir.contains(normalizedSheet)) {
        return pdfBaseNameToShpDir.value(normalizedSheet);
    }

    for (auto it = pdfBaseNameToShpDir.constBegin(); it != pdfBaseNameToShpDir.constEnd(); ++it) {
        if (it.key().contains(normalizedSheet, Qt::CaseInsensitive)) {
            return it.value();
        }
    }

    for (auto it = pdfBaseNameToShpDir.constBegin(); it != pdfBaseNameToShpDir.constEnd(); ++it) {
        if (normalizedSheet.contains(it.key(), Qt::CaseInsensitive)) {
            return it.value();
        }
    }

    return QString();
}

QStringList SeriesMapEdgeMatchTaskDialog::resolveGeoPdfSearchRoots(const QString& indexPath) const
{
    QStringList roots;

    // 1. 优先用分组配置区里已经填过的 GeoPDF 文件/文件夹
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        QStringList configuredPaths;
        if (auto* pdfEdit = configLineEdit(i, SeriesColMainMap)) {
            configuredPaths << pdfEdit->text().trimmed();
        }
        if (auto* assocEdit = configLineEdit(i, SeriesColAssocMap)) {
            configuredPaths << splitSeriesTableList(assocEdit->text().trimmed());
        }
        for (const QString& path : configuredPaths) {
            if (!path.isEmpty()) {
                QFileInfo fi(path);
                if (fi.isFile()) {
                    roots << fi.absolutePath();
                } else if (fi.isDir()) {
                    roots << fi.absoluteFilePath();
                }
            }
        }
    }

    // 2. 一期配置里保存的 GeoPDF 路径或 GeoPDF 根目录
    const QJsonObject snapshot = map_check_task_manager::loadPhaseOneConfigSnapshot();
    QString phaseOnePdfPath = snapshot.value(QStringLiteral("pdf_path")).toString().trimmed();
    if (!phaseOnePdfPath.isEmpty()) {
        QFileInfo fi(phaseOnePdfPath);
        if (fi.isFile()) {
            roots << fi.absolutePath();
        } else if (fi.isDir()) {
            roots << fi.absoluteFilePath();
        }
    }

    // 3. 图幅索引 shp 所在目录、上一级目录
    if (!indexPath.isEmpty()) {
        QFileInfo fi(indexPath);
        QDir dir = fi.absoluteDir();
        roots << dir.absolutePath();
        if (dir.cdUp()) {
            roots << dir.absolutePath();
        }
    }

    // 4. 结果输出目录
    QString outRoot = outputRootDir();
    if (!outRoot.isEmpty()) {
        roots << outRoot;
    }

    // 去重
    QStringList uniqueRoots;
    for (const QString& root : roots) {
        QString normalized = QDir::cleanPath(root);
        if (!uniqueRoots.contains(normalized, Qt::CaseInsensitive)) {
            uniqueRoots << normalized;
        }
    }

    return uniqueRoots;
}

static QString normalizeGeoPdfSheetToken(QString text)
{
    text = QFileInfo(text.trimmed()).completeBaseName();
    text.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—]+")));
    while (text.endsWith(QStringLiteral("地图"), Qt::CaseInsensitive)) {
        text.chop(2);
    }
    return text;
}

static bool isLikelySeriesScaleSuffix(const QString& suffix)
{
    QString normalized = suffix.trimmed();
    normalized.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）]+")));
    if (normalized.isEmpty()) return false;

    static const QRegularExpression scaleSuffixRe(
        QStringLiteral("^(?:1[:：/]?)?[1-9]\\d*(?:[wW万千kK])?$"));
    return scaleSuffixRe.match(normalized).hasMatch();
}

static bool isAllowedSeriesMapNameSuffix(QString suffix)
{
    suffix = suffix.trimmed();
    suffix.remove(QRegularExpression(QStringLiteral("^[\\s　_\\-—:：()（）]+")));
    if (suffix.isEmpty()) return false;
    if (isLikelySeriesScaleSuffix(suffix)) return true;

    QString compact = suffix;
    compact.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）]+")));
    if (compact == QStringLiteral("地图")) return true;

    if (compact.startsWith(QStringLiteral("地图"), Qt::CaseInsensitive)) {
        const QString tail = compact.mid(QStringLiteral("地图").length());
        if (tail.isEmpty()) return true;
        static const QRegularExpression mapVariantRe(
            QStringLiteral("^(?:新版|新版本|修订版|更新版|最终版|终版|v|V|version|Version)?[0-9A-Za-z一二三四五六七八九十]*$"));
        return mapVariantRe.match(tail).hasMatch();
    }

    static const QRegularExpression versionSuffixRe(
        QStringLiteral("^(?:新版|新版本|修订版|更新版|最终版|终版|v|V|version|Version)[0-9A-Za-z一二三四五六七八九十]*$"));
    return versionSuffixRe.match(compact).hasMatch();
}

static int parseSeriesScaleSuffix(QString suffix)
{
    suffix = suffix.trimmed();
    suffix.remove(QRegularExpression(QStringLiteral("^[\\s　_\\-—:：()（）]+")));
    suffix.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）]+")));
    if (suffix.startsWith(QStringLiteral("地图"), Qt::CaseInsensitive)) {
        suffix = suffix.mid(QStringLiteral("地图").length());
    }

    QRegularExpression scaleRe(QStringLiteral("^(?:1[:：/]?)?([1-9]\\d{2,})([wW万千kK]?)$"));
    QRegularExpressionMatch match = scaleRe.match(suffix);
    if (!match.hasMatch()) return 0;

    bool ok = false;
    int value = match.captured(1).toInt(&ok);
    if (!ok || value <= 0) return 0;

    const QString unit = match.captured(2);
    if (unit.compare(QStringLiteral("w"), Qt::CaseInsensitive) == 0 || unit == QStringLiteral("万")) {
        value *= 10000;
    } else if (unit.compare(QStringLiteral("k"), Qt::CaseInsensitive) == 0 || unit == QStringLiteral("千")) {
        value *= 1000;
    }
    return value >= 500 ? value : 0;
}

static int parseSeriesScaleFromMatchedPdfName(const QString& sheetName, const QString& pdfBaseName)
{
    const QString sheet = normalizeGeoPdfSheetToken(sheetName);
    const QString base = normalizeGeoPdfSheetToken(pdfBaseName);
    if (sheet.isEmpty() || base.isEmpty()) return 0;
    if (!base.startsWith(sheet, Qt::CaseInsensitive)) return 0;
    return parseSeriesScaleSuffix(base.mid(sheet.length()));
}

static bool geoPdfBaseMatchesSheetStrict(const QString& sheetName, const QString& pdfBaseName)
{
    const QString sheet = normalizeGeoPdfSheetToken(sheetName);
    const QString base = normalizeGeoPdfSheetToken(pdfBaseName);
    if (sheet.isEmpty() || base.isEmpty()) return false;
    if (base.compare(sheet, Qt::CaseInsensitive) == 0) return true;
    if (!base.startsWith(sheet, Qt::CaseInsensitive)) return false;
    return isAllowedSeriesMapNameSuffix(base.mid(sheet.length()));
}

static QStringList findStrictGeoPdfForSheetName(const QString& sheetName, const QStringList& pdfPaths)
{
    QStringList matches;
    for (const QString& pdfPath : pdfPaths) {
        if (geoPdfBaseMatchesSheetStrict(sheetName, QFileInfo(pdfPath).completeBaseName())) {
            matches << pdfPath;
        }
    }
    matches.removeDuplicates();
    return matches;
}

static QString findCsvForSheetName(const QString& sheetName, const QStringList& csvPaths)
{
    if (csvPaths.isEmpty()) return QString();
    
    QString normalizedSheet = sheetName.trimmed();
    for (const QString& csvPath : csvPaths) {
        QFileInfo fi(csvPath);
        if (fi.completeBaseName().compare(normalizedSheet, Qt::CaseInsensitive) == 0) {
            return csvPath;
        }
    }
    for (const QString& csvPath : csvPaths) {
        QFileInfo fi(csvPath);
        if (fi.completeBaseName().contains(normalizedSheet, Qt::CaseInsensitive)) {
            return csvPath;
        }
    }
    for (const QString& csvPath : csvPaths) {
        QFileInfo fi(csvPath);
        if (normalizedSheet.contains(fi.completeBaseName(), Qt::CaseInsensitive)) {
            return csvPath;
        }
    }
    return csvPaths.first();
}

QStringList SeriesMapEdgeMatchTaskDialog::collectConfiguredGeoPdfs() const
{
    QStringList allPdfPaths;

    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* rowItem = m_configTree->topLevelItem(i);
        if (!rowItem || rowItem->text(SeriesColStatus) != QStringLiteral("已完成")) continue;
        
        auto* mainEdit = configLineEdit(i, SeriesColMainMap);
        auto* assocEdit = configLineEdit(i, SeriesColAssocMap);
        
        if (mainEdit) {
            QString path = mainEdit->text().trimmed();
            if (!path.isEmpty()) {
                QStringList pdfs = collectGeoPdfFiles(path);
                for (const QString& p : pdfs) {
                    if (!allPdfPaths.contains(p)) allPdfPaths << p;
                }
            }
        }
        if (assocEdit) {
            QStringList paths = splitSeriesTableList(assocEdit->text().trimmed());
            for (const QString& path : paths) {
                QStringList pdfs = collectGeoPdfFiles(path);
                for (const QString& p : pdfs) {
                    if (!allPdfPaths.contains(p)) allPdfPaths << p;
                }
            }
        }
    }
    return allPdfPaths;
}

void SeriesMapEdgeMatchTaskDialog::updateLoadRelationProgress(int value, const QString& text)
{
    if (m_progressBar) {
        m_progressBar->setValue(value);
        m_progressBar->setFormat(text);
        QCoreApplication::processEvents();
    }
}

QString SeriesMapEdgeMatchTaskDialog::preprocessedShpFingerprint(const QString& shpDir) const
{
    QDir dir(shpDir);
    if (!dir.exists() || !QFileInfo(dir.filePath(QStringLiteral("内图廓.shp"))).isFile()) return QString();

    QStringList inputFiles;
    const QStringList frameComponents{QStringLiteral("内图廓.shp"), QStringLiteral("内图廓.shx"),
        QStringLiteral("内图廓.dbf"), QStringLiteral("内图廓.prj"), QStringLiteral("内图廓.cpg")};
    for (const QString& component : frameComponents) {
        const QString path = dir.filePath(component);
        if (QFileInfo(path).isFile()) inputFiles << path;
    }

    const QStringList legendShapes = dir.entryList(QStringList{QStringLiteral("*_Polygon.shp")}, QDir::Files, QDir::Name);
    for (const QString& shape : legendShapes) {
        const QString baseName = QFileInfo(shape).completeBaseName();
        for (const QString& extension : {QStringLiteral("shp"), QStringLiteral("shx"), QStringLiteral("dbf"),
                 QStringLiteral("prj"), QStringLiteral("cpg")}) {
            const QString path = dir.filePath(baseName + QLatin1Char('.') + extension);
            if (QFileInfo(path).isFile()) inputFiles << path;
        }
    }
    inputFiles.removeDuplicates();
    inputFiles.sort(Qt::CaseSensitive);

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData("series-frame-index-v2\0", 22);
    hash.addData(QDir::cleanPath(QFileInfo(shpDir).absoluteFilePath()).toUtf8());
    for (const QString& path : inputFiles) {
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly)) return QString();
        hash.addData(QDir(shpDir).relativeFilePath(path).toUtf8());
        hash.addData("\0", 1);
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(1024 * 1024);
            if (chunk.isEmpty() && input.error() != QFileDevice::NoError) return QString();
            hash.addData(chunk);
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonArray SeriesMapEdgeMatchTaskDialog::mergedIndexSourceSnapshot(
    const QStringList& pdfPaths,
    QMap<QString, QString>* resolvedDirs) const
{
    QJsonArray snapshot;
    if (resolvedDirs) resolvedDirs->clear();
    for (const QString& pdfPath : pdfPaths) {
        const QFileInfo pdfInfo(pdfPath);
        if (!pdfInfo.isFile()) return QJsonArray();
        const QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
        if (shpDir.isEmpty()) return QJsonArray();
        const QString fingerprint = preprocessedShpFingerprint(shpDir);
        if (fingerprint.isEmpty()) return QJsonArray();

        const QString normalizedPdf = QDir::cleanPath(QFileInfo(pdfPath).absoluteFilePath());
        const QString normalizedShpDir = QDir::cleanPath(QFileInfo(shpDir).absoluteFilePath());
        QJsonObject obj;
        obj[QStringLiteral("pdf_path")] = normalizedPdf;
        obj[QStringLiteral("pdf_size")] = pdfInfo.size();
        obj[QStringLiteral("pdf_last_modified")] = pdfInfo.lastModified().toString(Qt::ISODate);
        obj[QStringLiteral("shp_dir")] = normalizedShpDir;
        obj[QStringLiteral("source_fingerprint")] = fingerprint;
        snapshot.append(obj);
        if (resolvedDirs) (*resolvedDirs)[normalizedPdf] = normalizedShpDir;
    }

    return snapshot;
}

bool SeriesMapEdgeMatchTaskDialog::tryUseCachedMergedIndex(const QStringList& pdfPaths, QString* indexPath)
{
    const QJsonArray currentSources = mergedIndexSourceSnapshot(pdfPaths);
    if (currentSources.size() != pdfPaths.size()) return false;

    const QString cacheDir = QDir(outputRootDir()).filePath(QStringLiteral(".series_cache"));
    const QString manifestPath = QDir(cacheDir).filePath(QStringLiteral("merged_index_manifest.json"));
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("cache_schema")).toInt() != 2) return false;
    if (QJsonDocument(root.value(QStringLiteral("sources")).toArray()).toJson(QJsonDocument::Compact)
        != QJsonDocument(currentSources).toJson(QJsonDocument::Compact)) return false;

    const QString cachedIndexPath = root.value(QStringLiteral("merged_index")).toString();
    if (cachedIndexPath.isEmpty() || !QFileInfo(cachedIndexPath).isFile()) return false;
    QgsVectorLayer layer(cachedIndexPath, QStringLiteral("series_cached_frame_index"), QStringLiteral("ogr"));
    if (!layer.isValid() || layer.featureCount() != pdfPaths.size()) return false;

    if (indexPath) *indexPath = cachedIndexPath;
    return true;
}

bool SeriesMapEdgeMatchTaskDialog::saveMergedIndexCache(
    const QStringList& pdfPaths,
    const QString& indexPath,
    const QJsonArray& sourceSnapshot)
{
    if (sourceSnapshot.size() != pdfPaths.size()) return false;
    const QString cacheDir = QDir(outputRootDir()).filePath(QStringLiteral(".series_cache"));
    if (!QDir().mkpath(cacheDir)) return false;
    const QString manifestPath = QDir(cacheDir).filePath(QStringLiteral("merged_index_manifest.json"));

    QJsonArray pdfsArray;
    for (const QString& pdfPath : pdfPaths) {
        const QFileInfo fi(pdfPath);
        QJsonObject obj;
        obj[QStringLiteral("path")] = QDir::cleanPath(fi.absoluteFilePath());
        obj[QStringLiteral("size")] = fi.size();
        obj[QStringLiteral("last_modified")] = fi.lastModified().toString(Qt::ISODate);
        pdfsArray.append(obj);
    }

    QJsonObject root;
    root[QStringLiteral("cache_schema")] = 2;
    root[QStringLiteral("pdfs")] = pdfsArray;
    root[QStringLiteral("sources")] = sourceSnapshot;
    root[QStringLiteral("merged_index")] = indexPath;

    QSaveFile file(manifestPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray serialized = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(serialized) != serialized.size()) return false;
    return file.commit();
}

bool SeriesMapEdgeMatchTaskDialog::buildMergedIndexFromGeoPdfs(const QStringList& pdfPaths, QString* indexPath)
{
    if (pdfPaths.isEmpty()) return false;
    const QString cacheDir = QDir(outputRootDir()).filePath(QStringLiteral(".series_cache"));
    if (!QDir().mkpath(cacheDir)) return false;

    for (const QString& pdfPath : pdfPaths) {
        if (!resolvePreprocessedShpDir(pdfPath, outputRootDir()).isEmpty()) continue;
        const PreprocessResult result = preprocessGeoPdfIfNeeded(pdfPath, outputRootDir());
        if (!result.success) {
            appendRuntimeLog(QStringLiteral("无法准备 %1 的图廓来源：%2").arg(pdfPath, result.errorMessage));
            return false;
        }
    }

    const QJsonArray sourceSnapshot = mergedIndexSourceSnapshot(pdfPaths);
    if (sourceSnapshot.size() != pdfPaths.size()) {
        appendRuntimeLog(QStringLiteral("无法为所有 GeoPDF 建立稳定的 SHP 图廓来源指纹，已停止建立关系缓存。"));
        return false;
    }
    const QByteArray snapshotBytes = QJsonDocument(sourceSnapshot).toJson(QJsonDocument::Compact);
    const QString indexFingerprint = QString::fromLatin1(
        QCryptographicHash::hash(snapshotBytes, QCryptographicHash::Sha256).toHex().left(20));
    const QString mergedIndexPath = QDir(cacheDir).filePath(QStringLiteral("merged_index_v2_%1_%2.shp")
        .arg(indexFingerprint, QUuid::createUuid().toString(QUuid::WithoutBraces)));

    QgsFields indexFields;
    indexFields.append(QgsField(QStringLiteral("SheetName"), QVariant::String));
    indexFields.append(QgsField(QStringLiteral("SCALE_DEN"), QVariant::Int));
    QgsVectorFileWriter* writer = nullptr;
    QgsCoordinateReferenceSystem indexCrs;
    auto removeStagedIndex = [&]() {
        const QFileInfo info(mergedIndexPath);
        const QString base = info.path() + QLatin1Char('/') + info.completeBaseName();
        for (const QString& extension : {QStringLiteral("shp"), QStringLiteral("shx"), QStringLiteral("dbf"),
                 QStringLiteral("prj"), QStringLiteral("cpg"), QStringLiteral("qix")}) {
            QFile::remove(base + QLatin1Char('.') + extension);
        }
    };

    for (int i = 0; i < pdfPaths.size(); ++i) {
        const QString& pdfPath = pdfPaths[i];
        const QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
        if (shpDir.isEmpty()) {
            appendRuntimeLog(QStringLiteral("无法确定 %1 使用的 SHP 图层目录。").arg(pdfPath));
            removeStagedIndex();
            return false;
        }
        updateLoadRelationProgress(20 + (i + 1) * 40 / pdfPaths.size(),
            QStringLiteral("读取真实图廓 (%1/%2): %3").arg(i + 1).arg(pdfPaths.size()).arg(QFileInfo(pdfPath).fileName()));

        QgsGeometry frameGeometry;
        QgsCoordinateReferenceSystem frameCrs;
        QString frameError;
        if (!readPreprocessedFrameGeometry(shpDir, &frameGeometry, &frameCrs, &frameError)) {
            appendRuntimeLog(QStringLiteral("%1 图廓不可用于关系判断：%2")
                .arg(QFileInfo(pdfPath).completeBaseName(), frameError));
            removeStagedIndex();
            return false;
        }
        if (!indexCrs.isValid()) indexCrs = frameCrs;
        else if (frameCrs != indexCrs) {
            appendRuntimeLog(QStringLiteral("%1 图廓坐标系与其他图幅不同，未建立混合坐标的关系索引。")
                .arg(QFileInfo(pdfPath).completeBaseName()));
            removeStagedIndex();
            return false;
        }

        if (!writer) {
            writer = new QgsVectorFileWriter(mergedIndexPath, QStringLiteral("UTF-8"), indexFields,
                QgsWkbTypes::Polygon, indexCrs, QStringLiteral("ESRI Shapefile"));
            if (writer->hasError() != QgsVectorFileWriter::NoError) {
                delete writer;
                writer = nullptr;
                removeStagedIndex();
                return false;
            }
        }
        QgsFeature outputFeature(indexFields);
        outputFeature.setGeometry(frameGeometry);
        outputFeature.setAttribute(QStringLiteral("SheetName"), QFileInfo(pdfPath).completeBaseName());
        QString scaleSource;
        QString scaleWarning;
        const int scale = resolveSeriesScaleFromPreprocessedShpDir(shpDir, &scaleSource, &scaleWarning);
        outputFeature.setAttribute(QStringLiteral("SCALE_DEN"), scale > 0 ? QVariant(scale) : QVariant());
        if (!scaleWarning.isEmpty()) {
            appendRuntimeLog(QStringLiteral("%1 比例尺识别提示：%2")
                .arg(QFileInfo(pdfPath).completeBaseName(), scaleWarning));
        }
        if (!writer->addFeature(outputFeature)) {
            delete writer;
            writer = nullptr;
            removeStagedIndex();
            appendRuntimeLog(QStringLiteral("写入图幅关系索引失败：%1").arg(QFileInfo(pdfPath).completeBaseName()));
            return false;
        }
        appendRuntimeLog(QStringLiteral("图幅 %1 的关系范围来自 %2")
            .arg(QFileInfo(pdfPath).completeBaseName(), QDir(shpDir).filePath(QStringLiteral("内图廓.shp"))));
    }

    if (writer) { delete writer; writer = nullptr; }
    QgsVectorLayer builtIndex(mergedIndexPath, QStringLiteral("series_built_frame_index"), QStringLiteral("ogr"));
    if (!builtIndex.isValid() || builtIndex.featureCount() != pdfPaths.size()) {
        removeStagedIndex();
        return false;
    }
    const QJsonArray latestSourceSnapshot = mergedIndexSourceSnapshot(pdfPaths);
    if (QJsonDocument(latestSourceSnapshot).toJson(QJsonDocument::Compact)
        != QJsonDocument(sourceSnapshot).toJson(QJsonDocument::Compact)) {
        removeStagedIndex();
        appendRuntimeLog(QStringLiteral("建立图廓索引期间输入 SHP 已变化，未发布这份索引。"));
        return false;
    }
    if (!saveMergedIndexCache(pdfPaths, mergedIndexPath, sourceSnapshot)) return false;
    if (indexPath) *indexPath = mergedIndexPath;
    return true;
}
bool SeriesMapEdgeMatchTaskDialog::loadSheetIndex(const QString& indexPath)
{
    QVector<SheetInfo> loadedSheetInfos;
    QgsCoordinateReferenceSystem indexCrs;
    if (!readSheetInfosFromIndex(indexPath, &loadedSheetInfos, &indexCrs)) {
        return false;
    }

    m_sheetInfos = loadedSheetInfos;
    m_sheetIndexCrs = indexCrs;
    return true;
}

bool SeriesMapEdgeMatchTaskDialog::readSheetInfosFromIndex(
    const QString& indexPath,
    QVector<SheetInfo>* sheetInfos,
    QgsCoordinateReferenceSystem* crs) const
{
    // OGR can retain a pooled handle after the underlying file is removed.
    if (!sheetInfos || !QFileInfo(indexPath).isFile()) return false;

    QgsVectorLayer layer(indexPath, QStringLiteral("series_sheet_index"), QStringLiteral("ogr"));
    if (!layer.isValid()) {
        return false;
    }

    sheetInfos->clear();
    if (crs) *crs = layer.crs();
    QgsFeature feature;
    QgsFeatureIterator it = layer.getFeatures();
    while (it.nextFeature(feature)) {
        if (!feature.hasGeometry()) {
            continue;
        }

        SeriesMapEdgeMatchTaskDialog::SheetInfo info;
        info.featureId = feature.id();
        info.displayName = resolveSheetName(feature, layer.fields());
        info.geometry = feature.geometry();
        info.crs = layer.crs();
        info.scale = resolveScaleFromFeature(feature, layer.fields(), info.displayName);
        sheetInfos->push_back(info);
    }
    return !sheetInfos->isEmpty();
}

void SeriesMapEdgeMatchTaskDialog::refreshSheetList()
{
    int unrecognizedScaleCount = 0;
    for (const SeriesMapEdgeMatchTaskDialog::SheetInfo& info : m_sheetInfos) {
        if (info.scale <= 0) {
            ++unrecognizedScaleCount;
        }
    }

    appendRuntimeLog(
        QStringLiteral("图幅索引解析完成：%1 幅，未识别比例尺：%2 幅，同比例尺对：%3 对，跨比例尺组：%4 组")
        .arg(m_sheetInfos.size())
        .arg(unrecognizedScaleCount)
        .arg(m_adjacentPairs.size())
        .arg(m_coverageGroups.size()));
}

bool SeriesMapEdgeMatchTaskDialog::loadSheets(bool validateRealFrameRelations, QStringList* removedMainMapNames)
{
    if (removedMainMapNames) removedMainMapNames->clear();
    if (m_loadingSheets || (m_runningCheck && !validateRealFrameRelations)) return false;
    const QScopedValueRollback<bool> loading(m_loadingSheets,true);
    const bool wasEnabled=isEnabled();
    setEnabled(false);
    const auto restoreUi=qScopeGuard([this,wasEnabled]() { setEnabled(wasEnabled); });
    // Failed reloads must not leave geometry available for a later check.
    clearLoadedSheetRelations();

    updateLoadRelationProgress(0, QStringLiteral("准备加载图幅关系..."));
    
    QMap<QString, int> userScales;
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        auto* pdfEdit = configLineEdit(i, SeriesColMainMap);
        auto* infoEdit = configLineEdit(i, SeriesColMainScale);
        if (pdfEdit && infoEdit) {
            QString pdfPath = pdfEdit->text().trimmed();
            QString infoText = infoEdit->text().trimmed();
            if (!pdfPath.isEmpty() && !infoText.isEmpty() && !infoText.contains(QStringLiteral("自动解析"))) {
                int scale = parseScaleString(infoText);
                if (scale > 0) {
                    userScales[QFileInfo(pdfPath).completeBaseName()] = scale;
                }
            }
        }
        auto* assocEdit = configLineEdit(i, SeriesColAssocMap);
        auto* assocScaleEdit = configLineEdit(i, SeriesColAssocScale);
        if (assocEdit && assocScaleEdit) {
            const QStringList assocPaths = splitSeriesTableList(assocEdit->text().trimmed());
            const QStringList assocScaleTexts = splitSeriesTableList(assocScaleEdit->text().trimmed());
            for (int j = 0; j < assocPaths.size(); ++j) {
                if (j >= assocScaleTexts.size()) continue;
                const int scale = parseScaleString(assocScaleTexts[j]);
                if (scale > 0) {
                    userScales[QFileInfo(assocPaths[j]).completeBaseName()] = scale;
                }
            }
        }
    }

    QString indexPath = m_indexFileEdit->text().trimmed();

    if (indexPath.isEmpty()) {
        m_outputEdit->clear();
        m_outputEdit->append(QStringLiteral("未提供外部图幅索引，开始预处理 GeoPDF 以提取内图廓..."));

        updateLoadRelationProgress(10, QStringLiteral("收集 GeoPDF..."));
        QStringList allPdfPaths = collectConfiguredGeoPdfs();

        if (allPdfPaths.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先配置 GeoPDF 文件或提供外部图幅索引。"));
            updateLoadRelationProgress(0, QStringLiteral(""));
            return false;
        }

        if (tryUseCachedMergedIndex(allPdfPaths, &indexPath)) {
            m_outputEdit->append(QStringLiteral("复用缓存的图幅索引：%1").arg(indexPath));
            updateLoadRelationProgress(60, QStringLiteral("复用缓存的图幅索引..."));
        } else {
            if (!buildMergedIndexFromGeoPdfs(allPdfPaths, &indexPath)) {
                QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法从 GeoPDF 提取内图廓，请检查预处理日志或提供外部图幅索引。"));
                updateLoadRelationProgress(0, QStringLiteral(""));
                return false;
            }
            m_outputEdit->append(QStringLiteral("已成功从 GeoPDF 提取内图廓并构建索引。"));
        }
    }

    updateLoadRelationProgress(75, QStringLiteral("读取图幅索引..."));
    
    m_adjacentPairs.clear();
    m_taskChoices.clear();
    m_outputEdit->clear();
    m_lastErrorRecords.clear();
    m_originalErrorRecords.clear();
    m_pairRunContext.clear();
    m_checkedLayersByMap.clear();
    m_originalStats = QJsonObject();
    m_originalStats.insert(QStringLiteral("check_completed"), false);

    if (!loadSheetIndex(indexPath)) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("图幅索引文件无法加载，请检查 shp 文件是否有效。"));
        updateLoadRelationProgress(0, QStringLiteral(""));
        return false;
    }
    
    for (auto& info : m_sheetInfos) {
        if (info.scale <= 0 && userScales.contains(info.displayName)) {
            info.scale = userScales[info.displayName];
        }
    }

    refreshSheetList();

    updateLoadRelationProgress(85, QStringLiteral("自动填充分组配置..."));
    QStringList searchRoots = resolveGeoPdfSearchRoots(indexPath);
    if (!searchRoots.isEmpty()) {
        autoPopulateConfigRowsFromSheetInfos(searchRoots);
    }
    if (validateRealFrameRelations) {
        const QStringList removedNames = synchronizeConfigRowsWithRealFrameRelations();
        if (removedMainMapNames) *removedMainMapNames = removedNames;
        m_previewMode = PreviewMode::Configured;
    }
    
    updateSheetScalesFromConfigTable();
    rebuildPairList();
    checkTableStatus();
    updatePairCheckStates();
    
    updateSelectedCount();
    updateLoadRelationProgress(100, QStringLiteral("图幅关系加载完成"));
    return true;
}

int SeriesMapEdgeMatchTaskDialog::scaleForGeoPdf(const QString& pdfPath) const
{
    const QString baseName = QFileInfo(pdfPath).completeBaseName();
    auto it = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
        return geoPdfBaseMatchesSheetStrict(info.displayName, baseName);
    });
    if (it != m_sheetInfos.end()) {
        const int nameScale = parseSeriesScaleFromMatchedPdfName(it->displayName, baseName);
        if (nameScale > 0) return nameScale;
        if (it->scale > 0) return it->scale;
    }

    const QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
    if (!shpDir.isEmpty()) {
        const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
        if (inferredScale > 0) return inferredScale;
    }
    return 0;
}

bool SeriesMapEdgeMatchTaskDialog::readPreprocessedFrameGeometry(
    const QString& shpDir,
    QgsGeometry* geometry,
    QgsCoordinateReferenceSystem* crs,
    QString* error) const
{
    if (!geometry) return false;
    *geometry = QgsGeometry();
    const QString framePath = QDir(shpDir).filePath(QStringLiteral("内图廓.shp"));
    QgsVectorLayer layer(framePath, QStringLiteral("series_normalized_frame"), QStringLiteral("ogr"));
    if (!layer.isValid()) {
        if (error) *error = QStringLiteral("无法打开内图廓图层：%1").arg(framePath);
        return false;
    }
    if (!layer.crs().isValid() || layer.crs().mapUnits() != QgsUnitTypes::DistanceMeters) {
        if (error) *error = QStringLiteral("内图廓缺少有效的米制投影坐标系：%1").arg(framePath);
        return false;
    }

    double largestArea = 0.0;
    QgsFeature feature;
    QgsFeatureIterator features = layer.getFeatures();
    while (features.nextFeature(feature)) {
        QgsGeometry candidate = feature.geometry();
        if (candidate.isNull() || candidate.isEmpty()) continue;

        if (candidate.type() == QgsWkbTypes::LineGeometry) {
            bool singleClosedRing = false;
            QgsPolylineXY ring;
            if (candidate.isMultipart()) {
                const QgsMultiPolylineXY parts = candidate.asMultiPolyline();
                if (parts.size() == 1) {
                    ring = parts.first();
                    singleClosedRing = ring.size() >= 4 && ring.first().distance(ring.last()) <= 0.01;
                }
            } else {
                ring = candidate.asPolyline();
                singleClosedRing = ring.size() >= 4 && ring.first().distance(ring.last()) <= 0.01;
            }
            if (singleClosedRing) {
                ring.last() = ring.first();
                candidate = QgsGeometry::fromPolygonXY(QgsPolygonXY{ring});
            } else {
                candidate = QgsGeometry::polygonize(QVector<QgsGeometry>{candidate});
            }
        }

        if (candidate.isNull() || candidate.isEmpty()) continue;
        if (!candidate.isGeosValid()) candidate = candidate.makeValid();
        if (candidate.type() == QgsWkbTypes::GeometryCollection) {
            candidate.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
        }
        if (candidate.isNull() || candidate.isEmpty() || candidate.type() != QgsWkbTypes::PolygonGeometry) continue;

        if (candidate.isMultipart()) {
            const QgsMultiPolygonXY parts = candidate.asMultiPolygon();
            QgsGeometry largestPart;
            double partArea = 0.0;
            for (const QgsPolygonXY& part : parts) {
                QgsGeometry polygon = QgsGeometry::fromPolygonXY(part);
                if (polygon.area() > partArea) {
                    partArea = polygon.area();
                    largestPart = polygon;
                }
            }
            candidate = largestPart;
        }
        if (candidate.isNull() || !candidate.isGeosValid() || candidate.area() <= largestArea) continue;
        largestArea = candidate.area();
        *geometry = candidate;
    }

    if (geometry->isNull() || geometry->isEmpty() || largestArea <= 1e-8) {
        if (error) *error = QStringLiteral("内图廓无法构造成有效面，未用外包矩形替代：%1").arg(framePath);
        return false;
    }
    if (crs) *crs = layer.crs();
    return true;
}

bool SeriesMapEdgeMatchTaskDialog::readGeoPdfFrameGeometry(
    const QString& pdfPath,
    QgsGeometry* geometry,
    QString* error)
{
    if (!geometry) return false;
    QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
    if (shpDir.isEmpty()) {
        const PreprocessResult result = preprocessGeoPdfIfNeeded(pdfPath, outputRootDir());
        if (!result.success) {
            if (error) *error = QStringLiteral("无法准备真实图廓：%1").arg(result.errorMessage);
            return false;
        }
        shpDir = result.shpDir;
    }
    return readPreprocessedFrameGeometry(shpDir, geometry, nullptr, error);
}
bool SeriesMapEdgeMatchTaskDialog::applyRealFrameScope(
    const QStringList& leftDirs,
    const QStringList& rightDirs,
    AlgorithmFilePaths* paths,
    QJsonObject* scope,
    QString* error) const
{
    if (!paths || !scope) return false;
    *scope = QJsonObject{{QStringLiteral("source"), QStringLiteral("preprocessed_geopdf_inner_frames")}};
    if (!validateSeriesCrsAndUnits(leftDirs + rightDirs, error)) return false;

    auto readSide = [&](const QStringList& dirs, QgsGeometry& combined, QStringList& wkts, const QString& side) {
        QJsonArray framePaths;
        for (const QString& dir : dirs) {
            QgsGeometry frame;
            QgsCoordinateReferenceSystem frameCrs;
            if (!readPreprocessedFrameGeometry(dir, &frame, &frameCrs, error)) return false;
            if (frameCrs != m_sheetIndexCrs) {
                if (error) *error = QStringLiteral("执行图廓与关系索引的坐标系不一致：%1")
                    .arg(QDir(dir).filePath(QStringLiteral("内图廓.shp")));
                return false;
            }
            const QString framePath = QDir(dir).filePath(QStringLiteral("内图廓.shp"));
            wkts << frame.asWkt();
            framePaths.append(framePath);
            combined = combined.isNull() ? frame : combined.combine(frame);
        }
        (*scope)[side + QStringLiteral("_frame_paths")] = framePaths;
        return !combined.isNull() && !combined.isEmpty() && combined.area() > 1e-8;
    };

    QgsGeometry left, right;
    QStringList leftWkts, rightWkts;
    if (!readSide(leftDirs, left, leftWkts, QStringLiteral("left"))
        || !readSide(rightDirs, right, rightWkts, QStringLiteral("right"))) return false;

    paths->seriesLeftSheetWkt = left.asWkt();
    paths->seriesRightSheetWkt = right.asWkt();
    if (paths->checkMode == QStringLiteral("CrossScale")) {
        paths->seriesLargeScaleSheetWkts = leftWkts;
        paths->seriesSmallScaleSheetWkt = right.asWkt();
    }
    QgsGeometry common = left.intersection(right);
    if (paths->checkMode == QStringLiteral("CrossScale")) {
        common.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
    }
    (*scope)[QStringLiteral("left_area_m2")] = left.area();
    (*scope)[QStringLiteral("right_area_m2")] = right.area();
    (*scope)[QStringLiteral("common_area_m2")] = common.area();
    (*scope)[QStringLiteral("common_wkt")] = common.asWkt();
    if (paths->checkMode == QStringLiteral("CrossScale")
        && (common.isNull() || common.isEmpty() || common.type() != QgsWkbTypes::PolygonGeometry
            || !common.isGeosValid() || common.area() <= 1e-8)) {
        if (error) *error = QStringLiteral("真实图廓没有有效的共同重叠面，不能执行跨比例尺覆盖检查");
        return false;
    }
    return true;
}
void SeriesMapEdgeMatchTaskDialog::recordPairExecution(const QString& pairName, const CheckItemResult& result)
{
    QJsonObject context = m_pairRunContext.value(pairName);
    context[QStringLiteral("result_status")] = result.status;
    context[QStringLiteral("result_err_exist")] = result.err_exist;
    context[QStringLiteral("result_info")] = result.info;
    if (result.status != QStringLiteral("质检成功")) context[QStringLiteral("execution_errors")] = result.errors;
    m_pairRunContext[pairName] = context;
    QStringList sheets{context.value(QStringLiteral("left_name")).toString(),context.value(QStringLiteral("right_name")).toString(),
        context.value(QStringLiteral("small_name")).toString()};
    sheets << context.value(QStringLiteral("large_names")).toVariant().toStringList();
    QStringList categories = context.value(QStringLiteral("confirmed_layer_mapping")).toObject().value(QStringLiteral("layers")).toObject().keys();
    if (categories.isEmpty()) categories = seriesCheckTargetLayers(context.value(QStringLiteral("xml_path")).toString(),context.value(QStringLiteral("scale")).toInt());
    if (categories.isEmpty()) categories << QStringLiteral("图幅接边与系列化表达");
    for (const QString& sheet : sheets) if (!sheet.isEmpty())
        for (const QString& category : categories) m_checkedLayersByMap[sheet].insert(category);
}

QJsonObject SeriesMapEdgeMatchTaskDialog::executionSummaryForMap(const QString& mapName, const QString& layerName) const
{
    int complete = 0, failed = 0, skipped = 0, partial = 0, errorPairs = 0; QJsonArray records;
    for (auto it = m_pairRunContext.constBegin(); it != m_pairRunContext.constEnd(); ++it) {
        QJsonObject context = it.value();
        if (!mapName.isEmpty() && context.value(QStringLiteral("left_name")).toString() != mapName
            && context.value(QStringLiteral("right_name")).toString() != mapName
            && context.value(QStringLiteral("small_name")).toString() != mapName
            && !context.value(QStringLiteral("large_names")).toArray().contains(mapName)) continue;
        QString status = context.value(QStringLiteral("result_status")).toString();
        if (!layerName.isEmpty() && status != QStringLiteral("质检失败")) {
            const QJsonObject info = context.value(QStringLiteral("result_info")).toObject();
            const QJsonObject summaries = info.value(QStringLiteral("geometry_summary")).toObject();
            QString key = layerName;
            if (!summaries.contains(key)) {
                const QJsonObject expanded = info.value(QStringLiteral("expanded_target_layers")).toObject();
                for (auto e = expanded.constBegin(); e != expanded.constEnd(); ++e)
                    if (e.value().toArray().contains(layerName)) { key = e.key(); break; }
            }
            QString state = summaries.value(key).toObject().value(QStringLiteral("execution_state")).toString();
            if (key != layerName && state == QStringLiteral("completed")) {
                const QString requested = SeriesLayerMapping::legacyLayerKey(layerName);
                const bool crossScale = info.value(QStringLiteral("rule_source")).toObject().value(QStringLiteral("check_mode")).toString() == QStringLiteral("CrossScale")
                    || context.contains(QStringLiteral("small_name"));
                bool checked = false;
                if (crossScale) {
                    const QJsonArray evidence = summaries.value(key).toObject().value(QStringLiteral("support_evidence_records")).toArray();
                    for (const QJsonValue& record : evidence)
                        if (SeriesLayerMapping::legacyLayerKey(record.toObject().value(QStringLiteral("small_scale_shp")).toString()) == requested) { checked = true; break; }
                } else {
                    const QJsonArray reads = info.value(QStringLiteral("layer_path_summary")).toObject().value(key).toObject().value(QStringLiteral("feature_reads")).toArray();
                    for (const QJsonValue& read : reads) {
                        const QJsonObject entry = read.toObject();
                        if (SeriesLayerMapping::legacyLayerKey(entry.value(QStringLiteral("path")).toString()) == requested
                            && entry.value(QStringLiteral("in_scope_candidates")).toInt() > 0) { checked = true; break; }
                    }
                }
                if (!checked) state = QStringLiteral("no_applicable_features");
            }
            status = state == QStringLiteral("completed") ? QStringLiteral("质检成功") : QStringLiteral("未执行");
        }
        if (status == QStringLiteral("质检成功")) ++complete;
        else if (status == QStringLiteral("质检失败")) ++failed;
        else if (status == QStringLiteral("部分完成")) ++partial;
        else ++skipped;
        if (context.value(QStringLiteral("result_err_exist")).toBool()) ++errorPairs;
        context[QStringLiteral("pair_name")] = it.key(); records.append(context);
    }
    const QString status = failed ? QStringLiteral("质检失败") : partial ? QStringLiteral("部分完成") : skipped
        ? (complete ? QStringLiteral("部分完成") : QStringLiteral("未执行")) : QStringLiteral("质检成功");
    QString description;
    if (failed || skipped || partial) description = QStringLiteral("本次%1个任务中，%2个完成，%3个失败，%4个部分完成，%5个未执行；不能据此判定检查通过。")
        .arg(records.size()).arg(complete).arg(failed).arg(partial).arg(skipped);
    return QJsonObject{{QStringLiteral("status"),status},{QStringLiteral("description"),description},
        {QStringLiteral("pair_count"),records.size()},{QStringLiteral("executed_pair_count"),complete},
        {QStringLiteral("failed_pair_count"),failed},{QStringLiteral("skipped_pair_count"),skipped},
        {QStringLiteral("partial_pair_count"),partial},{QStringLiteral("error_pair_count"),errorPairs},
        {QStringLiteral("execution_records"),records}};
}

bool SeriesMapEdgeMatchTaskDialog::hasRealGeoPdfFrameRelation(
    const QString& mainPdfPath,
    int mainScale,
    const QString& assocPdfPath,
    int assocScale,
    QString* reason)
{
    QgsGeometry mainGeometry;
    QString readError;
    if (!readGeoPdfFrameGeometry(mainPdfPath, &mainGeometry, &readError)) {
        if (reason) *reason = readError;
        return false;
    }
    QgsGeometry assocGeometry;
    if (!readGeoPdfFrameGeometry(assocPdfPath, &assocGeometry, &readError)) {
        if (reason) *reason = readError;
        return false;
    }
    return evaluateConfiguredFrameRelation(mainGeometry, mainScale, assocGeometry, assocScale, reason);
}

bool SeriesMapEdgeMatchTaskDialog::hasRealGeoPdfFrameRelationFromMergedIndex(
    const QVector<SheetInfo>& frameSheetInfos,
    const QString& mainPdfPath,
    int mainScale,
    const QString& assocPdfPath,
    int assocScale,
    QString* reason) const
{
    const QString mainBaseName = QFileInfo(mainPdfPath).completeBaseName();
    const QString assocBaseName = QFileInfo(assocPdfPath).completeBaseName();
    auto findFrameInfo = [&](const QString& pdfBaseName) {
        return std::find_if(frameSheetInfos.cbegin(), frameSheetInfos.cend(), [&](const SheetInfo& info) {
            return geoPdfBaseMatchesSheetStrict(info.displayName, pdfBaseName);
        });
    };

    const auto mainIt = findFrameInfo(mainBaseName);
    if (mainIt == frameSheetInfos.cend()) {
        if (reason) *reason = QStringLiteral("无法在真实图廓索引中找到主图：%1").arg(mainBaseName);
        return false;
    }
    const auto assocIt = findFrameInfo(assocBaseName);
    if (assocIt == frameSheetInfos.cend()) {
        if (reason) *reason = QStringLiteral("无法在真实图廓索引中找到关联图：%1").arg(assocBaseName);
        return false;
    }

    const int resolvedMainScale = mainScale > 0 ? mainScale : mainIt->scale;
    const int resolvedAssocScale = assocScale > 0 ? assocScale : assocIt->scale;
    return evaluateConfiguredFrameRelation(
        mainIt->geometry, resolvedMainScale, assocIt->geometry, resolvedAssocScale, reason);
}
bool SeriesMapEdgeMatchTaskDialog::buildRealFrameSheetInfosForConfiguredPdfs(QVector<SheetInfo>* frameSheetInfos, QString* error)
{
    if (!frameSheetInfos) return false;
    frameSheetInfos->clear();

    QStringList configuredPdfs;
    for (int row = 0; m_configTree && row < m_configTree->topLevelItemCount(); ++row) {
        if (auto* mainEdit = configLineEdit(row, SeriesColMainMap)) {
            const QString mainPdf = mainEdit->text().trimmed();
            if (!mainPdf.isEmpty() && !configuredPdfs.contains(mainPdf)) configuredPdfs << mainPdf;
        }
        if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
            for (const QString& assocPdf : splitSeriesTableList(assocEdit->text().trimmed())) {
                if (!assocPdf.isEmpty() && !configuredPdfs.contains(assocPdf)) configuredPdfs << assocPdf;
            }
        }
    }

    if (configuredPdfs.isEmpty()) {
        if (error) *error = QStringLiteral("没有可用于构建真实图廓索引的 GeoPDF。");
        return false;
    }

    QString mergedIndexPath;
    if (!tryUseCachedMergedIndex(configuredPdfs, &mergedIndexPath)) {
        if (!buildMergedIndexFromGeoPdfs(configuredPdfs, &mergedIndexPath)) {
            if (error) *error = QStringLiteral("无法从 GeoPDF 构建 merged_index.shp。");
            return false;
        }
    } else {
        appendRuntimeLog(QStringLiteral("复用真实图廓 merged_index.shp：%1").arg(mergedIndexPath));
    }

    if (!readSheetInfosFromIndex(mergedIndexPath, frameSheetInfos)) {
        if (error) *error = QStringLiteral("无法读取真实图廓 merged_index.shp：%1").arg(mergedIndexPath);
        return false;
    }

    // Retain the PDF identity for preview lookup. A repeated basename is
    // ambiguous, so those frames are read from their individual PDF sources.
    for (SheetInfo& info : *frameSheetInfos) {
        QStringList matchingPaths;
        for (const QString& pdf : configuredPdfs)
            if (QFileInfo(pdf).completeBaseName().compare(info.displayName, Qt::CaseInsensitive) == 0)
                matchingPaths << pdf;
        if (matchingPaths.size() == 1) info.pdfPath = matchingPaths.first();
    }

    appendRuntimeLog(QStringLiteral("真实图廓 merged_index.shp 已准备完成：%1 幅。").arg(frameSheetInfos->size()));
    return true;
}

QStringList SeriesMapEdgeMatchTaskDialog::filterAssociatedPdfsByRealFrameRelation(
    const QString& mainPdfPath,
    const QStringList& candidateAssocPdfs,
    int mainScaleOverride,
    const QMap<QString, int>& assocScaleOverrides,
    const QVector<SheetInfo>* realFrameSheetInfos)
{
    QStringList confirmedPdfs;
    const int mainScale = mainScaleOverride > 0 ? mainScaleOverride : scaleForGeoPdf(mainPdfPath);
    int applicableCount = 0;
    int filteredCount = 0;
    int retainedForReviewCount = 0;

    for (const QString& candidatePdf : candidateAssocPdfs) {
        const QString assocPdf = candidatePdf.trimmed();
        if (assocPdf.isEmpty() || assocPdf == mainPdfPath) continue;
        const QString assocPathKey = QDir::cleanPath(QFileInfo(assocPdf).absoluteFilePath());
        const QString assocBaseKey = QFileInfo(assocPdf).completeBaseName();
        int assocScale = assocScaleOverrides.value(assocPathKey, 0);
        if (assocScale <= 0) assocScale = assocScaleOverrides.value(assocBaseKey, 0);
        if (assocScale <= 0) assocScale = scaleForGeoPdf(assocPdf);

        QString reason;
        const bool applicable = (realFrameSheetInfos && !realFrameSheetInfos->isEmpty())
            ? hasRealGeoPdfFrameRelationFromMergedIndex(*realFrameSheetInfos, mainPdfPath, mainScale, assocPdf, assocScale, &reason)
            : hasRealGeoPdfFrameRelation(mainPdfPath, mainScale, assocPdf, assocScale, &reason);
        if (applicable) {
            if (!confirmedPdfs.contains(assocPdf)) confirmedPdfs << assocPdf;
            ++applicableCount;
            continue;
        }

        const bool spatialOverlapNeedsReview =
            reason.contains(QStringLiteral("存在真实图廓面重叠"))
            || reason.contains(QStringLiteral("存在跨比例尺真实图廓面重叠"))
            || reason.contains(QStringLiteral("同比例尺真实图廓存在面重叠"));
        const bool relationCouldNotBeDetermined =
            reason.contains(QStringLiteral("无法"))
            || reason.contains(QStringLiteral("缺少"))
            || reason.contains(QStringLiteral("缺失"))
            || reason.contains(QStringLiteral("未能"));
        if (spatialOverlapNeedsReview || relationCouldNotBeDetermined) {
            if (!confirmedPdfs.contains(assocPdf)) confirmedPdfs << assocPdf;
            ++retainedForReviewCount;
            appendRuntimeLog(QStringLiteral("保留待确认关系：%1 与 %2；%3")
                .arg(QFileInfo(mainPdfPath).completeBaseName(), QFileInfo(assocPdf).completeBaseName(), reason));
            continue;
        }

        ++filteredCount;
        appendRuntimeLog(QStringLiteral("无可检查的图廓关系，已移除候选：%1 与 %2；%3")
            .arg(QFileInfo(mainPdfPath).completeBaseName(), QFileInfo(assocPdf).completeBaseName(), reason));
    }

    appendRuntimeLog(QStringLiteral("真实图廓判断：%1 个可检查，%2 个保留待确认，%3 个无空间关系")
        .arg(applicableCount).arg(retainedForReviewCount).arg(filteredCount));
    return confirmedPdfs;
}
void SeriesMapEdgeMatchTaskDialog::autoPopulateConfigRowsFromSheetInfos(const QStringList& searchRoots)
{
    if (searchRoots.isEmpty() || m_sheetInfos.isEmpty()) return;
    const bool useExternalCandidateIndex = hasExternalSheetIndex();

    appendRuntimeLog(QStringLiteral("GeoPDF搜索目录："));
    for (const QString& root : searchRoots) {
        appendRuntimeLog(root);
    }

    QStringList allPdfFiles;
    for (const QString& root : searchRoots) {
        QStringList pdfs = collectGeoPdfFiles(root);
        for (const QString& pdf : pdfs) {
            if (!allPdfFiles.contains(pdf)) {
                allPdfFiles << pdf;
            }
        }
    }
    
    appendRuntimeLog(QStringLiteral("\n共找到 GeoPDF：%1 个").arg(allPdfFiles.size()));

    if (allPdfFiles.isEmpty()) return;

    auto findSheetInfoForPdf = [&](const QString& pdfPath) {
        const QString baseName = QFileInfo(pdfPath).completeBaseName();
        return std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
            return geoPdfBaseMatchesSheetStrict(info.displayName, baseName);
        });
    };

    auto scaleForPdf = [&](const QString& pdfPath) {
        const QString baseName = QFileInfo(pdfPath).completeBaseName();
        auto it = findSheetInfoForPdf(pdfPath);
        if (it != m_sheetInfos.end()) {
            const int nameScale = parseSeriesScaleFromMatchedPdfName(it->displayName, baseName);
            if (nameScale > 0) return nameScale;
        }
        QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
        if (!shpDir.isEmpty()) {
            const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
            if (inferredScale > 0) {
                auto sheetIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                    return geoPdfBaseMatchesSheetStrict(info.displayName, baseName);
                });
                if (sheetIt != m_sheetInfos.end()) sheetIt->scale = inferredScale;
                return inferredScale;
            }
        }
        if (it != m_sheetInfos.end() && it->scale > 0) return it->scale;
        return 0;
    };

    auto relatedPdfsForSheet = [&](const SheetInfo& mainInfo) {
        QStringList assocPdfs;
        for (const SheetInfo& otherInfo : m_sheetInfos) {
            if (mainInfo.featureId == otherInfo.featureId) continue;
            const IndexRelationType relation = indexRelationType(mainInfo.geometry, otherInfo.geometry);
            if (relation == IndexRelationType::Adjacent || relation == IndexRelationType::Overlap) {
                QStringList matchedAssocPdfs = findStrictGeoPdfForSheetName(otherInfo.displayName, allPdfFiles);
                if (!matchedAssocPdfs.isEmpty()) {
                    assocPdfs << matchedAssocPdfs;
                }
            }
        }
        assocPdfs.removeDuplicates();
        return assocPdfs;
    };

    auto hasExistingMainConfigRows = [&]() {
        for (int row = 0; m_configTree && row < m_configTree->topLevelItemCount(); ++row) {
            QLineEdit* mainEdit = configLineEdit(row, SeriesColMainMap);
            if (mainEdit && !mainEdit->text().trimmed().isEmpty()) return true;
        }
        return false;
    };

    auto populateExistingConfigRowsFromSheetInfos = [&]() {
        for (int row = 0; m_configTree && row < m_configTree->topLevelItemCount(); ++row) {
            QLineEdit* mainEdit = configLineEdit(row, SeriesColMainMap);
            if (!mainEdit || mainEdit->text().trimmed().isEmpty()) continue;
            const QString mainPdf = mainEdit->text().trimmed();
            auto mainIt = findSheetInfoForPdf(mainPdf);
            if (mainIt == m_sheetInfos.end()) continue;

            if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
                QStringList manualAssocPdfs = splitSeriesTableList(assocEdit->text().trimmed());
                QStringList assocPdfs = manualAssocPdfs;
                const int manualMainScale = parseScaleString(
                    configLineEdit(row, SeriesColMainScale) ? configLineEdit(row, SeriesColMainScale)->text().trimmed() : QString());
                QMap<QString, int> assocScaleOverrides;
                const QStringList assocScaleTexts = configLineEdit(row, SeriesColAssocScale)
                    ? splitSeriesTableList(configLineEdit(row, SeriesColAssocScale)->text().trimmed())
                    : QStringList();
                for (int i = 0; i < manualAssocPdfs.size() && i < assocScaleTexts.size(); ++i) {
                    const int scale = parseScaleString(assocScaleTexts[i]);
                    if (scale <= 0) continue;
                    const QString assocPath = manualAssocPdfs[i].trimmed();
                    assocScaleOverrides[QDir::cleanPath(QFileInfo(assocPath).absoluteFilePath())] = scale;
                    assocScaleOverrides[QFileInfo(assocPath).completeBaseName()] = scale;
                }
                if (assocPdfs.isEmpty()) {
                    assocPdfs = relatedPdfsForSheet(*mainIt);
                    assocPdfs.removeAll(mainPdf);
                    if (!useExternalCandidateIndex) {
                        assocPdfs = filterAssociatedPdfsByRealFrameRelation(mainPdf, assocPdfs);
                    } else {
                        // External candidate index mode: keep administrative-boundary candidates for later task validation.
                    }
                    assocEdit->setText(assocPdfs.join(QStringLiteral(";")));
                } else {
                    assocPdfs.removeAll(mainPdf);
                    const QStringList originalAssocPdfs = assocPdfs;
                    QStringList confirmedManualAssocPdfs;
                    QStringList confirmedAssocScales;
                    QStringList removedAssocNames;
                    for (const QString& assocPdf : assocPdfs) {
                        const QString assocPathKey = QDir::cleanPath(QFileInfo(assocPdf).absoluteFilePath());
                        const QString assocBaseKey = QFileInfo(assocPdf).completeBaseName();
                        int scale = assocScaleOverrides.value(assocPathKey, 0);
                        if (scale <= 0) scale = assocScaleOverrides.value(assocBaseKey, 0);
                        if (scale <= 0) scale = scaleForPdf(assocPdf);

                        auto assocIt = findSheetInfoForPdf(assocPdf);
                        if (assocIt != m_sheetInfos.end()) {
                            const SheetInfo& assocInfo = *assocIt;
                            if (isConfiguredSheetRelationValid(*mainIt, assocInfo, manualMainScale, scale)
                                || indexRelationType(mainIt->geometry, assocInfo.geometry) == IndexRelationType::Overlap) {
                                confirmedManualAssocPdfs << assocPdf;
                                confirmedAssocScales << (scale > 0 ? QString::number(scale) : QString());
                                continue;
                            }
                        }
                        removedAssocNames << QFileInfo(assocPdf).completeBaseName();
                    }
                    assocPdfs = confirmedManualAssocPdfs;
                    assocEdit->setText(assocPdfs.join(QStringLiteral(";")));

                    if (auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale)) {
                        assocScaleEdit->setText(confirmedAssocScales.join(QStringLiteral(";")));
                    }

                    for (const QString& originalAssocPdf : originalAssocPdfs) {
                        if (!assocPdfs.contains(originalAssocPdf) &&
                            !removedAssocNames.contains(QFileInfo(originalAssocPdf).completeBaseName())) {
                            removedAssocNames << QFileInfo(originalAssocPdf).completeBaseName();
                        }
                    }
                    // Sync manual associated maps by removing entries without a valid sheet relation.
                    if (!removedAssocNames.isEmpty()) {
                        appendRuntimeLog(QStringLiteral("主图幅“%1”已移除无有效图幅关系的关联地图：%2。")
                            .arg(QFileInfo(mainPdf).completeBaseName(), removedAssocNames.join(QStringLiteral("、"))));
                    } else {
                        appendRuntimeLog(QStringLiteral("主图幅“%1”的人工关联地图均通过真实图廓校验。")
                            .arg(QFileInfo(mainPdf).completeBaseName()));
                    }
                }
                if (auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale); assocScaleEdit && assocScaleEdit->text().trimmed().isEmpty()) {
                    QStringList assocScales;
                    for (const QString& assocPdf : assocPdfs) {
                        const int scale = scaleForPdf(assocPdf);
                        assocScales << (scale > 0 ? QString::number(scale) : QString());
                    }
                    assocScaleEdit->setText(assocScales.join(QStringLiteral(";")));
                    if (assocScaleEdit->text().trimmed().isEmpty()) {
                        assocScaleEdit->setPlaceholderText(QStringLiteral("关联图比例尺未识别"));
                    }
                }
            }
            if (auto* mainScaleEdit = configLineEdit(row, SeriesColMainScale); mainScaleEdit && mainScaleEdit->text().trimmed().isEmpty()) {
                const int scale = scaleForPdf(mainPdf);
                if (scale > 0) {
                    mainScaleEdit->setText(QString::number(scale));
                } else {
                    mainScaleEdit->setPlaceholderText(QStringLiteral("主图比例尺未识别"));
                }
            }
            appendRuntimeLog(QStringLiteral("主图幅“%1”已完成外部索引校验，保留已有人工配置。").arg(QFileInfo(mainPdf).completeBaseName()));
        }
    };

    if (hasExistingMainConfigRows()) {
        populateExistingConfigRowsFromSheetInfos();
        checkTableStatus();
        return;
    }

    auto shouldSkipAutoMainSheet = [&](const SheetInfo& mainInfo) {
        for (const SheetInfo& otherInfo : m_sheetInfos) {
            if (mainInfo.featureId == otherInfo.featureId) continue;
            if (mainInfo.scale <= 0 || otherInfo.scale <= 0) continue;
            if (mainInfo.scale <= otherInfo.scale) continue;
            const IndexRelationType relation = indexRelationType(mainInfo.geometry, otherInfo.geometry);
            if (relation == IndexRelationType::Overlap) {
                const bool mainIsSmallScale = mainInfo.scale > otherInfo.scale;
                const QgsGeometry& largeScaleGeometry = mainIsSmallScale ? otherInfo.geometry : mainInfo.geometry;
                const double overlapRatio = largeScaleGeometry.area() > 0.0
                    ? mainInfo.geometry.intersection(otherInfo.geometry).area() / largeScaleGeometry.area() : 0.0;
                if (overlapRatio >= qBound(0.0, seriesCrossScaleOverlapThreshold(), 1.0)) return true;
            }
        }
        return false;
    };

    m_configTree->clear();

    for (const SheetInfo& mainInfo : m_sheetInfos) {
        if (shouldSkipAutoMainSheet(mainInfo)) {
            appendRuntimeLog(QStringLiteral("图幅“%1”为小比例尺覆盖图，自动生成时不作为主检查地图。").arg(mainInfo.displayName));
            continue;
        }

        QStringList matchedMainPdfs = findStrictGeoPdfForSheetName(mainInfo.displayName, allPdfFiles);
        if (matchedMainPdfs.isEmpty()) continue;

        if (matchedMainPdfs.size() > 1) {
            appendRuntimeLog(QStringLiteral("主图幅“%1”匹配到 %2 个 GeoPDF，将分别生成检查行。").arg(mainInfo.displayName).arg(matchedMainPdfs.size()));
        }

        for (const QString& mainPdf : matchedMainPdfs) {
            if (mainPdf.isEmpty()) continue;
            appendRuntimeLog(QStringLiteral("主图幅“%1”匹配到：%2").arg(mainInfo.displayName, mainPdf));

            QStringList assocPdfs;
            for (const SheetInfo& otherInfo : m_sheetInfos) {
                if (mainInfo.featureId == otherInfo.featureId) continue;
                const IndexRelationType relation = indexRelationType(mainInfo.geometry, otherInfo.geometry);
                if (relation == IndexRelationType::Adjacent || relation == IndexRelationType::Overlap) {
                    QStringList matchedAssocPdfs = findStrictGeoPdfForSheetName(otherInfo.displayName, allPdfFiles);
                    if (!matchedAssocPdfs.isEmpty()) {
                        assocPdfs << matchedAssocPdfs;
                    }
                }
            }
            assocPdfs.removeDuplicates();
            assocPdfs.removeAll(mainPdf);
            if (!useExternalCandidateIndex) {
                assocPdfs = filterAssociatedPdfsByRealFrameRelation(mainPdf, assocPdfs);
            } else {
                // External candidate index mode: keep administrative-boundary candidates for later task validation.
            }

            int row = addEmptyConfigRow();

            if (auto* mainEdit = configLineEdit(row, SeriesColMainMap)) {
                mainEdit->setText(mainPdf);
            }
            if (auto* mainScaleEdit = configLineEdit(row, SeriesColMainScale)) {
                const int scale = scaleForPdf(mainPdf);
                if (scale > 0) {
                    mainScaleEdit->setText(QString::number(scale));
                } else {
                    mainScaleEdit->setPlaceholderText(QStringLiteral("主图比例尺未识别"));
                }
            }
            if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
                assocEdit->setText(assocPdfs.join(QStringLiteral(";")));
            }
            if (auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale)) {
                QStringList assocScales;
                for (const QString& assocPdf : assocPdfs) {
                    const int scale = scaleForPdf(assocPdf);
                    assocScales << (scale > 0 ? QString::number(scale) : QString());
                }
                assocScaleEdit->setText(assocScales.join(QStringLiteral(";")));
                if (assocScaleEdit->text().trimmed().isEmpty()) {
                    assocScaleEdit->setPlaceholderText(QStringLiteral("关联图比例尺未识别"));
                }
            }
        }
    }
    
    checkTableStatus();
}

QStringList SeriesMapEdgeMatchTaskDialog::synchronizeConfigRowsWithRealFrameRelations()
{
    if (!hasExternalSheetIndex() || !m_configTree) return QStringList();

    QVector<SheetInfo> realFrameSheetInfos;
    QString realFrameError;
    QList<int> rowsWithoutConfirmedRelations;
    QStringList removedMainMapNames;
    if (!buildRealFrameSheetInfosForConfiguredPdfs(&realFrameSheetInfos, &realFrameError)) {
        appendRuntimeLog(QStringLiteral("无法确认真实图廓关系，暂保留候选：%1").arg(realFrameError));
        return QStringList();
    }

    for (int row = 0; row < m_configTree->topLevelItemCount(); ++row) {
        auto* mainEdit = configLineEdit(row, SeriesColMainMap);
        auto* mainScaleEdit = configLineEdit(row, SeriesColMainScale);
        auto* assocEdit = configLineEdit(row, SeriesColAssocMap);
        auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale);
        if (!mainEdit || !assocEdit) continue;

        const QString mainPdf = mainEdit->text().trimmed();
        QStringList assocPdfs = splitSeriesTableList(assocEdit->text().trimmed());
        if (mainPdf.isEmpty() || assocPdfs.isEmpty()) continue;

        int mainScale = mainScaleEdit ? parseScaleString(mainScaleEdit->text().trimmed()) : 0;
        if (mainScale <= 0) mainScale = scaleForGeoPdf(mainPdf);

        QMap<QString, int> assocScaleOverrides;
        const QStringList assocScaleTexts = assocScaleEdit ? splitSeriesTableList(assocScaleEdit->text().trimmed()) : QStringList();
        for (int i = 0; i < assocPdfs.size() && i < assocScaleTexts.size(); ++i) {
            const int scale = parseScaleString(assocScaleTexts[i]);
            if (scale <= 0) continue;
            const QString assocPdf = assocPdfs[i].trimmed();
            assocScaleOverrides[QDir::cleanPath(QFileInfo(assocPdf).absoluteFilePath())] = scale;
            assocScaleOverrides[QFileInfo(assocPdf).completeBaseName()] = scale;
        }

        const QStringList originalAssocPdfs = assocPdfs;
        QStringList confirmedAssocPdfs = filterAssociatedPdfsByRealFrameRelation(mainPdf, assocPdfs, mainScale, assocScaleOverrides, &realFrameSheetInfos);
        QStringList confirmedAssocScales;
        for (const QString& assocPdf : confirmedAssocPdfs) {
            const QString assocPathKey = QDir::cleanPath(QFileInfo(assocPdf).absoluteFilePath());
            const QString assocBaseKey = QFileInfo(assocPdf).completeBaseName();
            int assocScale = assocScaleOverrides.value(assocPathKey, 0);
            if (assocScale <= 0) assocScale = assocScaleOverrides.value(assocBaseKey, 0);
            if (assocScale <= 0) assocScale = scaleForGeoPdf(assocPdf);
            confirmedAssocScales << (assocScale > 0 ? QString::number(assocScale) : QString());
        }

        assocEdit->setText(confirmedAssocPdfs.join(QStringLiteral(";")));
        if (assocScaleEdit) {
            assocScaleEdit->setText(confirmedAssocScales.join(QStringLiteral(";")));
        }

        QStringList removedAssocNames;
        for (const QString& originalAssocPdf : originalAssocPdfs) {
            if (!confirmedAssocPdfs.contains(originalAssocPdf)) {
                removedAssocNames << QFileInfo(originalAssocPdf).completeBaseName();
            }
        }
        removedAssocNames.removeDuplicates();
        if (!removedAssocNames.isEmpty()) {
            appendRuntimeLog(QStringLiteral("主图幅“%1”已移除无真实图廓关系的候选关联地图：%2。")
                .arg(QFileInfo(mainPdf).completeBaseName(), removedAssocNames.join(QStringLiteral("、"))));
        }

        if (confirmedAssocPdfs.isEmpty() && !originalAssocPdfs.isEmpty()) {
            rowsWithoutConfirmedRelations << row;
            removedMainMapNames << QFileInfo(mainPdf).completeBaseName();
        }
    }

    if (!rowsWithoutConfirmedRelations.isEmpty()) {
        std::sort(rowsWithoutConfirmedRelations.begin(), rowsWithoutConfirmedRelations.end(), std::greater<int>());
        {
            const QSignalBlocker treeSignals(m_configTree);
            for (const int row : rowsWithoutConfirmedRelations) {
                delete m_configTree->takeTopLevelItem(row);
            }
        }
        rebindConfigTreeButtons();
        appendRuntimeLog(QStringLiteral("真实图廓验证后，从地图配置区移除了 %1 条没有有效关联图的主图配置：%2。")
            .arg(rowsWithoutConfirmedRelations.size())
            .arg(removedMainMapNames.join(QStringLiteral("、"))));
    }

    // Preserve real frame geometries for the overview; the current table is
    // the source of displayed relations, including later manual changes.
    m_verifiedPreviewSheets = realFrameSheetInfos;
    m_previewRelationsVerified = true;
    return removedMainMapNames;
}

void SeriesMapEdgeMatchTaskDialog::updateSheetScalesFromConfigTable()
{
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        auto* pdfEdit = configLineEdit(i, SeriesColMainMap);
        auto* mainScaleEdit = configLineEdit(i, SeriesColMainScale);
        if (pdfEdit && mainScaleEdit) {
            QString pdfPath = pdfEdit->text().trimmed();
            QString infoText = mainScaleEdit->text().trimmed();
            if (!pdfPath.isEmpty() && !infoText.isEmpty() && !infoText.contains(QStringLiteral("自动解析"))) {
                int scale = parseScaleString(infoText);
                if (scale > 0) {
                    QString sheetName = QFileInfo(pdfPath).completeBaseName();
                    auto it = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                        return geoPdfBaseMatchesSheetStrict(info.displayName, sheetName);
                    });
                    if (it != m_sheetInfos.end()) {
                        it->scale = scale;
                    }
                }
            }
        }
        auto* assocEdit = configLineEdit(i, SeriesColAssocMap);
        auto* assocScaleEdit = configLineEdit(i, SeriesColAssocScale);
        if (assocEdit && assocScaleEdit) {
            const QStringList assocPaths = splitSeriesTableList(assocEdit->text().trimmed());
            const QStringList assocScaleTexts = splitSeriesTableList(assocScaleEdit->text().trimmed());
            for (int j = 0; j < assocPaths.size(); ++j) {
                if (j >= assocScaleTexts.size()) continue;
                const int scale = parseScaleString(assocScaleTexts[j]);
                if (scale <= 0) continue;
                const QString sheetName = QFileInfo(assocPaths[j]).completeBaseName();
                auto it = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                    return geoPdfBaseMatchesSheetStrict(info.displayName, sheetName);
                });
                if (it != m_sheetInfos.end()) {
                    it->scale = scale;
                }
            }
        }
    }
}

void SeriesMapEdgeMatchTaskDialog::rebuildPairList()
{
    m_adjacentPairs.clear();
    m_coverageGroups.clear();
    m_taskChoices.clear();

    QSet<QString> pairKeys;

    for (int row = 0; m_configTree && row < m_configTree->topLevelItemCount(); ++row) {
        auto* mainEdit = configLineEdit(row, SeriesColMainMap);
        auto* mainScaleEdit = configLineEdit(row, SeriesColMainScale);
        auto* assocEdit = configLineEdit(row, SeriesColAssocMap);
        auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale);
        if (!mainEdit || mainEdit->text().isEmpty()) continue;

        QString mainPdf = mainEdit->text().trimmed();
        QString mainBaseName = QFileInfo(mainPdf).completeBaseName();
        const int mainScale = mainScaleEdit ? parseScaleString(mainScaleEdit->text().trimmed()) : 0;
        if (mainScale <= 0) continue;

        auto mainIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
            return geoPdfBaseMatchesSheetStrict(info.displayName, mainBaseName);
        });
        if (mainIt == m_sheetInfos.end()) continue;

        QStringList assocPdfs;
        if (assocEdit && !assocEdit->text().isEmpty()) {
            assocPdfs = splitSeriesTableList(assocEdit->text().trimmed());
        }
        const QStringList assocScaleTexts = assocScaleEdit ? splitSeriesTableList(assocScaleEdit->text().trimmed()) : QStringList();

        for (int assocIndex = 0; assocIndex < assocPdfs.size(); ++assocIndex) {
            const QString& assocPdf = assocPdfs[assocIndex];
            const int assocScale = assocIndex < assocScaleTexts.size() ? parseScaleString(assocScaleTexts[assocIndex]) : 0;
            if (assocScale <= 0) continue;
            QString assocBaseName = QFileInfo(assocPdf).completeBaseName();
            auto assocIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                return geoPdfBaseMatchesSheetStrict(info.displayName, assocBaseName);
            });
            if (assocIt == m_sheetInfos.end()) continue;

            const SheetInfo& left = *mainIt;
            const SheetInfo& right = *assocIt;

            const IndexRelationType relation = indexRelationType(left.geometry, right.geometry);
            if (relation != IndexRelationType::Adjacent && relation != IndexRelationType::Overlap) {
                m_outputEdit->append(QStringLiteral("跨比例尺候选“%1 - %2”未进入检查组：关系不是覆盖或相邻。")
                    .arg(left.displayName, right.displayName));
                continue;
            }

            if (mainScale == assocScale) {
                if (relation == IndexRelationType::Adjacent) {
                    const QString key = seriesTaskKey(mainBaseName, mainScale, assocBaseName, assocScale);
                    if (!pairKeys.contains(key)) {
                        pairKeys.insert(key);
                        AdjacentPair pair;
                        pair.leftId = left.featureId;
                        pair.rightId = right.featureId;
                        pair.leftName = mainBaseName;
                        pair.rightName = assocBaseName;
                        pair.leftScale = mainScale;
                        pair.rightScale = assocScale;
                        m_adjacentPairs.push_back(pair);
                    }
                } else if (relation == IndexRelationType::Overlap) {
                    m_outputEdit->append(QStringLiteral("同比例尺图幅 %1 与 %2 存在面重叠，当前任务只支持邻接接边检查。")
                        .arg(left.displayName, right.displayName));
                }
            } else {
                const bool mainIsSmallScale = mainScale > assocScale;
                const SheetInfo& smallSheet = mainIsSmallScale ? left : right;
                const SheetInfo& largeSheet = mainIsSmallScale ? right : left;
                const QString smallName = mainIsSmallScale ? mainBaseName : assocBaseName;
                const QString largeName = mainIsSmallScale ? assocBaseName : mainBaseName;
                const int smallScale = mainIsSmallScale ? mainScale : assocScale;
                const int largeScale = mainIsSmallScale ? assocScale : mainScale;

                QgsGeometry intersection = smallSheet.geometry.intersection(largeSheet.geometry);
                double overlapRatio = intersection.area() / largeSheet.geometry.area();
                const double overlapThreshold = seriesCrossScaleOverlapThreshold();
                if (relation == IndexRelationType::Adjacent || overlapRatio >= overlapThreshold) {
                    const QString key = seriesTaskKey(smallName, smallScale, largeName, largeScale);
                    if (!pairKeys.contains(key)) {
                        pairKeys.insert(key);
                        CoverageGroup group;
                        group.smallScaleId = smallSheet.featureId;
                        group.smallScaleName = smallName;
                        group.smallScale = smallScale;
                        group.largeScaleIds.push_back(largeSheet.featureId);
                        group.largeScaleNames.push_back(largeName);
                        group.largeScale = largeScale;
                        group.displayName = QStringLiteral("主检查地图：%1 -> 关联地图：%2（检查关系：%3 - %4）")
                            .arg(mainBaseName, assocBaseName, largeName, smallName);
                        m_coverageGroups.push_back(group);
                    }
                } else {
                    m_outputEdit->append(QStringLiteral("跨比例尺候选“%1 覆盖 %2”未进入检查组：覆盖比例 %3 低于阈值 %4。")
                        .arg(smallSheet.displayName, largeSheet.displayName)
                        .arg(overlapRatio, 0, 'f', 3)
                        .arg(overlapThreshold, 0, 'f', 3));
                }
            }
        }
    }

    for (int i = 0; i < m_adjacentPairs.size(); ++i) {
        const auto& pair = m_adjacentPairs[i];
        TaskChoice task;
        task.key = seriesTaskKey(pair.leftName,pair.leftScale,pair.rightName,pair.rightScale);
        task.index = i;
        m_taskChoices.push_back(task);
    }
    for (int i = 0; i < m_coverageGroups.size(); ++i) {
        const auto& group = m_coverageGroups[i];
        TaskChoice task;
        task.key = seriesTaskKey(group.smallScaleName,group.smallScale,
                                 group.largeScaleNames.value(0),group.largeScale);
        task.crossScale = true;
        task.index = i;
        m_taskChoices.push_back(task);
    }
}

QLineEdit* SeriesMapEdgeMatchTaskDialog::configLineEdit(int row, int col) const
{
    if (!m_configTree) return nullptr;
    QTreeWidgetItem* item = m_configTree->topLevelItem(row);
    if (!item) return nullptr;
    QWidget* widget = m_configTree->itemWidget(item, col);
    if (!widget) return nullptr;
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) return edit;
    return widget->findChild<QLineEdit*>();
}

int SeriesMapEdgeMatchTaskDialog::addEmptyConfigRow()
{
    if (!m_configTree) return -1;

    auto* item = new QTreeWidgetItem();
    item->setText(SeriesColRelationType, QStringLiteral("待判断"));
    item->setText(SeriesColStatus, QStringLiteral("未完成"));
    m_configTree->addTopLevelItem(item);

    const int row = m_configTree->indexOfTopLevelItem(item);

    auto* mainWidget = new QWidget();
    auto* mainLayout = new QHBoxLayout(mainWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    auto* mainEdit = new QLineEdit();
    mainEdit->setPlaceholderText(QStringLiteral("选择主检查地图 GeoPDF"));
    mainEdit->setMinimumWidth(0);
    mainEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* mainBtn = new QPushButton(QStringLiteral("选择"));
    mainBtn->setFixedWidth(52);
    mainLayout->addWidget(mainEdit, 1);
    mainLayout->addWidget(mainBtn);
    m_configTree->setItemWidget(item, SeriesColMainMap, mainWidget);
    connect(mainBtn, &QPushButton::clicked, this, [this, row]() { browseTableMainMap(row); });

    auto* mainScaleWidget = new QWidget();
    auto* mainScaleLayout = new QHBoxLayout(mainScaleWidget);
    mainScaleLayout->setContentsMargins(0, 0, 0, 0);
    auto* mainScaleEdit = new QLineEdit();
    mainScaleEdit->setPlaceholderText(QStringLiteral("如 1:250000"));
    mainScaleLayout->addWidget(mainScaleEdit);
    m_configTree->setItemWidget(item, SeriesColMainScale, mainScaleWidget);

    auto* assocWidget = new QWidget();
    auto* assocLayout = new QHBoxLayout(assocWidget);
    assocLayout->setContentsMargins(0, 0, 0, 0);
    assocLayout->setSpacing(0);
    auto* assocEdit = new QLineEdit();
    assocEdit->setPlaceholderText(QStringLiteral("点击配置选择关联地图"));
    assocEdit->setReadOnly(true);
    assocEdit->setMinimumWidth(0);
    assocEdit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto* assocBtn = new QPushButton(QStringLiteral("配置"));
    assocBtn->setFixedWidth(52);
    assocLayout->addWidget(assocEdit, 1);
    assocLayout->addWidget(assocBtn);
    m_configTree->setItemWidget(item, SeriesColAssocMap, assocWidget);
    connect(assocBtn, &QPushButton::clicked, this, [this, row]() { browseTableAssocMap(row); });

    auto* assocScaleWidget = new QWidget();
    auto* assocScaleLayout = new QHBoxLayout(assocScaleWidget);
    assocScaleLayout->setContentsMargins(0, 0, 0, 0);
    auto* assocScaleEdit = new QLineEdit();
    assocScaleEdit->setPlaceholderText(QStringLiteral("多个关联图用 ; 对应"));
    assocScaleLayout->addWidget(assocScaleEdit);
    m_configTree->setItemWidget(item, SeriesColAssocScale, assocScaleWidget);

    // Theme rules enlarge embedded editors/buttons independently of tree items.
    // Override only their typography and spacing with the tree's effective font.
    m_configTree->ensurePolished();
    const QFont rowFont = m_configTree->font();
    const int rowFontPixels = QFontInfo(rowFont).pixelSize();
    const int controlHeight = QFontMetrics(rowFont).height() + 6;
    const QString controlStyle = QStringLiteral(
        "QLineEdit, QPushButton { font-size: %1px; font-weight: %2; font-style: %3; "
        "padding: 0px 4px; min-height: 0px; } "
        "QPushButton { min-width: 0px; }")
        .arg(rowFontPixels)
        .arg(rowFont.bold() ? QStringLiteral("bold") : QStringLiteral("normal"))
        .arg(rowFont.italic() ? QStringLiteral("italic") : QStringLiteral("normal"));
    for (QWidget* control : {static_cast<QWidget*>(mainEdit), static_cast<QWidget*>(mainScaleEdit),
                            static_cast<QWidget*>(assocEdit), static_cast<QWidget*>(assocScaleEdit),
                            static_cast<QWidget*>(mainBtn), static_cast<QWidget*>(assocBtn)}) {
        control->setFont(rowFont);
        control->setStyleSheet(controlStyle);
        control->setFixedHeight(controlHeight);
    }

    auto setupLineEdit = [this](QLineEdit* edit) {
        connect(edit, &QLineEdit::textChanged, this, [this, edit]() {
            edit->setToolTip(edit->text());
            edit->setCursorPosition(0);
            checkTableStatus();
        });
    };

    setupLineEdit(mainEdit);
    setupLineEdit(assocEdit);
    for (QLineEdit* edit : {mainEdit, mainScaleEdit, assocEdit, assocScaleEdit})
        edit->installEventFilter(this);
    
    connect(mainScaleEdit, &QLineEdit::textChanged, this, [this]() {
        updateSheetScalesFromConfigTable();
        checkTableStatus();
    });
    connect(assocScaleEdit, &QLineEdit::textChanged, this, [this]() {
        updateSheetScalesFromConfigTable();
        checkTableStatus();
    });

    return row;
}

void SeriesMapEdgeMatchTaskDialog::addConfigRowsFromPaths(const QStringList& mainPdfPaths, const QStringList& assocPdfPaths)
{
    QStringList duplicateGroups;
    QStringList duplicateGroupKeys;
    bool removedDuplicateRow = false;
    for (const QString& mainPdf : mainPdfPaths) {
        if (mainPdf.trimmed().isEmpty()) continue;

        const int row = addEmptyConfigRow();
        if (row < 0) continue;

        if (auto* mainEdit = configLineEdit(row, SeriesColMainMap)) {
            mainEdit->setText(mainPdf.trimmed());
        }
        if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
            QStringList assocPaths = assocPdfPaths;
            assocPaths.removeAll(mainPdf.trimmed());
            assocPaths.removeDuplicates();
            assocEdit->setText(assocPaths.join(QStringLiteral(";")));
        }

        if (auto* mainScaleEdit = configLineEdit(row, SeriesColMainScale); mainScaleEdit && mainScaleEdit->text().trimmed().isEmpty()) {
            const QString shpDir = resolvePreprocessedShpDir(mainPdf.trimmed(), outputRootDir());
            if (!shpDir.isEmpty()) {
                const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
                if (inferredScale > 0) {
                    mainScaleEdit->setText(QString::number(inferredScale));
                } else if (inferredScale == -1) {
                    mainScaleEdit->setPlaceholderText(QStringLiteral("比例尺多值冲突"));
                } else {
                    mainScaleEdit->setPlaceholderText(QStringLiteral("比例尺未识别"));
                }
            }
        }

        if (auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale)) {
            QStringList assocScales;
            for (const QString& assocPdf : assocPdfPaths) {
                if (assocPdf.trimmed().isEmpty() || assocPdf.trimmed() == mainPdf.trimmed()) continue;
                const QString baseName = QFileInfo(assocPdf).completeBaseName();
                auto assocIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                    return geoPdfBaseMatchesSheetStrict(info.displayName, baseName);
                });
                int scale = assocIt != m_sheetInfos.end() ? assocIt->scale : 0;
                if (scale <= 0) {
                    const QString shpDir = resolvePreprocessedShpDir(assocPdf, outputRootDir());
                    if (!shpDir.isEmpty()) {
                        const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
                        if (inferredScale > 0) scale = inferredScale;
                    }
                }
                assocScales << (scale > 0 ? QString::number(scale) : QString());
            }
            assocScaleEdit->setText(assocScales.join(QStringLiteral(";")));
            if (assocScaleEdit->text().trimmed().isEmpty() && !assocPdfPaths.isEmpty()) {
                assocScaleEdit->setPlaceholderText(QStringLiteral("关联图比例尺未识别"));
            }
        }

        if (hasExternalSheetIndex() && !m_sheetInfos.isEmpty()) {
            autoFillAssociatedMaps(row, mainPdf.trimmed());
        }

        const auto* candidateMainEdit = configLineEdit(row, SeriesColMainMap);
        const auto* candidateAssocEdit = configLineEdit(row, SeriesColAssocMap);
        const QString candidateMainPath = candidateMainEdit ? candidateMainEdit->text().trimmed() : QString();
        const QStringList candidateAssocPaths = candidateAssocEdit
            ? splitSeriesTableList(candidateAssocEdit->text()) : QStringList();
        bool duplicate = false;
        for (int existingRow = 0; existingRow < m_configTree->topLevelItemCount(); ++existingRow) {
            if (existingRow == row) continue;
            const auto* existingMainEdit = configLineEdit(existingRow, SeriesColMainMap);
            const auto* existingAssocEdit = configLineEdit(existingRow, SeriesColAssocMap);
            if (existingMainEdit && SeriesMapConfigurationDeduplication::sameMapGroup(
                    candidateMainPath, candidateAssocPaths,
                    existingMainEdit->text(), existingAssocEdit ? splitSeriesTableList(existingAssocEdit->text()) : QStringList())) {
                duplicate = true;
                break;
            }
        }

        if (duplicate) {
            const QString duplicateGroupKey = SeriesMapConfigurationDeduplication::mapGroupIdentityKey(
                candidateMainPath, candidateAssocPaths);
            if (!duplicateGroupKeys.contains(duplicateGroupKey)) {
                duplicateGroupKeys << duplicateGroupKey;
                duplicateGroups << SeriesMapConfigurationDeduplication::groupDisplayName(
                    candidateMainPath, candidateAssocPaths);
            }
            delete m_configTree->takeTopLevelItem(row);
            removedDuplicateRow = true;
        }
    }

    if (removedDuplicateRow) rebindConfigTreeButtons();
    checkTableStatus();
    if (!duplicateGroups.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("重复地图组合"),
            QStringLiteral("以下地图组合已经添加，重复项已跳过：\n%1")
                .arg(duplicateGroups.join(QLatin1Char('\n'))));
    }
}

void SeriesMapEdgeMatchTaskDialog::addConfigRow()
{
    const QStringList mainPaths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择主检查地图 GeoPDF"),
        QString(),
        QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    if (mainPaths.isEmpty()) {
        addEmptyConfigRow();
        checkTableStatus();
        return;
    }

    const QStringList assocPaths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择关联地图 GeoPDF（可多选）"),
        QFileInfo(mainPaths.first()).absolutePath(),
        QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    addConfigRowsFromPaths(mainPaths, assocPaths);
}

void SeriesMapEdgeMatchTaskDialog::removeConfigRow()
{
    if (!m_configTree) return;
    QList<QTreeWidgetItem*> selectedItems = m_configTree->selectedItems();
    QSet<int> rows;
    for (auto* item : selectedItems) {
        QTreeWidgetItem* topItem = item->parent() ? item->parent() : item;
        rows.insert(m_configTree->indexOfTopLevelItem(topItem));
    }

    QList<int> rowList = rows.values();
    std::sort(rowList.begin(), rowList.end(), std::greater<int>());
    for (int row : rowList) {
        delete m_configTree->takeTopLevelItem(row);
    }

    rebindConfigTreeButtons();
    checkTableStatus(); // 移除行后更新状态并触发勾选逻辑
}

void SeriesMapEdgeMatchTaskDialog::rebindConfigTreeButtons()
{
    if (!m_configTree) return;
    for (int i = 0; i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_configTree->topLevelItem(i);
        auto* mainWidget = m_configTree->itemWidget(item, SeriesColMainMap);
        auto* assocWidget = m_configTree->itemWidget(item, SeriesColAssocMap);
        auto* mainBtn = mainWidget ? mainWidget->findChild<QPushButton*>() : nullptr;
        auto* assocBtn = assocWidget ? assocWidget->findChild<QPushButton*>() : nullptr;
        if (mainBtn) {
            mainBtn->disconnect();
            connect(mainBtn, &QPushButton::clicked, this, [this, i]() { browseTableMainMap(i); });
        }
        if (assocBtn) {
            assocBtn->disconnect();
            connect(assocBtn, &QPushButton::clicked, this, [this, i]() { browseTableAssocMap(i); });
        }
    }
}

void SeriesMapEdgeMatchTaskDialog::browseTableMainMap(int row)
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择主检查地图 GeoPDF"), QString(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    if (!paths.isEmpty()) {
        const QString path = paths.first();
        if (auto* edit = configLineEdit(row, SeriesColMainMap)) {
            edit->setText(path);
        }
        
        if (auto* scaleEdit = configLineEdit(row, SeriesColMainScale); scaleEdit && scaleEdit->text().isEmpty()) {
            QString shpDir = resolvePreprocessedShpDir(path, outputRootDir());
            if (!shpDir.isEmpty()) {
                int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
                if (inferredScale > 0) {
                    scaleEdit->setText(QString::number(inferredScale));
                } else if (inferredScale == -1) {
                    scaleEdit->setPlaceholderText(QStringLiteral("比例尺多值冲突"));
                } else {
                    scaleEdit->setPlaceholderText(QStringLiteral("比例尺未识别"));
                }
            }
        }

        QString indexPath = m_indexFileEdit->text().trimmed();
        if (!indexPath.isEmpty() && !m_sheetInfos.isEmpty()) {
            autoFillAssociatedMaps(row, path);
        }

        if (paths.size() > 1) {
            QStringList extraPaths = paths;
            extraPaths.removeFirst();
            QStringList assocPaths;
            if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
                assocPaths = splitSeriesTableList(assocEdit->text().trimmed());
            }
            addConfigRowsFromPaths(extraPaths, assocPaths);
        } else {
            checkTableStatus();
        }
    }
}

void SeriesMapEdgeMatchTaskDialog::browseTableAssocMap(int row)
{
    auto* edit = configLineEdit(row, SeriesColAssocMap);
    if (!edit) return;

    QString currentText = edit->text().trimmed();
    QStringList currentPaths = splitSeriesTableList(currentText);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("配置关联地图"));
    dlg.resize(500, 400);
    auto* layout = new QVBoxLayout(&dlg);

    layout->addWidget(new QLabel(QStringLiteral("已选关联地图（勾选以保留，取消勾选以移除）："), &dlg));

    auto* listWidget = new QListWidget(&dlg);
    layout->addWidget(listWidget);

    for (const QString& p : currentPaths) {
        auto* item = new QListWidgetItem(p, listWidget);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }

    auto* addBtn = new QPushButton(QStringLiteral("添加外部 GeoPDF..."), &dlg);
    layout->addWidget(addBtn);
    connect(addBtn, &QPushButton::clicked, &dlg, [&]() {
        QStringList paths = QFileDialog::getOpenFileNames(&dlg, QStringLiteral("选择关联地图 GeoPDF"), QString(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
        for (const QString& p : paths) {
            bool exists = false;
            for (int i = 0; i < listWidget->count(); ++i) {
                if (listWidget->item(i)->text() == p) { exists = true; break; }
            }
            if (!exists) {
                auto* item = new QListWidgetItem(p, listWidget);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(Qt::Checked);
            }
        }
    });

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttonBox);
    connect(buttonBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        QStringList newPaths;
        for (int i = 0; i < listWidget->count(); ++i) {
            if (listWidget->item(i)->checkState() == Qt::Checked) {
                newPaths << listWidget->item(i)->text();
            }
        }
        edit->setText(newPaths.join(QStringLiteral(";")));
        QStringList assocScales;
        for (const QString& path : newPaths) {
            const QString baseName = QFileInfo(path).completeBaseName();
            auto assocIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                return geoPdfBaseMatchesSheetStrict(info.displayName, baseName);
            });
            int scale = assocIt != m_sheetInfos.end() ? assocIt->scale : 0;
            if (scale <= 0) {
                const QString shpDir = resolvePreprocessedShpDir(path, outputRootDir());
                if (!shpDir.isEmpty()) {
                    const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
                    if (inferredScale > 0) scale = inferredScale;
                }
            }
            assocScales << (scale > 0 ? QString::number(scale) : QString());
        }
        if (auto* scaleEdit = configLineEdit(row, SeriesColAssocScale)) {
            scaleEdit->setText(assocScales.join(QStringLiteral(";")));
            if (scaleEdit->text().trimmed().isEmpty()) {
                scaleEdit->setPlaceholderText(QStringLiteral("关联图比例尺未识别"));
            }
        }
        checkTableStatus();
    }
}

void SeriesMapEdgeMatchTaskDialog::autoFillAssociatedMaps(int row, const QString& mainPdfPath)
{
    QFileInfo mainFi(mainPdfPath);
    QString mainBaseName = mainFi.completeBaseName();
    auto* existingAssocEdit = configLineEdit(row, SeriesColAssocMap);
    if (!existingAssocEdit) return;
    if (!splitSeriesTableList(existingAssocEdit->text().trimmed()).isEmpty()) {
        appendRuntimeLog(QStringLiteral("主图幅“%1”已有指定关联地图，保留现有配置并跳过自动填充。")
            .arg(mainBaseName));
        return;
    }

    auto mainIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
        return geoPdfBaseMatchesSheetStrict(info.displayName, mainBaseName);
    });

    if (mainIt == m_sheetInfos.end()) {
        appendRuntimeLog(QStringLiteral("主图幅“%1”未在外部索引中找到，无法自动填充关联地图。").arg(mainBaseName));
        return;
    }

    QStringList relatedSheetNames;
    for (const SheetInfo& info : m_sheetInfos) {
        if (info.featureId == mainIt->featureId) continue;
        const IndexRelationType relation = indexRelationType(mainIt->geometry, info.geometry);
        if (relation == IndexRelationType::Adjacent || relation == IndexRelationType::Overlap) {
            relatedSheetNames << info.displayName;
        }
    }

    if (relatedSheetNames.isEmpty()) {
        appendRuntimeLog(QStringLiteral("主图幅“%1”在外部索引中没有空间相邻/覆盖的图幅。").arg(mainBaseName));
        return;
    }

    QString searchDir = mainFi.absolutePath();
    QStringList searchRoots = resolveGeoPdfSearchRoots(m_indexFileEdit->text().trimmed());
    if (!searchRoots.contains(searchDir)) searchRoots << searchDir;

    QStringList allPdfFiles;
    for (const QString& root : searchRoots) {
        QStringList pdfs = collectGeoPdfFiles(root);
        for (const QString& pdf : pdfs) {
            if (!allPdfFiles.contains(pdf)) allPdfFiles << pdf;
        }
    }

    QStringList associatedPdfs;
    for (const QString& sheetName : relatedSheetNames) {
        QStringList matchedPdfs = findStrictGeoPdfForSheetName(sheetName, allPdfFiles);
        if (!matchedPdfs.isEmpty()) {
            associatedPdfs << matchedPdfs.first();
        }
    }

    associatedPdfs.removeDuplicates();
    associatedPdfs.removeAll(mainPdfPath);
    if (hasExternalSheetIndex()) {
        // External candidate index mode: keep administrative-boundary candidates until explicit checking.
    } else {
        associatedPdfs = filterAssociatedPdfsByRealFrameRelation(mainPdfPath, associatedPdfs);
    }

    if (!associatedPdfs.isEmpty()) {
        if (auto* assocEdit = configLineEdit(row, SeriesColAssocMap)) {
            assocEdit->setText(associatedPdfs.join(QStringLiteral(";")));
            appendRuntimeLog(QStringLiteral("主图幅“%1”自动匹配到 %2 个关联地图。").arg(mainBaseName).arg(associatedPdfs.size()));
        }

        QStringList associatedScales;
        for (const QString& assocPdf : associatedPdfs) {
            const QString assocBaseName = QFileInfo(assocPdf).completeBaseName();
            auto assocIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SheetInfo& info) {
                return info.displayName == assocBaseName || assocBaseName.contains(info.displayName) || info.displayName.contains(assocBaseName);
            });
            if (assocIt != m_sheetInfos.end() && assocIt->scale > 0) {
                associatedScales << QString::number(assocIt->scale);
            } else {
                associatedScales << QString();
            }
        }
        if (auto* assocScaleEdit = configLineEdit(row, SeriesColAssocScale)) {
            assocScaleEdit->setText(associatedScales.join(QStringLiteral(";")));
        }
    } else {
        appendRuntimeLog(QStringLiteral("主图幅“%1”有空间关联图幅，但在文件夹中未找到对应的 GeoPDF 文件。").arg(mainBaseName));
    }
}

void SeriesMapEdgeMatchTaskDialog::checkTableStatus()
{
    if (m_refreshingConfig) return;
    QScopedValueRollback<bool> refreshing(m_refreshingConfig, true);
    QSignalBlocker signalBlocker(m_configTree);
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* rowItem = m_configTree->topLevelItem(i);
        if (!rowItem) continue;

        auto* pdfEdit = configLineEdit(i, SeriesColMainMap);
        auto* mainScaleEdit = configLineEdit(i, SeriesColMainScale);
        auto* assocEdit = configLineEdit(i, SeriesColAssocMap);
        auto* assocScaleEdit = configLineEdit(i, SeriesColAssocScale);

        QString pdfPath = pdfEdit ? pdfEdit->text().trimmed() : QString();
        QString mainScaleText = mainScaleEdit ? mainScaleEdit->text().trimmed() : QString();
        QString assocPath = assocEdit ? assocEdit->text().trimmed() : QString();
        QString assocScaleText = assocScaleEdit ? assocScaleEdit->text().trimmed() : QString();

        bool isEmpty = pdfPath.isEmpty() && assocPath.isEmpty() && mainScaleText.isEmpty() && assocScaleText.isEmpty();

        bool valid = true;
        if (pdfPath.isEmpty() || !QFileInfo(pdfPath).exists()) valid = false;
        if (assocPath.isEmpty()) valid = false;
        const QStringList assocPaths = splitSeriesTableList(assocPath);
        if (!assocPath.isEmpty()) {
            for (const QString& path : assocPaths) {
                if (!QFileInfo(path.trimmed()).exists()) {
                    valid = false;
                    break;
                }
            }
        }
        
        const int mainScale = parseScaleString(mainScaleText);
        QStringList assocScaleTexts = splitSeriesTableList(assocScaleText);
        QList<int> assocScales;
        for (const QString& text : assocScaleTexts) {
            const int scale = parseScaleString(text);
            if (scale > 0) assocScales << scale;
        }
        const bool assocScaleReady = !assocPaths.isEmpty() && assocScales.size() >= assocPaths.size();
        QString relationText = QStringLiteral("待判断");
        if (mainScale > 0 && assocScaleReady) {
            bool allSame = true;
            bool allDifferent = true;
            for (int scale : assocScales) {
                if (scale != mainScale) allSame = false;
                if (scale == mainScale) allDifferent = false;
            }
            if (allSame) relationText = QStringLiteral("同比例尺");
            else if (allDifferent) relationText = QStringLiteral("跨比例尺");
            else relationText = QStringLiteral("混合");
        }
        rowItem->setText(SeriesColRelationType, relationText);

        if (isEmpty) {
            rowItem->setText(SeriesColStatus, QStringLiteral("空白"));
        } else if (mainScale <= 0) {
            rowItem->setText(SeriesColStatus, QStringLiteral("待填写主图比例尺"));
        } else if (assocPath.isEmpty()) {
            // Keep rows without candidates; relation filtering removes a row only after rejecting all candidates.
            rowItem->setText(SeriesColStatus, QStringLiteral("无有效关联地图"));
        } else if (!assocScaleReady) {
            rowItem->setText(SeriesColStatus, QStringLiteral("待填写关联图比例尺"));
        } else if (valid) {
            rowItem->setText(SeriesColStatus, QStringLiteral("已完成"));
        } else {
            rowItem->setText(SeriesColStatus, QStringLiteral("未完成"));
        }
    }

    rebuildPairList();
    refreshConfigTreeChildren();
    updatePairCheckStates();
    updateOverview();
}

void SeriesMapEdgeMatchTaskDialog::refreshConfigTreeChildren()
{
    if (!m_configTree) return;
    const QSignalBlocker blocker(m_configTree);
    QTreeWidgetItem* current = m_configTree->currentItem();
    QTreeWidgetItem* currentParent = current && current->parent() ? current->parent() : current;
    const QString currentKey = current ? current->data(0,SeriesTaskKeyRole).toString() : QString();
    QTreeWidgetItem* restored = currentParent;
    QSet<QString> validKeys;
    for (const auto& task : m_taskChoices) validKeys.insert(task.key);

    for (int row = 0; row < m_configTree->topLevelItemCount(); ++row) {
        QTreeWidgetItem* parent = m_configTree->topLevelItem(row);
        const bool expanded = parent->childCount()==0 || parent->isExpanded();
        while (parent->childCount()>0) delete parent->takeChild(0);
        const QString mainPath = configLineEdit(row,SeriesColMainMap)->text().trimmed();
        const QString mainName = QFileInfo(mainPath).completeBaseName();
        const int mainScale = parseScaleString(configLineEdit(row,SeriesColMainScale)->text());
        const QStringList paths = splitSeriesTableList(configLineEdit(row,SeriesColAssocMap)->text());
        const QStringList scales = splitSeriesTableList(configLineEdit(row,SeriesColAssocScale)->text());
        parent->setData(0,SeriesMainNameRole,mainName);
        parent->setToolTip(SeriesColMainMap,mainPath);
        for (int i=0;i<paths.size();++i) {
            const QString assocName = QFileInfo(paths[i]).completeBaseName();
            const int assocScale = parseScaleString(scales.value(i));
            const QString key = seriesTaskKey(mainName,mainScale,assocName,assocScale);
            const bool filesReady = QFileInfo(mainPath).isFile() && QFileInfo(paths[i]).isFile();
            const bool scalesReady = mainScale>0 && assocScale>0;
            auto knownSheet = [this](const QString& name) {
                return std::any_of(m_sheetInfos.cbegin(),m_sheetInfos.cend(),[&](const SheetInfo& sheet) {
                    return geoPdfBaseMatchesSheetStrict(sheet.displayName,name);
                });
            };
            const bool relationKnown = knownSheet(mainName) && knownSheet(assocName);
            const bool selectable = filesReady && scalesReady && (!relationKnown || validKeys.contains(key));
            auto* child = new QTreeWidgetItem(parent);
            child->setText(SeriesColMainMap,QStringLiteral("关联图 %1").arg(i+1));
            child->setText(SeriesColMainScale,mainScale>0 ? QString::number(mainScale) : QString());
            child->setText(SeriesColAssocMap,QFileInfo(paths[i]).fileName());
            child->setText(SeriesColAssocScale,scales.value(i));
            child->setText(SeriesColRelationType,!scalesReady ? QStringLiteral("待判断") :
                mainScale==assocScale ? QStringLiteral("同比例尺") : QStringLiteral("跨比例尺"));
            child->setText(SeriesColStatus,!filesReady ? QStringLiteral("文件不存在") :
                !scalesReady ? QStringLiteral("待填写比例尺") : !relationKnown ? QStringLiteral("待加载图廓") :
                !selectable ? QStringLiteral("无有效图幅关系") : QStringLiteral("已就绪"));
            if (filesReady && scalesReady && relationKnown && !selectable) {
                const auto mainFrame = std::find_if(m_sheetInfos.cbegin(), m_sheetInfos.cend(), [&](const SheetInfo& info) {
                    return geoPdfBaseMatchesSheetStrict(info.displayName, mainName);
                });
                const auto assocFrame = std::find_if(m_sheetInfos.cbegin(), m_sheetInfos.cend(), [&](const SheetInfo& info) {
                    return geoPdfBaseMatchesSheetStrict(info.displayName, assocName);
                });
                if (mainFrame != m_sheetInfos.cend() && assocFrame != m_sheetInfos.cend()) {
                    const IndexRelationType relation = indexRelationType(mainFrame->geometry, assocFrame->geometry);
                    if (relation == IndexRelationType::Overlap) {
                        if (mainScale == assocScale) {
                            child->setText(SeriesColStatus, QStringLiteral("同比例尺图幅重叠，当前仅检查邻接"));
                        } else {
                            const QgsGeometry& largerScaleGeometry = mainScale < assocScale
                                ? mainFrame->geometry : assocFrame->geometry;
                            const QgsGeometry common = mainFrame->geometry.intersection(assocFrame->geometry);
                            const double overlapRatio = largerScaleGeometry.area() > 0.0
                                ? common.area() / largerScaleGeometry.area() : 0.0;
                            const double threshold = qBound(0.0, seriesCrossScaleOverlapThreshold(), 1.0);
                            if (overlapRatio < threshold) {
                                child->setText(SeriesColStatus, QStringLiteral("存在重叠，覆盖率不足"));
                                child->setToolTip(SeriesColStatus, QStringLiteral("覆盖率 %1，检查阈值 %2")
                                    .arg(overlapRatio, 0, 'f', 3).arg(threshold, 0, 'f', 3));
                            }
                        }
                    } else if (relation == IndexRelationType::IntersectsOnly) {
                        child->setText(SeriesColStatus, QStringLiteral("仅点状接触，不作为接边"));
                    } else if (relation == IndexRelationType::None) {
                        child->setText(SeriesColStatus, QStringLiteral("真实图廓无空间交集"));
                    }
                }
            }
            child->setToolTip(SeriesColAssocMap,paths[i]);
            child->setData(0,SeriesTaskKeyRole,key);
            child->setData(0,SeriesMainNameRole,mainName);
            child->setData(0,SeriesAssocNameRole,assocName);
            if (selectable) {
                if (!m_pairSelection.contains(key)) m_pairSelection.insert(key,true);
                child->setFlags(child->flags() | Qt::ItemIsUserCheckable);
            } else child->setFlags(child->flags() & ~Qt::ItemIsUserCheckable);
            child->setCheckState(0,selectable && m_pairSelection.value(key,true) ? Qt::Checked : Qt::Unchecked);
            if (parent==currentParent && key==currentKey) restored=child;
        }
        parent->setExpanded(expanded);
    }
    m_configTree->setCurrentItem(restored);
}

void SeriesMapEdgeMatchTaskDialog::updatePairCheckStates()
{
    QSet<QString> readyKeys;
    for (int row=0;row<m_configTree->topLevelItemCount();++row) {
        auto* parent=m_configTree->topLevelItem(row);
        for (int i=0;i<parent->childCount();++i) {
            auto* child=parent->child(i);
            if (child->flags() & Qt::ItemIsUserCheckable)
                readyKeys.insert(child->data(0,SeriesTaskKeyRole).toString());
        }
    }
    for (auto& task : m_taskChoices) task.ready=readyKeys.contains(task.key);
    updateSelectedCount();
}

void SeriesMapEdgeMatchTaskDialog::setTaskChecked(QTreeWidgetItem* item, int column)
{
    if (m_refreshingConfig || !item || !item->parent() || column!=0 ||
        !(item->flags() & Qt::ItemIsUserCheckable)) return;
    const QString key=item->data(0,SeriesTaskKeyRole).toString();
    if (key.isEmpty()) return;
    const bool checked=item->checkState(0)==Qt::Checked;
    m_pairSelection[key]=checked;
    const QSignalBlocker blocker(m_configTree);
    for (int row=0;row<m_configTree->topLevelItemCount();++row) {
        auto* parent=m_configTree->topLevelItem(row);
        for (int i=0;i<parent->childCount();++i) {
            auto* child=parent->child(i);
            if ((child->flags() & Qt::ItemIsUserCheckable) && child->data(0,SeriesTaskKeyRole).toString()==key)
                child->setCheckState(0,checked ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateSelectedCount();
}

bool SeriesMapEdgeMatchTaskDialog::hasExternalSheetIndex() const
{
    return m_indexFileEdit && !m_indexFileEdit->text().trimmed().isEmpty();
}

bool SeriesMapEdgeMatchTaskDialog::hasCompletedConfigRows() const
{
    if (!m_configTree) return false;
    for (int i = 0; i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_configTree->topLevelItem(i);
        if (item && item->text(SeriesColStatus) == QStringLiteral("已完成")) {
            return true;
        }
    }
    return false;
}

void SeriesMapEdgeMatchTaskDialog::updateSelectedCount()
{
    QSet<QString> selected;
    for (int row=0;m_configTree && row<m_configTree->topLevelItemCount();++row) {
        auto* parent=m_configTree->topLevelItem(row);
        for (int i=0;i<parent->childCount();++i) {
            auto* child=parent->child(i);
            if ((child->flags() & Qt::ItemIsUserCheckable) && child->checkState(0)==Qt::Checked)
                selected.insert(child->data(0,SeriesTaskKeyRole).toString());
        }
    }
    if (m_taskCountLabel) m_taskCountLabel->setText(QStringLiteral("已选：%1 项检查任务").arg(selected.size()));
    if (m_startButton) m_startButton->setEnabled(hasCompletedConfigRows() && !selected.isEmpty());
}

void SeriesMapEdgeMatchTaskDialog::selectAllPairs()
{
    const QSignalBlocker blocker(m_configTree);
    for (int row=0;row<m_configTree->topLevelItemCount();++row) {
        auto* parent=m_configTree->topLevelItem(row);
        for (int i=0;i<parent->childCount();++i) {
            auto* child=parent->child(i);
            if (!(child->flags() & Qt::ItemIsUserCheckable)) continue;
            m_pairSelection[child->data(0,SeriesTaskKeyRole).toString()]=true;
            child->setCheckState(0,Qt::Checked);
        }
    }
    updateSelectedCount();
}

void SeriesMapEdgeMatchTaskDialog::clearAllPairs()
{
    const QSignalBlocker blocker(m_configTree);
    for (int row=0;row<m_configTree->topLevelItemCount();++row) {
        auto* parent=m_configTree->topLevelItem(row);
        for (int i=0;i<parent->childCount();++i) {
            auto* child=parent->child(i);
            m_pairSelection[child->data(0,SeriesTaskKeyRole).toString()]=false;
            child->setCheckState(0,Qt::Unchecked);
        }
    }
    updateSelectedCount();
}

bool SeriesMapEdgeMatchTaskDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress && m_configTree) {
        auto* editor = qobject_cast<QLineEdit*>(watched);
        if (editor) {
            const QPoint pos = editor->mapTo(m_configTree->viewport(), QPoint(1, 1));
            QTreeWidgetItem* item = m_configTree->itemAt(pos);
            if (item) {
                const bool alreadyCurrent = m_configTree->currentItem() == item;
                m_configTree->setCurrentItem(item);
                if (alreadyCurrent && m_previewMode != PreviewMode::Selection) highlightConfigItem(item);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SeriesMapEdgeMatchTaskDialog::updateOverview(bool allowPreprocess)
{
    if (!m_sheetOverview || m_updatingOverview) return;
    const QScopedValueRollback<bool> updating(m_updatingOverview, true);
    QVector<SheetInfo> overviewInfos;
    QString selectedMain;
    QStringList selectedAssociated;

    auto pathKey = [](const QString& path) {
        return QDir::cleanPath(QFileInfo(path.trimmed()).absoluteFilePath()).toLower();
    };
    if (m_previewMode == PreviewMode::Index) {
        overviewInfos = m_sheetInfos;
        if (hasExternalSheetIndex())
            for (SheetInfo& info : overviewInfos) info.previewFromExternalIndex = true;
    } else {
        QStringList paths;
        auto appendRow = [&](QTreeWidgetItem* item, bool selectedOnly) {
            if (!item || !m_configTree) return;
            QTreeWidgetItem* parent = item->parent() ? item->parent() : item;
            const int row = m_configTree->indexOfTopLevelItem(parent);
            const auto* mainEdit = configLineEdit(row, SeriesColMainMap);
            const auto* assocEdit = configLineEdit(row, SeriesColAssocMap);
            if (!mainEdit || !assocEdit) return;
            const QString main = mainEdit->text().trimmed();
            QStringList associated = splitSeriesTableList(assocEdit->text());
            if (selectedOnly && item->parent()) {
                const QString selected = item->toolTip(SeriesColAssocMap).trimmed();
                associated = selected.isEmpty() ? QStringList() : QStringList{selected};
            }
            if (main.isEmpty() || associated.isEmpty()) return;
            paths << main << associated;
            if (selectedOnly) { selectedMain = main; selectedAssociated = associated; }
        };
        if (m_previewMode == PreviewMode::Selection) {
            appendRow(m_configTree ? m_configTree->currentItem() : nullptr, true);
        } else if (m_configTree) {
            for (int row = 0; row < m_configTree->topLevelItemCount(); ++row)
                appendRow(m_configTree->topLevelItem(row), false);
        }

        QSet<QString> added;
        QgsCoordinateReferenceSystem displayCrs;
        for (const QString& path : paths) {
            const QString key = pathKey(path);
            if (added.contains(key)) continue;
            added.insert(key);
            const QString name = QFileInfo(path).completeBaseName();
            SheetInfo info;
            bool found = false;
            auto findSheet = [&](const QVector<SheetInfo>& source, bool exactOnly) {
                const SheetInfo* match = nullptr;
                for (const SheetInfo& candidate : source) {
                    if (exactOnly && (candidate.pdfPath.isEmpty() || pathKey(candidate.pdfPath) != key)) continue;
                    if (candidate.displayName.compare(name, Qt::CaseInsensitive) != 0) continue;
                    if (match) return false;
                    match = &candidate;
                }
                if (!match && !exactOnly) {
                    for (const SheetInfo& candidate : source) {
                        if (!geoPdfBaseMatchesSheetStrict(candidate.displayName, name)) continue;
                        if (match) return false;
                        match = &candidate;
                    }
                }
                if (!match) return false;
                info = *match;
                return true;
            };
            // Real-frame entries are named after their PDF; do not reuse a
            // different scale's frame merely because both match an index name.
            found = findSheet(m_verifiedPreviewSheets, true);
            if (!found && m_cachedPreviewSheets.contains(key)) {
                info = m_cachedPreviewSheets.value(key);
                found = true;
            }
            // Before validation the external index is a candidate preview.
            // Afterwards prefer the actual frame for newly configured PDFs.
            if (!found && hasExternalSheetIndex() && !m_previewRelationsVerified) {
                found = findSheet(m_sheetInfos, false);
                if (found) info.previewFromExternalIndex = true;
            }
            if (!found && QFileInfo(path).isFile()) {
                QString shpDir = resolvePreprocessedShpDir(path, outputRootDir());
                if (shpDir.isEmpty() && allowPreprocess) {
                    const bool wasEnabled = isEnabled();
                    setEnabled(false);
                    const auto restoreUi = qScopeGuard([this, wasEnabled]() { setEnabled(wasEnabled); });
                    const PreprocessResult result = preprocessGeoPdfIfNeeded(path, outputRootDir());
                    if (result.success) shpDir = result.shpDir;
                    else appendRuntimeLog(QStringLiteral("预览无法读取 %1 的图廓：%2").arg(name, result.errorMessage));
                }
                if (!shpDir.isEmpty()) {
                    QString error;
                    if (readPreprocessedFrameGeometry(shpDir, &info.geometry, &info.crs, &error)) {
                        info.displayName = name;
                        info.scale = scaleForGeoPdf(path);
                        m_cachedPreviewSheets.insert(key, info);
                        found = true;
                    } else if (allowPreprocess) {
                        appendRuntimeLog(QStringLiteral("预览无法读取 %1 的图廓：%2").arg(name, error));
                    }
                }
            }
            if (!found) {
                found = findSheet(m_sheetInfos, false);
                if (found && hasExternalSheetIndex()) info.previewFromExternalIndex = true;
            }
            if (!found || info.geometry.isNull() || info.geometry.isEmpty()) continue;
            if (!displayCrs.isValid()) displayCrs = info.crs;
            else if (info.crs.isValid() && info.crs != displayCrs) {
                try {
                    if (info.geometry.transform(QgsCoordinateTransform(info.crs, displayCrs, QgsProject::instance()))
                        != Qgis::GeometryOperationResult::Success) {
                        if (allowPreprocess) appendRuntimeLog(QStringLiteral("预览无法转换 %1 的图廓坐标系。").arg(name));
                        continue;
                    }
                } catch (const QgsCsException&) {
                    if (allowPreprocess) appendRuntimeLog(QStringLiteral("预览无法转换 %1 的图廓坐标系。").arg(name));
                    continue;
                }
            }
            info.pdfPath = path;
            info.displayName = name;
            overviewInfos.push_back(info);
        }
    }

    m_displayedPreviewSheets.clear();
    QVector<SeriesSheetOverview::Sheet> sheets;
    qint64 primary = -1;
    QSet<qint64> related;
    for (auto info : overviewInfos) {
        if (info.geometry.isNull() || info.geometry.isEmpty()) continue;
        info.featureId = m_displayedPreviewSheets.size();
        SeriesSheetOverview::Sheet sheet;
        sheet.id=info.featureId;
        sheet.name=info.displayName;
        const QgsGeometry anchor=info.geometry.pointOnSurface();
        if (!anchor.isNull() && !anchor.isEmpty()) {
            const QgsPointXY point=anchor.asPoint();
            sheet.labelPoint=QPointF(point.x(),-point.y());
        } else {
            const QgsPointXY center=info.geometry.boundingBox().center();
            sheet.labelPoint=QPointF(center.x(),-center.y());
        }
        sheet.tooltip=info.displayName + (info.scale>0 ? QStringLiteral("\n比例尺 1:%1").arg(info.scale) : QString());
        if (info.previewFromExternalIndex) sheet.tooltip += QStringLiteral("\n外部索引图廓（候选）");
        QgsMultiPolygonXY polygons;
        if (info.geometry.isMultipart()) polygons=info.geometry.asMultiPolygon();
        else polygons.push_back(info.geometry.asPolygon());
        sheet.outline.setFillRule(Qt::OddEvenFill);
        for (const auto& polygon : polygons) for (const auto& ring : polygon) {
            QPolygonF points;
            for (const auto& point : ring) points.push_back(QPointF(point.x(),-point.y()));
            if (points.size()>=3) { sheet.outline.addPolygon(points); sheet.outline.closeSubpath(); }
        }
        if (sheet.outline.isEmpty()) continue;
        m_displayedPreviewSheets.push_back(info);
        sheets.push_back(sheet);
        if (!selectedMain.isEmpty() && pathKey(info.pdfPath) == pathKey(selectedMain)) primary = info.featureId;
        for (const QString& path : selectedAssociated)
            if (pathKey(info.pdfPath) == pathKey(path)) related.insert(info.featureId);
    }
    m_sheetOverview->setFocusSheets(primary, related);
    m_sheetOverview->setSheets(sheets);
}

void SeriesMapEdgeMatchTaskDialog::highlightConfigItem(QTreeWidgetItem* item)
{
    if (!item || m_loadingSheets || m_runningCheck || m_updatingOverview) return;
    m_previewMode = PreviewMode::Selection;
    updateOverview(true);
}

void SeriesMapEdgeMatchTaskDialog::locateOverviewSheet(qint64 id)
{
    const auto& overviewInfos = m_displayedPreviewSheets;
    auto sheet=std::find_if(overviewInfos.cbegin(),overviewInfos.cend(),[&](const SheetInfo& info) { return info.featureId==id; });
    if (sheet==overviewInfos.cend()) return;
    QTreeWidgetItem* candidate=nullptr;
    const QString sheetPath = sheet->pdfPath.isEmpty() ? QString()
        : QDir::cleanPath(QFileInfo(sheet->pdfPath).absoluteFilePath()).toLower();
    if (!sheetPath.isEmpty()) {
        for (int row = 0; row < m_configTree->topLevelItemCount() && !candidate; ++row) {
            auto* parent = m_configTree->topLevelItem(row);
            const auto* mainEdit = configLineEdit(row, SeriesColMainMap);
            if (mainEdit && !mainEdit->text().trimmed().isEmpty()
                && QDir::cleanPath(QFileInfo(mainEdit->text().trimmed()).absoluteFilePath()).toLower() == sheetPath) {
                candidate = parent;
                break;
            }
            for (int i = 0; i < parent->childCount(); ++i) {
                auto* child = parent->child(i);
                const QString assocPath = child->toolTip(SeriesColAssocMap).trimmed();
                if (!assocPath.isEmpty()
                    && QDir::cleanPath(QFileInfo(assocPath).absoluteFilePath()).toLower() == sheetPath) {
                    candidate = child;
                    break;
                }
            }
        }
    }
    for (int row=0;row<m_configTree->topLevelItemCount();++row) {
        if (candidate) break;
        auto* parent=m_configTree->topLevelItem(row);
        if (geoPdfBaseMatchesSheetStrict(sheet->displayName,parent->data(0,SeriesMainNameRole).toString())) {
            candidate=parent; break;
        }
        for (int i=0;i<parent->childCount() && !candidate;++i) {
            auto* child=parent->child(i);
            if (geoPdfBaseMatchesSheetStrict(sheet->displayName,child->data(0,SeriesAssocNameRole).toString())) candidate=child;
        }
    }
    if (candidate) {
        (candidate->parent() ? candidate->parent() : candidate)->setExpanded(true);
        m_configTree->setCurrentItem(candidate);
        m_configTree->scrollToItem(candidate);
        highlightConfigItem(candidate);
    } else m_sheetOverview->setFocusSheets(id,{});
}

QString SeriesMapEdgeMatchTaskDialog::buildTaskSummary(
    const QVector<SeriesMapEdgeMatchTaskDialog::AdjacentPair>& selectedPairs,
    const QVector<SeriesMapEdgeMatchTaskDialog::CoverageGroup>& selectedCoverageGroups) const
{
    QStringList lines;
    lines << QStringLiteral("已选择 %1 个检查任务：").arg(selectedPairs.size() + selectedCoverageGroups.size());

    int taskIdx = 1;
    for (int i = 0; i < selectedPairs.size(); ++i) {
        const SeriesMapEdgeMatchTaskDialog::AdjacentPair& pair = selectedPairs[i];
        lines << QStringLiteral("%1. [同比例尺] %2 - %3").arg(taskIdx++).arg(pair.leftName, pair.rightName);
    }
    for (int i = 0; i < selectedCoverageGroups.size(); ++i) {
        const SeriesMapEdgeMatchTaskDialog::CoverageGroup& group = selectedCoverageGroups[i];
        const QString largeNamesText = group.largeScaleNames.join(QStringLiteral("、"));
        const QString displayName = group.displayName.isEmpty()
            ? QStringLiteral("%1 - %2").arg(largeNamesText, group.smallScaleName)
            : group.displayName;
        lines << QStringLiteral("%1. [跨比例尺] %2")
            .arg(taskIdx++)
            .arg(displayName);
    }

    lines << QString();
    lines << QStringLiteral("待执行检查项：");
    lines << QStringLiteral("图幅接边与系列化表达");
    lines << QString();
    lines << QStringLiteral("使用 XML：");
    lines << m_xmlEdit->text().trimmed();
    lines << QString();
    lines << QStringLiteral("图幅索引文件：");
    lines << m_indexFileEdit->text().trimmed();
    lines << QString();
    lines << QStringLiteral("城市分组数量：%1").arg(m_configTree ? m_configTree->topLevelItemCount() : 0);
    lines << QStringLiteral("任务数量：%1").arg(selectedPairs.size() + selectedCoverageGroups.size());

    return lines.join("\n");
}

QString SeriesMapEdgeMatchTaskDialog::buildSeriesLayerMappingJson(QTreeWidget* tree, const QString& pairName) const
{
    QJsonObject root;
    root[QStringLiteral("pair_name")] = pairName;
    QJsonObject layers;

    if (!tree) {
        root[QStringLiteral("layers")] = layers;
        return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }

    auto appendCheckedMappingRows = [&](QTreeWidgetItem* rootItem) {
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* rowItem) {
            if (!rowItem) return;
            const QString rowType = rowItem->data(0, Qt::UserRole).toString();
            const bool isMappingRow = rowType == QStringLiteral("MappingRow") || rowType == QStringLiteral("FamilyCandidateRow");
            if (!isMappingRow) {
                for (int i = 0; i < rowItem->childCount(); ++i) visit(rowItem->child(i));
                return;
            }
            if (rowItem->checkState(0) != Qt::Checked) return;

            const QString stdName = rowItem->data(0, Qt::UserRole + 2).toString();
            if (stdName.isEmpty()) return;
            QStringList leftPathList = rowItem->data(0, Qt::UserRole + 9).toStringList();
            QStringList rightPathList = rowItem->data(0, Qt::UserRole + 10).toStringList();
            const QString leftPath = rowItem->data(0, Qt::UserRole + 3).toString();
            const QString rightPath = rowItem->data(0, Qt::UserRole + 4).toString();
            if (leftPathList.isEmpty() && !leftPath.isEmpty()) leftPathList << leftPath;
            if (rightPathList.isEmpty() && !rightPath.isEmpty()) rightPathList << rightPath;
            const QString matchFamily = rowItem->data(0, Qt::UserRole + 7).toString();
            const QString relationType = rowItem->data(0, Qt::UserRole + 8).toString();

            QJsonObject layerObj = layers.value(stdName).toObject();
            QJsonArray leftPaths = layerObj.value(QStringLiteral("left")).toArray();
            QJsonArray rightPaths = layerObj.value(QStringLiteral("right")).toArray();
            auto appendUniquePath = [](QJsonArray* array, const QString& path) {
                if (!array || path.isEmpty()) return;
                for (const QJsonValue& value : *array) {
                    if (value.toString().compare(path, Qt::CaseInsensitive) == 0) return;
                }
                array->append(path);
            };

            for (const QString& path : leftPathList) appendUniquePath(&leftPaths, path);
            for (const QString& path : rightPathList) appendUniquePath(&rightPaths, path);
            layerObj[QStringLiteral("left")] = leftPaths;
            layerObj[QStringLiteral("right")] = rightPaths;
            SeriesLayerMapping::appendRow(layerObj, leftPathList, rightPathList);
            if (!matchFamily.isEmpty()) layerObj[QStringLiteral("match_family")] = matchFamily;
            if (!relationType.isEmpty()) layerObj[QStringLiteral("relation_type")] = relationType;
            layers[stdName] = layerObj;
        };
        visit(rootItem);
    };

    for (int taskIndex = 0; taskIndex < tree->topLevelItemCount(); ++taskIndex) {
        QTreeWidgetItem* taskItem = tree->topLevelItem(taskIndex);
        if (!taskItem || taskItem->data(0, Qt::UserRole + 1).toString() != pairName) continue;
        appendCheckedMappingRows(taskItem);
    }

    root[QStringLiteral("layers")] = layers;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool SeriesMapEdgeMatchTaskDialog::confirmSeriesLayerMappings(
    const QVector<SeriesMapEdgeMatchTaskDialog::AdjacentPair>& selectedPairs,
    const QVector<SeriesMapEdgeMatchTaskDialog::CoverageGroup>& selectedCoverageGroups,
    const QMap<QString, QString>& pdfBaseNameToShpDir,
    int defaultScale,
    QMap<QString, QString>* confirmedLayerMappings)
{
    if (!confirmedLayerMappings) return false;
    confirmedLayerMappings->clear();

    const QString xmlPath = m_xmlEdit ? m_xmlEdit->text().trimmed() : QString();
    QStringList targets = seriesCheckTargetLayers(xmlPath, defaultScale);
    if (targets.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("XML 中未配置 series_target_layers 等 501 检查图层，无法进行图层对应确认。"));
        return false;
    }

    QMap<QString, QString> layerNameAliases = parseSeriesLayerNameAliasesForDialog(
        readSeriesRuleText(QStringList() << QStringLiteral("series_layer_name_aliases"), QString(), defaultScale));
    for (const QString& target : targets) {
        addSeriesLayerAliasGroupForDialog(layerNameAliases, target, QStringList());
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("图层对应确认"));
    dlg.resize(980, 640);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QStringLiteral("请确认本次 501 检查使用的图层对应关系。自动匹配到左右两侧的行已默认勾选，缺少一侧的行可手动补选，也可保持不勾选跳过。"), &dlg));

    auto* tree = new QTreeWidget(&dlg);
    tree->setColumnCount(5);
    tree->setHeaderLabels({
        QStringLiteral("检查"),
        QStringLiteral("标准图层"),
        QStringLiteral("主图图层"),
        QStringLiteral("关联图层"),
        QStringLiteral("状态")
    });
    tree->setRootIsDecorated(true);
    tree->setItemsExpandable(true);
    tree->setAllColumnsShowFocus(true);
    tree->header()->setSectionResizeMode(QHeaderView::Stretch);
    tree->header()->setMinimumSectionSize(120);
    layout->addWidget(tree, 1);

    auto updateMappingRowStatus = [](QTreeWidgetItem* rowItem, const QString& completeStatus) {
        if (!rowItem) return;
        QStringList leftPaths = rowItem->data(0, Qt::UserRole + 9).toStringList();
        QStringList rightPaths = rowItem->data(0, Qt::UserRole + 10).toStringList();
        const QString leftPath = rowItem->data(0, Qt::UserRole + 3).toString();
        const QString rightPath = rowItem->data(0, Qt::UserRole + 4).toString();
        if (leftPaths.isEmpty() && !leftPath.isEmpty()) leftPaths << leftPath;
        if (rightPaths.isEmpty() && !rightPath.isEmpty()) rightPaths << rightPath;
        if (leftPaths.isEmpty() && rightPaths.isEmpty()) {
            rowItem->setText(4, QStringLiteral("未找到候选"));
            rowItem->setCheckState(0, Qt::Unchecked);
        } else if (leftPaths.isEmpty()) {
            rowItem->setText(4, QStringLiteral("待补主图图层"));
            rowItem->setCheckState(0, Qt::Unchecked);
        } else if (rightPaths.isEmpty()) {
            rowItem->setText(4, QStringLiteral("待补关联图层"));
            rowItem->setCheckState(0, Qt::Unchecked);
        } else {
            rowItem->setText(4, completeStatus.isEmpty() ? QStringLiteral("自动匹配") : completeStatus);
        }
    };

    auto makeLayerSelector = [&](const QString& rootDirs, const QString& selectedPath, const QString& objectName) {
        auto* selector = new LayerSelection::SearchableLayerComboBox(tree);
        selector->setObjectName(objectName);
        selector->setMaxVisibleItems(20);
        selector->addItem(QStringLiteral("未选择"), QString());
        for (const QString& path : collectSeriesShpFilesForDialog(rootDirs)) {
            const int index = selector->count();
            selector->addItem(QFileInfo(path).fileName(), path);
            selector->setItemData(index, path, Qt::ToolTipRole);
        }
        if (!selectedPath.isEmpty() && selector->findData(selectedPath) < 0) {
            selector->addItem(QFileInfo(selectedPath).fileName(), selectedPath);
            selector->setItemData(selector->count() - 1, selectedPath, Qt::ToolTipRole);
        }
        selector->setCurrentIndex(qMax(0, selector->findData(selectedPath)));
        selector->setToolTip(selectedPath);
        selector->enableSingleSelectionSearch();
        return selector;
    };

    auto attachLayerSelectors = [&](QTreeWidgetItem* rowItem) {
        if (!rowItem) return;
        const QString leftPath = rowItem->data(0, Qt::UserRole + 3).toString();
        const QString rightPath = rowItem->data(0, Qt::UserRole + 4).toString();
        auto* leftSelector = makeLayerSelector(
            rowItem->data(0, Qt::UserRole + 5).toString(), leftPath,
            QStringLiteral("edgeMatchLeftLayerSelector"));
        auto* rightSelector = makeLayerSelector(
            rowItem->data(0, Qt::UserRole + 6).toString(), rightPath,
            QStringLiteral("edgeMatchRightLayerSelector"));
        tree->setItemWidget(rowItem, 2, leftSelector);
        tree->setItemWidget(rowItem, 3, rightSelector);

        auto refreshRowMapping = [rowItem, leftSelector, rightSelector, &updateMappingRowStatus]() {
            const QString selectedLeftPath = leftSelector->currentData().toString();
            const QString selectedRightPath = rightSelector->currentData().toString();
            rowItem->setData(0, Qt::UserRole + 3, selectedLeftPath);
            rowItem->setData(0, Qt::UserRole + 4, selectedRightPath);
            rowItem->setData(0, Qt::UserRole + 9,
                selectedLeftPath.isEmpty() ? QStringList() : QStringList() << selectedLeftPath);
            rowItem->setData(0, Qt::UserRole + 10,
                selectedRightPath.isEmpty() ? QStringList() : QStringList() << selectedRightPath);
            rowItem->setText(2, selectedLeftPath.isEmpty() ? QString() : QFileInfo(selectedLeftPath).fileName());
            rowItem->setText(3, selectedRightPath.isEmpty() ? QString() : QFileInfo(selectedRightPath).fileName());
            rowItem->setToolTip(2, selectedLeftPath);
            rowItem->setToolTip(3, selectedRightPath);
            leftSelector->setToolTip(selectedLeftPath);
            rightSelector->setToolTip(selectedRightPath);

            const QString originalLeftPath = rowItem->data(0, Qt::UserRole + 12).toString();
            const QString originalRightPath = rowItem->data(0, Qt::UserRole + 13).toString();
            const bool unchanged = selectedLeftPath.compare(originalLeftPath, Qt::CaseInsensitive) == 0
                && selectedRightPath.compare(originalRightPath, Qt::CaseInsensitive) == 0;
            const QString status = unchanged
                ? rowItem->data(0, Qt::UserRole + 14).toString()
                : QStringLiteral("人工调整");
            updateMappingRowStatus(rowItem, status);
        };
        QObject::connect(leftSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), tree,
            [refreshRowMapping](int) { refreshRowMapping(); });
        QObject::connect(rightSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), tree,
            [refreshRowMapping](int) { refreshRowMapping(); });
        refreshRowMapping();
    };

    auto addLayerMappingRow = [&](QTreeWidgetItem* taskItem, const QString& target, const QString& displayName,
                                  const QString& leftPath, const QString& rightPath, const QString& status,
                                  const QString& leftRootDir, const QString& rightRootDir,
                                  const QString& matchFamily = QString(), const QString& relationType = QString()) -> QTreeWidgetItem* {
        if (!taskItem) return nullptr;

        const QStringList leftPaths = leftPath.isEmpty() ? QStringList() : (QStringList() << leftPath);
        const QStringList rightPaths = rightPath.isEmpty() ? QStringList() : (QStringList() << rightPath);

        auto* rowItem = new QTreeWidgetItem(taskItem);
        rowItem->setFlags(rowItem->flags() | Qt::ItemIsUserCheckable);
        rowItem->setCheckState(0, leftPaths.isEmpty() || rightPaths.isEmpty() ? Qt::Unchecked : Qt::Checked);
        rowItem->setText(1, displayName.isEmpty() ? target : displayName);
        rowItem->setText(2, leftPath.isEmpty() ? QString() : QFileInfo(leftPath).fileName());
        rowItem->setText(3, rightPath.isEmpty() ? QString() : QFileInfo(rightPath).fileName());
        rowItem->setText(4, status);
        rowItem->setToolTip(2, leftPath);
        rowItem->setToolTip(3, rightPath);
        rowItem->setData(0, Qt::UserRole, QStringLiteral("MappingRow"));
        rowItem->setData(0, Qt::UserRole + 1, taskItem->data(0, Qt::UserRole + 1).toString());
        rowItem->setData(0, Qt::UserRole + 2, target);
        rowItem->setData(0, Qt::UserRole + 3, leftPath);
        rowItem->setData(0, Qt::UserRole + 4, rightPath);
        rowItem->setData(0, Qt::UserRole + 5, leftRootDir);
        rowItem->setData(0, Qt::UserRole + 6, rightRootDir);
        rowItem->setData(0, Qt::UserRole + 7, matchFamily);
        rowItem->setData(0, Qt::UserRole + 8, relationType);
        rowItem->setData(0, Qt::UserRole + 12, leftPath);
        rowItem->setData(0, Qt::UserRole + 13, rightPath);
        rowItem->setData(0, Qt::UserRole + 14, status);
        updateMappingRowStatus(rowItem, status);
        attachLayerSelectors(rowItem);
        return rowItem;
    };

    auto addLayerMappingFamilyTree = [&](QTreeWidgetItem* taskItem, const QString& target,
                                         const QStringList& leftPaths, const QStringList& rightPaths,
                                         const QString& leftRootDir, const QString& rightRootDir,
                                         const QString& matchFamily, const QString& relationType) -> QTreeWidgetItem* {
        if (!taskItem) return nullptr;

        auto* familyItem = new QTreeWidgetItem(taskItem);
        familyItem->setText(1, target);
        familyItem->setText(2, QStringLiteral("%1 个主图图层").arg(leftPaths.size()));
        familyItem->setText(3, QStringLiteral("%1 个关联候选图层").arg(rightPaths.size()));
        familyItem->setText(4, QStringLiteral("跨比例尺族群"));
        familyItem->setData(0, Qt::UserRole, QStringLiteral("FamilyGroupRow"));
        familyItem->setData(0, Qt::UserRole + 1, taskItem->data(0, Qt::UserRole + 1).toString());
        familyItem->setData(0, Qt::UserRole + 2, target);
        familyItem->setData(0, Qt::UserRole + 5, leftRootDir);
        familyItem->setData(0, Qt::UserRole + 6, rightRootDir);
        familyItem->setData(0, Qt::UserRole + 7, matchFamily);
        familyItem->setData(0, Qt::UserRole + 8, relationType);

        QSet<QString> displayedRightPaths;
        for (const QString& leftPath : leftPaths) {
            QVector<QPair<QString, QString>> compatibleRightPaths;
            for (const QString& rightPath : rightPaths) {
                QString candidateStatus;
                const bool candidateDefaultChecked = seriesLayerPairDefaultCheckedForDialog(leftPath, rightPath, layerNameAliases, &candidateStatus);
                if (!candidateDefaultChecked) continue;
                if (!seriesLayerGeometryCompatibleForDialog(leftPath, rightPath)) continue;
                compatibleRightPaths.push_back(qMakePair(rightPath, candidateStatus));
                displayedRightPaths.insert(rightPath);
            }

            if (compatibleRightPaths.isEmpty()) {
                addLayerMappingRow(
                    familyItem,
                    target,
                    normalizeSeriesLayerFileBaseForDialog(QFileInfo(leftPath).fileName()),
                    leftPath,
                    QString(),
                    QStringLiteral("待补关联图层"),
                    leftRootDir,
                    rightRootDir,
                    matchFamily,
                    relationType);
                continue;
            }

            int bestScore = 0, bestCount = 0;
            QString bestPath;
            for (const auto& candidate : compatibleRightPaths) {
                const int score = seriesLayerNameMatchScoreForDialog(leftPath, candidate.first, layerNameAliases);
                if (score > bestScore) { bestScore = score; bestCount = 1; bestPath = candidate.first; }
                else if (score > 0 && score == bestScore) ++bestCount;
            }
            const bool uniqueMatch = bestScore > 0 && bestCount == 1;

            if (compatibleRightPaths.size() == 1) {
                const QString singleRightPath = compatibleRightPaths.first().first;
                const QString singleCandidateStatus = compatibleRightPaths.first().second;
                QTreeWidgetItem* rowItem = addLayerMappingRow(
                    familyItem,
                    target,
                    normalizeSeriesLayerFileBaseForDialog(QFileInfo(leftPath).fileName()),
                    leftPath,
                    singleRightPath,
                    uniqueMatch ? QStringLiteral("自动匹配") : QStringLiteral("候选待确认"),
                    leftRootDir,
                    rightRootDir,
                    matchFamily,
                    relationType);
                if (rowItem) {
                    rowItem->setCheckState(0, uniqueMatch ? Qt::Checked : Qt::Unchecked);
                    rowItem->setData(0, Qt::UserRole + 11, singleCandidateStatus);
                    rowItem->setToolTip(4, singleCandidateStatus.isEmpty() ? QStringLiteral("跨比例尺候选") : singleCandidateStatus);
                }
                continue;
            }

            auto* mainItem = new QTreeWidgetItem(familyItem);
            mainItem->setText(1, normalizeSeriesLayerFileBaseForDialog(QFileInfo(leftPath).fileName()));
            mainItem->setText(2, QFileInfo(leftPath).fileName());
            mainItem->setText(3, QStringLiteral("%1 个兼容关联候选").arg(compatibleRightPaths.size()));
            mainItem->setText(4, QStringLiteral("候选分组"));
            mainItem->setToolTip(2, leftPath);
            mainItem->setData(0, Qt::UserRole, QStringLiteral("FamilyMainLayerRow"));
            mainItem->setData(0, Qt::UserRole + 1, taskItem->data(0, Qt::UserRole + 1).toString());
            mainItem->setData(0, Qt::UserRole + 2, target);
            mainItem->setData(0, Qt::UserRole + 3, leftPath);
            mainItem->setData(0, Qt::UserRole + 5, leftRootDir);
            mainItem->setData(0, Qt::UserRole + 6, rightRootDir);
            mainItem->setData(0, Qt::UserRole + 7, matchFamily);
            mainItem->setData(0, Qt::UserRole + 8, relationType);

            for (const auto& compatibleRight : compatibleRightPaths) {
                const QString rightPath = compatibleRight.first;
                const QString candidateStatus = compatibleRight.second;
                auto* candidateItem = new QTreeWidgetItem(mainItem);
                candidateItem->setFlags(candidateItem->flags() | Qt::ItemIsUserCheckable);
                const bool autoSelected = uniqueMatch && rightPath == bestPath;
                const QString matchStatus = autoSelected ? QStringLiteral("自动匹配")
                    : bestCount > 1 ? QStringLiteral("多个候选，待确认") : QStringLiteral("候选待确认");
                candidateItem->setCheckState(0, autoSelected ? Qt::Checked : Qt::Unchecked);
                candidateItem->setText(1, QString());
                candidateItem->setText(2, QFileInfo(leftPath).fileName());
                candidateItem->setText(3, QFileInfo(rightPath).fileName());
                candidateItem->setText(4, matchStatus);
                candidateItem->setToolTip(2, leftPath);
                candidateItem->setToolTip(3, rightPath);
                candidateItem->setData(0, Qt::UserRole, QStringLiteral("FamilyCandidateRow"));
                candidateItem->setData(0, Qt::UserRole + 1, taskItem->data(0, Qt::UserRole + 1).toString());
                candidateItem->setData(0, Qt::UserRole + 2, target);
                candidateItem->setData(0, Qt::UserRole + 3, leftPath);
                candidateItem->setData(0, Qt::UserRole + 4, rightPath);
                candidateItem->setData(0, Qt::UserRole + 5, leftRootDir);
                candidateItem->setData(0, Qt::UserRole + 6, rightRootDir);
                candidateItem->setData(0, Qt::UserRole + 7, matchFamily);
                candidateItem->setData(0, Qt::UserRole + 8, relationType);
                candidateItem->setData(0, Qt::UserRole + 9, QStringList() << leftPath);
                candidateItem->setData(0, Qt::UserRole + 10, QStringList() << rightPath);
                candidateItem->setData(0, Qt::UserRole + 12, leftPath);
                candidateItem->setData(0, Qt::UserRole + 13, rightPath);
                candidateItem->setData(0, Qt::UserRole + 14, matchStatus);
                attachLayerSelectors(candidateItem);
            }
            mainItem->setExpanded(false);
        }
        for (const QString& rightPath : rightPaths) {
            if (displayedRightPaths.contains(rightPath)) continue;
            const QString rightBase = normalizeSeriesLayerFileBaseForDialog(QFileInfo(rightPath).fileName());
            addLayerMappingRow(
                familyItem,
                target,
                rightBase.isEmpty() ? target : rightBase,
                QString(),
                rightPath,
                QStringLiteral("待补主图图层"),
                leftRootDir,
                rightRootDir,
                matchFamily,
                relationType);
        }
        familyItem->setExpanded(true);
        return familyItem;
    };

    auto addTaskNode = [&](const QString& pairName, const QString& leftRootDir, const QString& rightRootDir, const bool crossScaleTask) {
        auto* taskItem = new QTreeWidgetItem(tree);
        taskItem->setText(0, pairName);
        taskItem->setText(4, QStringLiteral("检查任务"));
        taskItem->setData(0, Qt::UserRole, QStringLiteral("Task"));
        taskItem->setData(0, Qt::UserRole + 1, pairName);
        tree->setFirstItemColumnSpanned(taskItem, true);

        for (const QString& target : targets) {
            const QStringList aliasLeftPaths = collectSeriesShpFilesByAliasForDialog(target, layerNameAliases, leftRootDir);
            const QStringList aliasRightPaths = collectSeriesShpFilesByAliasForDialog(target, layerNameAliases, rightRootDir);
            const QStringList familyLeftPaths = crossScaleTask ? collectSeriesShpFilesByFamilyForDialog(target, layerNameAliases, leftRootDir) : QStringList();
            const QStringList familyRightPaths = crossScaleTask ? collectSeriesShpFilesByFamilyForDialog(target, layerNameAliases, rightRootDir) : QStringList();
            const QString targetFamily = crossScaleTask ? seriesLayerFamilyForDialog(target, layerNameAliases) : QString();
            const bool aggregateFamilyTarget = crossScaleTask
                && (targetFamily == QStringLiteral("road") || targetFamily == QStringLiteral("hydrology") || targetFamily == QStringLiteral("hydrology_area"))
                && seriesLayerBusinessClassForDialog(target, layerNameAliases).isEmpty()
                && (aliasLeftPaths.size() > 1 || aliasRightPaths.size() > 1 || familyLeftPaths.size() > 1 || familyRightPaths.size() > 1);
            const bool useFamilyGroup = crossScaleTask && !familyLeftPaths.isEmpty() && !familyRightPaths.isEmpty()
                && (aggregateFamilyTarget || aliasLeftPaths.isEmpty() || aliasRightPaths.isEmpty() || aliasLeftPaths.size() != familyLeftPaths.size() || aliasRightPaths.size() != familyRightPaths.size());
            const QStringList leftPaths = useFamilyGroup ? familyLeftPaths : aliasLeftPaths;
            const QStringList rightPaths = useFamilyGroup ? familyRightPaths : aliasRightPaths;
            const QString matchFamily = useFamilyGroup ? targetFamily : QString();
            const QString relationType = useFamilyGroup ? QStringLiteral("cross_scale_family_group") : QString();

            if (useFamilyGroup) {
                addLayerMappingFamilyTree(taskItem, target, leftPaths, rightPaths,
                    leftRootDir, rightRootDir, matchFamily, relationType);
                continue;
            }

            QSet<int> usedRightIndexes;
            for (const QString& leftPath : leftPaths) {
                const QString leftBase = normalizeSeriesLayerFileBaseForDialog(QFileInfo(leftPath).fileName());
                int matchedRightIndex = -1;
                int bestScore = 0, bestCount = 0;
                for (int rightIndex = 0; rightIndex < rightPaths.size(); ++rightIndex) {
                    if (usedRightIndexes.contains(rightIndex)) continue;
                    if (!seriesLayerGeometryCompatibleForDialog(leftPath, rightPaths[rightIndex])) continue;
                    const int score = seriesLayerNameMatchScoreForDialog(leftPath, rightPaths[rightIndex], layerNameAliases);
                    if (score > bestScore) {
                        bestScore = score; bestCount = 1; matchedRightIndex = rightIndex;
                    } else if (score > 0 && score == bestScore) ++bestCount;
                }
                if (bestCount != 1) matchedRightIndex = -1;

                QString rightPath;
                if (matchedRightIndex >= 0) {
                    rightPath = rightPaths[matchedRightIndex];
                    usedRightIndexes.insert(matchedRightIndex);
                }
                addLayerMappingRow(taskItem, target, leftBase.isEmpty() ? target : leftBase, leftPath, rightPath,
                    rightPath.isEmpty() ? (bestCount > 1 ? QStringLiteral("多个候选，待确认") : QStringLiteral("待补关联图层"))
                                        : QStringLiteral("自动匹配"), leftRootDir, rightRootDir);
            }

            for (int rightIndex = 0; rightIndex < rightPaths.size(); ++rightIndex) {
                if (usedRightIndexes.contains(rightIndex)) continue;
                const QString rightPath = rightPaths[rightIndex];
                const QString rightBase = normalizeSeriesLayerFileBaseForDialog(QFileInfo(rightPath).fileName());
                addLayerMappingRow(taskItem, target, rightBase.isEmpty() ? target : rightBase, QString(), rightPath,
                    QStringLiteral("待补主图图层"), leftRootDir, rightRootDir);
            }

            if (leftPaths.isEmpty() && rightPaths.isEmpty()) {
                addLayerMappingRow(taskItem, target, target, QString(), QString(), QStringLiteral("未找到候选"), leftRootDir, rightRootDir);
            }
        }
        taskItem->setExpanded(true);
    };

    for (const AdjacentPair& pair : selectedPairs) {
        const QString leftShpDir = resolveShpDirForSheetName(pair.leftName, pdfBaseNameToShpDir);
        const QString rightShpDir = resolveShpDirForSheetName(pair.rightName, pdfBaseNameToShpDir);
        addTaskNode(QStringLiteral("%1 - %2").arg(pair.leftName, pair.rightName), leftShpDir, rightShpDir, false);
    }
    for (const CoverageGroup& group : selectedCoverageGroups) {
        QStringList largeDirs;
        for (const QString& largeName : group.largeScaleNames) {
            const QString largeDir = resolveShpDirForSheetName(largeName, pdfBaseNameToShpDir);
            if (!largeDir.isEmpty() && !largeDirs.contains(largeDir)) largeDirs << largeDir;
        }
        const QString smallDir = resolveShpDirForSheetName(group.smallScaleName, pdfBaseNameToShpDir);
        const QString displayName = group.displayName.isEmpty()
            ? QStringLiteral("%1 - %2").arg(group.largeScaleNames.join(QStringLiteral("、")), group.smallScaleName)
            : group.displayName;
        addTaskNode(displayName,
            largeDirs.join(QStringLiteral(";")), smallDir, true);
    }

    auto selectedMappingRow = [&]() -> QTreeWidgetItem* {
        QTreeWidgetItem* item = tree->currentItem();
        const QString rowType = item ? item->data(0, Qt::UserRole).toString() : QString();
        if (item && (rowType == QStringLiteral("MappingRow") || rowType == QStringLiteral("FamilyCandidateRow"))) {
            return item;
        }
        return nullptr;
    };

    auto* toolbar = new QHBoxLayout();
    auto* addMappingRowButton = new QPushButton(QStringLiteral("新增对应行"), &dlg);
    addMappingRowButton->setObjectName(QStringLiteral("addEdgeMatchMappingRowButton"));
    auto* checkAllButton = new QPushButton(QStringLiteral("全选"), &dlg);
    auto* clearAllButton = new QPushButton(QStringLiteral("取消全选"), &dlg);
    toolbar->addWidget(addMappingRowButton);
    toolbar->addWidget(checkAllButton);
    toolbar->addWidget(clearAllButton);
    toolbar->addWidget(new QLabel(QStringLiteral("展开两侧下拉框后可输入文字筛选图层；勾选的完整对应关系才参与检查。"), &dlg));
    toolbar->addStretch();
    layout->addLayout(toolbar);

    connect(addMappingRowButton, &QPushButton::clicked, &dlg, [&]() {
        QTreeWidgetItem* selectedRow = selectedMappingRow();
        if (!selectedRow) {
            QMessageBox::information(&dlg, QStringLiteral("提示"), QStringLiteral("请先选中一行图层对应关系。"));
            return;
        }
        QTreeWidgetItem* parentItem = selectedRow->parent();
        if (!parentItem) return;

        const QString target = selectedRow->data(0, Qt::UserRole + 2).toString();
        const QString leftRootDir = selectedRow->data(0, Qt::UserRole + 5).toString();
        const QString rightRootDir = selectedRow->data(0, Qt::UserRole + 6).toString();
        QString displayName = selectedRow->text(1).trimmed();
        if (displayName.isEmpty()) displayName = parentItem->text(1).trimmed();
        if (displayName.isEmpty()) displayName = target;

        QTreeWidgetItem* newRow = addLayerMappingRow(
            parentItem,
            target,
            displayName,
            QString(),
            QString(),
            QStringLiteral("未找到候选"),
            leftRootDir,
            rightRootDir,
            selectedRow->data(0, Qt::UserRole + 7).toString(),
            selectedRow->data(0, Qt::UserRole + 8).toString());
        if (!newRow) return;

        newRow->setCheckState(0, Qt::Unchecked);
        for (QTreeWidgetItem* ancestor = parentItem; ancestor; ancestor = ancestor->parent()) {
            ancestor->setExpanded(true);
        }
        tree->setCurrentItem(newRow);
        tree->scrollToItem(newRow);
    });
    auto forEachCheckableMappingRow = [&](const std::function<void(QTreeWidgetItem*)>& fn) {
        std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
            if (!item) return;
            const QString rowType = item->data(0, Qt::UserRole).toString();
            if (rowType == QStringLiteral("MappingRow") || rowType == QStringLiteral("FamilyCandidateRow")) {
                fn(item);
            }
            for (int i = 0; i < item->childCount(); ++i) visit(item->child(i));
        };
        for (int t = 0; t < tree->topLevelItemCount(); ++t) visit(tree->topLevelItem(t));
    };
    connect(checkAllButton, &QPushButton::clicked, &dlg, [&]() {
        forEachCheckableMappingRow([&](QTreeWidgetItem* rowItem) {
            QStringList leftPaths = rowItem->data(0, Qt::UserRole + 9).toStringList();
            QStringList rightPaths = rowItem->data(0, Qt::UserRole + 10).toStringList();
            const QString leftPath = rowItem->data(0, Qt::UserRole + 3).toString();
            const QString rightPath = rowItem->data(0, Qt::UserRole + 4).toString();
            if (leftPaths.isEmpty() && !leftPath.isEmpty()) leftPaths << leftPath;
            if (rightPaths.isEmpty() && !rightPath.isEmpty()) rightPaths << rightPath;
            rowItem->setCheckState(0, !leftPaths.isEmpty() && !rightPaths.isEmpty() ? Qt::Checked : Qt::Unchecked);
        });
    });
    connect(clearAllButton, &QPushButton::clicked, &dlg, [&]() {
        forEachCheckableMappingRow([](QTreeWidgetItem* rowItem) {
            rowItem->setCheckState(0, Qt::Unchecked);
        });
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    if (QPushButton* okButton = buttons->button(QDialogButtonBox::Ok)) {
        okButton->setText(QStringLiteral("确认并开始检查"));
    }
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
        bool valid = true;
        forEachCheckableMappingRow([&](QTreeWidgetItem* row) {
            if (!valid || row->checkState(0) != Qt::Checked) return;
            const QString left = row->data(0, Qt::UserRole + 3).toString();
            const QString right = row->data(0, Qt::UserRole + 4).toString();
            if (left.isEmpty() || right.isEmpty() || !seriesLayerGeometryCompatibleForDialog(left, right)) {
                valid = false;
                tree->setCurrentItem(row);
                tree->scrollToItem(row);
                QMessageBox::warning(&dlg, QStringLiteral("图层对应未完成"),
                    QStringLiteral("勾选的对应行需要选择两侧图层，且点、线、面几何类型必须兼容。请调整或取消勾选该行。"));
            }
        });
        if (valid) dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;

    for (int taskIndex = 0; taskIndex < tree->topLevelItemCount(); ++taskIndex) {
        QTreeWidgetItem* taskItem = tree->topLevelItem(taskIndex);
        if (!taskItem) continue;
        const QString pairName = taskItem->data(0, Qt::UserRole + 1).toString();
        if (!pairName.isEmpty()) {
            confirmedLayerMappings->insert(pairName, buildSeriesLayerMappingJson(tree, pairName));
        }
    }
    return true;
}

bool SeriesMapEdgeMatchTaskDialog::writeCurrentOutputs(const QStringList& logLines)
{
    QVector<QJsonObject> finalRecords;
    auto appendConfirmed = [&](const QVector<QJsonObject>& records, const QString& stage) {
        for (QJsonObject err : records) {
            err = normalizeSeriesFindingStatus(err);
            if (!isConfirmedSeriesError(err)) continue;
            if (err.value(QStringLiteral("stage")).toString().isEmpty()) err[QStringLiteral("stage")] = stage;
            if (err.value(QStringLiteral("final_state")).toString().isEmpty()) err[QStringLiteral("final_state")] = QStringLiteral("confirmed");
            finalRecords.push_back(err);
        }
    };
    appendConfirmed(m_lastErrorRecords, QStringLiteral("auto"));
    QVector<QJsonObject> reviewRecords;
    for (QJsonObject err : m_originalErrorRecords) {
        err = normalizeSeriesFindingStatus(err);
        if (!isSeriesReviewFinding(err)) continue;
        if (err.value(QStringLiteral("stage")).toString().isEmpty()) err[QStringLiteral("stage")] = QStringLiteral("auto");
        reviewRecords.push_back(err);
    }
    assignUniqueSeriesOutputIds(finalRecords);
    assignUniqueSeriesOutputIds(reviewRecords);
    appendSeriesDebugLog(QStringLiteral("[501][output] writeCurrentOutputs begin lastRecords=%1 finalRecords=%2")
        .arg(m_lastErrorRecords.size()).arg(finalRecords.size()));

    QgsCoordinateReferenceSystem crs;
    QString indexPath = m_indexFileEdit ? m_indexFileEdit->text().trimmed() : QString();
    if (indexPath.isEmpty()) {
        const QString manifestPath = QDir(outputRootDir()).filePath(QStringLiteral(".series_cache/merged_index_manifest.json"));
        QFile manifestFile(manifestPath);
        if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QJsonDocument manifest = QJsonDocument::fromJson(manifestFile.readAll());
            if (manifest.isObject()) indexPath = manifest.object().value(QStringLiteral("merged_index")).toString();
        }
        if (indexPath.isEmpty()) indexPath = QDir(outputRootDir()).filePath(QStringLiteral("merged_index.shp"));
    }
    QgsVectorLayer indexLayer(indexPath, QStringLiteral("index"), QStringLiteral("ogr"));
    if (indexLayer.isValid()) crs = indexLayer.crs();

    QgsFields fields;
    fields.append(QgsField(QStringLiteral("err_id"), QVariant::String, QStringLiteral("string"), 40));
    fields.append(QgsField(QStringLiteral("err_type"), QVariant::String, QStringLiteral("string"), 60));
    fields.append(QgsField(QStringLiteral("sub_rule"), QVariant::String, QStringLiteral("string"), 80));
    fields.append(QgsField(QStringLiteral("final_st"), QVariant::String, QStringLiteral("string"), 20));
    fields.append(QgsField(QStringLiteral("pair_name"), QVariant::String, QStringLiteral("string"), 120));
    fields.append(QgsField(QStringLiteral("l_sheet"), QVariant::String, QStringLiteral("string"), 60));
    fields.append(QgsField(QStringLiteral("r_sheet"), QVariant::String, QStringLiteral("string"), 60));
    fields.append(QgsField(QStringLiteral("layer"), QVariant::String, QStringLiteral("string"), 80));
    fields.append(QgsField(QStringLiteral("l_shp"), QVariant::String, QStringLiteral("string"), 120));
    fields.append(QgsField(QStringLiteral("r_shp"), QVariant::String, QStringLiteral("string"), 120));
    fields.append(QgsField(QStringLiteral("l_fid"), QVariant::String, QStringLiteral("string"), 40));
    fields.append(QgsField(QStringLiteral("r_fid"), QVariant::String, QStringLiteral("string"), 40));
    fields.append(QgsField(QStringLiteral("bad_side"), QVariant::String, QStringLiteral("string"), 16));
    fields.append(QgsField(QStringLiteral("field"), QVariant::String, QStringLiteral("string"), 40));
    fields.append(QgsField(QStringLiteral("actual"), QVariant::String, QStringLiteral("string"), 120));
    fields.append(QgsField(QStringLiteral("expect"), QVariant::String, QStringLiteral("string"), 120));
    fields.append(QgsField(QStringLiteral("distance"), QVariant::Double));
    fields.append(QgsField(QStringLiteral("tolerance"), QVariant::Double));
    fields.append(QgsField(QStringLiteral("message"), QVariant::String, QStringLiteral("string"), 254));

    for (int i = 0; i < finalRecords.size(); ++i) {
        QJsonObject& err = finalRecords[i];
        const QString errId = err.value(QStringLiteral("err_id")).toString().isEmpty()
            ? QStringLiteral("error%1").arg(i + 1)
            : err.value(QStringLiteral("err_id")).toString();
        err[QStringLiteral("err_id")] = errId;

        QgsPointXY point;
        QString pointSource;
        if (resolveSeriesErrorPoint(err, point, pointSource)) {
            err[QStringLiteral("point_x")] = point.x();
            err[QStringLiteral("point_y")] = point.y();
            err[QStringLiteral("point_source")] = pointSource;
        }
    }

    auto setSeriesFeatureAttributes = [&](QgsFeature& feature, const QJsonObject& err, const QString&) {
        const QString leftValue = err.value(QStringLiteral("left_value")).toVariant().toString();
        const QString rightValue = err.value(QStringLiteral("right_value")).toVariant().toString();
        QString actualValue = err.value(QStringLiteral("actual_value")).toVariant().toString();
        if (actualValue.isEmpty()) actualValue = !leftValue.isEmpty() ? leftValue : rightValue;
        QString expectedValue = err.value(QStringLiteral("expected_value")).toVariant().toString();
        if (expectedValue.isEmpty()) expectedValue = err.value(QStringLiteral("expected_color")).toVariant().toString();
        if (expectedValue.isEmpty()) expectedValue = err.value(QStringLiteral("expected_width_mm")).toVariant().toString();
        if (expectedValue.isEmpty()) expectedValue = err.value(QStringLiteral("expected_font_size_mm")).toVariant().toString();
        if (expectedValue.isEmpty() && !leftValue.isEmpty() && !rightValue.isEmpty()) expectedValue = rightValue;
        feature.setAttribute(QStringLiteral("err_id"), err.value(QStringLiteral("err_id")).toString());
        feature.setAttribute(QStringLiteral("err_type"), err.value(QStringLiteral("error_type")).toString());
        feature.setAttribute(QStringLiteral("sub_rule"), err.value(QStringLiteral("sub_rule")).toString());
        feature.setAttribute(QStringLiteral("final_st"), err.value(QStringLiteral("final_state")).toString(QStringLiteral("confirmed")));
        feature.setAttribute(QStringLiteral("pair_name"), err.value(QStringLiteral("pair_name")).toString());
        feature.setAttribute(QStringLiteral("l_sheet"), err.value(QStringLiteral("left_sheet")).toString());
        feature.setAttribute(QStringLiteral("r_sheet"), err.value(QStringLiteral("right_sheet")).toString());
        feature.setAttribute(QStringLiteral("layer"), err.value(QStringLiteral("layer")).toString());
        feature.setAttribute(QStringLiteral("l_shp"), err.value(QStringLiteral("left_shp")).toString());
        feature.setAttribute(QStringLiteral("r_shp"), err.value(QStringLiteral("right_shp")).toString());
        feature.setAttribute(QStringLiteral("l_fid"), err.value(QStringLiteral("left_fid")).toString());
        feature.setAttribute(QStringLiteral("r_fid"), err.value(QStringLiteral("right_fid")).toString());
        feature.setAttribute(QStringLiteral("bad_side"), err.value(QStringLiteral("invalid_side")).toString(err.value(QStringLiteral("source_side")).toString()));
        feature.setAttribute(QStringLiteral("field"), err.value(QStringLiteral("field")).toString());
        feature.setAttribute(QStringLiteral("actual"), actualValue);
        feature.setAttribute(QStringLiteral("expect"), expectedValue);
        feature.setAttribute(QStringLiteral("distance"), jsonDoubleVariant(err, QStringLiteral("distance")));
        feature.setAttribute(QStringLiteral("tolerance"), jsonDoubleVariant(err, QStringLiteral("tolerance")));
        feature.setAttribute(QStringLiteral("message"), err.value(QStringLiteral("description")).toString());
    };

    struct OriginalErrorFeatureOutput
    {
        QJsonObject err;
        QgsGeometry geometry;
        QString role;
        QString side;
        QString sheet;
        QString layer;
        QString shp;
        QString fid;
    };

    auto originalFeatureAttributes = [&](const OriginalErrorFeatureOutput& output) {
        const QJsonObject& err = output.err;
        QVariantMap attrs;
        attrs[QStringLiteral("err_id")] = err.value(QStringLiteral("err_id")).toString();
        attrs[QStringLiteral("err_type")] = err.value(QStringLiteral("error_type")).toString();
        attrs[QStringLiteral("sub_rule")] = err.value(QStringLiteral("sub_rule")).toString();
        attrs[QStringLiteral("src_side")] = output.side;
        attrs[QStringLiteral("src_sheet")] = output.sheet;
        attrs[QStringLiteral("src_layer")] = output.shp.isEmpty() ? output.layer : QFileInfo(output.shp).completeBaseName();
        attrs[QStringLiteral("src_fid")] = output.fid;
        attrs[QStringLiteral("message")] = err.value(QStringLiteral("description")).toString();
        auto addText = [&](const QString& key, const QJsonValue& value) {
            if (!value.isNull() && !value.isUndefined() && !value.toVariant().toString().trimmed().isEmpty())
                attrs[key] = value.toVariant().toString();
        };
        addText(QStringLiteral("field"), err.value(QStringLiteral("field")));
        const QString sideKey = output.side == QStringLiteral("左") ? QStringLiteral("left_value") : QStringLiteral("right_value");
        addText(QStringLiteral("actual_val"), err.contains(sideKey) ? err.value(sideKey) : err.value(QStringLiteral("actual_value")));
        addText(QStringLiteral("expect_val"), err.value(QStringLiteral("expected_value")));
        const QString fieldName = err.value(QStringLiteral("field")).toString().toLower();
        if (!attrs.contains(QStringLiteral("expect_val"))) {
            if (fieldName.contains(QStringLiteral("col"))) addText(QStringLiteral("expect_val"), err.value(QStringLiteral("expected_color")));
            else if (fieldName.contains(QStringLiteral("font")) || fieldName.contains(QStringLiteral("size"))) addText(QStringLiteral("expect_val"), err.value(QStringLiteral("expected_font_size_mm")));
            else if (fieldName.contains(QStringLiteral("wid"))) addText(QStringLiteral("expect_val"), err.value(QStringLiteral("expected_width_mm")));
        }
        const QString peerKey = output.side == QStringLiteral("左") ? QStringLiteral("right_value") : QStringLiteral("left_value");
        addText(QStringLiteral("peer_val"), err.value(peerKey));
        for (const QString& key : {QStringLiteral("distance"), QStringLiteral("tolerance")})
            if (err.value(key).isDouble()) attrs[key] = err.value(key).toDouble();
        return attrs;
    };

    QMap<QString, QgsVectorLayer*> sourceLayerCache;
    auto sourceLayerForPath = [&](const QString& shpPath) -> QgsVectorLayer* {
        const QString cleanPath = shpPath.trimmed();
        if (cleanPath.isEmpty() || !QFileInfo::exists(cleanPath)) return nullptr;
        if (sourceLayerCache.contains(cleanPath)) return sourceLayerCache.value(cleanPath);
        QgsVectorLayer* layer = new QgsVectorLayer(cleanPath, QFileInfo(cleanPath).completeBaseName(), QStringLiteral("ogr"));
        if (!layer->isValid()) {
            delete layer;
            sourceLayerCache.insert(cleanPath, nullptr);
            return nullptr;
        }
        sourceLayerCache.insert(cleanPath, layer);
        return layer;
    };

    auto appendOriginalFeatureOutput = [&](QVector<OriginalErrorFeatureOutput>& pointOutputs,
                                           QVector<OriginalErrorFeatureOutput>& lineOutputs,
                                           QVector<OriginalErrorFeatureOutput>& polygonOutputs,
                                           const QJsonObject& err,
                                           const QString& shpPath,
                                           const QString& fidText,
                                           const QString& role,
                                           const QString& side,
                                           const QString& sheet,
                                           const QString& standardLayer,
                                           const QString& shpName) {
        bool ok = false;
        const QgsFeatureId fid = static_cast<QgsFeatureId>(fidText.trimmed().toLongLong(&ok));
        if (!ok) return;
        QgsVectorLayer* layer = sourceLayerForPath(shpPath);
        if (!layer) return;
        QgsFeature sourceFeature = layer->getFeature(fid);
        if (!sourceFeature.isValid() || !sourceFeature.hasGeometry()) return;
        QgsGeometry geometry = sourceFeature.geometry();
        const QString geometryKey = side == QStringLiteral("左") ? QStringLiteral("left_check_wkt") : QStringLiteral("right_check_wkt");
        if (!err.value(geometryKey).toString().isEmpty()) geometry = QgsGeometry::fromWkt(err.value(geometryKey).toString());
        if (geometry.isNull() || geometry.isEmpty()) return;
        const QgsWkbTypes::GeometryType geometryType = QgsWkbTypes::geometryType(geometry.wkbType());
        OriginalErrorFeatureOutput output;
        output.err = err;
        output.geometry = geometry;
        output.role = role;
        output.side = side;
        output.sheet = sheet;
        output.layer = standardLayer;
        output.shp = shpName.isEmpty() ? QFileInfo(shpPath).fileName() : shpName;
        output.fid = fidText;
        if (geometryType == QgsWkbTypes::PointGeometry) pointOutputs.push_back(output);
        else if (geometryType == QgsWkbTypes::LineGeometry) lineOutputs.push_back(output);
        else if (geometryType == QgsWkbTypes::PolygonGeometry) polygonOutputs.push_back(output);
    };

    auto collectOriginalErrorFeatureOutputs = [&](const QJsonObject& err,
                                                  QVector<OriginalErrorFeatureOutput>& pointOutputs,
                                                  QVector<OriginalErrorFeatureOutput>& lineOutputs,
                                                  QVector<OriginalErrorFeatureOutput>& polygonOutputs,
                                                  const QString& mapName) {
        const QString leftPath = err.value(QStringLiteral("left_shp_path")).toString();
        const QString rightPath = err.value(QStringLiteral("right_shp_path")).toString();
        const QString leftFid = err.value(QStringLiteral("left_fid")).toString();
        const QString rightFid = err.value(QStringLiteral("right_fid")).toString();
        const QString leftSheet = err.value(QStringLiteral("left_sheet")).toString();
        const QString rightSheet = err.value(QStringLiteral("right_sheet")).toString();
        const QString leftLayer = err.value(QStringLiteral("left_standard_layer")).toString(err.value(QStringLiteral("layer")).toString());
        const QString rightLayer = err.value(QStringLiteral("right_standard_layer")).toString(err.value(QStringLiteral("layer")).toString());
        const QString leftShp = err.value(QStringLiteral("left_shp")).toString();
        const QString rightShp = err.value(QStringLiteral("right_shp")).toString();
        if (leftSheet == mapName && !leftPath.isEmpty() && !leftFid.isEmpty()) {
            appendOriginalFeatureOutput(pointOutputs, lineOutputs, polygonOutputs, err, leftPath, leftFid,
                rightPath.isEmpty() ? QStringLiteral("主错误要素") : QStringLiteral("左侧错误要素"),
                QStringLiteral("左"), leftSheet, leftLayer, leftShp);
        }
        if (rightSheet == mapName && !rightPath.isEmpty() && !rightFid.isEmpty()) {
            appendOriginalFeatureOutput(pointOutputs, lineOutputs, polygonOutputs, err, rightPath, rightFid,
                leftPath.isEmpty() ? QStringLiteral("主错误要素") : QStringLiteral("右侧错误要素"),
                QStringLiteral("右"), rightSheet, rightLayer, rightShp);
        }

        // Candidate evidence stays in JSON; only actual error objects enter SHP.
    };

    auto writeOriginalErrorFeatureShapefile = [&](const QVector<OriginalErrorFeatureOutput>& outputs,
                                                  const QString& shpPath,
                                                  QgsWkbTypes::Type wkbType,
                                                  const QString& label) -> bool {
        if (outputs.isEmpty()) return true;
        QgsFields featureFields;
        QStringList fieldNames{QStringLiteral("err_id"), QStringLiteral("err_type"), QStringLiteral("sub_rule"),
            QStringLiteral("src_sheet"), QStringLiteral("src_layer"), QStringLiteral("src_fid"),
            QStringLiteral("src_side"), QStringLiteral("message")};
        for (const OriginalErrorFeatureOutput& output : outputs) {
            const QVariantMap attrs = originalFeatureAttributes(output);
            for (auto it = attrs.constBegin(); it != attrs.constEnd(); ++it)
                if (!fieldNames.contains(it.key())) fieldNames << it.key();
        }
        for (const QString& name : fieldNames) {
            const bool numeric = name == QStringLiteral("distance") || name == QStringLiteral("tolerance");
            featureFields.append(QgsField(name, numeric ? QVariant::Double : QVariant::String,
                QString(), numeric ? 0 : 254));
        }
        QgsVectorFileWriter writer(shpPath, QStringLiteral("UTF-8"), featureFields, wkbType, crs, QStringLiteral("ESRI Shapefile"));
        if (writer.hasError() != QgsVectorFileWriter::NoError) {
            QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("%1 SHP 创建失败：%2").arg(label, shpPath));
            return false;
        }
        for (const OriginalErrorFeatureOutput& output : outputs) {
            QgsFeature feature(featureFields);
            feature.setGeometry(output.geometry);
            const QVariantMap attrs = originalFeatureAttributes(output);
            for (auto it = attrs.constBegin(); it != attrs.constEnd(); ++it) feature.setAttribute(it.key(), it.value());
            if (!writer.addFeature(feature)) return false;
        }
        return true;
    };

    auto appendFallbackPointOutput = [&](QVector<OriginalErrorFeatureOutput>& pointOutputs,
                                         const QJsonObject& err,
                                         const QgsPointXY& point,
                                         const QString& pointSource) {
        OriginalErrorFeatureOutput output;
        output.err = err;
        output.geometry = QgsGeometry::fromPointXY(point);
        output.role = QStringLiteral("定位点兜底");
        output.side = pointSource;
        output.sheet = err.value(QStringLiteral("right_sheet")).toString(err.value(QStringLiteral("left_sheet")).toString());
        output.layer = err.value(QStringLiteral("layer")).toString();
        output.shp = QString();
        output.fid = QString();
        pointOutputs.push_back(output);
    };

    auto seriesReportInfoText = [](const QJsonObject& errorsJson) {
        if (errorsJson.isEmpty()) {
            return QStringLiteral("未发现图幅接边与系列化表达检查错误。");
        }

        QString example;
        for (auto it = errorsJson.constBegin(); it != errorsJson.constEnd(); ++it) {
            if (!it.value().isObject()) continue;
            example = it.value().toObject().value(QStringLiteral("description")).toString().trimmed();
            if (!example.isEmpty()) break;
        }
        if (example.isEmpty()) {
            example = QStringLiteral("存在错误但未给出详情");
        }

        return QStringLiteral("发现%1个图幅接边与系列化表达检查错误，例如：%2")
            .arg(errorsJson.size())
            .arg(example);
    };

    auto combineSeriesFindingsForSummary = [](const QJsonObject& errors, const QJsonObject& reviews) {
        QJsonObject findings = errors;
        for (auto it = reviews.constBegin(); it != reviews.constEnd(); ++it) findings[it.key()] = it.value();
        return findings;
    };
    auto buildSeriesCheckItem = [&](const QJsonObject& itemInfo, bool hasErrors,
                                    const QJsonObject& errors = QJsonObject(),
                                    const QJsonObject& reviewErrors = QJsonObject()) {
        QJsonObject checkItem;
        checkItem[QStringLiteral("id")] = QStringLiteral("501");
        checkItem[QStringLiteral("name")] = QStringLiteral("图幅接边与系列化表达");
        checkItem[QStringLiteral("status")] = itemInfo.value(QStringLiteral("execution_status")).toString(QStringLiteral("质检成功"));
        checkItem[QStringLiteral("err_exist")] = hasErrors;
        checkItem[QStringLiteral("info")] = itemInfo;
        checkItem[QStringLiteral("errors")] = errors;
        checkItem[QStringLiteral("review_errors")] = reviewErrors;
        return checkItem;
    };

    auto mergeIntoTotalResultJson = [&](const QString& jsonPath, const QVector<QJsonObject>& records,
                                        const QVector<QJsonObject>& reviewFindings,
                                        const QString& targetMapName, const QSet<QString>& targetLayers) -> bool {
        QJsonObject root;
        QFile file(jsonPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            if (doc.isObject()) root = doc.object();
            file.close();
        }

        QMap<QString, QJsonObject> layerErrors;
        QMap<QString, QJsonObject> layerReviewErrors;
        for (const QJsonObject& err : records) {
            QString layer = err.value(QStringLiteral("standard_layer")).toString();
            if (layer.isEmpty()) layer = err.value(QStringLiteral("layer")).toString();
            QString errId = err.value(QStringLiteral("err_id")).toString();
            
            QString leftSheet = err.value(QStringLiteral("left_sheet")).toString();
            QString rightSheet = err.value(QStringLiteral("right_sheet")).toString();
            if (leftSheet == targetMapName || rightSheet == targetMapName) {
                QJsonObject currentLayerErrors = layerErrors.value(layer);
                currentLayerErrors[errId] = err;
                layerErrors[layer] = currentLayerErrors;
            }
        }
        for (const QJsonObject& err : reviewFindings) {
            QString layer = err.value(QStringLiteral("standard_layer")).toString();
            if (layer.isEmpty()) layer = err.value(QStringLiteral("layer")).toString();
            const QString errId = err.value(QStringLiteral("err_id")).toString();
            if (err.value(QStringLiteral("left_sheet")).toString() != targetMapName
                && err.value(QStringLiteral("right_sheet")).toString() != targetMapName) continue;
            QJsonObject currentLayerErrors = layerReviewErrors.value(layer);
            currentLayerErrors[errId] = err;
            layerReviewErrors[layer] = currentLayerErrors;
        }

        QString geopdfKey;
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            if (it.key().startsWith(QStringLiteral("Geopdf")) && it.value().isObject()) {
                if (it.value().toObject().value(QStringLiteral("name")).toString() == targetMapName) {
                    geopdfKey = it.key();
                    break;
                }
            }
        }
        if (geopdfKey.isEmpty()) {
            geopdfKey = QStringLiteral("Geopdf");
            if (root.contains(geopdfKey)) {
                int idx = 2;
                while (root.contains(QStringLiteral("Geopdf%1").arg(idx))) idx++;
                geopdfKey = QStringLiteral("Geopdf%1").arg(idx);
            }
            QJsonObject newGeopdf;
            newGeopdf[QStringLiteral("name")] = targetMapName;
            QJsonObject info;
            info[QStringLiteral("tasks_num")] = 0;
            info[QStringLiteral("completed_num")] = 0;
            info[QStringLiteral("err_num")] = 0;
            info[QStringLiteral("uncompleted_num")] = 0;
            newGeopdf[QStringLiteral("info")] = info;
            newGeopdf[QStringLiteral("tasks")] = QJsonObject();
            root[geopdfKey] = newGeopdf;
        }

        QJsonObject geopdfNode = root.value(geopdfKey).toObject();
        QJsonObject tasks = geopdfNode.value(QStringLiteral("tasks")).toObject();
        QJsonObject info = geopdfNode.value(QStringLiteral("info")).toObject();

        for (const QString& layerName : targetLayers) {
            QString taskKey;
            for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
                if (it.key().startsWith(QStringLiteral("task")) && it.value().isObject()) {
                    if (it.value().toObject().value(QStringLiteral("layer_name")).toString() == layerName) {
                        taskKey = it.key();
                        break;
                    }
                }
            }
            if (taskKey.isEmpty()) {
                int idx = 1;
                while (tasks.contains(QStringLiteral("task%1").arg(idx))) idx++;
                taskKey = QStringLiteral("task%1").arg(idx);
                QJsonObject newTask;
                newTask[QStringLiteral("layer_name")] = layerName;
                tasks[taskKey] = newTask;
                info[QStringLiteral("tasks_num")] = info.value(QStringLiteral("tasks_num")).toInt() + 1;
            }

            QJsonObject taskNode = tasks.value(taskKey).toObject();
            
            QString checkItemKey;
            for (auto it = taskNode.constBegin(); it != taskNode.constEnd(); ++it) {
                if (it.key().startsWith(QStringLiteral("check_item")) && it.value().isObject()) {
                    QJsonObject item = it.value().toObject();
                    if (item.value(QStringLiteral("id")).toString() == QStringLiteral("501") || 
                        item.value(QStringLiteral("name")).toString() == QStringLiteral("图幅接边与系列化表达")) {
                        checkItemKey = it.key();
                        break;
                    }
                }
            }
            if (checkItemKey.isEmpty()) {
                int idx = 1;
                while (taskNode.contains(QStringLiteral("check_item%1").arg(idx))) idx++;
                checkItemKey = QStringLiteral("check_item%1").arg(idx);
            }

            QJsonObject currentLayerErrors = layerErrors.value(layerName);
            QJsonObject currentLayerReviewErrors = layerReviewErrors.value(layerName);
            bool hasErrors = !currentLayerErrors.isEmpty();
            
            QJsonObject itemInfo;
            itemInfo[QStringLiteral("error_count")] = currentLayerErrors.size();
            itemInfo[QStringLiteral("confirmed_error_count")] = currentLayerErrors.size();
            int needsReviewCount = 0;
            int notEvaluableCount = 0;
            for (const QJsonValue& value : currentLayerReviewErrors) {
                if (value.toObject().value(QStringLiteral("decision_status")).toString() == QStringLiteral("not_evaluable")) ++notEvaluableCount;
                else ++needsReviewCount;
            }
            itemInfo[QStringLiteral("needs_review_count")] = needsReviewCount;
            itemInfo[QStringLiteral("not_evaluable_count")] = notEvaluableCount;
            QJsonObject execution = executionSummaryForMap(targetMapName,layerName);
            execution.remove(QStringLiteral("execution_records"));
            const QString reportInfoText = execution.value(QStringLiteral("status")).toString() == QStringLiteral("质检成功")
                ? seriesReportInfoText(combineSeriesFindingsForSummary(currentLayerErrors, currentLayerReviewErrors))
                : execution.value(QStringLiteral("description")).toString();
            itemInfo[QStringLiteral("execution_summary")] = execution;
            itemInfo[QStringLiteral("description")] = reportInfoText;
            itemInfo[QStringLiteral("summary")] = reportInfoText;
            
            QJsonObject checkItem;
            checkItem[QStringLiteral("id")] = QStringLiteral("501");
            checkItem[QStringLiteral("name")] = QStringLiteral("图幅接边与系列化表达");
            checkItem[QStringLiteral("status")] = execution.value(QStringLiteral("status"));
            checkItem[QStringLiteral("err_exist")] = hasErrors;
            checkItem[QStringLiteral("info")] = itemInfo;
            checkItem[QStringLiteral("errors")] = currentLayerErrors;
            checkItem[QStringLiteral("review_errors")] = currentLayerReviewErrors;

            taskNode[checkItemKey] = checkItem;
            tasks[taskKey] = taskNode;
        }

            int errNum = 0;
            int completedNum = 0;
            for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
                if (it.key().startsWith(QStringLiteral("task")) && it.value().isObject()) {
                    QJsonObject taskNode = it.value().toObject();
                    bool taskHasError = false;
                    bool taskCompleted = false;
                    bool taskIncomplete = false;
                    for (auto cit = taskNode.constBegin(); cit != taskNode.constEnd(); ++cit) {
                        if (cit.key().startsWith(QStringLiteral("check_item")) && cit.value().isObject()) {
                            taskCompleted = true;
                            taskIncomplete = taskIncomplete || cit.value().toObject().value(QStringLiteral("status")).toString() != QStringLiteral("质检成功");
                            if (cit.value().toObject().value(QStringLiteral("err_exist")).toBool()) {
                                taskHasError = true;
                            }
                        }
                    }
                    if (taskHasError) errNum++;
                    if (taskCompleted && !taskIncomplete) completedNum++;
                }
            }
            info[QStringLiteral("err_num")] = errNum;
            info[QStringLiteral("completed_num")] = completedNum;
            info[QStringLiteral("uncompleted_num")] = info.value(QStringLiteral("tasks_num")).toInt() - completedNum;
            
            geopdfNode[QStringLiteral("tasks")] = tasks;
            geopdfNode[QStringLiteral("info")] = info;
            normalizeSeriesReportGeopdfNode(geopdfNode);
            root[geopdfKey] = geopdfNode;

        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        file.close();
        return true;
    };

    auto findLatestResultJson = [](const QString& dirPath, const QString& mapName) -> QString {
        QDir dir(dirPath);
        QFileInfoList fileList = dir.entryInfoList(QStringList() << QStringLiteral("*_result_*.json"), QDir::Files, QDir::Time);
        if (!fileList.isEmpty()) {
            return fileList.first().absoluteFilePath();
        }
        QString fallbackName = mapName.isEmpty() ? QStringLiteral("result.json") : QStringLiteral("%1_result_%2.json").arg(mapName, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")));
        return dir.filePath(fallbackName);
    };

    QFile::remove(QDir(resultOutputRootDir()).filePath(QStringLiteral("result.json")));

    if (finalRecords.isEmpty()) {
        appendSeriesDebugLog(QStringLiteral("[501][output] writeCurrentOutputs noConfirmedErrors"));
    }

    for (auto mapIt = m_checkedLayersByMap.constBegin(); mapIt != m_checkedLayersByMap.constEnd(); ++mapIt) {
        const QString& mapName = mapIt.key();
        const QSet<QString>& checkedLayers = mapIt.value();

        QString mapRootDir = QDir(resultOutputRootDir()).filePath(mapName);
        SeriesOutputPaths mapPaths = buildSeriesOutputPaths(mapRootDir);
        if (!ensureSeriesOutputDirs(mapPaths)) {
            QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("无法创建输出目录：%1").arg(mapPaths.rootDir));
            continue;
        }

        QFile::remove(mapPaths.jsonPath);
        QFile::remove(mapPaths.logPath);
        removeSeriesShapefile(mapPaths.pointShpPath);
        removeSeriesShapefile(mapPaths.lineShpPath);
        removeSeriesShapefile(mapPaths.polygonShpPath);
        removeSeriesShapefile(mapPaths.linkShpPath);

        QVector<QJsonObject> mapRecords;
        for (const QJsonObject& err : finalRecords) {
            if (err.value(QStringLiteral("left_sheet")).toString() == mapName ||
                err.value(QStringLiteral("right_sheet")).toString() == mapName) {
                mapRecords.push_back(err);
            }
        }
        QVector<QJsonObject> mapReviewRecords;
        for (const QJsonObject& finding : reviewRecords) {
            if (finding.value(QStringLiteral("left_sheet")).toString() == mapName
                || finding.value(QStringLiteral("right_sheet")).toString() == mapName) {
                mapReviewRecords.push_back(finding);
            }
        }

        QJsonObject errorsJson;
        QJsonObject reviewErrorsJson;
        QVector<QJsonObject> linkErrors;
        QVector<OriginalErrorFeatureOutput> origFeaturePointErrors;
        QVector<OriginalErrorFeatureOutput> origFeatureLineErrors;
        QVector<OriginalErrorFeatureOutput> origFeaturePolygonErrors;

        for (const QJsonObject& err : mapRecords) {
            QString errId = err.value(QStringLiteral("err_id")).toString();
            errorsJson[errId] = err;

            QgsPointXY lineLeft, lineRight;
            if (resolveSeriesErrorLine(err, lineLeft, lineRight)) {
                linkErrors.push_back(err);
            }

            QgsPointXY point;
            QString pointSource = err.value(QStringLiteral("point_source")).toString();
            bool hasPoint = err.contains(QStringLiteral("point_x"));
            if (hasPoint) {
                point = QgsPointXY(err.value(QStringLiteral("point_x")).toDouble(), err.value(QStringLiteral("point_y")).toDouble());
            }

            const int originalOutputCountBefore = origFeaturePointErrors.size() + origFeatureLineErrors.size() + origFeaturePolygonErrors.size();
            collectOriginalErrorFeatureOutputs(err, origFeaturePointErrors, origFeatureLineErrors, origFeaturePolygonErrors, mapName);
            const int originalOutputCountAfter = origFeaturePointErrors.size() + origFeatureLineErrors.size() + origFeaturePolygonErrors.size();
            const bool ownsSource = (err.value(QStringLiteral("left_sheet")).toString() == mapName && !err.value(QStringLiteral("left_shp_path")).toString().isEmpty())
                || (err.value(QStringLiteral("right_sheet")).toString() == mapName && !err.value(QStringLiteral("right_shp_path")).toString().isEmpty());
            const bool hasAnySource = !err.value(QStringLiteral("left_shp_path")).toString().isEmpty()
                || !err.value(QStringLiteral("right_shp_path")).toString().isEmpty();
            if (originalOutputCountAfter == originalOutputCountBefore && hasPoint && (ownsSource || !hasAnySource)) {
                appendFallbackPointOutput(origFeaturePointErrors, err, point, pointSource);
            }
        }

        for (const QJsonObject& finding : mapReviewRecords)
            reviewErrorsJson[finding.value(QStringLiteral("err_id")).toString()] = finding;

        if (!linkErrors.isEmpty()) {
            QgsFeatureList relationFeatures;
            for (const QJsonObject& err : linkErrors) {
                QgsPointXY lineLeft, lineRight;
                if (!resolveSeriesErrorLine(err, lineLeft, lineRight)) continue;
                QgsFeature feature(fields);
                QgsPolylineXY line;
                line << lineLeft << lineRight;
                feature.setGeometry(QgsGeometry::fromPolylineXY(line));
                setSeriesFeatureAttributes(feature, err, err.value(QStringLiteral("line_source")).toString());
                relationFeatures << feature;
            }
            QgsFields linkFields;
            QList<int> retainedColumns;
            for (const QString& name : {QStringLiteral("err_id"),QStringLiteral("err_type"),QStringLiteral("sub_rule"),
                QStringLiteral("l_sheet"),QStringLiteral("r_sheet"),QStringLiteral("l_shp"),QStringLiteral("r_shp"),
                QStringLiteral("l_fid"),QStringLiteral("r_fid"),QStringLiteral("field"),QStringLiteral("l_val"),
                QStringLiteral("r_val"),QStringLiteral("distance"),QStringLiteral("tolerance"),QStringLiteral("message")}) {
                const int column = fields.indexOf(name);
                bool populated = false;
                for (const QgsFeature& feature : relationFeatures) {
                    const QVariant value = feature.attribute(column);
                    if (value.isValid() && !value.isNull() && !value.toString().isEmpty()) { populated = true; break; }
                }
                if (populated) { linkFields.append(fields.at(column)); retainedColumns << column; }
            }
            QgsVectorFileWriter lineWriter(mapPaths.linkShpPath, QStringLiteral("UTF-8"), linkFields, QgsWkbTypes::LineString, crs, QStringLiteral("ESRI Shapefile"));
            if (lineWriter.hasError() != QgsVectorFileWriter::NoError) { qDeleteAll(sourceLayerCache); return false; }
            for (const QgsFeature& source : relationFeatures) {
                QgsFeature feature(linkFields); feature.setGeometry(source.geometry());
                for (int i = 0; i < retainedColumns.size(); ++i) feature.setAttribute(i, source.attribute(retainedColumns[i]));
                if (!lineWriter.addFeature(feature)) { qDeleteAll(sourceLayerCache); return false; }
            }
        }

        if (!writeOriginalErrorFeatureShapefile(origFeaturePointErrors, mapPaths.pointShpPath, QgsWkbTypes::Point, QStringLiteral("错误点要素"))
            || !writeOriginalErrorFeatureShapefile(origFeatureLineErrors, mapPaths.lineShpPath, QgsWkbTypes::LineString, QStringLiteral("错误线要素"))
            || !writeOriginalErrorFeatureShapefile(origFeaturePolygonErrors, mapPaths.polygonShpPath, QgsWkbTypes::Polygon, QStringLiteral("错误面要素"))) {
            qDeleteAll(sourceLayerCache);
            return false;
        }

        QJsonObject info;
        const QJsonObject execution = executionSummaryForMap(mapName);
        const QString mapReportInfoText = execution.value(QStringLiteral("status")).toString() == QStringLiteral("质检成功")
            ? seriesReportInfoText(combineSeriesFindingsForSummary(errorsJson, reviewErrorsJson))
            : execution.value(QStringLiteral("description")).toString();
        info[QStringLiteral("description")] = mapReportInfoText;
        info[QStringLiteral("summary")] = mapReportInfoText;
        info[QStringLiteral("auto_confirmed_count")] = errorsJson.size();
        info[QStringLiteral("final_error_count")] = errorsJson.size();
        info[QStringLiteral("confirmed_error_count")] = errorsJson.size();
        int needsReviewCount = 0;
        int notEvaluableCount = 0;
        for (const QJsonValue& value : reviewErrorsJson) {
            if (value.toObject().value(QStringLiteral("decision_status")).toString() == QStringLiteral("not_evaluable")) ++notEvaluableCount;
            else ++needsReviewCount;
        }
        info[QStringLiteral("needs_review_count")] = needsReviewCount;
        info[QStringLiteral("not_evaluable_count")] = notEvaluableCount;
        info[QStringLiteral("error_point_count")] = origFeaturePointErrors.size();
        info[QStringLiteral("error_line_count")] = origFeatureLineErrors.size();
        info[QStringLiteral("error_polygon_count")] = origFeaturePolygonErrors.size();
        info[QStringLiteral("error_link_count")] = linkErrors.size();
        info[QStringLiteral("pair_count")] = execution.value(QStringLiteral("pair_count"));
        info[QStringLiteral("execution_status")] = execution.value(QStringLiteral("status"));
        info[QStringLiteral("execution_records")] = execution.value(QStringLiteral("execution_records"));
        info[QStringLiteral("executed_pair_count")] = execution.value(QStringLiteral("executed_pair_count"));
        info[QStringLiteral("failed_pair_count")] = execution.value(QStringLiteral("failed_pair_count"));
        info[QStringLiteral("skipped_pair_count")] = execution.value(QStringLiteral("skipped_pair_count"));
        info[QStringLiteral("error_pair_count")] = execution.value(QStringLiteral("error_pair_count"));
        info[QStringLiteral("partial_pair_count")] = execution.value(QStringLiteral("partial_pair_count"));
        info[QStringLiteral("result_shp")] = !origFeaturePointErrors.isEmpty() ? mapPaths.pointShpPath : (!origFeatureLineErrors.isEmpty() ? mapPaths.lineShpPath : (!origFeaturePolygonErrors.isEmpty() ? mapPaths.polygonShpPath : QString()));
        info[QStringLiteral("error_point_shp")] = !origFeaturePointErrors.isEmpty() ? mapPaths.pointShpPath : QString();
        info[QStringLiteral("error_line_shp")] = !origFeatureLineErrors.isEmpty() ? mapPaths.lineShpPath : QString();
        info[QStringLiteral("error_polygon_shp")] = !origFeaturePolygonErrors.isEmpty() ? mapPaths.polygonShpPath : QString();
        info[QStringLiteral("error_link_shp")] = !linkErrors.isEmpty() ? mapPaths.linkShpPath : QString();
        info[QStringLiteral("log_path")] = mapPaths.logPath;

        QJsonObject checkItem = buildSeriesCheckItem(info, !errorsJson.isEmpty(), errorsJson, reviewErrorsJson);

        QJsonObject singleJsonRoot;
        singleJsonRoot[QStringLiteral("check_item1")] = checkItem;

        writeSeriesJsonFile(mapPaths.jsonPath, QJsonDocument(singleJsonRoot));

        const QString mapResultJsonPath = findLatestResultJson(mapRootDir, mapName);
        mergeIntoTotalResultJson(mapResultJsonPath, mapRecords, mapReviewRecords, mapName, checkedLayers);

        QFile logFile(mapPaths.logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream logOut(&logFile);
            logOut.setCodec("UTF-8");
            const QStringList lines = logLines.isEmpty() ? m_outputEdit->toPlainText().split(QStringLiteral("\n")) : logLines;
            for (const QString& line : lines) logOut << line << "\n";
            logOut << QStringLiteral("\n输出文件：\n");
            logOut << QStringLiteral("总结果JSON：%1\n").arg(mapResultJsonPath);
            logOut << QStringLiteral("单项JSON：%1\n").arg(mapPaths.jsonPath);
            if (!origFeaturePointErrors.isEmpty()) logOut << QStringLiteral("错误点要素SHP：%1\n").arg(mapPaths.pointShpPath);
            if (!origFeatureLineErrors.isEmpty()) logOut << QStringLiteral("错误线要素SHP：%1\n").arg(mapPaths.lineShpPath);
            if (!origFeaturePolygonErrors.isEmpty()) logOut << QStringLiteral("错误面要素SHP：%1\n").arg(mapPaths.polygonShpPath);
            if (!linkErrors.isEmpty()) logOut << QStringLiteral("错误关系线SHP：%1\n").arg(mapPaths.linkShpPath);
            logFile.close();
        }
    }

    qDeleteAll(sourceLayerCache);

    return true;
}

void SeriesMapEdgeMatchTaskDialog::startCheck()
{
    if (m_runningCheck || m_loadingSheets) return;
    const QScopedValueRollback<bool> running(m_runningCheck,true);
    const bool wasEnabled=isEnabled();
    setEnabled(false);
    const auto restoreUi=qScopeGuard([this,wasEnabled]() { setEnabled(wasEnabled); });
    const QString xmlPath = m_xmlEdit->text().trimmed();

    if (xmlPath.isEmpty()) { QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择 XML 质检规则文件。")); return; }

    if (!m_configTree || m_configTree->topLevelItemCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先添加城市分组并完成配置。"));
        return;
    }

    const QStringList configuredPairsBeforeRelationCheck = configuredMainAssocPairs();
    QStringList removedMainMapNames;
    if (!loadSheets(true, &removedMainMapNames)) return;
    const QStringList configuredPairsAfterRelationCheck = configuredMainAssocPairs();
    QStringList skippedNoRelationPairs;
    for (const QString& configuredPair : configuredPairsBeforeRelationCheck) {
        if (!configuredPairsAfterRelationCheck.contains(configuredPair)) {
            skippedNoRelationPairs << configuredPair;
        }
    }
    skippedNoRelationPairs.removeDuplicates();
    const QString skippedNoRelationDetail = skippedNoRelationPairs.isEmpty()
        ? QString()
        : QStringLiteral("（%1）").arg(skippedNoRelationPairs.join(QStringLiteral("；")));

    // 校验表格配置是否完整
    int validRowCount = 0;
    for (int i = 0; i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* rowItem = m_configTree->topLevelItem(i);
        QString status = rowItem ? rowItem->text(SeriesColStatus) : QString();

        if (status == QStringLiteral("空白") || status == QStringLiteral("无有效关联地图")) {
            continue;
        } else if (status.startsWith(QStringLiteral("待填写"))) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("第 %1 行%2，请填写后再开始检查。").arg(i + 1).arg(status));
            return;
        } else if (status != QStringLiteral("已完成")) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("第 %1 行配置不完整，请补充 GeoPDF 和比例尺，或删除该空白行。").arg(i + 1));
            return;
        } else {
            validRowCount++;
        }
    }

    if (validRowCount == 0) {
        if (!removedMainMapNames.isEmpty()) {
            QString message = QStringLiteral("真实图廓验证未发现可检查的接边关系，已从地图配置区移除 %1 条无有效关联的配置：%2。")
                .arg(removedMainMapNames.size())
                .arg(removedMainMapNames.join(QStringLiteral("、")));
            if (!skippedNoRelationPairs.isEmpty()) {
                message += QStringLiteral("\n跳过候选关系 %1 对：%2。")
                    .arg(skippedNoRelationPairs.size())
                    .arg(skippedNoRelationDetail);
            }
            QMessageBox::information(this, QStringLiteral("检查结果"), message);
            return;
        }
        if (!skippedNoRelationPairs.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("检查完成"),
                QStringLiteral("已检查%1组，已跳过无有效关系%2组%3。")
                .arg(0)
                .arg(skippedNoRelationPairs.size())
                .arg(skippedNoRelationDetail));
            return;
        }
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请至少添加并完成一组城市配置。"));
        return;
    }

    rebuildPairList();
    updatePairCheckStates();

    QVector<SeriesMapEdgeMatchTaskDialog::AdjacentPair> selectedPairs;
    QVector<SeriesMapEdgeMatchTaskDialog::CoverageGroup> selectedCoverageGroups;
    for (const auto& task : m_taskChoices) {
        if (!task.ready || !m_pairSelection.value(task.key,true)) continue;
        if (task.crossScale) selectedCoverageGroups.push_back(m_coverageGroups[task.index]);
        else selectedPairs.push_back(m_adjacentPairs[task.index]);
    }
    if (selectedPairs.isEmpty() && selectedCoverageGroups.isEmpty()) { QMessageBox::information(this, QStringLiteral("检查完成"), QStringLiteral("已检查%1组，已跳过无有效关系%2组%3。").arg(0).arg(skippedNoRelationPairs.size()).arg(skippedNoRelationDetail)); return; }

    int defaultScale = 0; QFile xmlFile(xmlPath); QDomDocument doc;
    if (xmlFile.open(QIODevice::ReadOnly | QIODevice::Text) && doc.setContent(&xmlFile)) {
        QDomElement scaleElem = doc.documentElement().firstChildElement(QStringLiteral("scale"));
        QDomElement valueElem = scaleElem.firstChildElement(QStringLiteral("value"));
        bool ok = false;
        int parsedScale = valueElem.text().trimmed().toInt(&ok);
        if (ok && parsedScale > 0) {
            defaultScale = parsedScale;
        }
    }
    xmlFile.close();

    m_lastErrorRecords.clear();
    m_originalErrorRecords.clear();
    m_pairRunContext.clear();
    m_originalStats = QJsonObject();
    m_originalStats.insert(QStringLiteral("check_completed"), false);
    m_exportErrorButton->setEnabled(false);

    m_outputEdit->clear();
    m_outputEdit->append(QStringLiteral("开始预处理 GeoPDF..."));

    if (m_progressBar) {
        m_progressBar->setValue(0);
        m_progressBar->setFormat(QStringLiteral("准备预处理 GeoPDF..."));
        QCoreApplication::processEvents();
    }

    QMap<QString, PreprocessResult> preprocessResults;

    // 1. 从勾选的任务中提取需要预处理的图幅名称
    QSet<QString> neededSheetNames;
    for (const auto& pair : selectedPairs) {
        neededSheetNames.insert(pair.leftName);
        neededSheetNames.insert(pair.rightName);
    }
    for (const auto& group : selectedCoverageGroups) {
        neededSheetNames.insert(group.smallScaleName);
        for (const QString& largeName : group.largeScaleNames) {
            neededSheetNames.insert(largeName);
        }
    }

    // 2. 收集所有需要预处理的 GeoPDF (仅限 neededSheetNames 中包含的，使用模糊匹配)
    QStringList allPdfPaths;
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* statusItem = m_configTree->topLevelItem(i);
        if (!statusItem || statusItem->text(SeriesColStatus) != QStringLiteral("已完成")) continue;

        auto* mainEdit = configLineEdit(i, SeriesColMainMap);
        auto* assocEdit = configLineEdit(i, SeriesColAssocMap);
        
        QStringList pathsToCheck;
        if (mainEdit) pathsToCheck << mainEdit->text().trimmed();
        if (assocEdit) pathsToCheck << splitSeriesTableList(assocEdit->text().trimmed());

        for (const QString& path : pathsToCheck) {
            QStringList pdfs = collectGeoPdfFiles(path);
            for (const QString& p : pdfs) {
                QString baseName = QFileInfo(p).completeBaseName();
                bool isNeeded = false;
                
                // 使用与 resolveShpDirForSheetName 一致的模糊匹配逻辑
                for (const QString& neededName : neededSheetNames) {
                    if (baseName.compare(neededName, Qt::CaseInsensitive) == 0 ||
                        baseName.contains(neededName, Qt::CaseInsensitive) ||
                        neededName.contains(baseName, Qt::CaseInsensitive)) {
                        isNeeded = true;
                        break;
                    }
                }
                
                if (isNeeded && !allPdfPaths.contains(p)) {
                    allPdfPaths << p;
                }
            }
        }
    }

    QMap<QString, QString> pdfBaseNameToShpDir;
    QJsonArray sourceAudits;
    int totalPdfs = allPdfPaths.size();
    int currentPdf = 0;
    
    for (const QString& pdfPath : allPdfPaths) {
        if (m_progressBar) {
            m_progressBar->setFormat(QStringLiteral("预处理 GeoPDF (%1/%2): %3").arg(currentPdf + 1).arg(totalPdfs).arg(QFileInfo(pdfPath).fileName()));
            QCoreApplication::processEvents();
        }
        
        QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
        PreprocessResult preprocessResult;
        if (!shpDir.isEmpty()) {
            preprocessResult.success = true;
            preprocessResult.shpDir = shpDir;
            preprocessResult.pdfPath = pdfPath;
            m_outputEdit->append(QStringLiteral("复用已有预处理结果：%1").arg(shpDir));
            QCoreApplication::processEvents();
        } else {
            preprocessResult = preprocessGeoPdfIfNeeded(pdfPath, outputRootDir());
        }
        
        preprocessResults[pdfPath] = preprocessResult;
        if (!preprocessResult.success) {
            const QString stageText = preprocessResult.errorStage.isEmpty()
                ? QStringLiteral("未知阶段")
                : preprocessResult.errorStage;
            const QString message = QStringLiteral("GeoPDF 预处理失败，已停止 501 检查。\n文件：%1\n阶段：%2\n原因：%3")
                .arg(pdfPath, stageText, preprocessResult.errorMessage);
            m_outputEdit->append(message);
            if (m_progressBar) m_progressBar->setFormat(QStringLiteral("预处理失败"));
            QMessageBox::critical(this, QStringLiteral("预处理失败"), message);
            return;
        }
        
        QString baseName = QFileInfo(pdfPath).completeBaseName();
        pdfBaseNameToShpDir[baseName] = preprocessResult.shpDir;
        m_outputEdit->append(QStringLiteral("核对原 GeoPDF 描画属性：%1").arg(baseName));
        sourceAudits.append(SeriesSourceAudit::run(getPreprocessPythonPath(),
            getPreprocessScriptPath(QStringLiteral("audit_series_pdf_source.py")),pdfPath,preprocessResult.shpDir,
            QDir(outputRootDir()).filePath(QStringLiteral("系列图检查/.series_cache/source_audits"))));
        
        currentPdf++;
        if (m_progressBar) {
            m_progressBar->setValue(currentPdf * 50 / totalPdfs); // 预处理占前 50% 进度
            QCoreApplication::processEvents();
        }
    }

    m_outputEdit->append(QStringLiteral("预处理完成，开始执行 501 检查..."));
    if (m_progressBar) {
        m_progressBar->setFormat(QStringLiteral("准备执行 501 检查..."));
        QCoreApplication::processEvents();
    }

    QStringList lines; lines << buildTaskSummary(selectedPairs, selectedCoverageGroups) << QString() << QStringLiteral("执行结果：");
    int errPairs = 0;
    QSet<QString> allTargetLayers;

    int totalChecks = selectedPairs.size() + selectedCoverageGroups.size();
    int currentCheck = 0;

    for (int i = 0; i < selectedPairs.size(); ++i) {
        const auto& pair = selectedPairs[i];
        const auto leftIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SeriesMapEdgeMatchTaskDialog::SheetInfo& info) { return info.featureId == pair.leftId; });
        const auto rightIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SeriesMapEdgeMatchTaskDialog::SheetInfo& info) { return info.featureId == pair.rightId; });
        if ((leftIt != m_sheetInfos.end() && leftIt->scale <= 0 && pair.leftScale <= 0) || (rightIt != m_sheetInfos.end() && rightIt->scale <= 0 && pair.rightScale <= 0)) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("存在比例尺未识别的图幅，请在主图比例尺或关联图比例尺列填写后重新加载图幅关系。"));
            return;
        }
    }
    for (int i = 0; i < selectedCoverageGroups.size(); ++i) {
        const auto& group = selectedCoverageGroups[i];
        if (group.smallScale <= 0 || group.largeScale <= 0) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("存在比例尺未识别的图幅，请在主图比例尺或关联图比例尺列填写后重新加载图幅关系。"));
            return;
        }
    }

    QMap<QString, QString> confirmedLayerMappings;
    if (!confirmSeriesLayerMappings(selectedPairs, selectedCoverageGroups, pdfBaseNameToShpDir, defaultScale, &confirmedLayerMappings)) {
        m_outputEdit->append(QStringLiteral("用户取消图层对应确认，已停止检查。"));
        if (m_progressBar) m_progressBar->setFormat(QStringLiteral("已取消"));
        return;
    }

    for (int i = 0; i < selectedPairs.size(); ++i) {
        const SeriesMapEdgeMatchTaskDialog::AdjacentPair& pair = selectedPairs[i];
        
        if (m_progressBar) {
            m_progressBar->setFormat(QStringLiteral("检查同比例尺图幅对 (%1/%2): %3 - %4").arg(currentCheck + 1).arg(totalChecks).arg(pair.leftName).arg(pair.rightName));
            QCoreApplication::processEvents();
        }
        
        const auto leftIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SeriesMapEdgeMatchTaskDialog::SheetInfo& info) { return info.featureId == pair.leftId; });
        const auto rightIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SeriesMapEdgeMatchTaskDialog::SheetInfo& info) { return info.featureId == pair.rightId; });
        if (leftIt == m_sheetInfos.end() || rightIt == m_sheetInfos.end()) continue;

        QString leftShpDir = resolveShpDirForSheetName(pair.leftName, pdfBaseNameToShpDir);
        QString rightShpDir = resolveShpDirForSheetName(pair.rightName, pdfBaseNameToShpDir);

        if (leftShpDir.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("图幅“%1”未找到对应 GeoPDF 预处理目录，请检查 GeoPDF 文件名是否与图幅索引名称一致。").arg(pair.leftName));
            return;
        }
        if (rightShpDir.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("图幅“%1”未找到对应 GeoPDF 预处理目录，请检查 GeoPDF 文件名是否与图幅索引名称一致。").arg(pair.rightName));
            return;
        }

        QStringList leftActualDirs; leftActualDirs << leftShpDir;
        QStringList rightActualDirs; rightActualDirs << rightShpDir;
        QStringList actualDirs; actualDirs << leftShpDir;
        if (!actualDirs.contains(rightShpDir)) actualDirs << rightShpDir;
        AlgorithmFilePaths filePaths;
        filePaths.shpPath = actualDirs.join(QStringLiteral(";"));
        filePaths.seriesLeftShpPath = leftActualDirs.join(QStringLiteral(";"));
        filePaths.seriesRightShpPath = rightActualDirs.join(QStringLiteral(";"));
        filePaths.xmlPaths << xmlPath;

        filePaths.checkMode = QStringLiteral("SameScale");
        filePaths.seriesPairName = QStringLiteral("%1 - %2").arg(pair.leftName, pair.rightName);
        filePaths.seriesLayerMappingJson = confirmedLayerMappings.value(filePaths.seriesPairName);
        {
            QJsonObject mapping=QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8()).object();
            mapping[QStringLiteral("source_audits")]=sourceAudits;
            filePaths.seriesLayerMappingJson=QString::fromUtf8(QJsonDocument(mapping).toJson(QJsonDocument::Compact));
        }
        filePaths.seriesLeftSheetName = pair.leftName;
        filePaths.seriesRightSheetName = pair.rightName;
        filePaths.seriesLeftScale = pair.leftScale > 0 ? pair.leftScale : (leftIt->scale > 0 ? leftIt->scale : defaultScale);
        filePaths.seriesRightScale = pair.rightScale > 0 ? pair.rightScale : (rightIt->scale > 0 ? rightIt->scale : defaultScale);
        QJsonObject scope; QString scopeError;
        const bool scopeOk = applyRealFrameScope(leftActualDirs,rightActualDirs,&filePaths,&scope,&scopeError);

        QJsonObject pairCtx;
        pairCtx[QStringLiteral("xml_path")] = xmlPath;
        pairCtx[QStringLiteral("actual_dir")] = filePaths.shpPath;
        pairCtx[QStringLiteral("scale")] = defaultScale;
        pairCtx[QStringLiteral("left_scale")] = filePaths.seriesLeftScale;
        pairCtx[QStringLiteral("right_scale")] = filePaths.seriesRightScale;
        pairCtx[QStringLiteral("left_name")] = pair.leftName;
        pairCtx[QStringLiteral("right_name")] = pair.rightName;
        pairCtx[QStringLiteral("left_wkt")] = filePaths.seriesLeftSheetWkt;
        pairCtx[QStringLiteral("right_wkt")] = filePaths.seriesRightSheetWkt;
        pairCtx[QStringLiteral("scope")] = scope;
        pairCtx[QStringLiteral("confirmed_layer_mapping")] = QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8()).object();
        m_pairRunContext[filePaths.seriesPairName] = pairCtx;

        CheckItemResult result;
        try {
            appendSeriesDebugLog(QStringLiteral("[501] wrapperCallBegin=%1").arg(filePaths.seriesPairName));
            if (scopeOk) SeriesMapTemplateCheckWrapper(QStringLiteral("图幅接边与系列化表达"), filePaths, defaultScale, QString(), result);
            else {
                result.status = QStringLiteral("质检失败"); result.err_exist = true;
                result.info = QJsonObject{{QStringLiteral("execution_state"),QStringLiteral("invalid_scope")},{QStringLiteral("description"),scopeError}};
            }
            appendSeriesDebugLog(QStringLiteral("[501] wrapperCallEnd=%1 status=%2").arg(filePaths.seriesPairName, result.status));
        }
        catch (const std::exception& ex) {
            appendSeriesDebugLog(QStringLiteral("[501] pairException=%1 msg=%2").arg(filePaths.seriesPairName, QString::fromUtf8(ex.what())));
            result.id = QStringLiteral("501");
            result.name = QStringLiteral("图幅接边与系列化表达");
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            QJsonObject err; err["sub_rule"] = QStringLiteral("运行异常"); err["description"] = QStringLiteral("执行异常：%1").arg(QString::fromUtf8(ex.what()));
            result.errors[QStringLiteral("exception")] = err;
        }
        catch (...) {
            appendSeriesDebugLog(QStringLiteral("[501] pairUnknownException=%1").arg(filePaths.seriesPairName));
            result.id = QStringLiteral("501");
            result.name = QStringLiteral("图幅接边与系列化表达");
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            QJsonObject err; err["sub_rule"] = QStringLiteral("运行异常"); err["description"] = QStringLiteral("执行过程中发生未知异常");
            result.errors[QStringLiteral("exception")] = err;
        }
        recordPairExecution(filePaths.seriesPairName,result);
        m_outputEdit->append(QStringLiteral("实际检查范围：GeoPDF内图廓；共同覆盖面积=%1平方米；%2")
            .arg(scope.value(QStringLiteral("common_area_m2")).toDouble(),0,'f',2).arg(scopeError));
        const QStringList errorKeys = result.errors.keys(); if (result.err_exist) ++errPairs;
        int confirmedRecordCount = 0;
        for (const QString& key : errorKeys) {
            if (isConfirmedSeriesError(result.errors.value(key).toObject())) ++confirmedRecordCount;
        }
        appendSeriesDebugLog(QStringLiteral("[501] pairResultSummary pair=%1 errExist=%2 errors=%3 confirmed=%4 lastBefore=%5")
            .arg(filePaths.seriesPairName)
            .arg(result.err_exist ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(errorKeys.size()).arg(confirmedRecordCount).arg(m_lastErrorRecords.size()));
        const QJsonObject pairInfo = result.info.value(QStringLiteral("pair_info")).toObject();
        const QJsonObject source = result.info.value(QStringLiteral("rule_source")).toObject();
        const QJsonObject totals = result.info.value(QStringLiteral("geometry_totals")).toObject();
        const QJsonObject problemSummary = result.info.value(QStringLiteral("problem_summary")).toObject();
        const QJsonObject pathSummary = result.info.value(QStringLiteral("layer_path_summary")).toObject();
        const QJsonObject expandedTargets = result.info.value(QStringLiteral("expanded_target_layers")).toObject();
        const QStringList targetLayers = source.value(QStringLiteral("target_layers")).toVariant().toStringList();
        
        QStringList expandedLayerNames;
        for (auto it = expandedTargets.constBegin(); it != expandedTargets.constEnd(); ++it) {
            const QStringList expanded = it.value().toVariant().toStringList();
            for (const QString& stdLayer : expanded) {
                if (!stdLayer.trimmed().isEmpty()) {
                    expandedLayerNames << stdLayer.trimmed();
                }
            }
        }
        for (const QString& layer : expandedLayerNames) {
            m_checkedLayersByMap[pair.leftName].insert(layer);
            m_checkedLayersByMap[pair.rightName].insert(layer);
        }

        for (const QString& layerName : targetLayers) {
            const QStringList expanded = expandedTargets.value(layerName).toVariant().toStringList();
            for (const QString& stdLayer : expanded) {
                allTargetLayers.insert(stdLayer);
            }
        }
        const QStringList lineStyleFields = source.value(QStringLiteral("line_style_fields")).toVariant().toStringList();
        const QStringList polygonStyleFields = source.value(QStringLiteral("polygon_style_fields")).toVariant().toStringList();
        const QStringList pointStyleFields = source.value(QStringLiteral("point_style_fields")).toVariant().toStringList();
        lines << QStringLiteral("%1. 图幅对：%2").arg(i + 1).arg(filePaths.seriesPairName);
        lines << QStringLiteral("   公共边长度：%1 米").arg(pairInfo.value(QStringLiteral("common_edge_length")).toDouble(), 0, 'f', 3);
        lines << QStringLiteral("   接边缓冲距离：%1 米").arg(source.value(QStringLiteral("edge_buffer_distance")).toDouble(), 0, 'f', 3);
        lines << QStringLiteral("   检查图层：%1").arg(targetLayers.join(QStringLiteral("、")));
        for (const QString& layerName : targetLayers) {
            const QJsonObject layerInfo = pathSummary.value(layerName).toObject();
            const QStringList expanded = expandedTargets.value(layerName).toVariant().toStringList();
            lines << QStringLiteral("   [%1] 展开后实际检查标准图层：%2").arg(layerName, expanded.join(QStringLiteral("、")));
            lines << QStringLiteral("   [%1] 实际成果 SHP：%2")
                .arg(layerName)
                .arg(layerInfo.value(QStringLiteral("actual_found")).toBool() ? layerInfo.value(QStringLiteral("actual_paths_text")).toString() : QStringLiteral("未找到"));
        }
        lines << QStringLiteral("   检查范围：%1").arg(source.value(QStringLiteral("scope_description")).toString());
        lines << QStringLiteral("   线要素样式字段：%1").arg(lineStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   面要素样式字段：%1").arg(polygonStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   点要素样式字段：%1").arg(pointStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   左侧候选要素数：%1").arg(totals.value(QStringLiteral("left_candidates")).toInt());
        lines << QStringLiteral("   右侧候选要素数：%1").arg(totals.value(QStringLiteral("right_candidates")).toInt());
        lines << QStringLiteral("   匹配成功要素数：%1").arg(totals.value(QStringLiteral("matched_candidates")).toInt());
        lines << QStringLiteral("   几何接边问题数：%1").arg(problemSummary.value(QStringLiteral("geometry_issue_count")).toInt());
        lines << QStringLiteral("   属性一致性问题数：%1").arg(problemSummary.value(QStringLiteral("attribute_issue_count")).toInt());
        lines << QStringLiteral("   线要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("line_style_issue_count")).toInt());
        lines << QStringLiteral("   面要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("polygon_style_issue_count")).toInt());
        lines << QStringLiteral("   点要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("point_style_issue_count")).toInt());
        lines << QStringLiteral("   字段缺失/配置问题数：%1").arg(problemSummary.value(QStringLiteral("field_config_issue_count")).toInt());
        lines << QStringLiteral("   字段值缺失问题数：%1").arg(problemSummary.value(QStringLiteral("field_value_missing_count")).toInt());
        lines << QStringLiteral("   已确认错误数：%1").arg(problemSummary.value(QStringLiteral("confirmed_error_count")).toInt()); lines << QStringLiteral("   待复核数：%1；无法检查数：%2").arg(problemSummary.value(QStringLiteral("needs_review_count")).toInt()).arg(problemSummary.value(QStringLiteral("not_evaluable_count")).toInt());
        lines << QStringLiteral("   状态：%1").arg(result.status);
        const QStringList inputErrors = result.info.value(QStringLiteral("input_validation_errors")).toVariant().toStringList();
        const auto jsonArrayToText = [](const QJsonValue& value) {
            QStringList parts;
            for (const QJsonValue& v : value.toArray()) {
                parts << v.toString();
            }
            return parts.isEmpty() ? QStringLiteral("[]") : parts.join(QStringLiteral(","));
        };
        for (const QString& msg : inputErrors) lines << QStringLiteral("   - [输入校验] %1").arg(msg);
        for (const QString& key : errorKeys) {
            QJsonObject err = result.errors.value(key).toObject();
            err = normalizeSeriesFindingStatus(err);
            const QString stableIssueId = err.value(QStringLiteral("issue_id")).toString().trimmed();
            err[QStringLiteral("err_id")] = stableIssueId.isEmpty() ? key : stableIssueId;
            if (err.value(QStringLiteral("stage")).toString().isEmpty()) err[QStringLiteral("stage")] = QStringLiteral("auto");
            if (err.value(QStringLiteral("source_phase")).toString().isEmpty()) err[QStringLiteral("source_phase")] = QStringLiteral("initial");
            if (err.value(QStringLiteral("final_state")).toString().isEmpty() && err.value(QStringLiteral("decision_status")).toString() == QStringLiteral("confirmed_error")) err[QStringLiteral("final_state")] = QStringLiteral("confirmed");
            const QString decisionStatus = err.value(QStringLiteral("decision_status")).toString().trimmed();
            if (isConfirmedSeriesError(err)) m_lastErrorRecords.push_back(err);
            if (decisionStatus == QStringLiteral("not_error") || decisionStatus == QStringLiteral("skipped")) continue;
            m_originalErrorRecords.push_back(err);
            lines << QStringLiteral("   [%1]").arg(err.value(QStringLiteral("sub_rule")).toString());
            if (!decisionStatus.isEmpty()) lines << QStringLiteral("   分流结论：%1").arg(decisionStatus);
            lines << QStringLiteral("   图幅对：%1").arg(err.value(QStringLiteral("pair_name")).toString());
            lines << QStringLiteral("   检查图层：%1").arg(err.value(QStringLiteral("layer")).toString());
            lines << QStringLiteral("   几何类型：%1").arg(err.value(QStringLiteral("geometry_type")).toString());
            lines << QStringLiteral("   实际 SHP：%1").arg(err.value(QStringLiteral("actual_shp")).toString());
            lines << QStringLiteral("   左侧图幅：%1").arg(err.value(QStringLiteral("left_sheet")).toString());
            lines << QStringLiteral("   右侧图幅：%1").arg(err.value(QStringLiteral("right_sheet")).toString());
            lines << QStringLiteral("   左侧SHP：%1").arg(err.value(QStringLiteral("left_shp")).toString());
            lines << QStringLiteral("   左侧SHP路径：%1").arg(err.value(QStringLiteral("left_shp_path")).isNull() ? QStringLiteral("NULL") : err.value(QStringLiteral("left_shp_path")).toString());
            lines << QStringLiteral("   右侧SHP：%1").arg(err.value(QStringLiteral("right_shp")).isNull() ? QStringLiteral("NULL") : err.value(QStringLiteral("right_shp")).toString());
            lines << QStringLiteral("   右侧SHP路径：%1").arg(err.value(QStringLiteral("right_shp_path")).isNull() ? QStringLiteral("NULL") : err.value(QStringLiteral("right_shp_path")).toString());
            lines << QStringLiteral("   左侧要素ID：%1").arg(err.value(QStringLiteral("left_fid")).toString());
            lines << QStringLiteral("   右侧要素ID：%1").arg(err.value(QStringLiteral("right_fid")).isNull() ? QStringLiteral("NULL") : err.value(QStringLiteral("right_fid")).toString());
            lines << QStringLiteral("   错误类型：%1").arg(err.value(QStringLiteral("error_type")).toString().isEmpty() ? err.value(QStringLiteral("sub_rule")).toString() : err.value(QStringLiteral("error_type")).toString());
            if (!err.value(QStringLiteral("field")).toString().isEmpty()) lines << QStringLiteral("   检查字段：%1").arg(err.value(QStringLiteral("field")).toString());
            if (!err.value(QStringLiteral("left_value")).isNull()) lines << QStringLiteral("   左侧值：%1").arg(err.value(QStringLiteral("left_value")).toString());
            if (!err.value(QStringLiteral("right_value")).isNull()) lines << QStringLiteral("   右侧值：%1").arg(err.value(QStringLiteral("right_value")).toString());
            if (err.contains(QStringLiteral("left_missing")) || err.contains(QStringLiteral("right_missing"))) lines << QStringLiteral("   字段缺失：左侧=%1，右侧=%2").arg(err.value(QStringLiteral("left_missing")).toBool() ? QStringLiteral("是") : QStringLiteral("否"), err.value(QStringLiteral("right_missing")).toBool() ? QStringLiteral("是") : QStringLiteral("否"));
            lines << QStringLiteral("   实际距离：%1 米").arg(err.value(QStringLiteral("distance")).isNull() ? QStringLiteral("NULL") : QString::number(err.value(QStringLiteral("distance")).toDouble(), 'f', 3));
            lines << QStringLiteral("   允许容差：%1 米").arg(err.value(QStringLiteral("tolerance")).isNull() ? QStringLiteral("NULL") : QString::number(err.value(QStringLiteral("tolerance")).toDouble(), 'f', 3));
            if (!err.value(QStringLiteral("search_radius")).isNull()) lines << QStringLiteral("   搜索半径：%1 米").arg(err.value(QStringLiteral("search_radius")).toDouble(), 0, 'f', 3);
            if (!err.value(QStringLiteral("point_source")).toString().isEmpty()) lines << QStringLiteral("   错误点来源：%1").arg(err.value(QStringLiteral("point_source")).toString());
            if (!err.value(QStringLiteral("line_source")).toString().isEmpty()) lines << QStringLiteral("   错误线来源：%1").arg(err.value(QStringLiteral("line_source")).toString());
            if (!err.value(QStringLiteral("point_to_left_feature_dist")).isNull()) lines << QStringLiteral("   点到左要素距离：%1 米").arg(err.value(QStringLiteral("point_to_left_feature_dist")).toDouble(), 0, 'f', 6);
            if (!err.value(QStringLiteral("point_to_common_edge_dist")).isNull()) lines << QStringLiteral("   点到公共边距离：%1 米").arg(err.value(QStringLiteral("point_to_common_edge_dist")).toDouble(), 0, 'f', 6);
            if (!err.value(QStringLiteral("endpoint_type")).toString().isEmpty()) lines << QStringLiteral("   端点类型：%1").arg(err.value(QStringLiteral("endpoint_type")).toString());
            if (!err.value(QStringLiteral("check_method")).toString().isEmpty()) lines << QStringLiteral("   接边判断方式：%1").arg(err.value(QStringLiteral("check_method")).toString());
            if (!err.value(QStringLiteral("check_point")).toString().isEmpty()) lines << QStringLiteral("   接边检查点坐标：%1").arg(err.value(QStringLiteral("check_point")).toString());
            if (!err.value(QStringLiteral("deviation_point")).toString().isEmpty()) lines << QStringLiteral("   偏差点坐标：%1").arg(err.value(QStringLiteral("deviation_point")).toString());
            if (!err.value(QStringLiteral("left_point")).toString().isEmpty()) lines << QStringLiteral("   左侧检查点坐标：%1").arg(err.value(QStringLiteral("left_point")).toString());
            if (!err.value(QStringLiteral("right_point")).toString().isEmpty()) lines << QStringLiteral("   右侧检查点坐标：%1").arg(err.value(QStringLiteral("right_point")).toString());
            if (!err.value(QStringLiteral("left_start_point")).toString().isEmpty()) lines << QStringLiteral("   左侧起点坐标：%1").arg(err.value(QStringLiteral("left_start_point")).toString());
            if (!err.value(QStringLiteral("left_end_point")).toString().isEmpty()) lines << QStringLiteral("   左侧终点坐标：%1").arg(err.value(QStringLiteral("left_end_point")).toString());
            lines << QStringLiteral("   说明：%1").arg(err.value(QStringLiteral("description")).toString());
        }
        
        currentCheck++;
        if (m_progressBar) {
            m_progressBar->setValue(50 + (currentCheck * 50 / totalChecks)); // 检查占后 50% 进度
            QCoreApplication::processEvents();
        }
    }

    for (int i = 0; i < selectedCoverageGroups.size(); ++i) {
        const SeriesMapEdgeMatchTaskDialog::CoverageGroup& group = selectedCoverageGroups[i];

        if (m_progressBar) {
            m_progressBar->setFormat(QStringLiteral("检查跨比例尺图幅组 (%1/%2): %3").arg(currentCheck + 1).arg(totalChecks).arg(group.smallScaleName));
            QCoreApplication::processEvents();
        }

        QString smallShpDir = resolveShpDirForSheetName(group.smallScaleName, pdfBaseNameToShpDir);
        if (smallShpDir.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("图幅“%1”未找到对应 GeoPDF 预处理目录，请检查 GeoPDF 文件名是否与图幅索引名称一致。").arg(group.smallScaleName));
            return;
        }

        QStringList largeActualDirs;
        for (int largeIndex = 0; largeIndex < group.largeScaleNames.size(); ++largeIndex) {
            const QString largeName = group.largeScaleNames[largeIndex];
            QString largeShpDir = resolveShpDirForSheetName(largeName, pdfBaseNameToShpDir);
            if (largeShpDir.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("图幅“%1”未找到对应 GeoPDF 预处理目录，请检查 GeoPDF 文件名是否与图幅索引名称一致。").arg(largeName));
                return;
            }
            largeActualDirs << largeShpDir;
        }

        const auto smallIt = std::find_if(m_sheetInfos.begin(), m_sheetInfos.end(), [&](const SeriesMapEdgeMatchTaskDialog::SheetInfo& info) {
            return info.featureId == group.smallScaleId || geoPdfBaseMatchesSheetStrict(info.displayName, group.smallScaleName);
        });
        if (smallIt == m_sheetInfos.end()) continue;

        QStringList smallActualDirs; smallActualDirs << smallShpDir;
        QStringList actualDirs = smallActualDirs;
        for (const QString& dir : largeActualDirs) {
            if (!actualDirs.contains(dir)) actualDirs << dir;
        }
        AlgorithmFilePaths filePaths;
        filePaths.shpPath = actualDirs.join(QStringLiteral(";"));
        filePaths.seriesLeftShpPath = largeActualDirs.join(QStringLiteral(";"));
        filePaths.seriesRightShpPath = smallActualDirs.join(QStringLiteral(";"));
        filePaths.xmlPaths << xmlPath;

        filePaths.checkMode = QStringLiteral("CrossScale");
        filePaths.seriesPairName = !group.displayName.isEmpty() ? group.displayName : QStringLiteral("%1 - %2").arg(group.largeScaleNames.join(QStringLiteral("、")), group.smallScaleName);
        filePaths.seriesLayerMappingJson = confirmedLayerMappings.value(filePaths.seriesPairName);
        {
            QJsonObject mapping=QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8()).object();
            mapping[QStringLiteral("source_audits")]=sourceAudits;
            filePaths.seriesLayerMappingJson=QString::fromUtf8(QJsonDocument(mapping).toJson(QJsonDocument::Compact));
        }
        filePaths.seriesLeftSheetName = group.largeScaleNames.join(QStringLiteral("、"));
        filePaths.seriesRightSheetName = group.smallScaleName;
        filePaths.seriesLeftScale = group.largeScale > 0 ? group.largeScale : defaultScale;
        filePaths.seriesRightScale = group.smallScale > 0 ? group.smallScale : defaultScale;
        filePaths.seriesSmallScaleSheetName = group.smallScaleName;
        filePaths.seriesLargeScaleSheetNames = group.largeScaleNames;
        QJsonObject scope; QString scopeError;
        const bool scopeOk = applyRealFrameScope(largeActualDirs,smallActualDirs,&filePaths,&scope,&scopeError);

        QJsonObject pairCtx;
        pairCtx[QStringLiteral("xml_path")] = xmlPath;
        pairCtx[QStringLiteral("actual_dir")] = filePaths.shpPath;
        pairCtx[QStringLiteral("scale")] = defaultScale;
        pairCtx[QStringLiteral("small_scale")] = group.smallScale > 0 ? group.smallScale : defaultScale;
        pairCtx[QStringLiteral("large_scale")] = group.largeScale > 0 ? group.largeScale : defaultScale;
        pairCtx[QStringLiteral("small_name")] = group.smallScaleName;
        pairCtx[QStringLiteral("large_names")] = QJsonArray::fromStringList(group.largeScaleNames);
        pairCtx[QStringLiteral("small_wkt")] = filePaths.seriesRightSheetWkt;
        pairCtx[QStringLiteral("large_wkts")] = QJsonArray::fromStringList(filePaths.seriesLargeScaleSheetWkts);
        pairCtx[QStringLiteral("scope")] = scope;
        pairCtx[QStringLiteral("confirmed_layer_mapping")] = QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8()).object();
        m_pairRunContext[filePaths.seriesPairName] = pairCtx;

        CheckItemResult result;
        try {
            appendSeriesDebugLog(QStringLiteral("[501] wrapperCallBegin=%1").arg(filePaths.seriesPairName));
            if (scopeOk) SeriesMapTemplateCheckWrapper(QStringLiteral("图幅接边与系列化表达"), filePaths, defaultScale, QString(), result);
            else {
                result.status = QStringLiteral("质检失败"); result.err_exist = true;
                result.info = QJsonObject{{QStringLiteral("execution_state"),QStringLiteral("invalid_scope")},{QStringLiteral("description"),scopeError}};
            }
            appendSeriesDebugLog(QStringLiteral("[501] wrapperCallEnd=%1 status=%2").arg(filePaths.seriesPairName, result.status));
        }
        catch (const std::exception& ex) {
            appendSeriesDebugLog(QStringLiteral("[501] pairException=%1 msg=%2").arg(filePaths.seriesPairName, QString::fromUtf8(ex.what())));
            result.id = QStringLiteral("501");
            result.name = QStringLiteral("图幅接边与系列化表达");
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            QJsonObject err; err["sub_rule"] = QStringLiteral("运行异常"); err["description"] = QStringLiteral("执行异常：%1").arg(QString::fromUtf8(ex.what()));
            result.errors[QStringLiteral("exception")] = err;
        }
        catch (...) {
            appendSeriesDebugLog(QStringLiteral("[501] pairUnknownException=%1").arg(filePaths.seriesPairName));
            result.id = QStringLiteral("501");
            result.name = QStringLiteral("图幅接边与系列化表达");
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            QJsonObject err; err["sub_rule"] = QStringLiteral("运行异常"); err["description"] = QStringLiteral("执行过程中发生未知异常");
            result.errors[QStringLiteral("exception")] = err;
        }
        recordPairExecution(filePaths.seriesPairName,result);
        m_outputEdit->append(QStringLiteral("实际检查范围：GeoPDF内图廓；共同覆盖面积=%1平方米；%2")
            .arg(scope.value(QStringLiteral("common_area_m2")).toDouble(),0,'f',2).arg(scopeError));
        const QStringList errorKeys = result.errors.keys(); if (result.err_exist) ++errPairs;
        int confirmedRecordCount = 0;
        for (const QString& key : errorKeys) {
            if (isConfirmedSeriesError(result.errors.value(key).toObject())) ++confirmedRecordCount;
        }
        appendSeriesDebugLog(QStringLiteral("[501] pairResultSummary pair=%1 errExist=%2 errors=%3 confirmed=%4 lastBefore=%5")
            .arg(filePaths.seriesPairName)
            .arg(result.err_exist ? QStringLiteral("true") : QStringLiteral("false"))
            .arg(errorKeys.size()).arg(confirmedRecordCount).arg(m_lastErrorRecords.size()));
        const QJsonObject pairInfo = result.info.value(QStringLiteral("pair_info")).toObject();
        const QJsonObject source = result.info.value(QStringLiteral("rule_source")).toObject();
        const QJsonObject totals = result.info.value(QStringLiteral("geometry_totals")).toObject();
        const QJsonObject problemSummary = result.info.value(QStringLiteral("problem_summary")).toObject();
        const QJsonObject pathSummary = result.info.value(QStringLiteral("layer_path_summary")).toObject();
        const QJsonObject expandedTargets = result.info.value(QStringLiteral("expanded_target_layers")).toObject();
        const QStringList targetLayers = source.value(QStringLiteral("target_layers")).toVariant().toStringList();
        
        QStringList expandedLayerNames;
        for (auto it = expandedTargets.constBegin(); it != expandedTargets.constEnd(); ++it) {
            const QStringList expanded = it.value().toVariant().toStringList();
            for (const QString& stdLayer : expanded) {
                if (!stdLayer.trimmed().isEmpty()) {
                    expandedLayerNames << stdLayer.trimmed();
                }
            }
        }
        for (const QString& layer : expandedLayerNames) {
            m_checkedLayersByMap[group.smallScaleName].insert(layer);
            for (const QString& largeName : group.largeScaleNames) {
                m_checkedLayersByMap[largeName].insert(layer);
            }
        }

        for (const QString& layerName : targetLayers) {
            const QStringList expanded = expandedTargets.value(layerName).toVariant().toStringList();
            for (const QString& stdLayer : expanded) {
                allTargetLayers.insert(stdLayer);
            }
        }
        const QStringList lineStyleFields = source.value(QStringLiteral("line_style_fields")).toVariant().toStringList();
        const QStringList polygonStyleFields = source.value(QStringLiteral("polygon_style_fields")).toVariant().toStringList();
        const QStringList pointStyleFields = source.value(QStringLiteral("point_style_fields")).toVariant().toStringList();
        lines << QStringLiteral("%1. 跨比例尺组：%2").arg(selectedPairs.size() + i + 1).arg(filePaths.seriesPairName);
        lines << QStringLiteral("   检查图层：%1").arg(targetLayers.join(QStringLiteral("、")));
        for (const QString& layerName : targetLayers) {
            const QJsonObject layerInfo = pathSummary.value(layerName).toObject();
            const QStringList expanded = expandedTargets.value(layerName).toVariant().toStringList();
            lines << QStringLiteral("   [%1] 展开后实际检查标准图层：%2").arg(layerName, expanded.join(QStringLiteral("、")));
            lines << QStringLiteral("   [%1] 实际成果 SHP：%2")
                .arg(layerName)
                .arg(layerInfo.value(QStringLiteral("actual_found")).toBool() ? layerInfo.value(QStringLiteral("actual_paths_text")).toString() : QStringLiteral("未找到"));
        }
        lines << QStringLiteral("   检查范围：%1").arg(source.value(QStringLiteral("scope_description")).toString());
        lines << QStringLiteral("   线要素样式字段：%1").arg(lineStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   面要素样式字段：%1").arg(polygonStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   点要素样式字段：%1").arg(pointStyleFields.join(QStringLiteral("、")));
        lines << QStringLiteral("   小比例尺候选要素数：%1").arg(totals.value(QStringLiteral("small_scale_candidates")).toInt());
        lines << QStringLiteral("   大比例尺候选要素数：%1").arg(totals.value(QStringLiteral("large_scale_candidates")).toInt());
        lines << QStringLiteral("   匹配成功要素数：%1").arg(totals.value(QStringLiteral("matched_candidates")).toInt());
        lines << QStringLiteral("   几何接边问题数：%1").arg(problemSummary.value(QStringLiteral("geometry_issue_count")).toInt());
        lines << QStringLiteral("   属性一致性问题数：%1").arg(problemSummary.value(QStringLiteral("attribute_issue_count")).toInt());
        lines << QStringLiteral("   线要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("line_style_issue_count")).toInt());
        lines << QStringLiteral("   面要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("polygon_style_issue_count")).toInt());
        lines << QStringLiteral("   点要素样式问题数：%1").arg(problemSummary.value(QStringLiteral("point_style_issue_count")).toInt());
        lines << QStringLiteral("   字段缺失/配置问题数：%1").arg(problemSummary.value(QStringLiteral("field_config_issue_count")).toInt());
        lines << QStringLiteral("   字段值缺失问题数：%1").arg(problemSummary.value(QStringLiteral("field_value_missing_count")).toInt());
        lines << QStringLiteral("   已确认错误数：%1").arg(problemSummary.value(QStringLiteral("confirmed_error_count")).toInt()); lines << QStringLiteral("   待复核数：%1；无法检查数：%2").arg(problemSummary.value(QStringLiteral("needs_review_count")).toInt()).arg(problemSummary.value(QStringLiteral("not_evaluable_count")).toInt());
        lines << QStringLiteral("   状态：%1").arg(result.status);
        const QStringList inputErrors = result.info.value(QStringLiteral("input_validation_errors")).toVariant().toStringList();
        for (const QString& msg : inputErrors) lines << QStringLiteral("   - [输入校验] %1").arg(msg);
        for (const QString& key : errorKeys) {
            QJsonObject err = result.errors.value(key).toObject();
            err = normalizeSeriesFindingStatus(err);
            const QString stableIssueId = err.value(QStringLiteral("issue_id")).toString().trimmed();
            err[QStringLiteral("err_id")] = stableIssueId.isEmpty() ? key : stableIssueId;
            if (err.value(QStringLiteral("stage")).toString().isEmpty()) err[QStringLiteral("stage")] = QStringLiteral("auto");
            if (err.value(QStringLiteral("source_phase")).toString().isEmpty()) err[QStringLiteral("source_phase")] = QStringLiteral("initial");
            if (err.value(QStringLiteral("final_state")).toString().isEmpty() && err.value(QStringLiteral("decision_status")).toString() == QStringLiteral("confirmed_error")) err[QStringLiteral("final_state")] = QStringLiteral("confirmed");
            const QString decisionStatus = err.value(QStringLiteral("decision_status")).toString().trimmed();
            if (isConfirmedSeriesError(err)) m_lastErrorRecords.push_back(err);
            if (decisionStatus == QStringLiteral("not_error") || decisionStatus == QStringLiteral("skipped")) continue;
            m_originalErrorRecords.push_back(err);
            lines << QStringLiteral("   [%1]").arg(err.value(QStringLiteral("sub_rule")).toString());
            if (!decisionStatus.isEmpty()) lines << QStringLiteral("   分流结论：%1").arg(decisionStatus);
            lines << QStringLiteral("   跨比例尺组：%1").arg(err.value(QStringLiteral("pair_name")).toString());
            lines << QStringLiteral("   检查图层：%1").arg(err.value(QStringLiteral("layer")).toString());
            lines << QStringLiteral("   几何类型：%1").arg(err.value(QStringLiteral("geometry_type")).toString());
            lines << QStringLiteral("   小比例尺图幅：%1").arg(err.value(QStringLiteral("small_scale_sheet")).toString());
            lines << QStringLiteral("   大比例尺图幅：%1").arg(err.value(QStringLiteral("large_scale_sheet")).toString());
            lines << QStringLiteral("   小比例尺要素ID：%1").arg(err.value(QStringLiteral("small_scale_fid")).toString());
            lines << QStringLiteral("   大比例尺要素ID：%1").arg(err.value(QStringLiteral("large_scale_fid")).isNull() ? QStringLiteral("NULL") : err.value(QStringLiteral("large_scale_fid")).toString());
            lines << QStringLiteral("   错误类型：%1").arg(err.value(QStringLiteral("error_type")).toString().isEmpty() ? err.value(QStringLiteral("sub_rule")).toString() : err.value(QStringLiteral("error_type")).toString());
            if (!err.value(QStringLiteral("field")).toString().isEmpty()) lines << QStringLiteral("   检查字段：%1").arg(err.value(QStringLiteral("field")).toString());
            if (!err.value(QStringLiteral("small_scale_value")).isNull()) lines << QStringLiteral("   小比例尺值：%1").arg(err.value(QStringLiteral("small_scale_value")).toString());
            if (!err.value(QStringLiteral("large_scale_value")).isNull()) lines << QStringLiteral("   大比例尺值：%1").arg(err.value(QStringLiteral("large_scale_value")).toString());
            lines << QStringLiteral("   说明：%1").arg(err.value(QStringLiteral("description")).toString());
        }
        
        currentCheck++;
        if (m_progressBar) {
            m_progressBar->setValue(50 + (currentCheck * 50 / totalChecks)); // 检查占后 50% 进度
            QCoreApplication::processEvents();
        }
    }

    const QJsonObject execution = executionSummaryForMap(QString());
    lines << QString() << QStringLiteral("已处理%1组，已跳过无有效关系%2组%3。").arg(selectedPairs.size() + selectedCoverageGroups.size()).arg(skippedNoRelationPairs.size()).arg(skippedNoRelationDetail);
    lines << QStringLiteral("完成%1组，失败%2组，未执行%3组；记录问题的任务%4组。")
        .arg(execution.value(QStringLiteral("executed_pair_count")).toInt())
        .arg(execution.value(QStringLiteral("failed_pair_count")).toInt())
        .arg(execution.value(QStringLiteral("skipped_pair_count")).toInt()).arg(errPairs);
    lines << QStringLiteral("图幅接边检查完成。");
    const QString runtimeLog = m_outputEdit ? m_outputEdit->toPlainText().trimmed() : QString();
    const QString summaryText = lines.join(QStringLiteral("\n"));
    m_outputEdit->setPlainText(runtimeLog.isEmpty() ? summaryText : runtimeLog + QStringLiteral("\n\n================ 检查汇总 ================\n") + summaryText);
    m_originalStats = QJsonObject();
    m_originalStats.insert(QStringLiteral("check_completed"), true);
    m_originalStats.insert(QStringLiteral("error_count"), m_lastErrorRecords.size());
    m_originalStats.insert(QStringLiteral("confirmed_error_count"), m_lastErrorRecords.size());
    m_originalStats.insert(QStringLiteral("finding_count"), m_originalErrorRecords.size());
    int reviewCount = 0;
    int notEvaluableCount = 0;
    for (const QJsonObject& record : m_originalErrorRecords) {
        const QString status = record.value(QStringLiteral("decision_status")).toString();
        if (status == QStringLiteral("needs_review")) ++reviewCount;
        else if (status == QStringLiteral("not_evaluable")) ++notEvaluableCount;
    }
    m_originalStats.insert(QStringLiteral("needs_review_count"), reviewCount);
    m_originalStats.insert(QStringLiteral("not_evaluable_count"), notEvaluableCount);
    m_originalStats.insert(QStringLiteral("pair_count"), selectedPairs.size() + selectedCoverageGroups.size());
    m_originalStats.insert(QStringLiteral("execution_summary"), execution);
    m_originalStats.insert(QStringLiteral("execution_complete"), execution.value(QStringLiteral("status")).toString() == QStringLiteral("质检成功"));
    m_originalStats.insert(QStringLiteral("error_pair_count"), errPairs);
    QJsonArray targetLayersArray;
    for (const QString& layer : allTargetLayers) targetLayersArray.append(layer);
    m_originalStats.insert(QStringLiteral("target_layers"), targetLayersArray);
    QString cacheFailedPath;
    const bool cacheOk = saveSeriesStageCacheFiles(outputRootDir(), m_lastErrorRecords, m_originalErrorRecords, &cacheFailedPath);
    m_exportErrorButton->setEnabled(false);

    if (m_progressBar) {
        m_progressBar->setValue(100);
        m_progressBar->setFormat(QStringLiteral("检查完成"));
        QCoreApplication::processEvents();
    }

    QString detailText;
    if (!cacheOk) {
        detailText = QStringLiteral("\n初检结果暂存失败：%1").arg(cacheFailedPath);
    } else {
        bool outputOk = writeCurrentOutputs();
        if (execution.value(QStringLiteral("status")).toString() != QStringLiteral("质检成功")) {
            detailText = QStringLiteral("\n%1\n%2").arg(execution.value(QStringLiteral("description")).toString(),
                outputOk ? QStringLiteral("已保存任务状态，请查看JSON和日志。") : QStringLiteral("任务状态输出失败。"));
        } else if (m_lastErrorRecords.isEmpty()) {
            detailText = outputOk ? QStringLiteral("\n当前无错误，已输出无错误质检结果。") : QStringLiteral("\n当前无错误，但输出结果失败。");
        } else {
            detailText = outputOk ? QStringLiteral("\n已自动导出错误结果。") : QStringLiteral("\n导出错误结果失败。");
        }
    }
    
    int totalTaskCount = selectedPairs.size() + selectedCoverageGroups.size();
    QMessageBox::information(this, QStringLiteral("检查完成"), 
        QStringLiteral("已处理%1组，已跳过无有效关系%2组%3。\n其中同比例尺图幅对 %4 对，跨比例尺覆盖组 %5 组。%6")
        .arg(totalTaskCount)
        .arg(skippedNoRelationPairs.size())
        .arg(skippedNoRelationDetail)
        .arg(selectedPairs.size())
        .arg(selectedCoverageGroups.size())
        .arg(detailText));
}

void SeriesMapEdgeMatchTaskDialog::exportErrorShp(const QVector<QJsonObject>& records, const QString& pointFileName, const QString& lineFileName)
{
    Q_UNUSED(records);
    Q_UNUSED(pointFileName);
    Q_UNUSED(lineFileName);
    if (writeCurrentOutputs()) {
        QMessageBox::information(this, QStringLiteral("输出完成"),
            QStringLiteral("已重新生成一期兼容输出，请前往输出目录查看各图层的单项结果。"));
    }
}

void SeriesMapEdgeMatchTaskDialog::exportReport()
{
    if (!m_reportExportHandler) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("质检报告导出功能未初始化。"));
        return;
    }
    m_reportExportHandler(currentReportResultJsonPaths());
}
