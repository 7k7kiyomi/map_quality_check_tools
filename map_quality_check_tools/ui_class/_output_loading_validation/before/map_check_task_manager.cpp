#include "map_check_task_manager.h"
#include "map_check_algorithms.h"
#include "point_isolation_output.h"
#include "redundancy_output_paths.h"
#include "quality_result_layer_group.h"
#include "line_topology_shared_tasks.h"
#include "../algorithm/se_annotation_check_interface.h"
#include "map_check_task_manager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTextStream>
#include <QFileDialog>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QDomElement>
#include <QDir>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QMenu>
#include <QlistView>
#include< QDirIterator>
#include <QTextStream>
#include <QDebug>
#include <QCoreApplication>
#include <QThread>
#include <QFrame>
#include <QScrollArea>
#include <QSizePolicy>
#include <stdexcept>
#include <QSettings>
#include <QSet>
#include <QFileInfo>
#include <QJsonValue>
#include <QTimer>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <QPushButton>
#include <QApplication>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <qgsgeometry.h>
#include <qgswkbtypes.h>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <ogr_spatialref.h>
#include <cpl_string.h>
#include "Drawing_Layout.h"

using namespace std;

static bool isSeriesEdgeMatchItem(const QString& id, const QString& name)
{
    return id.trimmed() == QStringLiteral("501")
        || name.trimmed() == QStringLiteral("图幅接边与系列化表达");
}

// 这些检查会创建 QWidget 或使用依赖 GUI 线程的 QGIS 对象。
static bool requiresGuiBoundCheck(const QString& checkItemName)
{
    const QString name = checkItemName.trimmed();
    return name == QStringLiteral("线要素拓扑检查")
        || name == QStringLiteral("几何走向异常检查")
        || name == QStringLiteral("直角化角度阈值控制")
        || name == QStringLiteral("要素间隔检查");
}

static void runAlgorithmWithThreadAffinity(
    const QString& checkItemName,
    const AlgorithmFunc& algorithm,
    const AlgorithmFilePaths& filePaths,
    int scale,
    const QString& outputDir,
    CheckItemResult& result)
{
    QCoreApplication* application = QCoreApplication::instance();
    const bool pointIsolation = checkItemName.trimmed() == QStringLiteral("点要素孤立检查");
    const bool featureRedundancy = checkItemName.trimmed() == QStringLiteral("要素冗余检查");
    if ((pointIsolation || featureRedundancy) && !outputDir.trimmed().isEmpty()) {
        // 先在 GUI 线程卸载旧结果图层，避免算法清理旧 Shapefile 时文件仍被占用。
        const auto unloadPrevious = [&]() {
            QgsProject* project = QgsProject::instance();
            if (!project) return;
            const auto layers = project->mapLayers();
            for (QgsMapLayer* layer : layers) {
                auto* vector = qobject_cast<QgsVectorLayer*>(layer);
                if (!vector) continue;
                const QString source = vector->source().section(QLatin1Char('|'), 0, 0);
                const bool owned = pointIsolation
                    ? PointIsolationOutput::ownsFile(source, outputDir, filePaths.standardLayer)
                    : RedundancyOutput::ownsFile(source, outputDir, filePaths.standardLayer);
                if (owned) {
                    project->removeMapLayer(vector->id());
                }
            }
        };
        if (!application || QThread::currentThread() == application->thread()) {
            unloadPrevious();
        }
        else if (!QMetaObject::invokeMethod(application, unloadPrevious, Qt::BlockingQueuedConnection)) {
            throw std::runtime_error("failed to unload previous check result layers on Qt main thread");
        }
    }
    if (!requiresGuiBoundCheck(checkItemName) || !application ||
        QThread::currentThread() == application->thread()) {
        algorithm(checkItemName, filePaths, scale, outputDir, result);
        return;
    }

    const bool invoked = QMetaObject::invokeMethod(
        application,
        [&]() { algorithm(checkItemName, filePaths, scale, outputDir, result); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        throw std::runtime_error("failed to dispatch GUI-bound check to Qt main thread");
    }
}

static QString commonSnapshotDir(const QStringList& paths)
{
    if (paths.isEmpty()) return QString();
    QString common = QDir::cleanPath(paths.first());
    for (const QString& raw : paths) {
        const QString path = QDir::cleanPath(raw);
        while (!common.isEmpty() && !path.startsWith(common, Qt::CaseInsensitive)) {
            const QString parent = QFileInfo(common).path();
            if (parent == common) { common.clear(); break; }
            common = parent;
        }
    }
    return common;
}

// XML 中的严重程度固定为“高 / 中 / 低”。未配置或配置异常时，为保持
// 总 JSON 结构完整，按“未配置”输出。
static QString normalizedSeverity(const QString& severity)
{
    const QString value = severity.trimmed();
    if (value == QStringLiteral("高") || value == QStringLiteral("中") || value == QStringLiteral("低")) {
        return value;
    }
    return QStringLiteral("未配置");
}

static QString severityFolderName(const QString& severity)
{
    const QString normalized = normalizedSeverity(severity);
    return normalized == QStringLiteral("高") || normalized == QStringLiteral("中")
        || normalized == QStringLiteral("低")
        ? normalized + QStringLiteral("严重程度") : QString();
}

static QString safeOutputName(QString name)
{
    name = name.trimmed();
    for (int i = 0; i < name.size(); ++i) {
        const QChar ch = name.at(i);
        if (QStringLiteral("<>:\"/\\|?*").contains(ch) || ch.isNull()) name[i] = QLatin1Char('_');
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) name.chop(1);
    return name.isEmpty() ? QStringLiteral("未命名") : name;
}

// 按完整图层名查找 Shapefile，但忽略大小写。这样旧配置中的
// BOUP6 也能找到数据库裁剪后生成的 boup6.shp，同时不会误命中 boup60。
static QString resolveShapefilePath(const QString& folderPath, const QString& layerName)
{
    const QString expectedName = layerName.trimmed();
    QDir directory(folderPath.trimmed());
    if (expectedName.isEmpty() || !directory.exists()) return QString();

    // 先走直接路径；在 Windows 等大小写不敏感文件系统上可避免反复扫描目录。
    const QString directPath = directory.filePath(expectedName + QStringLiteral(".shp"));
    if (QFileInfo::exists(directPath)) return QFileInfo(directPath).absoluteFilePath();

    // 在大小写敏感的文件系统上再遍历，返回磁盘上的真实文件名。
    const QFileInfoList files = directory.entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& file : files) {
        if (file.suffix().compare(QStringLiteral("shp"), Qt::CaseInsensitive) == 0
            && file.completeBaseName().compare(expectedName, Qt::CaseInsensitive) == 0) {
            return file.absoluteFilePath();
        }
    }
    return QString();
}

static QString uniqueDescriptionFieldName(const OGRFeatureDefn* definition)
{
    QString candidate = QStringLiteral("CheckDesc");
    int suffix = 2;
    auto fieldExists = [definition](const QString& fieldName) {
        for (int i = 0; i < definition->GetFieldCount(); ++i) {
            if (QString::fromUtf8(definition->GetFieldDefn(i)->GetNameRef())
                    .compare(fieldName, Qt::CaseInsensitive) == 0) return true;
        }
        return false;
    };
    while (fieldExists(candidate)) {
        candidate = QStringLiteral("ChkD%1").arg(suffix++);
    }
    return candidate;
}

static int findFieldIndexCaseInsensitive(const OGRFeatureDefn* definition, const QString& fieldName)
{
    for (int i = 0; i < definition->GetFieldCount(); ++i) {
        if (QString::fromUtf8(definition->GetFieldDefn(i)->GetNameRef())
                .compare(fieldName, Qt::CaseInsensitive) == 0) return i;
    }
    return -1;
}

static bool makePrecheckSymbolArea(const QVector<QgsGeometry>& lineParts, QgsGeometry& area)
{
    if (lineParts.isEmpty()) return false;
    const QgsGeometry nodedLines = QgsGeometry::unaryUnion(lineParts);
    if (!nodedLines.isNull() && !nodedLines.isEmpty()) {
        QVector<QgsGeometry> polygonInputs;
        polygonInputs.append(nodedLines);
        QgsGeometry polygonized = QgsGeometry::polygonize(polygonInputs);
        polygonized.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
        if (!polygonized.isNull() && !polygonized.isEmpty()
            && QgsWkbTypes::geometryType(polygonized.wkbType()) == QgsWkbTypes::PolygonGeometry
            && polygonized.isGeosValid()) {
            area = polygonized;
            return true;
        }
    }

    // 有些闭合圆线端点存在极小精度差，polygonize 可能漏掉；对闭合线环直接构面兜底。
    for (const QgsGeometry& partGeometry : lineParts) {
        QgsMultiPolylineXY lines;
        if (partGeometry.isMultipart()) lines = partGeometry.asMultiPolyline();
        else lines.append(partGeometry.asPolyline());

        for (QgsPolylineXY line : lines) {
            if (line.size() < 4) continue;
            const QgsPointXY first = line.first();
            const QgsPointXY last = line.last();
            const double dx = first.x() - last.x();
            const double dy = first.y() - last.y();
            if (std::sqrt(dx * dx + dy * dy) > 1e-6) continue;
            line.last() = first;

            QgsPolygonXY ring;
            ring.append(line);
            const QgsGeometry ringArea = QgsGeometry::fromPolygonXY(ring);
            if (ringArea.isNull() || ringArea.isEmpty() || !ringArea.isGeosValid()) continue;
            if (area.isNull()) area = ringArea;
            else area = area.combine(ringArea);
        }
    }
    return !area.isNull() && !area.isEmpty() && area.isGeosValid();
}

static QgsGeometry qgsGeometryFromOgr(const OGRGeometry* geometry)
{
    if (!geometry) return QgsGeometry();
    const size_t wkbSize = geometry->WkbSize();
    QByteArray wkb(static_cast<int>(wkbSize), Qt::Uninitialized);
    if (geometry->exportToWkb(wkbNDR, reinterpret_cast<unsigned char*>(wkb.data())) != OGRERR_NONE)
        return QgsGeometry();
    QgsGeometry result;
    result.fromWkb(wkb);
    return result;
}

static OGRGeometry* ogrGeometryFromQgs(const QgsGeometry& geometry)
{
    if (geometry.isNull() || geometry.isEmpty()) return nullptr;
    const QByteArray wkb = geometry.asWkb();
    OGRGeometry* result = nullptr;
    if (OGRGeometryFactory::createFromWkb(
            reinterpret_cast<const unsigned char*>(wkb.constData()), nullptr, &result,
            static_cast<size_t>(wkb.size())) != OGRERR_NONE) return nullptr;
    return result;
}

static QString normalizedFontFamily(QString value)
{
    return value.trimmed().toCaseFolded();
}

static QString normalizedFontColor(QString value)
{
    value = value.trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (value.startsWith(QLatin1Char('#'))) value.remove(0, 1);
    return value;
}

static int scaleDenominatorFromText(const QString& scaleText)
{
    static const QRegularExpression labeledScalePattern(
        QStringLiteral("1\\s*[:：]\\s*([0-9,，\\s]+)"));
    static const QRegularExpression numericScalePattern(QStringLiteral("^\\s*([0-9,，]+)\\s*$"));
    QRegularExpressionMatch match = labeledScalePattern.match(scaleText.trimmed());
    if (!match.hasMatch()) match = numericScalePattern.match(scaleText.trimmed());
    if (!match.hasMatch()) return 0;
    QString denominatorText = match.captured(1);
    denominatorText.remove(QLatin1Char(','));
    denominatorText.remove(QChar(0xFF0C));
    denominatorText.remove(QRegularExpression(QStringLiteral("\\s+")));
    bool ok = false;
    const int denominator = denominatorText.toInt(&ok);
    return ok && denominator > 0 ? denominator : 0;
}

static int scaleDenominatorFromResult(const CheckItemResult& result)
{
    int denominator = scaleDenominatorFromText(result.info.value(QStringLiteral("calculated_scale")).toString());
    if (denominator > 0) return denominator;
    denominator = scaleDenominatorFromText(result.info.value(QStringLiteral("legend_scale")).toString());
    if (denominator > 0) return denominator;
    for (auto it = result.errors.constBegin(); it != result.errors.constEnd(); ++it) {
        const QJsonObject error = it.value().toObject();
        denominator = scaleDenominatorFromText(error.value(QStringLiteral("calculated_scale")).toString());
        if (denominator > 0) return denominator;
        denominator = scaleDenominatorFromText(error.value(QStringLiteral("legend_scale")).toString());
        if (denominator > 0) return denominator;
    }
    return 0;
}

static bool writeActualLayerSpatialPrecheckErrors(const QString& sourcePath,
                                                   const QStringList& gdbPaths,
                                                   int scaleDenominator,
                                                   double bufferMillimeters,
                                                   const QString& outputPath,
                                                   int& totalFeatures,
                                                   int& unmatchedFeatures,
                                                   QString& errorMessage)
{
    const QString bufferText = QString::number(bufferMillimeters, 'f',
        qFuzzyCompare(bufferMillimeters, std::round(bufferMillimeters)) ? 0 : 1);
    struct GdbSource {
        GDALDataset* dataset = nullptr;
        OGRLayer* layer = nullptr;
        OGRSpatialReference* sourceSpatialRef = nullptr;
        OGRSpatialReference* spatialRef = nullptr;
        OGRCoordinateTransformation* transform = nullptr;
        OGRwkbGeometryType geometryType = wkbUnknown;
    };
    QVector<GdbSource> gdbSources;
    auto closeGdbSources = [&gdbSources]() {
        for (GdbSource& source : gdbSources) {
            if (source.transform) OCTDestroyCoordinateTransformation(source.transform);
            if (source.sourceSpatialRef) source.sourceSpatialRef->Release();
            if (source.spatialRef) source.spatialRef->Release();
            if (source.dataset) GDALClose(source.dataset);
        }
        gdbSources.clear();
    };

    GDALAllRegister();
    GDALDataset* sourceDataset = static_cast<GDALDataset*>(GDALOpenEx(
        sourcePath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!sourceDataset) {
        errorMessage = QStringLiteral("无法打开实际图层：%1").arg(sourcePath);
        return false;
    }
    OGRLayer* sourceLayer = sourceDataset->GetLayer(0);
    if (!sourceLayer || !sourceLayer->GetLayerDefn()) {
        errorMessage = QStringLiteral("无法读取实际图层结构：%1").arg(sourcePath);
        GDALClose(sourceDataset);
        return false;
    }
    OGRFeatureDefn* sourceDefinition = sourceLayer->GetLayerDefn();
    const int symbolIdIndex = findFieldIndexCaseInsensitive(sourceDefinition, QStringLiteral("symbol_id"));
    const OGRwkbGeometryType actualGeometryType = wkbFlatten(sourceLayer->GetGeomType());
    const bool actualIsLine = actualGeometryType == wkbLineString || actualGeometryType == wkbMultiLineString;
    OGRSpatialReference* sourceSpatialRef = sourceLayer->GetSpatialRef();
    if (!sourceSpatialRef) {
        errorMessage = QStringLiteral("实际图层缺少坐标参考系，无法按 %1 毫米缓冲检查：%2")
            .arg(bufferText, sourcePath);
        GDALClose(sourceDataset);
        return false;
    }

    QSet<QString> visitedGdbPaths;
    for (const QString& gdbPath : gdbPaths) {
        const QString pathKey = QDir::cleanPath(gdbPath).toCaseFolded();
        if (visitedGdbPaths.contains(pathKey)) continue;
        visitedGdbPaths.insert(pathKey);
        GDALDataset* gdbDataset = static_cast<GDALDataset*>(GDALOpenEx(
            gdbPath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
        if (!gdbDataset || !gdbDataset->GetLayer(0)) {
            if (gdbDataset) GDALClose(gdbDataset);
            errorMessage = QStringLiteral("无法打开对应 GDB 图层：%1").arg(gdbPath);
            closeGdbSources();
            GDALClose(sourceDataset);
            return false;
        }
        OGRLayer* gdbLayer = gdbDataset->GetLayer(0);
        OGRSpatialReference* gdbLayerSrs = gdbLayer->GetSpatialRef();
        if (!gdbLayerSrs || !gdbLayerSrs->IsProjected()) {
            errorMessage = QStringLiteral("对应 GDB 图层不是投影坐标系，无法按米构建 %1 毫米缓冲：%2")
                .arg(bufferText, gdbPath);
            GDALClose(gdbDataset);
            closeGdbSources();
            GDALClose(sourceDataset);
            return false;
        }
        OGRSpatialReference* gdbSrsClone = gdbLayerSrs->Clone();
        if (gdbSrsClone->GetLinearUnits() <= 0.0) {
            gdbSrsClone->Release();
            GDALClose(gdbDataset);
            errorMessage = QStringLiteral("对应 GDB 图层的坐标单位无效：%1").arg(gdbPath);
            closeGdbSources();
            GDALClose(sourceDataset);
            return false;
        }
#if GDAL_VERSION_NUM >= 3000000
        gdbSrsClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
#endif
        OGRSpatialReference* sourceSrsClone = sourceSpatialRef->Clone();
#if GDAL_VERSION_NUM >= 3000000
        sourceSrsClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
#endif
        OGRCoordinateTransformation* transform = nullptr;
        if (!sourceSrsClone->IsSame(gdbSrsClone)) {
            transform = OGRCreateCoordinateTransformation(sourceSrsClone, gdbSrsClone);
            if (!transform) {
                sourceSrsClone->Release();
                gdbSrsClone->Release();
                GDALClose(gdbDataset);
                errorMessage = QStringLiteral("实际图层与 GDB 图层坐标系无法转换：%1").arg(gdbPath);
                closeGdbSources();
                GDALClose(sourceDataset);
                return false;
            }
        }
        GdbSource source;
        source.dataset = gdbDataset;
        source.layer = gdbLayer;
        source.sourceSpatialRef = sourceSrsClone;
        source.spatialRef = gdbSrsClone;
        source.transform = transform;
        source.geometryType = wkbFlatten(gdbLayer->GetGeomType());
        gdbSources.push_back(source);
    }
    if (gdbSources.isEmpty()) {
        errorMessage = QStringLiteral("没有可用的对应 GDB 图层");
        GDALClose(sourceDataset);
        return false;
    }

    GDALDriver* outputDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!outputDriver) {
        errorMessage = QStringLiteral("无法获取 Shapefile 输出驱动");
        closeGdbSources();
        GDALClose(sourceDataset);
        return false;
    }
    QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.absolutePath());
    const QByteArray outputPathUtf8 = outputPath.toUtf8();
    GDALDataset* outputDataset = outputDriver->Create(outputPathUtf8.constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!outputDataset) {
        errorMessage = QStringLiteral("无法创建实际图层错误结果：%1").arg(outputPath);
        closeGdbSources();
        GDALClose(sourceDataset);
        return false;
    }
    char** layerOptions = nullptr;
    layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
    const OGRwkbGeometryType outputGeometryType = actualIsLine && symbolIdIndex >= 0
        ? wkbMultiLineString : sourceLayer->GetGeomType();
    OGRLayer* outputLayer = outputDataset->CreateLayer(
        "actual_layer_mixed_errors", sourceLayer->GetSpatialRef(), outputGeometryType, layerOptions);
    CSLDestroy(layerOptions);
    bool outputReady = outputLayer != nullptr;
    if (!outputReady) errorMessage = QStringLiteral("无法创建实际图层错误输出层");
    for (int i = 0; outputReady && i < sourceDefinition->GetFieldCount(); ++i) {
        OGRFieldDefn field(sourceDefinition->GetFieldDefn(i));
        if (outputLayer->CreateField(&field) != OGRERR_NONE) {
            outputReady = false;
            errorMessage = QStringLiteral("复制实际图层属性字段失败：%1")
                .arg(QString::fromUtf8(sourceDefinition->GetFieldDefn(i)->GetNameRef()));
        }
    }
    const QString descriptionField = uniqueDescriptionFieldName(sourceDefinition);
    if (outputReady) {
        OGRFieldDefn descriptionDef(descriptionField.toUtf8().constData(), OFTString);
        descriptionDef.SetWidth(80);
        if (outputLayer->CreateField(&descriptionDef) != OGRERR_NONE) {
            outputReady = false;
            errorMessage = QStringLiteral("无法创建实际图层检查说明字段");
        }
    }

    if (outputReady) {
        const int outputDescriptionIndex = outputLayer->GetLayerDefn()->GetFieldIndex(descriptionField.toUtf8().constData());
        bool hasPointGdb = false;
        for (const GdbSource& gdbSource : gdbSources) {
            if (gdbSource.geometryType == wkbPoint || gdbSource.geometryType == wkbMultiPoint) {
                hasPointGdb = true;
                break;
            }
        }

        // 点符号由多条线组成：按 symbol_id 聚合完整线网并面化，再检查符号面中是否有对应 GDB 点。
        // 这样不会把符号外框的每条线分别当作独立实际要素做 2 mm 缓冲判断。
        QHash<QString, QVector<QgsGeometry>> symbolLineParts;
        QHash<QString, QgsGeometry> combinedSymbolGeometries;
        QSet<QString> matchedPointSymbols;
        if (actualIsLine && symbolIdIndex >= 0) {
            sourceLayer->ResetReading();
            OGRFeature* component = nullptr;
            while ((component = sourceLayer->GetNextFeature()) != nullptr) {
                if (component->IsFieldSetAndNotNull(symbolIdIndex)) {
                    const QString symbolId = QString::fromUtf8(component->GetFieldAsString(symbolIdIndex)).trimmed();
                    OGRGeometry* geometry = component->GetGeometryRef();
                    if (!symbolId.isEmpty() && geometry) {
                        const QgsGeometry part = qgsGeometryFromOgr(geometry);
                        if (!part.isNull() && !part.isEmpty()) symbolLineParts[symbolId].append(part);
                    }
                }
                OGRFeature::DestroyFeature(component);
            }

            for (auto symbolIt = symbolLineParts.constBegin(); symbolIt != symbolLineParts.constEnd(); ++symbolIt) {
                QgsGeometry combinedGeometry = QgsGeometry::collectGeometry(symbolIt.value());
                if (combinedGeometry.isNull() || combinedGeometry.isEmpty()) continue;
                if (QgsWkbTypes::geometryType(combinedGeometry.wkbType()) != QgsWkbTypes::LineGeometry)
                    combinedGeometry.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
                if (combinedGeometry.isNull() || combinedGeometry.isEmpty()
                    || QgsWkbTypes::geometryType(combinedGeometry.wkbType()) != QgsWkbTypes::LineGeometry) continue;
                if (!combinedGeometry.isMultipart()) combinedGeometry.convertToMultiType();
                combinedSymbolGeometries.insert(symbolIt.key(), combinedGeometry);

                if (!hasPointGdb) continue;
                QgsGeometry symbolArea;
                if (!makePrecheckSymbolArea(symbolIt.value(), symbolArea)) continue;
                for (GdbSource& gdbSource : gdbSources) {
                    if (gdbSource.geometryType != wkbPoint && gdbSource.geometryType != wkbMultiPoint) continue;
                    OGRGeometry* areaInGdbCrs = ogrGeometryFromQgs(symbolArea);
                    if (!areaInGdbCrs) continue;
                    if (gdbSource.transform && areaInGdbCrs->transform(gdbSource.transform) != OGRERR_NONE) {
                        OGRGeometryFactory::destroyGeometry(areaInGdbCrs);
                        continue;
                    }
                    gdbSource.layer->SetSpatialFilter(areaInGdbCrs);
                    gdbSource.layer->ResetReading();
                    OGRFeature* pointFeature = nullptr;
                    while ((pointFeature = gdbSource.layer->GetNextFeature()) != nullptr) {
                        OGRGeometry* pointGeometry = pointFeature->GetGeometryRef();
                        if (pointGeometry && (areaInGdbCrs->Contains(pointGeometry)
                            || areaInGdbCrs->Intersects(pointGeometry))) matchedPointSymbols.insert(symbolIt.key());
                        OGRFeature::DestroyFeature(pointFeature);
                        if (matchedPointSymbols.contains(symbolIt.key())) break;
                    }
                    gdbSource.layer->SetSpatialFilter(nullptr);
                    OGRGeometryFactory::destroyGeometry(areaInGdbCrs);
                    if (matchedPointSymbols.contains(symbolIt.key())) break;
                }
            }
        }

        sourceLayer->ResetReading();
        QSet<QString> processedSymbolIds;
        OGRFeature* sourceFeature = nullptr;
        while ((sourceFeature = sourceLayer->GetNextFeature()) != nullptr) {
            ++totalFeatures;
            bool matched = false;
            OGRGeometry* sourceGeometry = sourceFeature->GetGeometryRef();
            const OGRGeometry* geometryToCheck = sourceGeometry;
            OGRGeometry* ownedSymbolGeometry = nullptr;
            QString symbolId;
            bool usesSymbolGroup = false;
            if (actualIsLine && symbolIdIndex >= 0
                && sourceFeature->IsFieldSetAndNotNull(symbolIdIndex)) {
                symbolId = QString::fromUtf8(sourceFeature->GetFieldAsString(symbolIdIndex)).trimmed();
                usesSymbolGroup = !symbolId.isEmpty() && combinedSymbolGeometries.contains(symbolId);
                if (usesSymbolGroup) {
                    if (processedSymbolIds.contains(symbolId)) {
                        OGRFeature::DestroyFeature(sourceFeature);
                        continue;
                    }
                    processedSymbolIds.insert(symbolId);
                    ownedSymbolGeometry = ogrGeometryFromQgs(combinedSymbolGeometries.value(symbolId));
                    if (!ownedSymbolGeometry) {
                        outputReady = false;
                        errorMessage = QStringLiteral("无法合并 symbol_id=%1 的符号组成线：%2")
                            .arg(symbolId, sourcePath);
                    } else {
                        geometryToCheck = ownedSymbolGeometry;
                        if (matchedPointSymbols.contains(symbolId)) matched = true;
                    }
                }
            }
            if (outputReady && geometryToCheck) {
                for (GdbSource& gdbSource : gdbSources) {
                    if (matched) break;
                    OGRGeometry* compareGeometry = geometryToCheck->clone();
                    if (gdbSource.transform && compareGeometry->transform(gdbSource.transform) != OGRERR_NONE) {
                        OGRGeometryFactory::destroyGeometry(compareGeometry);
                        outputReady = false;
                        errorMessage = usesSymbolGroup
                            ? QStringLiteral("符号整体坐标转换失败：%1，symbol_id=%2").arg(sourcePath, symbolId)
                            : QStringLiteral("实际要素坐标转换失败：%1，FID=%2")
                                .arg(sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                        break;
                    }
                    const double metersPerUnit = gdbSource.spatialRef->GetLinearUnits();
                    const double bufferDistance = ((bufferMillimeters / 1000.0)
                        * static_cast<double>(scaleDenominator)) / metersPerUnit;
                    OGRGeometry* bufferGeometry = compareGeometry->Buffer(bufferDistance, 8);
                    OGRGeometryFactory::destroyGeometry(compareGeometry);
                    if (!bufferGeometry) {
                        outputReady = false;
                        errorMessage = usesSymbolGroup
                            ? QStringLiteral("符号整体无法构建 %1 毫米缓冲区：%2，symbol_id=%3")
                                .arg(bufferText, sourcePath, symbolId)
                            : QStringLiteral("实际要素无法构建 %1 毫米缓冲区：%2，FID=%3")
                                .arg(bufferText, sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                        break;
                    }

                    gdbSource.layer->SetSpatialFilter(bufferGeometry);
                    gdbSource.layer->ResetReading();
                    OGRFeature* gdbFeature = nullptr;
                    while ((gdbFeature = gdbSource.layer->GetNextFeature()) != nullptr) {
                        OGRGeometry* gdbGeometry = gdbFeature->GetGeometryRef();
                        if (gdbGeometry && bufferGeometry->Intersects(gdbGeometry)) matched = true;
                        OGRFeature::DestroyFeature(gdbFeature);
                        if (matched) break;
                    }
                    gdbSource.layer->SetSpatialFilter(nullptr);
                    OGRGeometryFactory::destroyGeometry(bufferGeometry);
                    if (matched || !outputReady) break;
                }
            }

            if (outputReady && !matched) {
                ++unmatchedFeatures;
                OGRFeature* errorFeature = OGRFeature::CreateFeature(outputLayer->GetLayerDefn());
                if (!errorFeature || errorFeature->SetFrom(sourceFeature, TRUE) != OGRERR_NONE ||
                    (geometryToCheck && errorFeature->SetGeometry(geometryToCheck) != OGRERR_NONE)) {
                    outputReady = false;
                    errorMessage = usesSymbolGroup
                        ? QStringLiteral("写入合并符号失败：%1，symbol_id=%2").arg(sourcePath, symbolId)
                        : QStringLiteral("复制未匹配实际要素失败：%1，FID=%2")
                            .arg(sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                } else {
                    const QString description = usesSymbolGroup
                        ? QStringLiteral("符号面及符号整体 %1 毫米缓冲范围内均未找到对应 GDB 要素").arg(bufferText)
                        : QStringLiteral("实际图层 %1 毫米范围内未找到对应 GDB 要素").arg(bufferText);
                    errorFeature->SetField(outputDescriptionIndex, description.toUtf8().constData());
                    if (outputLayer->CreateFeature(errorFeature) != OGRERR_NONE) {
                        outputReady = false;
                        errorMessage = QStringLiteral("写入未匹配实际要素失败：%1，FID=%2")
                            .arg(sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                    }
                }
                if (errorFeature) OGRFeature::DestroyFeature(errorFeature);
            }
            if (ownedSymbolGeometry) OGRGeometryFactory::destroyGeometry(ownedSymbolGeometry);
            OGRFeature::DestroyFeature(sourceFeature);
            if (!outputReady) break;
        }
    }

    for (GdbSource& gdbSource : gdbSources) gdbSource.layer->SetSpatialFilter(nullptr);
    GDALClose(outputDataset);
    closeGdbSources();
    GDALClose(sourceDataset);
    if (!outputReady) {
        outputDriver->Delete(outputPathUtf8.constData());
        return false;
    }
    if (unmatchedFeatures == 0) outputDriver->Delete(outputPathUtf8.constData());
    return true;
}

static bool writeAnnotationPrecheckErrors(const QString& sourcePath, const QString& outputPath,
                                          int& totalFeatures, int& errorFeatures, int& typeErrorFeatures,
                                          int& fontErrorFeatures,
                                          QString& errorMessage, QStringList& warnings)
{
    GDALAllRegister();
    GDALDataset* sourceDataset = static_cast<GDALDataset*>(GDALOpenEx(
        sourcePath.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY, nullptr, nullptr, nullptr));
    if (!sourceDataset) {
        errorMessage = QStringLiteral("无法打开注记图层：%1").arg(sourcePath);
        return false;
    }

    OGRLayer* sourceLayer = sourceDataset->GetLayer(0);
    if (!sourceLayer || !sourceLayer->GetLayerDefn()) {
        errorMessage = QStringLiteral("无法读取注记图层结构：%1").arg(sourcePath);
        GDALClose(sourceDataset);
        return false;
    }

    OGRFeatureDefn* sourceDefinition = sourceLayer->GetLayerDefn();
    const int elementTypeIndex = findFieldIndexCaseInsensitive(sourceDefinition, QStringLiteral("element_ty"));
    if (elementTypeIndex < 0) {
        errorMessage = QStringLiteral("图层缺少 element_ty 字段：%1").arg(sourcePath);
        GDALClose(sourceDataset);
        return false;
    }

    const int fontFamilyIndex = findFieldIndexCaseInsensitive(sourceDefinition, QStringLiteral("font_family"));
    const int fontColorIndex = findFieldIndexCaseInsensitive(sourceDefinition, QStringLiteral("font_color"));
    const bool canCheckFontStyle = fontFamilyIndex >= 0 && fontColorIndex >= 0;
    if (fontFamilyIndex < 0) warnings << QStringLiteral("缺少 font_family 字段，已跳过该图层字体检查");
    if (fontColorIndex < 0) warnings << QStringLiteral("缺少 font_color 字段，已跳过该图层字体检查");

    // 第一遍统计 text 要素的字体组合。只在主流组合超过有效样式的一半时自动判异。
    QHash<QString, int> styleCounts;
    int validStyleFeatureCount = 0;
    if (canCheckFontStyle) {
        sourceLayer->ResetReading();
        OGRFeature* sourceFeature = nullptr;
        while ((sourceFeature = sourceLayer->GetNextFeature()) != nullptr) {
            QString typeValue;
            if (sourceFeature->IsFieldSetAndNotNull(elementTypeIndex)) {
                typeValue = QString::fromUtf8(sourceFeature->GetFieldAsString(elementTypeIndex)).trimmed();
            }
            if (typeValue.compare(QStringLiteral("text"), Qt::CaseInsensitive) == 0 &&
                sourceFeature->IsFieldSetAndNotNull(fontFamilyIndex) &&
                sourceFeature->IsFieldSetAndNotNull(fontColorIndex)) {
                const QString family = normalizedFontFamily(QString::fromUtf8(sourceFeature->GetFieldAsString(fontFamilyIndex)));
                const QString color = normalizedFontColor(QString::fromUtf8(sourceFeature->GetFieldAsString(fontColorIndex)));
                if (!family.isEmpty() && !color.isEmpty()) {
                    ++styleCounts[family + QChar(0x1f) + color];
                    ++validStyleFeatureCount;
                }
            }
            OGRFeature::DestroyFeature(sourceFeature);
        }
    }

    QString dominantStyle;
    int dominantStyleCount = 0;
    for (auto it = styleCounts.constBegin(); it != styleCounts.constEnd(); ++it) {
        if (it.value() > dominantStyleCount) {
            dominantStyle = it.key();
            dominantStyleCount = it.value();
        }
    }
    const bool hasDominantStyle = validStyleFeatureCount > 0 && dominantStyleCount * 2 > validStyleFeatureCount;
    if (canCheckFontStyle && validStyleFeatureCount == 0) {
        warnings << QStringLiteral("font_family/font_color 没有有效值，已跳过该图层字体混用判定");
    } else if (canCheckFontStyle && styleCounts.size() > 1 && !hasDominantStyle) {
        warnings << QStringLiteral("字体样式没有超过半数的主流组合，无法自动判定该图层字体混用");
    }

    GDALDriver* outputDriver = GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    if (!outputDriver) {
        errorMessage = QStringLiteral("无法获取 Shapefile 输出驱动");
        GDALClose(sourceDataset);
        return false;
    }

    QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.absolutePath());
    QByteArray outputPathUtf8 = outputPath.toUtf8();
    char** datasetOptions = nullptr;
    GDALDataset* outputDataset = outputDriver->Create(outputPathUtf8.constData(), 0, 0, 0, GDT_Unknown, datasetOptions);
    if (!outputDataset) {
        errorMessage = QStringLiteral("无法创建错误结果图层：%1").arg(outputPath);
        GDALClose(sourceDataset);
        return false;
    }

    const OGRwkbGeometryType geometryType = sourceLayer->GetGeomType();
    char** layerOptions = nullptr;
    layerOptions = CSLSetNameValue(layerOptions, "ENCODING", "UTF-8");
    OGRLayer* outputLayer = outputDataset->CreateLayer(
        "annotation_type_errors", sourceLayer->GetSpatialRef(), geometryType, layerOptions);
    CSLDestroy(layerOptions);
    bool outputReady = outputLayer != nullptr;
    if (!outputReady) {
        errorMessage = QStringLiteral("无法创建输出图层：%1").arg(outputPath);
    }

    for (int i = 0; outputReady && i < sourceDefinition->GetFieldCount(); ++i) {
        OGRFieldDefn field(sourceDefinition->GetFieldDefn(i));
        if (outputLayer->CreateField(&field) != OGRERR_NONE) {
            outputReady = false;
            errorMessage = QStringLiteral("复制原始属性字段失败：%1").arg(
                QString::fromUtf8(sourceDefinition->GetFieldDefn(i)->GetNameRef()));
        }
    }

    const QString descriptionField = uniqueDescriptionFieldName(sourceDefinition);
    if (outputReady) {
        OGRFieldDefn descriptionDef(descriptionField.toUtf8().constData(), OFTString);
        descriptionDef.SetWidth(80);
        if (outputLayer->CreateField(&descriptionDef) != OGRERR_NONE) {
            outputReady = false;
            errorMessage = QStringLiteral("无法创建错误说明字段");
        }
    }

    if (outputReady) {
        const int outputDescriptionIndex = outputLayer->GetLayerDefn()->GetFieldIndex(descriptionField.toUtf8().constData());
        sourceLayer->ResetReading();
        OGRFeature* sourceFeature = nullptr;
        while ((sourceFeature = sourceLayer->GetNextFeature()) != nullptr) {
            ++totalFeatures;
            QString typeValue;
            if (sourceFeature->IsFieldSetAndNotNull(elementTypeIndex)) {
                typeValue = QString::fromUtf8(sourceFeature->GetFieldAsString(elementTypeIndex)).trimmed();
            }
            const bool isTextFeature = typeValue.compare(QStringLiteral("text"), Qt::CaseInsensitive) == 0;
            const bool typeMismatch = !isTextFeature;
            bool fontMismatch = false;
            if (canCheckFontStyle && hasDominantStyle && isTextFeature) {
                if (!sourceFeature->IsFieldSetAndNotNull(fontFamilyIndex) ||
                    !sourceFeature->IsFieldSetAndNotNull(fontColorIndex)) {
                    fontMismatch = true;
                } else {
                    const QString family = normalizedFontFamily(QString::fromUtf8(sourceFeature->GetFieldAsString(fontFamilyIndex)));
                    const QString color = normalizedFontColor(QString::fromUtf8(sourceFeature->GetFieldAsString(fontColorIndex)));
                    fontMismatch = family.isEmpty() || color.isEmpty() ||
                        family + QChar(0x1f) + color != dominantStyle;
                }
            }

            if (typeMismatch || fontMismatch) {
                ++errorFeatures;
                if (typeMismatch) ++typeErrorFeatures;
                if (fontMismatch) ++fontErrorFeatures;
                OGRFeature* errorFeature = OGRFeature::CreateFeature(outputLayer->GetLayerDefn());
                if (!errorFeature || errorFeature->SetFrom(sourceFeature, TRUE) != OGRERR_NONE ||
                    (sourceFeature->GetGeometryRef() && errorFeature->SetGeometry(sourceFeature->GetGeometryRef()) != OGRERR_NONE)) {
                    outputReady = false;
                    errorMessage = QStringLiteral("复制异常要素失败：%1，FID=%2")
                        .arg(sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                } else {
                    QStringList descriptions;
                    if (typeMismatch) descriptions << QStringLiteral("注记层混入了其他要素");
                    if (fontMismatch) descriptions << QStringLiteral("注记层存在字体混用");
                    const QByteArray descriptionUtf8 = descriptions.join(QStringLiteral("；")).toUtf8();
                    errorFeature->SetField(outputDescriptionIndex, descriptionUtf8.constData());
                    if (outputLayer->CreateFeature(errorFeature) != OGRERR_NONE) {
                        outputReady = false;
                        errorMessage = QStringLiteral("写入异常要素失败：%1，FID=%2")
                            .arg(sourcePath).arg(static_cast<qlonglong>(sourceFeature->GetFID()));
                    }
                }
                if (errorFeature) OGRFeature::DestroyFeature(errorFeature);
            }
            OGRFeature::DestroyFeature(sourceFeature);
            if (!outputReady) break;
        }
    }

    GDALClose(outputDataset);
    GDALClose(sourceDataset);
    if (!outputReady) {
        outputDriver->Delete(outputPathUtf8.constData());
        return false;
    }
    if (errorFeatures == 0) {
        outputDriver->Delete(outputPathUtf8.constData());
    }
    return true;
}

// 仅将最终错误成果归档；临时匹配层仍保留在错误结果根目录，供 213 等交叉验证使用。
static bool isFinalErrorLayerFile(const QFileInfo& fileInfo)
{
    const QString suffix = fileInfo.suffix().toLower();
    if (suffix != QStringLiteral("shp") && suffix != QStringLiteral("geojson")) return false;

    const QString name = fileInfo.completeBaseName().toLower();
    if (!name.contains(QStringLiteral("error")) && !name.contains(QStringLiteral("错误"))) return false;

    static const QStringList temporaryMarkers {
        QStringLiteral("temp"), QStringLiteral("临时"), QStringLiteral("sample_line"),
        QStringLiteral("parsed_band"), QStringLiteral("辅助"),
        QStringLiteral("jump_endpoints")
    };
    for (const QString& marker : temporaryMarkers) {
        if (name.contains(marker)) return false;
    }
    return true;
}

struct ErrorLayerFileState {
    qint64 modifiedMsecs = -1;
    qint64 size = -1;
};

static QHash<QString, ErrorLayerFileState> snapshotFinalErrorLayers(const QString& errorResultDir)
{
    QHash<QString, ErrorLayerFileState> snapshot;
    const QDir dir(errorResultDir);
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fileInfo : files) {
        if (!isFinalErrorLayerFile(fileInfo)) continue;
        ErrorLayerFileState state;
        state.modifiedMsecs = fileInfo.lastModified().toMSecsSinceEpoch();
        state.size = fileInfo.size();
        snapshot.insert(QDir::cleanPath(fileInfo.absoluteFilePath()), state);
    }
    return snapshot;
}

static QStringList changedFinalErrorLayers(
    const QHash<QString, ErrorLayerFileState>& before,
    const QHash<QString, ErrorLayerFileState>& after)
{
    QStringList changed;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
        const auto previous = before.constFind(it.key());
        if (previous == before.constEnd()
            || previous->modifiedMsecs != it.value().modifiedMsecs
            || previous->size != it.value().size) {
            changed.append(it.key());
        }
    }
    return changed;
}

static void appendErrorLayerPaths(CheckItemResult& result, const QStringList& paths)
{
    if (paths.isEmpty()) return;
    QJsonArray outputPaths = result.info.value(QStringLiteral("error_layer_paths")).toArray();
    QSet<QString> knownPaths;
    for (const QJsonValue& value : outputPaths) knownPaths.insert(value.toString());
    for (const QString& path : paths) {
        if (!knownPaths.contains(path)) {
            outputPaths.append(path);
            knownPaths.insert(path);
        }
    }
    result.info[QStringLiteral("error_layer_paths")] = outputPaths;
}

static QJsonValue replaceArchivedLayerPaths(
    const QJsonValue& value, const QHash<QString, QString>& replacements)
{
    if (value.isString()) return replacements.value(value.toString(), value.toString());
    if (value.isArray()) {
        QJsonArray updated;
        for (const QJsonValue& child : value.toArray()) {
            updated.append(replaceArchivedLayerPaths(child, replacements));
        }
        return updated;
    }
    if (value.isObject()) {
        QJsonObject updated;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            updated.insert(it.key(), replaceArchivedLayerPaths(it.value(), replacements));
        }
        return updated;
    }
    return value;
}

static QString checkItemSeverityKey(const QString& layerName, const QString& checkItemName)
{
    return layerName.trimmed() + QChar(0x1f) + checkItemName.trimmed();
}

// 123 的任务节点来自 XML 文件名，例如“要素表示冲突检查_123-乡镇驻地”。
// 后缀是待检标准图层名；关联图层仍由 XML 规则和 CSV 分别映射。
static QString conflictTargetFromTaskLayer(const QString& taskLayerName)
{
    const QString prefix = QStringLiteral("要素表示冲突检查_123");
    if (!taskLayerName.startsWith(prefix)) return taskLayerName;
    QString target = taskLayerName.mid(prefix.size()).trimmed();
    while (!target.isEmpty() && (target.at(0) == QChar('-') || target.at(0) == QChar('_')))
        target.remove(0, 1);
    return target.trimmed();
}

static QString densityTargetFromTaskLayer(const QString& taskLayerName)
{
    const QString prefix = QStringLiteral("要素密度检查_121");
    if (!taskLayerName.startsWith(prefix)) return taskLayerName;
    QString target = taskLayerName.mid(prefix.size()).trimmed();
    while (!target.isEmpty() && (target.at(0) == QChar('-') || target.at(0) == QChar('_')))
        target.remove(0, 1);
    return target.trimmed();
}

static QString springTargetFromTaskLayer(const QString& taskLayerName)
{
    const QString prefix = QStringLiteral("关系检查_113");
    if (!taskLayerName.startsWith(prefix)) return QString();
    QString target = taskLayerName.mid(prefix.size()).trimmed();
    while (!target.isEmpty() && (target.at(0) == QChar('-') || target.at(0) == QChar('_')))
        target.remove(0, 1);
    target = target.trimmed();
    return target == QStringLiteral("泉") ? target : QString();
}

QJsonObject map_check_task_manager::loadPhaseOneConfigSnapshot()
{
    QSettings settings(QStringLiteral("GSZJPlugins"), QStringLiteral("map_quality_check_tools_test"));
    const QJsonDocument doc = QJsonDocument::fromJson(
        settings.value(QStringLiteral("phase_one_config_snapshot")).toByteArray());
    return doc.isObject() ? doc.object() : QJsonObject();
}

void map_check_task_manager::savePhaseOneConfigSnapshot() const
{
    QStringList shpDirs, gdbDirs, csvPaths;
    for (int row = 0; row < ui->tableWidget_GeopdfPath->rowCount(); ++row) {
        const AlgorithmFilePaths paths = const_cast<map_check_task_manager*>(this)->getFilePathsByRow(row);
        if (!paths.shpPath.trimmed().isEmpty()) shpDirs << paths.shpPath.trimmed();
        if (!paths.gdbShpPath.trimmed().isEmpty()) gdbDirs << paths.gdbShpPath.trimmed();
        if (!paths.csvPath.trimmed().isEmpty() && !csvPaths.contains(paths.csvPath.trimmed())) csvPaths << paths.csvPath.trimmed();
    }
    QJsonObject snapshot;
    snapshot[QStringLiteral("xml_rule_path")] = m_xmlPaths.isEmpty() ? ui->lineEdit_xmlfiles->text().trimmed() : m_xmlPaths.first();
    snapshot[QStringLiteral("actual_dir")] = commonSnapshotDir(shpDirs);
    snapshot[QStringLiteral("gdb_dir")] = commonSnapshotDir(gdbDirs);
    snapshot[QStringLiteral("csv_path")] = csvPaths.size() == 1 ? csvPaths.first() : QString();
    snapshot[QStringLiteral("output_dir")] = ui->lineEdit_OutputLogPath->text().trimmed();
    snapshot[QStringLiteral("saved_at")] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QSettings settings(QStringLiteral("GSZJPlugins"), QStringLiteral("map_quality_check_tools_test"));
    settings.setValue(QStringLiteral("phase_one_config_snapshot"), QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
}

map_check_task_manager::map_check_task_manager(QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::map_check_task_manager)
    , m_globalStartTime(QDateTime::currentDateTime())
    , m_globalEndTime(QDateTime::currentDateTime())
{
    ui->setupUi(this);

    // The designer layout places all task content in a single group box.  The
    // plugin host can constrain this dialog's height, so make that content
    // scrollable and keep the progress/action row visible at the bottom.
    auto* content = ui->groupBox;
    ui->gridLayout_5->removeWidget(content);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setObjectName(QStringLiteral("scrollArea_taskContent"));
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(true);
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    content->setMinimumSize(0, 0);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    scrollArea->setWidget(content);
    ui->gridLayout_5->addWidget(scrollArea, 0, 0, 1, 4);
    ui->gridLayout_5->setRowStretch(0, 1);

    // Keep the configuration sections compact so the task tree can show more
    // rows without requiring the user to scroll the whole dialog.
    ui->gridLayout_2->removeWidget(ui->label);
    ui->gridLayout_2->removeWidget(ui->lineEdit_xmlfiles);
    ui->gridLayout_2->removeWidget(ui->pushButton_loadxmlfiles);
    ui->gridLayout_2->removeWidget(ui->pushButton_loadxmlfile);
    ui->gridLayout_2->addWidget(ui->label, 0, 0);
    ui->gridLayout_2->addWidget(ui->lineEdit_xmlfiles, 0, 1);
    ui->gridLayout_2->addWidget(ui->pushButton_loadxmlfiles, 0, 2);
    ui->gridLayout_2->addWidget(ui->pushButton_loadxmlfile, 0, 3);
    ui->gridLayout_2->setColumnStretch(1, 1);
    ui->groupBox_4->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    ui->gridLayout_6->removeWidget(ui->label_2);
    ui->gridLayout_6->removeWidget(ui->lineEdit);
    ui->gridLayout_6->removeWidget(ui->label_ShpPath_3);
    ui->gridLayout_6->removeWidget(ui->lineEdit_OutputLogPath);
    ui->gridLayout_6->removeWidget(ui->Button_SaveLogFilePath);
    ui->gridLayout_6->addWidget(ui->label_2, 0, 0);
    ui->gridLayout_6->addWidget(ui->lineEdit, 0, 1);
    ui->gridLayout_6->addWidget(ui->label_ShpPath_3, 1, 0);
    ui->gridLayout_6->addWidget(ui->lineEdit_OutputLogPath, 1, 1, 1, 2);
    ui->gridLayout_6->addWidget(ui->Button_SaveLogFilePath, 1, 3);
    ui->gridLayout_6->setColumnStretch(2, 1);
    ui->groupBox_InputData_3->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // Give the task list most of the available vertical space when the dialog
    // is enlarged.  The map list remains resizable, but grows more slowly.
    ui->treeWidget_xmlTaskData->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->groupBox_2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->gridLayout->setRowStretch(0, 1);
    ui->gridLayout_3->setRowStretch(2, 2);

    ui->tableWidget_GeopdfPath->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->groupBox_InputData->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->gridLayout_4->setRowStretch(1, 1);
    ui->gridLayout_3->setRowStretch(0, 1);
    
    initAlgorithmMap(); // 初始化算法映射

     // === 新增：初始化成员变量 ===
    m_currentJsonFilePath.clear();
    m_saveDir.clear();
    m_originalTaskTreeData.clear();

    ui->lineEdit->setFixedWidth(150);
    QString userName = ui->lineEdit->text().trimmed();
    if (userName.isEmpty()) {
        userName = "operator";
    }

    // 初始化表格控件：地图路径、地图SHP、GDB_SHP、标准图例、图面配置
    ui->tableWidget_GeopdfPath->setColumnCount(5);
    QStringList headers;
    headers << "地图路径" << "地图shp文件夹" << "GDB shp文件夹" << "图例文件夹" << "图面配置选择";
    ui->tableWidget_GeopdfPath->setHorizontalHeaderLabels(headers);
    // 自适应列宽
    ui->tableWidget_GeopdfPath->horizontalHeader()->setStretchLastSection(true);

    // 设置表格“即使无数据行，也显示至少一行高度的空白区域”（避免表格高度为0）
    ui->tableWidget_GeopdfPath->setMinimumHeight(120);  // 保证地图路径列表具备可用的初始显示高度
    // 或设置垂直表头的“默认行高”，确保空白区域有点击范围
    ui->tableWidget_GeopdfPath->verticalHeader()->setDefaultSectionSize(30);

    // Designer 中地图列表下方右侧留有空位（截图红圈处），放置独立的数据预检查入口。
    auto* precheckButton = new QPushButton(QStringLiteral("数据预检查"), ui->groupBox_InputData);
    precheckButton->setObjectName(QStringLiteral("pushButton_DataPrecheck"));
    precheckButton->setToolTip(QStringLiteral("检查实际图层与对应 GDB 的空间关系，以及实际注记层要素类型和字体样式"));
    ui->gridLayout_4->addWidget(precheckButton, 2, 3, 1, 2, Qt::AlignRight);
    connect(precheckButton, &QPushButton::clicked,
            this, &map_check_task_manager::on_pushButton_DataPrecheck_clicked);

    // 初始化质检方案treeWidget
    ui->treeWidget_xmlTaskData->setColumnCount(4);
    // 增加默认可视高度，让折叠状态下也能一次查看更多子任务。
    ui->treeWidget_xmlTaskData->setMinimumHeight(280);
    QStringList treeHeaders;
    treeHeaders << "子任务列表" << "比例尺" << "质检项名称" << "完成状态";
    ui->treeWidget_xmlTaskData->setHeaderLabels(treeHeaders);

    ui->label_log->setText(tr("准备就绪"));
    // 子线程信号在开始执行时再连接（见 on_pushButton_Start_clicked）
}

map_check_task_manager::~map_check_task_manager()
{
    if (m_algorithmThread) {
        m_algorithmThread->requestStop();
        m_algorithmThread->wait();
        delete m_algorithmThread;
        m_algorithmThread = nullptr;
    }
    delete ui;
}

//保存原始任务树结构
void map_check_task_manager::saveOriginalTaskTree()
{
    m_originalTaskTreeData.clear();

    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);

    // 遍历所有节点
    QTreeWidgetItemIterator it(ui->treeWidget_xmlTaskData);
    while (*it) {
        QTreeWidgetItem* item = *it;
        int depth = 0;
        QTreeWidgetItem* parent = item->parent();
        while (parent) {
            depth++;
            parent = parent->parent();
        }

        out << depth
            << item->text(0)
            << item->text(1)
            << item->text(2)
            << item->text(3);
        ++it;
    }

    m_originalTaskTreeData = data;
}

//恢复任务树结构
void map_check_task_manager::restoreOriginalTaskTree()
{
    if (m_originalTaskTreeData.isEmpty()) return;

    ui->treeWidget_xmlTaskData->clear();
    QDataStream in(&m_originalTaskTreeData, QIODevice::ReadOnly);

    QList<QTreeWidgetItem*> stack; // 用栈保存层级关系
    while (!in.atEnd()) {
        int depth;
        QString col0, col1, col2, col3;
        in >> depth >> col0 >> col1 >> col2 >> col3;

        QTreeWidgetItem* newItem = new QTreeWidgetItem(QStringList() << col0 << col1 << col2 << col3);

        // 根据层级深度恢复结构
        if (depth == 0) {
            ui->treeWidget_xmlTaskData->addTopLevelItem(newItem);
            stack = { newItem };
        }
        else {
            while (stack.size() > depth)
                stack.removeLast();
            if (!stack.isEmpty())
                stack.last()->addChild(newItem);
            stack.append(newItem);
        }
    }

    // saveOriginalTaskTree() stores the task tree's text and hierarchy, but not
    // its QIcon data. Rebuild the default status icon after restoring so the
    // status column does not become blank when the tree is filtered by scale.
    QTreeWidgetItemIterator restoredItem(ui->treeWidget_xmlTaskData);
    while (*restoredItem) {
        QTreeWidgetItem* item = *restoredItem;
        if (!item->text(2).trimmed().isEmpty()) {
            setCompletionStatusIcon(item, CompletionStatus::NotChecked);
        }
        ++restoredItem;
    }

    // 任务树重建后保持默认折叠，由用户按需展开。
    ui->treeWidget_xmlTaskData->collapseAll();
}

//依据比例尺刷新任务树
void map_check_task_manager::refreshTaskTreeByScale(int checkedScale)
{
    if (!ui->treeWidget_xmlTaskData) return;

    // 比例尺检查结果可能已经更新到当前树上。按比例尺重建树时要保留
    // 这些状态，否则 restoreOriginalTaskTree() 会把成功状态重置成红叉。
    QHash<QString, CompletionStatus> savedStatuses;
    QTreeWidgetItemIterator currentItem(ui->treeWidget_xmlTaskData);
    while (*currentItem) {
        QTreeWidgetItem* item = *currentItem;
        if (!item->text(2).trimmed().isEmpty()) {
            QTreeWidgetItem* layerNode = item;
            while (layerNode->parent()) layerNode = layerNode->parent();
            const QString key = layerNode->text(0) + QChar(0x1f) + item->text(2).trimmed();
            bool ok = false;
            const int statusValue = item->data(3, Qt::UserRole).toInt(&ok);
            if (ok && statusValue >= static_cast<int>(CompletionStatus::NotChecked) &&
                statusValue <= static_cast<int>(CompletionStatus::Passed)) {
                savedStatuses.insert(key, static_cast<CompletionStatus>(statusValue));
            }
        }
        ++currentItem;
    }

    // 每幅图都从完整方案筛选，不能在上一幅图（或上一次运行）的筛选结果上继续筛选。
    restoreOriginalTaskTree();

    // 将当前任务树完整备份一次
    QList<QTreeWidgetItem*> allTopItems;
    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        allTopItems.append(ui->treeWidget_xmlTaskData->topLevelItem(i)->clone());
    }

    ui->treeWidget_xmlTaskData->clear();

    // 遍历顶层任务节点
    for (auto* taskItem : allTopItems) {
        QString taskName = taskItem->text(0);

        // === 始终保留 图名、比例尺、投影 三个任务 ===
        if (taskName == "图名" || taskName == "比例尺" || taskName == "投影") {
            ui->treeWidget_xmlTaskData->addTopLevelItem(taskItem);
            continue;
        }

        // 创建新的任务节点，用于存储匹配比例尺的部分
        QTreeWidgetItem* newTaskItem = new QTreeWidgetItem();
        newTaskItem->setText(0, taskName);

        // 遍历比例尺节点
        for (int j = 0; j < taskItem->childCount(); ++j) {
            QTreeWidgetItem* scaleItem = taskItem->child(j);
            QString scaleStr = scaleItem->text(1).trimmed();
            bool ok = false;
            int scaleVal = scaleStr.toInt(&ok);

            // === 条件1：比例尺为空 ===
            if (scaleStr.isEmpty()) {
                newTaskItem->addChild(scaleItem->clone());
                continue;
            }

            // === 条件2：比例尺匹配 ===
            if (ok && scaleVal == checkedScale) {
                newTaskItem->addChild(scaleItem->clone());
            }
        }

        // 若该任务下有匹配比例尺节点，则保留该任务
        if (newTaskItem->childCount() > 0)
            ui->treeWidget_xmlTaskData->addTopLevelItem(newTaskItem);
        else
            delete newTaskItem; // 无匹配则丢弃
        delete taskItem;
    }

    // 把筛选后仍存在的质检项状态恢复回来（例如比例尺检查的成功状态）。
    QTreeWidgetItemIterator filteredItem(ui->treeWidget_xmlTaskData);
    while (*filteredItem) {
        QTreeWidgetItem* item = *filteredItem;
        if (!item->text(2).trimmed().isEmpty()) {
            QTreeWidgetItem* layerNode = item;
            while (layerNode->parent()) layerNode = layerNode->parent();
            const QString key = layerNode->text(0) + QChar(0x1f) + item->text(2).trimmed();
            const auto statusIt = savedStatuses.constFind(key);
            if (statusIt != savedStatuses.constEnd()) {
                setCompletionStatusIcon(item, statusIt.value());
            }
        }
        ++filteredItem;
    }

    // 比例尺筛选会重建树，重建后仍保持默认折叠。
    ui->treeWidget_xmlTaskData->collapseAll();
    m_taskTreeFiltered = true;

}

QTreeWidgetItem* map_check_task_manager::findCheckItemByName(const QString& checkItemName)
{
    auto findRecursive = [&](auto&& self, QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) return nullptr;
        // 尝试当前节点（部分树结构可能直接在第3列存放名称）
        if (node->text(2).trimmed() == checkItemName) return node;
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (!child) continue;
            if (child->text(2).trimmed() == checkItemName) return child;
            if (QTreeWidgetItem* got = self(self, child)) return got;
        }
        return nullptr;
    };

    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        QTreeWidgetItem* root = ui->treeWidget_xmlTaskData->topLevelItem(i);
        if (QTreeWidgetItem* found = findRecursive(findRecursive, root)) return found;
    }
    return nullptr;
}

// 新增：根据图层名称和质检项名称精确查找质检项
QTreeWidgetItem* map_check_task_manager::findCheckItemByLayerAndName(const QString& layerName, const QString& checkItemName)
{
    // 先找到对应的图层（顶层节点）
    QTreeWidgetItem* layerNode = nullptr;
    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        QTreeWidgetItem* topItem = ui->treeWidget_xmlTaskData->topLevelItem(i);
        if (topItem && topItem->text(0).trimmed() == layerName) {
            layerNode = topItem;
            break;
        }
    }
    
    if (!layerNode) return nullptr;
    
    // 在该图层下递归查找质检项
    auto findRecursive = [&](auto&& self, QTreeWidgetItem* node) -> QTreeWidgetItem* {
        if (!node) return nullptr;
        // 检查当前节点是否是目标质检项（第2列存放质检项名称）
        if (node->text(2).trimmed() == checkItemName) return node;
        // 递归查找子节点
        for (int i = 0; i < node->childCount(); ++i) {
            QTreeWidgetItem* child = node->child(i);
            if (!child) continue;
            if (child->text(2).trimmed() == checkItemName) return child;
            if (QTreeWidgetItem* got = self(self, child)) return got;
        }
        return nullptr;
    };
    
    return findRecursive(findRecursive, layerNode);
}

// ====== 新增：构建任务树快照（主线程调用）
QVector<map_check_task_manager::LayerTaskSnapshot> map_check_task_manager::buildTaskSnapshot() const
{
    QVector<LayerTaskSnapshot> snapshot;
    for (int taskIdx = 0; taskIdx < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++taskIdx) {
        QTreeWidgetItem* layerNode = ui->treeWidget_xmlTaskData->topLevelItem(taskIdx);
        if (!layerNode) continue;
        LayerTaskSnapshot lt;
        lt.layerName = layerNode->text(0);
        QSet<QString> seen;
        for (int s = 0; s < layerNode->childCount(); ++s) {
            QTreeWidgetItem* childNode = layerNode->child(s);
            if (!childNode) continue;
            for (int c = 0; c < childNode->childCount(); ++c) {
                QTreeWidgetItem* item = childNode->child(c);
                if (!item) continue;
                const QString name = item->text(2).trimmed();
                if (name.isEmpty()) continue;
                if (!seen.contains(name)) {
                    lt.checkItemNames.append(name);
                    lt.checkItemSeverities.insert(name, m_checkItemSeverity.value(
                        checkItemSeverityKey(lt.layerName, name), QStringLiteral("未配置")));
                    seen.insert(name);
                }
            }
            // 某些结构可能只有两层（layer->item），兼容如下：
            const QString maybeName = childNode->text(2).trimmed();
            if (!maybeName.isEmpty() && !seen.contains(maybeName)) {
                lt.checkItemNames.append(maybeName);
                lt.checkItemSeverities.insert(maybeName, m_checkItemSeverity.value(
                    checkItemSeverityKey(lt.layerName, maybeName), QStringLiteral("未配置")));
                seen.insert(maybeName);
            }
        }
        snapshot.append(lt);
    }
    return snapshot;
}

// ====== 新增：构建表格行快照（主线程调用）
QVector<map_check_task_manager::MapRowSnapshot> map_check_task_manager::buildRowSnapshot() const
{
    QVector<MapRowSnapshot> rows;
    QTableWidget* table = ui->tableWidget_GeopdfPath;
    const int rowCount = table->rowCount();
    rows.reserve(rowCount);
    for (int row = 0; row < rowCount; ++row) {
        MapRowSnapshot mr;
        mr.paths = const_cast<map_check_task_manager*>(this)->getFilePathsByRow(row);
        QFileInfo pdfInfo(mr.paths.pdfPath);
        mr.mapName = pdfInfo.baseName();
        if (mr.mapName.isEmpty()) mr.mapName = QString("未命名地图%1").arg(row + 1);
        rows.append(mr);
    }
    return rows;
}

// QGIS 的 OGR 图层 URI 有时会附带 "|..." 选项；归档时只比较实际 shp 路径。
static QString localLayerSourcePath(const QString& source)
{
    const QString path = source.section(QLatin1Char('|'), 0, 0);
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// 该函数必须由 Qt 主线程调用。若宿主的 watchResultOutputs 已经加载了根目录
// 图层，切换其数据源以释放原 shp 的文件句柄；若尚未加载，则直接将归档后的
// 成果加入工程，避免监听事件因根目录文件已清理而漏加载。
static bool retargetOrLoadArchivedErrorLayer(
    const QString& sourcePath, const QString& destinationPath, const QString& displayName,
    bool loadIfAbsent)
{
    QgsProject* project = QgsProject::instance();
    if (!project) return false;

    const QString normalizedSource = localLayerSourcePath(sourcePath);
    bool foundLoadedLayer = false;
    bool retargetSucceeded = true;
    const auto layers = project->mapLayers();
    for (QgsMapLayer* mapLayer : layers) {
        QgsVectorLayer* vectorLayer = qobject_cast<QgsVectorLayer*>(mapLayer);
        if (!vectorLayer || localLayerSourcePath(vectorLayer->source()) != normalizedSource) continue;

        foundLoadedLayer = true;
        vectorLayer->setDataSource(destinationPath, vectorLayer->name(),
                                   vectorLayer->providerType(), false);
        if (vectorLayer->isValid() &&
            !placeQualityResultLayer(project, vectorLayer, destinationPath)) {
            qWarning() << "[错误图层归档] 无法将图层归入结果分组:" << destinationPath;
            vectorLayer->setDataSource(sourcePath, vectorLayer->name(),
                                       vectorLayer->providerType(), false);
            retargetSucceeded = false;
        }
        if (!vectorLayer->isValid()) {
            qWarning() << "[错误图层归档] 无法切换宿主图层数据源:"
                       << sourcePath << "->" << destinationPath;
            retargetSucceeded = false;
        }
    }
    if (foundLoadedLayer) return retargetSucceeded;
    if (!loadIfAbsent) return true;

    QgsVectorLayer* archivedLayer = new QgsVectorLayer(destinationPath, displayName,
                                                        QStringLiteral("ogr"));
    if (!archivedLayer->isValid()) {
        qWarning() << "[错误图层归档] 无法加载归档后的图层:" << destinationPath;
        delete archivedLayer;
        return false;
    }
    project->addMapLayer(archivedLayer, false);
    if (!placeQualityResultLayer(project, archivedLayer, destinationPath)) {
        qWarning() << "[错误图层归档] 无法将新加载图层归入结果分组:" << destinationPath;
        project->removeMapLayer(archivedLayer->id());
        return false;
    }
    return true;
}

static bool removeArchivedSourceFiles(const QStringList& sourceFilePaths)
{
    if (sourceFilePaths.isEmpty()) return true;

    bool removedAllFiles = true;
    // 附属文件先清理，主 shp 最后清理；任何失败都由调用方延迟重试。
    for (int i = 1; i < sourceFilePaths.size(); ++i) {
        const QString& sourcePath = sourceFilePaths.at(i);
        if (QFile::exists(sourcePath) && !QFile::remove(sourcePath)) {
            qWarning() << "[错误图层归档] 暂时无法清理根目录附属文件:" << sourcePath;
            removedAllFiles = false;
        }
    }
    const QString& primaryPath = sourceFilePaths.first();
    if (QFile::exists(primaryPath) && !QFile::remove(primaryPath)) {
        qWarning() << "[错误图层归档] 暂时无法清理根目录主文件:" << primaryPath;
        removedAllFiles = false;
    }
    return removedAllFiles;
}

// watchResultOutputs 可能在首次切换后才将根目录图层加入工程。延迟重试时先
// 再次重定向这些图层，随后清理残留文件，避免根目录留下不完整的 shp 组。
static void scheduleArchivedSourceCleanup(
    const QStringList& sourceFilePaths, const QString& sourcePath,
    const QString& destinationPath, const QString& displayName, int retriesRemaining)
{
    QCoreApplication* application = QCoreApplication::instance();
    if (!application || retriesRemaining <= 0) return;

    QTimer::singleShot(800, application,
        [sourceFilePaths, sourcePath, destinationPath, displayName, retriesRemaining]() {
            if (!retargetOrLoadArchivedErrorLayer(
                    sourcePath, destinationPath, displayName, false)) {
                qWarning() << "[错误图层归档] 延迟重试时图层重定向失败:" << sourcePath;
            }
            if (!removeArchivedSourceFiles(sourceFilePaths) && retriesRemaining > 1) {
                scheduleArchivedSourceCleanup(sourceFilePaths, sourcePath, destinationPath,
                                              displayName, retriesRemaining - 1);
            }
        });
}

int map_check_task_manager::archiveErrorLayersBySeverity(
    int row, const QString& errorResultDir,
    const QHash<QString, QString>& layerSeverities)
{
    QDir rootDir(errorResultDir);
    if (!rootDir.exists()) rootDir.mkpath(QStringLiteral("."));

    // 即使本图没有错误，也保持三个严重程度目录结构一致。
    const QStringList severityFolders {
        QStringLiteral("高严重程度"), QStringLiteral("中严重程度"), QStringLiteral("低严重程度")
    };
    for (const QString& severity : severityFolders) {
        rootDir.mkpath(severity);
    }

    QHash<QString, QString> replacements;
    int archivedCount = 0;
    int unconfiguredCount = 0;

    for (auto it = layerSeverities.constBegin(); it != layerSeverities.constEnd(); ++it) {
        const QString severityFolder = severityFolderName(it.value());
        if (severityFolder.isEmpty()) {
            ++unconfiguredCount;
            continue;
        }

        const QString sourcePath = QDir::cleanPath(it.key());
        const QFileInfo sourceInfo(sourcePath);
        if (!sourceInfo.exists() || !isFinalErrorLayerFile(sourceInfo)) continue;

        const QDir destinationDir(rootDir.filePath(severityFolder));
        const QString sourceBaseName = sourceInfo.completeBaseName();
        const QString suffix = sourceInfo.suffix();
        QString destinationBaseName = sourceBaseName;
        QString destinationPrimaryPath = destinationDir.filePath(destinationBaseName + QLatin1Char('.') + suffix);
        for (int suffixIndex = 2; QFileInfo::exists(destinationPrimaryPath); ++suffixIndex) {
            destinationBaseName = sourceBaseName + QStringLiteral("_%1").arg(suffixIndex);
            destinationPrimaryPath = destinationDir.filePath(destinationBaseName + QLatin1Char('.') + suffix);
        }

        const QDir sourceDir = sourceInfo.dir();
        QStringList sidecarNames = sourceDir.entryList(
            QStringList() << (sourceBaseName + QStringLiteral(".*")),
            QDir::Files | QDir::NoDotAndDotDot);
        sidecarNames.removeAll(sourceInfo.fileName());
        sidecarNames.prepend(sourceInfo.fileName());
        QStringList sourceFilePaths;

        // watchResultOutputs 可能已打开根目录 shp，Windows 下直接 rename 会失败。
        // 先复制完整 shp 组，再让宿主图层指向归档副本，最后清理根目录原文件。
        QStringList copiedDestinationPaths;
        bool copiedAllFiles = true;
        for (const QString& sidecarName : sidecarNames) {
            if (!sidecarName.startsWith(sourceBaseName + QLatin1Char('.'))) continue;
            const QString sourceSidecarPath = sourceDir.filePath(sidecarName);
            const QString suffixPart = sidecarName.mid(sourceBaseName.size());
            const QString destinationPath = destinationDir.filePath(destinationBaseName + suffixPart);
            if (!QFile::copy(sourceSidecarPath, destinationPath)) {
                qWarning() << "[错误图层归档] 无法复制文件:" << sourceSidecarPath;
                copiedAllFiles = false;
                break;
            }
            copiedDestinationPaths.append(destinationPath);
            sourceFilePaths.append(sourceSidecarPath);
        }
        if (!copiedAllFiles) {
            for (const QString& copiedPath : copiedDestinationPaths) QFile::remove(copiedPath);
            continue;
        }

        bool dataSourceUpdated = false;
        const auto updateDataSource = [&]() {
            dataSourceUpdated = retargetOrLoadArchivedErrorLayer(
                sourcePath, destinationPrimaryPath, sourceBaseName, true);
        };
        QCoreApplication* application = QCoreApplication::instance();
        if (!application || QThread::currentThread() == application->thread()) {
            updateDataSource();
        } else if (!QMetaObject::invokeMethod(application, updateDataSource,
                                               Qt::BlockingQueuedConnection)) {
            qWarning() << "[错误图层归档] 无法在主线程切换图层数据源:" << sourcePath;
        }
        if (!dataSourceUpdated) {
            for (const QString& copiedPath : copiedDestinationPaths) QFile::remove(copiedPath);
            continue;
        }

        replacements.insert(sourcePath, QDir::cleanPath(destinationPrimaryPath));
        ++archivedCount;
        if (!removeArchivedSourceFiles(sourceFilePaths)) {
            scheduleArchivedSourceCleanup(sourceFilePaths, sourcePath, destinationPrimaryPath,
                                          sourceBaseName, 5);
        }
    }

    if (!replacements.isEmpty()) {
        const QString keyPrefix = QString::number(row) + QLatin1Char('_');
        for (auto resultIt = m_checkResults.begin(); resultIt != m_checkResults.end(); ++resultIt) {
            if (!resultIt.key().startsWith(keyPrefix)) continue;
            resultIt.value().info = replaceArchivedLayerPaths(resultIt.value().info, replacements).toObject();
        }
    }

    // 宿主可能在任务执行期间加载或重新归组结果。归档完成后在工程主线程
    // 核对本次识别到的所有最终成果，补载缺失图层并放入对应结果分组。
    QStringList finalPaths;
    for (auto it = layerSeverities.constBegin(); it != layerSeverities.constEnd(); ++it) {
        const QString source = QDir::cleanPath(it.key());
        const QString finalPath = replacements.value(source, source);
        if (QFileInfo(finalPath).isFile() && !finalPaths.contains(finalPath)) {
            finalPaths.append(finalPath);
        }
    }

    int availableLayers = 0;
    const auto reconcile = [&]() {
        availableLayers = ensureQualityResultLayers(QgsProject::instance(), finalPaths);
    };
    QCoreApplication* application = QCoreApplication::instance();
    if (!application || QThread::currentThread() == application->thread()) {
        reconcile();
    } else if (!QMetaObject::invokeMethod(application, reconcile,
                                           Qt::BlockingQueuedConnection)) {
        qWarning() << "[错误图层归档] 无法在主线程核对最终结果图层";
    }
    if (availableLayers != finalPaths.size()) {
        qWarning() << "[错误图层归档] 最终可用图层:"
                   << availableLayers << "/" << finalPaths.size();
    }

    if (unconfiguredCount > 0) {
        qWarning() << "[错误图层归档] 有" << unconfiguredCount
                   << "个错误图层未配置高/中/低严重程度，保留在根目录。";
    }
    return archivedCount;
}

// ====== 新增：按名称在主线程更新图标（工作线程发起）
void map_check_task_manager::updateCheckItemStatusIconByName(
    int taskRow, const QString& layerName, const QString& checkItemName,
    const CheckItemResult& result)
{
    // 任务树只显示当前图幅。丢弃上一幅图延迟送达的 UI 更新，避免状态串图。
    if (taskRow != m_displayedTaskRow) {
        qDebug() << "[图标更新] 忽略非当前图幅的延迟更新 - 图幅:" << taskRow
                 << "| 当前图幅:" << m_displayedTaskRow
                 << "| 图层:" << layerName << "| 质检项:" << checkItemName;
        return;
    }

    // 同一个 XML 可以为同一图层配置多条同名检查规则（例如 121 分别检查
    // 主区和外区）。算法将这些规则合并为一次执行和一份结果，因此界面中
    // 所有同名节点都应使用同一个完成状态。
    QList<QTreeWidgetItem*> matchedItems;
    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        QTreeWidgetItem* layerNode = ui->treeWidget_xmlTaskData->topLevelItem(i);
        if (!layerNode || layerNode->text(0).trimmed() != layerName.trimmed()) continue;

        auto collectMatches = [&](auto&& self, QTreeWidgetItem* item) -> void {
            if (!item) return;
            if (item->text(2).trimmed() == checkItemName.trimmed()) {
                matchedItems.append(item);
            }
            for (int childIndex = 0; childIndex < item->childCount(); ++childIndex) {
                self(self, item->child(childIndex));
            }
        };
        collectMatches(collectMatches, layerNode);
        break;
    }

    if (matchedItems.isEmpty()) {
        qDebug() << "[图标更新] 警告：未找到质检项 - 图层:" << layerName << "| 质检项:" << checkItemName;
        return;
    }

    qDebug() << "[图标更新] 找到质检项" << matchedItems.size()
             << "个 - 图层:" << layerName << "| 质检项:" << checkItemName;

    // 图标状态严格使用本次算法返回的结果，不能读取可被下一幅图覆盖的全局 JSON 路径。
    for (QTreeWidgetItem* item : matchedItems) {
        updateCheckItemStatusIcon(item, result);
    }
}

// 创建带图标的单元格部件
QWidget* map_check_task_manager::createCellWidget(int row, int col, const QString& text)
{
    QWidget* widget = new QWidget(ui->tableWidget_GeopdfPath);

    // 设置焦点策略，允许焦点通过制表键获得，但不接收鼠标焦点
    widget->setFocusPolicy(Qt::StrongFocus);

    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(2, 2, 2, 2);

    // 文本标签（显示路径，可选中复制）
    QLabel* label = new QLabel(text, widget);
    label->setObjectName("pathLabel");
    label->setWordWrap(false);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // 标签不接收鼠标焦点
    label->setFocusPolicy(Qt::NoFocus);


    // 加载图标按钮
    QPushButton* button = new QPushButton(widget);
    button->setObjectName("loadButton");
    button->setFixedSize(24, 24);

    // 尝试加载图标（替换为实际图标路径）
    QPixmap icon(":/icons/open_folder.png"); // 示例：资源文件路径
    if (!icon.isNull()) {
        button->setIcon(QIcon(icon.scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    else {
        button->setText("...");
    }

    // 第5列（图面配置选择）的按钮逻辑
    if (col == 4) {
        connect(button, &QPushButton::clicked, [this, button, col, label]() {
            // 动态获取按钮所在的行号，避免删除行后行号错乱
            int currentRow = -1;
            for (int r = 0; r < ui->tableWidget_GeopdfPath->rowCount(); ++r) {
                QWidget* cellWidget = ui->tableWidget_GeopdfPath->cellWidget(r, col);
                if (cellWidget && cellWidget->findChild<QPushButton*>("loadButton") == button) {
                    currentRow = r;
                    break;
                }
            }
            if (currentRow < 0) return;
            
            // 1. 从当前行的第1列（shp文件夹）和第3列（gdb_shp文件夹）获取路径
        // 第2列：shp文件夹路径（通过UserRole获取完整路径）
            QTableWidgetItem* shpItem = ui->tableWidget_GeopdfPath->item(currentRow, 1);
            QString shpPath = shpItem ? shpItem->data(Qt::UserRole).toString().trimmed() : "";

            // 第3列：gdb_shp文件夹路径（通过UserRole获取完整路径）
            QTableWidgetItem* gdbShpItem = ui->tableWidget_GeopdfPath->item(currentRow, 2);
            QString gdbShpPath = gdbShpItem ? gdbShpItem->data(Qt::UserRole).toString().trimmed() : "";

            // 2. 验证路径（可选：提示用户先选择文件夹）
            if (shpPath.isEmpty() || gdbShpPath.isEmpty()) {
                QMessageBox::warning(this, tr("警告"), tr("请先选择「shp文件夹」和「gdb_shp文件夹」！"));
                return;
            }

            // 3. 传递路径给Drawing_Layout对话框（关键修改）
            Drawing_Layout dialog(shpPath, gdbShpPath, this);
            if (dialog.exec() == QDialog::Accepted) {
                // 获取对话框中保存的CSV路径
                QString csvPath = dialog.ui->lineEdit_savefile->text().trimmed();
                if (!csvPath.isEmpty()) {
                    // 将路径显示到列表
                    label->setText(csvPath);

                    // 存储完整路径到表格项
                    QTableWidgetItem* item = ui->tableWidget_GeopdfPath->item(currentRow, col);
                    if (item) {
                        item->setData(Qt::UserRole, csvPath);
                    }
                    else {
                        item = new QTableWidgetItem();
                        item->setData(Qt::UserRole, csvPath);
                        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                        ui->tableWidget_GeopdfPath->setItem(currentRow, col, item);
                    }
                    ui->tableWidget_GeopdfPath->resizeColumnsToContents();
                }
            }
            });
    }
    else {

        // 按钮点击：打开文件选择框，并同步更新标签和表格项数据
        connect(button, &QPushButton::clicked, [this, button, col, label]() {
            // 动态获取按钮所在的行号，避免删除行后行号错乱
            int currentRow = -1;
            for (int r = 0; r < ui->tableWidget_GeopdfPath->rowCount(); ++r) {
                QWidget* cellWidget = ui->tableWidget_GeopdfPath->cellWidget(r, col);
                if (cellWidget && cellWidget->findChild<QPushButton*>("loadButton") == button) {
                    currentRow = r;
                    break;
                }
            }
            if (currentRow < 0) return;
            // 步骤1：打开文件/文件夹选择框（逻辑与之前一致）
            QString selectedPath;
            bool isFile = (col == 0); // 第0列是PDF文件，其他列是文件夹
            QString dialogTitle = isFile ? "选择PDF文件" : "选择文件夹";
            QString filter = isFile ? "PDF文件 (*.pdf);;所有文件 (*)" : "";

            if (isFile) {
                QStringList filePaths = QFileDialog::getOpenFileNames(this, dialogTitle, QString(), filter);
                if (filePaths.isEmpty()) return;
                selectedPath = filePaths[0];
            }
            else {
                selectedPath = QFileDialog::getExistingDirectory(this, dialogTitle, QString());
                if (selectedPath.isEmpty()) return;
            }

            // 提取无后缀文件名（仅文件列处理）
            QString displayText = selectedPath;
            if (isFile) {
                QFileInfo fileInfo(selectedPath);
                displayText = fileInfo.baseName(); // 去除后缀
            }


            // 步骤2：更新自定义部件的标签显示
            label->setText(displayText); // 标签显示无后缀文件名

            // 步骤3：将路径存储到表格项的隐藏数据中（关键：用setData存真实路径）
            QTableWidgetItem* item = ui->tableWidget_GeopdfPath->item(currentRow, col);
            if (item) {
                item->setData(Qt::UserRole, selectedPath); // 用UserRole存储真实路径
            }
            else {
                // 若表格项不存在，创建新项并存储
                item = new QTableWidgetItem();
                item->setData(Qt::UserRole, selectedPath);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable); // 禁止编辑
                ui->tableWidget_GeopdfPath->setItem(currentRow, col, item);
            }
            // 内容修改后调整列宽
            ui->tableWidget_GeopdfPath->resizeColumnsToContents();
            });
    }

    // 布局：标签占大部分，按钮居右
    layout->addWidget(label, 1);
    layout->addWidget(button);

    return widget;
}

//加载xml文件夹
void map_check_task_manager::on_pushButton_loadxmlfiles_clicked()
{
    if (m_algorithmThread && m_algorithmThread->isRunning()) {
        QMessageBox::warning(this, tr("提示"), tr("请等待当前质检完成后再重新加载 XML。"));
        return;
    }
    // 选择文件夹
    QString folderPath = QFileDialog::getExistingDirectory(
        this,
        tr("选择质检方案规则文件所在文件夹"),
        QString()
    );

    if (folderPath.isEmpty()) {
        return;
    }

    // 显示文件夹路径
    ui->lineEdit_xmlfiles->setText(folderPath);

    // 清空旧任务树
    ui->treeWidget_xmlTaskData->clear();

    // 遍历文件夹中的XML文件
    QDir dir(folderPath);
    QStringList xmlFiles = dir.entryList(QStringList() << "*.xml", QDir::Files);

    // === 新增：固定加载顺序 ===
    QStringList fixedOrder = { "图名", "比例尺", "投影" };
    QStringList orderedFiles;
    QStringList others;

    // 先分离固定项和其他项
    for (const QString& file : xmlFiles) {
        QString base = QFileInfo(file).baseName();
        if (fixedOrder.contains(base))
            orderedFiles.append(file);
        else
            others.append(file);
    }

    // 按固定顺序排序（图名→比例尺→投影）
    QStringList sortedFiles;
    for (const QString& name : fixedOrder) {
        for (const QString& file : orderedFiles) {
            if (QFileInfo(file).baseName() == name)
                sortedFiles.append(file);
        }
    }
    sortedFiles.append(others); // 其他文件顺序不变

    // === 清空并记录路径 ===
    m_xmlPaths.clear();
    m_checkItemSeverity.clear();
    for (const QString& xmlFile : sortedFiles) {
        QString xmlFilePath = dir.absoluteFilePath(xmlFile);
        m_xmlPaths.append(xmlFilePath);

        // 创建XML文件对应的顶层节点
        QTreeWidgetItem* fileItem = new QTreeWidgetItem(ui->treeWidget_xmlTaskData);
        QFileInfo fileInfo(xmlFile);
        fileItem->setText(0, fileInfo.baseName());

        // 调用统一解析函数
        parseXmlFile(xmlFilePath, fileItem);
    }

    // 默认折叠并保存原始结构
    ui->treeWidget_xmlTaskData->collapseAll();
    saveOriginalTaskTree();
    m_taskTreeFiltered = false;
    
    // 【诊断】输出已加载的图层名称列表
    qDebug() << "========== 已加载的质检任务图层 ==========";
    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = ui->treeWidget_xmlTaskData->topLevelItem(i);
        QString layerName = item->text(0);
        int checkItemCount = 0;
        for (int j = 0; j < item->childCount(); ++j) {
            checkItemCount += item->child(j)->childCount();
        }
        qDebug() << QString("  [%1] 图层名称: \"%2\" (质检项数: %3)")
                    .arg(i+1).arg(layerName).arg(checkItemCount);
    }
    qDebug() << "==========================================";
}

void map_check_task_manager::on_pushButton_loadxmlfile_clicked()
{
    if (m_algorithmThread && m_algorithmThread->isRunning()) {
        QMessageBox::warning(this, tr("提示"), tr("请等待当前质检完成后再重新加载 XML。"));
        return;
    }
    QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("选择XML质检方案文件"),
        QString(),
        tr("XML文件 (*.xml);;所有文件 (*)")
    );

    if (filePaths.isEmpty()) {
        return;
    }

    // === 新增：固定加载顺序 ===
    QStringList fixedOrder = { "图名", "比例尺", "投影" };
    QStringList orderedPaths;
    QStringList others;

    for (const QString& path : filePaths) {
        QString base = QFileInfo(path).baseName();
        if (fixedOrder.contains(base))
            orderedPaths.append(path);
        else
            others.append(path);
    }

    QStringList sortedPaths;
    for (const QString& name : fixedOrder) {
        for (const QString& path : orderedPaths) {
            if (QFileInfo(path).baseName() == name)
                sortedPaths.append(path);
        }
    }
    sortedPaths.append(others);

    // 存储新的XML路径
    m_xmlPaths = sortedPaths;
    m_checkItemSeverity.clear();
    ui->lineEdit_xmlfiles->setText(sortedPaths.first());
    ui->treeWidget_xmlTaskData->clear();

    // 加载XML任务
    for (const QString& xmlFilePath : sortedPaths) {
        QFileInfo fileInfo(xmlFilePath);
        QTreeWidgetItem* fileItem = new QTreeWidgetItem(ui->treeWidget_xmlTaskData);
        fileItem->setText(0, fileInfo.baseName());
        parseXmlFile(xmlFilePath, fileItem);
    }

    // 任务树加载后默认折叠。
    ui->treeWidget_xmlTaskData->collapseAll();
    saveOriginalTaskTree();
    m_taskTreeFiltered = false;
    
    // 【诊断】输出已加载的图层名称列表
    qDebug() << "========== 已加载的质检任务图层 ==========";
    for (int i = 0; i < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = ui->treeWidget_xmlTaskData->topLevelItem(i);
        QString layerName = item->text(0);
        int checkItemCount = 0;
        for (int j = 0; j < item->childCount(); ++j) {
            checkItemCount += item->child(j)->childCount();
        }
        qDebug() << QString("  [%1] 图层名称: \"%2\" (质检项数: %3)")
                    .arg(i+1).arg(layerName).arg(checkItemCount);
    }
    qDebug() << "==========================================";
}

// 上调子任务优先级
void map_check_task_manager::on_pushButton_uptask_clicked()
{
    QTreeWidgetItem* currentItem = ui->treeWidget_xmlTaskData->currentItem();
    if (!currentItem) {
        QMessageBox::information(this, tr("提示"), tr("请先选中要上调的子任务列表或质检项"));
        return;
    }

    // 通用函数：根据图标判断质检状态
    auto getStatusByIcon = [this](QTreeWidgetItem* item) {
        QIcon icon = item->icon(3);
        if (icon.isNull()) return CompletionStatus::NotChecked;
        if (icon.name().contains("红叉", Qt::CaseInsensitive)) return CompletionStatus::HasErrors;
        if (icon.name().contains("叹号", Qt::CaseInsensitive)) return CompletionStatus::HasErrors;
        if (icon.name().contains("勾选", Qt::CaseInsensitive)) return CompletionStatus::Passed;
        return CompletionStatus::NotChecked;
    };

    QTreeWidgetItem* parent = currentItem->parent();

    // ===================== 情况1：顶层子任务 =====================
    if (!parent) {
        int currentIndex = ui->treeWidget_xmlTaskData->indexOfTopLevelItem(currentItem);
        if (currentIndex <= 0) {
            QMessageBox::information(this, tr("提示"), tr("已是第一个子任务列表，无法继续上调"));
            return;
        }

        QList<QTreeWidgetItem*> childItems;
        for (int i = 0; i < currentItem->childCount(); ++i)
            childItems.append(currentItem->child(i)->clone());

        ui->treeWidget_xmlTaskData->takeTopLevelItem(currentIndex);
        QTreeWidgetItem* newItem = new QTreeWidgetItem(*currentItem);
        ui->treeWidget_xmlTaskData->insertTopLevelItem(currentIndex - 1, newItem);

        for (auto* child : childItems)
            newItem->addChild(child);

        delete currentItem;
        ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
        return;
    }

    // ===================== 情况2：比例尺节点 =====================
    if (!currentItem->text(1).isEmpty() && currentItem->text(2).isEmpty()) {
        int currentIndex = parent->indexOfChild(currentItem);
        if (currentIndex <= 0) {
            QMessageBox::information(this, tr("提示"), tr("已是第一个比例尺节点，无法继续上调"));
            return;
        }

        QList<QTreeWidgetItem*> childItems;
        QList<CompletionStatus> childStatus;
        for (int i = 0; i < currentItem->childCount(); ++i) {
            auto* child = currentItem->child(i);
            childItems.append(child->clone());
            childStatus.append(getStatusByIcon(child));
        }

        QString scaleText = currentItem->text(1);
        parent->takeChild(currentIndex);
        QTreeWidgetItem* newItem = new QTreeWidgetItem();
        newItem->setText(1, scaleText);

        parent->insertChild(currentIndex - 1, newItem);

        // === 修改点：只恢复子节点图标，比例尺节点本身不设置任何图标 ===
        for (int i = 0; i < childItems.size(); ++i) {
            newItem->addChild(childItems[i]);
            setCompletionStatusIcon(childItems[i], childStatus[i]);
        }

        delete currentItem;
        ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
        return;
    }

    // ===================== 情况3：质检项节点 =====================
    int currentIndex = parent->indexOfChild(currentItem);
    if (currentIndex <= 0) {
        QMessageBox::information(this, tr("提示"), tr("已是当前子任务列表的第一个质检项，无法继续上调"));
        return;
    }

    QString scaleText = currentItem->text(1);
    QString nameText = currentItem->text(2);
    CompletionStatus status = getStatusByIcon(currentItem);

    parent->takeChild(currentIndex);
    QTreeWidgetItem* newItem = new QTreeWidgetItem();
    newItem->setText(1, scaleText);
    newItem->setText(2, nameText);

    // === 质检项节点需要保留图标 ===
    setCompletionStatusIcon(newItem, status);

    parent->insertChild(currentIndex - 1, newItem);
    delete currentItem;
    ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
}


// 下调子任务优先级
void map_check_task_manager::on_pushButton_downtask_clicked()
{
    QTreeWidgetItem* currentItem = ui->treeWidget_xmlTaskData->currentItem();
    if (!currentItem) {
        QMessageBox::information(this, tr("提示"), tr("请先选中要下调的子任务列表或质检项"));
        return;
    }

    auto getStatusByIcon = [this](QTreeWidgetItem* item) {
        QIcon icon = item->icon(3);
        if (icon.isNull()) return CompletionStatus::NotChecked;
        if (icon.name().contains("红叉", Qt::CaseInsensitive)) return CompletionStatus::HasErrors;
        if (icon.name().contains("叹号", Qt::CaseInsensitive)) return CompletionStatus::HasErrors;
        if (icon.name().contains("勾选", Qt::CaseInsensitive)) return CompletionStatus::Passed;
        return CompletionStatus::NotChecked;
    };

    QTreeWidgetItem* parent = currentItem->parent();

    // ===================== 情况1：顶层子任务 =====================
    if (!parent) {
        int currentIndex = ui->treeWidget_xmlTaskData->indexOfTopLevelItem(currentItem);
        int totalCount = ui->treeWidget_xmlTaskData->topLevelItemCount();
        if (currentIndex >= totalCount - 1) {
            QMessageBox::information(this, tr("提示"), tr("已是最后一个子任务列表，无法继续下调"));
            return;
        }

        QList<QTreeWidgetItem*> childItems;
        for (int i = 0; i < currentItem->childCount(); ++i)
            childItems.append(currentItem->child(i)->clone());

        ui->treeWidget_xmlTaskData->takeTopLevelItem(currentIndex);
        QTreeWidgetItem* newItem = new QTreeWidgetItem(*currentItem);
        ui->treeWidget_xmlTaskData->insertTopLevelItem(currentIndex + 1, newItem);

        for (auto* child : childItems)
            newItem->addChild(child);

        delete currentItem;
        ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
        return;
    }

    // ===================== 情况2：比例尺节点 =====================
    if (!currentItem->text(1).isEmpty() && currentItem->text(2).isEmpty()) {
        int currentIndex = parent->indexOfChild(currentItem);
        int totalCount = parent->childCount();
        if (currentIndex >= totalCount - 1) {
            QMessageBox::information(this, tr("提示"), tr("已是最后一个比例尺节点，无法继续下调"));
            return;
        }

        QList<QTreeWidgetItem*> childItems;
        QList<CompletionStatus> childStatus;
        for (int i = 0; i < currentItem->childCount(); ++i) {
            auto* child = currentItem->child(i);
            childItems.append(child->clone());
            childStatus.append(getStatusByIcon(child));
        }

        QString scaleText = currentItem->text(1);
        parent->takeChild(currentIndex);

        QTreeWidgetItem* newItem = new QTreeWidgetItem();
        newItem->setText(1, scaleText);

        parent->insertChild(currentIndex + 1, newItem);

        // === 修改点：比例尺节点不设置图标，仅恢复其子节点 ===
        for (int i = 0; i < childItems.size(); ++i) {
            newItem->addChild(childItems[i]);
            setCompletionStatusIcon(childItems[i], childStatus[i]);
        }

        delete currentItem;
        ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
        return;
    }

    // ===================== 情况3：质检项节点 =====================
    int currentIndex = parent->indexOfChild(currentItem);
    int totalCount = parent->childCount();
    if (currentIndex >= totalCount - 1) {
        QMessageBox::information(this, tr("提示"), tr("已是当前子任务列表的最后一个质检项，无法继续下调"));
        return;
    }

    QString scaleText = currentItem->text(1);
    QString nameText = currentItem->text(2);
    CompletionStatus status = getStatusByIcon(currentItem);

    parent->takeChild(currentIndex);
    QTreeWidgetItem* newItem = new QTreeWidgetItem();
    newItem->setText(1, scaleText);
    newItem->setText(2, nameText);

    // === 质检项节点保留图标 ===
    setCompletionStatusIcon(newItem, status);

    parent->insertChild(currentIndex + 1, newItem);
    delete currentItem;
    ui->treeWidget_xmlTaskData->setCurrentItem(newItem);
}


//删除子任务列表
void map_check_task_manager::on_pushButton_delete_clicked()
{
    QTreeWidgetItem* currentItem = ui->treeWidget_xmlTaskData->currentItem();
    if (!currentItem) {
        QMessageBox::information(this, tr("提示"), tr("请先选中要删除的子任务列表"));
        return;
    }

    // 确保是顶层子任务列表节点（无父节点）
    if (currentItem->parent() != nullptr) {
        QMessageBox::information(this, tr("提示"), tr("请选中子任务列表（顶层节点）进行操作"));
        return;
    }

    // 确认是否删除子任务列表及旗下所有质检项
    if (QMessageBox::question(this, tr("确认"), tr("确定要删除选中的子任务列表及所有旗下质检项吗？")) == QMessageBox::Yes) {
        delete currentItem;
    }
}

// 处理图标按钮点击事件
void map_check_task_manager::onLoadButtonClicked(const QModelIndex& index)
{
    loadFileOrFolderByIndex(index);
}

void map_check_task_manager::on_pushButton_close_clicked()
{
    if (m_algorithmThread && m_algorithmThread->isRunning()) {
        appendLog(tr("收到关闭窗口请求，正在终止本次质检。"));
        m_algorithmThread->requestStop();
    }
    reject();
}

//结果输出
void map_check_task_manager::on_Button_SaveLogFilePath_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("选择结果输出文件夹"),
        QDir::currentPath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (dir.isEmpty()) return;

    // 保存目录路径
    m_saveDir = dir;
    ui->lineEdit_OutputLogPath->setText(m_saveDir);

    // 如果你希望立即创建文件夹（防止不存在）
    QDir qdir;
    qdir.mkpath(m_saveDir);
}

void map_check_task_manager::on_pushButton_DataPrecheck_clicked()
{
    if (m_algorithmThread && m_algorithmThread->isRunning()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请等待当前质检任务完成后再执行数据预检查。"));
        return;
    }

    QTableWidget* table = ui->tableWidget_GeopdfPath;
    if (!table || table->rowCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先加载待检查地图。"));
        return;
    }

    const QString outputBaseDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择错误结果保存目录"), QDir::currentPath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (outputBaseDir.isEmpty()) return;

    const QDateTime precheckStartedAt = QDateTime::currentDateTime();
    const QString precheckTimeText = precheckStartedAt.toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString runDirName = QStringLiteral("数据预检查_%1").arg(precheckTimeText);
    QDir outputDir(outputBaseDir);
    QString runDirPath = outputDir.filePath(runDirName);
    int runSuffix = 2;
    while (QFileInfo::exists(runDirPath)) {
        runDirPath = outputDir.filePath(QStringLiteral("%1_%2").arg(runDirName).arg(runSuffix++));
    }
    if (!QDir().mkpath(runDirPath)) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("无法创建错误结果目录：\n%1").arg(runDirPath));
        return;
    }

    int mapsChecked = 0;
    int typeErrorLayers = 0;
    int fontErrorLayers = 0;
    int actualUnmatchedLayers = 0;
    QStringList inputProblems;
    QMap<QString, QStringList> errorLayersByMap;
    QMap<QString, QSet<QString>> seenErrorLayersByMap;
    struct PrecheckLogTarget {
        QString mapName;
        QString resultDir;
        int scaleDenominator = 0;
    };
    QVector<PrecheckLogTarget> precheckLogTargets;
    auto addErrorLayerName = [&errorLayersByMap, &seenErrorLayersByMap](const QString& mapName, QString layerName) {
        const QString key = layerName.toCaseFolded();
        if (seenErrorLayersByMap[mapName].contains(key)) return;
        seenErrorLayersByMap[mapName].insert(key);
        errorLayersByMap[mapName].append(layerName);
    };
    const QVector<LayerTaskSnapshot> scaleTaskSnapshot = buildTaskSnapshot();

    auto* button = findChild<QPushButton*>(QStringLiteral("pushButton_DataPrecheck"));
    if (button) button->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    for (int row = 0; row < table->rowCount(); ++row) {
        AlgorithmFilePaths filePaths = getFilePathsByRow(row);
        const QString mapName = QFileInfo(filePaths.pdfPath).completeBaseName().trimmed().isEmpty()
            ? QStringLiteral("地图%1").arg(row + 1)
            : QFileInfo(filePaths.pdfPath).completeBaseName();
        const QStringList mapDirParts { QStringLiteral("%1_%2").arg(row + 1).arg(safeOutputName(mapName)) };
        const QString mapOutputDir = QDir(runDirPath).filePath(mapDirParts.first());
        precheckLogTargets.append({ mapName, mapOutputDir, 0 });

        if (filePaths.csvPath.trimmed().isEmpty()) {
            inputProblems << QStringLiteral("%1：未配置图面配置 CSV").arg(mapName);
            continue;
        }
        const QMap<QString, CsvLayerInfo> layerMap = parseCsvToLayerMap(filePaths.csvPath);
        if (layerMap.isEmpty()) {
            inputProblems << QStringLiteral("%1：CSV 不存在、无法读取或表头格式不正确").arg(mapName);
            continue;
        }

        ++mapsChecked;

        int scaleDenominator = 0;
        QStringList scaleLayerNames;
        for (const LayerTaskSnapshot& task : scaleTaskSnapshot) {
            if (task.checkItemNames.contains(QStringLiteral("比例尺数值检查")) &&
                layerMap.contains(task.layerName) && !scaleLayerNames.contains(task.layerName)) {
                scaleLayerNames << task.layerName;
            }
        }
        for (auto it = layerMap.constBegin(); it != layerMap.constEnd(); ++it) {
            if (!scaleLayerNames.contains(it.key())) scaleLayerNames << it.key();
        }
        for (const QString& scaleLayerName : scaleLayerNames) {
            AlgorithmFilePaths scalePaths = filePaths;
            buildShpPaths(scalePaths, layerMap.value(scaleLayerName));
            scalePaths.standardLayer = scaleLayerName;
            CheckItemResult scaleResult;
            algorithm_ScaleValueCheck(QStringLiteral("比例尺数值检查"), scalePaths, 0, QString(), scaleResult);
            scaleDenominator = scaleDenominatorFromResult(scaleResult);
            if (scaleDenominator > 0) break;
        }
        precheckLogTargets.last().scaleDenominator = scaleDenominator;
        if (scaleDenominator <= 0) {
            inputProblems << QStringLiteral("%1：比例尺检查未能计算出有效比例尺，已跳过实际图层空间检查").arg(mapName);
        }

        // 先按实际 SHP 完整路径归并所有配置行，再执行空间检查。同一个实际图层
        // （如“景点景区”）同时配置给文物古迹、旅游景区时，只检查一次，并将
        // 两行中的 GDB 图层合并为候选集合；命中任意候选 GDB 即认为有空间支撑。
        struct ActualSpatialCheckGroup {
            QString sourcePath;
            QSet<QString> gdbPaths;
            double bufferMillimeters = 2.0;
        };
        QMap<QString, ActualSpatialCheckGroup> actualCheckGroups;
        const QStringList actualSuffixes { QString(), QStringLiteral("_Point"),
                                           QStringLiteral("_LineString"), QStringLiteral("_Polygon") };
        for (auto it = layerMap.constBegin(); it != layerMap.constEnd(); ++it) {
            QStringList rowGdbPaths;
            for (const QString& rawGdbName : it.value().gdbLayerNames) {
                const QString gdbName = rawGdbName.trimmed();
                if (gdbName.isEmpty()) continue;
                const QString gdbPath = resolveShapefilePath(filePaths.gdbShpPath, gdbName);
                if (!gdbPath.isEmpty()) {
                    if (!rowGdbPaths.contains(gdbPath, Qt::CaseInsensitive)) rowGdbPaths << gdbPath;
                } else {
                    inputProblems << QStringLiteral("%1 / %2：找不到对应 GDB 图层文件").arg(mapName, gdbName);
                }
            }

            for (const QString& rawActualName : it.value().actualLayerNames) {
                const QString actualName = rawActualName.trimmed();
                if (actualName.isEmpty()) continue;
                QStringList actualPaths;
                if (!filePaths.shpPath.trimmed().isEmpty()) {
                    for (const QString& suffix : actualSuffixes) {
                        const QString actualPath = resolveShapefilePath(filePaths.shpPath, actualName + suffix);
                        if (!actualPath.isEmpty() && !actualPaths.contains(actualPath, Qt::CaseInsensitive)) {
                            actualPaths << actualPath;
                        }
                    }
                }
                if (actualPaths.isEmpty()) {
                    inputProblems << QStringLiteral("%1 / %2：找不到实际图层文件").arg(mapName, actualName);
                    continue;
                }
                if (rowGdbPaths.isEmpty()) {
                    inputProblems << QStringLiteral("%1 / %2：未配置或找不到对应 GDB 图层，已跳过空间检查")
                        .arg(mapName, actualName);
                    continue;
                }
                const bool useNaturalVillageBuffer =
                    it.key().trimmed() == QStringLiteral("自然村")
                    || actualName.compare(QStringLiteral("自然村"), Qt::CaseInsensitive) == 0;
                const QString standardLayerName = it.key().trimmed();
                const bool useExpresswayEntranceBuffer =
                    standardLayerName == QStringLiteral("高速出入口")
                    || standardLayerName == QStringLiteral("高速路出入口")
                    || actualName.compare(QStringLiteral("高速出入口"), Qt::CaseInsensitive) == 0
                    || actualName.compare(QStringLiteral("高速路出入口"), Qt::CaseInsensitive) == 0;
                for (const QString& actualPath : actualPaths) {
                    const QString key = QDir::cleanPath(actualPath).toCaseFolded();
                    ActualSpatialCheckGroup& group = actualCheckGroups[key];
                    group.sourcePath = actualPath;
                    if (useExpresswayEntranceBuffer)
                        group.bufferMillimeters = qMax(group.bufferMillimeters, 5.0);
                    else if (useNaturalVillageBuffer)
                        group.bufferMillimeters = qMax(group.bufferMillimeters, 2.5);
                    for (const QString& gdbPath : rowGdbPaths) group.gdbPaths.insert(gdbPath);
                }
            }
        }

        if (scaleDenominator > 0) {
            for (auto actualIt = actualCheckGroups.constBegin(); actualIt != actualCheckGroups.constEnd(); ++actualIt) {
                const ActualSpatialCheckGroup& group = actualIt.value();
                const QString sourcePath = group.sourcePath;
                const QString sourceBase = QFileInfo(sourcePath).completeBaseName();
                const QString errorPath = QDir(mapOutputDir).filePath(
                    QStringLiteral("%1_实际图层混入错误.shp").arg(safeOutputName(sourceBase)));
                QStringList gdbPaths = group.gdbPaths.values();
                gdbPaths.sort(Qt::CaseInsensitive);
                int actualFeatures = 0;
                int unmatchedFeatures = 0;
                QString errorMessage;
                if (!writeActualLayerSpatialPrecheckErrors(sourcePath, gdbPaths, scaleDenominator,
                                                           group.bufferMillimeters,
                                                           errorPath, actualFeatures, unmatchedFeatures, errorMessage)) {
                    inputProblems << QStringLiteral("%1 / %2：%3").arg(mapName, sourceBase, errorMessage);
                } else if (unmatchedFeatures > 0) {
                    ++actualUnmatchedLayers;
                    addErrorLayerName(mapName, sourceBase);
                }
            }
        }

        QSet<QString> processedAnnotationPaths;
        for (auto it = layerMap.constBegin(); it != layerMap.constEnd(); ++it) {
            const QStringList annotNames = it.value().actualAnnotLayerNames;
            for (const QString& rawAnnotName : annotNames) {
                const QString annotName = rawAnnotName.trimmed();
                if (annotName.isEmpty()) continue;

                QStringList candidates;
                const QStringList suffixes { QString(), QStringLiteral("_Point"), QStringLiteral("_LineString"), QStringLiteral("_Polygon") };
                for (const QString& suffix : suffixes) {
                    const QString path = resolveShapefilePath(filePaths.shpPath, annotName + suffix);
                    if (!path.isEmpty() && !candidates.contains(path, Qt::CaseInsensitive)) candidates << path;
                }
                if (candidates.isEmpty()) {
                    inputProblems << QStringLiteral("%1 / %2：找不到实际注记层文件").arg(mapName, annotName);
                    continue;
                }

                for (const QString& sourcePath : candidates) {
                    // 解析后的注记 LineString 保存的是 element_ty=path 的文字路径，
                    // 不是实际注记文字，不参与 text 类型及字体混入检查。
                    if (QFileInfo(sourcePath).completeBaseName().endsWith(
                            QStringLiteral("_LineString"), Qt::CaseInsensitive)) {
                        continue;
                    }
                    const QString normalizedSourcePath = QDir::cleanPath(sourcePath).toCaseFolded();
                    if (processedAnnotationPaths.contains(normalizedSourcePath)) continue;
                    processedAnnotationPaths.insert(normalizedSourcePath);
                    const QString sourceBase = QFileInfo(sourcePath).completeBaseName();
                    const QString errorPath = QDir(mapOutputDir).filePath(
                        QStringLiteral("%1_注记预检查错误.shp").arg(safeOutputName(sourceBase)));
                    int fileFeatures = 0;
                    int fileErrors = 0;
                    int fileTypeErrors = 0;
                    int fileFontErrors = 0;
                    QString errorMessage;
                    QStringList layerWarnings;
                    if (!writeAnnotationPrecheckErrors(sourcePath, errorPath, fileFeatures, fileErrors,
                                                       fileTypeErrors, fileFontErrors, errorMessage, layerWarnings)) {
                        inputProblems << QStringLiteral("%1 / %2：%3").arg(mapName, sourceBase, errorMessage);
                    }
                    if (fileTypeErrors > 0) ++typeErrorLayers;
                    if (fileFontErrors > 0) ++fontErrorLayers;
                    if (fileTypeErrors > 0 || fileFontErrors > 0) addErrorLayerName(mapName, sourceBase);
                    for (const QString& warning : layerWarnings) {
                        inputProblems << QStringLiteral("%1 / %2：%3").arg(mapName, sourceBase, warning);
                    }
                }
            }
        }
    }

    const QDateTime precheckFinishedAt = QDateTime::currentDateTime();
    QStringList logWriteErrors;
    QSet<QString> usedLogPaths;
    for (const PrecheckLogTarget& target : precheckLogTargets) {
        const QString logBaseName = QStringLiteral("%1_%2").arg(safeOutputName(target.mapName), precheckTimeText);
        QString logPath = QDir(runDirPath).filePath(logBaseName + QStringLiteral(".log"));
        int logSuffix = 2;
        while (usedLogPaths.contains(QDir::cleanPath(logPath).toCaseFolded()) || QFileInfo::exists(logPath)) {
            logPath = QDir(runDirPath).filePath(
                QStringLiteral("%1_%2.log").arg(logBaseName).arg(logSuffix++));
        }
        usedLogPaths.insert(QDir::cleanPath(logPath).toCaseFolded());

        QFile logFile(logPath);
        if (!logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logWriteErrors.append(QStringLiteral("%1：无法写入日志文件").arg(target.mapName));
            continue;
        }
        QTextStream logStream(&logFile);
        logStream.setCodec("UTF-8");
        logStream << QStringLiteral("数据预检查日志\n")
                  << QStringLiteral("地图：%1\n").arg(target.mapName)
                  << QStringLiteral("开始时间：%1\n").arg(precheckStartedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                  << QStringLiteral("结束时间：%1\n").arg(precheckFinishedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                  << (target.scaleDenominator > 0
                        ? QStringLiteral("比例尺：1:%1\n").arg(target.scaleDenominator)
                        : QStringLiteral("比例尺：未能识别\n"))
                  << QStringLiteral("空间检查阈值：高速路出入口 5 毫米，自然村 2.5 毫米，其他图层 2 毫米\n")
                  << QStringLiteral("错误结果目录：%1\n\n").arg(target.resultDir);

        const QStringList errorLayers = errorLayersByMap.value(target.mapName);
        logStream << QStringLiteral("错误图层：\n");
        if (errorLayers.isEmpty()) {
            logStream << QStringLiteral("无\n");
        } else {
            for (const QString& layerName : errorLayers)
                logStream << QStringLiteral("- %1\n").arg(layerName);
        }

        QStringList mapProblems;
        const QString directPrefix = target.mapName + QStringLiteral("：");
        const QString layerPrefix = target.mapName + QStringLiteral(" / ");
        for (const QString& problem : inputProblems) {
            if (problem.startsWith(directPrefix) || problem.startsWith(layerPrefix))
                mapProblems.append(problem);
        }
        logStream << QStringLiteral("\n检查提示：\n");
        if (mapProblems.isEmpty()) {
            logStream << QStringLiteral("无\n");
        } else {
            for (const QString& problem : mapProblems)
                logStream << QStringLiteral("- %1\n").arg(problem);
        }
    }

    QApplication::restoreOverrideCursor();
    if (button) button->setEnabled(true);

    QString summary = QStringLiteral("数据预检查完成。\n地图：%1 幅\n实际图层未匹配到对应 GDB 要素：%2 个图层\n注记层混入符号：%3 个图层\n注记层混入注记：%4 个图层")
        .arg(mapsChecked).arg(actualUnmatchedLayers).arg(typeErrorLayers).arg(fontErrorLayers);
    if (errorLayersByMap.isEmpty()) {
        summary += QStringLiteral("\n\n错误图层：未发现错误图层");
    } else {
        QStringList displayedMaps;
        for (auto it = errorLayersByMap.constBegin(); it != errorLayersByMap.constEnd(); ++it) {
            const QStringList layerNames = it.value();
            QStringList wrappedLines;
            const int namesPerLine = 6;
            for (int i = 0; i < layerNames.size(); i += namesPerLine) {
                const QString names = layerNames.mid(i, namesPerLine).join(QStringLiteral("、"));
                wrappedLines.append(i == 0 ? QStringLiteral("%1：%2").arg(it.key(), names)
                                           : QStringLiteral("    %1").arg(names));
            }
            displayedMaps.append(wrappedLines.join(QLatin1Char('\n')));
        }
        summary += QStringLiteral("\n\n错误图层：\n%1")
            .arg(displayedMaps.join(QLatin1Char('\n')));
    }
    if (!logWriteErrors.isEmpty()) {
        summary += QStringLiteral("\n\n日志写入提示：\n%1").arg(logWriteErrors.join(QLatin1Char('\n')));
    }
    QMessageBox::information(this, QStringLiteral("数据预检查"), summary);
}

// 开始/暂停 单一按钮
void map_check_task_manager::on_pushButton_StartPause_clicked()
{
    QTableWidget* table = ui->tableWidget_GeopdfPath;
    int rowCount = table->rowCount();

    // 若未在运行，则视为“开始”动作
    if (!m_algorithmThread) {
        if (rowCount == 0) {
            QMessageBox::warning(this, tr("警告"), tr("请先添加地图文件路径！"));
            return;
        }

        // 上次运行结束后，界面可能只剩匹配该图比例尺的部分任务。
        // 本次运行必须先恢复完整方案，再从中建立新的任务快照。
        if (m_taskTreeFiltered) {
            restoreOriginalTaskTree();
            m_taskTreeFiltered = false;
        } else {
            // 保留用户在加载 XML 后对任务顺序或内容的调整。
            saveOriginalTaskTree();
        }

        const QVector<LayerTaskSnapshot> taskSnapshot = buildTaskSnapshot();
        bool hasCheckItem = false;
        for (const LayerTaskSnapshot& task : taskSnapshot) {
            if (!task.checkItemNames.isEmpty()) {
                hasCheckItem = true;
                break;
            }
        }
        if (!hasCheckItem) {
            QMessageBox::warning(this, tr("警告"), tr("当前质检方案没有可运行的质检项，请检查 XML 加载结果。"));
            return;
        }

        m_checkResults.clear();
        m_completedCheckCount.store(0);
        m_succeededCheckCount.store(0);
        m_failedCheckCount.store(0);
        m_skippedMapCount.store(0);
        m_currentTaskRow = -1;
        m_displayedTaskRow = -1;
        m_currentJsonFilePath.clear();
        m_globalStartTime = QDateTime::currentDateTime();

        // 在主线程构建 UI 快照，供工作线程使用
        m_layerTasksSnapshot = taskSnapshot;
        m_mapRowsSnapshot = buildRowSnapshot();
        m_outputDirSnapshot = ui->lineEdit_OutputLogPath->text().trimmed();
        if (m_outputDirSnapshot.isEmpty()) {
            m_outputDirSnapshot = QDir::current().filePath(QStringLiteral("output_results"));
            ui->lineEdit_OutputLogPath->setText(m_outputDirSnapshot);
        }
        initializeRunLog();
        if (m_runLogFilePath.isEmpty()) {
            QMessageBox::warning(this, tr("日志创建失败"),
                tr("无法在输出目录创建运行日志文件。本次质检可以继续，但不会生成完整运行日志。\n%1")
                    .arg(QDir::toNativeSeparators(m_outputDirSnapshot)));
        }
        savePhaseOneConfigSnapshot();

        m_algorithmThread = new AlgorithmThread(this);
        AlgorithmThread* const currentThread = m_algorithmThread;
        connect(m_algorithmThread, &AlgorithmThread::progressChanged, this, [this](int value) {
            if (ui->progressBar) ui->progressBar->setValue(value);
        });
        connect(currentThread, &AlgorithmThread::finished, this, [this, currentThread]() {
            if (m_algorithmThread != currentThread) return;
            const bool ranChecks = m_completedCheckCount.load() > 0;
            m_globalEndTime = QDateTime::currentDateTime();
            const qint64 elapsedSeconds = m_globalStartTime.secsTo(m_globalEndTime);
            appendLog(ranChecks
                ? tr("本次质检结束：共执行 %1 项，正常完成 %2 项，失败 %3 项，跳过地图 %4 幅，耗时 %5 秒。")
                    .arg(m_completedCheckCount.load())
                    .arg(m_succeededCheckCount.load())
                    .arg(m_failedCheckCount.load())
                    .arg(m_skippedMapCount.load())
                    .arg(elapsedSeconds)
                : tr("本次质检结束：未执行任何质检项。请检查图面配置 CSV 与 XML 子任务名称是否匹配。"));
            if (!m_runLogFilePath.isEmpty()) {
                appendLog(tr("运行日志已保存：%1").arg(QDir::toNativeSeparators(m_runLogFilePath)));
            }
            if (ui->progressBar && ranChecks) ui->progressBar->setValue(100);
            if (ui->pushButton_StartPause) ui->pushButton_StartPause->setText(tr("开始"));
            currentThread->wait();
            m_algorithmThread = nullptr;
            delete currentThread;
            m_isPaused = false;
            if (ranChecks) {
                QMessageBox::information(this, tr("完成"), tr("所有地图质检完成！"));
            } else {
                QMessageBox::warning(this, tr("未执行质检"),
                    tr("本次没有执行任何质检项。请检查图面配置 CSV 是否可读取，以及其中的“标准图层”是否与 XML 子任务名称一致。"));
            }
        });

        ui->pushButton_StartPause->setText(tr("暂停"));
        ui->progressBar->setValue(0);
        appendLog(tr("开始质检：地图 %1 幅，计划质检项按各图配置动态计算。").arg(rowCount));
        m_isPaused = false;
        m_algorithmThread->start();
        return;
    }

    // 正在运行：切换为“暂停/继续”
    if (m_algorithmThread->isRunning() && !m_isPaused) {
        m_algorithmThread->requestPause();
        m_isPaused = true;
        ui->pushButton_StartPause->setText(tr("开始"));
        appendLog("已暂停");
    } else if (m_algorithmThread->isRunning() && m_isPaused) {
        m_algorithmThread->requestResume();
        m_isPaused = false;
        ui->pushButton_StartPause->setText(tr("暂停"));
        appendLog("▶ 继续质检...");
    }
}

// 不再使用独立暂停按钮

void map_check_task_manager::initializeRunLog()
{
    m_runLogFilePath.clear();

    QDir outputDir(m_outputDirSnapshot);
    if (!outputDir.exists() && !QDir().mkpath(outputDir.absolutePath())) {
        if (ui->label_log) {
            ui->label_log->setText(tr("无法创建日志目录：%1")
                .arg(QDir::toNativeSeparators(outputDir.absolutePath())));
        }
        return;
    }

    const QString baseName = QStringLiteral("质检运行日志_%1")
        .arg(m_globalStartTime.toString(QStringLiteral("yyyyMMdd_HHmmss")));
    QString candidate = outputDir.filePath(baseName + QStringLiteral(".log"));
    int suffix = 2;
    while (QFileInfo::exists(candidate)) {
        candidate = outputDir.filePath(QStringLiteral("%1_%2.log").arg(baseName).arg(suffix++));
    }

    QFile logFile(candidate);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (ui->label_log) {
            ui->label_log->setText(tr("无法创建运行日志文件：%1")
                .arg(QDir::toNativeSeparators(candidate)));
        }
        return;
    }
    QTextStream stream(&logFile);
    stream.setCodec("UTF-8");
    stream << QStringLiteral("质检任务管理运行日志\n")
           << QStringLiteral("开始时间：%1\n").arg(m_globalStartTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
           << QStringLiteral("执行人：%1\n").arg(ui->lineEdit->text().trimmed().isEmpty()
                  ? QStringLiteral("operator") : ui->lineEdit->text().trimmed())
           << QStringLiteral("输出目录：%1\n").arg(QDir::toNativeSeparators(outputDir.absolutePath()))
           << QStringLiteral("地图数量：%1\n").arg(m_mapRowsSnapshot.size())
           << QStringLiteral("------------------------------------------------------------\n");
    m_runLogFilePath = candidate;
}

// 状态标签只显示当前状态；完整运行过程仅写入日志文件。
void map_check_task_manager::appendLog(const QString& msg)
{
    if (QThread::currentThread() != qApp->thread()) {
        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection, Q_ARG(QString, msg));
        return;
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString entry = QStringLiteral("[%1] %2").arg(timestamp, msg);
    if (ui->label_log) ui->label_log->setText(msg);
    if (!m_runLogFilePath.isEmpty()) {
        QFile logFile(m_runLogFilePath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << entry << QLatin1Char('\n');
        }
    }
}

void map_check_task_manager::updateProgress(int value)
{
    ui->progressBar->setValue(value);
}


// 通用的文件/文件夹加载函数
void map_check_task_manager::loadFileOrFolderByIndex(const QModelIndex& index)
{
    int row = index.row();
    int col = index.column();

    QString path;
    bool isFile = false;
    QString dialogTitle = "";
    QString filter = "";

    // 根据列的不同，选择加载PDF文件或文件夹
    if (col == 0) {
        // 地图路径列，加载PDF文件
        dialogTitle = "选择PDF文件";
        filter = "PDF文件 (*.pdf);;所有文件 (*)";
        isFile = true;
    }
    else if (col == 1) {
        // shp文件夹列，加载文件夹
        dialogTitle = "选择shp文件夹";
    }
    else if (col == 2) {
        // gdb_shp文件夹列，加载文件夹
        dialogTitle = "选择gdb_shp文件夹";
    }
    else if (col == 3) {
        // 标准图例文件夹列，加载预处理生成的图例目录
        dialogTitle = QString::fromUtf8(u8"选择图例文件夹");
    }
    else {
        // 无效列，直接返回
        return;
    }

    if (isFile) {
        // 选择文件
        QStringList filePaths = QFileDialog::getOpenFileNames(
            this,
            dialogTitle,
            QString(),
            filter
        );

        if (filePaths.isEmpty()) {
            return;
        }

        // 将选择的文件路径添加到表格中，与第一个函数保持一致
        QTableWidgetItem* item = new QTableWidgetItem(filePaths[0]);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_GeopdfPath->setItem(row, col, item);
    }
    else {
        // 选择文件夹
        path = QFileDialog::getExistingDirectory(
            this,
            dialogTitle,
            QString()
        );

        if (path.isEmpty()) {
            return;
        }

        // 将选择的文件夹路径添加到表格中，与第一个函数保持一致
        QTableWidgetItem* item = new QTableWidgetItem(path);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        ui->tableWidget_GeopdfPath->setItem(row, col, item);
    }
}

// 设置单元格内容
void map_check_task_manager::setCellContent(int row, int col, const QString& content)
{
    // 移除现有部件
    if (QWidget* oldWidget = ui->tableWidget_GeopdfPath->cellWidget(row, col)) {
        delete oldWidget;
    }

    // 提取显示名称（如果是路径则只显示文件名/文件夹名）
    QString displayText = content;
    if (!content.isEmpty()) {
        QFileInfo fileInfo(content);
        if (fileInfo.isFile()) {
            // 如果是文件，显示不含路径和后缀的文件名
            displayText = fileInfo.baseName();
        } else if (fileInfo.isDir()) {
            // 如果是文件夹，显示不含路径的文件夹名
            displayText = fileInfo.fileName();
        }
        // 如果既不是文件也不是文件夹（比如空字符串），保持原样
    }

    //创建新的带按钮的部件（显示文件名/文件夹名）
    QWidget* newWidget = createCellWidget(row, col, displayText);
    ui->tableWidget_GeopdfPath->setCellWidget(row, col, newWidget);

    // 同时创建表格项，存储完整路径到UserRole
    QTableWidgetItem* item = ui->tableWidget_GeopdfPath->item(row, col);
    if (!item) {
        item = new QTableWidgetItem();
        item->setData(Qt::UserRole, content); // 存储完整路径
        item->setFlags(item->flags() & ~Qt::ItemIsEditable); // 禁止编辑
        ui->tableWidget_GeopdfPath->setItem(row, col, item);
    }
    else {
        item->setData(Qt::UserRole, content); // 更新已有项的完整路径
    }
    // 添加列宽自适应
    ui->tableWidget_GeopdfPath->resizeColumnsToContents();
}

void map_check_task_manager::setCompletionStatusIcon(QTreeWidgetItem* item, CompletionStatus status)
{
    QIcon icon;
    switch (status) {
    case CompletionStatus::NotChecked:
        icon = QIcon(":/icons/cross.png");   // 未质检 → 红叉
        break;
    case CompletionStatus::HasErrors:
        icon = QIcon(":/icons/warn.png");    // 有错误 → 黄叹号
        break;
    case CompletionStatus::Passed:
        icon = QIcon(":/icons/check.png");   // 无错误 → 绿对勾
        break;
    }
    item->setData(3, Qt::UserRole, static_cast<int>(status));
    item->setIcon(3, icon); // 第3列显示图标
}

QString map_check_task_manager::getCheckItemStatus(const QTreeWidgetItem* item)
{
    QIcon icon = item->icon(3);
    QString iconPath = icon.availableSizes().isEmpty() ? "" : icon.name();

    if (iconPath.contains("红叉", Qt::CaseInsensitive)) {
        return "未质检"; // 与判断条件一致
    }
    else if (iconPath.contains("叹号", Qt::CaseInsensitive)) {
        return "质检失败"; // 与判断条件一致
    }
    else if (iconPath.contains("勾选", Qt::CaseInsensitive)) {
        return "质检成功"; // 与判断条件一致
    }
    return "未质检"; // 默认返回未质检
}

QJsonObject map_check_task_manager::generateJsonData()
{
    QJsonObject root;

    // 用户信息与时间
    QString userName = ui->lineEdit->text().trimmed();
    root["user"] = userName.isEmpty() ? "operator" : userName;

    QJsonObject timing;
    timing["start_time"] = m_globalStartTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    timing["end_time"] = m_globalEndTime.toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
    root["timing"] = timing;

    // 仅生成当前地图对应的单个 Geopdf 节点（每幅图一个文件）
    QTableWidget* table = ui->tableWidget_GeopdfPath;
    int rowCount = table->rowCount();
    int targetRow = (m_currentTaskRow >= 0 && m_currentTaskRow < rowCount) ? m_currentTaskRow : 0;

    const QString geopdfKey = QStringLiteral("Geopdf");
    QJsonObject geopdfNode;

    // 地图名
    QString mapName;
    QTableWidgetItem* nameItem = table->item(targetRow, 0);
    if (nameItem) {
        QString fullPath = nameItem->data(Qt::UserRole).toString();
        if (!fullPath.isEmpty()) {
            QFileInfo fileInfo(fullPath);
            mapName = fileInfo.baseName();
        }
    }
    if (mapName.isEmpty()) {
        mapName = QString("未命名地图%1").arg(targetRow + 1);
    }
    geopdfNode["name"] = mapName;

    // info统计（初始，根据当前已有结果）
    int map_completed_num = 0;
    int map_err_num = 0;           // 依据 err_exist 统计
    int map_uncompleted_num = 0;

    // 所有质检任务（task节点）
    QJsonObject mapTasks;
    int tasksAddedCount = 0;

    // ============ 第一层：子任务列表（图层） ============
    for (int taskIdx = 0; taskIdx < ui->treeWidget_xmlTaskData->topLevelItemCount(); ++taskIdx) {
        QTreeWidgetItem* layerNode = ui->treeWidget_xmlTaskData->topLevelItem(taskIdx);
        if (!layerNode) continue;
        QString layerName = layerNode->text(0);

        QJsonObject taskNode;
        taskNode["layer_name"] = layerName;

        int checkItemCount = 0;

        // ============ 第二层：比例尺节点 ============
        for (int scaleIdx = 0; scaleIdx < layerNode->childCount(); ++scaleIdx) {
            QTreeWidgetItem* scaleNode = layerNode->child(scaleIdx);
            if (!scaleNode) continue;

            // ============ 第三层：质检项节点 ============
            for (int itemIdx = 0; itemIdx < scaleNode->childCount(); ++itemIdx) {
                QTreeWidgetItem* checkItem = scaleNode->child(itemIdx);
                if (!checkItem) continue;
                QString checkItemName = checkItem->text(2).trimmed();
                if (checkItemName.isEmpty()) continue;

                ++checkItemCount;

                const QString uniqueKey = QString("%1_%2_%3").arg(targetRow).arg(layerName).arg(checkItemName);
                QJsonObject checkItemJson;
                const QString severity = m_checkItemSeverity.value(
                    checkItemSeverityKey(layerName, checkItemName), QStringLiteral("未配置"));

                // === 有执行结果 ===
                if (m_checkResults.contains(uniqueKey)) {
                    const CheckItemResult& res = m_checkResults[uniqueKey];
                    checkItemJson["id"] = res.id;
                    checkItemJson["name"] = res.name.isEmpty() ? checkItemName : res.name;
                    checkItemJson["status"] = res.status;
                    checkItemJson["err_exist"] = res.err_exist;
                    checkItemJson["info"] = res.info;
                    checkItemJson["errors"] = res.errors;

                    if (res.status == "质检成功" || res.status == "质检完成") map_completed_num++;
                    else if (res.status == "质检失败") map_uncompleted_num++; // 失败计入未完成
                    else map_uncompleted_num++;
                    if (res.err_exist) map_err_num++;
                }
                // === 未执行 ===
                else {
                    checkItemJson["id"] = "";
                    checkItemJson["name"] = checkItemName;
                    checkItemJson["status"] = "未质检";
                    checkItemJson["err_exist"] = false;
                    checkItemJson["info"] = QJsonObject();
                    checkItemJson["errors"] = QJsonObject();
                    map_uncompleted_num++;
                }

                // 严重程度由规则 XML 的 check_item/severity 提供，
                // 与具体质检算法无关，因此统一在总 JSON 组装时写入。
                checkItemJson["severity"] = severity;

                taskNode[QString("check_item%1").arg(checkItemCount)] = checkItemJson;
            }
        }

        // 增加 task 节点
        mapTasks[QString("task%1").arg(taskIdx + 1)] = taskNode;
        tasksAddedCount++;
    }

    // info统计（tasks_num 按 task 数量统计；err_num 按 err_exist）
    QJsonObject mapInfo;
    mapInfo["tasks_num"] = tasksAddedCount;
    mapInfo["completed_num"] = map_completed_num;
    mapInfo["err_num"] = map_err_num;
    mapInfo["uncompleted_num"] = map_uncompleted_num;
    geopdfNode["info"] = mapInfo;

    // 任务集合
    geopdfNode["tasks"] = mapTasks;

    root[geopdfKey] = geopdfNode;

    return root;
}

bool map_check_task_manager::writeJsonToFile(const QJsonObject& jsonData, const QString& filePath)
{
    try {
        QFile file(filePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                Q_ARG(QString, tr("无法打开文件：%1").arg(filePath)));
            return false;
        }

        QJsonDocument doc(jsonData);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
        // 保持label_log用于显示当前地图进度，成功保存JSON不再覆盖状态标签
        return true;
    }
    catch (const std::exception& e) {
        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
            Q_ARG(QString, tr("JSON生成错误：%1").arg(QString::fromStdString(e.what()))));
        return false;
    }
}

bool map_check_task_manager::updateJsonFileWithResult(int row, const QString& layerName, const QString& checkItemName, const CheckItemResult& result)
{
    if (m_currentJsonFilePath.isEmpty()) {
        return false;
    }

    const QString severity = m_checkItemSeverity.value(
        checkItemSeverityKey(layerName, checkItemName), QStringLiteral("未配置"));

    try {
        QJsonObject root;

        // 检查文件是否存在
        if (!QFile::exists(m_currentJsonFilePath)) {
            QJsonObject tmp;
            QMetaObject::invokeMethod(this, [&]() { tmp = generateJsonData(); }, Qt::BlockingQueuedConnection);
            root = tmp; // 文件不存在，创建新的
            return writeJsonToFile(root, m_currentJsonFilePath);
        }

        // 读取现有JSON文件
        QFile file(m_currentJsonFilePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        QByteArray jsonData = file.readAll();
        file.close();

        // 解析JSON
        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isNull() || !doc.isObject()) {
            // 解析失败，用新数据覆盖（在主线程生成）
            QJsonObject tmp;
            QMetaObject::invokeMethod(this, [&]() { tmp = generateJsonData(); }, Qt::BlockingQueuedConnection);
            root = tmp;
            return writeJsonToFile(root, m_currentJsonFilePath);
        }
        root = doc.object();

        // 泛化查找 Geopdf 节点键（优先无索引，其次按行号，最后按第一个匹配）
        QString geopdfKeyQStr;
        if (root.contains(QStringLiteral("Geopdf"))) {
            geopdfKeyQStr = QStringLiteral("Geopdf");
        } else if (root.contains(QString("Geopdf%1").arg(row + 1))) {
            geopdfKeyQStr = QString("Geopdf%1").arg(row + 1);
        } else {
            for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
                if (it.key().startsWith("Geopdf")) { geopdfKeyQStr = it.key(); break; }
            }
        }
        if (geopdfKeyQStr.isEmpty() || !root.contains(geopdfKeyQStr)) {
            return false;
        }

        // --- 注意：Qt JSON 对象是“写时复制”的，修改时需要小心 ---
        QJsonObject geopdfNode = root[geopdfKeyQStr].toObject(); // 1. 获取 Geopdf 节点副本
        bool found = false;

        if (geopdfNode.contains("tasks") && geopdfNode["tasks"].isObject()) {
            QJsonObject mapTasks = geopdfNode["tasks"].toObject(); // 2. 获取 Tasks 副本

            for (auto taskIt = mapTasks.begin(); taskIt != mapTasks.end(); ++taskIt) {
                if (taskIt.key().startsWith("task")) {
                    QJsonObject task = taskIt.value().toObject(); // 3. 获取 Task 副本
                    if (task.contains("layer_name") && task["layer_name"].toString() == layerName) {

                        for (auto checkIt = task.begin(); checkIt != task.end(); ++checkIt) {
                            if (checkIt.key().startsWith("check_item")) {
                                QJsonObject itemJson = checkIt.value().toObject(); // 4. 获取 Item 副本
                                if (itemJson.contains("name") && itemJson["name"].toString() == checkItemName) {

                                    // 5. 修改 Item 副本
                                    if (!result.id.isEmpty()) {
                                        itemJson["id"] = result.id;
                                    }
                                    itemJson["status"] = result.status;
                                    itemJson["err_exist"] = result.err_exist;
                                    itemJson["info"] = result.info;
                                    itemJson["errors"] = result.errors;
                                    // 兼容任务中途续跑或既有总 JSON：每次回写结果时
                                    // 均补齐当前质检项的严重程度。
                                    itemJson.remove(QStringLiteral("importance"));
                                    itemJson["severity"] = severity;
                                    found = true;

                                    // 6. 将修改后的 Item 写回 Task
                                    task[checkIt.key()] = itemJson;
                                    break;
                                }
                            }
                        }
                        if (found) {
                            // 7. 将修改后的 Task 写回 mapTasks
                            mapTasks[taskIt.key()] = task;
                            break;
                        }
                    }
                }
            }
            if (found) {
                // 8. 将修改后的 mapTasks 写回 geopdfNode
                geopdfNode["tasks"] = mapTasks;
            }
        }

        // *** 此处开始的旧代码块已被删除 ***

        // 重新计算当前地图的统计信息（仅单幅地图）
        if (found && geopdfNode.contains("tasks")) {
            QJsonObject mapTasks = geopdfNode["tasks"].toObject(); // 获取更新后的 tasks
            int map_tasks_num = 0, map_completed_num = 0, map_err_num = 0, map_uncompleted_num = 0;

            for (auto taskIt = mapTasks.constBegin(); taskIt != mapTasks.constEnd(); ++taskIt) {
                if (taskIt.key().startsWith("task")) {
                    QJsonObject task = taskIt.value().toObject();
                    map_tasks_num++; // tasks_num 按 task 数量统计
                    for (auto checkIt = task.constBegin(); checkIt != task.constEnd(); ++checkIt) {
                        if (checkIt.key().startsWith("check_item")) {
                            QJsonObject itemJson = checkIt.value().toObject();
                            QString status = itemJson["status"].toString();
                            if (status == "质检成功" || status == "质检完成") map_completed_num++;
                            else map_uncompleted_num++; // 非成功均计入未完成
                            if (itemJson.value("err_exist").toBool()) map_err_num++;
                        }
                    }
                }
            }

            QJsonObject mapInfo = geopdfNode["info"].toObject(); // 9. 获取 Info 副本
            mapInfo["tasks_num"] = map_tasks_num;
            mapInfo["completed_num"] = map_completed_num;
            mapInfo["err_num"] = map_err_num;
            mapInfo["uncompleted_num"] = map_uncompleted_num;
            geopdfNode["info"] = mapInfo; // 10. 将修改后的 Info 写回 geopdfNode
        }

        // 更新全局时间
        if (root.contains("timing") && root["timing"].isObject()) {
            QJsonObject timing = root["timing"].toObject(); // 获取副本
            timing["end_time"] = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
            root["timing"] = timing; // 写回
        }

        // 11. 将修改后的 geopdfNode 写回 root
        root[geopdfKeyQStr] = geopdfNode;

        // 写入文件
        if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QJsonDocument updatedDoc(root);
            file.write(updatedDoc.toJson(QJsonDocument::Indented));
            file.close();
            return true;
        }
    }
    catch (const std::exception& e) {
        QJsonObject newData;
        QMetaObject::invokeMethod(this, [&]() { newData = generateJsonData(); }, Qt::BlockingQueuedConnection);
        return writeJsonToFile(newData, m_currentJsonFilePath);
    }

    return false;
}


void map_check_task_manager::refreshAnnotationContentCheckResults(int row)
{
    const QString keyPrefix = QString::number(row) + "_";
    for (auto it = m_checkResults.begin(); it != m_checkResults.end(); ++it)
    {
        if (!it.key().startsWith(keyPrefix) || it.value().id != "213")
            continue;

        CheckItemResult refreshed = it.value();
        const QString jsonPath = refreshed.info.value("result_json").toString();
        QFile jsonFile(jsonPath);
        if (jsonPath.isEmpty() || !jsonFile.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        const QJsonDocument document = QJsonDocument::fromJson(jsonFile.readAll());
        jsonFile.close();
        if (!document.isObject())
            continue;

        const QJsonObject report = document.object();
        const QJsonArray groups = report.value("results").toArray();
        const QJsonObject summary = report.value("summary").toObject();

        int totalItems = summary.value("total_items").toInt(-1);
        if (totalItems < 0)
        {
            totalItems = 0;
            for (const QJsonValue &groupValue : groups)
                totalItems += groupValue.toObject().value("items").toArray().size();
        }

        refreshed.status = QStringLiteral("质检成功");
        refreshed.err_exist = totalItems > 0;
        // 跨类型校验会改写 213 单项报告；同步其最新说明，
        // 防止总 JSON 退回为首次执行时的固定文案。
        const QJsonValue reportInfo = report.value("info");
        const QString reportDescription = (reportInfo.isObject()
            ? reportInfo.toObject().value("description")
            : reportInfo).toString().trimmed();
        if (!reportDescription.isEmpty())
            refreshed.info["description"] = reportDescription;

        refreshed.info["total_groups"] = summary.value("total_groups").toInt(groups.size());
        refreshed.info["total_items"] = totalItems;
        const QString resultShpPath = refreshed.info.value("result_shp").toString();
        if (totalItems <= 0 || resultShpPath.isEmpty() || !QFile::exists(resultShpPath))
            refreshed.info.remove("result_shp");
        refreshed.errors = QJsonObject();
        for (int i = 0; i < groups.size(); ++i)
            refreshed.errors[QStringLiteral("group_%1").arg(i + 1)] = groups.at(i);

        it.value() = refreshed;
    }
}

void map_check_task_manager::initAlgorithmMap()
{
    // 绑定质检项名称与对应的算法函数（一一对应）
    m_algorithmMap.insert("比例尺数值检查", algorithm_ScaleValueCheck);
    m_algorithmMap.insert("要素层缺失检查", FeatureLayerMissingCheck);
    m_algorithmMap.insert("要素缺失检查", FeatureMissingCheck);
    m_algorithmMap.insert("要素错绘检查", FeatureMisplacementCheck);
    m_algorithmMap.insert("绝对偏移检查", AbsoluteOffsetCheck);
    m_algorithmMap.insert("几何走向异常检查", checkWaterSystemDirection);
    m_algorithmMap.insert("最小弯曲阈值控制", MinBendThresholdCheck);
    m_algorithmMap.insert("特征点限差控制", FeaturePointToleranceChecker);
    m_algorithmMap.insert("光滑度阈值控制", SmoothnessChecker);
    m_algorithmMap.insert("直角化角度阈值控制", checkRightAngleThreshold);
    m_algorithmMap.insert("削尖角度阈值控制", AngleCheckInterface);
    m_algorithmMap.insert("面要素重叠间隙检查", checkPolygonOverlapGap);
    m_algorithmMap.insert("线要素拓扑检查", checkLineTopology);
    m_algorithmMap.insert("关系检查", ContainmentCheckInterface);
    m_algorithmMap.insert("全局空白检查", GlobalBlankCheckWrapper);
    m_algorithmMap.insert("要素表示冲突检查", FeatureConflictCheckWrapper);
    m_algorithmMap.insert("要素密度检查", FeatureDensityCheckWrapper);
    // 501 由图幅接边 / 跨比例尺专用入口执行；注册在此用于统一算法定义。
    m_algorithmMap.insert("图幅接边与系列化表达", SeriesMapTemplateCheckWrapper);
    m_algorithmMap.insert("境界线越级检查", BoundaryMissingLevelCheckWrapper);
    m_algorithmMap.insert("行政区划面套合检查", AdministrativeDivisionFitCheckWrapper);
    m_algorithmMap.insert("境界线缺失和跳绘检查", BoundaryJumpDrawCheckWrapper);
    m_algorithmMap.insert(QStringLiteral("点要素孤立检查"), PointIsolationCheckWrapper);
    m_algorithmMap.insert(QStringLiteral("要素冗余检查"), FeatureRedundancyCheckWrapper);
    m_algorithmMap.insert("要素间隔检查", FeatureSpacingCheckWrapper);
    m_algorithmMap.insert("飞地漏绘检查", EnclaveMissingCheckWrapper);
    // 新增算法时，在此处添加映射即可
    // ========== 注记检查算法 ==========
    m_algorithmMap.insert("字体", AnnotationFontCheckWrapper);
    m_algorithmMap.insert("字大", AnnotationFontSizeCheckWrapper);
    m_algorithmMap.insert("字体颜色", AnnotationColorCheckWrapper);
    m_algorithmMap.insert("字体斜体", AnnotationItalicCheckWrapper);
    m_algorithmMap.insert("重叠检查", AnnotationOverlapCheckWrapper);
    m_algorithmMap.insert("注记匹配", AnnotationMatchFeaturesWrapper);
    m_algorithmMap.insert("线要素注记间距", GroupSpacingCheckWrapper);
    m_algorithmMap.insert("面要素等间距标注", EqualSpacingCheckWrapper);
    m_algorithmMap.insert("符号方向性检查", SymbolDirectionCheckWrapper);
    m_algorithmMap.insert("符号注记匹配", SymbolAnnotationMatchCheckWrapper);
    m_algorithmMap.insert("字距", CharSpacingCheckWrapper);
    m_algorithmMap.insert("政区名称注记错误/缺失检查", AdminNameAnnotationCheckWrapper);
    m_algorithmMap.insert("敏感文字合规性检查", SensitiveContentCheckWrapper);
    m_algorithmMap.insert("注记内容合规性", AnnotationContentCheckWrapper);
    m_algorithmMap.insert("境界线色带渲染合规性检查", BoundaryBandRenderCheckWrapper);
    
    // ========== 地图样式检查算法 ==========
    m_algorithmMap.insert("图名", MapStyleCheckWrapper);
    m_algorithmMap.insert("制图说明检查", MapAnnotationCheckWrapper);
    m_algorithmMap.insert("行政区划面颜色检查", AdminColorCheckWrapper);
    m_algorithmMap.insert("投影参数准确性", ProjectionParameterCheckWrapper);
    m_algorithmMap.insert("标准图例匹配检查", LegendMatchCheckWrapper);
    m_algorithmMap.insert("图名与行政区划匹配检查", MapNameMatchCheckWrapper);
    m_algorithmMap.insert("必须图层检查", RequiredLayersCheckWrapper);
    m_algorithmMap.insert("内容与图廓错位检查", ContentFrameMisalignmentCheckWrapper);
    m_algorithmMap.insert("要素超出边界检查", FeatureOutOfBoundaryCheckWrapper);
    m_algorithmMap.insert("行政区划设色检查", AdminColorStyleCheckWrapper);
}

AlgorithmFilePaths map_check_task_manager::getFilePathsByRow(int row)
{
    AlgorithmFilePaths paths;
    QTableWidget* table = ui->tableWidget_GeopdfPath;

    // 从表格项的UserRole中获取完整路径（之前存储的真实路径）
    auto getItemPath = [&](int col) -> QString {
        QTableWidgetItem* item = table->item(row, col);
        return item ? item->data(Qt::UserRole).toString().trimmed() : "";
    };

    paths.pdfPath = getItemPath(0);     // 第0列：地图PDF路径
    paths.shpPath = getItemPath(1);     // 第1列：SHP文件夹路径
    paths.gdbShpPath = getItemPath(2);  // 第2列：GDB_SHP文件夹路径
    paths.legendDir = getItemPath(3);   // 第3列：标准图例文件夹路径
    paths.csvPath = getItemPath(4);     // 第4列：图面配置CSV路径

    return paths;
}

void map_check_task_manager::updateCheckItemStatusIcon(QTreeWidgetItem* checkItem, const CheckItemResult& result)
{
    if (!checkItem) return;

    // 根据结果设置图标
    CompletionStatus status;
    if (result.status == "质检成功" || result.status == "质检完成") {
        // 图标表达任务是否已成功完成；err_exist 表示发现待复核要素，
        // 由结果 JSON 和错误图层展示，不应把成功任务显示为失败或警告。
        status = CompletionStatus::Passed;
    }
    else if (result.status == "质检失败") {
        status = CompletionStatus::HasErrors;
    }
    else {
        status = CompletionStatus::NotChecked;
    }
    setCompletionStatusIcon(checkItem, status);
}

QMap<QString, CsvLayerInfo> map_check_task_manager::parseCsvToLayerMap(const QString& csvPath)
{
    QMap<QString, CsvLayerInfo> layerMap;
    QFile csvFile(csvPath);

    // 检查CSV文件存在性
    if (!csvFile.exists()) {
        // 工作线程内不弹框，直接返回空映射
        return layerMap;
    }

    // 打开CSV（UTF-8编码，兼容中文）
    if (!csvFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // 工作线程内不弹框，直接返回空映射
        return layerMap;
    }

    QTextStream in(&csvFile);
    in.setCodec("UTF-8");
    bool isHeader = true; // 跳过表头行

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        // 跳过表头行（验证表头格式是否正确）
        if (isHeader) {
            QStringList headerCols = line.split(",", Qt::KeepEmptyParts);
            // 校验表头是否包含必要字段（避免CSV格式错误）
            if (headerCols.size() < 6 ||
                headerCols[0] != "标准图层" ||
                headerCols[1] != "gdb图层" ||
                headerCols[2] != "实际图层" ||
                headerCols[3] != "实际注记图层" ||
                headerCols[4] != "对应注记层" ||
                headerCols[5] != "图例字段") {
                // 工作线程内不弹框，直接返回空映射
                csvFile.close();
                return layerMap;
            }
            isHeader = false;
            continue;
        }

        // 按逗号分割6列数据（保留空值，确保列数匹配）
        QStringList cols = line.split(",", Qt::KeepEmptyParts);
        while (cols.size() < 6) cols << ""; // 补全空列，避免索引越界

        // 提取各列数据（按CSV顺序映射）
        QString standardLayer = cols[0].trimmed();  // 第1列：标准图层（与子任务名称匹配）
        QString gdbLayer = cols[1].trimmed();       // 第2列：gdb图层
        QString actualLayer = cols[2].trimmed();    // 第3列：实际图层
        QString actualAnnotLayer = cols[3].trimmed();// 第4列：实际注记图层
        QString correspondingAnnotLayer = cols[4].trimmed(); // 第5列：对应注记层
        QString legendField = cols[5].trimmed();    // 第6列：图例字段


        if (!standardLayer.isEmpty()) {
            CsvLayerInfo layerInfo;
            
            // 存储图例字段
            layerInfo.legendField = legendField;
            layerInfo.correspondingAnnotLayer = correspondingAnnotLayer;

            // 1. 拆分gdb图层（支持单个/多个，按|分割）
            if (!gdbLayer.isEmpty()) {
                QStringList gdbNames = gdbLayer.split("|", Qt::SkipEmptyParts);
                for (QString& name : gdbNames) {
                    name = name.trimmed();
                    if (!name.isEmpty()) {
                        layerInfo.gdbLayerNames.append(name);
                    }
                }
            }

            // 2. 拆分实际图层（支持单个/多个，按|分割）
            if (!actualLayer.isEmpty()) {
                QStringList actualNames = actualLayer.split("|", Qt::SkipEmptyParts);
                for (QString& name : actualNames) {
                    name = name.trimmed();
                    if (!name.isEmpty()) {
                        layerInfo.actualLayerNames.append(name);
                    }
                }
            }

            // 3. 拆分注记图层（支持单个/多个，按|分割）
            if (!actualAnnotLayer.isEmpty()) {
                QStringList annotNames = actualAnnotLayer.split("|", Qt::SkipEmptyParts);
                for (QString& name : annotNames) {
                    name = name.trimmed();
                    if (!name.isEmpty()) {
                        layerInfo.actualAnnotLayerNames.append(name);
                    }
                }
            }

            layerMap[standardLayer] = layerInfo;
        }
    }

    csvFile.close();
    return layerMap;
}

//执行算法线程主体
void map_check_task_manager::executeAlgorithms(std::atomic<bool>& stopFlag, std::atomic<bool>& pauseFlag, AlgorithmThread* thread)
{
    // 使用主线程预先构建的快照，避免在工作线程读取任何 UI 控件
    const int rowCount = m_mapRowsSnapshot.size();
    if (rowCount == 0) return;

    int completedMaps = 0;

    // 统计总地图数量用于进度（按地图）
    // 先预计算需要执行的质检项总数（按图幅 + 图层 + 质检项），用于更细粒度的进度更新
    const QString scaleCheckItemName = QStringLiteral("比例尺数值检查");
    int totalItems = 0;
    for (int r = 0; r < rowCount; ++r) {
        AlgorithmFilePaths fpaths = m_mapRowsSnapshot[r].paths;
        QMap<QString, CsvLayerInfo> layerMapPreview = parseCsvToLayerMap(fpaths.csvPath);
        if (layerMapPreview.isEmpty()) continue;
        for (const auto& layerTask : m_layerTasksSnapshot) {
            QString layerName = layerTask.layerName;
            const bool springTask = !springTargetFromTaskLayer(layerName).isEmpty() &&
                layerTask.checkItemNames.contains(QStringLiteral("关系检查"));
            if (!layerMapPreview.contains(layerName) &&
                !layerTask.checkItemNames.contains(QStringLiteral("全局空白检查")) &&
                !layerTask.checkItemNames.contains(QStringLiteral("要素表示冲突检查")) &&
                !layerTask.checkItemNames.contains(QStringLiteral("要素密度检查")) && !springTask) continue;
            for (const QString& checkItemName : layerTask.checkItemNames) {
                if (checkItemName.isEmpty() || isSeriesEdgeMatchItem(QString(), checkItemName)) continue;
                if (!layerMapPreview.contains(layerName) &&
                    checkItemName != QStringLiteral("全局空白检查") &&
                    checkItemName != QStringLiteral("要素表示冲突检查") &&
                    checkItemName != QStringLiteral("要素密度检查") &&
                    !(springTask && checkItemName == QStringLiteral("关系检查"))) continue;
                // 只有当算法映射中存在对应算法时才统计（避免统计无法执行的项）
                if (m_algorithmMap.contains(checkItemName)) totalItems++;
            }
        }
    }
    if (totalItems <= 0) return; // 不把“没有可执行项”伪装成完成

    int completedItems = 0; // 每完成一个质检项就 ++
    const auto finishInterruptedRun = [&]() {
        m_completedCheckCount.store(completedItems);
        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
            Q_ARG(QString, QString("质检已终止：终止前共执行 %1 项。").arg(completedItems)));
    };

    for (int row = 0; row < rowCount; ++row) {
        if (stopFlag.load()) {
            finishInterruptedRun();
            return;
        }
        while (pauseFlag.load()) QThread::msleep(200);

        const QDateTime mapStartedAt = QDateTime::currentDateTime();
        m_currentTaskRow = row; // 记录当前地图所在行，供 JSON 生成使用
        AlgorithmFilePaths filePaths = m_mapRowsSnapshot[row].paths;
        QString mapName = m_mapRowsSnapshot[row].mapName;

        // 显示当前地图及序号
        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
            Q_ARG(QString, QString("正在质检第%1/%2幅：%3").arg(row + 1).arg(rowCount).arg(mapName)));

        // 每幅图开始时切换任务树状态。该调用与后续图标更新都在 GUI
        // 线程排队执行，因此上一幅图的迟到更新会被行号保护丢弃。
        QMetaObject::invokeMethod(this, "beginMapTaskStatus", Qt::QueuedConnection,
            Q_ARG(int, row));

        // 每幅图创建独立 JSON 文件路径
        QString jsonName = QString("%1_result_%2.json")
            .arg(mapName)
            .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmsszzz"));
        QString outputDir = m_outputDirSnapshot;
        if (outputDir.isEmpty()) {
            outputDir = QDir::currentPath() + "/output_results";
            QDir dir;
            dir.mkpath(outputDir);
            QMetaObject::invokeMethod(this, [this, outputDir]() {
                ui->lineEdit_OutputLogPath->setText(outputDir);
                }, Qt::QueuedConnection);
        }
        
        // 为每幅地图创建独立的输出子目录
        QString mapOutputDir = QDir(outputDir).filePath(mapName);
        QDir dir;
        dir.mkpath(mapOutputDir);
        m_saveDir = mapOutputDir;
        m_currentJsonFilePath = QDir(mapOutputDir).filePath(jsonName);
        const QString errorResultDir = QDir(mapOutputDir).filePath(QStringLiteral("质检错误结果"));
        // 记录本图本次运行生成的最终错误图层；所有检查结束后再统一移动，
        // 这样 213 等交叉验证在运行期间始终能从根目录读取前置 shp。
        QHash<QString, QString> generatedErrorLayerSeverities;
        LineTopologySharedTasks::Cache sharedCanalChecks;

        // ---------- 1. 执行比例尺数值检查（仅该项） ----------
        const QString scaleCheckItemName = QStringLiteral("比例尺数值检查");
        bool scaleCheckCompleted = false;

        QMap<QString, CsvLayerInfo> layerMap = parseCsvToLayerMap(filePaths.csvPath);
        if (layerMap.isEmpty()) {
            // 如果 CSV 解析失败，跳过此地图，仍显示序号与地图名
            m_skippedMapCount.fetch_add(1);
            QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                Q_ARG(QString, QString("[跳过] 第%1/%2幅：%3，图面配置 CSV 不存在、无法读取或格式不正确。")
                    .arg(row + 1).arg(rowCount).arg(mapName)));
            continue;
        }

        for (const auto& layerTask : m_layerTasksSnapshot) {
            if (stopFlag.load()) {
                finishInterruptedRun();
                return;
            }
            while (pauseFlag.load()) QThread::msleep(200);
            QString layerName = layerTask.layerName;
            if (!layerMap.contains(layerName)) continue;
            CsvLayerInfo matchedLayers = layerMap[layerName];
            buildShpPaths(filePaths, matchedLayers);
            
            // 【重要】设置标准图层名称，供算法使用（用于文件命名和XML匹配）
            filePaths.standardLayer = layerName;
            
            // 只传递与当前图层名称匹配的 XML 文件
            QString matchedXml = MatchXmlByLayerName(m_xmlPaths, layerName);
            filePaths.xmlPaths = matchedXml.isEmpty() ? m_xmlPaths : QStringList() << matchedXml;

            for (const QString& checkItemName : layerTask.checkItemNames) {
                if (stopFlag.load()) {
                    finishInterruptedRun();
                    return;
                }
                while (pauseFlag.load()) QThread::msleep(200);

                if (checkItemName.isEmpty() || isSeriesEdgeMatchItem(QString(), checkItemName)) continue;
                const QString uniqueKey = QString("%1_%2_%3").arg(row).arg(layerName).arg(checkItemName);
                if (checkItemName == scaleCheckItemName && m_algorithmMap.contains(checkItemName)) {
                    CheckItemResult result;
                    result.name = checkItemName;
                    try {
                        // 在label_log显示当前正在进行的质检项（比例尺检查）
                        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                            Q_ARG(QString, QString("正在质检第%1/%2幅：%3 | 正在检查质检项：%4（图层：%5）")
                                .arg(row + 1).arg(rowCount).arg(mapName)
                                .arg(checkItemName).arg(layerName)));
                        m_algorithmMap[checkItemName](checkItemName, filePaths, 0, "", result);
                        m_checkResults.insert(uniqueKey, result);
                        // 写入 JSON（创建或更新）
                        QJsonObject jsonTmp;
                        QMetaObject::invokeMethod(this, [&]() { jsonTmp = generateJsonData(); }, Qt::BlockingQueuedConnection);
                        writeJsonToFile(jsonTmp, m_currentJsonFilePath);
                        updateJsonFileWithResult(row, layerName, checkItemName, result);
                        // 更新图标基于本次算法结果
                        QMetaObject::invokeMethod(this, [this, row, layerName, checkItemName, result]() {
                            updateCheckItemStatusIconByName(row, layerName, checkItemName, result);
                        }, Qt::QueuedConnection);
                        scaleCheckCompleted = true;
                        m_succeededCheckCount.fetch_add(1);
                        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                            Q_ARG(QString, QString("[完成] %1 / %2；状态：%3；发现问题：%4")
                                .arg(layerName, checkItemName,
                                     result.status.trimmed().isEmpty() ? QStringLiteral("已完成") : result.status,
                                     result.err_exist ? QStringLiteral("是") : QStringLiteral("否"))));
                        // 完成一个质检项 -> 更新按项计的进度
                        completedItems++;
                        {
                            int prog = static_cast<int>(100.0 * completedItems / totalItems);
                            emit thread->progressChanged(prog);
                        }
                    } catch (const std::exception& e) {
                        result.status = "质检失败";
                        result.err_exist = true;
                        QJsonObject errObj;
                        errObj["description"] = QString("比例尺检查异常：%1").arg(e.what());
                        result.errors["error1"] = errObj;
                        updateJsonFileWithResult(row, layerName, checkItemName, result);
                        m_failedCheckCount.fetch_add(1);
                        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                            Q_ARG(QString, QString("[失败] %1 / %2；比例尺检查异常：%3")
                                .arg(layerName, checkItemName, QString::fromLocal8Bit(e.what()))));
                        // 异常时也要更新图标
                        QMetaObject::invokeMethod(this, [this, row, layerName, checkItemName, result]() {
                            updateCheckItemStatusIconByName(row, layerName, checkItemName, result);
                        }, Qt::QueuedConnection);
                        // 即使异常也当作完成一个质检项，更新进度
                        completedItems++;
                        {
                            int prog = static_cast<int>(100.0 * completedItems / totalItems);
                            emit thread->progressChanged(prog);
                        }
                    }
                }
            }
        }

        // ---------- 2. 提取比例尺并刷新任务树 ----------
        if (scaleCheckCompleted) {
            int scale = getScaleFromJson();
            if (scale > 0) {
                // 阻塞调用，等待主线程刷新完成后我们再重建快照
                QMetaObject::invokeMethod(this, "refreshTaskTreeByScale", Qt::BlockingQueuedConnection,
                    Q_ARG(int, scale));
                // 刷新后重建任务快照
                QVector<LayerTaskSnapshot> newSnap;
                QMetaObject::invokeMethod(this, [&]() { newSnap = buildTaskSnapshot(); }, Qt::BlockingQueuedConnection);
                m_layerTasksSnapshot = newSnap;
            }
            else {
                // 无比例尺则保持原任务树
            }
        }

        // 刷新 JSON 模板
        QJsonObject refreshedJson;
        QMetaObject::invokeMethod(this, [&]() { refreshedJson = generateJsonData(); }, Qt::BlockingQueuedConnection);
        writeJsonToFile(refreshedJson, m_currentJsonFilePath);

        // 122 是整幅图检查，同一图即使被多个任务节点引用也只运行一次。
        bool globalBlankDone = false;
        CheckItemResult globalBlankResult;

        // ---------- 3. 执行除了比例尺检查以外的所有算法（仅未质检项） ----------
        QVector<LayerTaskSnapshot> orderedTasks = m_layerTasksSnapshot;
        std::stable_partition(orderedTasks.begin(), orderedTasks.end(), [](const LayerTaskSnapshot& task) {
            return !task.checkItemNames.contains(QStringLiteral("要素密度检查"));
        });

        // 200 是一批任务的公共前置条件。必须先完成本图全部 200，再统一处理
        // 共用注记的归属冲突，之后才能放行任何读取 200 中间层的检查项。
        const QString annotationMatchItemName = QStringLiteral("注记匹配");
        QVector<AnnotationMatchBatchTask> annotationMatchBatch;
        QStringList annotationMatchKeys;
        QStringList annotationMatchLayers;
        bool annotationMatchBarrierSucceeded = true;
        QString annotationMatchBarrierError;
        const int annotationMatchScale = getScaleFromJson();

        for (const LayerTaskSnapshot& layerTask : orderedTasks) {
            if (!layerTask.checkItemNames.contains(annotationMatchItemName))
                continue;
            if (stopFlag.load()) {
                finishInterruptedRun();
                return;
            }
            while (pauseFlag.load()) QThread::msleep(200);

            const QString layerName = layerTask.layerName;
            if (layerName.isEmpty() || !layerMap.contains(layerName))
                continue;

            AlgorithmFilePaths matchPaths = filePaths;
            buildShpPaths(matchPaths, layerMap.value(layerName));
            matchPaths.standardLayer = layerName;
            const QString matchedXml = MatchXmlByLayerName(m_xmlPaths, layerName);
            matchPaths.xmlPaths = matchedXml.isEmpty()
                ? m_xmlPaths : QStringList() << matchedXml;

            const QString uniqueKey = QString("%1_%2_%3")
                .arg(row).arg(layerName).arg(annotationMatchItemName);
            CheckItemResult result;
            result.name = annotationMatchItemName;
            try {
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                    Q_ARG(QString, QString("正在质检第%1/%2幅：%3 | 正在检查质检项：%4（图层：%5）")
                        .arg(row + 1).arg(rowCount).arg(mapName)
                        .arg(annotationMatchItemName).arg(layerName)));

                runAlgorithmWithThreadAffinity(annotationMatchItemName,
                    m_algorithmMap.value(annotationMatchItemName), matchPaths,
                    annotationMatchScale, m_saveDir, result);
                m_checkResults.insert(uniqueKey, result);
                updateJsonFileWithResult(row, layerName, annotationMatchItemName, result);
                QMetaObject::invokeMethod(this,
                    [this, row, layerName, result, annotationMatchItemName]() {
                        updateCheckItemStatusIconByName(
                            row, layerName, annotationMatchItemName, result);
                    }, Qt::QueuedConnection);

                if (result.status == QStringLiteral("质检成功")) {
                    const QString outputPath = result.info.value("output_layer").toString();
                    // “实际注记图层不存在”是合法空任务；只有确实生成了 200 结果的
                    // 任务才进入批次交叉验证。
                    if (!outputPath.isEmpty()) {
                        AnnotationMatchBatchTask task;
                        task.standardLayer = result.info.value("standard_layer").toString();
                        task.sourcePath = result.info.value("gdb_layer").toString();
                        task.xmlPath = result.info.value("xml_path").toString();
                        task.actualPath = result.info.value("actual_layer").toString();
                        task.annotationPath = result.info.value("anno_layer").toString();
                        task.outputPath = outputPath;
                        QStringList missingInputs;
                        if (task.standardLayer.isEmpty()) missingInputs.append(QStringLiteral("标准图层"));
                        if (task.sourcePath.isEmpty()) missingInputs.append(QStringLiteral("GDB图层"));
                        if (task.xmlPath.isEmpty()) missingInputs.append(QStringLiteral("规则XML"));
                        if (task.annotationPath.isEmpty()) missingInputs.append(QStringLiteral("实际注记图层"));
                        if (task.outputPath.isEmpty()) missingInputs.append(QStringLiteral("200输出图层"));
                        // actualPath 对地理名称、山脉、自然保护区等纯注记任务本来就允许为空；
                        // AnnotationMatchFeaturesInternal 也把 actualLayer 明确定义为可选输入。
                        if (!missingInputs.isEmpty()) {
                            const QString taskInputError = QStringLiteral(
                                "200 已生成结果，但当前任务缺少交叉验证输入：%1")
                                .arg(missingInputs.join(QStringLiteral("、")));
                            result.status = QStringLiteral("质检失败");
                            result.err_exist = true;
                            result.errors["cross_validation_input"] =
                                taskInputError;
                            m_checkResults[uniqueKey] = result;
                            updateJsonFileWithResult(
                                row, layerName, annotationMatchItemName, result);
                        } else {
                            annotationMatchBatch.append(task);
                            annotationMatchKeys.append(uniqueKey);
                            annotationMatchLayers.append(layerName);
                        }
                    }
                }
                if (result.status == QStringLiteral("质检成功"))
                    m_succeededCheckCount.fetch_add(1);
                else
                    m_failedCheckCount.fetch_add(1);
                QMetaObject::invokeMethod(this,
                    [this, row, layerName, result, annotationMatchItemName]() {
                        updateCheckItemStatusIconByName(
                            row, layerName, annotationMatchItemName, result);
                    }, Qt::QueuedConnection);
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                    Q_ARG(QString, QString("[完成] %1 / %2；状态：%3；发现问题：%4")
                        .arg(layerName, annotationMatchItemName,
                            result.status.trimmed().isEmpty()
                                ? QStringLiteral("已完成") : result.status,
                            result.err_exist ? QStringLiteral("是") : QStringLiteral("否"))));
            } catch (const std::exception& e) {
                result.id = QStringLiteral("200");
                result.status = QStringLiteral("质检失败");
                result.err_exist = true;
                result.errors["exception"] = QStringLiteral("算法执行异常：%1")
                    .arg(QString::fromLocal8Bit(e.what()));
                m_checkResults.insert(uniqueKey, result);
                updateJsonFileWithResult(row, layerName, annotationMatchItemName, result);
                m_failedCheckCount.fetch_add(1);
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                    Q_ARG(QString, QString("[失败] %1 / %2；算法执行异常：%3")
                        .arg(layerName, annotationMatchItemName,
                            QString::fromLocal8Bit(e.what()))));
                QMetaObject::invokeMethod(this,
                    [this, row, layerName, result, annotationMatchItemName]() {
                        updateCheckItemStatusIconByName(
                            row, layerName, annotationMatchItemName, result);
                    }, Qt::QueuedConnection);
            }

            ++completedItems;
            emit thread->progressChanged(
                static_cast<int>(100.0 * completedItems / totalItems));
        }

        if (!annotationMatchBatch.isEmpty()) {
            QString crossValidationSummary = annotationMatchBarrierError;
            QStringList crossValidationAmbiguities;
            if (annotationMatchBarrierSucceeded) {
                annotationMatchBarrierSucceeded = ReconcileAnnotationMatchBatch(
                    annotationMatchBatch, annotationMatchScale,
                    crossValidationSummary, crossValidationAmbiguities);
                if (!annotationMatchBarrierSucceeded)
                    annotationMatchBarrierError = crossValidationSummary;
            }
            if (crossValidationSummary.isEmpty())
                crossValidationSummary = QStringLiteral("200 批次交叉验证失败");

            QJsonArray ambiguityJson;
            for (const QString& ambiguity : crossValidationAmbiguities)
                ambiguityJson.append(ambiguity);

            for (int i = 0; i < annotationMatchKeys.size(); ++i) {
                CheckItemResult result = m_checkResults.value(annotationMatchKeys.at(i));
                result.info["output_layer"] = annotationMatchBatch.at(i).outputPath;
                result.info["cross_validation"] = crossValidationSummary;
                result.info["cross_validation_status"] = annotationMatchBarrierSucceeded
                    ? QStringLiteral("成功") : QStringLiteral("失败");
                result.info["cross_validation_ambiguities"] = ambiguityJson;
                if (!annotationMatchBarrierSucceeded) {
                    // 交叉验证是对已成功 200 结果的归属修正。技术性失败时保留原始
                    // 200 结果并给出警告，不能把一个共享注记组的失败扩散成全图
                    // 所有 200 失败，更不能阻断 210/213 产生基础检查结果。
                    result.info["cross_validation_warning"] = crossValidationSummary;
                }
                const QString standaloneReportPath =
                    result.info.value("result_json").toString();
                if (!standaloneReportPath.isEmpty()) {
                    const QJsonObject standaloneReport{
                        {"id", result.id}, {"name", result.name},
                        {"status", result.status}, {"err_exist", result.err_exist},
                        {"info", result.info}, {"errors", result.errors}
                    };
                    if (!writeJsonToFile(standaloneReport, standaloneReportPath))
                        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                            Q_ARG(QString, QStringLiteral("200交叉验证结果未能写入独立报告：%1")
                                .arg(standaloneReportPath)));
                }
                m_checkResults[annotationMatchKeys.at(i)] = result;
                updateJsonFileWithResult(row, annotationMatchLayers.at(i),
                    annotationMatchItemName, result);
                QMetaObject::invokeMethod(this,
                    [this, row, layerName = annotationMatchLayers.at(i), result,
                        annotationMatchItemName]() {
                        updateCheckItemStatusIconByName(
                            row, layerName, annotationMatchItemName, result);
                    }, Qt::QueuedConnection);
            }

            QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                Q_ARG(QString, QString("[%1] %2")
                    .arg(annotationMatchBarrierSucceeded
                        ? QStringLiteral("完成") : QStringLiteral("失败"),
                        crossValidationSummary)));
        }

        for (const auto& layerTask2 : orderedTasks) {
            if (stopFlag.load()) {
                finishInterruptedRun();
                return;
            }
            while (pauseFlag.load()) QThread::msleep(200);
            QString layerName = layerTask2.layerName;
            
            // 【修复】如果layerName为空，记录警告并跳过
            if (layerName.isEmpty()) {
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                    Q_ARG(QString, QString("警告：发现空图层名称，已跳过")));
                continue;
            }
            
            // 【修复】如果CSV中没有匹配的图层，记录警告并跳过
            const bool hasLayer = layerMap.contains(layerName);
            const bool springTask = !springTargetFromTaskLayer(layerName).isEmpty() &&
                layerTask2.checkItemNames.contains(QStringLiteral("关系检查"));
            if (!hasLayer && !layerTask2.checkItemNames.contains(QStringLiteral("全局空白检查")) &&
                !layerTask2.checkItemNames.contains(QStringLiteral("要素表示冲突检查")) &&
                !layerTask2.checkItemNames.contains(QStringLiteral("要素密度检查")) && !springTask) {
                QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                    Q_ARG(QString, QString("警告：CSV配置中未找到图层 '%1'，已跳过相关质检项").arg(layerName)));
                continue;
            }
            
            if (hasLayer) buildShpPaths(filePaths, layerMap.value(layerName));
            
            // 【重要】设置标准图层名称，供算法使用（用于文件命名和XML匹配）
            filePaths.standardLayer = layerName;
            
            // 【调试】输出当前处理的图层信息
            qDebug() << "[质检任务] 处理图层:" << layerName 
                     << "| 实际图层路径数:" << filePaths.actualShpList.size()
                     << "| 注记路径数:" << filePaths.annotShpList.size()
                     << "| 质检项数:" << layerTask2.checkItemNames.size();
            
            // 只传递与当前图层名称匹配的 XML 文件
            QString matchedXml = MatchXmlByLayerName(m_xmlPaths, layerName);
            filePaths.xmlPaths = layerTask2.checkItemNames.contains(QStringLiteral("全局空白检查"))
                ? m_xmlPaths : (matchedXml.isEmpty() ? m_xmlPaths : QStringList() << matchedXml);
            
            // 【调试】输出XML匹配结果
            if (!matchedXml.isEmpty()) {
                qDebug() << "[质检任务]   匹配到XML:" << QFileInfo(matchedXml).fileName();
            } else {
                qDebug() << "[质检任务]   警告：未匹配到专属XML，使用全部" << m_xmlPaths.size() << "个XML";
            }

            for (const QString& checkItemName : layerTask2.checkItemNames) {
                if (stopFlag.load()) {
                    finishInterruptedRun();
                    return;
                }
                while (pauseFlag.load()) QThread::msleep(200);
                if (checkItemName.isEmpty() || checkItemName == scaleCheckItemName
                    || isSeriesEdgeMatchItem(QString(), checkItemName)) continue;
                // 200 已在上面的批次前置阶段执行；此处不得重复覆盖交叉验证结果。
                if (checkItemName == annotationMatchItemName) continue;

                const QString uniqueKey = QString("%1_%2_%3").arg(row).arg(layerName).arg(checkItemName);
                if (!m_algorithmMap.contains(checkItemName)) {
                    QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                        Q_ARG(QString, QString("[跳过] %1 / %2：未注册对应算法。")
                            .arg(layerName, checkItemName)));
                    continue;
                }
                if (!hasLayer && checkItemName != QStringLiteral("全局空白检查") &&
                    checkItemName != QStringLiteral("要素表示冲突检查") &&
                    checkItemName != QStringLiteral("要素密度检查") &&
                    !(springTask && checkItemName == QStringLiteral("关系检查"))) continue;

                CheckItemResult result;
                result.name = checkItemName;
                try {
                    // 在label_log显示当前正在进行的质检项（常规检查）
                    QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                        Q_ARG(QString, QString("正在质检第%1/%2幅：%3 | 正在检查质检项：%4（图层：%5）")
                            .arg(row + 1).arg(rowCount).arg(mapName)
                            .arg(checkItemName).arg(layerName)));
                    int scaleVal = getScaleFromJson();
                    const AlgorithmFunc algorithm = m_algorithmMap.value(checkItemName);
                    const QString algorithmOutputDir = m_saveDir;
                    const QString severity = layerTask2.checkItemSeverities.value(
                        checkItemName, QStringLiteral("未配置"));
                    AlgorithmFilePaths runPaths = filePaths;
                    // 使用本图本次 200 交叉验证后的最终路径；未修正的图层仍用首轮路径。
                    const QString matchKey = QString("%1_%2_%3")
                        .arg(row).arg(layerName).arg(annotationMatchItemName);
                    if (m_checkResults.contains(matchKey))
                        runPaths.annotationMatchResultPath =
                            m_checkResults.value(matchKey).info.value("output_layer").toString();
                    if (springTask && checkItemName == QStringLiteral("关系检查")) {
                        runPaths.standardLayer = springTargetFromTaskLayer(layerName);
                        runPaths.actualShpSemicolon.clear();
                        runPaths.xmlPaths.clear();
                        for (const QString& path : m_xmlPaths) {
                            if (QFileInfo(path).completeBaseName() == layerName) {
                                runPaths.xmlPaths << path;
                                break;
                            }
                        }
                    }
                    if (checkItemName == QStringLiteral("全局空白检查")) {
                        runPaths.xmlPaths = m_xmlPaths;
                        runPaths.blankSeverity = severity;
                    }
                    if (checkItemName == QStringLiteral("要素表示冲突检查")) {
                        runPaths.standardLayer = conflictTargetFromTaskLayer(layerName);
                        for (const QString& path : m_xmlPaths) {
                            if (QFileInfo(path).completeBaseName() == layerName) {
                                runPaths.conflictXmlPath = path;
                                break;
                            }
                        }
                    }
                    if (checkItemName == QStringLiteral("要素密度检查")) {
                        runPaths.standardLayer = densityTargetFromTaskLayer(layerName);
                        runPaths.densitySeverity = severity;
                        for (const QString& path : m_xmlPaths) {
                            if (QFileInfo(path).completeBaseName() == layerName) {
                                runPaths.densityXmlPath = path;
                                break;
                            }
                        }
                        QJsonArray matches;
                        const QString rowPrefix = QString::number(row) + QChar('_');
                        for (auto it = m_checkResults.constBegin(); it != m_checkResults.constEnd(); ++it) {
                            if (!it.key().startsWith(rowPrefix) || it.value().id != QStringLiteral("200")) continue;
                            QJsonObject match = it.value().info;
                            match.insert(QStringLiteral("status"), it.value().status);
                            matches.append(match);
                        }
                        runPaths.densityMatchesJson = QString::fromUtf8(QJsonDocument(matches).toJson(QJsonDocument::Compact));
                    }
                    const QHash<QString, ErrorLayerFileState> errorLayersBefore =
                        snapshotFinalErrorLayers(errorResultDir);
                    const QString sharedCanalKey = LineTopologySharedTasks::taskKey(
                        checkItemName, runPaths,
                        MatchXmlByLayerName(runPaths.xmlPaths, runPaths.standardLayer), scaleVal);
                    const bool reusedSharedCanal =
                        sharedCanalChecks.find(sharedCanalKey, checkItemName, result);
                    if (reusedSharedCanal) {
                        if (!LineTopologySharedTasks::writeReusedOutput(
                                algorithmOutputDir, runPaths.standardLayer, result)) {
                            throw std::runtime_error("failed to write shared canal result reference");
                        }
                        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                            Q_ARG(QString, result.info.value("description").toString()));
                    } else if (checkItemName == QStringLiteral("全局空白检查") && globalBlankDone) {
                        result = globalBlankResult;
                    } else {
                        runAlgorithmWithThreadAffinity(
                            checkItemName, algorithm, runPaths, scaleVal, algorithmOutputDir, result);
                        if (checkItemName == QStringLiteral("全局空白检查")) {
                            globalBlankDone = true;
                        }
                    }
                    const QHash<QString, ErrorLayerFileState> errorLayersAfter =
                        snapshotFinalErrorLayers(errorResultDir);
                    const QStringList newErrorLayerPaths = changedFinalErrorLayers(
                        errorLayersBefore, errorLayersAfter);
                    appendErrorLayerPaths(result, newErrorLayerPaths);
                    if (!reusedSharedCanal) {
                        sharedCanalChecks.remember(sharedCanalKey, runPaths.standardLayer, result);
                    }
                    if (checkItemName == QStringLiteral("全局空白检查")) globalBlankResult = result;
                    for (const QString& path : newErrorLayerPaths) {
                        generatedErrorLayerSeverities.insert(path, severity);
                    }
                    m_checkResults.insert(uniqueKey, result);
                    if (result.id == "213") {
                        // 213 跨类型验证可能改写本图先前执行的 213 报告，统一重建汇总 JSON。
                        refreshAnnotationContentCheckResults(row);
                        QJsonObject jsonTmp;
                        QMetaObject::invokeMethod(this, [&]() { jsonTmp = generateJsonData(); }, Qt::BlockingQueuedConnection);
                        writeJsonToFile(jsonTmp, m_currentJsonFilePath);
                    } else {
                        updateJsonFileWithResult(row, layerName, checkItemName, result);
                    }
                    QMetaObject::invokeMethod(this, [this, row, layerName, checkItemName, result]() {
                        updateCheckItemStatusIconByName(row, layerName, checkItemName, result);
                    }, Qt::QueuedConnection);
                    m_succeededCheckCount.fetch_add(1);
                    QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                        Q_ARG(QString, QString("[完成] %1 / %2；状态：%3；发现问题：%4")
                            .arg(layerName, checkItemName,
                                 result.status.trimmed().isEmpty() ? QStringLiteral("已完成") : result.status,
                                 result.err_exist ? QStringLiteral("是") : QStringLiteral("否"))));
                    // 完成一个质检项 -> 更新按项计的进度
                    completedItems++;
                    {
                        int prog = static_cast<int>(100.0 * completedItems / totalItems);
                        emit thread->progressChanged(prog);
                    }
                } catch (const std::exception& e) {
                    result.status = "质检失败";
                    result.err_exist = true;
                    QJsonObject errObj;
                    errObj["description"] = QString("算法执行异常：%1").arg(e.what());
                    result.errors["error1"] = errObj;
                    updateJsonFileWithResult(row, layerName, checkItemName, result);
                    m_failedCheckCount.fetch_add(1);
                    QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                        Q_ARG(QString, QString("[失败] %1 / %2；算法执行异常：%3")
                            .arg(layerName, checkItemName, QString::fromLocal8Bit(e.what()))));
                    // 异常时也要更新图标
                    QMetaObject::invokeMethod(this, [this, row, layerName, checkItemName, result]() {
                        updateCheckItemStatusIconByName(row, layerName, checkItemName, result);
                    }, Qt::QueuedConnection);
                    // 异常也算作完成一个项，推进进度
                    completedItems++;
                    {
                        int prog = static_cast<int>(100.0 * completedItems / totalItems);
                        emit thread->progressChanged(prog);
                    }
                }
            }
        }

        // 兜底扫描：部分算法会覆盖既有文件或沿用旧时间戳，导致“运行前后差异”
        // 无法识别到新成果。整幅图执行结束后，按输出文件名中的标准图层和质检项
        // 反查严重程度，确保这些最终错误图层仍可归档。
        const QHash<QString, ErrorLayerFileState> finalErrorLayers =
            snapshotFinalErrorLayers(errorResultDir);
        int recoveredErrorLayerCount = 0;
        for (auto fileIt = finalErrorLayers.constBegin(); fileIt != finalErrorLayers.constEnd(); ++fileIt) {
            const QString errorLayerPath = fileIt.key();
            if (generatedErrorLayerSeverities.contains(errorLayerPath)) continue;

            const QString fileName = QFileInfo(errorLayerPath).completeBaseName();
            int bestMatchLength = -1;
            QString matchedSeverity;
            for (const LayerTaskSnapshot& layerTask : m_layerTasksSnapshot) {
                const QString layerName = layerTask.layerName.trimmed();
                if (layerName.isEmpty()
                    || !fileName.contains(layerName, Qt::CaseInsensitive)) continue;

                for (const QString& checkItemName : layerTask.checkItemNames) {
                    QString nameToken = checkItemName.trimmed();
                    nameToken.remove(QStringLiteral("检查"));
                    nameToken.remove(QStringLiteral("控制"));
                    nameToken.remove(QStringLiteral("合规性"));
                    if (nameToken.size() < 2
                        || !fileName.contains(nameToken, Qt::CaseInsensitive)
                        || nameToken.size() <= bestMatchLength) continue;

                    bestMatchLength = nameToken.size();
                    matchedSeverity = layerTask.checkItemSeverities.value(
                        checkItemName, QStringLiteral("未配置"));
                }
            }
            if (bestMatchLength >= 0) {
                generatedErrorLayerSeverities.insert(errorLayerPath, matchedSeverity);
                ++recoveredErrorLayerCount;
            }
        }

        const int archivedLayerCount = archiveErrorLayersBySeverity(
            row, errorResultDir, generatedErrorLayerSeverities);
        if (archivedLayerCount > 0 || recoveredErrorLayerCount > 0) {
            QJsonObject finalJson;
            QMetaObject::invokeMethod(this, [&]() { finalJson = generateJsonData(); }, Qt::BlockingQueuedConnection);
            writeJsonToFile(finalJson, m_currentJsonFilePath);
            QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
                Q_ARG(QString, QString("已按严重程度归档 %1 个最终错误图层（补充识别 %2 个；高严重程度 / 中严重程度 / 低严重程度）。")
                    .arg(archivedLayerCount).arg(recoveredErrorLayerCount)));
        }

        // 每完成一幅地图记录计数（进度由每个质检项触发）
        completedMaps++;
        QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
            Q_ARG(QString, QString("第%1/%2幅地图完成：%3；耗时 %4 秒。")
                .arg(row + 1).arg(rowCount).arg(mapName)
                .arg(mapStartedAt.secsTo(QDateTime::currentDateTime()))));
    }

    // 全部完成后在 UI 显示；最终统计由线程 finished 槽统一记录。
    QMetaObject::invokeMethod(this, "appendLog", Qt::QueuedConnection,
        Q_ARG(QString, QString("所有地图处理完毕，正在汇总运行结果。")));
    m_completedCheckCount.store(completedItems);
}

int map_check_task_manager::getScaleFromJson()
{
    if (m_currentJsonFilePath.isEmpty()) {
        return 0;
    }

    QFile jsonFile(m_currentJsonFilePath);
    if (!jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return 0;
    }
    QByteArray jsonData = jsonFile.readAll();
    jsonFile.close();

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (doc.isNull() || !doc.isObject()) {
        return 0;
    }
    QJsonObject root = doc.object();

    for (auto geopdfIt = root.constBegin(); geopdfIt != root.constEnd(); ++geopdfIt) {
        if (!geopdfIt.key().startsWith("Geopdf")) {
            continue;
        }

        QJsonObject geopdfNode = geopdfIt.value().toObject();
        if (!geopdfNode.contains("tasks") || !geopdfNode["tasks"].isObject()) {
            continue;
        }

        QJsonObject tasks = geopdfNode["tasks"].toObject();
        for (auto taskIt = tasks.constBegin(); taskIt != tasks.constEnd(); ++taskIt) {
            if (!taskIt.key().startsWith("task")) {
                continue;
            }

            QJsonObject task = taskIt.value().toObject();
            for (auto checkIt = task.constBegin(); checkIt != task.constEnd(); ++checkIt) {
                if (!checkIt.key().startsWith("check_item")) {
                    continue;
                }

                QJsonObject itemJson = checkIt.value().toObject();
                if (itemJson["name"].toString() != "比例尺数值检查") {
                    continue;
                }

                if (!itemJson.contains("info") || !itemJson["info"].isObject() ||
                    !itemJson["info"].toObject().contains("legend_scale")) {
                    continue;
                }

                QString qScaleStr = itemJson["info"].toObject()["legend_scale"].toString();

                if (qScaleStr.contains(":")) {
                    QStringList scaleParts = qScaleStr.split(":", Qt::SkipEmptyParts);
                    if (scaleParts.size() == 2) {
                        bool ok;
                        int scaleInt = scaleParts[1].toInt(&ok);
                        if (ok && scaleInt > 0) {
                            return scaleInt;
                        }
                    }
                }
            }
        }
    }

    return 0;
}

void map_check_task_manager::buildShpPaths(AlgorithmFilePaths& filePaths, const CsvLayerInfo& matchedLayers)
{
    // -------------------------- 1. 处理gdb图层路径 --------------------------
    // 清空原有列表和单个路径字段（避免残留旧数据）
    filePaths.gdbShp = "";
    // 保留 CSV 第2列的逻辑图层名。飞地漏绘等算法需要该名称在 GDB_SHP 目录中定位图层。
    filePaths.gdbLayer = matchedLayers.gdbLayerNames.join("|");
    QList<QString> allGdbPaths;

    foreach(const QString & gdbName, matchedLayers.gdbLayerNames) {
        if (gdbName.isEmpty()) continue;

        // 按忽略大小写的完整图层名定位 Shp。
        QString gdbPath = resolveShapefilePath(filePaths.gdbShpPath, gdbName);
        if (!gdbPath.isEmpty() && !allGdbPaths.contains(gdbPath)) {
            allGdbPaths << gdbPath;
        }
    }

    // 分号拼接路径（供算法直接使用）
    filePaths.gdbShpSemicolon = allGdbPaths.join(";");
    // 保留单个路径字段（取第一个有效路径，兼容旧逻辑）
    if (!allGdbPaths.isEmpty()) {
        filePaths.gdbShp = allGdbPaths.first();
    }


    // -------------------------- 2. 处理实际图层路径 --------------------------
    // 清空原有列表和单个路径字段
    filePaths.actualShp = "";
    filePaths.actualShpList.clear();

    foreach(const QString & actualName, matchedLayers.actualLayerNames) {
        if (actualName.isEmpty()) continue;

        // 2.1 查找基础图层（无分类后缀）
        QString basePath = resolveShapefilePath(filePaths.shpPath, actualName);
        if (!basePath.isEmpty() && !filePaths.actualShpList.contains(basePath)) {
            filePaths.actualShpList << basePath;
        }

        // 2.2 拼接线/面分类路径（不再搜索点要素后缀）
        QStringList shapeSuffixes = { "_LineString", "_Polygon" };
        foreach(const QString & suffix, shapeSuffixes) {
            QString categoryPath = resolveShapefilePath(filePaths.shpPath, actualName + suffix);
            if (!categoryPath.isEmpty() && !filePaths.actualShpList.contains(categoryPath)) {
                filePaths.actualShpList << categoryPath;
            }
        }
    }

    // 分号拼接路径（供算法直接使用）
    filePaths.actualShpSemicolon = filePaths.actualShpList.join(";");
    // 保留单个路径字段（取第一个有效路径，兼容旧逻辑）
    if (!filePaths.actualShpList.isEmpty()) {
        filePaths.actualShp = filePaths.actualShpList.first();
    }


    // -------------------------- 3. 处理注记图层路径 --------------------------
    // 清空原有列表和单个路径字段
    filePaths.annotShp = "";
    filePaths.annotShpList.clear();

    foreach(const QString & annotName, matchedLayers.actualAnnotLayerNames) {
        if (annotName.isEmpty()) continue;

        // 3.1 查找基础图层（无分类后缀）
        QString basePath = resolveShapefilePath(filePaths.shpPath, annotName);
        if (!basePath.isEmpty() && !filePaths.annotShpList.contains(basePath)) {
            filePaths.annotShpList << basePath;
        }

        // 3.2 拼接线/面分类路径（不再搜索点要素后缀）
        QStringList shapeSuffixes = { "_LineString", "_Polygon" };
        foreach(const QString & suffix, shapeSuffixes) {
            QString categoryPath = resolveShapefilePath(filePaths.shpPath, annotName + suffix);
            if (!categoryPath.isEmpty() && !filePaths.annotShpList.contains(categoryPath)) {
                filePaths.annotShpList << categoryPath;
            }
        }
    }

    // 分号拼接路径（供算法直接使用）
    filePaths.annotShpSemicolon = filePaths.annotShpList.join(";");
    // 保留单个路径字段（取第一个有效路径，兼容旧逻辑）
    if (!filePaths.annotShpList.isEmpty()) {
        filePaths.annotShp = filePaths.annotShpList.first();
    }

    // -------------------------- 4. 填充附加字段 --------------------------
    // 213 跨类型校验按第4列“实际注记图层”分组。213 当前也只使用首个注记图层，
    // 因此必须使用同一个名称作为 cross_check_<名称>.json 的中间文件标识。
    filePaths.correspondingAnnotLayer = matchedLayers.actualAnnotLayerNames.value(0);
    filePaths.legendField = matchedLayers.legendField;
}

// 批量添加地图路径
void map_check_task_manager::on_pushButton_BatchAdd_clicked()
{
    // 打开文件夹选择对话框，选择包含PDF文件的根文件夹
    QString rootDirPath = QFileDialog::getExistingDirectory(
        this,
        tr("选择包含待检图的根文件夹"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (rootDirPath.isEmpty()) {
        return; // 用户取消选择
    }

    QDir rootDir(rootDirPath);
    QStringList pdfFilters;
    pdfFilters << "*.pdf";
    QStringList pdfFiles = rootDir.entryList(pdfFilters, QDir::Files | QDir::Readable, QDir::Name);

    if (pdfFiles.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("所选文件夹中未找到PDF文件"));
        return;
    }

    int addedCount = 0;
    int currentRow = ui->tableWidget_GeopdfPath->rowCount();

    // 遍历找到的PDF文件
    foreach(const QString & pdfFile, pdfFiles) {
        QString pdfPath = rootDir.absoluteFilePath(pdfFile);
        QString baseName = QFileInfo(pdfFile).baseName(); // 不含扩展名的文件名

        // 构造对应的文件夹路径
        QString shpFolderPath = rootDir.absoluteFilePath(baseName + "_shp");
        QString gdbShpFolderPath = rootDir.absoluteFilePath(baseName + "_gdb_shp");
        QString legendFolderPath = rootDir.absoluteFilePath(QString::fromUtf8(u8"标准图例"));

        // 检查文件夹是否存在
        bool shpFolderExists = QDir(shpFolderPath).exists();
        bool gdbShpFolderExists = QDir(gdbShpFolderPath).exists();
        bool legendFolderExists = QDir(legendFolderPath).exists();

        // 如果至少有一个相关文件夹存在，则添加这一行
        if (shpFolderExists || gdbShpFolderExists) {
            ui->tableWidget_GeopdfPath->insertRow(currentRow);

            // 第0列：PDF文件路径
            setCellContent(currentRow, 0, pdfPath);

            // 第1列：shp文件夹路径（如果存在）
            if (shpFolderExists) {
                setCellContent(currentRow, 1, shpFolderPath);
            } else {
                setCellContent(currentRow, 1, "");
            }

            // 第2列：gdb_shp文件夹路径（如果存在）
            if (gdbShpFolderExists) {
                setCellContent(currentRow, 2, gdbShpFolderPath);
            } else {
                setCellContent(currentRow, 2, "");
            }

            // 第3列：图例文件夹（同级目录下存在"标准图例"时自动填充，也可手动修改）
            setCellContent(currentRow, 3, legendFolderExists ? legendFolderPath : "");

            // 第4列：图面配置选择（留空，让用户手动添加）
            setCellContent(currentRow, 4, "");

            currentRow++;
            addedCount++;
        }
    }

    // 批量添加后调整列宽
    ui->tableWidget_GeopdfPath->resizeColumnsToContents();

    if (addedCount > 0) {
        QMessageBox::information(this, tr("完成"),
            tr("成功添加 %1 个地图数据组").arg(addedCount));
    } else {
        QMessageBox::warning(this, tr("警告"),
            tr("未找到任何有效的地图数据组\n"
               "请确保文件夹中包含PDF文件以及对应的_shp或_gdb_shp文件夹"));
    }
}

// 添加地图路径
void map_check_task_manager::on_pushButton_AddGeopdfPath_clicked()
{
    // 打开文件选择对话框，允许选择多个geopdf文件
    QStringList filePaths = QFileDialog::getOpenFileNames(
        this,
        tr("选择pdf文件"),
        QString(),
        tr("pdf文件 (*.pdf);;所有文件 (*)")
    );

    if (filePaths.isEmpty()) {
        return; // 用户取消选择
    }

    // 将选择的文件路径添加到表格中
    int currentRow = ui->tableWidget_GeopdfPath->rowCount();
    foreach(const QString & path, filePaths) {
        ui->tableWidget_GeopdfPath->insertRow(currentRow);

        // 为所有列创建单元格部件：第0列传完整路径，其他列传空
        for (int col = 0; col < ui->tableWidget_GeopdfPath->columnCount(); ++col) {
            QString content = (col == 0) ? path : "";
            setCellContent(currentRow, col, content); // setCellContent 会自动截取第0列的文件名
        }

        currentRow++;
    }
    // 批量添加后再次调用，确保所有列适应内容
    ui->tableWidget_GeopdfPath->resizeColumnsToContents();
}

// 清空地图路径
void map_check_task_manager::on_pushButton_ClearGeopdfPath_clicked()
{
    // 确认是否清空
    if (QMessageBox::question(this, tr("确认"), tr("确定要清空所有文件路径吗？")) == QMessageBox::Yes) {
        ui->tableWidget_GeopdfPath->setRowCount(0);
    }
}

// 上调地图优先级
void map_check_task_manager::on_pushButton_upmap_clicked()
{
    int currentRow = ui->tableWidget_GeopdfPath->currentRow();
    if (currentRow <= 0 || currentRow >= ui->tableWidget_GeopdfPath->rowCount()) {
        return;
    }

    // 从表格项中获取当前行所有列的路径
    QList<QString> rowContents;
    for (int col = 0; col < ui->tableWidget_GeopdfPath->columnCount(); ++col) {
        QTableWidgetItem* item = ui->tableWidget_GeopdfPath->item(currentRow, col);
        rowContents.append(item ? item->data(Qt::UserRole).toString() : "");
    }

    // 删除当前行
    ui->tableWidget_GeopdfPath->removeRow(currentRow);

    // 在上一行插入新行，并重建自定义部件+表格项
    ui->tableWidget_GeopdfPath->insertRow(currentRow - 1);
    for (int col = 0; col < ui->tableWidget_GeopdfPath->columnCount(); ++col) {
        setCellContent(currentRow - 1, col, rowContents[col]);
    }

    // 保持选中状态
    ui->tableWidget_GeopdfPath->setCurrentCell(currentRow - 1, 0);
}

// 下调地图优先级（逻辑与上调类似，仅插入位置不同）
void map_check_task_manager::on_pushButton_downmap_clicked()
{
    int currentRow = ui->tableWidget_GeopdfPath->currentRow();
    int rowCount = ui->tableWidget_GeopdfPath->rowCount();
    if (currentRow < 0 || currentRow >= rowCount - 1) {
        return;
    }

    QList<QString> rowContents;
    for (int col = 0; col < ui->tableWidget_GeopdfPath->columnCount(); ++col) {
        QTableWidgetItem* item = ui->tableWidget_GeopdfPath->item(currentRow, col);
        rowContents.append(item ? item->data(Qt::UserRole).toString() : "");
    }

    ui->tableWidget_GeopdfPath->removeRow(currentRow);
    ui->tableWidget_GeopdfPath->insertRow(currentRow + 1);
    for (int col = 0; col < ui->tableWidget_GeopdfPath->columnCount(); ++col) {
        setCellContent(currentRow + 1, col, rowContents[col]);
    }

    ui->tableWidget_GeopdfPath->setCurrentCell(currentRow + 1, 0);
}

// 删除地图
void map_check_task_manager::on_pushButton_deletemap_clicked()
{
    int currentRow = ui->tableWidget_GeopdfPath->currentRow();
    if (currentRow < 0) {
        QMessageBox::information(this, tr("提示"), tr("请先选中要删除的地图路径"));
        return;
    }

    // 确认是否删除
    if (QMessageBox::question(this, tr("确认"), tr("确定要删除选中的整行吗？")) == QMessageBox::Yes) {
        ui->tableWidget_GeopdfPath->removeRow(currentRow);
    }
}


//解析xml文件，获取质检项目信息
void map_check_task_manager::parseXmlFile(const QString& xmlFilePath, QTreeWidgetItem* parentItem)
{
    QDomDocument doc;
    QFile file(xmlFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "无法打开XML文件：" << xmlFilePath;
        return;
    }

    if (!doc.setContent(&file)) {
        qWarning() << "XML文件解析失败：" << xmlFilePath;
        file.close();
        return;
    }
    file.close();

    QDomElement root = doc.documentElement();
    QDomNodeList scaleNodes = root.elementsByTagName("scale");

    // =====================必须有 <scale> 节点 =====================
    if (scaleNodes.isEmpty()) {
        qWarning() << "XML格式错误：未找到 <scale> 节点，所有质检方案必须包含 <scale> 标签：" << xmlFilePath;
        return;
    }

    // 解析所有 <scale> 节点
    for (int i = 0; i < scaleNodes.size(); ++i) {
        QDomElement scaleElem = scaleNodes.at(i).toElement();
        if (scaleElem.isNull()) continue;

        // 创建比例尺节点
        QString scaleValueText = "";  // 默认为空字符串（表示与比例尺无关）
        QDomElement valueElem = scaleElem.firstChildElement("value");
        if (!valueElem.isNull()) {
            scaleValueText = valueElem.text().trimmed();
            // 支持三种格式：
            // 1. <value/> 或 <value></value> → 空字符串（表示与比例尺无关）
            // 2. <value>25000</value> → "25000"（具体比例尺值）
            // 3. 无<value>节点 → 空字符串（表示与比例尺无关）
        }

        QTreeWidgetItem* scaleItem = new QTreeWidgetItem(parentItem);
        scaleItem->setText(1, scaleValueText); // 第二列显示比例尺（空字符串表示与比例尺无关）

        // 在比例尺节点下创建所有 check_item
        QDomNodeList checkItems = scaleElem.elementsByTagName("check_item");
        for (int j = 0; j < checkItems.size(); ++j) {
            QDomElement checkItem = checkItems.at(j).toElement();
            if (checkItem.isNull()) continue;

            QTreeWidgetItem* checkItemItem = new QTreeWidgetItem(scaleItem);

            // 质检项名称列
            QDomElement nameElem = checkItem.firstChildElement("name");
            if (!nameElem.isNull()) {
                const QString checkItemName = nameElem.text().trimmed();
                checkItemItem->setText(2, checkItemName);
                checkItemItem->setData(2, Qt::UserRole, xmlFilePath);

                // XML 配置字段和输出字段均使用 severity。
                const QString severity = normalizedSeverity(
                    checkItem.firstChildElement("severity").text());
                m_checkItemSeverity.insert(
                    checkItemSeverityKey(parentItem->text(0), checkItemName), severity);
            }

            // 默认未检查状态图标
            setCompletionStatusIcon(checkItemItem, CompletionStatus::NotChecked);
        }
    }
}

void map_check_task_manager::beginMapTaskStatus(int taskRow)
{
    m_displayedTaskRow = taskRow;
    resetAllCheckItemStatusIcons();
}

// 重置所有质检项的状态图标为未质检状态
void map_check_task_manager::resetAllCheckItemStatusIcons()
{
    QTreeWidget* tree = ui->treeWidget_xmlTaskData;
    if (!tree) return;
    
    // 遍历所有顶级项（比例尺）
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* scaleItem = tree->topLevelItem(i);
        if (!scaleItem) continue;
        
        // 遍历比例尺下的所有子项
        for (int j = 0; j < scaleItem->childCount(); ++j) {
            QTreeWidgetItem* childItem = scaleItem->child(j);
            if (!childItem) continue;
            
            // 如果是质检项节点（有3列的项），重置其图标
            if (childItem->childCount() == 0 && !childItem->text(2).isEmpty()) {
                setCompletionStatusIcon(childItem, CompletionStatus::NotChecked);
            }
            
            // 遍历质检项的子项（如果有的话）
            for (int k = 0; k < childItem->childCount(); ++k) {
                QTreeWidgetItem* checkItem = childItem->child(k);
                if (checkItem && !checkItem->text(2).isEmpty()) {
                    setCompletionStatusIcon(checkItem, CompletionStatus::NotChecked);
                }
            }
        }
    }
}

// "质检报告导出"按钮点击槽函数
void map_check_task_manager::on_pushButton_ReportExport_clicked()
{
    // 发射信号，让插件主类处理报告导出
    emit reportRequested(m_currentJsonFilePath);
}
