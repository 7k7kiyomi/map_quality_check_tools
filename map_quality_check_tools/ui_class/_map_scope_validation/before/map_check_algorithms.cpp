#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#include "map_check_algorithms.h"
#include <QCoreApplication>
#include <QColor>
#include <QCryptographicHash>
#include "series_layer_mapping.h"
#include "series_source_audit.h"
#include "series_cartographic_policy.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QDir>
#include <QStandardPaths>
#include <QSet>
#include <QTextStream>
#include <QThread>
#include <QTemporaryDir>
#include <QVector>
#include "algorithm_ScaleChecker.h"
#include "AngleCheckInterface.h"
#include "json_result_parser.h"
#include "map_check_task_manager.h"
#include "ReadShp_interface.h"
#include "../algorithm/se_annotation_check_interface.h"
#include "../algorithm/se_annotation_advanced_check_interface.h"
#include "../algorithm/se_global_blank_check.h"
#include "se_map_style_check_algorithm.h"
#include "se_feature_layer_missing_check_algorithm.h"
#include "se_min_bend_threshold_algorithm.h"
#include "se_map_annotation_check_algorithm.h"
#include "se_admin_color_check_algorithm.h"
#include "se_absolute_offset_check_algorithm.h"
#include "se_feature_misplacement_check_algorithm.h"
#include "se_feature_missing_check_algorithm.h"
#include "se_content_frame_misalignment_check_algorithm.h"
#include "se_feature_out_of_boundary_check_algorithm.h"
#include "se_boundary_missing_level_check_algorithm.h"
#include "se_administrative_division_fit_check_algorithm.h"
#include "se_boundary_jump_draw_check_algorithm.h"
#include "FeatureSpacingCheckDialog.h"
#include "IMapFrameAlignmentCheck.h"
#include "FeaturePointToleranceChecker.h"
#include "ScaleChecker.h"
#include "SmoothnessChecker.h"
#include "se_legend_match_check_algorithm.h"
#include "se_map_name_match_check_algorithm.h"
#include "se_required_layers_check_algorithm.h"
#include "sensitive_content_check_algorithm.h"
#include "se_annotation_content_check_interface.h"
#include "cross_check.h"
#include "ReadShp_interface.h"
#include "ContainmentCheckInterface.h"
#include <QDomDocument>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>
#include <limits>
#include <qgsfeature.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>
#include <qgsspatialindex.h>
#include <qgsvectorlayer.h>
#include <qgswkbtypes.h>
#include "../commontype/se_error_define.h"
#include "QualityCheckConfig.h"

#include"ui_class/se_admin_color_style_check_algorithm.h"
#include"ui_class/se_enclave_missing_check_algorithm.h"
#include"ui_class/se_admin_name_annotation_check_algorithm.h"
#include"ui_class/se_boundary_band_render_check_algorithm.h"
#include "boundary_band_color.h"
#include "boundary_band_geometry.h"

static void append501DebugLog(const QString& message)
{
    QFile file(QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                   .filePath(QStringLiteral("501_debug.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz "))
        << message << "\n";
}

// === 辅助函数：从XML获取特定规则的值（支持比例尺 + 名称容错 + 全局回退） ===
static QString getRuleValueFromXml(const QString& xmlPath, const QString& checkItemName, const QString& ruleName, int scale)
{
    QFile xmlFile(xmlPath);
    if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }

    QDomDocument doc;
    if (!doc.setContent(&xmlFile)) {
        xmlFile.close();
        return QString();
    }
    xmlFile.close();

    QDomElement root = doc.documentElement();
    QDomNodeList scaleNodes = root.elementsByTagName("scale");

    // 名称匹配容错：支持“关系检查”“包含关系倒置检查”同义
    auto matchesCheckItemName = [&](const QString& xmlName)->bool {
        QString a = xmlName.trimmed();
        QString b = checkItemName.trimmed();
        if (a == b) return true;
        // 同义词处理
        static const QStringList synonyms = { QStringLiteral("关系检查"), QStringLiteral("包含关系倒置检查") };
        return synonyms.contains(a) && synonyms.contains(b);
    };

    QDomElement targetScaleElem;
    QDomElement emptyScaleElem;

    // 1. 遍历查找匹配的scale节点和空的scale节点
    for (int i = 0; i < scaleNodes.count(); ++i) {
        QDomElement scaleElem = scaleNodes.at(i).toElement();
        QDomElement valueElem = scaleElem.firstChildElement("value");
        if (valueElem.isNull() || valueElem.text().isEmpty()) {
            emptyScaleElem = scaleElem; // 找到空的scale节点
        }
        else if (valueElem.text().toInt() == scale) {
            targetScaleElem = scaleElem; // 找到匹配的scale节点
            break; // 优先使用精确匹配的，直接跳出
        }
    }

    // 2. 定义一个lambda函数用于在给定的scale节点下查找规则值
    auto findRuleInScale = [&](const QDomElement& scaleElem) -> QString {
        if (scaleElem.isNull()) return QString();

        QDomNodeList checkItems = scaleElem.elementsByTagName("check_item");
        for (int i = 0; i < checkItems.count(); ++i) {
            QDomElement itemElem = checkItems.at(i).toElement();
            QDomElement nameElem = itemElem.firstChildElement("name");
            if (!nameElem.isNull() && matchesCheckItemName(nameElem.text())) {
                QDomNodeList ruleNodes = itemElem.elementsByTagName("rule");
                for (int j = 0; j < ruleNodes.count(); ++j) {
                    QDomElement ruleElem = ruleNodes.at(j).toElement();
                    QDomElement ruleNameElem = ruleElem.firstChildElement("name");
                    if (!ruleNameElem.isNull() && ruleNameElem.text() == ruleName) {
                        QDomElement valueElem = ruleElem.firstChildElement("value");
                        if (!valueElem.isNull()) {
                            return valueElem.text();
                        }
                    }
                }
            }
        }
        return QString();
    };

    // 3. 优先从精确匹配的scale下查找
    QString result = findRuleInScale(targetScaleElem);
    if (!result.isEmpty()) {
        return result;
    }

    // 4. 如果找不到，则从空的scale下查找
    result = findRuleInScale(emptyScaleElem);
    if (!result.isEmpty()) {
        return result;
    }

    // 5. 进一步回退：遍历所有scale节点进行查找（忽略比例尺）
    for (int i = 0; i < scaleNodes.count(); ++i) {
        QString v = findRuleInScale(scaleNodes.at(i).toElement());
        if (!v.isEmpty()) return v;
    }

    // 6. 最后回退：全局搜索（不依赖scale层级），查找任意check_item中对应规则
    QDomNodeList globalCheckItems = root.elementsByTagName("check_item");
    for (int i = 0; i < globalCheckItems.count(); ++i) {
        QDomElement itemElem = globalCheckItems.at(i).toElement();
        QDomElement nameElem = itemElem.firstChildElement("name");
        if (!nameElem.isNull() && matchesCheckItemName(nameElem.text())) {
            QDomNodeList ruleNodes = itemElem.elementsByTagName("rule");
            for (int j = 0; j < ruleNodes.count(); ++j) {
                QDomElement ruleElem = ruleNodes.at(j).toElement();
                QDomElement ruleNameElem = ruleElem.firstChildElement("name");
                if (!ruleNameElem.isNull() && ruleNameElem.text() == ruleName) {
                    QDomElement valueElem = ruleElem.firstChildElement("value");
                    if (!valueElem.isNull()) {
                        return valueElem.text();
                    }
                }
            }
        }
    }

    return QString();
}

static QString getFirstRuleValueFromXml(const QString& xmlPath, const QString& checkItemName, const QStringList& ruleNames, int scale)
{
    for (const QString& ruleName : ruleNames) {
        const QString value = getRuleValueFromXml(xmlPath, checkItemName, ruleName, scale).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return QString();
}

static double getDoubleRuleValueFromXml(const QString& xmlPath, const QString& checkItemName, const QStringList& ruleNames, int scale, double defaultValue)
{
    const QString value = getFirstRuleValueFromXml(xmlPath, checkItemName, ruleNames, scale);
    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok ? number : defaultValue;
}

static QString resolveBoundaryBandColorFromXml(const QString& xmlPath, const QString& checkItemName, const QStringList& preferredRuleNames, int scale)
{
    QString colorValue = getFirstRuleValueFromXml(xmlPath, checkItemName, preferredRuleNames, scale).trimmed().toLower();
    if (!colorValue.isEmpty()) {
        return colorValue;
    }

    return getFirstRuleValueFromXml(xmlPath, checkItemName, QStringList()
        << QStringLiteral("标准填充颜色") << QStringLiteral("标准颜色"), scale).trimmed().toLower();
}

// === 新增辅助函数：根据标准图层名获取实际图层路径 ===
static QString getPathsForStdLayers(const QString& stdLayerNames, const QString& csvPath, const QString& shpRootDir) {
    if (csvPath.isEmpty() || shpRootDir.isEmpty() || stdLayerNames.isEmpty()) {
        return "";
    }

    QMap<QString, CsvLayerInfo> layerMap = map_check_task_manager::parseCsvToLayerMap(csvPath);
    if (layerMap.isEmpty()) {
        return "";
    }

    QStringList allActualPaths;
    QStringList stdNames = stdLayerNames.split(';', Qt::SkipEmptyParts);

    for (const QString& stdName : stdNames) {
        if (!layerMap.contains(stdName.trimmed())) continue;

        const CsvLayerInfo& layerInfo = layerMap.value(stdName.trimmed());

        for (const QString& actualName : layerInfo.actualLayerNames) {
            if (actualName.isEmpty()) continue;

            QString basePath = QDir(shpRootDir).filePath(actualName + ".shp");
            if (QFile::exists(basePath) && !allActualPaths.contains(basePath)) {
                allActualPaths << basePath;
            }

            QStringList suffixes = { "_Point", "_LineString", "_Polygon" };
            for (const QString& suffix : suffixes) {
                QString categoryPath = QDir(shpRootDir).filePath(actualName + suffix + ".shp");
                if (QFile::exists(categoryPath) && !allActualPaths.contains(categoryPath)) {
                    allActualPaths << categoryPath;
                }
            }
        }
    }
    return allActualPaths.join(';');
}

// === 新增辅助函数：根据标准图层名获取GDB图层路径 ===
static QString getGdbPathsForStdLayers(const QString& stdLayerNames, const QString& csvPath, const QString& gdbRootDir) {
    if (csvPath.isEmpty() || gdbRootDir.isEmpty() || stdLayerNames.isEmpty()) {
        return "";
    }

    QMap<QString, CsvLayerInfo> layerMap = map_check_task_manager::parseCsvToLayerMap(csvPath);
    if (layerMap.isEmpty()) {
        return "";
    }

    QStringList allGdbPaths;
    QStringList stdNames = stdLayerNames.split(';', Qt::SkipEmptyParts);

    for (const QString& stdName : stdNames) {
        QString key = stdName.trimmed();
        if (!layerMap.contains(key)) continue;

        const CsvLayerInfo& layerInfo = layerMap.value(key);
        for (const QString& gdbName : layerInfo.gdbLayerNames) {
            if (gdbName.isEmpty()) continue;
            QString gdbPath = QDir(gdbRootDir).filePath(gdbName + ".shp");
            if (QFile::exists(gdbPath) && !allGdbPaths.contains(gdbPath)) {
                allGdbPaths << gdbPath;
            }
        }
    }
    return allGdbPaths.join(';');
}

// === 新增辅助函数：按几何后缀筛选路径（优先返回匹配后缀；否则返回无后缀的基础路径） ===
static QString filterPathsBySuffixPreferBase(const QString& semicolonPaths, const QString& suffixPreferred) {
    if (semicolonPaths.isEmpty() || suffixPreferred.isEmpty()) return semicolonPaths;

    QStringList inputPaths = semicolonPaths.split(';', Qt::SkipEmptyParts);
    QStringList matched;
    QStringList base;
    const QString suffixExt = suffixPreferred + ".shp";
    const QStringList knownSuffixes = { "_Point.shp", "_LineString.shp", "_Polygon.shp" };

    for (const QString& p : inputPaths) {
        QString lower = p.toLower();
        if (lower.endsWith(suffixExt.toLower())) {
            matched << p;
        } else {
            // 如果不是任何已知后缀，则认为是无后缀基础路径
            bool hasKnownSuffix = false;
            for (const QString& ks : knownSuffixes) {
                if (lower.endsWith(ks)) { hasKnownSuffix = true; break; }
            }
            if (!hasKnownSuffix) base << p;
        }
    }

    if (!matched.isEmpty()) return matched.join(';');
    if (!base.isEmpty()) return base.join(';');
    return semicolonPaths; // 保底返回原始列表
}

static QStringList splitExistingShpPaths(const QString& semicolonPaths)
{
    QStringList paths;
    const QStringList rawPaths = semicolonPaths.split(';', Qt::SkipEmptyParts);
    for (QString path : rawPaths) {
        path = path.trimmed();
        if (!path.isEmpty() && QFileInfo::exists(path) && !paths.contains(path)) {
            paths << path;
        }
    }
    return paths;
}

static QString findBoundaryBandPolygonPath(const AlgorithmFilePaths& filePaths, const QString& selectedXmlPath, int scale)
{
    // 晕带质检约定：晕带面属于“实际图层”列，不再从“实际注记图层”列强制读取。
    // 这里按 XML 中的“色带图层/晕带图层”名称，从实际图层候选路径里找真正的晕带面。
    const QString bandCheckName = QStringLiteral("境界线色带渲染合规性检查");
    const QString bandLayerName = getFirstRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("色带图层") << QStringLiteral("晕带图层"), scale).trimmed();

    QStringList candidates = filePaths.actualShpList;
    candidates << splitExistingShpPaths(filePaths.actualShpSemicolon);
    candidates << filePaths.actualShp;
    candidates << filePaths.annotShpList;
    candidates << splitExistingShpPaths(filePaths.annotShpSemicolon);
    candidates << filePaths.annotShp;
    if (!filePaths.actualLayer.trimmed().isEmpty()) {
        candidates << QDir(filePaths.shpPath).filePath(filePaths.actualLayer.trimmed() + QStringLiteral(".shp"));
        candidates << QDir(filePaths.shpPath).filePath(filePaths.actualLayer.trimmed() + QStringLiteral("_Polygon.shp"));
    }
    candidates.removeAll(QString());
    candidates.removeDuplicates();

    auto nameMatchesBand = [&](const QString& path) {
        if (bandLayerName.isEmpty()) {
            return false;
        }
        const QString baseName = QFileInfo(path).completeBaseName();
        return baseName == bandLayerName || baseName == bandLayerName + QStringLiteral("_Polygon");
    };

    for (const QString& path : candidates) {
        if (nameMatchesBand(path)) {
            return path;
        }
    }
    for (const QString& path : candidates) {
        const QString baseName = QFileInfo(path).completeBaseName();
        if (baseName.contains(QStringLiteral("晕带")) || baseName.contains(QStringLiteral("色带"))) {
            return path;
        }
    }
    return candidates.value(0);
}

static QStringList resolveBoundaryBandLinePaths(const AlgorithmFilePaths& filePaths, const QString& selectedXmlPath, int scale)
{
    // 县级、市州级、省级境界线只用于挂接方向判断，来源固定为 XML 的“境界基准线图层”。
    // 这样不会把 CSV 中的“实际图层=晕带”误当作境界线，也不需要在任务管理器里写特殊分支。
    const QString bandCheckName = QStringLiteral("境界线色带渲染合规性检查");
    const QString boundaryLayerNames = getFirstRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("境界基准线图层") << QStringLiteral("关联图层") << QStringLiteral("边界线图层"), scale).trimmed();

    QStringList paths;
    const QStringList names = boundaryLayerNames.split(';', Qt::SkipEmptyParts);
    for (QString name : names) {
        name = name.trimmed();
        if (name.isEmpty()) {
            continue;
        }
        if (QFileInfo::exists(name)) {
            paths << name;
            continue;
        }
        const QString linePath = QDir(filePaths.shpPath).filePath(name + QStringLiteral("_LineString.shp"));
        const QString basePath = QDir(filePaths.shpPath).filePath(name + QStringLiteral(".shp"));
        if (QFileInfo::exists(linePath)) {
            paths << linePath;
        }
        else if (QFileInfo::exists(basePath)) {
            paths << basePath;
        }
    }
    paths.removeDuplicates();
    return paths;
}


// 境界线越级检查只接受线要素。这里仅过滤非线 SHP，
// 不参与行政区划面套合检查的线/面双输入分流。
static QStringList filterBoundaryLevelLinePaths(const QString& semicolonPaths)
{
    QStringList linePaths;
    GDALAllRegister();

    for (const QString& rawPath : semicolonPaths.split(';', Qt::SkipEmptyParts)) {
        const QString path = rawPath.trimmed();
        if (path.isEmpty() || !QFileInfo(path).isFile())
            continue;

        GDALDataset* dataset = static_cast<GDALDataset*>(
            GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                nullptr, nullptr, nullptr));
        if (dataset == nullptr)
            continue;

        bool hasLineGeometry = false;
        for (int layerIndex = 0; layerIndex < dataset->GetLayerCount(); ++layerIndex) {
            OGRLayer* layer = dataset->GetLayer(layerIndex);
            if (layer == nullptr)
                continue;

            const OGRwkbGeometryType type = wkbFlatten(layer->GetGeomType());
            if (type == wkbLineString || type == wkbMultiLineString) {
                hasLineGeometry = true;
                break;
            }
        }
        GDALClose(dataset);

        if (hasLineGeometry && !linePaths.contains(path))
            linePaths << path;
    }

    return linePaths;
}

// 行政区划套合检查需要同时获取行政界线和行政区划面。解析后的实际图层路径
// 可能是 _LineString/_Polygon，也可能是未带后缀的 SHP，因此以实际几何类型为准分类。
static void classifyAdministrativeFitPaths(
    const QString& semicolonPaths,
    QStringList& linePaths,
    QStringList& polygonPaths)
{
    GDALAllRegister();

    const QStringList paths = semicolonPaths.split(';', Qt::SkipEmptyParts);
    for (const QString& rawPath : paths) {
        const QString path = rawPath.trimmed();
        if (path.isEmpty() || !QFileInfo(path).isFile())
            continue;

        GDALDataset* dataset = static_cast<GDALDataset*>(
            GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                nullptr, nullptr, nullptr));
        if (dataset == nullptr)
            continue;

        bool hasLine = false;
        bool hasPolygon = false;
        for (int layerIndex = 0; layerIndex < dataset->GetLayerCount(); ++layerIndex) {
            OGRLayer* layer = dataset->GetLayer(layerIndex);
            if (layer == nullptr)
                continue;

            const OGRwkbGeometryType type = wkbFlatten(layer->GetGeomType());
            hasLine = hasLine || type == wkbLineString || type == wkbMultiLineString;
            hasPolygon = hasPolygon || type == wkbPolygon || type == wkbMultiPolygon;
        }
        GDALClose(dataset);

        if (hasLine && !linePaths.contains(path))
            linePaths << path;
        if (hasPolygon && !polygonPaths.contains(path))
            polygonPaths << path;
    }
}

// 116 的“关联图层”是标准图层名称列表，XML 允许使用中英文逗号、顿号、分号或竖线分隔。
static QStringList splitBoundaryJumpAssociatedLayers(const QString& ruleValue)
{
    QStringList names = ruleValue.split(QRegularExpression(QStringLiteral("[;；,，、|]+")),
        Qt::SkipEmptyParts);
    QStringList uniqueNames;
    for (QString& name : names) {
        name = name.trimmed();
        if (!name.isEmpty() && !uniqueNames.contains(name))
            uniqueNames << name;
    }
    return uniqueNames;
}

// 解析后的图面要素按实际图层名称选择几何类型：名称含“面”时只取 Polygon，
// 其余只取 LineString。明确的分类文件优先于基础 SHP，避免重复加载。
static QStringList selectBoundaryJumpParsedGeometryPaths(const QString& semicolonPaths)
{
    const QStringList allPaths = splitExistingShpPaths(semicolonPaths);
    const QStringList lineSuffixes = {
        QStringLiteral("_LineString"), QStringLiteral("_MultiLineString") };
    const QStringList polygonSuffixes = {
        QStringLiteral("_Polygon"), QStringLiteral("_MultiPolygon") };

    QSet<QString> typedBases;
    QStringList selectedPaths;
    for (const QString& path : allPaths) {
        const QString baseName = QFileInfo(path).completeBaseName();
        QString actualLayerName = baseName;
        bool isLineFile = false;
        bool isPolygonFile = false;
        for (const QString& suffix : lineSuffixes) {
            if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
                actualLayerName.chop(suffix.size());
                isLineFile = true;
                break;
            }
        }
        if (!isLineFile) {
            for (const QString& suffix : polygonSuffixes) {
                if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
                    actualLayerName.chop(suffix.size());
                    isPolygonFile = true;
                    break;
                }
            }
        }
        if (!isLineFile && !isPolygonFile)
            continue;

        const bool requiresPolygon = actualLayerName.contains(QStringLiteral("面"));
        if ((requiresPolygon && isPolygonFile) || (!requiresPolygon && isLineFile)) {
            typedBases.insert(actualLayerName.toLower());
            selectedPaths << path;
        }
    }

    for (const QString& path : allPaths) {
        const QString baseName = QFileInfo(path).completeBaseName();
        bool isTypedFile = false;
        QStringList allTypeSuffixes = lineSuffixes;
        allTypeSuffixes.append(polygonSuffixes);
        for (const QString& suffix : allTypeSuffixes) {
            if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
                isTypedFile = true;
                break;
            }
        }
        if (isTypedFile || typedBases.contains(baseName.toLower()))
            continue;

        QStringList linePaths;
        QStringList polygonPaths;
        classifyAdministrativeFitPaths(path, linePaths, polygonPaths);
        const bool requiresPolygon = baseName.contains(QStringLiteral("面"));
        if ((requiresPolygon && !polygonPaths.isEmpty())
            || (!requiresPolygon && !linePaths.isEmpty())) {
            selectedPaths << path;
        }
    }

    selectedPaths.removeDuplicates();
    return selectedPaths;
}

static QStringList filterSharpAngleGeometryPaths(const QString& semicolonPaths)
{
    QStringList paths;
    for (const QString& rawPath : semicolonPaths.split(';', Qt::SkipEmptyParts)) {
        const QString path = rawPath.trimmed();
        if (path.isEmpty())
            continue;
        QStringList linePaths;
        QStringList polygonPaths;
        classifyAdministrativeFitPaths(path, linePaths, polygonPaths);
        if ((!linePaths.isEmpty() || !polygonPaths.isEmpty()) && !paths.contains(path))
            paths << path;
    }
    return paths;
}

// ReadShp_interface 内部会创建 ReadShpDialog（QWidget）和 QGIS 对象，
// 因此不能直接从 AlgorithmThread 调用。所有旧 ReadShp 检查统一经此入口
// 切换到 QGIS 主线程；BlockingQueuedConnection 保持原有的同步返回语义。
static ReadShp_error runReadShpInterfaceOnGuiThread(
    const std::function<ReadShp_error()>& check)
{
    if (!check)
        return READSHP_INVALID_PARAM;

    QCoreApplication* application = QCoreApplication::instance();
    if (application == nullptr)
        return READSHP_FAIL;

    if (QThread::currentThread() == application->thread())
        return check();

    ReadShp_error result = READSHP_FAIL;
    const bool invoked = QMetaObject::invokeMethod(
        application,
        [&]() { result = check(); },
        Qt::BlockingQueuedConnection);
    return invoked ? result : READSHP_FAIL;
}

// 获取固定的结果输出目录(桌面/resultdata)
static QString GetFixedOutputDir()
{
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString resultDataDir = QDir(desktopPath).filePath("resultdata");
    
    // 确保目录存在
    QDir dir;
    if (!dir.exists(resultDataDir)) {
        dir.mkpath(resultDataDir);
    }
    
    return resultDataDir;
}

// 辅助函数:创建质检输出目录结构
struct OutputPaths {
    QString jsonDir;
    QString logDir;
    QString errorResultDir;
};

static OutputPaths CreateOutputDirectories(const QString& baseOutputDir)
{
    OutputPaths paths;
    QDir outputDirObj(baseOutputDir);
    
    // 使用 UTF-8 编码的字符串字面量
    paths.jsonDir = outputDirObj.filePath(QString::fromUtf8("质检json文件"));
    paths.logDir = outputDirObj.filePath(QString::fromUtf8("质检日志文件"));
    paths.errorResultDir = outputDirObj.filePath(QString::fromUtf8("质检错误结果"));
    
    // 确保目录存在 - QDir 会自动处理 Unicode 路径
    QDir().mkpath(paths.jsonDir);
    QDir().mkpath(paths.logDir);
    QDir().mkpath(paths.errorResultDir);
    
    return paths;
}

// 辅助函数:根据标准图层名称匹配XML文件
QString MatchXmlByLayerName(const QStringList& xmlPaths, const QString& standardLayer)
{
    if (xmlPaths.isEmpty()) {
        return QString();
    }
    
    // 尝试找到文件名完全匹配标准图层名称的XML文件
    for (const QString& xmlPath : xmlPaths) {
        QFileInfo xmlFileInfo(xmlPath);
        QString xmlBaseName = xmlFileInfo.completeBaseName();
        if (xmlBaseName.compare(standardLayer, Qt::CaseInsensitive) == 0) {
            return xmlPath;
        }
    }
    
    // 如果没找到匹配的,使用第一个XML
    return xmlPaths.first();
}

static QStringList globalBlankLayerNames(QString value)
{
    value.replace(QChar(0xff1b), QChar(';'));
    QStringList names;
    for (QString name : value.split(';', Qt::SkipEmptyParts)) {
        name = name.trimmed();
        if (!name.isEmpty() && !names.contains(name)) names << name;
    }
    return names;
}

// 122 的 XML 按检查项及实际比例尺选择，不能使用“文件名匹配失败取第一个”的旧逻辑。
static bool selectGlobalBlankRules(const QStringList& xmlPaths, int scale, QString& selectedPath,
                                   QMap<QString, QString>& rules, QString& error)
{
    int selectedRank = 0;
    QString duplicate;
    for (const QString& path : xmlPaths) {
        QFile file(path);
        QDomDocument doc;
        if (!file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) continue;
        const QDomNodeList scales = doc.documentElement().elementsByTagName(QStringLiteral("scale"));
        for (int i = 0; i < scales.size(); ++i) {
            const QDomElement scaleElement = scales.at(i).toElement();
            const QString value = scaleElement.firstChildElement(QStringLiteral("value")).text().trimmed();
            bool ok = false;
            const int configuredScale = value.toInt(&ok);
            const int rank = ok && configuredScale == scale ? 2 :
                             (value.isEmpty() || (ok && configuredScale == 0) ? 1 : 0);
            if (rank == 0) continue;
            const QDomNodeList items = scaleElement.elementsByTagName(QStringLiteral("check_item"));
            for (int j = 0; j < items.size(); ++j) {
                const QDomElement item = items.at(j).toElement();
                if (item.firstChildElement(QStringLiteral("name")).text().trimmed() !=
                    QStringLiteral("全局空白检查")) continue;
                if (rank == selectedRank) {
                    duplicate = QStringLiteral("同一比例尺存在多个全局空白检查配置：%1；%2")
                                    .arg(selectedPath, path);
                    continue;
                }
                if (rank < selectedRank) continue;
                selectedRank = rank;
                duplicate.clear();
                selectedPath = path;
                rules.clear();
                const QDomNodeList nodes = item.elementsByTagName(QStringLiteral("rule"));
                for (int k = 0; k < nodes.size(); ++k) {
                    const QDomElement rule = nodes.at(k).toElement();
                    const QString name = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
                    if (rules.contains(name)) {
                        error = QStringLiteral("全局空白检查 XML 规则重复：%1").arg(name);
                        return false;
                    }
                    rules.insert(name, rule.firstChildElement(QStringLiteral("value")).text().trimmed());
                }
            }
        }
    }
    if (selectedPath.isEmpty()) {
        error = QStringLiteral("未找到适用于比例尺 1:%1 的全局空白检查 XML").arg(scale);
        return false;
    }
    if (!duplicate.isEmpty()) {
        error = duplicate;
        return false;
    }
    return true;
}

static void failGlobalBlank(const QString& checkItemName, const QString& jsonPath,
                            const QString& reason, CheckItemResult& result)
{
    result.id = QStringLiteral("122");
    result.name = checkItemName;
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

void GlobalBlankCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths,
                             int scale, const QString& outputDir, CheckItemResult& result)
{
    const OutputPaths outputs = CreateOutputDirectories(outputDir);
    const QString base = QStringLiteral("全局空白检查_error_内图廓");
    const QString jsonPath = QDir(outputs.jsonDir).filePath(base + QStringLiteral(".json"));
    const QString logPath = QDir(outputs.logDir).filePath(base + QStringLiteral(".log"));
    const QString regionPath = QDir(outputs.errorResultDir).filePath(base + QStringLiteral(".shp"));
    if (QFileInfo::exists(jsonPath) && !QFile::remove(jsonPath)) {
        failGlobalBlank(checkItemName, jsonPath,
                        QStringLiteral("无法清除上次运行的 JSON：%1").arg(jsonPath), result);
        return;
    }

    if (scale <= 0) {
        failGlobalBlank(checkItemName, jsonPath, QStringLiteral("缺少有效的实际比例尺"), result);
        return;
    }
    QString xmlPath, selectionError;
    QMap<QString, QString> rules;
    if (!selectGlobalBlankRules(filePaths.xmlPaths, scale, xmlPath, rules, selectionError)) {
        failGlobalBlank(checkItemName, jsonPath, selectionError, result);
        return;
    }
    const QDir parsedDir(filePaths.shpPath);
    const QString boundaryPath = parsedDir.filePath(QStringLiteral("内图廓.shp"));
    if (!parsedDir.exists() || !QFileInfo::exists(boundaryPath)) {
        failGlobalBlank(checkItemName, jsonPath,
                        QStringLiteral("缺少解析图层目录或内图廓：%1").arg(boundaryPath), result);
        return;
    }
    const QMap<QString, CsvLayerInfo> layerMap =
        map_check_task_manager::parseCsvToLayerMap(filePaths.csvPath);
    if (layerMap.isEmpty()) {
        failGlobalBlank(checkItemName, jsonPath, QStringLiteral("图面配置 CSV 缺失或无法解析"), result);
        return;
    }

    QStringList excludedPaths, missingExcluded, sourcePaths;
    for (const QString& standardName : globalBlankLayerNames(
             rules.value(QStringLiteral("不参与空白检查图层")))) {
        if (!layerMap.contains(standardName) || layerMap.value(standardName).actualLayerNames.isEmpty()) {
            failGlobalBlank(checkItemName, jsonPath,
                            QStringLiteral("排除图层无法映射到实际图层：%1").arg(standardName), result);
            return;
        }
        const QStringList actualPaths = getPathsForStdLayers(
            standardName, filePaths.csvPath, filePaths.shpPath).split(';', Qt::SkipEmptyParts);
        if (actualPaths.isEmpty()) missingExcluded << standardName;
        for (const QString& path : actualPaths)
            if (!excludedPaths.contains(path, Qt::CaseInsensitive)) excludedPaths << path;
    }
    for (const QString& standardName : globalBlankLayerNames(
             rules.value(QStringLiteral("空白区提取图层")))) {
        if (filePaths.gdbShpPath.trimmed().isEmpty()) {
            failGlobalBlank(checkItemName, jsonPath,
                            QStringLiteral("配置了候选源图层，但未提供 GDB_SHP 目录"), result);
            return;
        }
        if (!layerMap.contains(standardName) || layerMap.value(standardName).gdbLayerNames.isEmpty()) {
            failGlobalBlank(checkItemName, jsonPath,
                            QStringLiteral("候选源图层无法映射：%1").arg(standardName), result);
            return;
        }
        for (const QString& sourceName : layerMap.value(standardName).gdbLayerNames) {
            const QString sourcePath = QDir(filePaths.gdbShpPath).filePath(sourceName + QStringLiteral(".shp"));
            if (!QFileInfo::exists(sourcePath)) {
                failGlobalBlank(checkItemName, jsonPath,
                                QStringLiteral("缺少候选源图层 %1：%2").arg(standardName, sourcePath), result);
                return;
            }
            if (!sourcePaths.contains(sourcePath, Qt::CaseInsensitive)) sourcePaths << sourcePath;
        }
    }

    const QByteArray boundary = boundaryPath.toUtf8(), occupied = parsedDir.absolutePath().toUtf8();
    const QByteArray excluded = excludedPaths.join(';').toUtf8(), sources = sourcePaths.join(';').toUtf8();
    const QByteArray xml = xmlPath.toUtf8(), severity = filePaths.blankSeverity.toUtf8();
    const QByteArray region = regionPath.toUtf8(), json = jsonPath.toUtf8(), log = logPath.toUtf8();
    const int code = GlobalBlankCheck(boundary.constData(), occupied.constData(), excluded.constData(),
                                      sources.constData(), "", xml.constData(), scale, severity.constData(),
                                      region.constData(), json.constData(), log.constData());
    if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
        failGlobalBlank(checkItemName, jsonPath,
                        QStringLiteral("全局空白检查未生成可解析的本次 JSON，错误码：%1").arg(code), result);
        return;
    }
    if (code != 0 || result.status != QStringLiteral("质检成功")) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info[QStringLiteral("return_code")] = code;
    }
    if (!missingExcluded.isEmpty())
        result.info[QStringLiteral("本次不存在的排除图层")] = missingExcluded.join(';');
    if (!missingExcluded.isEmpty()) {
        QFile file(logPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            file.write((QStringLiteral("本次不存在的排除图层：") + missingExcluded.join(';') + QLatin1Char('\n')).toUtf8());
    }
    if (sourcePaths.isEmpty())
        result.info[QStringLiteral("候选提取")] = QStringLiteral("未配置候选源图层，仅识别空白区域");
    if (result.status == QStringLiteral("质检成功") && QFileInfo::exists(regionPath))
        result.info[QStringLiteral("blank_region_path")] = regionPath;
    const QString manifestPath = QDir(outputs.errorResultDir).filePath(QStringLiteral("候选要素输出清单.json"));
    if (result.status == QStringLiteral("质检成功") && !sourcePaths.isEmpty() &&
        QFileInfo::exists(manifestPath))
        result.info[QStringLiteral("candidate_manifest_path")] = manifestPath;
}

static QgsGeometry collectWholeLayerGeometry(QgsVectorLayer& layer)
{
    QgsGeometry merged;
    QgsFeature f;
    for (QgsFeatureIterator it = layer.getFeatures(); it.nextFeature(f);) {
        if (!f.hasGeometry()) continue;
        QgsGeometry g = f.geometry();
        merged = merged.isNull() ? g : merged.combine(g);
    }
    return merged;
}

static bool isSeriesErrorMessage(const QString& text)
{
    return text.contains(QStringLiteral("超出")) || text.contains(QStringLiteral("错位")) || text.contains(QStringLiteral("偏移")) || text.contains(QStringLiteral("未居中"));
}

static QStringList splitSeriesList(const QString& text)
{
    QStringList out = text.split(QRegularExpression(QStringLiteral("[;,；，、]+")), Qt::SkipEmptyParts);
    QStringList dedup;
    for (QString v : out) {
        v = v.trimmed();
        if (!v.isEmpty() && !dedup.contains(v)) dedup << v;
    }
    return dedup;
}

static QStringList expandSeriesLayerGroup(const QString& layerName)
{
    const QString key = layerName.trimmed();
    if (key == QStringLiteral("境界线")) return QStringList() << QStringLiteral("国界线") << QStringLiteral("省级行政区界线") << QStringLiteral("市级行政区界线") << QStringLiteral("县级行政区界线") << QStringLiteral("乡镇级行政区界线");
    if (key == QStringLiteral("道路")) return QStringList() << QStringLiteral("高速公路") << QStringLiteral("快速路") << QStringLiteral("国道") << QStringLiteral("省道") << QStringLiteral("县道") << QStringLiteral("乡道") << QStringLiteral("专用公路") << QStringLiteral("连接道") << QStringLiteral("村道");
    if (key == QStringLiteral("水系线")) return QStringList() << QStringLiteral("一级河流") << QStringLiteral("二级河流") << QStringLiteral("三级河流") << QStringLiteral("四级河流") << QStringLiteral("五级河流") << QStringLiteral("六级河流") << QStringLiteral("等外河流") << QStringLiteral("时令河") << QStringLiteral("水系") << QStringLiteral("沟渠") << QStringLiteral("总干渠") << QStringLiteral("干渠") << QStringLiteral("支渠");
    return QStringList() << key;
}

static QStringList expandSeriesLayerGroups(const QStringList& groups)
{
    QStringList expanded;
    for (const QString& group : groups) {
        for (const QString& name : expandSeriesLayerGroup(group)) {
            if (!expanded.contains(name)) expanded << name;
        }
    }
    return expanded;
}

static QString stripShpSuffix(const QString& name)
{
    QString v = name.trimmed();
    if (v.endsWith(QStringLiteral(".shp"), Qt::CaseInsensitive)) v.chop(4);
    return v;
}

static QString normalizeSeriesLayerFileBase(QString name)
{
    name = QFileInfo(stripShpSuffix(name)).completeBaseName().trimmed();
    static const QStringList suffixes{
        QStringLiteral("_LineString"),
        QStringLiteral("_MultiLineString"),
        QStringLiteral("_Point"),
        QStringLiteral("_MultiPoint"),
        QStringLiteral("_Polygon"),
        QStringLiteral("_MultiPolygon")
    };
    for (const QString& suffix : suffixes) {
        if (name.endsWith(suffix, Qt::CaseInsensitive)) {
            name.chop(suffix.size());
            break;
        }
    }
    return name.trimmed();
}

static QString normalizeSeriesLayerAliasText(QString name)
{
    name = normalizeSeriesLayerFileBase(name);
    name.remove(QRegularExpression(QStringLiteral("[\\s_\\-－—]+")));
    return name.trimmed().toLower();
}

static void addSeriesLayerAliasGroup(QMap<QString, QString>& aliases, const QString& category, const QStringList& names)
{
    const QString categoryText = normalizeSeriesLayerFileBase(category);
    if (categoryText.isEmpty()) return;
    QStringList allNames = names;
    allNames << categoryText;
    for (const QString& name : allNames) {
        const QString key = normalizeSeriesLayerAliasText(name);
        if (!key.isEmpty()) aliases[key] = categoryText;
    }
}

static QMap<QString, QString> parseSeriesLayerNameAliases(const QString& text)
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
        addSeriesLayerAliasGroup(aliases, category, names);
    }
    return aliases;
}

static QString seriesLayerCategoryKey(const QString& shpName, const QMap<QString, QString>& aliases)
{
    const QString fallback = normalizeSeriesLayerFileBase(shpName);
    const QString key = normalizeSeriesLayerAliasText(shpName);
    if (!key.isEmpty() && aliases.contains(key)) return aliases.value(key);
    return fallback;
}

static QString seriesLayerAnnotationClass(const QString& shpName)
{
    const QString base = normalizeSeriesLayerFileBase(shpName);
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
        subjectHas(QStringLiteral("常年河")) || subjectHas(QStringLiteral("干涸河")) ||
        subjectHas(QStringLiteral("时令河")) || subjectHas(QStringLiteral("沟渠")) ||
        subjectHas(QStringLiteral("水渠")) || subjectHas(QStringLiteral("干渠")) ||
        subjectHas(QStringLiteral("支渠")) || subjectHas(QStringLiteral("水库")) ||
        subjectHas(QStringLiteral("湖泊")) || subjectHas(QStringLiteral("水域"))) {
        if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("hydrology_annotation:代码标注");
        if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("hydrology_annotation:名称标注");
        if (annotationType == QStringLiteral("注记")) return QStringLiteral("hydrology_annotation:注记");
        return QStringLiteral("hydrology_annotation:标注");
    }
    if (subjectHas(QStringLiteral("山脉")) || subjectHas(QStringLiteral("山岭"))) {
        if (annotationType == QStringLiteral("代码标注")) return QStringLiteral("mountain_annotation:代码标注");
        if (annotationType == QStringLiteral("名称标注")) return QStringLiteral("mountain_annotation:名称标注");
        if (annotationType == QStringLiteral("注记")) return QStringLiteral("mountain_annotation:注记");
        return QStringLiteral("mountain_annotation:标注");
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

static QString seriesLayerFamily(const QString& shpName, const QMap<QString, QString>& aliases)
{
    const QString base = normalizeSeriesLayerFileBase(shpName);
    const QString category = seriesLayerCategoryKey(shpName, aliases);
    const QString text = base + QStringLiteral("|") + category;
    auto has = [&](const QString& token) { return text.contains(token, Qt::CaseInsensitive); };

    const QString annotationClass = seriesLayerAnnotationClass(shpName);
    if (!annotationClass.isEmpty()) return annotationClass.section(QLatin1Char(':'), 0, 0);

    if (has(QStringLiteral("公路隧道")) || has(QStringLiteral("公路隧道口"))) {
        return QStringLiteral("road_facility");
    }
    if (has(QStringLiteral("水系面")) || has(QStringLiteral("河流面")) || has(QStringLiteral("水域面")) ||
        has(QStringLiteral("水域")) || has(QStringLiteral("湖泊")) || has(QStringLiteral("坑塘")) ||
        (has(QStringLiteral("水库")) && !has(QStringLiteral("不依比例水库")))) {
        return QStringLiteral("hydrology_area");
    }
    if (has(QStringLiteral("山脉")) || has(QStringLiteral("山岭"))) {
        return QStringLiteral("mountain");
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

static bool seriesLayerLooksHydrologyAreaAnnotation(const QString& shpName)
{
    const QString base = normalizeSeriesLayerFileBase(shpName);
    return base.contains(QStringLiteral("河流面"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("水域面"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("水域"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("湖泊"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("水库"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("坑塘"), Qt::CaseInsensitive)
        || base.contains(QStringLiteral("区域标注"), Qt::CaseInsensitive);
}

static QStringList seriesLayerAliasNamesForCategory(const QMap<QString, QString>& aliases, const QString& category)
{
    QStringList names;
    const QString categoryKey = seriesLayerCategoryKey(category, aliases);
    if (!categoryKey.isEmpty()) names << categoryKey;
    for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
        if (it.value() == categoryKey && !names.contains(it.key())) names << it.key();
    }
    return names;
}

static QMap<QString, QStringList> collectSeriesPathGroupsByAlias(const QString& category, const QMap<QString, QString>& aliases, const QString& shpRootDir)
{
    QMap<QString, QStringList> groupedPaths;
    if (category.trimmed().isEmpty() || shpRootDir.trimmed().isEmpty()) return groupedPaths;

    const QString categoryKey = seriesLayerCategoryKey(category, aliases);
    const QStringList dirs = shpRootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) {
            append501DebugLog(QStringLiteral("[501] collectSeriesPathGroupsByAlias rootNotExist dir=%1 category=%2").arg(dirPath, category));
            continue;
        }
        const QStringList shpFiles = dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files, QDir::Name);
        for (const QString& fileName : shpFiles) {
            const QString path = dir.filePath(fileName);
            const QString pathCategory = seriesLayerCategoryKey(fileName, aliases);
            if (pathCategory.compare(categoryKey, Qt::CaseInsensitive) != 0) continue;
            QStringList& paths = groupedPaths[categoryKey];
            if (!paths.contains(path)) paths << path;
        }
    }
    return groupedPaths;
}

static QMap<QString, QStringList> collectSeriesPathGroupsByFamily(const QString& category, const QMap<QString, QString>& aliases, const QString& shpRootDir)
{
    QMap<QString, QStringList> groupedPaths;
    const QString targetFamily = seriesLayerFamily(category, aliases);
    if (targetFamily.isEmpty()) return collectSeriesPathGroupsByAlias(category, aliases, shpRootDir);
    if (shpRootDir.trimmed().isEmpty()) return groupedPaths;

    const QStringList dirs = shpRootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) {
            append501DebugLog(QStringLiteral("[501] collectSeriesPathGroupsByFamily rootNotExist dir=%1 category=%2").arg(dirPath, category));
            continue;
        }
        const QStringList shpFiles = dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files, QDir::Name);
        for (const QString& fileName : shpFiles) {
            const QString pathFamily = seriesLayerFamily(fileName, aliases);
            if (pathFamily != targetFamily) continue;
            const QString path = dir.filePath(fileName);
            QStringList& paths = groupedPaths[category];
            if (!paths.contains(path)) paths << path;
        }
    }
    return groupedPaths;
}

static QMap<QString, QStringList> collectSeriesAnnotationPathGroupsForCategory(const QString& category, const QMap<QString, QString>& aliases, const QString& shpRootDir)
{
    QMap<QString, QStringList> groupedPaths;
    const QString targetFamily = seriesLayerFamily(category, aliases);
    if (targetFamily.isEmpty() || shpRootDir.trimmed().isEmpty()) return groupedPaths;

    QStringList acceptedFamilies;
    if (targetFamily == QStringLiteral("hydrology") || targetFamily == QStringLiteral("hydrology_area")) {
        acceptedFamilies << QStringLiteral("hydrology_annotation");
    } else if (targetFamily == QStringLiteral("road")) {
        acceptedFamilies << QStringLiteral("road_annotation");
    } else if (targetFamily == QStringLiteral("boundary")) {
        acceptedFamilies << QStringLiteral("boundary_annotation");
    } else if (targetFamily == QStringLiteral("mountain")) {
        acceptedFamilies << QStringLiteral("mountain_annotation");
    } else if (targetFamily.endsWith(QStringLiteral("_annotation"))) {
        acceptedFamilies << targetFamily;
    }
    if (acceptedFamilies.isEmpty()) return groupedPaths;

    const QStringList dirs = shpRootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) continue;
        const QStringList shpFiles = dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files, QDir::Name);
        for (const QString& fileName : shpFiles) {
            const QString pathFamily = seriesLayerFamily(fileName, aliases);
            if (!acceptedFamilies.contains(pathFamily)) continue;
            if (targetFamily == QStringLiteral("hydrology") && seriesLayerLooksHydrologyAreaAnnotation(fileName)) continue;
            if (targetFamily == QStringLiteral("hydrology_area") && !seriesLayerLooksHydrologyAreaAnnotation(fileName)) continue;
            const QString path = dir.filePath(fileName);
            QStringList& paths = groupedPaths[category];
            if (!paths.contains(path)) paths << path;
        }
    }
    return groupedPaths;
}

static QJsonObject confirmedSeriesLayerMappingRoot(const AlgorithmFilePaths& filePaths)
{
    if (filePaths.seriesLayerMappingJson.trimmed().isEmpty()) return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8());
    return doc.isObject() ? doc.object() : QJsonObject();
}

static QJsonObject confirmedLayerMetadataForStandardLayer(const QJsonObject& mappingRoot, const QString& stdName)
{
    const QJsonObject layers = mappingRoot.value(QStringLiteral("layers")).toObject();
    const QJsonObject layerObj = layers.value(stdName).toObject();
    QJsonObject metadata;
    if (layerObj.contains(QStringLiteral("match_family"))) metadata[QStringLiteral("match_family")] = layerObj.value(QStringLiteral("match_family"));
    if (layerObj.contains(QStringLiteral("relation_type"))) metadata[QStringLiteral("relation_type")] = layerObj.value(QStringLiteral("relation_type"));
    return metadata;
}

static QMap<QString, QStringList> confirmedLayerGroupsForStandardLayer(const QJsonObject& mappingRoot, const QString& stdName, const QString& side)
{
    QMap<QString, QStringList> groups;
    const QJsonObject layers = mappingRoot.value(QStringLiteral("layers")).toObject();
    if (!layers.contains(stdName)) return groups;

    const QJsonObject layerObj = layers.value(stdName).toObject();
    const QJsonArray paths = SeriesLayerMapping::selectedPaths(layerObj, side);
    QStringList confirmedPaths;
    for (const QJsonValue& value : paths) {
        const QString path = value.toString().trimmed();
        if (!path.isEmpty() && QFileInfo::exists(path) && !confirmedPaths.contains(path)) {
            confirmedPaths << path;
        }
    }
    if (!confirmedPaths.isEmpty()) groups[stdName] = confirmedPaths;
    return groups;
}

static QStringList resolveLayerFileCandidates(const QString& rootDir, const QString& layerName)
{
    QStringList out;
    if (rootDir.trimmed().isEmpty()) {
        append501DebugLog(QStringLiteral("[501] resolveLayerFileCandidates emptyRootDir layer=%1").arg(layerName));
        return out;
    }
    const QStringList dirs = rootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) {
            append501DebugLog(QStringLiteral("[501] resolveLayerFileCandidates rootNotExist dir=%1 layer=%2").arg(dirPath, layerName));
            continue;
        }
        const QString base = stripShpSuffix(layerName);
        append501DebugLog(QStringLiteral("[501] resolveLayerFileCandidates begin dir=%1 layer=%2 base=%3").arg(dirPath, layerName, base));
        const QStringList candidates = QStringList() << (base + QStringLiteral(".shp")) << (base + QStringLiteral("_Point.shp")) << (base + QStringLiteral("_LineString.shp")) << (base + QStringLiteral("_Polygon.shp")) << (base + QStringLiteral("_MultiPoint.shp")) << (base + QStringLiteral("_MultiLineString.shp")) << (base + QStringLiteral("_MultiPolygon.shp"));
        for (const QString& candidate : candidates) {
            const QString path = dir.filePath(candidate);
            const bool exists = QFile::exists(path);
            append501DebugLog(QStringLiteral("[501] resolveCandidate path=%1 exists=%2").arg(path).arg(exists ? QStringLiteral("true") : QStringLiteral("false")));
            if (exists && !out.contains(path)) out << path;
        }
    }
    append501DebugLog(QStringLiteral("[501] resolveLayerFileCandidates end layer=%1 count=%2").arg(layerName).arg(out.size()));
    return out;
}

static QStringList getActualPathsForSeriesLayer(const QString& stdLayerNames, const QString& shpRootDir)
{
    QStringList paths;
    append501DebugLog(QStringLiteral("[501] getActualPaths begin std=%1 dir=%2").arg(stdLayerNames, shpRootDir));
    const QStringList groups = splitSeriesList(stdLayerNames);
    append501DebugLog(QStringLiteral("[501] getActualPaths splitGroups=%1").arg(groups.join(QStringLiteral("、"))));
    const QStringList expanded = expandSeriesLayerGroups(groups);
    append501DebugLog(QStringLiteral("[501] getActualPaths expanded=%1").arg(expanded.join(QStringLiteral("、"))));
    for (const QString& name : expanded) {
        const QStringList resolved = resolveLayerFileCandidates(shpRootDir, name);
        append501DebugLog(QStringLiteral("[501] getActualPaths resolved layer=%1 count=%2").arg(name).arg(resolved.size()));
        for (const QString& path : resolved) {
            if (!paths.contains(path)) paths << path;
        }
    }
    append501DebugLog(QStringLiteral("[501] getActualPaths end std=%1 total=%2").arg(stdLayerNames).arg(paths.size()));
    return paths;
}

static QMap<QString, QStringList> getPathGroupsForStdLayers(const QString& stdLayerNames, const QString& csvPath, const QString& shpRootDir)
{
    QMap<QString, QStringList> groupedPaths;
    if (csvPath.isEmpty() || shpRootDir.isEmpty() || stdLayerNames.isEmpty()) return groupedPaths;
    const QMap<QString, CsvLayerInfo> layerMap = map_check_task_manager::parseCsvToLayerMap(csvPath);
    if (layerMap.isEmpty()) return groupedPaths;
    const QStringList stdNames = expandSeriesLayerGroups(splitSeriesList(stdLayerNames));
    for (const QString& stdName : stdNames) {
        const QString key = stdName.trimmed();
        if (!layerMap.contains(key)) continue;
        QStringList paths;
        const CsvLayerInfo& layerInfo = layerMap.value(key);
        for (const QString& actualName : layerInfo.actualLayerNames) {
            for (const QString& p : resolveLayerFileCandidates(shpRootDir, actualName)) {
                if (!paths.contains(p)) paths << p;
            }
        }
        if (!paths.isEmpty()) groupedPaths.insert(key, paths);
    }
    return groupedPaths;
}

static double seriesDistanceToGround(double value, const QString& unit, int scale)
{
    if (unit.contains(QStringLiteral("毫米"))) {
        if (scale <= 0) {
            return -1.0;
        }
        return value * scale / 1000.0;
    }
    return value;
}

static QgsGeometry collectLayerGeometry(QgsVectorLayer& layer, const QgsGeometry& filter)
{
    QgsGeometry merged; QgsFeature f; QgsFeatureRequest req; req.setFilterRect(filter.boundingBox());
    for (QgsFeatureIterator it = layer.getFeatures(req); it.nextFeature(f);) {
        if (!f.hasGeometry() || !f.geometry().intersects(filter)) continue;
        QgsGeometry g = f.geometry(); merged = merged.isNull() ? g : merged.combine(g);
    }
    return merged;
}

static bool extractLineEndpoints(const QgsGeometry& geom, QgsPointXY& startPt, QgsPointXY& endPt)
{
    if (QgsWkbTypes::geometryType(geom.wkbType()) != QgsWkbTypes::LineGeometry) return false;
    QgsVertexIterator it = geom.vertices();
    if (!it.hasNext()) return false;
    QgsPoint first = it.next();
    QgsPoint last = first;
    while (it.hasNext()) last = it.next();
    if (first == last) return false;
    startPt = QgsPointXY(first);
    endPt = QgsPointXY(last);
    return true;
}

static QString pointText(const QgsPointXY& pt)
{
    return QStringLiteral("X=%1, Y=%2").arg(pt.x(), 0, 'f', 3).arg(pt.y(), 0, 'f', 3);
}

struct SeriesNamedPoint { QString name; QgsPointXY point; };
struct SeriesFeatureRef { QgsFeature feature; QString shpPath; QString shpName; QString standardLayerName; QString sheetName; };
static QString seriesFeatureId(const SeriesFeatureRef& ref)
{
    return ref.feature.isValid() ? QString::number(ref.feature.id()) : QString();
}
static void writeSeriesFeatureRef(QJsonObject& obj, const QString& prefix, const SeriesFeatureRef& ref)
{
    if (ref.feature.isValid() && ref.feature.hasGeometry())
        obj.insert(prefix + QStringLiteral("_check_wkt"), ref.feature.geometry().asWkt());
    obj.insert(prefix + QStringLiteral("_shp"), ref.shpName.isEmpty() ? QJsonValue::Null : QJsonValue(ref.shpName));
    obj.insert(prefix + QStringLiteral("_shp_path"), ref.shpPath.isEmpty() ? QJsonValue::Null : QJsonValue(ref.shpPath));
    obj.insert(prefix + QStringLiteral("_standard_layer"), ref.standardLayerName.isEmpty() ? QJsonValue::Null : QJsonValue(ref.standardLayerName));
    const QString fid = seriesFeatureId(ref);
    obj.insert(prefix + QStringLiteral("_fid"), fid.isEmpty() ? QJsonValue::Null : QJsonValue(fid));
}

static void appendUniqueSeriesPoint(QList<QgsPointXY>& pts, const QgsPointXY& pt)
{
    for (const QgsPointXY& cur : pts) if (QgsGeometry::fromPointXY(cur).distance(QgsGeometry::fromPointXY(pt)) <= 1e-8) return;
    pts << pt;
}

static QList<QgsPointXY> collectSeriesLinePartEndpoints(const QgsGeometry& geom)
{
    QList<QgsPointXY> pts; const auto flat = QgsWkbTypes::flatType(geom.wkbType());
    if (flat == QgsWkbTypes::LineString) { const QgsPolylineXY line = geom.asPolyline(); if (line.size() >= 2) { appendUniqueSeriesPoint(pts, line.first()); appendUniqueSeriesPoint(pts, line.last()); } }
    else if (flat == QgsWkbTypes::MultiLineString) { const QgsMultiPolylineXY lines = geom.asMultiPolyline(); for (const QgsPolylineXY& line : lines) if (line.size() >= 2) { appendUniqueSeriesPoint(pts, line.first()); appendUniqueSeriesPoint(pts, line.last()); } }
    return pts;
}

static QList<QgsPointXY> collectSeriesCrossPoints(const QgsGeometry& geom)
{
    QList<QgsPointXY> pts; const auto flat = QgsWkbTypes::flatType(geom.wkbType());
    if (flat == QgsWkbTypes::Point) appendUniqueSeriesPoint(pts, geom.asPoint());
    else if (flat == QgsWkbTypes::MultiPoint) { const QgsMultiPointXY points = geom.asMultiPoint(); for (const QgsPointXY& pt : points) appendUniqueSeriesPoint(pts, pt); }
    else pts = collectSeriesLinePartEndpoints(geom);
    return pts;
}

static QList<QgsPointXY> collectSeriesCommonEdgeCrossPoints(const QgsGeometry& geom, const QgsGeometry& commonEdge)
{
    if (commonEdge.isNull() || commonEdge.isEmpty()) return QList<QgsPointXY>();
    try { return collectSeriesCrossPoints(geom.intersection(commonEdge)); }
    catch (...) { return QList<QgsPointXY>(); }
}

static bool isSeriesPointNearEdge(const QgsPointXY& pt, const QgsGeometry& edgeZone, const QgsGeometry& commonEdge, double tol)
{
    const QgsGeometry g = QgsGeometry::fromPointXY(pt);
    return (!edgeZone.isNull() && !edgeZone.isEmpty() && g.intersects(edgeZone)) || (!commonEdge.isNull() && !commonEdge.isEmpty() && g.distance(commonEdge) <= tol);
}

static QList<SeriesNamedPoint> collectOriginalEndpoints(const QgsGeometry& geom, const QgsGeometry& edgeZone, const QgsGeometry& commonEdge, double tol)
{
    QList<SeriesNamedPoint> pts; QgsPointXY s, e; if (!extractLineEndpoints(geom, s, e)) return pts;
    if (isSeriesPointNearEdge(s, edgeZone, commonEdge, tol)) pts << SeriesNamedPoint{ QStringLiteral("起点"), s };
    if (isSeriesPointNearEdge(e, edgeZone, commonEdge, tol)) pts << SeriesNamedPoint{ QStringLiteral("终点"), e };
    return pts;
}

static QList<QgsPointXY> namedSeriesPoints(const QList<SeriesNamedPoint>& pts)
{
    QList<QgsPointXY> out; for (const SeriesNamedPoint& pt : pts) appendUniqueSeriesPoint(out, pt.point); return out;
}

static bool isSeriesPointOnFeature(const QgsPointXY& pt, const QgsGeometry& geom, double tol, double* distance = nullptr)
{
    if (geom.isNull() || geom.isEmpty()) { if (distance) *distance = -1.0; return false; }
    const double d = QgsGeometry::fromPointXY(pt).distance(geom);
    if (distance) *distance = d;
    return d <= tol;
}

static bool pickSeriesMissingErrorPoint(const SeriesFeatureRef& leftRef, const QgsGeometry& commonEdge, double tol, QgsPointXY& pt, QString& pointSource, QString& endpointType)
{
    if (!leftRef.feature.isValid() || !leftRef.feature.hasGeometry() || commonEdge.isNull() || commonEdge.isEmpty()) return false;
    const QgsGeometry geom = leftRef.feature.geometry();
    const QList<QgsPointXY> crossPts = collectSeriesCommonEdgeCrossPoints(geom, commonEdge);
    if (!crossPts.isEmpty()) { pt = crossPts.first(); pointSource = QStringLiteral("common_edge_cross"); endpointType.clear(); return true; }
    QgsPointXY s, e; if (!extractLineEndpoints(geom, s, e)) return false;
    const double ds = QgsGeometry::fromPointXY(s).distance(commonEdge), de = QgsGeometry::fromPointXY(e).distance(commonEdge);
    if (qMin(ds, de) > tol) return false;
    pt = ds <= de ? s : e;
    pointSource = QStringLiteral("nearest_endpoint");
    endpointType = ds <= de ? QStringLiteral("起点") : QStringLiteral("终点");
    return true;
}

static void writeSeriesPointDiagnostics(QJsonObject& ext, const QgsPointXY& pt, const QString& pointSource, const SeriesFeatureRef& leftRef, const QgsGeometry& commonEdge)
{
    ext["check_point"] = pointText(pt); ext["point_x"] = pt.x(); ext["point_y"] = pt.y(); ext["point_source"] = pointSource;
    ext["point_to_left_feature_dist"] = leftRef.feature.isValid() && leftRef.feature.hasGeometry() ? QgsGeometry::fromPointXY(pt).distance(leftRef.feature.geometry()) : QJsonValue::Null;
    ext["point_to_common_edge_dist"] = (!commonEdge.isNull() && !commonEdge.isEmpty()) ? QJsonValue(QgsGeometry::fromPointXY(pt).distance(commonEdge)) : QJsonValue::Null;
}
static void writeSeriesLineDiagnostics(QJsonObject& ext, const QgsPointXY& leftPt, const QgsPointXY& rightPt, const QString& lineSource)
{
    ext["left_point"] = pointText(leftPt); ext["right_point"] = pointText(rightPt); ext["left_x"] = leftPt.x(); ext["left_y"] = leftPt.y(); ext["right_x"] = rightPt.x(); ext["right_y"] = rightPt.y(); ext["line_source"] = lineSource;
}
static QString seriesShpBaseName(const QString& shpName)
{
    return QFileInfo(shpName).completeBaseName().trimmed();
}
static int seriesBoundaryLevel(const QString& shpName)
{
    const QString n = seriesShpBaseName(shpName);
    if (n.contains(QStringLiteral("国界线"))) return 0;
    if (n.contains(QStringLiteral("省级行政区界线"))) return 1;
    if (n.contains(QStringLiteral("市级行政区界线"))) return 2;
    if (n.contains(QStringLiteral("县级行政区界线"))) return 3;
    if (n.contains(QStringLiteral("乡镇级行政区界线"))) return 4;
    return -1;
}
static bool isSeriesEndpointLayerCompatible(const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef)
{
    if (!leftRef.standardLayerName.trimmed().isEmpty() || !rightRef.standardLayerName.trimmed().isEmpty()) {
        return !leftRef.standardLayerName.trimmed().isEmpty()
            && !rightRef.standardLayerName.trimmed().isEmpty()
            && leftRef.standardLayerName.trimmed() == rightRef.standardLayerName.trimmed();
    }
    const QString leftBase = seriesShpBaseName(leftRef.shpName), rightBase = seriesShpBaseName(rightRef.shpName);
    if (leftBase.isEmpty() || rightBase.isEmpty()) return false;
    if (leftBase == rightBase) return true;
    const int leftLevel = seriesBoundaryLevel(leftRef.shpName), rightLevel = seriesBoundaryLevel(rightRef.shpName);
    if (leftLevel >= 0 || rightLevel >= 0) return false;
    static const QStringList allowedPairs;
    const QString key = leftBase < rightBase ? (leftBase + QStringLiteral("|") + rightBase) : (rightBase + QStringLiteral("|") + leftBase);
    return allowedPairs.contains(key);
}
static bool isSeriesEndpointPairEligible(const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, const SeriesNamedPoint& leftPt, const SeriesNamedPoint& rightPt, const QgsGeometry& commonEdge, double searchTol, double* projectionGap = nullptr)
{
    if (projectionGap) *projectionGap = -1.0;
    if (!isSeriesEndpointLayerCompatible(leftRef, rightRef) || commonEdge.isNull() || commonEdge.isEmpty()) return false;
    const double leftEdgeDist = QgsGeometry::fromPointXY(leftPt.point).distance(commonEdge), rightEdgeDist = QgsGeometry::fromPointXY(rightPt.point).distance(commonEdge);
    if (leftEdgeDist > searchTol || rightEdgeDist > searchTol) return false;
    const double endpointDistance = QgsGeometry::fromPointXY(leftPt.point).distance(QgsGeometry::fromPointXY(rightPt.point));
    if (endpointDistance > searchTol) return false;
    const QgsGeometry leftProj = commonEdge.nearestPoint(QgsGeometry::fromPointXY(leftPt.point)), rightProj = commonEdge.nearestPoint(QgsGeometry::fromPointXY(rightPt.point));
    if (leftProj.isNull() || rightProj.isNull() || leftProj.isEmpty() || rightProj.isEmpty()) return false;
    const double gap = leftProj.distance(rightProj);
    if (projectionGap) *projectionGap = gap;
    return gap <= searchTol;
}
static bool findNearestSeriesPointPair(const QList<QgsPointXY>& leftPts, const QList<QgsPointXY>& rightPts, QgsPointXY& leftPt, QgsPointXY& rightPt, double& distance)
{
    if (leftPts.isEmpty() || rightPts.isEmpty()) return false;
    distance = std::numeric_limits<double>::max();
    for (const QgsPointXY& lp : leftPts) for (const QgsPointXY& rp : rightPts) { const double d = QgsGeometry::fromPointXY(lp).distance(QgsGeometry::fromPointXY(rp)); if (d < distance) { distance = d; leftPt = lp; rightPt = rp; } }
    return true;
}

static void buildFeatureIndex(const QList<QgsFeature>& features, QgsSpatialIndex& index, QMap<qint64, QgsFeature>& featureMap)
{
    featureMap.clear();
    for (const QgsFeature& srcFeature : features) {
        if (!srcFeature.hasGeometry()) continue;
        QgsFeature feature = srcFeature;
        index.addFeature(feature);
        featureMap.insert(feature.id(), feature);
    }
}

static bool findNearestFeatureByIndex(const QgsSpatialIndex& index, const QMap<qint64, QgsFeature>& featureMap, const QgsGeometry& geom, double tolerance, QgsFeature& best, double* bestDistance = nullptr)
{
    QgsRectangle rect = geom.boundingBox();
    rect.grow(tolerance);
    const QList<QgsFeatureId> ids = index.intersects(rect);
    double minDistance = std::numeric_limits<double>::max();
    bool found = false;
    for (QgsFeatureId id : ids) {
        if (!featureMap.contains(id)) continue;
        const QgsFeature& ref = featureMap[id];
        const double distance = geom.distance(ref.geometry());
        if (distance <= tolerance && distance < minDistance) {
            minDistance = distance;
            best = ref;
            found = true;
        }
    }
    if (bestDistance) *bestDistance = found ? minDistance : -1.0;
    return found;
}

static QgsGeometry buildSeriesEdgeZone(const AlgorithmFilePaths& filePaths, double bufferDistance, QJsonObject& pairInfo)
{
    pairInfo = QJsonObject();
    append501DebugLog(QStringLiteral("[501] buildEdgeZone begin pair=%1").arg(filePaths.seriesPairName));
    if (!filePaths.seriesLeftSheetWkt.isEmpty() && !filePaths.seriesRightSheetWkt.isEmpty()) {
        QgsGeometry leftGeom = QgsGeometry::fromWkt(filePaths.seriesLeftSheetWkt);
        QgsGeometry rightGeom = QgsGeometry::fromWkt(filePaths.seriesRightSheetWkt);
        if (!leftGeom.isGeosValid()) leftGeom = leftGeom.makeValid();
        if (!rightGeom.isGeosValid()) rightGeom = rightGeom.makeValid();
        try {
            const QgsGeometry commonEdge = leftGeom.intersection(rightGeom);
            if (!commonEdge.isNull() && !commonEdge.isEmpty() && commonEdge.length() > 0.0) {
                const QgsGeometry edgeZone = commonEdge.buffer(bufferDistance, 8);
                if (!edgeZone.isNull() && !edgeZone.isEmpty()) {
                    pairInfo["pair_name"] = filePaths.seriesPairName;
                    pairInfo["left_sheet"] = filePaths.seriesLeftSheetName;
                    pairInfo["right_sheet"] = filePaths.seriesRightSheetName;
                    pairInfo["mode"] = QStringLiteral("pair_common_edge");
                    pairInfo["common_edge_length"] = commonEdge.length();
                    pairInfo["common_edge_wkt"] = commonEdge.asWkt();
                    append501DebugLog(QStringLiteral("[501] buildEdgeZone ok pair=%1 commonEdgeLength=%2").arg(filePaths.seriesPairName).arg(commonEdge.length(), 0, 'f', 3));
                    return edgeZone;
                }
            }
        }
        catch (const std::exception& ex) {
            pairInfo["edge_build_error"] = QStringLiteral("公共边或缓冲区构建异常：%1").arg(QString::fromUtf8(ex.what()));
            append501DebugLog(QStringLiteral("[501] buildEdgeZone exception pair=%1 msg=%2").arg(filePaths.seriesPairName, QString::fromUtf8(ex.what())));
        }
        catch (...) {
            pairInfo["edge_build_error"] = QStringLiteral("公共边或缓冲区构建发生未知异常");
            append501DebugLog(QStringLiteral("[501] buildEdgeZone unknownException pair=%1").arg(filePaths.seriesPairName));
        }
    }

    const QString innerFramePath = QDir(filePaths.shpPath).filePath(QStringLiteral("内图廓.shp"));
    QgsVectorLayer frame(innerFramePath, "frame", "ogr");
    if (!frame.isValid()) return QgsGeometry();
    const QgsGeometry frameGeom = collectWholeLayerGeometry(frame);
    const QgsGeometry outerEdgeZone = frameGeom.buffer(bufferDistance, 8);
    const QgsGeometry innerEdgeZone = frameGeom.buffer(-bufferDistance, 8);
    pairInfo["mode"] = QStringLiteral("inner_frame_fallback");
    return innerEdgeZone.isNull() ? outerEdgeZone : outerEdgeZone.difference(innerEdgeZone);
}

void SeriesMapTemplateCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString&, CheckItemResult& result)
{
    append501DebugLog(QStringLiteral("[501] wrapper begin pair=%1 shpPath=%2 scale=%3").arg(filePaths.seriesPairName, filePaths.shpPath).arg(scale));
    result.id = "501";
    result.name = checkItemName;
    result.status = "质检失败";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    result.info["execution_state"] = QStringLiteral("input_error");
    const QString xmlPath = MatchXmlByLayerName(filePaths.xmlPaths, filePaths.standardLayer);
    QualityCheckConfig cfg; QStringList errs;
    if (xmlPath.isEmpty() || !cfg.loadFromFile(xmlPath) || !cfg.validateSeriesMapTemplate(&errs)) { result.err_exist = true; QJsonObject err; err["description"] = errs.join("；"); result.errors["template_error"] = err; append501DebugLog(QStringLiteral("[501] xml load failed pair=%1 err=%2").arg(filePaths.seriesPairName, errs.join(QStringLiteral("；")))); return; }
    append501DebugLog(QStringLiteral("[501] xml load ok pair=%1 xml=%2").arg(filePaths.seriesPairName, xmlPath));
    const CheckItemConfig c = cfg.getCheckItemConfig(checkItemName);
    auto S = [&](const QString& k, const QString& d = QString()) { return c.parameters.value(k, d).toString().trimmed(); };
    auto D = [&](const QString& k, double d = 0.0) { return c.parameters.value(k, d).toDouble(); };
    auto B = [&](const QString& k, bool d = false) { QVariant v = c.parameters.value(k, d); return v.toBool() || v.toString().trimmed().compare("true", Qt::CaseInsensitive) == 0; };
    auto F = [&](const QString& key, const QMap<QString, QString>& alias) { QStringList out; for (const QString& n : splitSeriesList(S(key))) out << alias.value(n, n); out.removeDuplicates(); return out; };
    const bool isCrossScale = filePaths.checkMode.compare(QStringLiteral("CrossScale"), Qt::CaseInsensitive) == 0;
    QStringList requiredSeriesErrors;
    auto requiredSeriesDouble = [&](const QString& key) {
        bool ok = false;
        const QVariant value = c.parameters.value(key);
        const double parsed = value.toDouble(&ok);
        if (!c.parameters.contains(key) || !ok) {
            requiredSeriesErrors << key;
            return 0.0;
        }
        return parsed;
    };
    const QString nameField = S("series_name_field", QStringLiteral("NAME"));
    const QString levelField = S("series_level_field", QStringLiteral("LEVEL"));
    const QString codeField = S("series_code_field", QStringLiteral("CODE"));
    const QJsonObject seriesContext = QJsonDocument::fromJson(filePaths.seriesLayerMappingJson.toUtf8()).object();
    const QJsonArray sourceAudits = seriesContext.value(QStringLiteral("source_audits")).toArray();
    const QString projectStyleId=S(QStringLiteral("series_project_style_id")).toLower();
    const QStringList mainRoleLayers=splitSeriesList(S(QStringLiteral("series_main_role_layers")));
    const QStringList externalRoleLayers=splitSeriesList(S(QStringLiteral("series_external_role_layers")));
    auto featureMapRole=[&](const SeriesFeatureRef& ref) {
        return SeriesCartographicPolicy::featureRole(ref.feature,ref.shpPath,seriesContext,mainRoleLayers,externalRoleLayers);
    };
    const QString symbolIdField = S("series_symbol_id_field", QStringLiteral("symbol_id"));
    const QString crossScaleKeepLevelField = S("series_cross_scale_keep_level_field", levelField);
    const int crossScaleMinKeepLevel = static_cast<int>(D(QStringLiteral("series_cross_scale_min_keep_level"), 0.0));
    const bool crossScaleLevelSmallerIsHigher = B(QStringLiteral("series_cross_scale_level_smaller_is_higher"), true);
    const bool crossScaleKeepWhenLevelMissing = B(QStringLiteral("series_cross_scale_keep_when_level_missing"), true);
    const double crossScaleOverlapThreshold = isCrossScale ? qBound(0.0, requiredSeriesDouble(QStringLiteral("series_cross_scale_overlap_threshold")), 1.0) : 0.0;
    const double crossScaleWeakSupportScore = isCrossScale ? qBound(0.0, requiredSeriesDouble(QStringLiteral("series_cross_scale_weak_support_score")), 100.0) : 0.0;
    const bool crossScalePolygonAggregationAllowed = B(QStringLiteral("series_cross_scale_polygon_aggregation_allowed"), false);
    if (isCrossScale && !requiredSeriesErrors.isEmpty()) {
        requiredSeriesErrors.removeDuplicates();
        result.err_exist = true;
        QJsonObject err;
        err["error_type"] = QStringLiteral("series_config_error");
        err["sub_rule"] = QStringLiteral("XML规则配置");
        err["description"] = QStringLiteral("XML 缺少或无法解析跨比例尺检查阈值：%1").arg(requiredSeriesErrors.join(QStringLiteral("，")));
        result.errors["series_config_error"] = err;
        append501DebugLog(QStringLiteral("[501] series cross-scale XML required threshold missing pair=%1 rules=%2").arg(filePaths.seriesPairName, requiredSeriesErrors.join(QStringLiteral(","))));
        return;
    }
    const QStringList attributeFields = F("series_attribute_fields", QMap<QString, QString>{ {QStringLiteral("名称"), nameField}, { QStringLiteral("等级"), levelField }, { QStringLiteral("编码"), codeField }});
    const QStringList configuredCrossScaleAttributeFields = F(QStringLiteral("series_cross_scale_attribute_fields"), QMap<QString, QString>{});
    QStringList crossScaleAttributeFields,invalidCrossScaleAttributeFields;
    for(const QString& field:configuredCrossScaleAttributeFields) {
        if(SeriesCartographicPolicy::expressionField(field)) invalidCrossScaleAttributeFields<<field;
        else crossScaleAttributeFields<<field;
    }
    bool attributeComparisonExecuted = !isCrossScale && !attributeFields.isEmpty();
    auto styleFieldsOrDefault = [&](const QString& key, const QStringList& defaults) {
        const QStringList configured = splitSeriesList(S(key));
        return configured.isEmpty() ? defaults : configured;
    };
    const QStringList lineStyleFields = styleFieldsOrDefault(QStringLiteral("series_line_style_fields"), { QStringLiteral("stroked"), QStringLiteral("style_prio"), QStringLiteral("stroke_col"), QStringLiteral("stroke_wid"), QStringLiteral("is_dashed") });
    const QStringList polygonStyleFields = styleFieldsOrDefault(QStringLiteral("series_polygon_style_fields"), { QStringLiteral("closed"), QStringLiteral("filled"), QStringLiteral("stroked"), QStringLiteral("style_prio"), QStringLiteral("fill_color"), QStringLiteral("stroke_col"), QStringLiteral("stroke_wid"), QStringLiteral("is_dashed") });
    const QStringList pointStyleFields = styleFieldsOrDefault(QStringLiteral("series_point_style_fields"), { QStringLiteral("stroked"), QStringLiteral("style_prio"), QStringLiteral("stroke_col"), QStringLiteral("stroke_wid"), QStringLiteral("is_dashed") });
    const QString cartographicSymbolRuleText = S(QStringLiteral("series_cartographic_symbol_rules"));
    const QString cartographicSymbolRuleSourceText = S(QStringLiteral("series_cartographic_symbol_rule_source"), QStringLiteral("XML规则：点/线/面符号样式规范"));
    const QString riverAnnotationGradeRuleText = S(QStringLiteral("series_river_annotation_grade_rules"));
    const QString riverAnnotationGradeRuleSourceText = S(QStringLiteral("series_river_annotation_grade_rule_source"), QStringLiteral("挂图参数 - 0815.doc 附件1：河流分级表"));
    const QString mountainAnnotationGradeRuleText = S(QStringLiteral("series_mountain_annotation_grade_rules"));
    const QString mountainAnnotationGradeRuleSourceText = S(QStringLiteral("series_mountain_annotation_grade_rule_source"), QStringLiteral("挂图参数 - 0815.doc 附件2：山脉，山岭"));
    const QStringList annotationTextFields = styleFieldsOrDefault(QStringLiteral("series_annotation_text_field"), { QStringLiteral("text_conte"), QStringLiteral("text_content"), QStringLiteral("name"), QStringLiteral("NAME") });
    const QStringList annotationFontSizeFields = styleFieldsOrDefault(QStringLiteral("series_annotation_font_size_field"), { QStringLiteral("font_size"), QStringLiteral("text_size"), QStringLiteral("字号") });
    QStringList riverLevelFields = splitSeriesList(S(QStringLiteral("series_river_level_fields")));
    if (riverLevelFields.isEmpty()) riverLevelFields << QStringLiteral("河流等级") << QStringLiteral("分级") << levelField;
    riverLevelFields.removeDuplicates();
    const QString severity = S("series_output_severity", QStringLiteral("错误"));
    const QString boundaryStd = S("series_boundary_line_layer"), adminStd = S("series_admin_area_layer"), roadStd = S("series_road_layer"), waterStd = S("series_water_layer"), waterAreaStd = S("series_water_area_layer");
    const int leftScaleDenom = filePaths.seriesLeftScale > 0 ? filePaths.seriesLeftScale : scale;
    const int rightScaleDenom = filePaths.seriesRightScale > 0 ? filePaths.seriesRightScale : scale;
    QMap<QString, QString> layerNameAliases = parseSeriesLayerNameAliases(S(QStringLiteral("series_layer_name_aliases")));
    for (const QString& category : splitSeriesList(boundaryStd + QStringLiteral("；") + adminStd + QStringLiteral("；") + roadStd + QStringLiteral("；") + waterStd + QStringLiteral("；") + waterAreaStd + QStringLiteral("；") + S(QStringLiteral("series_target_layers")))) {
        addSeriesLayerAliasGroup(layerNameAliases, category, expandSeriesLayerGroup(category));
    }
    const QJsonObject confirmedLayerMapping = confirmedSeriesLayerMappingRoot(filePaths);
    if (!filePaths.seriesLayerMappingJson.trimmed().isEmpty()
        && !confirmedLayerMapping.value(QStringLiteral("layers")).isObject()) {
        result.err_exist = true;
        result.errors[QStringLiteral("mapping_error")] = QJsonObject{
            {QStringLiteral("error_type"), QStringLiteral("series_config_error")},
            {QStringLiteral("description"), QStringLiteral("图层确认配置损坏，请重新确认图层对应关系")}};
        return;
    }
    const bool hasConfirmedLayerMapping = !confirmedLayerMapping.isEmpty();
    auto sourceSheetNameForPath = [&](const QString& path, bool leftSide) {
        const QString fallback = leftSide ? filePaths.seriesLeftSheetName : filePaths.seriesRightSheetName;
        if (!leftSide || filePaths.seriesLargeScaleSheetNames.size() <= 1) return fallback;
        const QStringList roots = filePaths.seriesLeftShpPath.split(QStringLiteral(";"), Qt::SkipEmptyParts);
        if (roots.size() == filePaths.seriesLargeScaleSheetNames.size()) {
            const QString key = SeriesLayerMapping::pathKey(path);
            for (int i = 0; i < roots.size(); ++i) {
                const QString rootKey = SeriesLayerMapping::pathKey(roots[i]) + QLatin1Char('/');
                if (key.startsWith(rootKey)) return filePaths.seriesLargeScaleSheetNames[i];
            }
        }
        return fallback;
    };
    const double registrationError = isCrossScale ? D(QStringLiteral("series_cross_scale_registration_error"), D(QStringLiteral("series_registration_error"), 0.0)) : 0.0;
    auto toleranceForScale = [&](const QString& valueKey, const QString& unitKey, int scaleDenom) {
        return seriesDistanceToGround(D(valueKey), S(unitKey, QStringLiteral("毫米")), scaleDenom);
    };
    auto combineTolerance = [&](const QString& valueKey, const QString& unitKey) {
        const double leftTol = toleranceForScale(valueKey, unitKey, leftScaleDenom);
        const double rightTol = toleranceForScale(valueKey, unitKey, rightScaleDenom);
        if (!isCrossScale) return leftTol;
        return std::sqrt(leftTol * leftTol + rightTol * rightTol) + registrationError;
    };
    const double buf = combineTolerance(QStringLiteral("series_edge_buffer_distance"), QStringLiteral("series_edge_buffer_distance_unit"));
    const double lineTol = combineTolerance(QStringLiteral("series_line_edge_tolerance"), QStringLiteral("series_line_edge_tolerance_unit"));
    const double endTol = combineTolerance(QStringLiteral("series_endpoint_closure_tolerance"), QStringLiteral("series_endpoint_closure_tolerance_unit"));
    const bool featurePointToleranceConfigured = c.parameters.contains(QStringLiteral("series_feature_point_tolerance"));
    const double featurePointTol = featurePointToleranceConfigured
        ? combineTolerance(QStringLiteral("series_feature_point_tolerance"), QStringLiteral("series_feature_point_tolerance_unit"))
        : 0.0;
    const bool crossScaleMaxGapConfigured = c.parameters.contains(QStringLiteral("series_cross_scale_max_uncovered_segment_length"));
    const double crossScaleMaxGapLength = crossScaleMaxGapConfigured
        ? combineTolerance(QStringLiteral("series_cross_scale_max_uncovered_segment_length"), QStringLiteral("series_cross_scale_max_uncovered_segment_length_unit"))
        : -1.0;
    
    if (buf < 0 || lineTol < 0 || endTol < 0) {
        result.err_exist = true;
        QJsonObject err;
        err["description"] = QStringLiteral("规则单位为毫米，但图幅比例尺未识别，无法换算为实地距离。");
        result.errors["input_error"] = err;
        append501DebugLog(QStringLiteral("[501] scale missing for mm unit pair=%1").arg(filePaths.seriesPairName));
        return;
    }
    
    QgsGeometry suppliedCoverage;
    if (isCrossScale) {
        QgsGeometry left = QgsGeometry::fromWkt(filePaths.seriesLeftSheetWkt);
        QgsGeometry right = QgsGeometry::fromWkt(filePaths.seriesRightSheetWkt);
        if (!left.isGeosValid()) left = left.makeValid();
        if (!right.isGeosValid()) right = right.makeValid();
        if (!left.isNull() && !right.isNull()) suppliedCoverage = left.intersection(right);
        suppliedCoverage.convertGeometryCollectionToSubclass(QgsWkbTypes::PolygonGeometry);
        if (left.type() != QgsWkbTypes::PolygonGeometry || right.type() != QgsWkbTypes::PolygonGeometry
            || suppliedCoverage.isNull() || suppliedCoverage.isEmpty() || !suppliedCoverage.isGeosValid()
            || suppliedCoverage.type() != QgsWkbTypes::PolygonGeometry || suppliedCoverage.area() <= 1e-8) {
            result.err_exist = true;
            result.info["execution_state"] = QStringLiteral("invalid_scope");
            result.info["description"] = QStringLiteral("跨比例尺图幅必须具有有效共同覆盖面；公共边线或空范围不能作为覆盖检查区");
            result.errors[QStringLiteral("scope_error")] = QJsonObject{
                {QStringLiteral("error_type"),QStringLiteral("series_scope_error")},
                {QStringLiteral("description"),result.info.value(QStringLiteral("description"))}};
            return;
        }
    }
    QJsonObject pairInfo;
    QgsGeometry edgeZone = buildSeriesEdgeZone(filePaths, buf, pairInfo);
    if (isCrossScale) {
        pairInfo["common_coverage_area_m2"] = suppliedCoverage.area();
        pairInfo["common_coverage_wkt"] = suppliedCoverage.asWkt();
    }
    const QgsGeometry commonEdge = QgsGeometry::fromWkt(pairInfo.value("common_edge_wkt").toString());
    if (edgeZone.isNull() || edgeZone.isEmpty()) { result.err_exist = true; QJsonObject err; err["description"] = pairInfo.value("edge_build_error").toString(QStringLiteral("无法构建图幅公共边缓冲区")); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] edge zone failed pair=%1 reason=%2").arg(filePaths.seriesPairName, err["description"].toString())); return; }
    int idx = 1; QJsonObject summary; QJsonObject problemSummary; QJsonObject geometrySummary; QJsonObject layerPathSummary; QJsonObject geometryTotals; bool hasError = false; bool hasFinding = false; auto geomName = [&](const QgsFeature& f) { if (!f.isValid() || !f.hasGeometry()) return QString(); const auto t = QgsWkbTypes::geometryType(f.geometry().wkbType()); if (t == QgsWkbTypes::LineGeometry) return QStringLiteral("线要素"); if (t == QgsWkbTypes::PolygonGeometry) return QStringLiteral("面要素"); if (t == QgsWkbTypes::PointGeometry) return QStringLiteral("点要素"); return QStringLiteral("未知"); };
    auto addCrossScaleAliases = [&](QJsonObject& err) {
        if (!isCrossScale) return;
        err["large_scale_sheet"] = filePaths.seriesLeftSheetName;
        err["small_scale_sheet"] = filePaths.seriesRightSheetName;
        err["large_scale_scale"] = leftScaleDenom;
        err["small_scale_scale"] = rightScaleDenom;
        err["large_scale_shp"] = err.value(QStringLiteral("left_shp"));
        err["large_scale_shp_path"] = err.value(QStringLiteral("left_shp_path"));
        err["large_scale_fid"] = err.value(QStringLiteral("left_fid"));
        err["small_scale_shp"] = err.value(QStringLiteral("right_shp"));
        err["small_scale_shp_path"] = err.value(QStringLiteral("right_shp_path"));
        err["small_scale_fid"] = err.value(QStringLiteral("right_fid"));
        if (err.contains(QStringLiteral("left_value"))) err["large_scale_value"] = err.value(QStringLiteral("left_value"));
        if (err.contains(QStringLiteral("right_value"))) err["small_scale_value"] = err.value(QStringLiteral("right_value"));
        if (err.contains(QStringLiteral("left_missing"))) err["large_scale_missing"] = err.value(QStringLiteral("left_missing"));
        if (err.contains(QStringLiteral("right_missing"))) err["small_scale_missing"] = err.value(QStringLiteral("right_missing"));
    };
    auto addErr = [&](const QString& rule, const QString& layer, const QString& desc, const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, const QJsonObject& ext = QJsonObject()) {
        QJsonObject err = ext;
        hasFinding = true;
        writeSeriesFeatureRef(err, QStringLiteral("left"), leftRef);
        writeSeriesFeatureRef(err, QStringLiteral("right"), rightRef);
        addCrossScaleAliases(err);
        if (!err.contains("geometry_type")) err["geometry_type"] = !geomName(leftRef.feature).isEmpty() ? geomName(leftRef.feature) : geomName(rightRef.feature);
        if (!err.contains("match_status")) err["match_status"] = rightRef.feature.isValid() ? QStringLiteral("matched") : QStringLiteral("unmatched");
        if (!err.contains("candidate_count")) err["candidate_count"] = rightRef.feature.isValid() ? 1 : 0;
        if (!err.contains("match_score")) err["match_score"] = QJsonValue::Null;
        if (!err.contains("match_reason")) err["match_reason"] = rightRef.feature.isValid() ? QStringLiteral("已找到右侧成果候选") : QStringLiteral("未找到右侧成果候选");
        err["sub_rule"] = rule;
        err["layer"] = layer;
        err["severity"] = severity;
        err["description"] = desc;
        err["pair_name"] = filePaths.seriesPairName;
        err["left_sheet"] = leftRef.sheetName.isEmpty() ? filePaths.seriesLeftSheetName : leftRef.sheetName;
        err["right_sheet"] = rightRef.sheetName.isEmpty() ? filePaths.seriesRightSheetName : rightRef.sheetName;
        if (isCrossScale) {
            err["large_scale_sheet"] = err.value(QStringLiteral("left_sheet"));
            err["small_scale_sheet"] = err.value(QStringLiteral("right_sheet"));
        }
        if (err.value(QStringLiteral("issue_id")).toString().trimmed().isEmpty()) {
            QStringList identityParts{
                filePaths.seriesPairName.trimmed(),
                rule.trimmed(),
                layer.trimmed(),
                err.value(QStringLiteral("error_type")).toString().trimmed(),
                SeriesLayerMapping::pathKey(leftRef.shpPath),
                seriesFeatureId(leftRef),
                SeriesLayerMapping::pathKey(rightRef.shpPath),
                seriesFeatureId(rightRef),
                err.value(QStringLiteral("field")).toString().trimmed()
            };
            for (const QString& key : {QStringLiteral("check_point"), QStringLiteral("problem_point"),
                                       QStringLiteral("error_location"), QStringLiteral("geometry_fingerprint"),
                                       QStringLiteral("point_x"), QStringLiteral("point_y"),
                                       QStringLiteral("left_value"), QStringLiteral("right_value"),
                                       QStringLiteral("actual"), QStringLiteral("expected")}) {
                const QJsonValue value = err.value(key);
                if (!value.isUndefined() && !value.isNull()) {
                    QJsonArray serializedValue;
                    serializedValue.append(value);
                    identityParts << key + QLatin1Char('=') + QString::fromUtf8(QJsonDocument(serializedValue).toJson(QJsonDocument::Compact));
                }
            }
            const QByteArray issueDigest = QCryptographicHash::hash(identityParts.join(QChar(0)).toUtf8(), QCryptographicHash::Sha256).toHex();
            err[QStringLiteral("issue_id")] = QString::fromLatin1(issueDigest.left(32));
        }
        if (err.value(QStringLiteral("err_id")).toString().trimmed().isEmpty())
            err[QStringLiteral("err_id")] = err.value(QStringLiteral("issue_id"));
        if (!err.contains("decision_status")) err["decision_status"] = QStringLiteral("confirmed_error");
        const QString decisionStatus = err.value("decision_status").toString();
        if (decisionStatus == QStringLiteral("confirmed_error")) hasError = true;
        if (!err.contains("confidence")) err["confidence"] = QStringLiteral("high");
        result.errors[QString("error%1").arg(idx++)] = err;
        summary[rule] = summary.value(rule).toInt() + 1;
        if (decisionStatus == QStringLiteral("needs_review")) {
            problemSummary["needs_review_count"] = problemSummary.value("needs_review_count").toInt() + 1;
        } else if (decisionStatus == QStringLiteral("not_evaluable")) {
            problemSummary["not_evaluable_count"] = problemSummary.value("not_evaluable_count").toInt() + 1;
        }
        if (decisionStatus != QStringLiteral("confirmed_error")) return;
        problemSummary["confirmed_error_count"] = problemSummary.value("confirmed_error_count").toInt() + 1;
        if (rule == QStringLiteral("属性一致性")) problemSummary["attribute_issue_count"] = problemSummary.value("attribute_issue_count").toInt() + 1;
        else if (rule == QStringLiteral("样式一致性")) { const QString g = err.value("geometry_type").toString(); const QString k = g == QStringLiteral("线要素") ? QStringLiteral("line_style_issue_count") : (g == QStringLiteral("面要素") ? QStringLiteral("polygon_style_issue_count") : QStringLiteral("point_style_issue_count")); problemSummary[k] = problemSummary.value(k).toInt() + 1; }
        else if (rule == QStringLiteral("符号样式规范")) problemSummary["cartographic_symbol_rule_issue_count"] = problemSummary.value("cartographic_symbol_rule_issue_count").toInt() + 1;
        else if (rule == QStringLiteral("注记分级规范")) problemSummary["annotation_grade_rule_issue_count"] = problemSummary.value("annotation_grade_rule_issue_count").toInt() + 1;
        else if (rule == QStringLiteral("跨比例尺属性等级一致性")) problemSummary["cross_scale_attribute_level_consistency_issue_count"] = problemSummary.value("cross_scale_attribute_level_consistency_issue_count").toInt() + 1;
        else if (err.value("error_type").toString() == QStringLiteral("field_missing")) problemSummary["field_config_issue_count"] = problemSummary.value("field_config_issue_count").toInt() + 1;
        else if (err.value("error_type").toString() == QStringLiteral("feature_ambiguous")) { problemSummary["spatial_ambiguous_count"] = problemSummary.value("spatial_ambiguous_count").toInt() + 1; }
        else if (rule == QStringLiteral("字段值缺失")) problemSummary["field_value_missing_count"] = problemSummary.value("field_value_missing_count").toInt() + 1;
        else if (rule == QStringLiteral("字段值异常")) problemSummary["field_value_invalid_count"] = problemSummary.value("field_value_invalid_count").toInt() + 1;
        else problemSummary["geometry_issue_count"] = problemSummary.value("geometry_issue_count").toInt() + 1;
    };
    auto addLayerIssue = [&](const QString& stdName, const QString& errorType, const QString& desc, const SeriesFeatureRef& leftRef = SeriesFeatureRef(), const SeriesFeatureRef& rightRef = SeriesFeatureRef(), const QStringList& details = QStringList()) { QJsonObject ext; ext["error_type"] = errorType; if (!details.isEmpty()) ext["details"] = QJsonArray::fromStringList(details); addErr(QStringLiteral("配置/匹配问题"), stdName, desc, leftRef, rightRef, ext); };
    QgsGeometry leftSheetGeom = QgsGeometry::fromWkt(filePaths.seriesLeftSheetWkt);
    QgsGeometry rightSheetGeom = QgsGeometry::fromWkt(filePaths.seriesRightSheetWkt);
    if (!leftSheetGeom.isGeosValid()) leftSheetGeom = leftSheetGeom.makeValid();
    if (!rightSheetGeom.isGeosValid()) rightSheetGeom = rightSheetGeom.makeValid();
    if (leftSheetGeom.isNull() || leftSheetGeom.isEmpty() || !leftSheetGeom.isGeosValid()) { result.err_exist = true; QJsonObject err; err["description"] = QStringLiteral("左图幅几何无效"); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] invalid left geometry pair=%1").arg(filePaths.seriesPairName)); return; }
    if (rightSheetGeom.isNull() || rightSheetGeom.isEmpty() || !rightSheetGeom.isGeosValid()) { result.err_exist = true; QJsonObject err; err["description"] = QStringLiteral("右图幅几何无效"); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] invalid right geometry pair=%1").arg(filePaths.seriesPairName)); return; }
    QgsGeometry leftZone;
    QgsGeometry rightZone;
    try { leftZone = leftSheetGeom.intersection(edgeZone); rightZone = rightSheetGeom.intersection(edgeZone); }
    catch (...) { result.err_exist = true; QJsonObject err; err["description"] = QStringLiteral("左右接边检查区构建异常"); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] left/right zone exception pair=%1").arg(filePaths.seriesPairName)); return; }
    if (isCrossScale) {
        // Buffered envelopes may contain features belonging exclusively to the
        // surrounding map. Only the actual common coverage is inspected.
        leftZone = suppliedCoverage;
        rightZone = leftZone;
    }
    if (leftZone.isNull() || leftZone.isEmpty()) { result.err_exist = true; QJsonObject err; err["description"] = QStringLiteral("左侧接边检查区为空"); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] left zone empty pair=%1").arg(filePaths.seriesPairName)); return; }
    if (rightZone.isNull() || rightZone.isEmpty()) { result.err_exist = true; QJsonObject err; err["description"] = QStringLiteral("右侧接边检查区为空"); result.errors["input_error"] = err; append501DebugLog(QStringLiteral("[501] right zone empty pair=%1").arg(filePaths.seriesPairName)); return; }
    int checkedLayerCount = 0;
    QStringList inputValidationErrors;
    auto normText = [&](const QVariant& v) { return (!v.isValid() || v.isNull()) ? QString() : v.toString().trimmed(); };
    auto normalizeSeriesColorValue = [&](QString text) {
        text = text.trimmed().toLower();
        text.remove(QRegularExpression(QStringLiteral("\\s+")));
        if (text.isEmpty() || text == QStringLiteral("null") || text == QStringLiteral("<null>")) return QString();
        if (text.startsWith(QStringLiteral("0x"))) text = QStringLiteral("#") + text.mid(2);
        if (text.startsWith(QStringLiteral("#"))) return text;
        const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;，；]+")), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            bool okR = false, okG = false, okB = false;
            const int r = parts.value(0).toInt(&okR);
            const int g = parts.value(1).toInt(&okG);
            const int b = parts.value(2).toInt(&okB);
            if (okR && okG && okB) return QStringLiteral("#%1%2%3")
                .arg(qBound(0, r, 255), 2, 16, QLatin1Char('0'))
                .arg(qBound(0, g, 255), 2, 16, QLatin1Char('0'))
                .arg(qBound(0, b, 255), 2, 16, QLatin1Char('0'));
        }
        return text;
    };
    auto normalizeSeriesCompareValue = [&](const QString& fieldName, const QVariant& value) {
        QString text = normText(value);
        const QString lower = text.trimmed().toLower();
        if (lower.isEmpty() || lower == QStringLiteral("null") || lower == QStringLiteral("<null>") || lower == QStringLiteral("nan")) return QString();
        const QString fld = fieldName.toLower();
        if (fld.contains(QStringLiteral("col")) || fld.contains(QStringLiteral("color")) || fld.contains(QStringLiteral("colour"))) return normalizeSeriesColorValue(text);
        if (lower == QStringLiteral("true") || lower == QStringLiteral("yes") || lower == QStringLiteral("y")) return QStringLiteral("1");
        if (lower == QStringLiteral("false") || lower == QStringLiteral("no") || lower == QStringLiteral("n")) return QStringLiteral("0");
        if (fld.contains(QStringLiteral("wid")) || fld.contains(QStringLiteral("width")) || fld.contains(QStringLiteral("prio")) || fld.contains(QStringLiteral("size"))) {
            bool ok = false;
            const double number = text.toDouble(&ok);
            if (ok) return QString::number(number, 'g', 12);
        }
        return text.trimmed();
    };
    auto sameText = [&](const QVariant& l, const QVariant& r) { const QString ls = normText(l), rs = normText(r); return (ls.isEmpty() && rs.isEmpty()) || ls == rs; };
    auto sameFieldText = [&](const QString& fieldName, const QVariant& l, const QVariant& r) { const QString ls = normalizeSeriesCompareValue(fieldName, l), rs = normalizeSeriesCompareValue(fieldName, r); return (ls.isEmpty() && rs.isEmpty()) || ls == rs; };
    auto crossScaleRetainRequired = [&](const QgsFeature& feature) {
        if (!isCrossScale || crossScaleMinKeepLevel <= 0) return true;
        const int fieldIndex = feature.fields().indexOf(crossScaleKeepLevelField);
        if (fieldIndex < 0) return crossScaleKeepWhenLevelMissing;
        bool ok = false;
        const int level = normText(feature.attribute(fieldIndex)).toInt(&ok);
        if (!ok) return crossScaleKeepWhenLevelMissing;
        return crossScaleLevelSmallerIsHigher ? level <= crossScaleMinKeepLevel : level >= crossScaleMinKeepLevel;
    };
    auto normalizeSeriesLayerBaseName = [&](QString name) { name = QFileInfo(name).completeBaseName().trimmed(); static const QStringList suffixes{ QStringLiteral("_LineString"), QStringLiteral("_MultiLineString"), QStringLiteral("_Point"), QStringLiteral("_MultiPoint"), QStringLiteral("_Polygon"), QStringLiteral("_MultiPolygon") }; for (const QString& suffix : suffixes) if (name.endsWith(suffix, Qt::CaseInsensitive)) { name.chop(suffix.size()); break; } return name.trimmed(); };
    auto areSeriesRefsLayerCompatible = [&](const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef) {
        const QString leftStd = leftRef.standardLayerName.trimmed();
        const QString rightStd = rightRef.standardLayerName.trimmed();
        if (hasConfirmedLayerMapping) {
            const QJsonObject layer = confirmedLayerMapping.value(QStringLiteral("layers")).toObject().value(leftStd).toObject();
            if (leftStd != rightStd || !SeriesLayerMapping::allowsPair(layer, leftRef.shpPath, rightRef.shpPath)) return false;
        }
        if (!leftStd.isEmpty() || !rightStd.isEmpty()) {
            return !leftStd.isEmpty() && !rightStd.isEmpty() && leftStd == rightStd;
        }
        const QString leftLayerBase = seriesLayerCategoryKey(leftRef.shpName, layerNameAliases);
        const QString rightLayerBase = seriesLayerCategoryKey(rightRef.shpName, layerNameAliases);
        return leftLayerBase.isEmpty() || rightLayerBase.isEmpty() || leftLayerBase == rightLayerBase;
    };
    auto crossScaleBusinessClass = [&](const SeriesFeatureRef& ref) {
        const QString shpBase = normalizeSeriesLayerBaseName(ref.shpName);
        const QString annotationClass = seriesLayerAnnotationClass(shpBase);
        if (!annotationClass.isEmpty()) return annotationClass;
        QString text = shpBase;
        text.append(QStringLiteral("|"));
        text.append(ref.standardLayerName);
        text = text.trimmed();
        auto has = [&](const QString& token) { return text.contains(token, Qt::CaseInsensitive); };
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
    };
    auto crossScaleBusinessMismatchReason = [](const QString& leftClass, const QString& rightClass) {
        const QString leftDomain = leftClass.section(QLatin1Char(':'), 0, 0);
        const QString rightDomain = rightClass.section(QLatin1Char(':'), 0, 0);
        if (leftDomain == QStringLiteral("road") && rightDomain == leftDomain) return QStringLiteral("road_rank_mismatch");
        if (leftDomain == QStringLiteral("boundary") && rightDomain == leftDomain) return QStringLiteral("boundary_level_mismatch");
        if (leftDomain == QStringLiteral("hydrology") && rightDomain == leftDomain) return QStringLiteral("hydrology_rank_mismatch");
        return QStringLiteral("business_class_mismatch");
    };
    auto crossScaleBoundaryClassFromFeature = [&](const SeriesFeatureRef& ref) {
        const QString layerHints = normalizeSeriesLayerBaseName(ref.shpName) + QLatin1Char(' ') + ref.standardLayerName;
        if (!layerHints.contains(QStringLiteral("admin"), Qt::CaseInsensitive)
            && !layerHints.contains(QStringLiteral("boundary"), Qt::CaseInsensitive)
            && !layerHints.contains(QStringLiteral("行政"))
            && !layerHints.contains(QStringLiteral("界线"))
            && !layerHints.contains(QStringLiteral("边界"))) return QString();
        QStringList fields{levelField, QStringLiteral("ADMIN_LEVEL"), QStringLiteral("行政级别"),
            QStringLiteral("界线级别"), QStringLiteral("界线等级"), nameField};
        fields.removeDuplicates();
        for (const QString& field : fields) {
            const int index = ref.feature.fields().indexOf(field);
            if (index < 0) continue;
            const QString value = ref.feature.attribute(index).toString().trimmed();
            if (value.isEmpty()) continue;
            if (value.contains(QStringLiteral("乡镇")) || value.contains(QStringLiteral("乡级"))
                || value.contains(QStringLiteral("乡")) || value.contains(QStringLiteral("镇"))
                || value.contains(QStringLiteral("街道"))) return QStringLiteral("boundary:township");
            if (value.contains(QStringLiteral("村级")) || value.endsWith(QStringLiteral("村"))) return QStringLiteral("boundary:village");
            if (value.contains(QStringLiteral("县级")) || value.contains(QStringLiteral("区县"))
                || value.contains(QStringLiteral("县界")) || value.endsWith(QStringLiteral("县"))
                || value.endsWith(QStringLiteral("区")) || value.endsWith(QStringLiteral("旗"))) return QStringLiteral("boundary:county");
            if (value.contains(QStringLiteral("市级")) || value.endsWith(QStringLiteral("市"))) return QStringLiteral("boundary:city");
            if (value.contains(QStringLiteral("省级")) || value.endsWith(QStringLiteral("省"))) return QStringLiteral("boundary:province");
        }
        return QString();
    };
    auto crossScaleResolvedBusinessClass = [&](const SeriesFeatureRef& ref) {
        const QString featureClass = crossScaleBoundaryClassFromFeature(ref);
        return featureClass.isEmpty() ? crossScaleBusinessClass(ref) : featureClass;
    };
    struct LayerSemanticAttribute {
        QString businessClass;
        QString businessDomain;
        QString attributeLevel;
        QString attributeName;
        QString sourceLayer;
        bool valid = false;
    };
    QSet<QString> invalidStableIdentityLayers;
    auto invalidIdentityValue = [&](const QString& field, const QVariant& value) {
        const QString text = value.toString().trimmed().toLower();
        return !text.isEmpty() && (text == QStringLiteral("true") || text == QStringLiteral("false"))
            && field.compare(QStringLiteral("original_c"), Qt::CaseInsensitive) == 0;
    };
    auto crossScaleStableFeatureIdentity = [&](const SeriesFeatureRef& ref) {
        if (invalidIdentityValue(codeField, ref.feature.attribute(codeField))) {
            invalidStableIdentityLayers.insert(ref.shpPath);
            const QString name = normalizeSeriesCompareValue(nameField, ref.feature.attribute(nameField));
            return name.isEmpty() ? QString() : QStringLiteral("name:%1").arg(name);
        }
        const QString code = normalizeSeriesCompareValue(codeField, ref.feature.attribute(codeField));
        if (!code.isEmpty()) return QStringLiteral("code:%1").arg(code);
        const QString name = normalizeSeriesCompareValue(nameField, ref.feature.attribute(nameField));
        if (!name.isEmpty()) return QStringLiteral("name:%1").arg(name);
        return QString();
    };
    auto areCrossScaleSupportRefsCompatible = [&](const SeriesFeatureRef& largeRef, const SeriesFeatureRef& smallRef, QString* rejectReason = nullptr) {
        if (!areSeriesRefsLayerCompatible(largeRef, smallRef)) {
            if (rejectReason) *rejectReason = QStringLiteral("aggregate_layer_mismatch");
            return false;
        }
        const QString largeClass = crossScaleResolvedBusinessClass(largeRef);
        const QString smallClass = crossScaleResolvedBusinessClass(smallRef);
        if (largeClass.isEmpty() || smallClass.isEmpty()) {
            if (rejectReason) *rejectReason = QStringLiteral("unclassified_business_class");
            return false;
        } else if (largeClass != smallClass) {
            if (rejectReason) *rejectReason = crossScaleBusinessMismatchReason(largeClass, smallClass);
            return false;
        }
        const QString largeIdentity = crossScaleStableFeatureIdentity(largeRef);
        const QString smallIdentity = crossScaleStableFeatureIdentity(smallRef);
        if (!largeIdentity.isEmpty() && !smallIdentity.isEmpty() && largeIdentity != smallIdentity) {
            if (rejectReason) *rejectReason = QStringLiteral("feature_identity_mismatch");
            return false;
        }
        if (rejectReason) *rejectReason = QString();
        return true;
    };
    auto buildLayerSemanticAttribute = [&](const SeriesFeatureRef& ref) {
        LayerSemanticAttribute attr;
        attr.businessClass = crossScaleResolvedBusinessClass(ref);
        attr.sourceLayer = normalizeSeriesLayerBaseName(ref.shpName);
        attr.attributeName = attr.sourceLayer;
        if (attr.businessClass.isEmpty()) return attr;
        attr.businessDomain = attr.businessClass.section(QLatin1Char(':'), 0, 0);
        attr.attributeLevel = attr.businessClass.section(QLatin1Char(':'), 1);
        attr.valid = !attr.businessDomain.isEmpty() && !attr.attributeLevel.isEmpty();
        return attr;
    };
    auto layerSemanticAttributeToJson = [&](const LayerSemanticAttribute& attr) {
        QJsonObject obj;
        obj["business_class"] = attr.businessClass;
        obj["business_domain"] = attr.businessDomain;
        obj["attribute_level"] = attr.attributeLevel;
        obj["attribute_name"] = attr.attributeName;
        obj["source_layer"] = attr.sourceLayer;
        obj["attribute_source"] = QStringLiteral("actual_layer_name_and_feature_attributes");
        return obj;
    };
    auto calculateMatchScore = [&](const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, bool& isStrongMatch, QString& matchReason, QString& confidence) {
        isStrongMatch = false;
        matchReason = QString();
        confidence = QStringLiteral("low");
        const QgsFeature& leftF = leftRef.feature;
        const QgsFeature& rightF = rightRef.feature;

        const QString leftName = normText(leftF.attribute(nameField));
        const QString rightName = normText(rightF.attribute(nameField));
        const QString leftCode = normText(leftF.attribute(codeField));
        const QString rightCode = normText(rightF.attribute(codeField));
        const QString leftLevel = normText(leftF.attribute(levelField));
        const QString rightLevel = normText(rightF.attribute(levelField));

        bool nameMatch = !leftName.isEmpty() && leftName == rightName;
        bool codeMatch = !invalidIdentityValue(codeField,leftF.attribute(codeField))
            && !invalidIdentityValue(codeField,rightF.attribute(codeField)) && !leftCode.isEmpty() && leftCode == rightCode;
        bool levelMatch = leftLevel == rightLevel;

        if ((nameMatch || codeMatch) && levelMatch) {
            isStrongMatch = true;
            matchReason = QStringLiteral("强匹配：名称/编码与等级一致");
            confidence = QStringLiteral("high");
            return 100.0;
        }

        double score = 0.0;
        if (nameMatch) score += 40.0;
        if (codeMatch) score += 40.0;
        if (levelMatch) score += 20.0;

        const auto geomType = QgsWkbTypes::geometryType(leftF.geometry().wkbType());
        if (geomType == QgsWkbTypes::LineGeometry) {
            double leftLen = leftF.geometry().length();
            double rightLen = rightF.geometry().length();
            if (leftLen > 0 && rightLen > 0) score += (qMin(leftLen, rightLen) / qMax(leftLen, rightLen)) * 20.0;
        } else if (geomType == QgsWkbTypes::PolygonGeometry) {
            double leftArea = leftF.geometry().area();
            double rightArea = rightF.geometry().area();
            if (leftArea > 0 && rightArea > 0) score += (qMin(leftArea, rightArea) / qMax(leftArea, rightArea)) * 20.0;
        }

        matchReason = QStringLiteral("弱匹配：综合评分 %1").arg(score);
        confidence = score >= 60.0 ? QStringLiteral("medium") : QStringLiteral("low");
        return score;
    };
    auto attachMatchInfo = [&](QJsonObject& ext, const QString& prefix, const SeriesFeatureRef& ref) {
        ext[prefix + QStringLiteral("_fid")] = seriesFeatureId(ref);
        ext[prefix + QStringLiteral("_layer")] = ref.shpName;
    };
    auto attachMatchEvidence = [&](QJsonObject& ext, const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, const QString& status, int candidateCount) {
        ext["match_status"] = status;
        ext["candidate_count"] = candidateCount;
        if (rightRef.feature.isValid()) {
            bool isStrongMatch = false;
            QString matchReason;
            QString confidence;
            const double score = calculateMatchScore(leftRef, rightRef, isStrongMatch, matchReason, confidence);
            ext["match_score"] = score;
            ext["match_reason"] = matchReason;
            ext["confidence"] = confidence;
            ext["match_strength"] = isStrongMatch ? QStringLiteral("strong") : QStringLiteral("weak");
        } else {
            ext["match_score"] = QJsonValue::Null;
            ext["match_reason"] = status == QStringLiteral("ambiguous")
                ? QStringLiteral("右侧候选不唯一")
                : QStringLiteral("未找到满足空间和字段评分阈值的右侧成果候选");
        }
    };
    auto addGeometryErr = [&](const QString& rule, const QString& stdName, const QString& desc, const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, QJsonObject ext) {
        attachMatchInfo(ext, QStringLiteral("left"), leftRef);
        if (rightRef.feature.isValid()) attachMatchInfo(ext, QStringLiteral("right"), rightRef);
        if (!ext.contains("match_status")) attachMatchEvidence(ext, leftRef, rightRef, rightRef.feature.isValid() ? QStringLiteral("matched") : QStringLiteral("unmatched"), rightRef.feature.isValid() ? 1 : 0);
        if (!ext.contains("decision_status")) ext["decision_status"] = QStringLiteral("confirmed_error");
        addErr(rule, stdName, desc, leftRef, rightRef, ext);
    };
    auto simplifyCrossScaleRightGeometry = [&](const QgsGeometry& geometry) {
        if (!isCrossScale || geometry.isNull() || geometry.isEmpty()
            || QgsWkbTypes::geometryType(geometry.wkbType()) != QgsWkbTypes::LineGeometry) return geometry;
        try {
            const QgsGeometry simplified = geometry.simplify(lineTol);
            if (!simplified.isNull() && !simplified.isEmpty()) return simplified;
        } catch (...) {}
        return geometry;
    };
    auto toAttrBool = [&](const QgsFeature& feature, const QString& fieldName) {
        const int fieldIndex = feature.fields().indexOf(fieldName);
        if (fieldIndex < 0) return false;
        const QVariant value = feature.attribute(fieldIndex);
        if (!value.isValid() || value.isNull()) return false;
        const QString text = value.toString().trimmed().toLower();
        return value.toBool()
            || text == QStringLiteral("1")
            || text == QStringLiteral("true")
            || text == QStringLiteral("yes")
            || text == QStringLiteral("y");
    };
    auto styleFieldsForType = [&](QgsWkbTypes::GeometryType geometryType) {
        if (geometryType == QgsWkbTypes::LineGeometry) return lineStyleFields;
        if (geometryType == QgsWkbTypes::PolygonGeometry) return polygonStyleFields;
        if (geometryType == QgsWkbTypes::PointGeometry) return pointStyleFields;
        return QStringList();
    };
    auto addMissingFieldErr = [&](const QString& stdName, const QString& fieldName, const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, bool leftMissing, bool rightMissing) {
        QJsonObject ext;
        ext["error_type"] = QStringLiteral("field_missing");
        ext["decision_status"] = QStringLiteral("not_evaluable");
        ext["field"] = fieldName;
        ext["left_missing"] = leftMissing;
        ext["right_missing"] = rightMissing;
        const QString sideText = leftMissing && rightMissing
            ? QStringLiteral("左右要素")
            : (leftMissing ? QStringLiteral("左侧要素") : QStringLiteral("右侧要素"));
        attachMatchEvidence(ext, leftRef, rightRef, rightRef.feature.isValid() ? QStringLiteral("matched") : QStringLiteral("unmatched"), rightRef.feature.isValid() ? 1 : 0);
        addErr(QStringLiteral("字段缺失"), stdName, QStringLiteral("%1缺少字段%2").arg(sideText, fieldName), leftRef, rightRef, ext);
    };
    auto addFieldErr = [&](const QString& rule, const QString& stdName, const QString& errorType, const QString& fieldName, const QString& desc, const QString& leftValue, const QString& rightValue, const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, bool levelMismatch) {
        QJsonObject ext;
        ext["error_type"] = errorType;
        ext["field"] = fieldName;
        ext["left_value"] = leftValue;
        ext["right_value"] = rightValue;
        ext["level_mismatch"] = levelMismatch;
        if (invalidIdentityValue(fieldName,leftRef.feature.attribute(fieldName))
            || invalidIdentityValue(fieldName,rightRef.feature.attribute(fieldName))) {
            ext["error_type"]=QStringLiteral("series_stable_identity_field_invalid");
            ext["decision_status"]=QStringLiteral("not_evaluable");
        } else if (errorType==QStringLiteral("field_value_missing")) {
            ext["decision_status"]=QStringLiteral("not_evaluable");
        } else if (errorType==QStringLiteral("style_field_mismatch")) {
            const auto leftEvidence=SeriesSourceAudit::layerEvidence(sourceAudits,leftRef.shpPath);
            const auto rightEvidence=SeriesSourceAudit::layerEvidence(sourceAudits,rightRef.shpPath);
            auto sourceState=[&](const QJsonObject& evidence,const QString& value) {
                if(fieldName==QStringLiteral("stroke_col") || fieldName==QStringLiteral("fill_color")) return SeriesSourceAudit::colorStatus(evidence,fieldName,value);
                if(fieldName==QStringLiteral("stroke_wid") && SeriesSourceAudit::widthVerifiedInPoints(evidence,value.toDouble())) return QStringLiteral("verified");
                return QStringLiteral("unverified");
            };
            const QString ls=sourceState(leftEvidence,leftValue),rs=sourceState(rightEvidence,rightValue);
            const bool verified=ls==QStringLiteral("verified") && rs==QStringLiteral("verified");
            const bool componentKnown=!leftEvidence.value(QStringLiteral("composite_stroke_palette")).toBool()
                && !rightEvidence.value(QStringLiteral("composite_stroke_palette")).toBool();
            ext["source_evidence_verified"]=verified;ext["symbol_component_confirmed"]=componentKnown;
            if(!verified || !componentKnown) {
                ext["decision_status"]=QStringLiteral("not_evaluable");
                ext["error_type"]=(ls==QStringLiteral("mismatch") || rs==QStringLiteral("mismatch"))
                    ? QStringLiteral("cartographic_source_style_mismatch")
                    : (!verified?QStringLiteral("cartographic_source_not_verified"):QStringLiteral("cartographic_symbol_component_unresolved"));
            }
            const QString leftRole=featureMapRole(leftRef),rightRole=featureMapRole(rightRef);
            ext["left_map_role"]=leftRole;ext["right_map_role"]=rightRole;
            ext["style_context_verified"]=!isCrossScale && !leftRole.isEmpty() && leftRole==rightRole;
            if(verified && componentKnown && (leftRole.isEmpty() || rightRole.isEmpty() || leftRole!=rightRole)) {
                ext["decision_status"]=QStringLiteral("not_evaluable");
                ext["error_type"]=leftRole.isEmpty() || rightRole.isEmpty()
                    ? QStringLiteral("series_style_role_not_evaluable"):QStringLiteral("series_style_comparison_not_applicable");
                ext["style_comparison_reason"]=QStringLiteral("主区与外区表达角色不同或未知，不直接要求样式字段相等；应分别核查适用图式");
            }
        }
        attachMatchEvidence(ext, leftRef, rightRef, rightRef.feature.isValid() ? QStringLiteral("matched") : QStringLiteral("unmatched"), rightRef.feature.isValid() ? 1 : 0);
        addErr(rule, stdName, desc, leftRef, rightRef, ext);
    };
    auto compareConfiguredFields = [&](const QString& rule, const QgsFeature& leftF, const SeriesFeatureRef& leftRef, const QgsFeature& rightF, const SeriesFeatureRef& rightRef, const QStringList& fields, const QString& stdName) { const bool levelMismatch = leftF.fields().indexOf(levelField) >= 0 && rightF.fields().indexOf(levelField) >= 0 && !sameFieldText(levelField, leftF.attribute(levelField), rightF.attribute(levelField)); for (const QString& fld : fields) { if (fld.isEmpty()) continue; const bool leftMissing = leftF.fields().indexOf(fld) < 0, rightMissing = rightF.fields().indexOf(fld) < 0; if (leftMissing || rightMissing) { addMissingFieldErr(stdName, fld, leftRef, rightRef, leftMissing, rightMissing); continue; } const QString lv = normText(leftF.attribute(fld)), rv = normText(rightF.attribute(fld)); if (sameFieldText(fld, leftF.attribute(fld), rightF.attribute(fld))) continue; QString errorType = rule == QStringLiteral("属性一致性") ? QStringLiteral("attribute_field_mismatch") : QStringLiteral("style_field_mismatch"); QString desc = rule == QStringLiteral("属性一致性") ? QStringLiteral("接边处属性字段%1不一致").arg(fld) : QStringLiteral("接边处样式字段%1不一致").arg(fld); if (fld == nameField) { errorType = QStringLiteral("name_mismatch"); desc = QStringLiteral("接边处名称不一致"); } else if (fld == levelField) { errorType = QStringLiteral("level_mismatch"); desc = QStringLiteral("接边处等级不一致"); } else if (fld == codeField) { errorType = QStringLiteral("code_mismatch"); desc = QStringLiteral("接边处编码不一致"); } addFieldErr(rule, stdName, errorType, fld, desc, lv, rv, leftRef, rightRef, rule == QStringLiteral("样式一致性") && levelMismatch); } };
    auto compareAttributeConsistency = [&](const QgsFeature& leftF, const SeriesFeatureRef& leftRef, const QgsFeature& rightF, const SeriesFeatureRef& rightRef, const QStringList& fields, const QString& stdName) { compareConfiguredFields(QStringLiteral("属性一致性"), leftF, leftRef, rightF, rightRef, fields, stdName); };
    QSet<QString> layerSemanticAttributeKeys;
    auto compareLayerSemanticAttributeConsistency = [&](const SeriesFeatureRef& leftRef, const SeriesFeatureRef& rightRef, const QString& stdName) {
        if (!leftRef.feature.isValid() || !rightRef.feature.isValid()) return;
        const LayerSemanticAttribute leftAttr = buildLayerSemanticAttribute(leftRef);
        const LayerSemanticAttribute rightAttr = buildLayerSemanticAttribute(rightRef);
        if (!leftAttr.valid || !rightAttr.valid) return;
        if (leftAttr.businessClass == rightAttr.businessClass) return;
        const QString key = QStringLiteral("%1|%2|%3|%4").arg(leftRef.shpPath, seriesFeatureId(leftRef), rightRef.shpPath, seriesFeatureId(rightRef));
        if (layerSemanticAttributeKeys.contains(key)) return;
        layerSemanticAttributeKeys.insert(key);
        QJsonObject ext;
        ext["error_type"] = QStringLiteral("layer_semantic_attribute_mismatch");
        ext["check_method"] = QStringLiteral("actual_layer_name_semantic_attribute");
        ext["field"] = QStringLiteral("actual_layer_name");
        ext["attribute_source"] = QStringLiteral("actual_layer_name");
        ext["left_business_class"] = leftAttr.businessClass;
        ext["right_business_class"] = rightAttr.businessClass;
        ext["left_business_domain"] = leftAttr.businessDomain;
        ext["right_business_domain"] = rightAttr.businessDomain;
        ext["left_attribute_level"] = leftAttr.attributeLevel;
        ext["right_attribute_level"] = rightAttr.attributeLevel;
        ext["left_attribute_name"] = leftAttr.attributeName;
        ext["right_attribute_name"] = rightAttr.attributeName;
        ext["left_semantic_attribute"] = layerSemanticAttributeToJson(leftAttr);
        ext["right_semantic_attribute"] = layerSemanticAttributeToJson(rightAttr);
        attachMatchEvidence(ext, leftRef, rightRef, QStringLiteral("matched"), 1);
        addErr(QStringLiteral("属性一致性"), stdName, QStringLiteral("接边处地物实际图层名反映的业务类别/属性等级不一致：%1 vs %2").arg(leftAttr.attributeLevel, rightAttr.attributeLevel), leftRef, rightRef, ext);
    };
    auto compareStyleConsistency = [&](const QgsFeature& leftF, const SeriesFeatureRef& leftRef, const QgsFeature& rightF, const SeriesFeatureRef& rightRef, const QString& stdName) { const auto gt = QgsWkbTypes::geometryType(leftF.geometry().wkbType()); const QStringList fields = styleFieldsForType(gt); const bool levelMismatch = leftF.fields().indexOf(levelField) >= 0 && rightF.fields().indexOf(levelField) >= 0 && !sameFieldText(levelField, leftF.attribute(levelField), rightF.attribute(levelField)); for (const QString& fld : fields) { if (fld.isEmpty()) continue; if ((fld == QStringLiteral("stroke_col") || fld == QStringLiteral("stroke_wid")) && !(toAttrBool(leftF, QStringLiteral("stroked")) && toAttrBool(rightF, QStringLiteral("stroked")))) continue; if (fld == QStringLiteral("fill_color") && !(toAttrBool(leftF, QStringLiteral("filled")) && toAttrBool(rightF, QStringLiteral("filled")))) continue; const bool leftMissing = leftF.fields().indexOf(fld) < 0, rightMissing = rightF.fields().indexOf(fld) < 0; if (leftMissing || rightMissing) { addMissingFieldErr(stdName, fld, leftRef, rightRef, leftMissing, rightMissing); continue; } const QString lv = normText(leftF.attribute(fld)), rv = normText(rightF.attribute(fld)); const QString nlv = normalizeSeriesCompareValue(fld, leftF.attribute(fld)), nrv = normalizeSeriesCompareValue(fld, rightF.attribute(fld)); if ((nlv.isEmpty() || nrv.isEmpty()) && (fld == QStringLiteral("style_prio") || fld == QStringLiteral("fill_color") || fld == QStringLiteral("stroke_col") || fld == QStringLiteral("stroke_wid") || fld == QStringLiteral("stroked") || fld == QStringLiteral("filled") || fld == QStringLiteral("closed"))) addFieldErr(QStringLiteral("字段值缺失"), stdName, QStringLiteral("field_value_missing"), fld, (fld == QStringLiteral("stroke_col") && (toAttrBool(leftF, QStringLiteral("stroked")) || toAttrBool(rightF, QStringLiteral("stroked")) || toAttrBool(leftF, QStringLiteral("filled")) || toAttrBool(rightF, QStringLiteral("filled")))) ? QStringLiteral("stroked/filled=true 但 stroke_col 为空") : ((fld == QStringLiteral("stroke_wid") && (toAttrBool(leftF, QStringLiteral("stroked")) || toAttrBool(rightF, QStringLiteral("stroked")))) ? QStringLiteral("stroked=true 但 stroke_wid 为空") : QStringLiteral("字段%1值为空").arg(fld)), lv, rv, leftRef, rightRef, levelMismatch); if (sameFieldText(fld, leftF.attribute(fld), rightF.attribute(fld))) continue; addFieldErr(QStringLiteral("样式一致性"), stdName, QStringLiteral("style_field_mismatch"), fld, QStringLiteral("接边处%1样式字段%2不一致").arg(geomName(leftRef.feature), fld), lv, rv, leftRef, rightRef, levelMismatch); } };
    struct CartographicSymbolSpec {
        QString ruleKey;
        QString label;
        QString expectedColor;
        QString colorField;
        QString actualWidthUnit;
        QString paintRole;
        QString applicability;
        QString areaRole;
        double widthMm;
        double minWidthMm;
        double maxWidthMm;
        CartographicSymbolSpec() : widthMm(-1.0), minWidthMm(-1.0), maxWidthMm(-1.0) {}
        CartographicSymbolSpec(const QString& l, const QString& c, double w) : label(l), expectedColor(c), widthMm(w), minWidthMm(-1.0), maxWidthMm(-1.0) {}
        CartographicSymbolSpec(const QString& l, const QString& c, double minW, double maxW) : label(l), expectedColor(c), widthMm(-1.0), minWidthMm(minW), maxWidthMm(maxW) {}
    };
    auto parseCartographicSymbolRules = [&](const QString& text) {
        QMap<QString, CartographicSymbolSpec> specs;
        const QStringList rows = text.split(QRegularExpression(QStringLiteral("[\\r\\n;；]+")), Qt::SkipEmptyParts);
        for (QString row : rows) {
            row = row.trimmed();
            if (row.isEmpty() || row.startsWith(QStringLiteral("#")) || row.startsWith(QStringLiteral("//"))) continue;
            const QStringList parts = row.split(QRegularExpression(QStringLiteral("[|｜]")), Qt::KeepEmptyParts);
            if (parts.size() < 3) continue;
            const QString businessClass = parts.value(0).trimmed();
            const QString label = parts.value(1).trimmed();
            const QString color = parts.value(2).trimmed();
            QString colorField = parts.value(4).trimmed();
            QString actualWidthUnit = parts.value(5).trimmed().toLower();
            if (actualWidthUnit == QStringLiteral("pt_exported_stroke_wid")) actualWidthUnit = QStringLiteral("pt");
            if (colorField == QStringLiteral("fill")) colorField = QStringLiteral("fill_color");
            else if (colorField == QStringLiteral("stroke")) colorField = QStringLiteral("stroke_col");
            QString widthText = parts.value(3).trimmed();
            widthText.remove(QStringLiteral("mm"), Qt::CaseInsensitive);
            widthText.remove(QStringLiteral("毫米"));
            widthText.replace(QStringLiteral("－"), QStringLiteral("-"));
            widthText.replace(QStringLiteral("—"), QStringLiteral("-"));
            widthText.replace(QStringLiteral("~"), QStringLiteral("-"));
            widthText.replace(QStringLiteral("至"), QStringLiteral("-"));
            widthText.replace(QStringLiteral("×"), QStringLiteral("x"));
            if (widthText.contains(QStringLiteral("x"), Qt::CaseInsensitive)) {
                widthText = widthText.section(QRegularExpression(QStringLiteral("[xX]")), 0, 0).trimmed();
            }
            if (businessClass.isEmpty() || label.isEmpty() || color.isEmpty()) continue;
            if (widthText.isEmpty()) {
                CartographicSymbolSpec spec;
                spec.ruleKey = businessClass;
                spec.label = label;
                spec.expectedColor = color;
                spec.colorField = colorField;
                spec.actualWidthUnit = actualWidthUnit;
                specs.insert(businessClass, spec);
            } else if (widthText.contains(QStringLiteral("-"))) {
                const QStringList range = widthText.split(QStringLiteral("-"), Qt::SkipEmptyParts);
                bool okMin = false, okMax = false;
                const double minW = range.value(0).trimmed().toDouble(&okMin);
                const double maxW = range.value(1).trimmed().toDouble(&okMax);
                if (okMin && okMax) {
                    CartographicSymbolSpec spec(label, color, qMin(minW, maxW), qMax(minW, maxW));
                    spec.ruleKey = businessClass;
                    spec.colorField = colorField;
                    spec.actualWidthUnit = actualWidthUnit;
                    specs.insert(businessClass, spec);
                }
            } else {
                bool ok = false;
                const double width = widthText.toDouble(&ok);
                if (ok) {
                    CartographicSymbolSpec spec(label, color, width);
                    spec.ruleKey = businessClass;
                    spec.colorField = colorField;
                    spec.actualWidthUnit = actualWidthUnit;
                    specs.insert(businessClass, spec);
                }
            }
        }
        // Extra columns explicitly declare the symbol component and applicability.
        for (const QString& row : rows) {
            const QStringList parts = row.split(QRegularExpression(QStringLiteral("[|｜]")), Qt::KeepEmptyParts);
            auto it = specs.find(parts.value(0).trimmed());
            if (it != specs.end()) {
                it->paintRole = parts.value(6).trimmed().toLower();
                it->applicability = parts.value(7).trimmed().toLower();
                QString role=parts.value(8).trimmed().toLower();
                const QString keyRole=it->ruleKey.startsWith(QStringLiteral("role:"))?it->ruleKey.section(':',1,1):QString();
                if(!keyRole.isEmpty()) {
                    if(!role.isEmpty() && role!=QStringLiteral("any") && SeriesCartographicPolicy::normalizedRole(role)!=keyRole) role=QStringLiteral("invalid");
                    else role=keyRole;
                }
                it->areaRole=(role.isEmpty() || role==QStringLiteral("any"))?QString():
                    (SeriesCartographicPolicy::normalizedRole(role).isEmpty()?QStringLiteral("invalid"):SeriesCartographicPolicy::normalizedRole(role));
            }
        }
        return specs;
    };
    QMap<QString, CartographicSymbolSpec> cartographicSymbolSpecs = parseCartographicSymbolRules(cartographicSymbolRuleText);
    auto cartographicSymbolKeyCandidates = [&](const SeriesFeatureRef& ref, int scaleDenom) {
        QStringList keys;
        const auto gt = ref.feature.isValid() && ref.feature.hasGeometry()
            ? QgsWkbTypes::geometryType(ref.feature.geometry().wkbType())
            : QgsWkbTypes::UnknownGeometry;
        const QString businessClass = crossScaleBusinessClass(ref);
        QString geometryPrefix;
        if (gt == QgsWkbTypes::PointGeometry) geometryPrefix = QStringLiteral("point");
        else if (gt == QgsWkbTypes::LineGeometry) geometryPrefix = QStringLiteral("line");
        else if (gt == QgsWkbTypes::PolygonGeometry) geometryPrefix = QStringLiteral("polygon");
        QStringList names;
        const QString shpBase = normalizeSeriesLayerBaseName(ref.shpName);
        const QString stdLayer = ref.standardLayerName.trimmed();
        const QString aliasLayer = seriesLayerCategoryKey(ref.shpName, layerNameAliases);
        if (!shpBase.isEmpty()) names << shpBase;
        if (!stdLayer.isEmpty()) names << stdLayer;
        if (!aliasLayer.isEmpty()) names << aliasLayer;
        names.removeDuplicates();
        if (!geometryPrefix.isEmpty()) {
            for (const QString& name : names) {
                keys << QStringLiteral("%1:%2").arg(geometryPrefix, name);
            }
        }
        if (businessClass.startsWith(QStringLiteral("hydrology_area:")) && geometryPrefix == QStringLiteral("polygon")) {
            keys << QStringLiteral("polygon:水域");
            keys << QStringLiteral("hydrology_area:水域");
        }
        keys << names;
        if (!businessClass.isEmpty()) keys << businessClass;
        keys.removeDuplicates();
        QStringList scopedKeys,roleKeys;
        const QString role=featureMapRole(ref);
        for (const QString& key : keys) {
            if(!role.isEmpty()) {
                if(scaleDenom>0) roleKeys<<QStringLiteral("role:%1:scale:%2:%3").arg(role).arg(scaleDenom).arg(key);
                roleKeys<<QStringLiteral("role:%1:%2").arg(role,key);
            }
            if (scaleDenom > 0) scopedKeys << QStringLiteral("scale:%1:%2").arg(scaleDenom).arg(key);
            scopedKeys << key;
        }
        if(!roleKeys.isEmpty()) scopedKeys=roleKeys+scopedKeys;
        if(role.isEmpty()) for(const QString& key:keys) for(const QString& expectedRole:{QStringLiteral("main"),QStringLiteral("external")}) {
            if(scaleDenom>0) scopedKeys<<QStringLiteral("role:%1:scale:%2:%3").arg(expectedRole).arg(scaleDenom).arg(key);
            scopedKeys<<QStringLiteral("role:%1:%2").arg(expectedRole,key);
        }
        scopedKeys.removeDuplicates();
        return scopedKeys;
    };
    auto cartographicSymbolSpecForRef = [&](const SeriesFeatureRef& ref, int scaleDenom) {
        CartographicSymbolSpec diagnosticFallback;
        for (const QString& key : cartographicSymbolKeyCandidates(ref, scaleDenom)) {
            if (!cartographicSymbolSpecs.contains(key)) continue;
            const auto spec=cartographicSymbolSpecs.value(key);
            if(diagnosticFallback.label.isEmpty()) diagnosticFallback=spec;
            bool stateMatches=true;
            const QString actualLayer=normalizeSeriesLayerBaseName(ref.shpName);
            for(const QString& state:{QStringLiteral("在建"),QStringLiteral("规划"),QStringLiteral("拟建"),QStringLiteral("废弃"),QStringLiteral("停用")})
                if(actualLayer.contains(state) && !spec.ruleKey.contains(state)) stateMatches=false;
            const bool roleMatches=spec.areaRole.isEmpty() || spec.areaRole==featureMapRole(ref);
            if(stateMatches && roleMatches && SeriesCartographicPolicy::applicable(spec.ruleKey,spec.applicability,scaleDenom,projectStyleId)) return spec;
        }
        return diagnosticFallback;
    };
    auto parseCmykComponents = [&](QString text, double& c, double& m, double& y, double& k) {
        c = 0.0; m = 0.0; y = 0.0; k = 0.0;
        QString upper = text.trimmed().toUpper();
        upper.remove(QRegularExpression(QStringLiteral("\\s+")));
        if (upper.isEmpty()) return false;
        if (upper.contains(QStringLiteral("CMYK"))) {
            QList<double> numbers;
            QRegularExpression numRe(QStringLiteral("[-+]?\\d+(?:\\.\\d+)?"));
            QRegularExpressionMatchIterator it = numRe.globalMatch(upper);
            while (it.hasNext()) {
                bool ok = false;
                const double value = it.next().captured(0).toDouble(&ok);
                if (ok) numbers << value;
            }
            if (numbers.size() >= 4) {
                c = numbers.value(0);
                m = numbers.value(1);
                y = numbers.value(2);
                k = numbers.value(3);
                return true;
            }
        }
        bool found = false;
        QRegularExpression componentRe(QStringLiteral("([CMYK])[:：]?([0-9]+(?:\\.[0-9]+)?)"));
        QRegularExpressionMatchIterator it = componentRe.globalMatch(upper);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            bool ok = false;
            const double value = match.captured(2).toDouble(&ok);
            if (!ok) continue;
            const QString channel = match.captured(1);
            if (channel == QStringLiteral("C")) c = value;
            else if (channel == QStringLiteral("M")) m = value;
            else if (channel == QStringLiteral("Y")) y = value;
            else if (channel == QStringLiteral("K")) k = value;
            found = true;
        }
        return found;
    };
    auto normalizeCartographicCmykText = [&](const QString& text) {
        double c = 0.0, m = 0.0, y = 0.0, k = 0.0;
        if (!parseCmykComponents(text, c, m, y, k)) return normalizeSeriesColorValue(text);
        return QStringLiteral("cmyk(%1,%2,%3,%4)")
            .arg(QString::number(c, 'g', 6),
                 QString::number(m, 'g', 6),
                 QString::number(y, 'g', 6),
                 QString::number(k, 'g', 6));
    };
    auto isCartographicColorValid = [&](const QString& text) {
        if (text.trimmed().isEmpty()) return false;
        double c = 0.0, m = 0.0, y = 0.0, k = 0.0;
        if (parseCmykComponents(text, c, m, y, k))
            return c >= 0.0 && c <= 100.0 && m >= 0.0 && m <= 100.0
                && y >= 0.0 && y <= 100.0 && k >= 0.0 && k <= 100.0;
        return QColor(text.trimmed()).isValid();
    };
    auto sameCartographicColor = [&](const QString& actualText, const QString& expectedText) {
        double ac = 0.0, am = 0.0, ay = 0.0, ak = 0.0;
        double ec = 0.0, em = 0.0, ey = 0.0, ek = 0.0;
        if (!parseCmykComponents(actualText, ac, am, ay, ak) || !parseCmykComponents(expectedText, ec, em, ey, ek)) {
            return normalizeSeriesColorValue(actualText) == normalizeSeriesColorValue(expectedText);
        }
        const double tol = 1.0;
        return qAbs(ac - ec) <= tol && qAbs(am - em) <= tol && qAbs(ay - ey) <= tol && qAbs(ak - ek) <= tol;
    };
    auto symbolSizeOrWidthUnitCandidates = [&](const CartographicSymbolSpec& spec) {
        QJsonArray arr;
        const double ptPerMm = 2.83464566929134;
        if (spec.widthMm >= 0.0) {
            if (spec.actualWidthUnit == QStringLiteral("mm")) {
                QJsonObject mmObj; mmObj["unit"] = QStringLiteral("mm"); mmObj["value"] = spec.widthMm; arr.append(mmObj);
            }
            if (spec.actualWidthUnit == QStringLiteral("pt")) {
                QJsonObject ptObj; ptObj["unit"] = QStringLiteral("pt_exported_stroke_wid"); ptObj["value"] = spec.widthMm * ptPerMm; arr.append(ptObj);
            }
        } else if (spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm) {
            if (spec.actualWidthUnit == QStringLiteral("mm")) {
                QJsonObject mmObj; mmObj["unit"] = QStringLiteral("mm"); mmObj["min"] = spec.minWidthMm; mmObj["max"] = spec.maxWidthMm; arr.append(mmObj);
            }
            if (spec.actualWidthUnit == QStringLiteral("pt")) {
                QJsonObject ptObj; ptObj["unit"] = QStringLiteral("pt_exported_stroke_wid"); ptObj["min"] = spec.minWidthMm * ptPerMm; ptObj["max"] = spec.maxWidthMm * ptPerMm; arr.append(ptObj);
            }
        }
        return arr;
    };
    auto cartographicWidthMatches = [&](double actualWidth, const CartographicSymbolSpec& spec) {
        const double ptPerMm = 2.83464566929134;
        if (spec.actualWidthUnit != QStringLiteral("mm")
            && spec.actualWidthUnit != QStringLiteral("pt")) return false;
        auto matchWidth = [&](double expectedMm, double toleranceMm, double tolerancePt) {
            if (spec.actualWidthUnit == QStringLiteral("mm")) return qAbs(actualWidth - expectedMm) <= toleranceMm;
            if (spec.actualWidthUnit == QStringLiteral("pt")) return qAbs(actualWidth - expectedMm * ptPerMm) <= tolerancePt;
            return false;
        };
        if (spec.widthMm >= 0.0) {
            return matchWidth(spec.widthMm, 0.03, 0.08);
        }
        if (spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm) {
            if (spec.actualWidthUnit == QStringLiteral("mm")) {
                return actualWidth >= spec.minWidthMm - 0.03 && actualWidth <= spec.maxWidthMm + 0.03;
            }
            if (spec.actualWidthUnit == QStringLiteral("pt")) {
                return actualWidth >= spec.minWidthMm * ptPerMm - 0.08 && actualWidth <= spec.maxWidthMm * ptPerMm + 0.08;
            }
            return false;
        }
        return true;
    };
    QSet<QString> cartographicSymbolRuleKeys;
    int cartographicSymbolRuleUncoveredCount = 0;
    QMap<QString, int> cartographicSymbolRuleUncoveredByLayer;
    auto writeCartographicRulePoint = [&](QJsonObject& ext, const SeriesFeatureRef& ref) {
        if (!ref.feature.isValid() || !ref.feature.hasGeometry()) return;
        QgsGeometry pointGeom;
        try {
            pointGeom = ref.feature.geometry().centroid();
        } catch (...) {
        }
        if (pointGeom.isNull() || pointGeom.isEmpty() || QgsWkbTypes::geometryType(pointGeom.wkbType()) != QgsWkbTypes::PointGeometry) return;
        const QgsPointXY pt = pointGeom.asPoint();
        ext["point_x"] = pt.x();
        ext["point_y"] = pt.y();
        ext["point_source"] = QStringLiteral("cartographic_symbol_rule_point");
        ext["check_point"] = pointText(pt);
        ext["point_to_common_edge_dist"] = (!commonEdge.isNull() && !commonEdge.isEmpty()) ? QJsonValue(QgsGeometry::fromPointXY(pt).distance(commonEdge)) : QJsonValue::Null;
    };
    auto addCartographicSymbolRuleErr = [&](const SeriesFeatureRef& ref, const QString& side, const CartographicSymbolSpec& spec, const QString& fieldName, const QString& actualText, const QString& desc) {
        const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(side, ref.standardLayerName, ref.shpPath, seriesFeatureId(ref), fieldName);
        if (cartographicSymbolRuleKeys.contains(key)) return;
        cartographicSymbolRuleKeys.insert(key);
        QJsonObject ext;
        const bool actualValueMissing = actualText.trimmed().isEmpty();
        ext["error_type"] = actualValueMissing
            ? QStringLiteral("cartographic_symbol_rule_value_missing")
            : QStringLiteral("cartographic_symbol_rule_mismatch");
        if (actualValueMissing) ext["decision_status"] = QStringLiteral("not_evaluable");
        ext["check_method"] = QStringLiteral("cartographic_symbol_rule");
        ext["source_evidence_verified"] = !actualValueMissing;
        ext["rule_applicability_confirmed"] = !actualValueMissing;
        ext["symbol_component_confirmed"] = !actualValueMissing;
        ext["project_style_id"]=projectStyleId;
        ext["symbol_rule_applicability"]=spec.applicability;
        ext["map_role"]=featureMapRole(ref);
        ext["expected_map_role"]=spec.areaRole;
        const auto evidence = SeriesSourceAudit::layerEvidence(sourceAudits, ref.shpPath);
        ext["source_pdf"] = evidence.value(QStringLiteral("pdf_path"));
        ext["source_pdf_sha256"] = evidence.value(QStringLiteral("pdf_sha256"));
        ext["invalid_side"] = side;
        ext["field"] = fieldName;
        ext["symbol_rule_source"] = cartographicSymbolRuleSourceText;
        ext["symbol_rule_feature"] = spec.label;
        ext["business_class"] = spec.ruleKey;
        ext["expected_color"] = spec.expectedColor;
        ext["expected_color_normalized"] = normalizeCartographicCmykText(spec.expectedColor);
        if (spec.widthMm >= 0.0) ext["expected_width_mm"] = spec.widthMm;
        else if (spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm) {
            ext["expected_width_mm"] = QStringLiteral("%1-%2").arg(spec.minWidthMm).arg(spec.maxWidthMm);
        } else {
            ext["expected_width_mm"] = QJsonValue::Null;
        }
        ext["symbol_size_or_width_unit_candidates"] = symbolSizeOrWidthUnitCandidates(spec);
        ext["actual_value"] = actualText;
        ext["match_status"] = QStringLiteral("style_check");
        ext["style_check_detail"] = QStringLiteral("candidate_style_validation");
        ext["candidate_count"] = 0;
        ext["match_score"] = QJsonValue::Null;
        ext["match_reason"] = QStringLiteral("按挂图参数图式规则核查单个候选要素符号，不做跨比例尺字段相等比较");
        ext["confidence"] = QStringLiteral("high");
        writeCartographicRulePoint(ext, ref);
        const QString layerText = ref.standardLayerName.isEmpty() ? spec.label : ref.standardLayerName;
        if (side == QStringLiteral("左")) {
            ext["left_value"] = actualText;
            ext["right_value"] = QJsonValue::Null;
            addErr(QStringLiteral("符号样式规范"), layerText, desc, ref, SeriesFeatureRef(), ext);
        } else {
            ext["left_value"] = QJsonValue::Null;
            ext["right_value"] = actualText;
            addErr(QStringLiteral("符号样式规范"), layerText, desc, SeriesFeatureRef(), ref, ext);
        }
    };
    auto addCartographicSymbolRuleNotEvaluable = [&](const SeriesFeatureRef& ref, const QString& side, const CartographicSymbolSpec& spec,
                                                      const QString& fieldName, const QString& actualText, const QString& errorType, const QString& desc) {
        const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(side, ref.standardLayerName, ref.shpPath, seriesFeatureId(ref), fieldName);
        if (cartographicSymbolRuleKeys.contains(key)) return;
        cartographicSymbolRuleKeys.insert(key);
        QJsonObject ext;
        ext["error_type"] = errorType;
        ext["decision_status"] = errorType == QStringLiteral("cartographic_symbol_rule_applicability_unverified")
            || errorType == QStringLiteral("cartographic_symbol_rule_state_mismatch") ? QStringLiteral("needs_review") : QStringLiteral("not_evaluable");
        ext["check_method"] = QStringLiteral("cartographic_symbol_rule");
        ext["field"] = fieldName;
        ext["symbol_rule_source"] = cartographicSymbolRuleSourceText;
        ext["symbol_rule_feature"] = spec.label;
        ext["business_class"] = spec.ruleKey;
        ext["expected_color"] = spec.expectedColor;
        ext["expected_width_mm"] = spec.widthMm >= 0.0 ? QJsonValue(spec.widthMm)
            : ((spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm)
                ? QJsonValue(QStringLiteral("%1-%2").arg(spec.minWidthMm).arg(spec.maxWidthMm)) : QJsonValue::Null);
        ext["actual_value"] = actualText;
        ext["actual_width_unit"] = spec.actualWidthUnit.isEmpty() ? QStringLiteral("not_configured") : spec.actualWidthUnit;
        ext["project_style_id"]=projectStyleId;
        ext["symbol_rule_applicability"]=spec.applicability;
        ext["map_role"]=featureMapRole(ref);
        ext["expected_map_role"]=spec.areaRole;
        ext["match_status"] = QStringLiteral("not_evaluable");
        ext["match_score"] = QJsonValue::Null;
        ext["confidence"] = QStringLiteral("none");
        ext["description"] = desc;
        writeCartographicRulePoint(ext, ref);
        const QString layerText = ref.standardLayerName.isEmpty() ? spec.label : ref.standardLayerName;
        if (side == QStringLiteral("左")) addErr(QStringLiteral("制图符号规范"), layerText, desc, ref, SeriesFeatureRef(), ext);
        else addErr(QStringLiteral("制图符号规范"), layerText, desc, SeriesFeatureRef(), ref, ext);
    };
    auto validateCartographicSymbolRulesForCollectedRefs = [&](const QList<SeriesFeatureRef>& refs, const QString& side) {
        QSet<QString> invalidFieldReported;
        QSet<QString> invalidComparisonReported;
        for (const SeriesFeatureRef& ref : refs) {
            if (!ref.feature.isValid() || !ref.feature.hasGeometry()) continue;
            if(isCrossScale && !invalidCrossScaleAttributeFields.isEmpty() && !invalidComparisonReported.contains(ref.shpPath)) {
                invalidComparisonReported.insert(ref.shpPath);
                QJsonObject ext{{QStringLiteral("error_type"),QStringLiteral("series_cross_scale_attribute_field_invalid")},
                    {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},
                    {QStringLiteral("invalid_fields"),QJsonArray::fromStringList(invalidCrossScaleAttributeFields)}};
                const QString description=QStringLiteral("配置的跨比例尺稳定属性包含绘制或表达字段，已排除直接相等比较：%1").arg(invalidCrossScaleAttributeFields.join(QStringLiteral("、")));
                if(side==QStringLiteral("左")) addErr(QStringLiteral("属性一致性"),ref.standardLayerName,description,ref,SeriesFeatureRef(),ext);
                else addErr(QStringLiteral("属性一致性"),ref.standardLayerName,description,SeriesFeatureRef(),ref,ext);
            }
            if ((invalidIdentityValue(codeField, ref.feature.attribute(codeField))
                || (configuredCrossScaleAttributeFields.contains(QStringLiteral("original_c"))
                    && invalidIdentityValue(QStringLiteral("original_c"), ref.feature.attribute(QStringLiteral("original_c")))))
                && !invalidFieldReported.contains(ref.shpPath)) {
                invalidFieldReported.insert(ref.shpPath);
                invalidStableIdentityLayers.insert(ref.shpPath);
                QJsonObject ext{{QStringLiteral("error_type"),QStringLiteral("series_stable_identity_field_invalid")},
                    {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},
                    {QStringLiteral("field"),QStringLiteral("original_c")}};
                const QString description=QStringLiteral("original_c 的 true/false 是描画路径闭合状态，不能用作业务编码或跨比例尺稳定属性");
                if(side==QStringLiteral("左")) addErr(QStringLiteral("属性一致性"),ref.standardLayerName,description,ref,SeriesFeatureRef(),ext);
                else addErr(QStringLiteral("属性一致性"),ref.standardLayerName,description,SeriesFeatureRef(),ref,ext);
            }
            const auto gt = QgsWkbTypes::geometryType(ref.feature.geometry().wkbType());
            if (gt != QgsWkbTypes::LineGeometry && gt != QgsWkbTypes::PointGeometry && gt != QgsWkbTypes::PolygonGeometry) continue;
            const int scaleDenom = side == QStringLiteral("左") ? leftScaleDenom : rightScaleDenom;
            CartographicSymbolSpec spec = cartographicSymbolSpecForRef(ref, scaleDenom);
            if (spec.label.isEmpty()) {
                ++cartographicSymbolRuleUncoveredCount;
                const QString layerKey = ref.standardLayerName.isEmpty() ? normalizeSeriesLayerBaseName(ref.shpName) : ref.standardLayerName;
                cartographicSymbolRuleUncoveredByLayer[layerKey] = cartographicSymbolRuleUncoveredByLayer.value(layerKey) + 1;
                continue;
            }
            if(!spec.areaRole.isEmpty() && spec.areaRole!=featureMapRole(ref)) {
                addCartographicSymbolRuleNotEvaluable(ref,side,spec,QStringLiteral("map_role"),featureMapRole(ref),
                    QStringLiteral("cartographic_symbol_role_not_evaluable"),QStringLiteral("图式规则要求明确的主区／外区角色，当前要素角色未知或不适用，未判定样式错误"));
                continue;
            }
            const QString defaultColorField = gt == QgsWkbTypes::PolygonGeometry ? QStringLiteral("fill_color") : QStringLiteral("stroke_col");
            const QString colorField = spec.colorField.isEmpty() ? defaultColorField : spec.colorField;
            const QStringList expectedColors=spec.expectedColor.split('/',Qt::SkipEmptyParts);
            bool expectedColorsValid=!expectedColors.isEmpty();
            for(const QString& color:expectedColors) expectedColorsValid=expectedColorsValid && isCartographicColorValid(color);
            auto matchesExpectedColor=[&](const QString& actual) {
                for(const QString& color:expectedColors) if(sameCartographicColor(actual,color)) return true;
                return false;
            };
            const bool colorComponentDrawn = colorField == QStringLiteral("fill_color")
                ? toAttrBool(ref.feature, QStringLiteral("filled"))
                : toAttrBool(ref.feature, QStringLiteral("stroked"));
            const auto sourceEvidence = SeriesSourceAudit::layerEvidence(sourceAudits, ref.shpPath);
            if (spec.actualWidthUnit.isEmpty() && SeriesSourceAudit::widthVerifiedInPoints(sourceEvidence,
                ref.feature.attribute(QStringLiteral("stroke_wid")).toDouble())) spec.actualWidthUnit=QStringLiteral("pt");
            auto evidenceBlock = [&](const QString& field, const QString& value, bool width) {
                const QString sourceState = width
                    ? (spec.actualWidthUnit == QStringLiteral("pt") && SeriesSourceAudit::widthVerifiedInPoints(sourceEvidence, value.toDouble())
                        ? QStringLiteral("verified") : QStringLiteral("unverified"))
                    : SeriesSourceAudit::colorStatus(sourceEvidence, field, value);
                QString type, description;
                if (sourceState == QStringLiteral("mismatch")) {
                    type = QStringLiteral("cartographic_source_style_mismatch");
                    description = QStringLiteral("SHP 描画属性与原 GeoPDF 同图层调色板不一致，应检查输入来源，未判定原图错误");
                } else if (sourceState != QStringLiteral("verified")) {
                    type = QStringLiteral("cartographic_source_not_verified");
                    description = QStringLiteral("未取得原 GeoPDF 对此描画属性的核验证据，不能确认原图错误");
                } else {
                    const QString actualLayer = normalizeSeriesLayerBaseName(ref.shpName);
                    for (const QString& state : {QStringLiteral("在建"), QStringLiteral("规划"), QStringLiteral("拟建"), QStringLiteral("废弃"), QStringLiteral("停用")}) {
                        if (actualLayer.contains(state) && !spec.ruleKey.contains(state)) {
                            type = QStringLiteral("cartographic_symbol_rule_state_mismatch");
                            description = QStringLiteral("实际图层的建设/使用状态与规则不一致，需配置对应状态规则");
                            break;
                        }
                    }
                    if (type.isEmpty() && !SeriesCartographicPolicy::applicable(spec.ruleKey,spec.applicability,scaleDenom,projectStyleId)) {
                        type = QStringLiteral("cartographic_symbol_rule_applicability_unverified");
                        description = QStringLiteral("发现规则差异，但规则未绑定当前比例尺或已声明的项目图式，需确认适用性");
                    }
                    if (type.isEmpty() && (spec.colorField.isEmpty()
                        || ((width || field == QStringLiteral("stroke_col"))
                            && sourceEvidence.value(QStringLiteral("composite_stroke_palette")).toBool()
                            && spec.paintRole != QStringLiteral("all_strokes"))
                        || (field==QStringLiteral("fill_color") && spec.paintRole==QStringLiteral("base_fill")
                            && sourceEvidence.value(QStringLiteral("colors")).toObject().value(field).toObject()
                                .value(QStringLiteral("pdf_paint_color_count")).toInt()!=1))) {
                        type = QStringLiteral("cartographic_symbol_component_unresolved");
                        description = QStringLiteral("规则未明确描画分量或复合符号的组成，不能以单条路径判定完整符号错误");
                    }
                }
                if (type.isEmpty()) return false;
                addCartographicSymbolRuleNotEvaluable(ref, side, spec, field, value, type, description);
                return true;
            };
            if (!spec.expectedColor.trimmed().isEmpty() && colorComponentDrawn) {
                const int colorIndex = ref.feature.fields().indexOf(colorField);
                if (colorIndex < 0) {
                    addCartographicSymbolRuleErr(ref, side, spec, colorField, QString(), QStringLiteral("%1侧%2缺少颜色字段，无法核查图式颜色规则").arg(side, spec.label));
                } else {
                    const QString actualColor = normText(ref.feature.attribute(colorIndex));
                    if (!isCartographicColorValid(actualColor) || !expectedColorsValid) {
                        addCartographicSymbolRuleNotEvaluable(ref, side, spec, colorField, actualColor,
                            QStringLiteral("cartographic_symbol_rule_color_invalid"), QStringLiteral("实际颜色或规则颜色格式无效，未据此判定图式不一致"));
                    } else if (SeriesSourceAudit::colorStatus(sourceEvidence, colorField, actualColor) == QStringLiteral("mismatch")) {
                        evidenceBlock(colorField, actualColor, false);
                    } else if (!matchesExpectedColor(actualColor)) {
                        if (!evidenceBlock(colorField, actualColor, false))
                            addCartographicSymbolRuleErr(ref, side, spec, colorField, actualColor, QStringLiteral("%1侧%2颜色不符合适用图式规则，应为%3").arg(side, spec.label, spec.expectedColor));
                    }
                }
            }
            if (toAttrBool(ref.feature, QStringLiteral("stroked"))
                && (spec.widthMm >= 0.0 || (spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm))
                && spec.actualWidthUnit != QStringLiteral("mm") && spec.actualWidthUnit != QStringLiteral("pt")) {
                const QString errorKey = QStringLiteral("%1|%2|%3|%4|stroke_wid").arg(side, ref.standardLayerName, ref.shpPath, seriesFeatureId(ref));
                if (!cartographicSymbolRuleKeys.contains(errorKey)) {
                    cartographicSymbolRuleKeys.insert(errorKey);
                    const int widthIndex = ref.feature.fields().indexOf(QStringLiteral("stroke_wid"));
                    const QString actualWidthText = widthIndex < 0 ? QString() : normText(ref.feature.attribute(widthIndex));
                    QJsonObject ext;
                    ext["error_type"] = QStringLiteral("cartographic_symbol_width_unit_unknown");
                    ext["decision_status"] = QStringLiteral("not_evaluable");
                    ext["check_method"] = QStringLiteral("cartographic_symbol_rule");
                    ext["field"] = QStringLiteral("stroke_wid");
                    ext["actual_value"] = actualWidthText;
                    ext["actual_width_unit"] = spec.actualWidthUnit.isEmpty() ? QStringLiteral("not_configured") : spec.actualWidthUnit;
                    ext["expected_width_mm"] = spec.widthMm >= 0.0 ? QJsonValue(spec.widthMm)
                        : QJsonValue(QStringLiteral("%1-%2").arg(spec.minWidthMm).arg(spec.maxWidthMm));
                    ext["symbol_rule_source"] = cartographicSymbolRuleSourceText;
                    ext["symbol_rule_feature"] = spec.label;
                    ext["business_class"] = spec.ruleKey;
                    ext["match_status"] = QStringLiteral("not_evaluable");
                    ext["match_score"] = QJsonValue::Null;
                    ext["confidence"] = QStringLiteral("none");
                    ext["description"] = QStringLiteral("线宽规则未声明 stroke_wid 的实际单位，无法与毫米规范可靠比较");
                    writeCartographicRulePoint(ext, ref);
                    const QString layerText = ref.standardLayerName.isEmpty() ? spec.label : ref.standardLayerName;
                    if (side == QStringLiteral("左")) addErr(QStringLiteral("制图符号规范"), layerText, ext.value(QStringLiteral("description")).toString(), ref, SeriesFeatureRef(), ext);
                    else addErr(QStringLiteral("制图符号规范"), layerText, ext.value(QStringLiteral("description")).toString(), SeriesFeatureRef(), ref, ext);
                }
            }
            if (toAttrBool(ref.feature, QStringLiteral("stroked"))
                && (spec.widthMm >= 0.0 || (spec.minWidthMm >= 0.0 && spec.maxWidthMm >= spec.minWidthMm))
                && (spec.actualWidthUnit == QStringLiteral("mm") || spec.actualWidthUnit == QStringLiteral("pt"))) {
                const int widthIndex = ref.feature.fields().indexOf(QStringLiteral("stroke_wid"));
                if (widthIndex < 0) {
                    addCartographicSymbolRuleErr(ref, side, spec, QStringLiteral("stroke_wid"), QString(), QStringLiteral("%1侧%2缺少 stroke_wid，无法核查图式尺寸/线宽规则").arg(side, spec.label));
                } else {
                    bool ok = false;
                    const QString actualWidthText = normText(ref.feature.attribute(widthIndex));
                    const double actualWidth = actualWidthText.toDouble(&ok);
                    if (!ok) {
                        addCartographicSymbolRuleNotEvaluable(ref, side, spec, QStringLiteral("stroke_wid"), actualWidthText,
                            QStringLiteral("cartographic_symbol_rule_value_invalid"), QStringLiteral("线宽值无法解析为数值，未据此判定符号不合规范"));
                        continue;
                    }
                    if (!ok || !cartographicWidthMatches(actualWidth, spec)) {
                        if (!evidenceBlock(QStringLiteral("stroke_wid"), actualWidthText, true))
                            addCartographicSymbolRuleErr(ref, side, spec, QStringLiteral("stroke_wid"), actualWidthText, QStringLiteral("%1侧%2尺寸/线宽不符合适用图式规则").arg(side, spec.label));
                    }
                }
            }
        }
    };
    struct AnnotationGradeSpec {
        QString ruleKey;
        QString category;
        QString grade;
        double sizeMm;
        double minSizeMm;
        double maxSizeMm;
        AnnotationGradeSpec() : sizeMm(-1.0), minSizeMm(-1.0), maxSizeMm(-1.0) {}
    };
    auto normalizeAnnotationGradeKey = [](QString text) {
        text = text.trimmed();
        text.remove(QRegularExpression(QStringLiteral("[\\s　]+")));
        return text;
    };
    auto parseAnnotationSizeText = [](QString sizeText, double& exact, double& minSize, double& maxSize) {
        exact = -1.0;
        minSize = -1.0;
        maxSize = -1.0;
        sizeText = sizeText.trimmed();
        sizeText.remove(QStringLiteral("mm"), Qt::CaseInsensitive);
        sizeText.remove(QStringLiteral("毫米"));
        sizeText.replace(QStringLiteral("－"), QStringLiteral("-"));
        sizeText.replace(QStringLiteral("—"), QStringLiteral("-"));
        sizeText.replace(QStringLiteral("~"), QStringLiteral("-"));
        sizeText.replace(QStringLiteral("至"), QStringLiteral("-"));
        if (sizeText.contains(QStringLiteral("-"))) {
            const QStringList range = sizeText.split(QStringLiteral("-"), Qt::SkipEmptyParts);
            bool okMin = false, okMax = false;
            const double a = range.value(0).trimmed().toDouble(&okMin);
            const double b = range.value(1).trimmed().toDouble(&okMax);
            if (!okMin || !okMax) return false;
            minSize = qMin(a, b);
            maxSize = qMax(a, b);
            return true;
        }
        bool ok = false;
        exact = sizeText.toDouble(&ok);
        return ok;
    };
    auto insertAnnotationGradeSpec = [&](QMap<QString, AnnotationGradeSpec>& specs, QString key, const AnnotationGradeSpec& spec) {
        key = normalizeAnnotationGradeKey(key);
        if (key.isEmpty()) return;
        AnnotationGradeSpec copy = spec;
        copy.ruleKey = key;
        specs.insert(key, copy);
    };
    auto parseAnnotationGradeRules = [&](const QString& text, const QString& defaultCategory) {
        QMap<QString, AnnotationGradeSpec> specs;
        const QStringList rows = text.split(QRegularExpression(QStringLiteral("[\\r\\n;；]+")), Qt::SkipEmptyParts);
        for (QString row : rows) {
            row = row.trimmed();
            if (row.isEmpty() || row.startsWith(QStringLiteral("#")) || row.startsWith(QStringLiteral("//"))) continue;
            const QStringList parts = row.split(QRegularExpression(QStringLiteral("[|｜]+")), Qt::KeepEmptyParts);
            if (parts.size() < 2) continue;
            AnnotationGradeSpec spec;
            QString key;
            QString sizeText;
            if (parts.size() >= 3) {
                spec.category = parts.value(0).trimmed();
                spec.grade = parts.value(1).trimmed();
                key = QStringLiteral("%1:%2").arg(spec.category, spec.grade);
                sizeText = parts.value(2).trimmed();
            } else {
                spec.category = defaultCategory;
                spec.grade = parts.value(0).trimmed();
                key = spec.grade;
                sizeText = parts.value(1).trimmed();
            }
            if (spec.grade.isEmpty()) continue;
            if (!parseAnnotationSizeText(sizeText, spec.sizeMm, spec.minSizeMm, spec.maxSizeMm)) continue;
            insertAnnotationGradeSpec(specs, key, spec);
            insertAnnotationGradeSpec(specs, spec.grade, spec);
            if (!spec.category.isEmpty()) insertAnnotationGradeSpec(specs, QStringLiteral("%1:%2").arg(spec.category, spec.grade), spec);
            if (spec.grade.endsWith(QStringLiteral("级"))) insertAnnotationGradeSpec(specs, spec.grade + QStringLiteral("河流"), spec);
        }
        return specs;
    };
    const QMap<QString, AnnotationGradeSpec> riverAnnotationGradeSpecs = parseAnnotationGradeRules(riverAnnotationGradeRuleText, QStringLiteral("河流"));
    const QMap<QString, AnnotationGradeSpec> mountainAnnotationGradeSpecs = parseAnnotationGradeRules(mountainAnnotationGradeRuleText, QStringLiteral("山脉山岭"));
    auto annotationGradeSizeCandidates = [&](const AnnotationGradeSpec& spec) {
        QJsonArray arr;
        const double ptPerMm = 2.83464566929134;
        if (spec.sizeMm >= 0.0) {
            QJsonObject mmObj; mmObj["unit"] = QStringLiteral("mm"); mmObj["value"] = spec.sizeMm; arr.append(mmObj);
            QJsonObject ptObj; ptObj["unit"] = QStringLiteral("pt_exported_font_size"); ptObj["value"] = spec.sizeMm * ptPerMm; arr.append(ptObj);
        } else if (spec.minSizeMm >= 0.0 && spec.maxSizeMm >= spec.minSizeMm) {
            QJsonObject mmObj; mmObj["unit"] = QStringLiteral("mm"); mmObj["min"] = spec.minSizeMm; mmObj["max"] = spec.maxSizeMm; arr.append(mmObj);
            QJsonObject ptObj; ptObj["unit"] = QStringLiteral("pt_exported_font_size"); ptObj["min"] = spec.minSizeMm * ptPerMm; ptObj["max"] = spec.maxSizeMm * ptPerMm; arr.append(ptObj);
        }
        return arr;
    };
    auto annotationGradeSizeMatches = [&](double actualSize, const AnnotationGradeSpec& spec) {
        const double ptPerMm = 2.83464566929134;
        if (spec.sizeMm >= 0.0) {
            return qAbs(actualSize - spec.sizeMm) <= 0.03 || qAbs(actualSize - spec.sizeMm * ptPerMm) <= 0.08;
        }
        if (spec.minSizeMm >= 0.0 && spec.maxSizeMm >= spec.minSizeMm) {
            const bool inMmRange = actualSize >= spec.minSizeMm - 0.03 && actualSize <= spec.maxSizeMm + 0.03;
            const bool inPtRange = actualSize >= spec.minSizeMm * ptPerMm - 0.08 && actualSize <= spec.maxSizeMm * ptPerMm + 0.08;
            return inMmRange || inPtRange;
        }
        return true;
    };
    auto firstExistingFieldValue = [&](const QgsFeature& feature, const QStringList& fields) {
        for (const QString& fieldName : fields) {
            const int idx = feature.fields().indexOf(fieldName.trimmed());
            if (idx >= 0) return normText(feature.attribute(idx));
        }
        return QString();
    };
    auto normalizeChineseLevelText = [](QString text) {
        text = text.trimmed();
        if (text.isEmpty()) return QString();
        bool ok = false;
        const double numeric = text.toDouble(&ok);
        if (ok) {
            const int value = qRound(numeric);
            static const QStringList levels{ QString(), QStringLiteral("一级"), QStringLiteral("二级"), QStringLiteral("三级"), QStringLiteral("四级"), QStringLiteral("五级"), QStringLiteral("六级") };
            if (value > 0 && value < levels.size()) return levels.value(value);
        }
        text.remove(QStringLiteral("河流"));
        return text;
    };
    auto appendAnnotationGradeKey = [&](QStringList& keys, const QString& key) {
        const QString normalized = normalizeAnnotationGradeKey(key);
        if (!normalized.isEmpty() && !keys.contains(normalized)) keys << normalized;
    };
    auto annotationGradeKeyCandidates = [&](const SeriesFeatureRef& ref, bool mountainRules) {
        QStringList keys;
        const QString base = normalizeSeriesLayerBaseName(ref.shpName);
        const QString textValue = firstExistingFieldValue(ref.feature, annotationTextFields);
        const QString haystack = base + QStringLiteral("|") + ref.standardLayerName + QStringLiteral("|") + textValue;
        auto has = [&](const QString& token) { return haystack.contains(token, Qt::CaseInsensitive); };
        auto addLevel = [&](const QString& rawLevel) {
            const QString level = normalizeChineseLevelText(rawLevel);
            if (level.isEmpty()) return;
            appendAnnotationGradeKey(keys, level);
            appendAnnotationGradeKey(keys, level + QStringLiteral("河流"));
            if (mountainRules) {
                if (has(QStringLiteral("山脉"))) appendAnnotationGradeKey(keys, QStringLiteral("山脉:%1").arg(level));
                if (has(QStringLiteral("山岭"))) appendAnnotationGradeKey(keys, QStringLiteral("山岭:%1").arg(level));
            }
        };
        for (const QString& fieldName : riverLevelFields) {
            const int idx = ref.feature.fields().indexOf(fieldName.trimmed());
            if (idx >= 0) addLevel(normText(ref.feature.attribute(idx)));
        }
        if (has(QStringLiteral("一级"))) addLevel(QStringLiteral("一级"));
        if (has(QStringLiteral("二级"))) addLevel(QStringLiteral("二级"));
        if (has(QStringLiteral("三级"))) addLevel(QStringLiteral("三级"));
        if (has(QStringLiteral("四级"))) addLevel(QStringLiteral("四级"));
        if (has(QStringLiteral("五级"))) addLevel(QStringLiteral("五级"));
        if (has(QStringLiteral("六级"))) addLevel(QStringLiteral("六级"));
        if (!mountainRules) {
            if (has(QStringLiteral("总干渠"))) appendAnnotationGradeKey(keys, QStringLiteral("总干渠"));
            if (has(QStringLiteral("支渠"))) appendAnnotationGradeKey(keys, QStringLiteral("支渠"));
            if (has(QStringLiteral("干渠")) && !has(QStringLiteral("总干渠"))) appendAnnotationGradeKey(keys, QStringLiteral("干渠"));
            if (has(QStringLiteral("沟渠")) || has(QStringLiteral("水渠"))) appendAnnotationGradeKey(keys, QStringLiteral("其他渠道"));
            if (has(QStringLiteral("水库"))) appendAnnotationGradeKey(keys, QStringLiteral("水库"));
            if (has(QStringLiteral("湖泊"))) appendAnnotationGradeKey(keys, QStringLiteral("湖泊"));
        }
        appendAnnotationGradeKey(keys, textValue);
        appendAnnotationGradeKey(keys, base);
        return keys;
    };
    auto annotationGradeSpecForRef = [&](const SeriesFeatureRef& ref, QString* sourceText = nullptr, QString* matchedKey = nullptr) {
        const QString annotationClass = seriesLayerAnnotationClass(ref.shpName);
        const bool mountainCandidate = annotationClass.startsWith(QStringLiteral("mountain_annotation"))
            || normalizeSeriesLayerBaseName(ref.shpName).contains(QStringLiteral("山脉"))
            || normalizeSeriesLayerBaseName(ref.shpName).contains(QStringLiteral("山岭"));
        const bool riverCandidate = annotationClass.startsWith(QStringLiteral("hydrology_annotation"));
        const QMap<QString, AnnotationGradeSpec>& specs = mountainCandidate ? mountainAnnotationGradeSpecs : riverAnnotationGradeSpecs;
        if (!mountainCandidate && !riverCandidate) return AnnotationGradeSpec();
        const QStringList keys = annotationGradeKeyCandidates(ref, mountainCandidate);
        for (const QString& key : keys) {
            if (specs.contains(key)) {
                if (sourceText) *sourceText = mountainCandidate ? mountainAnnotationGradeRuleSourceText : riverAnnotationGradeRuleSourceText;
                if (matchedKey) *matchedKey = key;
                return specs.value(key);
            }
        }
        return AnnotationGradeSpec();
    };
    QSet<QString> annotationGradeRuleKeys;
    auto addAnnotationGradeRuleErr = [&](const SeriesFeatureRef& ref, const QString& side, const AnnotationGradeSpec& spec, const QString& sourceText, const QString& matchedKey, const QString& actualText, const QString& desc) {
        const QString key = QStringLiteral("%1|%2|%3|%4|annotation_font_size").arg(side, ref.standardLayerName, ref.shpPath, seriesFeatureId(ref));
        if (annotationGradeRuleKeys.contains(key)) return;
        annotationGradeRuleKeys.insert(key);
        QJsonObject ext;
        ext["error_type"] = QStringLiteral("annotation_grade_rule_mismatch");
        ext["check_method"] = QStringLiteral("annotation_grade_rule");
        ext["invalid_side"] = side;
        ext["field"] = annotationFontSizeFields.value(0, QStringLiteral("font_size"));
        ext["annotation_text_field"] = annotationTextFields.value(0, QStringLiteral("text_conte"));
        ext["annotation_text"] = firstExistingFieldValue(ref.feature, annotationTextFields);
        ext["annotation_grade_rule_source"] = sourceText;
        ext["annotation_grade_rule_key"] = matchedKey;
        ext["annotation_grade"] = spec.grade;
        ext["annotation_grade_category"] = spec.category;
        if (spec.sizeMm >= 0.0) ext["expected_font_size_mm"] = spec.sizeMm;
        else ext["expected_font_size_mm"] = QStringLiteral("%1-%2").arg(spec.minSizeMm).arg(spec.maxSizeMm);
        ext["annotation_size_unit_candidates"] = annotationGradeSizeCandidates(spec);
        ext["actual_font_size"] = actualText;
        ext["actual_value"] = actualText;
        ext["match_status"] = QStringLiteral("annotation_grade_check");
        ext["style_check_detail"] = QStringLiteral("annotation_grade_validation");
        ext["candidate_count"] = 0;
        ext["match_score"] = QJsonValue::Null;
        ext["match_reason"] = QStringLiteral("按附件1/附件2注记分级规则核查单个注记要素字号，不做跨比例尺字段相等比较");
        ext["confidence"] = QStringLiteral("high");
        writeCartographicRulePoint(ext, ref);
        const QString layerText = ref.standardLayerName.isEmpty() ? normalizeSeriesLayerBaseName(ref.shpName) : ref.standardLayerName;
        if (side == QStringLiteral("左")) {
            ext["left_value"] = actualText;
            ext["right_value"] = QJsonValue::Null;
            addErr(QStringLiteral("注记分级规范"), layerText, desc, ref, SeriesFeatureRef(), ext);
        } else {
            ext["left_value"] = QJsonValue::Null;
            ext["right_value"] = actualText;
            addErr(QStringLiteral("注记分级规范"), layerText, desc, SeriesFeatureRef(), ref, ext);
        }
    };
    auto validateAnnotationGradeRulesForCollectedRefs = [&](const QList<SeriesFeatureRef>& refs, const QString& side) {
        for (const SeriesFeatureRef& ref : refs) {
            if (!ref.feature.isValid() || !ref.feature.hasGeometry()) continue;
            QString sourceText, matchedKey;
            const AnnotationGradeSpec spec = annotationGradeSpecForRef(ref, &sourceText, &matchedKey);
            if (spec.grade.isEmpty()) continue;
            const QString actualSizeText = firstExistingFieldValue(ref.feature, annotationFontSizeFields);
            if (actualSizeText.isEmpty()) {
                addAnnotationGradeRuleErr(ref, side, spec, sourceText, matchedKey, QString(), QStringLiteral("%1侧%2缺少注记字号字段，无法核查分级注记规则").arg(side, normalizeSeriesLayerBaseName(ref.shpName)));
                continue;
            }
            bool ok = false;
            const double actualSize = actualSizeText.toDouble(&ok);
            if (!ok || !annotationGradeSizeMatches(actualSize, spec)) {
                addAnnotationGradeRuleErr(ref, side, spec, sourceText, matchedKey, actualSizeText, QStringLiteral("%1侧%2注记字号不符合附件分级规则").arg(side, normalizeSeriesLayerBaseName(ref.shpName)));
            }
        }
    };
    QSet<QString> invalidStyleValueKeys;
    auto addInvalidStyleValueErr = [&](const SeriesFeatureRef& ref, const QString& side, const QString& fieldName, const QString& valueText, const QString& desc) {
        const QString key = QStringLiteral("%1|%2|%3|%4|%5").arg(side, ref.standardLayerName, ref.shpPath, seriesFeatureId(ref), fieldName);
        if (invalidStyleValueKeys.contains(key)) return;
        invalidStyleValueKeys.insert(key);
        QJsonObject ext;
        ext["error_type"] = QStringLiteral("style_field_value_invalid");
        ext["field"] = fieldName;
        ext["invalid_side"] = side;
        ext["match_status"] = QStringLiteral("style_check");
        ext["style_check_detail"] = QStringLiteral("candidate_style_validation");
        ext["candidate_count"] = 0;
        ext["match_score"] = QJsonValue::Null;
        ext["match_reason"] = QStringLiteral("候选要素样式字段自身无效，未依赖一对一几何匹配");
        ext["confidence"] = QStringLiteral("high");
        writeCartographicRulePoint(ext, ref);
        if (side == QStringLiteral("左")) {
            ext["left_value"] = valueText;
            ext["right_value"] = QJsonValue::Null;
            addErr(QStringLiteral("字段值异常"), ref.standardLayerName.isEmpty() ? QStringLiteral("未知图层") : ref.standardLayerName, desc, ref, SeriesFeatureRef(), ext);
        } else {
            ext["left_value"] = QJsonValue::Null;
            ext["right_value"] = valueText;
            addErr(QStringLiteral("字段值异常"), ref.standardLayerName.isEmpty() ? QStringLiteral("未知图层") : ref.standardLayerName, desc, SeriesFeatureRef(), ref, ext);
        }
    };
    auto validateStyleValuesForCollectedRefs = [&](const QList<SeriesFeatureRef>& refs, const QString& side) {
        for (const SeriesFeatureRef& ref : refs) {
            if (!ref.feature.isValid() || !ref.feature.hasGeometry()) continue;
            const auto gt = QgsWkbTypes::geometryType(ref.feature.geometry().wkbType());
            const QStringList fields = styleFieldsForType(gt);
            const bool hasStrokeColRule = fields.contains(QStringLiteral("stroke_col"));
            const bool hasFillColorRule = fields.contains(QStringLiteral("fill_color"));
            const bool hasStrokeWidRule = fields.contains(QStringLiteral("stroke_wid"));
            const bool needsStrokeStyle = toAttrBool(ref.feature, QStringLiteral("stroked"));
            if (hasStrokeColRule && needsStrokeStyle && ref.feature.fields().indexOf(QStringLiteral("stroke_col")) >= 0) {
                const QString stroke_col = normText(ref.feature.attribute(QStringLiteral("stroke_col")));
                const QString normalizedColor = normalizeSeriesCompareValue(QStringLiteral("stroke_col"), ref.feature.attribute(QStringLiteral("stroke_col")));
                if (normalizedColor.isEmpty() || stroke_col == QStringLiteral("0")) {
                    addInvalidStyleValueErr(ref, side, QStringLiteral("stroke_col"), stroke_col, QStringLiteral("%1侧候选要素 stroked/filled=true 但 stroke_col 为空或为0").arg(side));
                }
            }
            if (hasFillColorRule && toAttrBool(ref.feature, QStringLiteral("filled"))
                && ref.feature.fields().indexOf(QStringLiteral("fill_color")) >= 0) {
                const QString fillColor = normText(ref.feature.attribute(QStringLiteral("fill_color")));
                const QString normalizedFillColor = normalizeSeriesCompareValue(QStringLiteral("fill_color"), ref.feature.attribute(QStringLiteral("fill_color")));
                if (normalizedFillColor.isEmpty() || fillColor == QStringLiteral("0")) {
                    addInvalidStyleValueErr(ref, side, QStringLiteral("fill_color"), fillColor, QStringLiteral("%1侧候选要素 filled=true 但 fill_color 为空或为0").arg(side));
                }
            }
            if (hasStrokeWidRule && toAttrBool(ref.feature, QStringLiteral("stroked")) && ref.feature.fields().indexOf(QStringLiteral("stroke_wid")) >= 0) {
                bool ok = false;
                const double strokeWidth = normText(ref.feature.attribute(QStringLiteral("stroke_wid"))).toDouble(&ok);
                if (!ok || strokeWidth <= 0.0) {
                    addInvalidStyleValueErr(ref, side, QStringLiteral("stroke_wid"), normText(ref.feature.attribute(QStringLiteral("stroke_wid"))), QStringLiteral("%1侧候选要素 stroked=true 但 stroke_wid 不是有效正数").arg(side));
                }
            }
        }
    };
    auto buildExpressionSemantics = [&](const SeriesFeatureRef& ref, int scaleDenom) {
        QJsonObject semantics;
        const QString stdLayer = ref.standardLayerName.trimmed();
        const QString layerName = stdLayer.isEmpty() ? normalizeSeriesLayerBaseName(ref.shpName) : stdLayer;
        const auto geomType = ref.feature.isValid() && ref.feature.hasGeometry()
            ? QgsWkbTypes::geometryType(ref.feature.geometry().wkbType())
            : QgsWkbTypes::UnknownGeometry;
        QString geomText = QStringLiteral("unknown");
        if (geomType == QgsWkbTypes::LineGeometry) geomText = QStringLiteral("line");
        else if (geomType == QgsWkbTypes::PolygonGeometry) geomText = QStringLiteral("polygon");
        else if (geomType == QgsWkbTypes::PointGeometry) geomText = QStringLiteral("point");
        QString businessDomain = QStringLiteral("other");
        const QString key = layerName;
        if (key.contains(QStringLiteral("道")) || key.contains(QStringLiteral("路")) || key.contains(QStringLiteral("高速"))) businessDomain = QStringLiteral("road");
        else if (key.contains(QStringLiteral("河")) || key.contains(QStringLiteral("渠")) || key.contains(QStringLiteral("水"))) businessDomain = QStringLiteral("hydrology");
        else if (key.contains(QStringLiteral("界")) || key.contains(QStringLiteral("境界"))) businessDomain = QStringLiteral("boundary");
        else if (key.contains(QStringLiteral("政区")) || key.contains(QStringLiteral("行政"))) businessDomain = QStringLiteral("admin_area");
        const bool stroked = toAttrBool(ref.feature, QStringLiteral("stroked"));
        const bool filled = toAttrBool(ref.feature, QStringLiteral("filled"));
        const bool dashed = toAttrBool(ref.feature, QStringLiteral("is_dashed"));
        QString expressionClass = geomText;
        if (geomType == QgsWkbTypes::LineGeometry) {
            expressionClass = dashed ? QStringLiteral("line_dashed") : (stroked ? QStringLiteral("line_solid") : QStringLiteral("line_unstroked"));
        } else if (geomType == QgsWkbTypes::PolygonGeometry) {
            expressionClass = filled && stroked ? QStringLiteral("polygon_fill_and_stroke") : (filled ? QStringLiteral("polygon_fill") : (stroked ? QStringLiteral("polygon_outline") : QStringLiteral("polygon_unstyled")));
        } else if (geomType == QgsWkbTypes::PointGeometry) {
            expressionClass = stroked || filled ? QStringLiteral("point_symbol") : QStringLiteral("point_unstyled");
        }
        semantics["scale"] = scaleDenom;
        semantics["standard_layer"] = layerName;
        semantics["business_domain"] = businessDomain;
        semantics["geometry_semantics"] = geomText;
        semantics["expression_class"] = expressionClass;
        semantics["stroked"] = stroked;
        semantics["filled"] = filled;
        semantics["is_dashed"] = dashed;
        return semantics;
    };
    auto buildStyleSignature = [&](const SeriesFeatureRef& ref) {
        QStringList parts;
        if (!ref.feature.isValid() || !ref.feature.hasGeometry()) return QString();
        const QStringList fields = styleFieldsForType(QgsWkbTypes::geometryType(ref.feature.geometry().wkbType()));
        for (const QString& fieldName : fields) {
            if (fieldName.trimmed().isEmpty()) continue;
            const int fieldIndex = ref.feature.fields().indexOf(fieldName);
            const QString value = fieldIndex < 0
                ? QStringLiteral("<missing>")
                : normalizeSeriesCompareValue(fieldName, ref.feature.attribute(fieldIndex));
            parts << QStringLiteral("%1=%2").arg(fieldName, value);
        }
        return parts.join(QStringLiteral("|"));
    };
    auto appendStyleRiskRecords = [&](const QList<SeriesFeatureRef>& refs, const QString& side, int scaleDenom, QJsonArray& styleRiskRecords, QJsonObject& styleRiskSummary) {
        QMap<QString, QMap<QString, int>> countsByLayer;
        QMap<QString, QList<SeriesFeatureRef>> refsByLayer;
        QMap<QString, QMap<QString, QJsonObject>> sampleSemanticsByLayer;
        for (const SeriesFeatureRef& ref : refs) {
            if (!ref.feature.isValid() || !ref.feature.hasGeometry()) continue;
            const QString layerKey = ref.standardLayerName.trimmed().isEmpty() ? normalizeSeriesLayerBaseName(ref.shpName) : ref.standardLayerName.trimmed();
            const QString signature = buildStyleSignature(ref);
            if (layerKey.isEmpty() || signature.isEmpty()) continue;
            countsByLayer[layerKey][signature] = countsByLayer[layerKey].value(signature) + 1;
            refsByLayer[layerKey] << ref;
            if (!sampleSemanticsByLayer[layerKey].contains(signature)) sampleSemanticsByLayer[layerKey][signature] = buildExpressionSemantics(ref, scaleDenom);
        }
        for (auto layerIt = refsByLayer.constBegin(); layerIt != refsByLayer.constEnd(); ++layerIt) {
            const QString layerKey = layerIt.key();
            const QMap<QString, int> signatureCounts = countsByLayer.value(layerKey);
            QString dominantSignature;
            int dominantCount = 0;
            for (auto sigIt = signatureCounts.constBegin(); sigIt != signatureCounts.constEnd(); ++sigIt) {
                if (sigIt.value() > dominantCount) {
                    dominantSignature = sigIt.key();
                    dominantCount = sigIt.value();
                }
            }
            if (signatureCounts.size() <= 1 || dominantCount < 2) continue;
            for (const SeriesFeatureRef& ref : layerIt.value()) {
                const QString signature = buildStyleSignature(ref);
                if (signature == dominantSignature) continue;
                QJsonObject record;
                record["style_risk_status"] = QStringLiteral("diagnostic_only");
                record["risk_type"] = QStringLiteral("same_scale_internal_style_outlier");
                record["side"] = side;
                record["scale"] = scaleDenom;
                record["standard_layer"] = layerKey;
                record["shp"] = ref.shpName;
                record["shp_path"] = ref.shpPath;
                record["fid"] = seriesFeatureId(ref);
                record["style_signature"] = signature;
                record["dominant_style_signature"] = dominantSignature;
                record["dominant_style_count"] = dominantCount;
                record["style_signature_count"] = signatureCounts.value(signature);
                record["expression_semantics"] = buildExpressionSemantics(ref, scaleDenom);
                record["dominant_expression_semantics"] = sampleSemanticsByLayer.value(layerKey).value(dominantSignature);
                record["reason"] = QStringLiteral("同一比例尺同一标准图层内部样式与主流样式不一致；style_risk_records are diagnostic only");
                styleRiskRecords.append(record);
                styleRiskSummary[side] = styleRiskSummary.value(side).toInt() + 1;
                styleRiskSummary[layerKey] = styleRiskSummary.value(layerKey).toInt() + 1;
            }
        }
    };
    auto lineCheck = [&](const QString& stdName) {
        append501DebugLog(QStringLiteral("[501] lineCheck begin pair=%1 layer=%2").arg(filePaths.seriesPairName, stdName));
        QStringList expandedLayers;
        QStringList actualPaths;
        QStringList leftActualPaths;
        QStringList rightActualPaths;
        QMap<QString, QStringList> leftPathGroups;
        QMap<QString, QStringList> rightPathGroups;
        QMap<QString, QStringList> leftAnnotationPathGroups;
        QMap<QString, QStringList> rightAnnotationPathGroups;
        QString matchingStrategy = hasConfirmedLayerMapping ? QStringLiteral("user_confirmed_layer_mapping") : QStringLiteral("shp_name_alias");
        try {
            append501DebugLog(QStringLiteral("[501] lineCheck expand begin pair=%1 layer=%2").arg(filePaths.seriesPairName, stdName));
            expandedLayers = seriesLayerAliasNamesForCategory(layerNameAliases, stdName);
            if (expandedLayers.isEmpty()) expandedLayers = expandSeriesLayerGroup(stdName);
            append501DebugLog(QStringLiteral("[501] lineCheck expand ok pair=%1 layer=%2 expanded=%3").arg(filePaths.seriesPairName, stdName, expandedLayers.join(QStringLiteral("、"))));
            append501DebugLog(QStringLiteral("[501] lineCheck actualPathResolve begin pair=%1 layer=%2").arg(filePaths.seriesPairName, stdName));
            const QString leftRootDir = filePaths.seriesLeftShpPath.isEmpty() ? filePaths.shpPath : filePaths.seriesLeftShpPath;
            const QString rightRootDir = filePaths.seriesRightShpPath.isEmpty() ? filePaths.shpPath : filePaths.seriesRightShpPath;
            auto appendUniquePaths = [](QStringList& target, const QStringList& paths) {
                for (const QString& path : paths) if (!target.contains(path)) target << path;
            };
            if (hasConfirmedLayerMapping) {
                leftPathGroups = confirmedLayerGroupsForStandardLayer(confirmedLayerMapping, stdName, QStringLiteral("left"));
                rightPathGroups = confirmedLayerGroupsForStandardLayer(confirmedLayerMapping, stdName, QStringLiteral("right"));
            } else {
                leftPathGroups = isCrossScale ? collectSeriesPathGroupsByFamily(stdName, layerNameAliases, leftRootDir) : collectSeriesPathGroupsByAlias(stdName, layerNameAliases, leftRootDir);
                rightPathGroups = isCrossScale ? collectSeriesPathGroupsByFamily(stdName, layerNameAliases, rightRootDir) : collectSeriesPathGroupsByAlias(stdName, layerNameAliases, rightRootDir);
            }
            if (!hasConfirmedLayerMapping && (!riverAnnotationGradeSpecs.isEmpty() || !mountainAnnotationGradeSpecs.isEmpty())) {
                leftAnnotationPathGroups = collectSeriesAnnotationPathGroupsForCategory(stdName, layerNameAliases, leftRootDir);
                rightAnnotationPathGroups = collectSeriesAnnotationPathGroupsForCategory(stdName, layerNameAliases, rightRootDir);
            }
            for (auto it = leftPathGroups.constBegin(); it != leftPathGroups.constEnd(); ++it) {
                appendUniquePaths(leftActualPaths, it.value());
            }
            for (auto it = rightPathGroups.constBegin(); it != rightPathGroups.constEnd(); ++it) {
                appendUniquePaths(rightActualPaths, it.value());
            }
            actualPaths = leftActualPaths;
            for (const QString& path : rightActualPaths) if (!actualPaths.contains(path)) actualPaths << path;
            append501DebugLog(QStringLiteral("[501] actualPaths pair=%1 layer=%2 paths=%3").arg(filePaths.seriesPairName, stdName, actualPaths.join(QStringLiteral(";"))));
        }
        catch (const std::exception& ex) {
            inputValidationErrors << QStringLiteral("[%1] 图层路径解析异常").arg(stdName);
            append501DebugLog(QStringLiteral("[501] lineCheck resolve exception pair=%1 layer=%2 msg=%3").arg(filePaths.seriesPairName, stdName, QString::fromUtf8(ex.what())));
            QJsonObject stat; stat["left_candidates"] = 0; stat["right_candidates"] = 0; stat["matched_candidates"] = 0; geometrySummary[stdName] = stat; return;
        }
        catch (...) {
            inputValidationErrors << QStringLiteral("[%1] 图层路径解析发生未知异常").arg(stdName);
            append501DebugLog(QStringLiteral("[501] lineCheck resolve unknownException pair=%1 layer=%2").arg(filePaths.seriesPairName, stdName));
            QJsonObject stat; stat["left_candidates"] = 0; stat["right_candidates"] = 0; stat["matched_candidates"] = 0; geometrySummary[stdName] = stat; return;
        }
        auto pathGroupsToJson = [](const QMap<QString, QStringList>& pathGroups) {
            QJsonObject obj;
            for (auto it = pathGroups.constBegin(); it != pathGroups.constEnd(); ++it) {
                obj[it.key()] = QJsonArray::fromStringList(it.value());
            }
            return obj;
        };
        QJsonObject pathInfo; pathInfo["xml_layer_name"] = stdName; pathInfo["expanded_standard_layers"] = QJsonArray::fromStringList(expandedLayers); pathInfo["matching_strategy"] = matchingStrategy; pathInfo["actual_dir"] = filePaths.shpPath; pathInfo["left_actual_dir"] = filePaths.seriesLeftShpPath; pathInfo["right_actual_dir"] = filePaths.seriesRightShpPath; pathInfo["actual_found"] = !actualPaths.isEmpty(); pathInfo["actual_paths"] = QJsonArray::fromStringList(actualPaths); pathInfo["left_actual_paths"] = QJsonArray::fromStringList(leftActualPaths); pathInfo["right_actual_paths"] = QJsonArray::fromStringList(rightActualPaths); pathInfo["left_path_groups"] = pathGroupsToJson(leftPathGroups); pathInfo["right_path_groups"] = pathGroupsToJson(rightPathGroups); pathInfo["left_annotation_path_groups"] = pathGroupsToJson(leftAnnotationPathGroups); pathInfo["right_annotation_path_groups"] = pathGroupsToJson(rightAnnotationPathGroups); pathInfo["actual_paths_text"] = actualPaths.join(QStringLiteral("；")); pathInfo["issue_types"] = QJsonArray::fromStringList(inputValidationErrors); pathInfo["layer_status"] = actualPaths.isEmpty() && hasConfirmedLayerMapping ? QStringLiteral("user_skipped") : (actualPaths.isEmpty() ? QStringLiteral("actual_layer_missing") : QStringLiteral("checked")); if (hasConfirmedLayerMapping) { const QJsonObject layerMetadata = confirmedLayerMetadataForStandardLayer(confirmedLayerMapping, stdName); for (auto metaIt = layerMetadata.constBegin(); metaIt != layerMetadata.constEnd(); ++metaIt) pathInfo[metaIt.key()] = metaIt.value(); } else if (isCrossScale) { pathInfo["match_family"] = seriesLayerFamily(stdName, layerNameAliases); pathInfo["relation_type"] = QStringLiteral("cross_scale_family_group"); } layerPathSummary[stdName] = pathInfo;
        if (hasConfirmedLayerMapping && (leftActualPaths.isEmpty() || rightActualPaths.isEmpty())) { QJsonObject stat; stat["left_candidates"] = 0; stat["right_candidates"] = 0; stat["matched_candidates"] = 0; stat["execution_state"] = QStringLiteral("user_skipped"); geometrySummary[stdName] = stat; append501DebugLog(QStringLiteral("[501] userSkippedLayer pair=%1 layer=%2 left=%3 right=%4").arg(filePaths.seriesPairName, stdName).arg(leftActualPaths.size()).arg(rightActualPaths.size())); return; }
        if (actualPaths.isEmpty()) { inputValidationErrors << QStringLiteral("[%1] 实际成果数据目录下未找到对应 SHP").arg(stdName); QJsonObject stat; stat["left_candidates"] = 0; stat["right_candidates"] = 0; stat["matched_candidates"] = 0; geometrySummary[stdName] = stat; return; }
        ++checkedLayerCount;
        QList<SeriesFeatureRef> lefts, rights, leftAnnotationRefs, rightAnnotationRefs;
        QStringList shpNames;
        QJsonArray layerReadRecords;
        auto collectRefsForStandardLayer = [&](const QMap<QString, QStringList>& pathGroups, const QgsGeometry& zone, QList<SeriesFeatureRef>& refs, bool leftSide) {
            for (auto groupIt = pathGroups.constBegin(); groupIt != pathGroups.constEnd(); ++groupIt) {
            const QString standardLayerName = groupIt.key();
            for (const QString& p : groupIt.value()) {
            QgsVectorLayer l(p, "a", "ogr");
            if (!l.isValid()) { inputValidationErrors << QStringLiteral("[%1] 图层加载失败：%2").arg(stdName, p); append501DebugLog(QStringLiteral("[501] invalidLayer pair=%1 layer=%2 path=%3").arg(filePaths.seriesPairName, stdName, p)); continue; }
            const QString shpName = QFileInfo(p).fileName();
            if (!shpNames.contains(shpName)) shpNames << shpName;
            const int beforeCount = refs.size(); int readCount = 0;
            QgsFeature f; QgsFeatureRequest q; q.setFilterRect(edgeZone.boundingBox());
            for (QgsFeatureIterator it = l.getFeatures(q); it.nextFeature(f);) {
                ++readCount;
                if (!f.hasGeometry()) continue;
                QgsGeometry g = f.geometry();
                if (!g.isGeosValid()) {
                    g = g.makeValid();
                    if (g.isNull() || g.isEmpty()) {
                        append501DebugLog(QStringLiteral("[501] makeValid failed pair=%1 layer=%2 fid=%3").arg(filePaths.seriesPairName, stdName).arg(f.id()));
                        inputValidationErrors << QStringLiteral("[%1] 要素几何无法修复：%2，FID=%3").arg(stdName,p).arg(f.id());
                        continue;
                    }
                    f.setGeometry(g);
                }
                const auto geomType = QgsWkbTypes::geometryType(g.wkbType()); if (g.isNull() || g.isEmpty() || (geomType != QgsWkbTypes::LineGeometry && geomType != QgsWkbTypes::PolygonGeometry && geomType != QgsWkbTypes::PointGeometry)) continue;
                if (!g.intersects(zone)) continue;
                if (isCrossScale) {
                    QgsGeometry clipped = g.intersection(zone);
                    clipped.convertGeometryCollectionToSubclass(geomType);
                    if (clipped.isNull() || clipped.isEmpty()
                        || QgsWkbTypes::geometryType(clipped.wkbType()) != geomType) continue;
                    // A long outside line touching the edge for a sub-tolerance
                    // fragment is not evidence that the whole line is covered.
                    const double positionTolerance = qMax(lineTol, endTol);
                    if (geomType == QgsWkbTypes::LineGeometry
                        && clipped.length() < positionTolerance
                        && g.length() > positionTolerance
                        && clipped.length() < g.length() - 1e-8) continue;
                    f.setGeometry(clipped);
                }
                const SeriesFeatureRef ref{ f, p, shpName, standardLayerName, sourceSheetNameForPath(p, leftSide) };
                refs << ref;
            }
            layerReadRecords.append(QJsonObject{{QStringLiteral("path"),p},{QStringLiteral("side"),leftSide?QStringLiteral("left"):QStringLiteral("right")},
                {QStringLiteral("source_features"),static_cast<double>(l.featureCount())},{QStringLiteral("bbox_candidates"),readCount},
                {QStringLiteral("in_scope_candidates"),refs.size()-beforeCount}});
            }
            }
        };
        collectRefsForStandardLayer(leftPathGroups, leftZone, lefts, true);
        collectRefsForStandardLayer(rightPathGroups, rightZone, rights, false);
        collectRefsForStandardLayer(leftAnnotationPathGroups, leftZone, leftAnnotationRefs, true);
        collectRefsForStandardLayer(rightAnnotationPathGroups, rightZone, rightAnnotationRefs, false);
        QMap<QString, int> leftStableIdentityCounts;
        QMap<QString, int> rightStableIdentityCounts;
        for (const SeriesFeatureRef& ref : lefts) {
            const QString identity = crossScaleStableFeatureIdentity(ref);
            if (!identity.isEmpty()) ++leftStableIdentityCounts[identity];
        }
        for (const SeriesFeatureRef& ref : rights) {
            const QString identity = crossScaleStableFeatureIdentity(ref);
            if (!identity.isEmpty()) ++rightStableIdentityCounts[identity];
        }
        validateStyleValuesForCollectedRefs(lefts, QStringLiteral("左"));
        validateStyleValuesForCollectedRefs(rights, QStringLiteral("右"));
        validateCartographicSymbolRulesForCollectedRefs(lefts, QStringLiteral("左"));
        validateCartographicSymbolRulesForCollectedRefs(rights, QStringLiteral("右"));
        if (hasConfirmedLayerMapping) {
            validateAnnotationGradeRulesForCollectedRefs(lefts, QStringLiteral("左"));
            validateAnnotationGradeRulesForCollectedRefs(rights, QStringLiteral("右"));
        }
        validateAnnotationGradeRulesForCollectedRefs(leftAnnotationRefs, QStringLiteral("左"));
        validateAnnotationGradeRulesForCollectedRefs(rightAnnotationRefs, QStringLiteral("右"));
        pathInfo["actual_shp_names"] = QJsonArray::fromStringList(shpNames);
        pathInfo["feature_reads"] = layerReadRecords;
        pathInfo["left_candidates"] = lefts.size(); pathInfo["right_candidates"] = rights.size();
        layerPathSummary[stdName] = pathInfo;
        QgsSpatialIndex rightIndex; QMap<qint64, SeriesFeatureRef> rightMap; qint64 rightKey = 1; for (const SeriesFeatureRef& ref : rights) { if (!ref.feature.hasGeometry()) continue; QgsFeature indexed = ref.feature; indexed.setId(rightKey); rightIndex.addFeature(indexed); rightMap.insert(rightKey, ref); ++rightKey; } QgsSpatialIndex leftIndex; QMap<qint64, SeriesFeatureRef> leftMap; qint64 leftKey = 1; for (const SeriesFeatureRef& ref : lefts) { if (!ref.feature.hasGeometry()) continue; QgsFeature indexed = ref.feature; indexed.setId(leftKey); leftIndex.addFeature(indexed); leftMap.insert(leftKey, ref); ++leftKey; } int matchedCount = 0; const double searchTol = qMax(lineTol, endTol) * 3.0;
        auto runCrossScaleSupportCheck = [&]() {
            const double supportSearchTol = qMax(searchTol, buf);
            int checkedSmallScaleCount = 0;
            int supportedSmallScaleCount = 0;
            int weakSupportedSmallScaleCount = 0;
            int unsupportedSmallScaleCount = 0;
            int ambiguousSmallScaleCount = 0;
            QJsonArray supportEvidenceRecords;
            QJsonArray largeUnrepresentedRecords;
            QJsonArray styleRiskRecords;
            QJsonObject styleRiskSummary;
            QJsonObject crossScaleRelationSummary;
            int largeUnrepresentedLogCount = 0;
            int layerBusinessMismatchExcludedCount = 0;
            appendStyleRiskRecords(lefts, QStringLiteral("大比例尺"), leftScaleDenom, styleRiskRecords, styleRiskSummary);
            appendStyleRiskRecords(rights, QStringLiteral("小比例尺"), rightScaleDenom, styleRiskRecords, styleRiskSummary);
            auto bumpRelationClass = [&](const QString& relationClass) {
                if (relationClass.isEmpty()) return;
                crossScaleRelationSummary[relationClass] = crossScaleRelationSummary.value(relationClass).toInt() + 1;
            };
            auto writeCrossScaleSupportPoint = [&](QJsonObject& ext, const QgsGeometry& geom, const SeriesFeatureRef& ref) {
                QgsGeometry pointGeom;
                try {
                    pointGeom = geom.centroid();
                } catch (...) {
                }
                if (pointGeom.isNull() || pointGeom.isEmpty() || QgsWkbTypes::geometryType(pointGeom.wkbType()) != QgsWkbTypes::PointGeometry) return;
                const QgsPointXY pt = pointGeom.asPoint();
                ext["point_x"] = pt.x();
                ext["point_y"] = pt.y();
                ext["point_source"] = QStringLiteral("cross_scale_support_point");
                ext["check_point"] = pointText(pt);
                ext["point_to_left_feature_dist"] = QJsonValue::Null;
                ext["point_to_common_edge_dist"] = (!commonEdge.isNull() && !commonEdge.isEmpty()) ? QJsonValue(QgsGeometry::fromPointXY(pt).distance(commonEdge)) : QJsonValue::Null;
                ext["support_point_shp"] = ref.shpName;
                ext["support_point_fid"] = seriesFeatureId(ref);
            };
            for (const SeriesFeatureRef& smallRef : rights) {
                if (!smallRef.feature.isValid() || !smallRef.feature.hasGeometry()) continue;
                if (!crossScaleRetainRequired(smallRef.feature)) continue;
                const QgsGeometry smallGeom = smallRef.feature.geometry();
                if (smallGeom.isNull() || smallGeom.isEmpty()) continue;
                const auto smallGeomType = QgsWkbTypes::geometryType(smallGeom.wkbType());
                const QList<QgsFeatureId> ids = leftIndex.intersects(smallGeom.boundingBox().buffered(supportSearchTol));
                QJsonArray supportCandidateSamples;
                QJsonArray businessMismatchSamples;
                int supportingCandidateCount = 0;
                int businessMismatchExcludedCount = 0;
                double nearestDistance = std::numeric_limits<double>::max();
                double bestDistance = std::numeric_limits<double>::max();
                double bestScore = -1.0;
                double coveredLength = 0.0;
                double coveredArea = 0.0;
                double longestUncoveredLength = smallGeomType == QgsWkbTypes::LineGeometry ? smallGeom.length() : 0.0;
                QgsGeometry uncoveredGeometry;
                QMap<QString, QVector<QgsGeometry>> coverageMasksByIdentity;
                QMap<QString, int> identityCandidateCounts;
                QMap<QString, SeriesFeatureRef> identityRepresentativeRefs;
                QMap<QString, double> identityRepresentativeScores;
                QMap<QString, double> identityRepresentativeDistances;
                const double positionTolerance = qMax(lineTol,endTol);
                SeriesFeatureRef bestLargeRef;
                const QString smallBusinessClass = crossScaleResolvedBusinessClass(smallRef);
                const QString smallIdentity = crossScaleStableFeatureIdentity(smallRef);
                const bool smallBusinessClassKnown = !smallBusinessClass.isEmpty();
                for (QgsFeatureId id : ids) {
                    if (!leftMap.contains(id)) continue;
                    const SeriesFeatureRef& largeRef = leftMap[id];
                    if (!areSeriesRefsLayerCompatible(largeRef, smallRef)) continue;
                    if (!largeRef.feature.hasGeometry()) continue;
                    const QgsGeometry largeGeom = largeRef.feature.geometry();
                    if (largeGeom.isNull() || largeGeom.isEmpty()) continue;
                    if (QgsWkbTypes::geometryType(largeGeom.wkbType()) != smallGeomType) continue;
                    const double distance = smallGeom.distance(largeGeom);
                    if (distance < 0.0 || distance > supportSearchTol) continue;
                    QString rejectReason;
                    if (!areCrossScaleSupportRefsCompatible(largeRef, smallRef, &rejectReason)) {
                        if (rejectReason != QStringLiteral("aggregate_layer_mismatch")) {
                            ++businessMismatchExcludedCount;
                            ++layerBusinessMismatchExcludedCount;
                            if (businessMismatchSamples.size() < 10) {
                                QJsonObject sample;
                                sample["large_scale_shp"] = largeRef.shpName;
                                sample["large_scale_shp_path"] = largeRef.shpPath;
                                sample["large_scale_fid"] = seriesFeatureId(largeRef);
                                sample["large_scale_standard_layer"] = largeRef.standardLayerName;
                                sample["candidate_business_class"] = crossScaleResolvedBusinessClass(largeRef);
                                sample["small_business_class"] = smallBusinessClass;
                                sample["candidate_feature_identity"] = crossScaleStableFeatureIdentity(largeRef);
                                sample["small_feature_identity"] = smallIdentity;
                                sample["reject_reason"] = rejectReason;
                                sample["distance"] = distance;
                                businessMismatchSamples.append(sample);
                            }
                        }
                        continue;
                    }
                    nearestDistance = qMin(nearestDistance,distance);
                    const QString largeIdentity = crossScaleStableFeatureIdentity(largeRef);
                    const bool largeIdentityUnique = !largeIdentity.isEmpty() && leftStableIdentityCounts.value(largeIdentity) == 1;
                    const bool configuredPolygonIdentityMerge = smallGeomType == QgsWkbTypes::PolygonGeometry
                        && crossScalePolygonAggregationAllowed && !smallIdentity.isEmpty() && largeIdentity == smallIdentity;
                    // Line features may be split into several records at the larger scale. Group matching stable IDs
                    // so their union can support one smaller-scale feature; the smaller-side ID must still be unique.
                    const QString candidateRelationKey = configuredPolygonIdentityMerge
                        ? smallIdentity
                        : (largeIdentity.isEmpty()
                        ? QStringLiteral("unidentified:%1|%2").arg(SeriesLayerMapping::pathKey(largeRef.shpPath), seriesFeatureId(largeRef))
                        : largeIdentity);
                    double supportScore = smallGeom.intersects(largeGeom) ? 100.0
                        : qMax(0.0,100.0*(1.0-distance/qMax(supportSearchTol,1e-9)));
                    if (smallGeomType == QgsWkbTypes::PointGeometry) {
                        supportScore = featurePointTol > 0.0 && distance <= featurePointTol ? 100.0 : 0.0;
                    }
                    if (smallGeomType == QgsWkbTypes::LineGeometry) {
                        const QgsGeometry mask = positionTolerance > 0.0 ? largeGeom.buffer(positionTolerance,8) : largeGeom;
                        if (mask.isNull()) { inputValidationErrors << QStringLiteral("[%1] 线支撑缓冲构建失败").arg(stdName); continue; }
                        QgsGeometry covered = smallGeom.intersection(mask);
                        covered.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
                        const double length = covered.isNull() || covered.isEmpty() ? 0.0 : covered.length();
                        if (length <= 1e-8 || smallGeom.length() <= 1e-8) continue;
                        supportScore = 100.0*qBound(0.0,length/smallGeom.length(),1.0);
                        coverageMasksByIdentity[candidateRelationKey] << mask;
                    } else if (smallGeomType == QgsWkbTypes::PolygonGeometry) {
                        if (largeGeom.isNull() || largeGeom.isEmpty() || smallGeom.area() <= 1e-8) continue;
                        const QgsGeometry actualCovered = smallGeom.intersection(largeGeom);
                        const double area = actualCovered.isNull() || actualCovered.isEmpty() ? 0.0 : actualCovered.area();
                        if (area <= 1e-8) continue;
                        supportScore = 100.0*qBound(0.0,area/smallGeom.area(),1.0);
                        coverageMasksByIdentity[candidateRelationKey] << largeGeom;
                    }
                    nearestDistance = qMin(nearestDistance, distance);
                    ++supportingCandidateCount;
                    ++identityCandidateCounts[candidateRelationKey];
                    if (!identityRepresentativeScores.contains(candidateRelationKey)
                        || supportScore > identityRepresentativeScores.value(candidateRelationKey)
                        || (qFuzzyCompare(supportScore, identityRepresentativeScores.value(candidateRelationKey))
                            && distance < identityRepresentativeDistances.value(candidateRelationKey))) {
                        identityRepresentativeScores[candidateRelationKey] = supportScore;
                        identityRepresentativeDistances[candidateRelationKey] = distance;
                        identityRepresentativeRefs[candidateRelationKey] = largeRef;
                    }
                    if (supportScore > bestScore || (qFuzzyCompare(supportScore, bestScore) && distance < bestDistance)) {
                        bestScore = supportScore;
                        bestDistance = distance;
                        bestLargeRef = largeRef;
                    }
                    if (supportCandidateSamples.size() < 10) {
                        QJsonObject sample;
                        sample["large_scale_shp"] = largeRef.shpName;
                        sample["large_scale_shp_path"] = largeRef.shpPath;
                        sample["large_scale_fid"] = seriesFeatureId(largeRef);
                        sample["large_scale_standard_layer"] = largeRef.standardLayerName;
                        sample["candidate_business_class"] = crossScaleResolvedBusinessClass(largeRef);
                        sample["candidate_identity"] = largeIdentity;
                        sample["candidate_identity_unique_in_scope"] = largeIdentityUnique;
                        sample["relation_group_key"] = candidateRelationKey;
                        sample["distance"] = distance;
                        sample["support_score"] = supportScore;
                        supportCandidateSamples.append(sample);
                    }
                }
                const double bestCandidateScore = bestScore;
                QString bestRelationGroupKey;
                int bestRelationCandidateCount = 0;
                QgsGeometry bestCoverageMask;
                if (smallGeomType == QgsWkbTypes::LineGeometry || smallGeomType == QgsWkbTypes::PolygonGeometry) {
                    bestScore = -1.0;
                    for (auto groupIt = coverageMasksByIdentity.constBegin(); groupIt != coverageMasksByIdentity.constEnd(); ++groupIt) {
                        const QgsGeometry groupMask = QgsGeometry::unaryUnion(groupIt.value());
                        if (groupMask.isNull() || groupMask.isEmpty()) continue;
                        double groupScore = 0.0;
                        if (smallGeomType == QgsWkbTypes::LineGeometry) {
                            QgsGeometry groupCovered = smallGeom.intersection(groupMask);
                            groupCovered.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
                            groupScore = smallGeom.length() > 1e-8
                                ? 100.0 * qBound(0.0, groupCovered.isNull() || groupCovered.isEmpty() ? 0.0 : groupCovered.length(), smallGeom.length()) / smallGeom.length()
                                : 0.0;
                        } else {
                            const QgsGeometry groupCovered = smallGeom.intersection(groupMask);
                            groupScore = smallGeom.area() > 1e-8
                                ? 100.0 * qBound(0.0, groupCovered.isNull() || groupCovered.isEmpty() ? 0.0 : groupCovered.area(), smallGeom.area()) / smallGeom.area()
                                : 0.0;
                        }
                        if (groupScore > bestScore) {
                            bestScore = groupScore;
                            bestRelationGroupKey = groupIt.key();
                            bestRelationCandidateCount = identityCandidateCounts.value(groupIt.key());
                            bestCoverageMask = groupMask;
                            bestLargeRef = identityRepresentativeRefs.value(groupIt.key());
                            bestDistance = identityRepresentativeDistances.value(groupIt.key(), bestDistance);
                        }
                    }
                } else if (bestLargeRef.feature.isValid()) {
                    const QString bestIdentity = crossScaleStableFeatureIdentity(bestLargeRef);
                    bestRelationGroupKey = bestIdentity.isEmpty()
                        ? QStringLiteral("unidentified:%1|%2").arg(SeriesLayerMapping::pathKey(bestLargeRef.shpPath), seriesFeatureId(bestLargeRef))
                        : bestIdentity;
                    bestRelationCandidateCount = identityCandidateCounts.value(bestRelationGroupKey);
                }
                if (smallGeomType == QgsWkbTypes::LineGeometry && !bestCoverageMask.isNull() && !bestCoverageMask.isEmpty()) {
                    const QgsGeometry mask = bestCoverageMask;
                    if (mask.isNull()) inputValidationErrors << QStringLiteral("[%1] 候选联合支撑范围构建失败").arg(stdName);
                    else {
                        QgsGeometry covered = smallGeom.intersection(mask);
                        covered.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
                        coveredLength = covered.isNull() || covered.isEmpty() ? 0.0 : qBound(0.0,covered.length(),smallGeom.length());
                        QgsGeometry uncovered = smallGeom.difference(mask);
                        uncovered.convertGeometryCollectionToSubclass(QgsWkbTypes::LineGeometry);
                            uncoveredGeometry = uncovered;
                        longestUncoveredLength = 0.0;
                        if (!uncovered.isNull() && !uncovered.isEmpty()) {
                            if (!uncovered.isMultipart()) longestUncoveredLength = uncovered.length();
                            else for (const QgsGeometry& part : uncovered.asGeometryCollection()) longestUncoveredLength = qMax(longestUncoveredLength,part.length());
                        }
                    }
                    bestScore = smallGeom.length() > 1e-8 ? 100.0*coveredLength/smallGeom.length() : 0.0;
                }
                if (smallGeomType == QgsWkbTypes::PolygonGeometry && !bestCoverageMask.isNull() && !bestCoverageMask.isEmpty()) {
                    const QgsGeometry mask = bestCoverageMask;
                    if (mask.isNull()) inputValidationErrors << QStringLiteral("[%1] polygon support union failed").arg(stdName);
                    else {
                        const QgsGeometry covered = smallGeom.intersection(mask);
                        coveredArea = covered.isNull() || covered.isEmpty() ? 0.0 : qBound(0.0,covered.area(),smallGeom.area());
                        bestScore = smallGeom.area() > 1e-8 ? 100.0*coveredArea/smallGeom.area() : 0.0;
                        uncoveredGeometry = smallGeom.difference(mask);
                    }
                }
                if (bestCoverageMask.isNull() || bestCoverageMask.isEmpty()) uncoveredGeometry = smallGeom;
                ++checkedSmallScaleCount;
                const double requiredSupportScore = smallGeomType == QgsWkbTypes::PolygonGeometry
                    ? crossScaleOverlapThreshold * 100.0 : crossScaleWeakSupportScore;
                QString relationClass;
                QString smallToLargeRelation;
                QString largeToSmallRelation;
                QString supportStatus;
                QString supportRelation;
                QString relationReason;
                if (supportingCandidateCount == 0) {
                    relationClass = QStringLiteral("0:1");
                    smallToLargeRelation = QStringLiteral("1:0");
                    largeToSmallRelation = QStringLiteral("0:1");
                    supportStatus = QStringLiteral("unsupported");
                    supportRelation = QStringLiteral("no_large_support");
                    relationReason = QStringLiteral("小比例尺有表达，大比例尺侧未找到同类空间支撑");
                } else if (supportingCandidateCount == 1) {
                    relationClass = QStringLiteral("1:1");
                    smallToLargeRelation = QStringLiteral("1:1");
                    largeToSmallRelation = QStringLiteral("1:1");
                    supportStatus = bestScore >= 0.0 && bestScore < requiredSupportScore ? QStringLiteral("weak_supported") : QStringLiteral("supported");
                    supportRelation = QStringLiteral("one_small_to_one_large_candidate");
                    relationReason = supportStatus == QStringLiteral("weak_supported")
                        ? QStringLiteral("小比例尺候选要素找到唯一大比例尺同类支撑，但支撑评分低于弱支撑阈值")
                        : QStringLiteral("小比例尺候选要素找到唯一大比例尺同类支撑");
                } else if (smallGeomType == QgsWkbTypes::LineGeometry || smallGeomType == QgsWkbTypes::PolygonGeometry) {
                    relationClass = QStringLiteral("N:1");
                    smallToLargeRelation = QStringLiteral("1:N");
                    largeToSmallRelation = QStringLiteral("N:1");
                    supportStatus = bestScore >= 0.0 && bestScore < requiredSupportScore ? QStringLiteral("weak_supported") : QStringLiteral("supported");
                    supportRelation = smallGeomType == QgsWkbTypes::PolygonGeometry
                        ? QStringLiteral("large_polygon_union_supports_one_small")
                        : QStringLiteral("many_large_candidates_support_one_small");
                    relationReason = supportStatus == QStringLiteral("weak_supported")
                        ? QStringLiteral("多条大比例尺线要素共同支撑一条小比例尺概括线要素，但最佳支撑评分低于弱支撑阈值")
                        : QStringLiteral("多条大比例尺线要素共同支撑一条小比例尺概括线要素");
                } else {
                    relationClass = QStringLiteral("ambiguous");
                    smallToLargeRelation = QStringLiteral("1:N");
                    largeToSmallRelation = QStringLiteral("N:1");
                    supportStatus = QStringLiteral("ambiguous");
                    supportRelation = QStringLiteral("multiple_large_candidates_not_resolved");
                    relationReason = QStringLiteral("小比例尺候选要素对应多个大比例尺候选，当前几何类型未做聚合支撑判定");
                }
                const bool smallIdentityUnique = !smallIdentity.isEmpty() && rightStableIdentityCounts.value(smallIdentity) == 1;
                const bool stableIdentityMatched = smallIdentityUnique && bestRelationGroupKey == smallIdentity;
                if (supportingCandidateCount > 0 && (!smallBusinessClassKnown || !stableIdentityMatched)) {
                    relationClass = QStringLiteral("ambiguous");
                    smallToLargeRelation = QStringLiteral("1:N");
                    largeToSmallRelation = QStringLiteral("N:1");
                    supportStatus = QStringLiteral("ambiguous");
                    supportRelation = !smallBusinessClassKnown
                        ? QStringLiteral("business_class_unclassified")
                        : QStringLiteral("stable_feature_identity_unresolved");
                    relationReason = !smallBusinessClassKnown
                        ? QStringLiteral("无法从图层或要素属性确认跨比例尺业务类别，空间重叠不足以确认支撑关系")
                        : QStringLiteral("缺少可比的稳定要素标识，候选关系不能仅凭距离或覆盖率确认");
                } else if (supportingCandidateCount > 0) {
                    relationClass = bestRelationCandidateCount > 1 ? QStringLiteral("N:1") : QStringLiteral("1:1");
                    smallToLargeRelation = bestRelationCandidateCount > 1 ? QStringLiteral("1:N") : QStringLiteral("1:1");
                    largeToSmallRelation = bestRelationCandidateCount > 1 ? QStringLiteral("N:1") : QStringLiteral("1:1");
                    if (smallGeomType == QgsWkbTypes::PointGeometry && !featurePointToleranceConfigured) {
                        supportStatus = QStringLiteral("not_evaluable");
                        supportRelation = QStringLiteral("point_position_tolerance_missing");
                        relationReason = QStringLiteral("未配置点要素位置容差，不能判定跨比例尺点支撑");
                    } else if (smallGeomType == QgsWkbTypes::PointGeometry && bestDistance > featurePointTol) {
                        supportStatus = QStringLiteral("weak_supported");
                        supportRelation = QStringLiteral("point_outside_position_tolerance");
                        relationReason = QStringLiteral("同名同类点候选超过位置容差，仅保留为待复核证据");
                    } else if (smallGeomType == QgsWkbTypes::PolygonGeometry && bestRelationCandidateCount > 1
                        && !crossScalePolygonAggregationAllowed) {
                        relationClass = QStringLiteral("ambiguous");
                        smallToLargeRelation = QStringLiteral("1:N");
                        largeToSmallRelation = QStringLiteral("N:1");
                        supportStatus = QStringLiteral("ambiguous");
                        supportRelation = QStringLiteral("polygon_aggregation_rule_missing");
                        relationReason = QStringLiteral("多个大比例尺面合并支撑一个小比例尺面，但未配置面聚合规则");
                    } else if (bestScore >= 0.0 && bestScore < requiredSupportScore) {
                        supportStatus = QStringLiteral("weak_supported");
                    }
                    if (smallGeomType == QgsWkbTypes::LineGeometry) {
                        const double maxGap = crossScaleMaxGapConfigured ? crossScaleMaxGapLength : positionTolerance;
                        if (longestUncoveredLength > maxGap) {
                            supportStatus = QStringLiteral("weak_supported");
                            supportRelation = crossScaleMaxGapConfigured
                                ? QStringLiteral("long_uncovered_segment")
                                : QStringLiteral("long_uncovered_segment_rule_missing");
                            relationReason = QStringLiteral("最长连续未支撑线段超过可用缺口阈值，需复核该具体区段");
                        }
                    }
                }
                bumpRelationClass(relationClass);
                QJsonObject supportEvidence;
                supportEvidence["small_scale_shp"] = smallRef.shpName;
                supportEvidence["small_scale_shp_path"] = smallRef.shpPath;
                supportEvidence["small_scale_fid"] = seriesFeatureId(smallRef);
                supportEvidence["small_scale_standard_layer"] = smallRef.standardLayerName;
                supportEvidence["small_business_class"] = smallBusinessClass;
                supportEvidence["small_feature_identity"] = smallIdentity;
                supportEvidence["small_identity_unique_in_scope"] = smallIdentityUnique;
                supportEvidence["business_class_known"] = smallBusinessClassKnown;
                supportEvidence["configured_cross_scale_attribute_fields"] = QJsonArray::fromStringList(crossScaleAttributeFields);
                supportEvidence["stable_attribute_check_applicable"] = smallIdentityUnique && bestRelationCandidateCount == 1 && bestLargeRef.feature.isValid();
                supportEvidence["small_semantic_attribute"] = layerSemanticAttributeToJson(buildLayerSemanticAttribute(smallRef));
                supportEvidence["geometry_type"] = geomName(smallRef.feature);
                supportEvidence["candidate_count"] = supportingCandidateCount;
                supportEvidence["supporting_candidate_count"] = supportingCandidateCount;
                supportEvidence["candidate_relation_group_count"] = identityCandidateCounts.size();
                supportEvidence["best_relation_group_key"] = bestRelationGroupKey;
                supportEvidence["best_relation_group_candidate_count"] = bestRelationCandidateCount;
                supportEvidence["stable_identity_matched"] = stableIdentityMatched;
                supportEvidence["business_mismatch_excluded_count"] = businessMismatchExcludedCount;
                supportEvidence["business_mismatch_samples"] = businessMismatchSamples;
                supportEvidence["support_search_radius"] = supportSearchTol;
                supportEvidence["weak_support_score_threshold"] = crossScaleWeakSupportScore;
                if (smallGeomType == QgsWkbTypes::LineGeometry) {
                    supportEvidence["score_model"] = QStringLiteral("best_stable_identity_group_union_buffer_line_coverage");
                    supportEvidence["position_tolerance"] = positionTolerance;
                    supportEvidence["checked_length"] = smallGeom.length();
                    supportEvidence["covered_length"] = coveredLength;
                    supportEvidence["uncovered_length"] = qMax(0.0,smallGeom.length()-coveredLength);
                    supportEvidence["longest_uncovered_length"] = longestUncoveredLength;
                    supportEvidence["max_uncovered_segment_length"] = crossScaleMaxGapConfigured ? QJsonValue(crossScaleMaxGapLength) : QJsonValue(positionTolerance);
                    supportEvidence["max_uncovered_segment_rule_configured"] = crossScaleMaxGapConfigured;
                    supportEvidence["support_coverage_ratio"] = smallGeom.length() > 1e-8 ? coveredLength/smallGeom.length() : 0.0;
                    supportEvidence["best_candidate_score"] = bestCandidateScore;
                } else if (smallGeomType == QgsWkbTypes::PolygonGeometry) {
                    supportEvidence["score_model"] = QStringLiteral("best_stable_identity_group_polygon_union_area_coverage");
                    supportEvidence["position_tolerance"] = 0.0;
                    supportEvidence["checked_area"] = smallGeom.area();
                    supportEvidence["covered_area"] = coveredArea;
                    supportEvidence["support_coverage_ratio"] = smallGeom.area() > 1e-8 ? coveredArea/smallGeom.area() : 0.0;
                    supportEvidence["best_candidate_score"] = bestCandidateScore;
                    supportEvidence["weak_support_score_threshold"] = crossScaleOverlapThreshold * 100.0;
                    supportEvidence["polygon_aggregation_rule_configured"] = crossScalePolygonAggregationAllowed;
                } else if (smallGeomType == QgsWkbTypes::PointGeometry) {
                    supportEvidence["score_model"] = QStringLiteral("feature_identity_and_point_position_tolerance");
                    supportEvidence["position_tolerance"] = featurePointToleranceConfigured ? QJsonValue(featurePointTol) : QJsonValue::Null;
                    supportEvidence["position_tolerance_configured"] = featurePointToleranceConfigured;
                }
                supportEvidence["support_candidate_samples"] = supportCandidateSamples;
                supportEvidence["relation_class"] = relationClass;
                supportEvidence["small_to_large_relation"] = smallToLargeRelation;
                supportEvidence["large_to_small_relation"] = largeToSmallRelation;
                supportEvidence["small_expression_semantics"] = buildExpressionSemantics(smallRef, rightScaleDenom);
                if (bestLargeRef.feature.isValid()) {
                    supportEvidence["best_large_scale_shp"] = bestLargeRef.shpName;
                    supportEvidence["best_large_scale_shp_path"] = bestLargeRef.shpPath;
                    supportEvidence["best_large_scale_fid"] = seriesFeatureId(bestLargeRef);
                    supportEvidence["best_large_scale_standard_layer"] = bestLargeRef.standardLayerName;
                    supportEvidence["best_large_business_class"] = crossScaleResolvedBusinessClass(bestLargeRef);
                    supportEvidence["best_large_semantic_attribute"] = layerSemanticAttributeToJson(buildLayerSemanticAttribute(bestLargeRef));
                    supportEvidence["best_distance"] = bestDistance;
                    supportEvidence["best_score"] = bestScore;
                    supportEvidence["best_large_expression_semantics"] = buildExpressionSemantics(bestLargeRef, leftScaleDenom);
                    supportEvidence["semantic_compatibility_status"] = supportStatus == QStringLiteral("ambiguous")
                        ? QStringLiteral("candidate_semantics_need_review")
                        : QStringLiteral("compatible_business_class");
                    supportEvidence["business_compatibility_status"] = QStringLiteral("compatible_business_class");
                } else {
                    supportEvidence["best_large_scale_shp"] = QJsonValue::Null;
                    supportEvidence["best_large_scale_shp_path"] = QJsonValue::Null;
                    supportEvidence["best_large_scale_fid"] = QJsonValue::Null;
                    supportEvidence["best_large_scale_standard_layer"] = QJsonValue::Null;
                    supportEvidence["best_large_business_class"] = QJsonValue::Null;
                    supportEvidence["best_large_semantic_attribute"] = QJsonValue::Null;
                    supportEvidence["best_distance"] = QJsonValue::Null;
                    supportEvidence["best_score"] = QJsonValue::Null;
                    supportEvidence["best_large_expression_semantics"] = QJsonValue::Null;
                    supportEvidence["semantic_compatibility_status"] = QStringLiteral("unsupported_no_semantic_comparison");
                    supportEvidence["business_compatibility_status"] = businessMismatchExcludedCount > 0
                        ? QStringLiteral("only_business_mismatched_candidates")
                        : QStringLiteral("no_spatial_business_candidate");
                }
                supportEvidence["support_status"] = supportStatus;
                supportEvidence["support_relation"] = supportRelation;
                supportEvidence["reason"] = relationReason;
                supportEvidenceRecords.append(supportEvidence);
                append501DebugLog(QStringLiteral("[501] crossScaleSupport pair=%1 layer=%2 small=%3 fid=%4 status=%5 relation=%6 candidates=%7 businessMismatchExcluded=%8 best=%9 bestFid=%10 distance=%11 score=%12")
                    .arg(filePaths.seriesPairName, stdName, smallRef.shpName, seriesFeatureId(smallRef), supportStatus, relationClass)
                    .arg(supportingCandidateCount)
                    .arg(businessMismatchExcludedCount)
                    .arg(bestLargeRef.shpName.isEmpty() ? QStringLiteral("NULL") : bestLargeRef.shpName,
                        bestLargeRef.feature.isValid() ? seriesFeatureId(bestLargeRef) : QStringLiteral("NULL"),
                        bestDistance == std::numeric_limits<double>::max() ? QStringLiteral("NULL") : QString::number(bestDistance, 'f', 3),
                        bestScore < 0.0 ? QStringLiteral("NULL") : QString::number(bestScore, 'f', 1)));
                if (supportStatus == QStringLiteral("supported")) {
                    ++supportedSmallScaleCount;
                    ++matchedCount;
                    if (isCrossScale && !crossScaleAttributeFields.isEmpty() && bestRelationCandidateCount == 1
                        && bestLargeRef.feature.isValid() && stableIdentityMatched) {
                        compareAttributeConsistency(bestLargeRef.feature, bestLargeRef, smallRef.feature, smallRef,
                            crossScaleAttributeFields, stdName);
                        attributeComparisonExecuted = true;
                    }
                    continue;
                }
                if (supportStatus == QStringLiteral("not_evaluable")) {
                    QJsonObject ext;
                    ext["error_type"] = QStringLiteral("cross_scale_support_not_evaluable");
                    ext["decision_status"] = QStringLiteral("not_evaluable");
                    ext["check_method"] = QStringLiteral("small_scale_point_position");
                    ext["support_status"] = supportStatus;
                    ext["support_evidence"] = supportEvidence;
                    ext["support_candidate_samples"] = supportCandidateSamples;
                    ext["candidate_count"] = supportingCandidateCount;
                    ext["match_status"] = QStringLiteral("not_evaluable");
                    ext["match_score"] = QJsonValue::Null;
                    ext["tolerance"] = QJsonValue::Null;
                    ext["match_reason"] = relationReason;
                    if (!smallIdentity.isEmpty()) ext["small_feature_identity"] = smallIdentity;
                    writeCrossScaleSupportPoint(ext, smallGeom, smallRef);
                    addGeometryErr(QStringLiteral("跨比例尺点支撑无法检查"), smallRef.standardLayerName.isEmpty() ? stdName : smallRef.standardLayerName,
                        relationReason, SeriesFeatureRef(), smallRef, ext);
                    continue;
                }
                if (supportStatus == QStringLiteral("weak_supported")) {
                    ++weakSupportedSmallScaleCount;
                    ++matchedCount;
                    QJsonObject ext;
                    ext["error_type"] = QStringLiteral("cross_scale_weak_support");
                    ext["decision_status"] = QStringLiteral("needs_review");
                    ext["check_method"] = QStringLiteral("small_scale_support_score");
                    ext["support_evidence"] = supportEvidence;
                    ext["support_status"] = supportStatus;
                    ext["relation_class"] = relationClass;
                    ext["small_to_large_relation"] = smallToLargeRelation;
                    ext["large_to_small_relation"] = largeToSmallRelation;
                    ext["support_candidate_samples"] = supportCandidateSamples;
                    ext["business_mismatch_excluded_count"] = businessMismatchExcludedCount;
                    ext["business_mismatch_samples"] = businessMismatchSamples;
                    ext["supporting_candidate_count"] = supportingCandidateCount;
                    ext["support_search_radius"] = supportSearchTol;
                    ext["weak_support_score_threshold"] = requiredSupportScore;
                    ext["match_status"] = QStringLiteral("weak_supported");
                    ext["candidate_count"] = supportingCandidateCount;
                    ext["match_score"] = bestScore;
                    if (smallGeomType == QgsWkbTypes::LineGeometry) {
                        ext["field"] = QStringLiteral("支撑覆盖率(%)");
                        ext["right_value"] = bestScore;
                        ext["expected_value"] = crossScaleWeakSupportScore;
                        ext["tolerance"] = positionTolerance;
                    } else if (smallGeomType == QgsWkbTypes::PolygonGeometry) {
                        ext["field"] = QStringLiteral("support_coverage_ratio");
                        ext["right_value"] = bestScore / 100.0;
                        ext["expected_value"] = crossScaleOverlapThreshold;
                        ext["tolerance"] = positionTolerance;
                    }
                    ext["match_reason"] = relationReason;
                    ext["confidence"] = QStringLiteral("low");
                    if (bestLargeRef.feature.isValid()) {
                        ext["best_large_scale_shp"] = bestLargeRef.shpName;
                        ext["best_large_scale_fid"] = seriesFeatureId(bestLargeRef);
                        ext["best_distance"] = bestDistance;
                        ext["best_score"] = bestScore;
                    }
                    if (!uncoveredGeometry.isNull() && !uncoveredGeometry.isEmpty()) ext["right_check_wkt"] = uncoveredGeometry.asWkt();
                    writeCrossScaleSupportPoint(ext, uncoveredGeometry.isNull() || uncoveredGeometry.isEmpty() ? smallGeom : uncoveredGeometry, smallRef);
                    addGeometryErr(QStringLiteral("跨比例尺弱支撑"), smallRef.standardLayerName.isEmpty() ? stdName : smallRef.standardLayerName, QStringLiteral("小比例尺候选要素虽找到同类支撑，但支撑评分低于阈值，疑似空间偏移或弱支撑"), SeriesFeatureRef(), smallRef, ext);
                    continue;
                }
                if (supportStatus == QStringLiteral("ambiguous")) {
                    ++ambiguousSmallScaleCount;
                    QJsonObject ext;
                    ext["error_type"] = QStringLiteral("cross_scale_candidate_ambiguous");
                    ext["decision_status"] = QStringLiteral("needs_review");
                    ext["check_method"] = QStringLiteral("small_scale_support_relation");
                    ext["support_evidence"] = supportEvidence;
                    ext["support_status"] = supportStatus;
                    ext["relation_class"] = relationClass;
                    ext["small_to_large_relation"] = smallToLargeRelation;
                    ext["large_to_small_relation"] = largeToSmallRelation;
                    ext["support_candidate_samples"] = supportCandidateSamples;
                    ext["business_mismatch_excluded_count"] = businessMismatchExcludedCount;
                    ext["business_mismatch_samples"] = businessMismatchSamples;
                    ext["supporting_candidate_count"] = supportingCandidateCount;
                    ext["support_search_radius"] = supportSearchTol;
                    ext["match_status"] = QStringLiteral("ambiguous");
                    ext["candidate_count"] = supportingCandidateCount;
                    if (bestScore < 0.0) ext["match_score"] = QJsonValue::Null;
                    else ext["match_score"] = bestScore;
                    ext["match_reason"] = relationReason;
                    ext["confidence"] = QStringLiteral("medium");
                    writeCrossScaleSupportPoint(ext, smallGeom, smallRef);
                    addGeometryErr(QStringLiteral("跨比例尺候选匹配"), smallRef.standardLayerName.isEmpty() ? stdName : smallRef.standardLayerName, QStringLiteral("小比例尺候选要素对应多个大比例尺候选，无法唯一确认支撑关系"), SeriesFeatureRef(), smallRef, ext);
                    continue;
                }
                ++unsupportedSmallScaleCount;
                const bool onlyBusinessMismatchedCandidates = businessMismatchExcludedCount > 0;
                QJsonObject ext;
                ext["error_type"] = onlyBusinessMismatchedCandidates
                    ? QStringLiteral("cross_scale_actual_layer_semantic_mismatch")
                    : QStringLiteral("cross_scale_support_missing");
                ext["decision_status"] = QStringLiteral("needs_review");
                ext["check_method"] = onlyBusinessMismatchedCandidates
                    ? QStringLiteral("cross_scale_actual_layer_name_semantic_support")
                    : QStringLiteral("small_scale_support");
                ext["attribute_source"] = QStringLiteral("actual_layer_name");
                ext["supporting_candidate_count"] = 0;
                ext["support_candidate_samples"] = supportCandidateSamples;
                ext["business_mismatch_excluded_count"] = businessMismatchExcludedCount;
                ext["business_mismatch_samples"] = businessMismatchSamples;
                ext["rejected_candidate_count"] = businessMismatchExcludedCount;
                ext["compatible_candidate_count"] = 0;
                ext["support_evidence"] = supportEvidence;
                ext["support_status"] = QStringLiteral("unsupported");
                ext["relation_class"] = relationClass;
                ext["small_to_large_relation"] = smallToLargeRelation;
                ext["large_to_small_relation"] = largeToSmallRelation;
                ext["best_large_scale_shp"] = QJsonValue::Null;
                ext["best_large_scale_fid"] = QJsonValue::Null;
                ext["best_distance"] = QJsonValue::Null;
                ext["best_score"] = QJsonValue::Null;
                ext["support_search_radius"] = supportSearchTol;
                if (nearestDistance == std::numeric_limits<double>::max()) {
                    ext["distance"] = QJsonValue::Null;
                } else {
                    ext["distance"] = nearestDistance;
                }
                ext["tolerance"] = supportSearchTol;
                ext["match_status"] = onlyBusinessMismatchedCandidates ? QStringLiteral("business_mismatch") : QStringLiteral("unsupported");
                ext["candidate_count"] = onlyBusinessMismatchedCandidates ? businessMismatchExcludedCount : 0;
                ext["match_score"] = QJsonValue::Null;
                ext["match_reason"] = onlyBusinessMismatchedCandidates
                    ? QStringLiteral("小比例尺表达要素附近存在空间候选，但实际图层名反映的业务类别/属性等级不一致，未找到同类支撑")
                    : QStringLiteral("小比例尺表达要素在大比例尺候选集合中未找到同类支撑");
                ext["confidence"] = QStringLiteral("medium");
                if (!uncoveredGeometry.isNull() && !uncoveredGeometry.isEmpty()) ext["right_check_wkt"] = uncoveredGeometry.asWkt();
                writeCrossScaleSupportPoint(ext, uncoveredGeometry.isNull() || uncoveredGeometry.isEmpty() ? smallGeom : uncoveredGeometry, smallRef);
                addGeometryErr(
                    onlyBusinessMismatchedCandidates ? QStringLiteral("跨比例尺属性等级一致性") : QStringLiteral("跨比例尺表达支撑"),
                    smallRef.standardLayerName.isEmpty() ? stdName : smallRef.standardLayerName,
                    onlyBusinessMismatchedCandidates
                        ? QStringLiteral("小比例尺候选要素附近存在大比例尺空间候选，但实际图层名业务类别/属性等级不一致，不能作为同类支撑")
                        : QStringLiteral("小比例尺候选要素在大比例尺覆盖范围内未找到同类支撑要素"),
                    SeriesFeatureRef(), smallRef, ext);
            }
            for (const SeriesFeatureRef& largeRef : lefts) {
                if (!largeRef.feature.isValid() || !largeRef.feature.hasGeometry()) continue;
                const QgsGeometry largeGeom = largeRef.feature.geometry();
                if (largeGeom.isNull() || largeGeom.isEmpty()) continue;
                const auto largeGeomType = QgsWkbTypes::geometryType(largeGeom.wkbType());
                const QList<QgsFeatureId> ids = rightIndex.intersects(largeGeom.boundingBox().buffered(supportSearchTol));
                int smallCandidateCount = 0;
                double nearestSmallDistance = std::numeric_limits<double>::max();
                for (QgsFeatureId id : ids) {
                    if (!rightMap.contains(id)) continue;
                    const SeriesFeatureRef& smallRef = rightMap[id];
                    if (!areCrossScaleSupportRefsCompatible(largeRef, smallRef)) continue;
                    if (!smallRef.feature.hasGeometry()) continue;
                    QgsGeometry smallGeom = simplifyCrossScaleRightGeometry(smallRef.feature.geometry());
                    if (smallGeom.isNull() || smallGeom.isEmpty()) continue;
                    if (QgsWkbTypes::geometryType(smallGeom.wkbType()) != largeGeomType) continue;
                    const double distance = largeGeom.distance(smallGeom);
                    if (distance > supportSearchTol && !largeGeom.intersects(smallGeom)) continue;
                    nearestSmallDistance = qMin(nearestSmallDistance, distance);
                    ++smallCandidateCount;
                }
                if (smallCandidateCount > 0) continue;
                bumpRelationClass(QStringLiteral("1:0"));
                QJsonObject record;
                record["large_scale_shp"] = largeRef.shpName;
                record["large_scale_shp_path"] = largeRef.shpPath;
                record["large_scale_fid"] = seriesFeatureId(largeRef);
                record["large_scale_standard_layer"] = largeRef.standardLayerName;
                record["geometry_type"] = geomName(largeRef.feature);
                record["relation_class"] = QStringLiteral("1:0");
                record["small_to_large_relation"] = QStringLiteral("0:1");
                record["large_to_small_relation"] = QStringLiteral("1:0");
                record["support_status"] = QStringLiteral("large_unrepresented");
                record["candidate_count"] = 0;
                record["support_search_radius"] = supportSearchTol;
                if (nearestSmallDistance == std::numeric_limits<double>::max()) record["nearest_small_distance"] = QJsonValue::Null;
                else record["nearest_small_distance"] = nearestSmallDistance;
                record["reason"] = QStringLiteral("大比例尺有要素，小比例尺侧未找到对应表达；默认仅作为诊断证据，不直接判错");
                largeUnrepresentedRecords.append(record);
                ++largeUnrepresentedLogCount;
            }
            if (largeUnrepresentedLogCount > 0) {
                append501DebugLog(QStringLiteral("[501] crossScaleLargeUnrepresentedSummary pair=%1 layer=%2 count=%3 status=large_unrepresented_summary relation=1:0 output=diagnostic_only")
                    .arg(filePaths.seriesPairName, stdName).arg(largeUnrepresentedLogCount));
            }
            QJsonObject stat;
            stat["left_candidates"] = lefts.size();
            stat["right_candidates"] = rights.size();
            stat["large_scale_candidates"] = lefts.size();
            stat["small_scale_candidates"] = rights.size();
            stat["small_scale_checked"] = checkedSmallScaleCount;
            stat["small_scale_supported"] = supportedSmallScaleCount;
            stat["small_scale_weak_supported"] = weakSupportedSmallScaleCount;
            stat["small_scale_unsupported"] = unsupportedSmallScaleCount;
            stat["small_scale_ambiguous"] = ambiguousSmallScaleCount;
            stat["matched_candidates"] = matchedCount;
            stat["execution_state"] = checkedSmallScaleCount > 0 ? QStringLiteral("completed") : QStringLiteral("no_applicable_support_features");
            stat["match_model"] = QStringLiteral("cross_scale_support_set");
            stat["support_evidence_records"] = supportEvidenceRecords;
            stat["large_unrepresented_records"] = largeUnrepresentedRecords;
            stat["cross_scale_relation_summary"] = crossScaleRelationSummary;
            stat["business_mismatch_excluded_count"] = layerBusinessMismatchExcludedCount;
            stat["style_risk_status"] = QStringLiteral("diagnostic_only");
            stat["style_risk_records"] = styleRiskRecords;
            stat["style_risk_summary"] = styleRiskSummary;
            geometrySummary[stdName] = stat;
            geometryTotals["left_candidates"] = geometryTotals.value("left_candidates").toInt() + lefts.size();
            geometryTotals["right_candidates"] = geometryTotals.value("right_candidates").toInt() + rights.size();
            geometryTotals["large_scale_candidates"] = geometryTotals.value("large_scale_candidates").toInt() + lefts.size();
            geometryTotals["small_scale_candidates"] = geometryTotals.value("small_scale_candidates").toInt() + rights.size();
            geometryTotals["small_scale_checked"] = geometryTotals.value("small_scale_checked").toInt() + checkedSmallScaleCount;
            geometryTotals["small_scale_supported"] = geometryTotals.value("small_scale_supported").toInt() + supportedSmallScaleCount;
            geometryTotals["small_scale_weak_supported"] = geometryTotals.value("small_scale_weak_supported").toInt() + weakSupportedSmallScaleCount;
            geometryTotals["small_scale_unsupported"] = geometryTotals.value("small_scale_unsupported").toInt() + unsupportedSmallScaleCount;
            geometryTotals["small_scale_ambiguous"] = geometryTotals.value("small_scale_ambiguous").toInt() + ambiguousSmallScaleCount;
            geometryTotals["large_scale_unrepresented"] = geometryTotals.value("large_scale_unrepresented").toInt() + largeUnrepresentedRecords.size();
            geometryTotals["business_mismatch_excluded_count"] = geometryTotals.value("business_mismatch_excluded_count").toInt() + layerBusinessMismatchExcludedCount;
            geometryTotals["style_risk_count"] = geometryTotals.value("style_risk_count").toInt() + styleRiskRecords.size();
            geometryTotals["matched_candidates"] = geometryTotals.value("matched_candidates").toInt() + matchedCount;
        };
        if (isCrossScale) {
            runCrossScaleSupportCheck();
            return;
        }
        auto nonLineSpatialScore = [&](const QgsGeometry& leftGeom, const QgsGeometry& rightGeom, QgsWkbTypes::GeometryType geomType, double distance) {
            if (leftGeom.isNull() || leftGeom.isEmpty() || rightGeom.isNull() || rightGeom.isEmpty()) return 0.0;
            if (geomType == QgsWkbTypes::PointGeometry) {
                if (distance > searchTol) return 0.0;
                return qMax(0.0, 70.0 * (1.0 - distance / qMax(searchTol, 1e-9)));
            }
            if (geomType == QgsWkbTypes::PolygonGeometry) {
                double overlapScore = 0.0;
                try {
                    const QgsGeometry overlap = leftGeom.intersection(rightGeom);
                    const double minArea = qMin(leftGeom.area(), rightGeom.area());
                    if (!overlap.isNull() && !overlap.isEmpty() && minArea > 0.0) {
                        overlapScore = qBound(0.0, overlap.area() / minArea, 1.0) * 70.0;
                    }
                } catch (...) {
                }
                if (overlapScore > 0.0) return overlapScore;
                if (distance > searchTol) return 0.0;
                return qMax(0.0, 45.0 * (1.0 - distance / qMax(searchTol, 1e-9)));
            }
            return 0.0;
        };
        auto matchNonLinePair = [&](const SeriesFeatureRef& leftRef, SeriesFeatureRef& rightRef, double& nearestDistance, bool& ambiguous, QJsonArray* candidateArray = nullptr) {
            ambiguous = false;
            const QgsGeometry leftGeom = leftRef.feature.geometry();
            const auto geomType = QgsWkbTypes::geometryType(leftGeom.wkbType());
            const QList<QgsFeatureId> ids = rightIndex.intersects(leftGeom.boundingBox().buffered(searchTol));
            double bestScore = -1.0;
            double secondBestScore = -1.0;
            int comparableCount = 0;
            SeriesFeatureRef bestRef;
            for (QgsFeatureId id : ids) {
                if (!rightMap.contains(id)) continue;
                const SeriesFeatureRef& candRef = rightMap[id];
                if (!areSeriesRefsLayerCompatible(leftRef, candRef)) continue;
                const QgsGeometry candGeom = candRef.feature.geometry();
                if (QgsWkbTypes::geometryType(candGeom.wkbType()) != geomType) continue;
                const double dist = leftGeom.distance(candGeom);
                if (dist > searchTol && !leftGeom.intersects(candGeom)) continue;
                bool isStrongMatch = false;
                QString matchReason;
                QString confidence;
                const double attrScore = calculateMatchScore(leftRef, candRef, isStrongMatch, matchReason, confidence);
                const double spatialScore = nonLineSpatialScore(leftGeom, candGeom, geomType, dist);
                const double score = attrScore + spatialScore;
                if (score < 35.0) continue;
                ++comparableCount;
                if (candidateArray) {
                    QJsonObject c;
                    c["right_shp"] = candRef.shpName;
                    c["right_shp_path"] = candRef.shpPath;
                    c["right_fid"] = seriesFeatureId(candRef);
                    c["distance"] = dist;
                    c["score"] = score;
                    c["attribute_score"] = attrScore;
                    c["spatial_score"] = spatialScore;
                    c["reason"] = spatialScore > 0.0 ? QStringLiteral("%1；空间评分 %2").arg(matchReason, QString::number(spatialScore, 'f', 1)) : matchReason;
                    c["confidence"] = score >= 80.0 ? QStringLiteral("high") : (score >= 55.0 ? QStringLiteral("medium") : confidence);
                    candidateArray->append(c);
                }
                if (score > bestScore) {
                    secondBestScore = bestScore;
                    bestScore = score;
                    nearestDistance = dist;
                    bestRef = candRef;
                } else if (score > secondBestScore) {
                    secondBestScore = score;
                }
            }
            ambiguous = comparableCount > 1 && (bestScore < 80.0 || (bestScore - secondBestScore) < 10.0);
            if (comparableCount == 0 || ambiguous) return false;
            rightRef = bestRef;
            return true;
        };
        for (const SeriesFeatureRef& leftRef : lefts) {
            const QgsFeature& leftF = leftRef.feature; const auto geomType = QgsWkbTypes::geometryType(leftF.geometry().wkbType());
            QgsFeature rightF; SeriesFeatureRef rightRef; double nearestDistance = -1.0; const QString shpText = shpNames.join(QStringLiteral("；"));
            if (geomType != QgsWkbTypes::LineGeometry) {
                bool ambiguous = false;
                QJsonArray candidateArray;
                if (!matchNonLinePair(leftRef, rightRef, nearestDistance, ambiguous, &candidateArray)) {
                    continue;
                }
                rightF = rightRef.feature;
                ++matchedCount;
                compareAttributeConsistency(leftF, leftRef, rightF, rightRef, attributeFields, stdName);
                compareLayerSemanticAttributeConsistency(leftRef, rightRef, stdName);
                compareStyleConsistency(leftF, leftRef, rightF, rightRef, stdName);
                continue;
            }
            QgsGeometry leftEdgeGeom; try { leftEdgeGeom = leftF.geometry().intersection(edgeZone); }
            catch (...) {}
            if (leftEdgeGeom.isNull() || leftEdgeGeom.isEmpty()) continue;
            const QList<QgsPointXY> leftCrossPts = collectSeriesCommonEdgeCrossPoints(leftF.geometry(), commonEdge);
            const QList<SeriesNamedPoint> leftEndPts = collectOriginalEndpoints(leftF.geometry(), edgeZone, commonEdge, searchTol);
            const QList<QgsPointXY> leftAnchorPts = !leftCrossPts.isEmpty() ? leftCrossPts : namedSeriesPoints(leftEndPts);
            const QList<QgsFeatureId> candidateIds = rightIndex.intersects(leftEdgeGeom.boundingBox().buffered(searchTol));
            double bestScore = -1.0;
            bool foundMatch = false;
            int comparableCandidateCount = 0;
            QJsonArray rightFeatureCandidates;
            for (QgsFeatureId id : candidateIds) {
                if (!rightMap.contains(id)) continue; const SeriesFeatureRef& candRef = rightMap[id]; const QgsFeature& cand = candRef.feature; const QgsGeometry candGeometryForCheck = simplifyCrossScaleRightGeometry(cand.geometry()); QgsGeometry candEdgeGeom; try { candEdgeGeom = candGeometryForCheck.intersection(edgeZone); }
                catch (...) {}
                if (candEdgeGeom.isNull() || candEdgeGeom.isEmpty()) continue; const double edgeDistance = leftEdgeGeom.distance(candEdgeGeom); if (edgeDistance > searchTol) continue;
                const QList<QgsPointXY> candCrossPts = collectSeriesCommonEdgeCrossPoints(candGeometryForCheck, commonEdge);
                const QList<SeriesNamedPoint> candEndPts = collectOriginalEndpoints(candGeometryForCheck, edgeZone, commonEdge, searchTol);
                const QList<QgsPointXY> candAnchorPts = !candCrossPts.isEmpty() ? candCrossPts : namedSeriesPoints(candEndPts);
                QgsPointXY anchorL, anchorR; double anchorDistance = -1.0;
                if (leftAnchorPts.isEmpty() || candAnchorPts.isEmpty() || !findNearestSeriesPointPair(leftAnchorPts, candAnchorPts, anchorL, anchorR, anchorDistance) || anchorDistance > searchTol) continue;
                if (!areSeriesRefsLayerCompatible(leftRef, candRef)) { append501DebugLog(QStringLiteral("[501] excluded mismatched candidate pair=%1 layer=%2 leftStd=%3 rightStd=%4 leftLayer=%5 rightLayer=%6").arg(filePaths.seriesPairName, stdName, leftRef.standardLayerName, candRef.standardLayerName, normalizeSeriesLayerBaseName(leftRef.shpName), normalizeSeriesLayerBaseName(candRef.shpName))); continue; }
                bool isStrongMatch = false; QString matchReason; QString confidence; const double attrScore = calculateMatchScore(leftRef, candRef, isStrongMatch, matchReason, confidence);
                const double anchorScore = qMax(0.0, 45.0 * (1.0 - anchorDistance / qMax(searchTol, 1e-9)));
                const double edgeScore = qMax(0.0, 25.0 * (1.0 - edgeDistance / qMax(searchTol, 1e-9)));
                const double matchScore = attrScore + anchorScore + edgeScore;
                if (matchScore < 20.0) continue;
                ++comparableCandidateCount; QJsonObject candObj; candObj["right_shp"] = candRef.shpName; candObj["right_shp_path"] = candRef.shpPath; candObj["right_fid"] = seriesFeatureId(candRef); candObj["distance"] = edgeDistance; candObj["score"] = matchScore; candObj["attribute_score"] = attrScore; candObj["anchor_score"] = anchorScore; candObj["edge_score"] = edgeScore; candObj["reason"] = QStringLiteral("%1；锚点评分 %2；线段评分 %3").arg(matchReason, QString::number(anchorScore, 'f', 1), QString::number(edgeScore, 'f', 1)); candObj["confidence"] = matchScore >= 90.0 ? QStringLiteral("high") : (matchScore >= 60.0 ? QStringLiteral("medium") : confidence); rightFeatureCandidates.append(candObj);
                if (matchScore > bestScore) { bestScore = matchScore; nearestDistance = edgeDistance; rightF = cand; rightRef = candRef; foundMatch = true; }
            }
            if (comparableCandidateCount > 1 && bestScore < 80.0) { QJsonObject ext; ext["error_type"] = QStringLiteral("feature_ambiguous"); ext["right_feature_candidates"] = rightFeatureCandidates; attachMatchEvidence(ext, leftRef, SeriesFeatureRef(), QStringLiteral("ambiguous"), comparableCandidateCount); addGeometryErr(QStringLiteral("配置/匹配问题"), stdName, QStringLiteral("右侧接边候选不唯一"), leftRef, SeriesFeatureRef(), ext); continue; }
            if (!foundMatch) {
                if (isCrossScale && !crossScaleRetainRequired(leftF)) continue;
                QgsPointXY pt; QString pointSource, endpointType;
                if (!pickSeriesMissingErrorPoint(leftRef, commonEdge, searchTol, pt, pointSource, endpointType)) continue;
                double pointDistance = -1.0;
                if (!isSeriesPointOnFeature(pt, leftF.geometry(), 1e-7, &pointDistance)) { QJsonObject ext; ext["actual_shp"] = shpText; ext["point_source"] = pointSource; ext["point_to_left_feature_dist"] = pointDistance; ext["point_to_common_edge_dist"] = QgsGeometry::fromPointXY(pt).distance(commonEdge); ext["search_radius"] = searchTol; addGeometryErr(QStringLiteral("算法异常"), stdName, QStringLiteral("接边要素缺失定位点与左侧要素不一致"), leftRef, SeriesFeatureRef(), ext); continue; }
                QJsonObject ext; ext["actual_shp"] = shpText; ext["error_type"] = QStringLiteral("feature_unmatched"); ext["distance"] = QJsonValue::Null; ext["tolerance"] = QJsonValue::Null; ext["search_radius"] = searchTol; ext["check_method"] = pointSource == QStringLiteral("common_edge_cross") ? QStringLiteral("公共边交点") : QStringLiteral("真实端点"); if (!endpointType.isEmpty()) ext["endpoint_type"] = endpointType; attachMatchEvidence(ext, leftRef, SeriesFeatureRef(), QStringLiteral("unmatched"), comparableCandidateCount); writeSeriesPointDiagnostics(ext, pt, pointSource, leftRef, commonEdge);
                addGeometryErr(QStringLiteral("接边要素缺失"), stdName, QStringLiteral("左侧接边线要素在右侧图幅中未找到对应接边要素"), leftRef, SeriesFeatureRef(), ext); continue;
            }
            const QgsGeometry rightGeometryForCheck = simplifyCrossScaleRightGeometry(rightF.geometry());
            QgsGeometry rightEdgeGeom; try { rightEdgeGeom = rightGeometryForCheck.intersection(edgeZone); }
            catch (...) {}
            if (rightEdgeGeom.isNull() || rightEdgeGeom.isEmpty()) continue;
            ++matchedCount;
            auto attachLineBestEvidence = [&](QJsonObject& ext) {
                ext["match_status"] = QStringLiteral("matched");
                ext["candidate_count"] = comparableCandidateCount;
                ext["match_score"] = bestScore;
                ext["match_reason"] = QStringLiteral("线候选综合评分：属性、锚点距离和线段距离");
                ext["confidence"] = bestScore >= 90.0 ? QStringLiteral("high") : (bestScore >= 60.0 ? QStringLiteral("medium") : QStringLiteral("low"));
            };
            if (nearestDistance > lineTol) {
                const QgsPointXY leftPt = leftEdgeGeom.nearestPoint(rightEdgeGeom).asPoint();
                const QgsPointXY rightPt = rightEdgeGeom.nearestPoint(leftEdgeGeom).asPoint();
                QJsonObject ext;
                ext["actual_shp"] = shpText;
                ext["distance"] = nearestDistance;
                ext["tolerance"] = lineTol;
                ext["search_radius"] = searchTol;
                ext["check_method"] = QStringLiteral("接边缓冲区线段");
                attachLineBestEvidence(ext);
                writeSeriesPointDiagnostics(ext, leftPt, QStringLiteral("left_nearest_point"), leftRef, commonEdge);
                writeSeriesLineDiagnostics(ext, leftPt, rightPt, QStringLiteral("nearest_points"));
                addGeometryErr(QStringLiteral("线要素接边容差"), stdName, QStringLiteral("左右接边缓冲区线段距离超过线要素接边容差"), leftRef, rightRef, ext);
            }
            const QList<SeriesNamedPoint> rightEndPts = collectOriginalEndpoints(rightGeometryForCheck, edgeZone, commonEdge, searchTol);
            for (const SeriesNamedPoint& leftEndPt : leftEndPts) {
                double bestDistance = std::numeric_limits<double>::max();
                double bestProjectionGap = -1.0;
                SeriesNamedPoint bestRightEndPt;
                bool foundEndMatch = false;
                for (const SeriesNamedPoint& rightEndPt : rightEndPts) {
                    double projectionGap = -1.0;
                    if (!isSeriesEndpointPairEligible(leftRef, rightRef, leftEndPt, rightEndPt, commonEdge, searchTol, &projectionGap)) continue;
                    const double d = QgsGeometry::fromPointXY(leftEndPt.point).distance(QgsGeometry::fromPointXY(rightEndPt.point));
                    if (d < bestDistance) {
                        bestDistance = d;
                        bestProjectionGap = projectionGap;
                        bestRightEndPt = rightEndPt;
                        foundEndMatch = true;
                    }
                }
                if (foundEndMatch && bestDistance > endTol) {
                    QJsonObject ext;
                    ext["actual_shp"] = shpText;
                    ext["endpoint_type"] = leftEndPt.name;
                    ext["check_method"] = QStringLiteral("真实端点");
                    ext["distance"] = bestDistance;
                    ext["tolerance"] = endTol;
                    ext["search_radius"] = searchTol;
                    ext["projection_gap"] = bestProjectionGap;
                    attachLineBestEvidence(ext);
                    writeSeriesPointDiagnostics(ext, leftEndPt.point, QStringLiteral("left_endpoint"), leftRef, commonEdge);
                    writeSeriesLineDiagnostics(ext, leftEndPt.point, bestRightEndPt.point, QStringLiteral("endpoint_pair"));
                    addGeometryErr(QStringLiteral("端点闭合容差"), stdName, QStringLiteral("左侧真实端点与右侧真实端点距离超过端点闭合容差"), leftRef, rightRef, ext);
                }
            }
            compareAttributeConsistency(leftF, leftRef, rightF, rightRef, attributeFields, stdName);
            compareLayerSemanticAttributeConsistency(leftRef, rightRef, stdName);
            compareStyleConsistency(leftF, leftRef, rightF, rightRef, stdName);
        }
        auto runMissingDirection = [&](const QList<SeriesFeatureRef>& sources, const QgsSpatialIndex& targetIndex, const QMap<qint64, SeriesFeatureRef>& targetMap, const QString& sourceSide, const QString& missingSide) {
            for (const SeriesFeatureRef& sourceRef : sources) {
                if (!sourceRef.feature.isValid() || !sourceRef.feature.hasGeometry()) continue;
                if (isCrossScale && !crossScaleRetainRequired(sourceRef.feature)) continue;
                const QgsGeometry sourceGeom = sourceRef.feature.geometry();
                const auto geomType = QgsWkbTypes::geometryType(sourceGeom.wkbType());
                bool hasCandidate = false;
                if (geomType != QgsWkbTypes::LineGeometry) {
                    const QList<QgsFeatureId> ids = targetIndex.intersects(sourceGeom.boundingBox().buffered(searchTol));
                    for (QgsFeatureId id : ids) {
                        if (!targetMap.contains(id)) continue;
                        const SeriesFeatureRef& candRef = targetMap[id];
                        if (!(sourceSide == QStringLiteral("左") ? areSeriesRefsLayerCompatible(sourceRef, candRef)
                            : areSeriesRefsLayerCompatible(candRef, sourceRef))) continue;
                        const QgsGeometry candGeom = candRef.feature.geometry();
                        if (QgsWkbTypes::geometryType(candGeom.wkbType()) != geomType) continue;
                        const double dist = sourceGeom.distance(candGeom);
                        if (dist > searchTol && !sourceGeom.intersects(candGeom)) continue;
                        bool isStrongMatch = false;
                        QString matchReason;
                        QString confidence;
                        const double attrScore = calculateMatchScore(sourceRef, candRef, isStrongMatch, matchReason, confidence);
                        const double spatialScore = nonLineSpatialScore(sourceGeom, candGeom, geomType, dist);
                        if (attrScore + spatialScore >= 35.0) {
                            hasCandidate = true;
                            break;
                        }
                    }
                    if (hasCandidate) continue;
                    continue;
                }

                QgsGeometry sourceEdgeGeom;
                try { sourceEdgeGeom = sourceGeom.intersection(edgeZone); } catch (...) {}
                if (sourceEdgeGeom.isNull() || sourceEdgeGeom.isEmpty()) continue;
                const QList<QgsPointXY> sourceCrossPts = collectSeriesCommonEdgeCrossPoints(sourceGeom, commonEdge);
                const QList<SeriesNamedPoint> sourceEndPts = collectOriginalEndpoints(sourceGeom, edgeZone, commonEdge, searchTol);
                const QList<QgsPointXY> sourceAnchorPts = !sourceCrossPts.isEmpty() ? sourceCrossPts : namedSeriesPoints(sourceEndPts);
                const QList<QgsFeatureId> ids = targetIndex.intersects(sourceEdgeGeom.boundingBox().buffered(searchTol));
                for (QgsFeatureId id : ids) {
                    if (!targetMap.contains(id)) continue;
                    const SeriesFeatureRef& candRef = targetMap[id];
                    const QgsGeometry candGeom = simplifyCrossScaleRightGeometry(candRef.feature.geometry());
                    if (QgsWkbTypes::geometryType(candGeom.wkbType()) != QgsWkbTypes::LineGeometry) continue;
                    QgsGeometry candEdgeGeom;
                    try { candEdgeGeom = candGeom.intersection(edgeZone); } catch (...) {}
                    if (candEdgeGeom.isNull() || candEdgeGeom.isEmpty()) continue;
                    const double edgeDistance = sourceEdgeGeom.distance(candEdgeGeom);
                    if (edgeDistance > searchTol) continue;
                    if (!(sourceSide == QStringLiteral("左") ? areSeriesRefsLayerCompatible(sourceRef, candRef)
                        : areSeriesRefsLayerCompatible(candRef, sourceRef))) continue;
                    const QList<QgsPointXY> candCrossPts = collectSeriesCommonEdgeCrossPoints(candGeom, commonEdge);
                    const QList<SeriesNamedPoint> candEndPts = collectOriginalEndpoints(candGeom, edgeZone, commonEdge, searchTol);
                    const QList<QgsPointXY> candAnchorPts = !candCrossPts.isEmpty() ? candCrossPts : namedSeriesPoints(candEndPts);
                    QgsPointXY anchorSource, anchorTarget;
                    double anchorDistance = -1.0;
                    if (!sourceAnchorPts.isEmpty() && !candAnchorPts.isEmpty() && findNearestSeriesPointPair(sourceAnchorPts, candAnchorPts, anchorSource, anchorTarget, anchorDistance) && anchorDistance <= searchTol) {
                        hasCandidate = true;
                        break;
                    }
                }
                if (hasCandidate) continue;
                QgsPointXY pt;
                QString pointSource, endpointType;
                if (!pickSeriesMissingErrorPoint(sourceRef, commonEdge, searchTol, pt, pointSource, endpointType)) continue;
                QJsonObject ext;
                ext["error_type"] = QStringLiteral("feature_unmatched");
                ext["reverse_missing_direction"] = true;
                ext["source_side"] = sourceSide;
                ext["missing_side"] = missingSide;
                ext["match_status"] = QStringLiteral("unmatched");
                ext["candidate_count"] = 0;
                ext["search_radius"] = searchTol;
                ext["check_method"] = pointSource == QStringLiteral("common_edge_cross") ? QStringLiteral("公共边交点") : QStringLiteral("真实端点");
                if (!endpointType.isEmpty()) ext["endpoint_type"] = endpointType;
                writeSeriesPointDiagnostics(ext, pt, pointSource, sourceRef, commonEdge);
                addGeometryErr(QStringLiteral("接边要素缺失"), stdName, QStringLiteral("%1侧接边线要素在%2侧图幅中未找到对应接边要素").arg(sourceSide, missingSide), sourceSide == QStringLiteral("左") ? sourceRef : SeriesFeatureRef(), sourceSide == QStringLiteral("右") ? sourceRef : SeriesFeatureRef(), ext);
            }
        };
        runMissingDirection(rights, leftIndex, leftMap, QStringLiteral("右"), QStringLiteral("左"));
        QJsonObject stat; stat["left_candidates"] = lefts.size(); stat["right_candidates"] = rights.size(); stat["execution_state"] = lefts.isEmpty() && rights.isEmpty() ? QStringLiteral("no_applicable_features") : QStringLiteral("completed"); if (isCrossScale) { stat["large_scale_candidates"] = lefts.size(); stat["small_scale_candidates"] = rights.size(); } stat["matched_candidates"] = matchedCount; geometrySummary[stdName] = stat; geometryTotals["left_candidates"] = geometryTotals.value("left_candidates").toInt() + lefts.size(); geometryTotals["right_candidates"] = geometryTotals.value("right_candidates").toInt() + rights.size(); if (isCrossScale) { geometryTotals["large_scale_candidates"] = geometryTotals.value("large_scale_candidates").toInt() + lefts.size(); geometryTotals["small_scale_candidates"] = geometryTotals.value("small_scale_candidates").toInt() + rights.size(); } geometryTotals["matched_candidates"] = geometryTotals.value("matched_candidates").toInt() + matchedCount;
    };
    QStringList targets = splitSeriesList(S("series_target_layers"));
    for (const QString& extra : splitSeriesList(boundaryStd + QStringLiteral("；") + roadStd + QStringLiteral("；") + waterStd + QStringLiteral("；") + waterAreaStd)) if (!extra.isEmpty() && !targets.contains(extra)) targets << extra;
    targets.removeDuplicates();
    QJsonObject expandedTargetMap;
    for (const QString& n : targets) {
        expandedTargetMap[n] = QJsonArray::fromStringList(expandSeriesLayerGroup(n));
        lineCheck(n);
    }
    int completedLayers = 0, emptyLayers = 0;
    for (const QJsonValue& value : geometrySummary) {
        const QString state = value.toObject().value(QStringLiteral("execution_state")).toString();
        if (state == QStringLiteral("completed")) ++completedLayers;
        else if (state.startsWith(QStringLiteral("no_applicable"))) ++emptyLayers;
    }
    if (!inputValidationErrors.isEmpty()) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info["execution_state"] = QStringLiteral("input_error");
        result.info["description"] = QStringLiteral("501 检查未完整执行：%1").arg(inputValidationErrors.join(QStringLiteral("；")));
    }
    else if (completedLayers == 0) {
        result.status = QStringLiteral("未执行"); result.err_exist = hasError;
        result.info["execution_state"] = QStringLiteral("no_applicable_features");
        result.info["description"] = QStringLiteral("没有适用的配对/支撑检查对象；单侧候选的样式提示单独保留，本次不能判定检查通过");
    }
    else if (emptyLayers > 0) {
        result.status = QStringLiteral("部分完成"); result.err_exist = hasError;
        result.info["execution_state"] = QStringLiteral("completed_with_empty_layers");
        result.info["description"] = QStringLiteral("完成%1个图层检查，%2个已选择图层没有适用检查对象；本次未完整执行").arg(completedLayers).arg(emptyLayers);
    }
    else {
        result.status = QStringLiteral("质检成功");
        result.err_exist = hasError;
        result.info["execution_state"] = QStringLiteral("completed");
        result.info["description"] = hasError ? QStringLiteral("501图幅接边检查完成，发现问题") : QStringLiteral("501图幅接边检查完成，未发现问题");
    }
    if (hasFinding && !hasError) result.info["description"] = QStringLiteral("501检查完成，存在待复核项目；没有确认错误。");
    if (!attributeComparisonExecuted && !hasError) result.info["description"] = isCrossScale
        ? QStringLiteral("501�����ɣ�������δ�������ݴ�����ӳ�䣬���ܾݴ˽�������һ�¡�")
        : QStringLiteral("501�����ɣ�δ���ý������Ե��ֶ��б��������Ե��һ�¡�");
    if (isCrossScale && !crossScaleAttributeFields.isEmpty() && !attributeComparisonExecuted && !hasError)
        result.info["description"] = QStringLiteral("501检查完成：已配置跨比例尺稳定属性字段，但本次没有可唯一关联的 1:1 要素对，属性规则未执行。");
    if (cartographicSymbolRuleUncoveredCount > 0 && !hasError) {
        result.info["description"] = QStringLiteral("501检查完成：%1 个要素没有命中适用的符号规范规则，不能据此判定全部通过。")
            .arg(cartographicSymbolRuleUncoveredCount);
    }
    result.info["confirmed_error_count"] = problemSummary.value("confirmed_error_count").toInt();
    result.info["needs_review_count"] = problemSummary.value("needs_review_count").toInt();
    result.info["not_evaluable_count"] = problemSummary.value("not_evaluable_count").toInt();
    result.info["has_unconfirmed_findings"] = (hasFinding && !hasError) || !attributeComparisonExecuted || cartographicSymbolRuleUncoveredCount > 0;
    result.info["uncovered_symbol_rule_feature_count"] = cartographicSymbolRuleUncoveredCount;
    result.info["input_validation_errors"] = QJsonArray::fromStringList(inputValidationErrors);
    result.info["template_path"] = xmlPath;
    result.info["pair_info"] = pairInfo;
    result.info["sub_rule_summary"] = summary;
    result.info["problem_summary"] = problemSummary;
    result.info["layer_path_summary"] = layerPathSummary;
    result.info["expanded_target_layers"] = expandedTargetMap;
    result.info["geometry_summary"] = geometrySummary;
    geometryTotals["checked_layers"] = checkedLayerCount;
    geometryTotals["finding_count"] = result.errors.keys().size();
    geometryTotals["error_count"] = problemSummary.value("confirmed_error_count").toInt();
    result.info["geometry_totals"] = geometryTotals;
    QJsonObject source;
    QJsonObject symbolRuleCoverageByLayer;
    for (auto it = cartographicSymbolRuleUncoveredByLayer.constBegin(); it != cartographicSymbolRuleUncoveredByLayer.constEnd(); ++it)
        symbolRuleCoverageByLayer[it.key()] = it.value();
    source["target_layers"] = QJsonArray::fromStringList(targets);
    source["attribute_fields"] = QJsonArray::fromStringList(isCrossScale ? crossScaleAttributeFields : attributeFields);
    source["same_scale_attribute_fields"] = QJsonArray::fromStringList(attributeFields);
    source["cross_scale_stable_attribute_fields"] = QJsonArray::fromStringList(crossScaleAttributeFields);
    source["invalid_cross_scale_attribute_fields"]=QJsonArray::fromStringList(invalidCrossScaleAttributeFields);
    source["project_style_id"]=projectStyleId;
    source["project_style_source"]=S(QStringLiteral("series_project_style_source"));
    source["main_role_layers"]=QJsonArray::fromStringList(mainRoleLayers);
    source["external_role_layers"]=QJsonArray::fromStringList(externalRoleLayers);
    source["cross_scale_style_comparison"]=QStringLiteral("independent_applicable_symbol_rules_only");
    source["symbol_rule_role_policy"]=QStringLiteral("explicit_feature_context_or_role_attribute_or_configured_actual_layer; unknown_is_not_evaluable");
    source["attribute_check_executed"] = attributeComparisonExecuted;
    source["invalid_stable_identity_layers"] = QJsonArray::fromStringList(invalidStableIdentityLayers.values());
    source["source_audits"] = sourceAudits;
    source["attribute_check_status"] = isCrossScale
        ? (crossScaleAttributeFields.isEmpty() ? QStringLiteral("cross_scale_stable_field_mapping_not_configured")
            : (attributeComparisonExecuted ? QStringLiteral("executed_on_unique_1to1_supports") : QStringLiteral("configured_no_unique_pairs")))
        : (attributeFields.isEmpty() ? QStringLiteral("not_configured") : QStringLiteral("executed"));
    source["line_style_fields"] = QJsonArray::fromStringList(lineStyleFields);
    source["polygon_style_fields"] = QJsonArray::fromStringList(polygonStyleFields);
    source["point_style_fields"] = QJsonArray::fromStringList(pointStyleFields);
    source["cartographic_symbol_rule_source"] = cartographicSymbolRuleSourceText;
    source["cartographic_symbol_rule_key_format"] = QStringLiteral("[role:<main|external>:][scale:<denominator>:]<key>|label|colors(/)|width_mm|field|actual_unit|component|scope(project:<id>|all_scales)|role");
    source["cartographic_symbol_rule_scope"] = QStringLiteral("分别核查适用图式的描画颜色和已核验线宽；跨比例尺不直接比较样式字段相等");
    source["whole_point_symbol_size_check_status"]=QStringLiteral("not_evaluated_by_stroke_width");
    source["composite_symbol_check_status"]=QStringLiteral("requires_resolved_component_evidence; halo_and_glow_not_equal_to_base_paint");
    source["cartographic_symbol_rule_count"] = cartographicSymbolSpecs.size();
    source["cartographic_symbol_rule_coverage_status"] = cartographicSymbolSpecs.isEmpty()
        ? QStringLiteral("not_configured")
        : (cartographicSymbolRuleUncoveredCount > 0 ? QStringLiteral("partial_with_uncovered_features") : QStringLiteral("matched_for_all_checked_features"));
    source["cartographic_symbol_rule_uncovered_feature_count"] = cartographicSymbolRuleUncoveredCount;
    source["cartographic_symbol_rule_uncovered_by_layer"] = symbolRuleCoverageByLayer;
    source["rule_source_text"] = cartographicSymbolRuleText;
    source["river_annotation_grade_rule_source"] = riverAnnotationGradeRuleSourceText;
    source["river_annotation_grade_rule_count"] = riverAnnotationGradeSpecs.size();
    source["river_annotation_grade_rule_text"] = riverAnnotationGradeRuleText;
    source["mountain_annotation_grade_rule_source"] = mountainAnnotationGradeRuleSourceText;
    source["mountain_annotation_grade_rule_count"] = mountainAnnotationGradeSpecs.size();
    source["mountain_annotation_grade_rule_text"] = mountainAnnotationGradeRuleText;
    source["annotation_text_fields"] = QJsonArray::fromStringList(annotationTextFields);
    source["annotation_font_size_fields"] = QJsonArray::fromStringList(annotationFontSizeFields);
    source["river_level_fields"] = QJsonArray::fromStringList(riverLevelFields);
    source["scope_description"] = isCrossScale
        ? QStringLiteral("本次跨比例尺检查线要素执行表达支撑关系核查，并按挂图参数分别核查各图幅候选要素符号样式；不做跨比例尺样式字段相等检查")
        : QStringLiteral("本次同比例尺检查线要素执行几何接边检查，点/线/面要素执行属性与样式表达一致性检查，并按挂图参数核查线状符号样式");
    source["expanded_target_layers"] = expandedTargetMap;
    source["pair_name"] = filePaths.seriesPairName;
    source["boundary_layer"] = boundaryStd;
    source["admin_area_layer"] = adminStd;
    source["road_layer"] = roadStd;
    source["water_layer"] = waterStd;
    source["water_area_layer"] = waterAreaStd;
    source["edge_buffer_distance"] = buf;
    source["line_edge_tolerance"] = lineTol;
    source["endpoint_closure_tolerance"] = endTol;
    source["check_mode"] = isCrossScale ? QStringLiteral("CrossScale") : QStringLiteral("SameScale");
    source["left_scale"] = leftScaleDenom;
    source["right_scale"] = rightScaleDenom;
    source["layer_name_aliases"] = S(QStringLiteral("series_layer_name_aliases"));
    source["layer_matching_strategy"] = hasConfirmedLayerMapping ? QStringLiteral("user_confirmed_layer_mapping") : QStringLiteral("shp_name_alias");
    source["user_confirmed_layer_mapping"] = hasConfirmedLayerMapping;
    source["cross_scale_registration_error"] = registrationError;
    source["series_cross_scale_overlap_threshold"] = crossScaleOverlapThreshold;
    source["cross_scale_weak_support_score"] = crossScaleWeakSupportScore;
    source["cross_scale_support_model"] = QStringLiteral("best_identity_group_line_union_buffer_coverage_and_polygon_union_area_coverage");
    source["cross_scale_polygon_aggregation_rule_configured"] = crossScalePolygonAggregationAllowed;
    source["cross_scale_max_uncovered_segment_length_configured"] = crossScaleMaxGapConfigured;
    source["cross_scale_max_uncovered_segment_length"] = crossScaleMaxGapConfigured ? QJsonValue(crossScaleMaxGapLength) : QJsonValue::Null;
    source["cross_scale_point_position_tolerance_configured"] = featurePointToleranceConfigured;
    source["cross_scale_point_position_tolerance"] = featurePointToleranceConfigured ? QJsonValue(featurePointTol) : QJsonValue::Null;
    source["cross_scale_line_support_tolerance"] = qMax(lineTol,endTol);
    if (hasConfirmedLayerMapping) source["confirmed_layer_mapping"] = confirmedLayerMapping;
    source["series_index_adjacency_tolerance"] = D(QStringLiteral("series_index_adjacency_tolerance"), 1e-8);
    source["crs_validation"] = QStringLiteral("dialog_precheck_requires_same_meter_projected_crs");
    source["series_cross_scale_keep_level_field"] = crossScaleKeepLevelField;
    source["series_cross_scale_min_keep_level"] = crossScaleMinKeepLevel;
    source["series_cross_scale_level_smaller_is_higher"] = crossScaleLevelSmallerIsHigher;
    source["series_cross_scale_keep_when_level_missing"] = crossScaleKeepWhenLevelMissing;
    source["tolerance_model"] = isCrossScale
        ? QStringLiteral("sqrt(left_tolerance^2 + right_tolerance^2) + registrationError")
        : QStringLiteral("map_mm_to_ground_by_single_scale");
    source["geometry_sub_rules"] = QJsonArray::fromStringList(QStringList() << QStringLiteral("接边要素缺失") << QStringLiteral("线要素接边容差") << QStringLiteral("端点闭合容差") << QStringLiteral("算法异常"));
    source["severity"] = severity;
    result.info["rule_source"] = source;
}


// 100 - 比例尺数值检查

void algorithm_ScaleValueCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    ScaleCheckerAlgorithm::execute(checkItemName, filePaths, scale, outputDir, result);
}

// 101 - 要素层缺失检查
void FeatureLayerMissingCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;
    
    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "101";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("要素层缺失_%1.json").arg(standardLayer));
    
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szSourceShpFilePath = gdbShpBytes.constData();
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szSourceShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_gdb\\永登县_gdb\\HYDL_HL.shp";
    const char* szParsedDataPath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_shp\\永登县_shp\\水系_LineString.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\河流.xml";
    const int testScale = 145000;
    */

    try {
        // 调用C接口
        SE_Error error = ::FeatureLayerMissingCheck(
            szSourceShpFilePath,
            szParsedDataPath,
            szSchemaXmlPath,
            szOutputJsonPath,
            scale  // 使用传入的比例尺参数
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "101";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["source_shp"] = QString::fromUtf8(szSourceShpFilePath);
            errorDetail["parsed_data"] = QString::fromUtf8(szParsedDataPath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "101";
        }
    }
    catch (const std::exception& e) {
        result.id = "101";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

static QString ResolveFeatureMissingMainAreaPaths(const AlgorithmFilePaths& filePaths,
                                                  const QString& xmlPath,
                                                  int scale)
{
    const QString associatedStandardLayer = getRuleValueFromXml(
        xmlPath,
        QStringLiteral("要素缺失检查"),
        QStringLiteral("关联图层"),
        scale).trimmed();
    if (associatedStandardLayer.isEmpty()) {
        return QString();
    }

    const QMap<QString, CsvLayerInfo> layerMap =
        map_check_task_manager::parseCsvToLayerMap(filePaths.csvPath);
    auto layerIt = layerMap.constFind(associatedStandardLayer);
    if (layerIt == layerMap.constEnd()) {
        for (auto it = layerMap.constBegin(); it != layerMap.constEnd(); ++it) {
            if (it.key().compare(associatedStandardLayer, Qt::CaseInsensitive) == 0) {
                layerIt = it;
                break;
            }
        }
    }
    if (layerIt == layerMap.constEnd()) {
        return QString();
    }

    AlgorithmFilePaths associatedPaths = filePaths;
    map_check_task_manager::buildShpPaths(associatedPaths, layerIt.value());

    // 关联图层必须是面要素。解析结果同时存在基础文件和分类文件时，
    // 优先只传入 _Polygon.shp，避免线图层使主区几何加载失败。
    QStringList polygonPaths;
    for (const QString& path : associatedPaths.actualShpList) {
        if (QFileInfo(path).completeBaseName().endsWith(
                QStringLiteral("_Polygon"), Qt::CaseInsensitive)) {
            polygonPaths.append(path);
        }
    }
    const QStringList selectedPaths = polygonPaths.isEmpty()
        ? associatedPaths.actualShpList
        : polygonPaths;
    return selectedPaths.join(QLatin1Char(';'));
}

// 102 - 要素缺失检查
void FeatureMissingCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;
    
    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "102";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    QString mainAreaShpSemicolon = ResolveFeatureMissingMainAreaPaths(
        filePaths, selectedXmlPath, scale);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("要素缺失%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("要素缺失_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("要素缺失%1.log").arg(standardLayer));
    
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray mainAreaShpBytes = mainAreaShpSemicolon.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szSourceShpFilePath = gdbShpBytes.constData();
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szMainAreaShpFilePath = mainAreaShpBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szOutputCheckResultLayerPath = resultLayerPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szSourceShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_gdb\\永登县_gdb\\HYDL_HL.shp";
    const char* szParsedDataPath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_shp\\永登县_shp\\水系_LineString.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\河流.xml";
    const int testScale = 145000;
    */

    try {
        // 调用C接口
        SE_Error error = ::FeatureMissingCheck(
            szSourceShpFilePath,
            szParsedDataPath,
            szSchemaXmlPath,
            scale,  // 使用传入的比例尺参数
            szMainAreaShpFilePath,
            szOutputCheckResultLayerPath,
            szOutputJsonPath,
            szOutputLogFilePath
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "102";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["source_shp"] = QString::fromUtf8(szSourceShpFilePath);
            errorDetail["parsed_data"] = QString::fromUtf8(szParsedDataPath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["main_area_shp"] = QString::fromUtf8(szMainAreaShpFilePath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "102";
        }
    }
    catch (const std::exception& e) {
        result.id = "102";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 103 - 要素错绘检查
void FeatureMisplacementCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "103";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("要素错绘%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("要素错绘_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("要素错绘%1.log").arg(standardLayer));
    
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szSourceShpFilePath = gdbShpBytes.constData();
    const char* szParsedShpFilePath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szOutputCheckResultLayerPath = resultLayerPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szSourceShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_gdb\\永登县_gdb\\HYDL_HL.shp";
    const char* szParsedShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_shp\\永登县_shp\\水系_LineString.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\河流.xml";
    const int testScale = 145000;
    */

    try {
        // 调用C接口
        SE_Error error = ::FeatureMisplacementCheck(
            szSourceShpFilePath,
            szParsedShpFilePath,
            szSchemaXmlPath,
            scale,  // 使用传入的比例尺参数
            szOutputCheckResultLayerPath,
            szOutputJsonPath,
            szOutputLogFilePath
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "103";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["source_shp"] = QString::fromUtf8(szSourceShpFilePath);
            errorDetail["parsed_shp"] = QString::fromUtf8(szParsedShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "103";
        }
    }
    catch (const std::exception& e) {
        result.id = "103";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 104 - 绝对偏移检查
void AbsoluteOffsetCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "104";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("绝对偏移%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("绝对偏移_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("绝对偏移%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szSourceShpFilePath = gdbShpBytes.constData();
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szOutputCheckResultLayerPath = resultLayerPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szSourceShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_gdb\\永登县_gdb\\HYDL_HL.shp";
    const char* szParsedDataPath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_shp\\永登县_shp\\水系_LineString.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\河流.xml";
    const int testScale = 145000;
    */

    try {
        // 调用C接口
        SE_Error error = ::AbsoluteOffsetCheck(
            szSourceShpFilePath,
            szParsedDataPath,
            szSchemaXmlPath,
            szOutputCheckResultLayerPath,
            szOutputJsonPath,
            szOutputLogFilePath,
            scale  // 使用传入的比例尺参数
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "104";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["source_shp"] = QString::fromUtf8(szSourceShpFilePath);
            errorDetail["parsed_data"] = QString::fromUtf8(szParsedDataPath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "104";
        }
    }
    catch (const std::exception& e) {
        result.id = "104";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//105-几何走向异常检查
void checkWaterSystemDirection(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "105";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("几何走向异常_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("几何走向异常_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("几何走向异常_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szParsedShpFilePath = actualShpBytes.constData();
    const char* szReferenceShpFilePath = gdbShpBytes.constData();
    const char* szXmlConfigPath = xmlPathBytes.constData();
    const char* szOutputProblemShpFilePath = resultLayerPathBytes.constData();
    const char* szOutputJsonFilePath = jsonPathBytes.constData();
    const char* szOutputLogFolderPath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szParsedShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/最新_永登县新版数据/永登县shp新版/沟渠.shp";
    const char* szReferenceShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/永登县_gdb/永登县_gdb/HYDL_Qu.shp";
    const char* szXmlConfigPath = "D:/Desktop/集成/xml规则 (2)/xml规则/支渠.xml";
    const int testScale = 145000;
    */

    try {
        ReadShp_error error = runReadShpInterfaceOnGuiThread([&]() {
            return checkWaterSystemDirection(
                szParsedShpFilePath,
                szReferenceShpFilePath,
                szXmlConfigPath,
                szOutputProblemShpFilePath,
                szOutputJsonFilePath,
                szOutputLogFolderPath,
                scale);
        });

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "105";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            QJsonObject errorDetail;
            if (error != READSHP_SUCCESS) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["parsed_shp"] = QString::fromUtf8(szParsedShpFilePath);
            errorDetail["reference_shp"] = QString::fromUtf8(szReferenceShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlConfigPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "105";
        }
    }
    catch (const std::exception& e) {
        result.id = "105";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 106 - 最小弯曲阈值控制
void MinBendThresholdCheck(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "106";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("最小弯曲阈值%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("最小弯曲阈值_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("最小弯曲阈值%1.log").arg(standardLayer));
    
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputCheckReportPath = jsonPathBytes.constData();
    const char* szOutputCheckResultLayerPath = resultLayerPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szParsedDataPath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\永登县_shp\\永登县_shp\\水系_LineString.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\河流.xml";
    const int testScale = 145000;
    */

    try {
        // 调用C接口
        SE_Error error = ::MinBendThresholdCheck(
            szParsedDataPath,
            szSchemaXmlPath,
            szOutputCheckReportPath,
            szOutputCheckResultLayerPath,
            szOutputLogFilePath,
            scale  // 使用传入的比例尺参数
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "106";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["parsed_data"] = QString::fromUtf8(szParsedDataPath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "106";
        }
    }
    catch (const std::exception& e) {
        result.id = "106";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}
//107 特征点限差控制 √ 
void FeaturePointToleranceChecker(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{

        result.id = "107";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "跳过质检";
        return;

    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "107";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("点位容差_%1.json").arg(standardLayer));
    QString errorPointsLayerPath = QDir(outPaths.errorResultDir).filePath(QString("点位容差_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("点位容差_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray errorPointsLayerPathBytes = errorPointsLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szSourceShpFilePath = gdbShpBytes.constData();
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szOutputErrorPointsLayerPath = errorPointsLayerPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();

    try {
        // 调用C接口
        SE_Error error = ::FeaturePointToleranceCheck(
            szSourceShpFilePath,
            szParsedDataPath,
            szSchemaXmlPath,
            scale,
            szOutputErrorPointsLayerPath,
            szOutputJsonPath,
            szOutputLogFilePath
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "107";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["source_shp"] = gdbShpSemicolon;
            errorDetail["parsed_data"] = actualShpSemicolon;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "107";
        }
    }
    catch (const std::exception& e) {
        result.id = "107";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 108 - 光滑度阈值控制
void SmoothnessChecker(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
        
    
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "108";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("光滑度阈值_%1.json").arg(standardLayer));
    QString errorPointsPath = QDir(outPaths.errorResultDir).filePath(QString("光滑度阈值_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("光滑度阈值_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray errorPointsBytes = errorPointsPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szParsedDataPath = actualShpBytes.constData();
    const char* szOutputErrorPointsLayerPath = errorPointsBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    
    try {
        // 调用C接口
        SE_Error error = ::SmoothnessThresholdCheck(
            szSchemaXmlPath,
            szParsedDataPath,
            scale,
            szOutputErrorPointsLayerPath,
            szOutputJsonPath,
            szOutputLogFilePath
        );
        
        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "108";
            result.status = "质检失败";
            result.err_exist = true;
            
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["parsed_data"] = actualShpSemicolon;
            errorDetail["scale"] = scale;
            
            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "108";
        }
    }
    catch (const std::exception& e) {
        result.id = "108";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}


//109  直角化角度阈值控制 
void checkRightAngleThreshold(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 109 的旧单文件接口不能接收以分号拼接的路径。实际图层可能同时
    // 包含 LineString 和 Polygon，必须使用批量接口逐层加载，而不是把
    // "a.shp;b.shp" 当作一个文件名传入。
    const QStringList actualShpPaths = splitExistingShpPaths(filePaths.actualShpSemicolon);
    const QStringList gdbShpPaths = splitExistingShpPaths(filePaths.gdbShpSemicolon);
    const QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpPaths.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "109";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    const QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);

    // 直角化必须同时具备库体和规则。过去会由旧接口生成一个空结果，
    // 最终表现为“未发现错误”却“质检失败”；这里直接给出可定位的原因。
    if (gdbShpPaths.isEmpty() || selectedXmlPath.isEmpty() || !QFileInfo::exists(selectedXmlPath)) {
        result.id = "109";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject detail;
        detail["description"] = gdbShpPaths.isEmpty()
            ? QStringLiteral("未找到对应的库体SHP文件")
            : QStringLiteral("未找到直角化角度阈值控制的XML规则文件");
        detail["actual_shp_paths"] = actualShpPaths.join(';');
        detail["gdb_shp_paths"] = gdbShpPaths.join(';');
        detail["xml_path"] = selectedXmlPath;
        result.errors["error1"] = detail;
        return;
    }
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("直角化角度阈值_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("直角化角度阈值_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("直角化角度阈值_%1.log").arg(standardLayer));
    
    // QByteArray 生命周期管理：先固定字节数组，再取得 constData 指针，
    // 以满足批量 C 接口对 const char** 的要求。
    QVector<QByteArray> actualShpBytes;
    QVector<QByteArray> gdbShpBytes;
    actualShpBytes.reserve(actualShpPaths.size());
    gdbShpBytes.reserve(gdbShpPaths.size());
    for (const QString& path : actualShpPaths) actualShpBytes.append(path.toUtf8());
    for (const QString& path : gdbShpPaths) gdbShpBytes.append(path.toUtf8());

    QVector<const char*> actualShpPointers;
    QVector<const char*> gdbShpPointers;
    actualShpPointers.reserve(actualShpBytes.size());
    gdbShpPointers.reserve(gdbShpBytes.size());
    for (const QByteArray& path : actualShpBytes) actualShpPointers.append(path.constData());
    for (const QByteArray& path : gdbShpBytes) gdbShpPointers.append(path.constData());

    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();

    const char* szXmlConfigPath = xmlPathBytes.constData();
    const char* szOutputProblemShpFilePath = resultLayerPathBytes.constData();
    const char* szOutputJsonFilePath = jsonPathBytes.constData();
    const char* szOutputLogFolderPath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szParsedShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/最新_永登县新版数据/永登县shp新版/沟渠.shp";
    const char* szReferenceShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/永登县_gdb/永登县_gdb/HYDL_Qu.shp";
    const char* szXmlConfigPath = "D:/Desktop/集成/xml规则 (2)/xml规则/支渠.xml";
    const int testScale = 145000;
    */

    try {
        ReadShp_error error = runReadShpInterfaceOnGuiThread([&]() {
            return checkRightAngleThresholdBatch(
                // QVector<const char*>::data() 的类型是 const char**；
                // constData() 则是 const char* const*，不能传给 C 接口。
                actualShpPointers.data(), actualShpPointers.size(),
                gdbShpPointers.data(), gdbShpPointers.size(),
                szXmlConfigPath,
                szOutputProblemShpFilePath,
                szOutputJsonFilePath,
                szOutputLogFolderPath,
                scale);
        });

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "109";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            QJsonObject errorDetail;
            if (error != READSHP_SUCCESS) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["parsed_shp_paths"] = actualShpPaths.join(';');
            errorDetail["reference_shp_paths"] = gdbShpPaths.join(';');
            errorDetail["xml_path"] = QString::fromUtf8(szXmlConfigPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "109";
        }
    }
    catch (const std::exception& e) {
        result.id = "109";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//110削尖角阈值
void AngleCheckInterface(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 先写入固定 ID，确保任意提前返回或异常分支都不会产生空 ID。
    result.id = "110";
    QString standardLayer = filePaths.standardLayer;
    const QStringList sharpAngleShpPaths = filterSharpAngleGeometryPaths(filePaths.actualShpSemicolon);

    if (sharpAngleShpPaths.isEmpty()) {
        result.id = "110";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "未找到线或面要素图层，削尖角度阈值控制不执行。";
        return;
    }

    const QString actualShpSemicolon = sharpAngleShpPaths.first();
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径（与算法101保持一致）
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("削尖角阈值_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("削尖角阈值_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("削尖角阈值_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    // 筛选开关由 110 XML 决定；关闭时扩展接口会忽略源图层参数。
    QByteArray sourceDataPathsBytes = filePaths.gdbShpSemicolon.toUtf8();
    
    const char* szShpFilePath = actualShpBytes.constData();
    const char* szXmlConfigPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szResultShpPath = resultLayerPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szSourceDataPaths = sourceDataPathsBytes.constData();

    // SharpAngleCheckWithSource 是单图层接口。管理系统传入多个线/面图层时逐个执行，
    // 再汇总为当前检查项的一个 JSON 结果，不能把路径以分号拼接后直接传入。
    if (sharpAngleShpPaths.size() > 1) {
        QJsonObject mergedErrors;
        QJsonObject layerResults;
        QJsonArray checkedPaths;
        QJsonArray errorShpPaths;
        QJsonArray anglePointPaths;
        bool executionFailed = false;
        int geometryInvalidLayerCount = 0;
        int normalLayerCount = 0;
        int errorIndex = 1;

        for (int i = 0; i < sharpAngleShpPaths.size(); ++i) {
            const QString& inputPath = sharpAngleShpPaths.at(i);
            const QString partErrorShpPath = QDir(outPaths.errorResultDir).filePath(
                QString("削尖角度阈值_%1_part%2_errors.shp").arg(standardLayer).arg(i + 1));

            const QByteArray inputBytes = inputPath.toUtf8();
            const QByteArray partErrorShpBytes = partErrorShpPath.toUtf8();
            QFile::remove(jsonPath);
            const SE_Error error = SharpAngleCheckWithSource(
                inputBytes.constData(), szXmlConfigPath, szLogPath,
                partErrorShpBytes.constData(), szOutputJsonPath, scale,
                szSourceDataPaths);

            checkedPaths.append(inputPath);

            CheckItemResult partResult;
            const bool parsed = ParseJsonToCheckItemResult(jsonPath, partResult, checkItemName);
            if (error != SE_ERROR_NONE || !parsed ||
                partResult.status == "质检失败") {
                executionFailed = true;
                QJsonObject detail;
                detail["input_shp"] = inputPath;
                detail["description"] = parsed && partResult.status == "质检失败"
                    ? partResult.info.value("description").toString()
                    : QString("削尖角度阈值控制执行失败，错误码: %1").arg(error);
                detail["error_code"] = error;
                mergedErrors[QString("error%1").arg(errorIndex++)] = detail;

                QJsonObject layerResult;
                layerResult["input_shp"] = inputPath;
                layerResult["status"] = "质检失败";
                layerResult["geometry_validation"] = "未完成";
                layerResult["description"] = detail["description"];
                layerResults[QString("layer%1").arg(i + 1)] = layerResult;
                continue;
            }

            const QString description = partResult.info.value("description").toString();
            const bool geometryInvalid = description.contains("几何无效");
            const QString invalidGeometryShpPath = QDir(outPaths.errorResultDir).filePath(
                QString("削尖角度阈值_%1_part%2_errors_%3_无效几何_err.shp")
                    .arg(standardLayer).arg(i + 1).arg(QFileInfo(inputPath).baseName()));
            const QString outputErrorShpPath = geometryInvalid ? invalidGeometryShpPath : partErrorShpPath;

            QJsonObject layerResult;
            layerResult["input_shp"] = inputPath;
            layerResult["status"] = partResult.status;
            layerResult["err_exist"] = partResult.err_exist;
            layerResult["geometry_validation"] = geometryInvalid ? "几何无效" : "正常";
            layerResult["info"] = partResult.info;
            layerResult["errors"] = partResult.errors;
            if (QFileInfo::exists(outputErrorShpPath)) {
                layerResult["error_shp_path"] = outputErrorShpPath;
                errorShpPaths.append(outputErrorShpPath);
            }
            const QString anglePointPath = QDir(outPaths.errorResultDir).filePath(
                QString("削尖角度阈值_%1_part%2_error_尖角点.shp").arg(standardLayer).arg(i + 1));
            if (QFileInfo::exists(anglePointPath)) {
                layerResult["angle_point_path"] = anglePointPath;
                anglePointPaths.append(anglePointPath);
            }
            layerResults[QString("layer%1").arg(i + 1)] = layerResult;

            if (geometryInvalid)
                ++geometryInvalidLayerCount;
            else
                ++normalLayerCount;

            for (auto it = partResult.errors.constBegin(); it != partResult.errors.constEnd(); ++it) {
                QJsonObject detail;
                detail["input_shp"] = inputPath;
                detail["geometry_validation"] = geometryInvalid ? "几何无效" : "正常";
                detail["issue"] = it.value();
                mergedErrors[QString("error%1").arg(errorIndex++)] = detail;
            }
        }

        result.id = "110";
        result.name = checkItemName;
        result.status = executionFailed ? "质检失败" : "质检成功";
        result.err_exist = !mergedErrors.isEmpty();
        result.errors = mergedErrors;
        result.info["description"] = executionFailed
            ? "削尖角度阈值控制未能完成全部图层检查。"
            : "削尖角度阈值控制完成。";
        result.info["input_shp_paths"] = checkedPaths;
        result.info["error_shp_paths"] = errorShpPaths;
        result.info["angle_point_paths"] = anglePointPaths;
        result.info["problem_count"] = mergedErrors.size();
        result.info["geometry_invalid_layer_count"] = geometryInvalidLayerCount;
        result.info["normal_layer_count"] = normalLayerCount;
        result.info["layer_results"] = layerResults;

        QJsonObject checkItem;
        checkItem["id"] = result.id;
        checkItem["name"] = result.name;
        checkItem["status"] = result.status;
        checkItem["err_exist"] = result.err_exist;
        checkItem["info"] = result.info;
        checkItem["errors"] = result.errors;
        QJsonObject root;
        root["check_item"] = checkItem;
        QFile jsonFile(jsonPath);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            jsonFile.close();
        }
        return;
    }
    
    /* 原硬编码测试路径(已禁用):
    const char* szShpFilePath = "D:\\Desktop\\项目\\甘肃地图成果质检系统\\数据部分\\最新_永登县新版数据\\永登县shp新版\\村道_LineString.shp";
    const char* szXmlConfigPath = "D:\\Desktop\\xmll\\村道.xml";
    const int testScale = 50000;
    */
    
    try {
        QFile::remove(jsonPath);
        SE_Error error = SharpAngleCheckWithSource(
            szShpFilePath,
            szXmlConfigPath,
            szLogPath,              // 第3个参数：日志路径
            szResultShpPath,        // 第4个参数：错误SHP路径
            szOutputJsonPath,       // 第5个参数：JSON报告路径
            scale,
            szSourceDataPaths
        );
        
        // 使用通用解析函数解析JSON并填充结果
        const bool parsed = ParseJsonToCheckItemResult(jsonPath, result, checkItemName);
        if (!parsed || error != SE_ERROR_NONE) {
            // 解析失败时的默认处理
            result.id = "110";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;
            if (!parsed) {
                result.info = QJsonObject();
                result.errors = QJsonObject();
            }
            
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["shp_path"] = QString::fromUtf8(szShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlConfigPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "110";
            const QString pointPath = QDir(outPaths.errorResultDir).filePath(
                QString("削尖角阈值_%1_error_尖角点.shp").arg(standardLayer));
            if (QFileInfo::exists(pointPath))
                result.info["angle_point_path"] = pointPath;
        }
    }
    catch (const std::exception& e) {
        result.id = "110";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//111 面要素重叠 / 间隙检查
void checkPolygonOverlapGap(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;
    
    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "111";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }

    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("面要素重叠间隙_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("面要素重叠间隙_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("面要素重叠间隙_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szParsedShpFilePath = actualShpBytes.constData();
    const char* szXmlConfigPath = xmlPathBytes.constData();
    const char* szOutputProblemShpFilePath = resultLayerPathBytes.constData();
    const char* szOutputJsonFilePath = jsonPathBytes.constData();
    const char* szOutputLogFolderPath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szParsedShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/最新_永登县新版数据/永登县shp新版/行政区划面普色.shp";
    const char* szXmlConfigPath = "D:/Desktop/集成/xml规则 (2)/xml规则/行政区划面普色.xml";
    const int testScale = 145000;
    */

    try {
        ReadShp_error error = runReadShpInterfaceOnGuiThread([&]() {
            return checkPolygonOverlapGap(
                szParsedShpFilePath,
                szXmlConfigPath,
                szOutputProblemShpFilePath,
                szOutputJsonFilePath,
                szOutputLogFolderPath,
                scale);
        });

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "111";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            QJsonObject errorDetail;
            if (error != READSHP_SUCCESS) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["parsed_shp"] = QString::fromUtf8(szParsedShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlConfigPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "111";
        }
    }
    catch (const std::exception& e) {
        result.id = "111";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }

}

//112 线要素拓扑检查
void checkLineTopology(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        result.id = "112";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        result.info["description"] = "未执行：未找到匹配的成果线图层，不能视为拓扑检查通过";
        result.info["summary"] = result.info["description"];
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);

    // Other algorithms retain their legacy matching; topology must never use
    // the first unrelated XML (for example #比例尺.xml) with zero tolerances.
    QDomDocument topologyRules;
    QFile topologyXml(selectedXmlPath);
    bool hasTopologyRule = false;
    if (QFileInfo(selectedXmlPath).completeBaseName() == standardLayer &&
        topologyXml.open(QIODevice::ReadOnly) && topologyRules.setContent(&topologyXml)) {
        const auto items = topologyRules.elementsByTagName(QStringLiteral("check_item"));
        for (int i = 0; i < items.size(); ++i) {
            const auto item = items.at(i).toElement();
            if (item.firstChildElement(QStringLiteral("id")).text().trimmed() == QStringLiteral("112") &&
                item.firstChildElement(QStringLiteral("name")).text().trimmed() == checkItemName)
                hasTopologyRule = true;
        }
    }
    if (!hasTopologyRule) {
        result.id = "112"; result.name = checkItemName;
        result.status = "质检失败"; result.err_exist = true;
        result.info["summary"] = QStringLiteral("未执行：缺少图层“%1”的专属XML或112拓扑规则").arg(standardLayer);
        result.info["description"] = result.info["summary"];
        result.info["xml_path"] = selectedXmlPath;
        return;
    }
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("线要素拓扑_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("线要素拓扑_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("线要素拓扑_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szParsedShpFilePath = actualShpBytes.constData();
    const char* szReferenceShpFilePath = gdbShpBytes.constData();
    const char* szXmlConfigPath = xmlPathBytes.constData();
    const char* szOutputProblemShpFilePath = resultLayerPathBytes.constData();
    const char* szOutputJsonFilePath = jsonPathBytes.constData();
    const char* szOutputLogFolderPath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szParsedShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/最新_永登县新版数据/永登县shp新版/省道.shp";
    const char* szReferenceShpFilePath = "D:/Desktop/项目/甘肃地图成果质检系统/数据部分/永登县_gdb/永登县_gdb/LRDL.shp";
    const char* szXmlConfigPath = "D:/Desktop/集成/xml规则 (2)/xml规则/省道.xml";
    const int testScale = 145000;
    */

    try {
        // 标准图层语义由 CSV 决定，必须与图层名一起传递给拓扑接口。
        QByteArray standardCsvBytes = filePaths.csvPath.toUtf8();
        QByteArray standardLayerBytes = standardLayer.toUtf8();
        ReadShp_error error = checkLineTopologyWithStandardLayer(
            szParsedShpFilePath,
            szReferenceShpFilePath,
            szXmlConfigPath,
            standardCsvBytes.constData(),
            standardLayerBytes.constData(),
            szOutputProblemShpFilePath,
            szOutputJsonFilePath,
            szOutputLogFolderPath,
            scale);

        // 当前执行失败时不能读取上次残留的成功 JSON，否则会被误判为本次成功并参与复用。
        if (error != READSHP_SUCCESS || !ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "112";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            QJsonObject errorDetail;
            if (error != READSHP_SUCCESS) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["parsed_shp"] = QString::fromUtf8(szParsedShpFilePath);
            errorDetail["reference_shp"] = QString::fromUtf8(szReferenceShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlConfigPath);
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "112";
        }
    }
    catch (const std::exception& e) {
        result.id = "112";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }

}
//113  包含关系倒置检查
void ContainmentCheckInterface(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    
    // 1. 查找包含“关系检查”的XML配置文件，并获取具体的检查类型
    // 先根据当前任务主图层路径的关键字，推断期望的具体项，避免误选到其他检查类型的XML
    auto detectExpectedCheckType = [](const QString& semicolonPaths) -> QString {
        const QString lower = semicolonPaths.toLower();
        // 关键字匹配（中文关键字使用原样匹配）
        if (lower.contains(QStringLiteral("泉"))) return QStringLiteral("泉位置检查");
        if (lower.contains(QString::fromUtf8("桥")) || lower.contains(QString::fromUtf8("桥梁"))) {
            return QString::fromUtf8("桥梁位置检查");
        }
        if (lower.contains(QString::fromUtf8("渠流向"))) {
            return QString::fromUtf8("渠流向位置检查");
        }
        if (lower.contains(QString::fromUtf8("隧道口")) || lower.contains(QString::fromUtf8("隧道"))) {
            return QString::fromUtf8("隧道口位置检查");
        }
        if (lower.contains(QString::fromUtf8("行政"))) {
            return QString::fromUtf8("行政面包含检查");
        }
        if (lower.contains(QString::fromUtf8("河流"))) {
            return QString::fromUtf8("河流包含检查");
        }
        return QString();
    };

    QString xmlPath;
    QString specificCheckType;
    const QString expectedType = detectExpectedCheckType(
        filePaths.actualShpSemicolon + QChar(';') + filePaths.standardLayer);

     // 判断实际图层是否为空或不存在
    if (filePaths.actualShpSemicolon.isEmpty() && expectedType != QStringLiteral("泉位置检查")) {
        // 实际图层不存在，直接返回成功状态
        result.id = "112";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }

    // 优先选择与期望类型严格匹配的XML
    if (!expectedType.isEmpty()) {
        for (const QString& path : filePaths.xmlPaths) {
            const QString typeInXml = getRuleValueFromXml(path, checkItemName, QString::fromUtf8("具体项"), scale);
            if (typeInXml == expectedType) {
                xmlPath = path;
                specificCheckType = typeInXml;
                break;
            }
        }
    }

    // 如果未找到严格匹配，则退回到原有策略：选择首个带有“具体项”的XML
    if (xmlPath.isEmpty() && expectedType != QStringLiteral("泉位置检查")) {
        for (const QString& path : filePaths.xmlPaths) {
            const QString typeInXml = getRuleValueFromXml(path, checkItemName, QString::fromUtf8("具体项"), scale);
            if (!typeInXml.isEmpty()) {
                xmlPath = path;
                specificCheckType = typeInXml;
                break;
            }
        }
    }

    // 如果未找到配置文件，则报错返回
    if (xmlPath.isEmpty()) {
        result.id = "113";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString("未找到与当前任务匹配的关系检查配置文件，或配置文件缺少“具体项”规则。");
        if (!expectedType.isEmpty()) {
            errorDetail["hint"] = QString("期望的具体项: %1").arg(expectedType);
        }
        result.errors["error1"] = errorDetail;
        if (expectedType == QStringLiteral("泉位置检查")) {
            result.info[QStringLiteral("description")] = errorDetail.value(QStringLiteral("error"));
            const OutputPaths out = CreateOutputDirectories(outputDir);
            const QString jsonPath = QDir(out.jsonDir).filePath(QStringLiteral("关系检查_泉.json"));
            const QJsonObject item{{QStringLiteral("id"), result.id},
                                   {QStringLiteral("name"), result.name},
                                   {QStringLiteral("status"), result.status},
                                   {QStringLiteral("err_exist"), true},
                                   {QStringLiteral("info"), errorDetail.value(QStringLiteral("error"))},
                                   {QStringLiteral("errors"), result.errors}};
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
                jsonFile.write(QJsonDocument(QJsonObject{{QStringLiteral("check_item"), item}})
                                   .toJson(QJsonDocument::Indented));
        }
        return;
    }

    // 2. 根据具体检查类型，获取关联图层路径
    QString basePolygonPath, basePointPath, parsedPolygonPath, parsedPointPath,
        riverPolygonPath, ditchFlowPaths, ditchPaths, bridgePaths, roadPaths,
        tunnelEntrancePaths, tunnelPaths, roadPathsForTunnel;

    if (specificCheckType == QStringLiteral("泉位置检查")) {
        const OutputPaths out = CreateOutputDirectories(outputDir);
        QString label = filePaths.standardLayer.isEmpty() ? QStringLiteral("泉") : filePaths.standardLayer;
        label.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")), QStringLiteral("_"));
        const QString jsonPath = QDir(out.jsonDir).filePath(QStringLiteral("关系检查_") + label + QStringLiteral(".json"));
        const QString errorPath = QDir(out.errorResultDir).filePath(QStringLiteral("关系检查错误_") + label + QStringLiteral(".shp"));
        const QString logPath = QDir(out.logDir).filePath(QStringLiteral("关系检查_") + label + QStringLiteral(".log"));

        QString parsed = getPathsForStdLayers(filePaths.standardLayer, filePaths.csvPath, filePaths.shpPath);
        const QString pointPaths = filterPathsBySuffixPreferBase(parsed, QStringLiteral("_Point"));
        if (!pointPaths.isEmpty()) parsed = pointPaths;
        QString riverNames = getRuleValueFromXml(xmlPath, checkItemName,
            QStringLiteral("泉位置检查关联的河流图层"), scale);
        if (riverNames.trimmed().isEmpty())
            riverNames = getRuleValueFromXml(xmlPath, checkItemName,
                QStringLiteral("渠流向位置检查关联图层"), scale);
        riverNames.replace(QChar(0xff1b), QChar(';'));
        const QString mappedRivers = getGdbPathsForStdLayers(
            riverNames, filePaths.csvPath, filePaths.gdbShpPath);
        QStringList riverPaths;
        for (const QString& path : mappedRivers.split(';', Qt::SkipEmptyParts))
            if (!riverPaths.contains(path, Qt::CaseInsensitive)) riverPaths << path;
        const QString rivers = riverPaths.join(';');

        if (QFileInfo::exists(jsonPath) && !QFile::remove(jsonPath)) {
            result.id = QStringLiteral("113");
            result.name = checkItemName;
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            result.errors[QStringLiteral("execution_error")] =
                QStringLiteral("无法清除上次运行的 JSON：%1").arg(jsonPath);
            return;
        }
        const QByteArray xml = xmlPath.toUtf8();
        const QByteArray springBytes = parsed.toUtf8();
        const QByteArray riverBytes = rivers.toUtf8();
        const QByteArray logBytes = logPath.toUtf8();
        const QByteArray errorBytes = errorPath.toUtf8();
        const QByteArray jsonBytes = jsonPath.toUtf8();
        const SE_Error code = RelationshipCheck(xml.constData(), nullptr, nullptr, nullptr,
            nullptr, nullptr, springBytes.constData(), riverBytes.constData(), nullptr,
            nullptr, nullptr, nullptr, nullptr, logBytes.constData(),
            errorBytes.constData(), jsonBytes.constData(), scale);
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = QStringLiteral("113");
            result.name = checkItemName;
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            result.errors[QStringLiteral("execution_error")] =
                QStringLiteral("泉位置检查未生成可解析的本次 JSON，错误码：%1").arg(code);
        } else if (code != SE_ERROR_NONE) {
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
        }
        return;
    }

    if (specificCheckType == "桥梁位置检查") {
        bridgePaths = filePaths.actualShpSemicolon; // 主检查图层
        // 获取关联的道路图层
        QString roadStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "桥梁位置检查关联的道路图层", scale);
        roadPaths = getPathsForStdLayers(roadStdLayers, filePaths.csvPath, filePaths.shpPath);
        // 道路优先选择线
        roadPaths = filterPathsBySuffixPreferBase(roadPaths, "_LineString");
        // 获取关联的河流图层
        QString riverStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "桥梁位置检查关联的河流图层", scale);
        riverPolygonPath = getPathsForStdLayers(riverStdLayers, filePaths.csvPath, filePaths.shpPath);
        // 河流优先选择面
        riverPolygonPath = filterPathsBySuffixPreferBase(riverPolygonPath, "_Polygon");
    }
    else if (specificCheckType == "渠流向位置检查") {
        ditchFlowPaths = filePaths.actualShpSemicolon; // 主检查图层
        // 渠流向优先选择面
        ditchFlowPaths = filterPathsBySuffixPreferBase(ditchFlowPaths, "_Polygon");
        // 获取关联的沟渠图层
        QString ditchStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "渠流向位置检查关联图层", scale);
        ditchPaths = getPathsForStdLayers(ditchStdLayers, filePaths.csvPath, filePaths.shpPath);
        // 沟渠优先选择线
        ditchPaths = filterPathsBySuffixPreferBase(ditchPaths, "_LineString");
    }
    else if (specificCheckType == "隧道口位置检查") {
        tunnelEntrancePaths = filePaths.actualShpSemicolon; // 主检查图层
        // 获取关联的道路图层
        QString roadStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "隧道口位置检查关联的道路图层", scale);
        roadPathsForTunnel = getPathsForStdLayers(roadStdLayers, filePaths.csvPath, filePaths.shpPath);
        roadPathsForTunnel = filterPathsBySuffixPreferBase(roadPathsForTunnel, "_LineString");
        // 获取关联的隧道图层
        QString tunnelStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "隧道口位置检查关联的隧道图层", scale);
        tunnelPaths = getPathsForStdLayers(tunnelStdLayers, filePaths.csvPath, filePaths.shpPath);
    }
    else if (specificCheckType == "河流包含检查") {
        parsedPointPath = filePaths.actualShpSemicolon; // 主检查图层（通常是点）
        // 主图层优先选择点
        parsedPointPath = filterPathsBySuffixPreferBase(parsedPointPath, "_Point");
        // 获取关联的河流面图层
        QString riverStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "河流包含检查关联图层", scale);
        riverPolygonPath = getPathsForStdLayers(riverStdLayers, filePaths.csvPath, filePaths.shpPath);
        riverPolygonPath = filterPathsBySuffixPreferBase(riverPolygonPath, "_Polygon");
    }
    else if (specificCheckType == "行政面包含检查") {
        // 主检查图层（通常是点，如行政村）
        parsedPointPath = filePaths.actualShpSemicolon;
        parsedPointPath = filterPathsBySuffixPreferBase(parsedPointPath, "_Point");
        // 获取关联的行政区划面图层（同时获取GDB与实际数据）
        QString adminAreaStdLayers = getRuleValueFromXml(xmlPath, checkItemName, "行政面包含检查关联图层", scale);
        // 关联图层的GDB路径（作为基础面数据）
        basePolygonPath = getGdbPathsForStdLayers(adminAreaStdLayers, filePaths.csvPath, filePaths.gdbShpPath);
        // 质检目标图层的GDB路径（作为基础点数据）
        basePointPath = filePaths.gdbShpSemicolon;
        // 关联图层的实际数据（作为解析面数据）
        parsedPolygonPath = getPathsForStdLayers(adminAreaStdLayers, filePaths.csvPath, filePaths.shpPath);
        parsedPolygonPath = filterPathsBySuffixPreferBase(parsedPolygonPath, "_Polygon");
    }
    else {
        // 不支持的检查类型
        result.id = "113";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString("不支持的关系检查具体项: %1").arg(specificCheckType);
        result.errors["error1"] = errorDetail;
        return;
    }

    // 3. 准备C-API所需参数
    QByteArray xmlPathBytes = xmlPath.toUtf8();
    QByteArray basePolygonPathBytes = basePolygonPath.toUtf8();
    QByteArray basePointPathBytes = basePointPath.toUtf8();
    QByteArray parsedPolygonPathBytes = parsedPolygonPath.toUtf8();
    QByteArray parsedPointPathBytes = parsedPointPath.toUtf8();
    QByteArray riverPolygonPathBytes = riverPolygonPath.toUtf8();
    QByteArray ditchFlowPathsBytes = ditchFlowPaths.toUtf8();
    QByteArray ditchPathsBytes = ditchPaths.toUtf8();
    QByteArray bridgePathsBytes = bridgePaths.toUtf8();
    QByteArray roadPathsBytes = roadPaths.toUtf8();
    QByteArray tunnelEntrancePathsBytes = tunnelEntrancePaths.toUtf8();
    QByteArray tunnelPathsBytes = tunnelPaths.toUtf8();
    QByteArray roadPathsForTunnelBytes = roadPathsForTunnel.toUtf8();

    // 创建输出目录结构（参考101）
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径（与算法101保持一致）
    QString standardLayer = filePaths.standardLayer;
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("关系检查_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("关系检查_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("关系检查_%1.log").arg(standardLayer));
    
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();

    const char* basePolygonPtr      = basePolygonPath.isEmpty()      ? nullptr : basePolygonPathBytes.constData();
    const char* basePointPtr        = basePointPath.isEmpty()        ? nullptr : basePointPathBytes.constData();
    const char* parsedPolygonPtr    = parsedPolygonPath.isEmpty()    ? nullptr : parsedPolygonPathBytes.constData();
    const char* parsedPointPtr      = parsedPointPath.isEmpty()      ? nullptr : parsedPointPathBytes.constData();
    const char* riverPolygonPtr     = riverPolygonPath.isEmpty()     ? nullptr : riverPolygonPathBytes.constData();
    const char* ditchFlowPathsPtr   = ditchFlowPaths.isEmpty()       ? nullptr : ditchFlowPathsBytes.constData();
    const char* ditchPathsPtr       = ditchPaths.isEmpty()           ? nullptr : ditchPathsBytes.constData();
    const char* bridgePathsPtr      = bridgePaths.isEmpty()          ? nullptr : bridgePathsBytes.constData();
    const char* roadPathsPtr        = roadPaths.isEmpty()            ? nullptr : roadPathsBytes.constData();
    const char* tunnelEntrancePtr   = tunnelEntrancePaths.isEmpty()  ? nullptr : tunnelEntrancePathsBytes.constData();
    const char* tunnelPathsPtr      = tunnelPaths.isEmpty()          ? nullptr : tunnelPathsBytes.constData();
    const char* roadPathsForTunnelPtr = roadPathsForTunnel.isEmpty() ? nullptr : roadPathsForTunnelBytes.constData();

    // 4. 调用关系检查核心接口
    SE_Error error = RelationshipCheck(
        xmlPathBytes.constData(),
        basePolygonPtr,
        basePointPtr,
        parsedPolygonPtr,
        parsedPointPtr,
        riverPolygonPtr,
        ditchFlowPathsPtr,
        ditchPathsPtr,
        bridgePathsPtr,
        roadPathsPtr,
        tunnelEntrancePtr,
        tunnelPathsPtr,
        roadPathsForTunnelPtr,
        logPathBytes.constData(),           // 第14个参数：日志路径
        resultLayerPathBytes.constData(),   // 第15个参数：错误SHP路径
        jsonPathBytes.constData(),          // 第16个参数：JSON报告路径
        scale
    );

    // 5. 解析结果
    QString tempJsonPath = jsonPath;
    if (!ParseJsonToCheckItemResult(tempJsonPath, result, checkItemName)) {
        result.id = "113";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        if (error != SE_ERROR_NONE) {
            errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
        }
        else {
            errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(tempJsonPath);
        }
        errorDetail["scale"] = scale;
        result.errors["error1"] = errorDetail;
    }
}

QStringList getActualLayerPaths(const QString& stdLayerNames, AlgorithmFilePaths& filePaths)
{
    QStringList resultPaths;
    if (filePaths.csvPath.isEmpty()) return resultPaths;

    // --- 已修复：通过类名直接调用静态函数，不再创建实例 ---
    QMap<QString, CsvLayerInfo> layerMap = map_check_task_manager::parseCsvToLayerMap(filePaths.csvPath);
    if (layerMap.isEmpty()) return resultPaths;

    QStringList stdNames = stdLayerNames.split(";", Qt::SkipEmptyParts);
    for (const QString& stdName : stdNames) {
        if (!layerMap.contains(stdName)) continue;
        CsvLayerInfo matchedLayers = layerMap.value(stdName);

        // --- 已修复：通过类名直接调用静态函数 ---
        map_check_task_manager::buildShpPaths(filePaths, matchedLayers);

        resultPaths.append(filePaths.actualShpList);
    }

    // 去重，因为多次调用 buildShpPaths 可能会添加重复路径
    resultPaths.removeDuplicates();

    return resultPaths;
}

// 要素超出边界检查307
void FeatureOutOfBoundaryCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "307";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    try {
        // 使用传入的路径参数
        QString shpPath = filePaths.shpPath;
        QString actualShpSemicolon = filePaths.actualShpSemicolon;
        QString standardLayer = filePaths.standardLayer;
        
        // 判断实际图层是否为空或不存在
        if (actualShpSemicolon.isEmpty()) {
            result.id = "307";
            result.name = checkItemName;
            result.status = "质检成功";
            result.err_exist = false;
            result.info["description"] = "实际图层不存在";
            return;
        }
        
        // 构建内图廓文件路径作为边界
        QString boundaryShpPath = QDir(shpPath).filePath("内图廓.shp");
        
        // 检查边界文件是否存在
        if (!QFile::exists(boundaryShpPath)) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "内图廓文件不存在";
            errorDetail["expected_file"] = "内图廓.shp";
            errorDetail["shp_path"] = shpPath;
            result.errors["error1"] = errorDetail;
            return;
        }
        
        // 根据标准图层名称匹配对应的XML文件
        QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
        
        // 创建输出目录结构
        OutputPaths outPaths = CreateOutputDirectories(outputDir);
        
        // 构建输出文件路径
        QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("要素超出边界_%1.json").arg(standardLayer));
        QString errorShpPath = QDir(outPaths.errorResultDir).filePath(QString("要素超出边界_%1_errors.shp").arg(standardLayer));
        QString logPath = QDir(outPaths.logDir).filePath(QString("要素超出边界_%1.log").arg(standardLayer));
        
        // QByteArray生命周期管理
        QByteArray boundaryBytes = boundaryShpPath.toUtf8();
        QByteArray featureBytes = actualShpSemicolon.toUtf8();
        QByteArray errorShpPathBytes = errorShpPath.toUtf8();
        QByteArray jsonPathBytes = jsonPath.toUtf8();
        QByteArray logPathBytes = logPath.toUtf8();
        
        const char* szBoundaryShpPath = boundaryBytes.constData();
        const char* szFeatureShpPaths = featureBytes.constData();
        const char* szOutputErrorLayerPath = errorShpPathBytes.constData();
        const char* szOutputJsonPath = jsonPathBytes.constData();
        const char* szOutputLogPath = logPathBytes.constData();
        
        // 调用C接口
        SE_Error error = ::FeatureOutOfBoundaryCheck(
            szBoundaryShpPath,
            szFeatureShpPaths,
            szOutputErrorLayerPath,
            szOutputJsonPath,
            szOutputLogPath
        );
        
        // 使用通用解析函数解析JSON并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "307";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QString("要素超出边界检查失败，错误码: %1").arg(error);
            errorDetail["boundary_shp"] = boundaryShpPath;
            errorDetail["feature_shp"] = actualShpSemicolon;
            errorDetail["xml_path"] = selectedXmlPath;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "307";
        }
    }
    catch (const std::exception& e) {
        result.id = "307";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}



//境界线越级检查
void BoundaryMissingLevelCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    Q_UNUSED(scale);

    // 质检管理系统中的检查项 ID：境界线越级检查为 114。
    result.id = "114";
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    const QString standardLayer = filePaths.standardLayer;
    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(
        QString("%1%2.json").arg(checkItemName, standardLayer));
    const QString logPath = QDir(outPaths.logDir).filePath(
        QString("境界线越级检查_%1.log").arg(standardLayer));
    QStringList runLog;
    runLog << "开始执行境界线越级检查。"
           << QString("标准图层：%1").arg(standardLayer)
           << QString("JSON 输出：%1").arg(jsonPath);

    const auto writeReport = [&jsonPath, &result]() {
        QJsonObject checkItem;
        checkItem["id"] = result.id;
        checkItem["name"] = result.name;
        checkItem["status"] = result.status;
        checkItem["err_exist"] = result.err_exist;
        checkItem["info"] = result.info;
        checkItem["errors"] = result.errors;

        QJsonObject root;
        root["check_item"] = checkItem;

        QFile reportFile(jsonPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
            reportFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    };

    const auto writeLog = [&logPath, &runLog]() {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << runLog.join("\n") << "\n";
        }
    };

    try {
        const QStringList inputPaths =
            filterBoundaryLevelLinePaths(filePaths.actualShpSemicolon);
        if (inputPaths.isEmpty()) {
            runLog << "未找到解析后的线图层，跳过检查。";
            result.info["description"] = QStringLiteral("\u5b9e\u9645\u7ebf\u56fe\u5c42\u4e0d\u5b58\u5728\uff0c\u5df2\u8df3\u8fc7\u5883\u754c\u7ebf\u8d8a\u7ea7\u68c0\u67e5");
            writeReport();
            writeLog();
            return;
        }
        runLog << QString("输入线图层：%1").arg(inputPaths.join(";"));

        QString xmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
        if (xmlPath.isEmpty() && !filePaths.xmlPaths.isEmpty())
            xmlPath = filePaths.xmlPaths.first();

        if (xmlPath.isEmpty()) {
            result.status = QStringLiteral("\u8d28\u68c0\u5931\u8d25");
            result.err_exist = true;
            QJsonObject error;
            error["description"] = QStringLiteral("\u672a\u627e\u5230 ID=114 \u5bf9\u5e94\u7684 XML \u89c4\u5219\u6587\u4ef6");
            error["standard_layer"] = standardLayer;
            result.errors["error1"] = error;
            runLog << "未找到 ID=114 对应的 XML 规则文件。";
            writeReport();
            writeLog();
            return;
        }
        runLog << QString("XML 规则：%1").arg(xmlPath);

        int totalProblemCount = 0;
        int errorIndex = 1;
        QJsonArray outputShpPaths;
        for (int i = 0; i < inputPaths.size(); ++i) {
            const QString inputPath = inputPaths.at(i).trimmed();
            if (inputPath.isEmpty())
                continue;

            const QString outputShpPath = QDir(outPaths.errorResultDir).filePath(
                QString("境界线越级检查_%1_errors.shp")
                    .arg(standardLayer)
                    );

            BoundaryMissingLevelCheckResult checkResult;
            if (!se_boundary_missing_level_check_algorithm::RunBoundaryMissingLevelCheck(
                    inputPath, xmlPath, outputShpPath, checkResult)) {
                result.status = QStringLiteral("\u8d28\u68c0\u5931\u8d25");
                result.err_exist = true;
                QJsonObject error;
                error["description"] = checkResult.message;
                error["input_shp"] = inputPath;
                error["xml_path"] = xmlPath;
                result.errors[QString("error%1").arg(errorIndex++)] = error;
                runLog << QString("检查失败：%1；原因：%2")
                    .arg(inputPath, checkResult.message);
                continue;
            }

            totalProblemCount += checkResult.problemCount;
            if (checkResult.problemCount > 0)
                outputShpPaths.append(outputShpPath);
            runLog << QString("检查完成：%1；问题数：%2。")
                .arg(inputPath)
                .arg(checkResult.problemCount);

            for (const QJsonValue& value : checkResult.problemDetails) {
                QJsonObject detail = value.toObject();
                QJsonObject error;
                error["issue"] = detail.value("issue").toString();
                result.errors[QString("error%1").arg(errorIndex++)] = error;
            }
        }

        if (result.status != QStringLiteral("\u8d28\u68c0\u5931\u8d25")) {
            result.status = QStringLiteral("\u8d28\u68c0\u5b8c\u6210");
            result.err_exist = totalProblemCount > 0;
        }
        result.info["description"] = result.err_exist
            ? QStringLiteral("\u5883\u754c\u7ebf\u8d8a\u7ea7\u68c0\u67e5\u5b8c\u6210\uff0c\u53d1\u73b0\u95ee\u9898\u8981\u7d20")
            : QStringLiteral("\u5883\u754c\u7ebf\u8d8a\u7ea7\u68c0\u67e5\u5b8c\u6210\uff0c\u672a\u53d1\u73b0\u95ee\u9898\u8981\u7d20");
        result.info["problem_count"] = totalProblemCount;
        result.info["xml_path"] = xmlPath;
        result.info["error_shp_paths"] = outputShpPaths;
    }
    catch (const std::exception& e) {
        result.status = QStringLiteral("\u8d28\u68c0\u5931\u8d25");
        result.err_exist = true;
        QJsonObject error;
        error["description"] = QString::fromLocal8Bit(e.what());
        result.errors["exception"] = error;
        runLog << QString("发生异常：%1").arg(QString::fromLocal8Bit(e.what()));
    }

    runLog << QString("最终状态：%1；问题数：%2。")
        .arg(result.status)
        .arg(result.info.value("problem_count").toInt());
    writeReport();
    writeLog();
}

// 212 的敏感词库属于当前图层规则。当前可用 XML 已由任务管理器按图层筛选，
// 此处只在其中定位包含 check_item id=212 的配置。
static QString FindSensitiveContentCheckXml(const QStringList& xmlPaths)
{
    for (const QString& xmlPath : xmlPaths) {
        QFile xmlFile(xmlPath);
        if (!xmlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        QDomDocument document;
        const bool parsed = document.setContent(&xmlFile);
        xmlFile.close();
        if (!parsed) {
            continue;
        }

        const QDomNodeList checkItems = document.elementsByTagName("check_item");
        for (int i = 0; i < checkItems.count(); ++i) {
            const QDomElement checkItem = checkItems.at(i).toElement();
            if (checkItem.firstChildElement("id").text().trimmed() == QStringLiteral("212")) {
                return xmlPath;
            }
        }
    }
    return QString();
}

// 115 - 行政区划面套合检查
void AdministrativeDivisionFitCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "115";
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(
        QStringLiteral("行政区划面套合检查_%1.json").arg(filePaths.standardLayer));
    const QString logPath = QDir(outPaths.logDir).filePath(
        QStringLiteral("行政区划面套合检查_%1.log").arg(filePaths.standardLayer));
    const QString outputBaseShpPath = QDir(outPaths.errorResultDir).filePath(
        QStringLiteral("行政区划面套合检查_%1_errors.shp").arg(filePaths.standardLayer));

    QStringList runLog;
    const auto appendRunLog = [&runLog](const QString& text) {
        runLog << QString("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
            .arg(text);
    };
    const auto writeReport = [&jsonPath, &result]() {
        QJsonObject checkItem;
        checkItem["id"] = result.id;
        checkItem["name"] = result.name;
        checkItem["status"] = result.status;
        checkItem["err_exist"] = result.err_exist;
        checkItem["info"] = result.info;
        checkItem["errors"] = result.errors;

        QJsonObject root;
        root["check_item"] = checkItem;
        QFile reportFile(jsonPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
            reportFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    };
    const auto writeLog = [&logPath, &runLog]() {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << runLog.join("\n") << "\n";
        }
    };

    appendRunLog("开始执行行政区划面套合检查。");

    // 境界线始终取当前任务图层的实际 SHP；行政区划面由 XML 的“关联图层”规则决定。
    QStringList linePaths;
    QStringList ignoredCurrentPolygonPaths;
    classifyAdministrativeFitPaths(filePaths.actualShpSemicolon, linePaths, ignoredCurrentPolygonPaths);
    appendRunLog(QString("当前任务图层按几何类型识别：境界线=%1；忽略的面图层=%2。")
        .arg(linePaths.join(";"), ignoredCurrentPolygonPaths.join(";")));

    QString xmlPath;
    QString associatedStandardLayer;
    for (const QString& candidatePath : filePaths.xmlPaths) {
        const QString ruleValue = getRuleValueFromXml(
            candidatePath, checkItemName, QStringLiteral("关联图层"), scale).trimmed();
        if (!ruleValue.isEmpty()) {
            xmlPath = candidatePath;
            associatedStandardLayer = ruleValue;
            break;
        }
    }
    if (xmlPath.isEmpty()) {
        xmlPath = MatchXmlByLayerName(filePaths.xmlPaths, checkItemName);
        if (xmlPath.isEmpty() && !filePaths.xmlPaths.isEmpty())
            xmlPath = filePaths.xmlPaths.first();
        if (!xmlPath.isEmpty()) {
            associatedStandardLayer = getRuleValueFromXml(
                xmlPath, checkItemName, QStringLiteral("关联图层"), scale).trimmed();
        }
    }

    QString associatedActualPaths;
    QStringList polygonPaths;
    if (!associatedStandardLayer.isEmpty()) {
        // 关联图层名是标准图层名，需通过 CSV 的“实际图层”列定位解析后的 SHP。
        associatedActualPaths = getPathsForStdLayers(
            associatedStandardLayer, filePaths.csvPath, filePaths.shpPath);
        // 同一图层若同时存在基础 SHP 和 _Polygon SHP，优先使用几何分类后的文件，避免重复检查。
        associatedActualPaths = filterPathsBySuffixPreferBase(associatedActualPaths, "_Polygon");
        QStringList ignoredAssociatedLinePaths;
        classifyAdministrativeFitPaths(associatedActualPaths, ignoredAssociatedLinePaths, polygonPaths);
    }
    appendRunLog(QString("关联图层：%1；关联图层实际路径=%2；行政区划面=%3。")
        .arg(associatedStandardLayer, associatedActualPaths, polygonPaths.join(";")));

    if (xmlPath.isEmpty() || associatedStandardLayer.isEmpty() ||
        linePaths.isEmpty() || polygonPaths.size() != 1) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject error;
        error["description"] = QStringLiteral("行政区划面套合检查需要 XML 中配置“关联图层”、至少一个境界线图层，以及恰好一个关联行政区划面图层。");
        error["boundary_actual_shp_paths"] = filePaths.actualShpSemicolon;
        error["line_shp_paths"] = linePaths.join(";");
        error["associated_standard_layer"] = associatedStandardLayer;
        error["associated_actual_shp_paths"] = associatedActualPaths;
        error["polygon_shp_paths"] = polygonPaths.join(";");
        error["xml_path"] = xmlPath;
        result.errors["error1"] = error;
        appendRunLog(error["description"].toString());
        writeReport();
        writeLog();
        return;
    }

    const QString polygonShpPath = polygonPaths.first();
    appendRunLog(QString("XML 规则：%1").arg(xmlPath));
    appendRunLog(QString("境界线：%1；行政区划面：%2。")
        .arg(linePaths.join(";"), polygonShpPath));
    appendRunLog(QString("错误结果基路径：%1").arg(outputBaseShpPath));

    AdministrativeFitCheckResult checkResult;
    const bool succeeded = se_administrative_division_fit_check_algorithm::RunAdministrativeFitCheck(
        polygonShpPath,
        linePaths.join(";"),
        xmlPath,
        scale,
        outputBaseShpPath,
        checkResult,
        appendRunLog);

    if (!succeeded) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject error;
        error["description"] = checkResult.message;
        error["line_shp_paths"] = linePaths.join(";");
        error["associated_standard_layer"] = associatedStandardLayer;
        error["associated_actual_shp_paths"] = associatedActualPaths;
        error["polygon_shp_path"] = polygonShpPath;
        error["xml_path"] = xmlPath;
        result.errors["error1"] = error;
    }
    else {
        result.status = QStringLiteral("质检完成");
        result.err_exist = checkResult.problemCount > 0;
        int errorIndex = 1;
        for (const QJsonValue& value : checkResult.problemDetails)
            result.errors[QString("error%1").arg(errorIndex++)] = value.toObject();
    }

    result.info["description"] = checkResult.message;
    result.info["problem_count"] = checkResult.problemCount;
    result.info["line_shp_paths"] = linePaths.join(";");
    result.info["associated_standard_layer"] = associatedStandardLayer;
    result.info["associated_actual_shp_paths"] = associatedActualPaths;
    result.info["polygon_shp_path"] = polygonShpPath;
    result.info["xml_path"] = xmlPath;
    result.info["error_shp_paths"] = QJsonArray::fromStringList(checkResult.outputPaths);
    appendRunLog(QString("最终状态：%1；问题数：%2。")
        .arg(result.status)
        .arg(checkResult.problemCount));
    writeReport();
    writeLog();
}

// 116 - 境界线缺失和跳绘检查
void BoundaryJumpDrawCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "116";
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString fileStem = QStringLiteral("境界线缺失和跳绘检查_%1").arg(filePaths.standardLayer);
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(fileStem + QStringLiteral(".json"));
    const QString logPath = QDir(outPaths.logDir).filePath(fileStem + QStringLiteral(".log"));
    const QString outputBaseShpPath = QDir(outPaths.errorResultDir).filePath(fileStem + QStringLiteral("_errors.shp"));

    QStringList runLog;
    const auto appendRunLog = [&runLog](const QString& text) {
        runLog << QString("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"))
            .arg(text);
    };
    const auto writeReport = [&jsonPath, &result]() {
        QJsonObject checkItem;
        checkItem["id"] = result.id;
        checkItem["name"] = result.name;
        checkItem["status"] = result.status;
        checkItem["err_exist"] = result.err_exist;
        checkItem["info"] = result.info;
        checkItem["errors"] = result.errors;
        QJsonObject root;
        root["check_item"] = checkItem;
        QFile reportFile(jsonPath);
        if (reportFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
            reportFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    };
    const auto writeLog = [&logPath, &runLog]() {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << runLog.join("\n") << "\n";
        }
    };

    appendRunLog("开始执行境界线缺失和跳绘检查。");

    QString xmlPath;
    QString associatedRuleValue;
    for (const QString& candidatePath : filePaths.xmlPaths) {
        const QString value = getRuleValueFromXml(
            candidatePath, checkItemName, QStringLiteral("关联图层"), scale).trimmed();
        if (!value.isEmpty()) {
            xmlPath = candidatePath;
            associatedRuleValue = value;
            break;
        }
    }
    if (xmlPath.isEmpty()) {
        xmlPath = MatchXmlByLayerName(filePaths.xmlPaths, filePaths.standardLayer);
        if (xmlPath.isEmpty() && !filePaths.xmlPaths.isEmpty())
            xmlPath = filePaths.xmlPaths.first();
        if (!xmlPath.isEmpty()) {
            associatedRuleValue = getRuleValueFromXml(
                xmlPath, checkItemName, QStringLiteral("关联图层"), scale).trimmed();
        }
    }

    const QStringList associatedStandardLayers = splitBoundaryJumpAssociatedLayers(associatedRuleValue);
    const QString associatedStandardLayerText = associatedStandardLayers.join(";");

    // 成果境界线和行政界线均取当前配置行：前者取实际图层，后者取 GDB 图层。
    // 116 的成果境界线只能输入一份线数据；同一实际图层的基础/分类文件由几何类型去重。
    const QStringList boundaryPaths = filterBoundaryLevelLinePaths(filePaths.actualShpSemicolon);
    const QStringList adminPaths = filterBoundaryLevelLinePaths(filePaths.gdbShpSemicolon);

    // 关联图层的 GDB 数据只作为原始线状要素，严禁把面要素送入跳绘关联计算。
    const QString associatedGdbPathText = getGdbPathsForStdLayers(
        associatedStandardLayerText, filePaths.csvPath, filePaths.gdbShpPath);
    const QStringList otherLinePaths = filterBoundaryLevelLinePaths(associatedGdbPathText);

    // 关联实际图层名称含“面”时只加载 _Polygon，其余只加载 _LineString；
    // 同名基础 SHP 不会与分类 SHP 重复加载。
    const QString associatedActualPathText = getPathsForStdLayers(
        associatedStandardLayerText, filePaths.csvPath, filePaths.shpPath);
    const QStringList generalizedPaths = selectBoundaryJumpParsedGeometryPaths(associatedActualPathText);

    // 将任务管理器传入的数据与最终调用数据完整写入日志，便于定位 CSV 映射、
    // 几何类型过滤或关联图层去重导致的问题。
    appendRunLog("116 调用参数开始：");
    appendRunLog(QString("标准图层=%1；质检项=%2；比例尺=1:%3。")
        .arg(filePaths.standardLayer, checkItemName).arg(scale));
    appendRunLog(QString("XML 规则文件=%1；关联图层规则原值=%2；解析后的关联标准图层=%3。")
        .arg(xmlPath, associatedRuleValue, associatedStandardLayerText));
    appendRunLog(QString("配置的成果实际图层原始路径=%1。")
        .arg(filePaths.actualShpSemicolon));
    appendRunLog(QString("最终成果境界线(实际线图层)=%1。")
        .arg(boundaryPaths.join(";")));
    appendRunLog(QString("配置的行政界线 GDB 原始路径=%1。")
        .arg(filePaths.gdbShpSemicolon));
    appendRunLog(QString("最终行政界线(GDB 线图层)=%1。")
        .arg(adminPaths.join(";")));
    appendRunLog(QString("关联图层解析出的 GDB 原始路径=%1；过滤面要素后的原始线要素=%2。")
        .arg(associatedGdbPathText, otherLinePaths.join(";")));
    appendRunLog(QString("关联图层解析出的实际图层原始路径=%1；按“面→Polygon、非面→LineString”筛选后的图面要素=%2。")
        .arg(associatedActualPathText, generalizedPaths.join(";")));
    appendRunLog(QString("错误结果输出基路径=%1。")
        .arg(outputBaseShpPath));
    appendRunLog("116 调用参数结束。");

    if (xmlPath.isEmpty() || associatedStandardLayers.isEmpty() || boundaryPaths.size() != 1
        || adminPaths.size() != 1 || otherLinePaths.isEmpty() || generalizedPaths.isEmpty()) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject error;
        error["description"] = QStringLiteral("境界线缺失和跳绘检查需要 ID=116 的“关联图层”规则、唯一的实际境界线和 GDB 行政界线、至少一个关联 GDB 线要素以及至少一个关联解析后线/面要素。");
        error["xml_path"] = xmlPath;
        error["associated_standard_layers"] = associatedStandardLayerText;
        error["boundary_actual_paths"] = boundaryPaths.join(";");
        error["admin_gdb_paths"] = adminPaths.join(";");
        error["associated_gdb_paths"] = associatedGdbPathText;
        error["associated_gdb_line_paths"] = otherLinePaths.join(";");
        error["associated_actual_paths"] = associatedActualPathText;
        error["generalized_paths"] = generalizedPaths.join(";");
        result.errors["error1"] = error;
        appendRunLog(error["description"].toString());
        writeReport();
        writeLog();
        return;
    }

    BoundaryJumpCheckResult checkResult;
    const bool succeeded = se_boundary_jump_draw_check_algorithm::RunBoundaryJumpDrawCheck(
        boundaryPaths.first(), adminPaths.first(), otherLinePaths.join(";"),
        generalizedPaths.join(";"), xmlPath, scale, outputBaseShpPath, checkResult, appendRunLog);

    if (!succeeded) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject error;
        error["description"] = checkResult.message;
        result.errors["error1"] = error;
    }
    else {
        result.status = QStringLiteral("质检完成");
        result.err_exist = checkResult.problemCount > 0;
        int errorIndex = 1;
        for (const QJsonValue& value : checkResult.problemDetails)
            result.errors[QString("error%1").arg(errorIndex++)] = value.toObject();
    }

    result.info["description"] = checkResult.message;
    result.info["problem_count"] = checkResult.problemCount;
    result.info["xml_path"] = xmlPath;
    result.info["associated_standard_layers"] = associatedStandardLayerText;
    result.info["boundary_actual_path"] = boundaryPaths.first();
    result.info["admin_gdb_path"] = adminPaths.first();
    result.info["associated_gdb_line_paths"] = otherLinePaths.join(";");
    result.info["generalized_paths"] = generalizedPaths.join(";");
    result.info["error_shp_paths"] = QJsonArray::fromStringList(checkResult.outputPaths);
    appendRunLog(QString("最终状态：%1；问题数：%2。")
        .arg(result.status).arg(checkResult.problemCount));
    writeReport();
    writeLog();
}

// 120 的“关联图层”允许在一个 check_item 中重复出现，不能使用只返回首个值的通用读取器。
static QStringList spacingAssociatedLayers(const QString& xmlPath, int scale)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QDomDocument document;
    if (!document.setContent(&file)) return {};

    QDomElement chosenScale;
    QDomElement defaultScale;
    for (QDomElement element = document.documentElement().firstChildElement(QStringLiteral("scale"));
         !element.isNull(); element = element.nextSiblingElement(QStringLiteral("scale"))) {
        const QString value = element.firstChildElement(QStringLiteral("value")).text().trimmed();
        if (value.isEmpty()) defaultScale = element;
        else if (value.toInt() == scale) { chosenScale = element; break; }
    }
    if (chosenScale.isNull()) chosenScale = defaultScale;
    QStringList names;
    for (QDomElement item = chosenScale.firstChildElement(QStringLiteral("check_item"));
         !item.isNull(); item = item.nextSiblingElement(QStringLiteral("check_item"))) {
        if (item.firstChildElement(QStringLiteral("id")).text().trimmed() != QStringLiteral("120")
            || item.firstChildElement(QStringLiteral("name")).text().trimmed()
                != QStringLiteral("要素间隔检查")) continue;
        for (QDomElement rule = item.firstChildElement(QStringLiteral("rule"));
             !rule.isNull(); rule = rule.nextSiblingElement(QStringLiteral("rule"))) {
            if (rule.firstChildElement(QStringLiteral("name")).text().trimmed()
                != QStringLiteral("关联图层")) continue;
            for (const QString& name : splitBoundaryJumpAssociatedLayers(
                     rule.firstChildElement(QStringLiteral("value")).text())) {
                if (!names.contains(name)) names << name;
            }
        }
        break;
    }
    return names;
}

static QStringList spacingTypedPaths(const QString& paths, const QStringList& suffixes,
                                     bool allowBase)
{
    const QStringList input = splitExistingShpPaths(paths);
    QStringList typed;
    QSet<QString> typedBases;
    for (const QString& path : input) {
        const QString baseName = QFileInfo(path).completeBaseName();
        for (const QString& suffix : suffixes) {
            if (baseName.endsWith(suffix, Qt::CaseInsensitive)) {
                typed << path;
                typedBases.insert(baseName.left(baseName.size() - suffix.size()).toLower());
                break;
            }
        }
    }
    if (allowBase) {
        const QStringList allSuffixes = { QStringLiteral("_Point"), QStringLiteral("_LineString"),
            QStringLiteral("_Polygon"), QStringLiteral("_MultiPoint"),
            QStringLiteral("_MultiLineString"), QStringLiteral("_MultiPolygon") };
        for (const QString& path : input) {
            const QString baseName = QFileInfo(path).completeBaseName();
            bool classified = false;
            for (const QString& suffix : allSuffixes)
                classified = classified || baseName.endsWith(suffix, Qt::CaseInsensitive);
            if (!classified && !typedBases.contains(baseName.toLower())) typed << path;
        }
    }
    return typed;
}

static QStringList spacingGdbPaths(const QString& standardLayer,
                                   const AlgorithmFilePaths& filePaths,
                                   const QString& requiredBaseName = QString())
{
    const QStringList paths = splitExistingShpPaths(getGdbPathsForStdLayers(
        standardLayer, filePaths.csvPath, filePaths.gdbShpPath));
    if (requiredBaseName.isEmpty()) return paths;
    QStringList selected;
    for (const QString& path : paths)
        if (QFileInfo(path).completeBaseName().compare(requiredBaseName, Qt::CaseInsensitive) == 0)
            selected << path;
    return selected;
}

static QStringList spacingPathsWithGeometry(const QStringList& paths,
                                            QgsWkbTypes::GeometryType geometryType)
{
    QStringList selected;
    for (const QString& path : paths) {
        QgsVectorLayer layer(path, QFileInfo(path).completeBaseName(), QStringLiteral("ogr"));
        if (layer.isValid() && QgsWkbTypes::geometryType(layer.wkbType()) == geometryType)
            selected << path;
    }
    return selected;
}

static void clearOldSpacingOutput(const QString& outputShpPath, QStringList& log)
{
    const QString outputBase = outputShpPath.left(outputShpPath.size() - 4);
    const QStringList sidecars = { QStringLiteral(".shp"), QStringLiteral(".shx"),
        QStringLiteral(".dbf"), QStringLiteral(".prj"), QStringLiteral(".cpg"),
        QStringLiteral(".qpj") };
    for (const QString& extension : sidecars) {
        const QString sidecar = outputBase + extension;
        if (QFile::exists(sidecar) && !QFile::remove(sidecar))
            log << QStringLiteral("无法清理本检查的旧错误结果文件：%1").arg(sidecar);
    }
}

void FeatureSpacingCheckWrapper(const QString& checkItemName,
                                const AlgorithmFilePaths& filePaths, int scale,
                                const QString& outputDir, CheckItemResult& result)
{
    result.id = QStringLiteral("120");
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString fileStem = QStringLiteral("要素间隔检查_%1").arg(filePaths.standardLayer);
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(fileStem + QStringLiteral(".json"));
    const QString logPath = QDir(outPaths.logDir).filePath(fileStem + QStringLiteral(".log"));
    const QString outputShpPath = QDir(outPaths.errorResultDir).filePath(
        fileStem + QStringLiteral("_errors.shp"));
    QStringList runLog;
    const auto appendRunLog = [&runLog](const QString& text) {
        runLog << QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), text);
    };
    const auto writeReport = [&]() {
        QJsonObject checkItem;
        checkItem[QStringLiteral("id")] = result.id;
        checkItem[QStringLiteral("name")] = result.name;
        checkItem[QStringLiteral("status")] = result.status;
        checkItem[QStringLiteral("err_exist")] = result.err_exist;
        checkItem[QStringLiteral("info")] = result.info;
        checkItem[QStringLiteral("errors")] = result.errors;
        QJsonObject root;
        root[QStringLiteral("check_item")] = checkItem;
        QFile report(jsonPath);
        if (report.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
            report.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        QFile log(logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&log);
            stream.setCodec("UTF-8");
            stream << runLog.join(QStringLiteral("\n")) << QStringLiteral("\n");
        }
    };
    const auto fail = [&](const QString& description) {
        clearOldSpacingOutput(outputShpPath, runLog);
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject error;
        error[QStringLiteral("description")] = description;
        result.errors[QStringLiteral("error1")] = error;
        result.info[QStringLiteral("description")] = description;
        appendRunLog(description);
        writeReport();
    };

    QString xmlPath;
    for (const QString& path : filePaths.xmlPaths) {
        if (QFileInfo(path).completeBaseName().compare(filePaths.standardLayer, Qt::CaseInsensitive) == 0) {
            xmlPath = path;
            break;
        }
    }
    if (xmlPath.isEmpty()) {
        fail(QStringLiteral("要素间隔检查找不到与标准图层“%1”同名的 XML。")
                 .arg(filePaths.standardLayer));
        return;
    }
    if (scale <= 0) {
        fail(QStringLiteral("要素间隔检查缺少有效比例尺。"));
        return;
    }
    const QString layerName = filePaths.standardLayer;
    const bool spring = layerName == QStringLiteral("泉");
    const bool well = layerName == QStringLiteral("井");
    const bool village = layerName == QStringLiteral("行政村");
    const bool river = layerName.contains(QStringLiteral("河流"));
    const bool railway = layerName == QStringLiteral("铁路隧道");
    const bool road = layerName == QStringLiteral("公路隧道");
    if (!spring && !well && !village && !river && !railway && !road) {
        fail(QStringLiteral("标准图层“%1”没有对应的要素间隔算法。")
                 .arg(layerName));
        return;
    }

    const QStringList associatedNames = spacingAssociatedLayers(xmlPath, scale);
    if (river && associatedNames.isEmpty()) {
        fail(QStringLiteral("河流间隔检查需要在 XML 中配置“关联图层”=主区面标准图层，以便从配置表定位实际面图层。"));
        return;
    }
    QStringList sourcePaths = splitExistingShpPaths(filePaths.gdbShpSemicolon);
    QStringList targetPaths;
    QStringList associatedPaths;
    const QStringList actualPaths = splitExistingShpPaths(filePaths.actualShpSemicolon);
    if (spring || well)
        sourcePaths = spacingPathsWithGeometry(sourcePaths, QgsWkbTypes::PointGeometry);
    else if (river)
        sourcePaths = spacingPathsWithGeometry(sourcePaths, QgsWkbTypes::LineGeometry);
    if (spring || well || village || river || railway || road) {
        targetPaths = spacingPathsWithGeometry(spacingTypedPaths(actualPaths.join(';'),
            { QStringLiteral("_LineString"), QStringLiteral("_MultiLineString") }, true),
            QgsWkbTypes::LineGeometry);
    }
    if (spring) {
        for (const QString& name : associatedNames) {
            const QString mapped = getPathsForStdLayers(name, filePaths.csvPath, filePaths.shpPath);
            // 泉只使用关联实际图层的点、线分类文件，不把基础/面文件重复计入缓冲区。
            const QStringList paths = spacingTypedPaths(mapped,
                { QStringLiteral("_LineString"), QStringLiteral("_Point") }, false);
            if (paths.isEmpty()) {
                fail(QStringLiteral("泉关联标准图层“%1”没有找到实际图层的 _LineString/_Point SHP。")
                         .arg(name));
                return;
            }
            for (const QString& path : paths)
                if (!associatedPaths.contains(path)) associatedPaths << path;
        }
    } else if (village || river) {
        for (const QString& name : associatedNames) {
            const QString mapped = getPathsForStdLayers(name, filePaths.csvPath, filePaths.shpPath);
            const QStringList paths = spacingPathsWithGeometry(spacingTypedPaths(mapped,
                { QStringLiteral("_Polygon"), QStringLiteral("_MultiPolygon") }, true),
                QgsWkbTypes::PolygonGeometry);
            for (const QString& path : paths)
                if (!associatedPaths.contains(path)) associatedPaths << path;
        }
    } else if (railway || road) {
        sourcePaths.clear();
        for (const QString& name : associatedNames) {
            if (railway && name == QStringLiteral("火车站")) {
                const QString mapped = getPathsForStdLayers(name, filePaths.csvPath, filePaths.shpPath);
                const QStringList paths = splitExistingShpPaths(mapped);
                // 火车站取配置行的实际图层；基础 SHP 优先，避免基础/分类重复加载。
                QString stationPath;
                for (const QString& path : paths)
                    if (QFileInfo(path).completeBaseName() == QStringLiteral("火车站")) {
                        stationPath = path;
                        break;
                    }
                if (stationPath.isEmpty()) {
                    for (const QString& path : paths)
                        if (!QFileInfo(path).completeBaseName().contains(QStringLiteral("标注"))) {
                            stationPath = path;
                            break;
                        }
                }
                if (stationPath.isEmpty()) {
                    fail(QStringLiteral("铁路关联图层“火车站”未映射到有效实际 SHP。"));
                    return;
                }
                associatedPaths << stationPath;
            } else {
                const QString required = railway ? QStringLiteral("LRRL") : QStringLiteral("LRDL");
                const QStringList paths = spacingGdbPaths(name, filePaths, required);
                if (paths.isEmpty()) {
                    fail(QStringLiteral("%1关联标准图层“%2”未映射到 %3 GDB SHP。")
                             .arg(railway ? QStringLiteral("铁路") : QStringLiteral("公路"), name, required));
                    return;
                }
                for (const QString& path : paths)
                    if (!sourcePaths.contains(path)) sourcePaths << path;
            }
        }
    }

    if ((spring || village || river || railway) && associatedPaths.isEmpty()) {
        fail(QStringLiteral("“%1”缺少 XML 指定的有效关联实际图层。")
                 .arg(layerName));
        return;
    }
    if ((spring || well || river || railway || road) && sourcePaths.isEmpty()) {
        fail(QStringLiteral("“%1”缺少有效的源 GDB SHP。")
                 .arg(layerName));
        return;
    }
    if (targetPaths.isEmpty()) {
        fail(QStringLiteral("“%1”缺少有效的解析后待检线图层。")
                 .arg(layerName));
        return;
    }
    appendRunLog(QStringLiteral("开始执行要素间隔检查：标准图层=%1；比例尺=1:%2。")
                     .arg(layerName).arg(scale));
    appendRunLog(QStringLiteral("XML=%1；关联标准图层=%2。")
                     .arg(xmlPath, associatedNames.join(';')));
    appendRunLog(QStringLiteral("源 GDB=%1；关联实际图层=%2；待检解析图层=%3。")
                     .arg(sourcePaths.join(';'), associatedPaths.join(';'), targetPaths.join(';')));
    appendRunLog(QStringLiteral("错误结果输出路径=%1。")
                     .arg(outputShpPath));

    FeatureSpacingCheckDialog checker;
    const FeatureSpacingRunResult run = checker.runConfigured(
        sourcePaths, associatedPaths, targetPaths, xmlPath, scale, outputShpPath);
    for (const QString& message : run.logs) appendRunLog(message);
    // 同名旧结果不能在本次零错误/失败时被任务管理器误认作新结果。
    if (run.outputPaths.isEmpty()) clearOldSpacingOutput(outputShpPath, runLog);
    result.status = run.succeeded ? QStringLiteral("质检完成") : QStringLiteral("质检失败");
    result.err_exist = !run.succeeded || run.problemCount > 0;
    result.info[QStringLiteral("description")] = run.logs.isEmpty()
        ? QStringLiteral("要素间隔检查未产生结束信息。") : run.logs.last();
    result.info[QStringLiteral("problem_count")] = run.problemCount;
    result.info[QStringLiteral("xml_path")] = xmlPath;
    result.info[QStringLiteral("associated_standard_layers")] = associatedNames.join(';');
    result.info[QStringLiteral("source_gdb_paths")] = sourcePaths.join(';');
    result.info[QStringLiteral("associated_actual_paths")] = associatedPaths.join(';');
    result.info[QStringLiteral("target_actual_paths")] = targetPaths.join(';');
    result.info[QStringLiteral("error_shp_paths")] = QJsonArray::fromStringList(run.outputPaths);
    if (!run.succeeded) {
        QJsonObject error;
        error[QStringLiteral("description")] = result.info.value(QStringLiteral("description"));
        result.errors[QStringLiteral("error1")] = error;
    } else if (!run.outputPaths.isEmpty()) {
        QgsVectorLayer errorsLayer(run.outputPaths.first(), QStringLiteral("要素间隔错误"), QStringLiteral("ogr"));
        if (errorsLayer.isValid()) {
            int errorIndex = 1;
            QgsFeature feature;
            for (QgsFeatureIterator it = errorsLayer.getFeatures(); it.nextFeature(feature);) {
                QJsonObject detail;
                for (int i = 0; i < errorsLayer.fields().size(); ++i) {
                    const QVariant value = feature.attribute(i);
                    detail[errorsLayer.fields().at(i).name()] = value.isNull()
                        ? QJsonValue() : QJsonValue::fromVariant(value);
                }
                detail[QStringLiteral("error_shp_path")] = run.outputPaths.first();
                result.errors[QStringLiteral("error%1").arg(errorIndex++)] = detail;
            }
        }
    }
    appendRunLog(QStringLiteral("最终状态：%1；问题数：%2。")
                     .arg(result.status).arg(run.problemCount));
    writeReport();
}

// ========== 注记检查算法实现 ========== 
// 201 - 注记字体检查
void AnnotationFontCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    Q_UNUSED(scale); // scale参数已直接传递给C函数
    
    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    
    // 智能筛选逻辑：优先使用带几何类型后缀的注记文件，如果没有则使用所有文件
    QString annotShpSemicolon;
    QStringList annotShpList = filePaths.annotShpSemicolon.split(";", Qt::SkipEmptyParts);
    QStringList filteredAnnotShps;
    for (const QString& shp : annotShpList) {
        QString trimmedShp = shp.trimmed();
        // 注记文件通常是Polygon类型
        if (trimmedShp.endsWith("_Polygon.shp", Qt::CaseInsensitive)) {
            filteredAnnotShps.append(trimmedShp);
        }
    }
    // 如果找到了带几何类型后缀的文件，使用筛选结果；否则使用所有文件
    annotShpSemicolon = filteredAnnotShps.isEmpty() ? filePaths.annotShpSemicolon : filteredAnnotShps.join(";");
    // 判断实际图层是否为空或不存在
    if (annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "201";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 根据XML文件名查找200算法生成的匹配结果文件(算法201依赖算法200)
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "201";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记字体_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("注记字体_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("注记字体_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szShpFilePath = matchResultBytes.constData();  // 使用200算法的匹配结果
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    const char* szResultShpPath = resultLayerPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szShpFilePath = "D:\\Desktop\\a测试数据\\永登县shp新版\\泉_标注.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\泉_标注.xml";
    */
    
    try {
        // 调试输出：检查参数是否有效
        qDebug() << "[算法201] 开始调用 AnnotationFontCheck";
        qDebug() << "  shp:" << QString::fromUtf8(szShpFilePath);
        qDebug() << "  xml:" << QString::fromUtf8(szSchemaXmlPath);
        qDebug() << "  scale:" << scale;
        qDebug() << "  result:" << QString::fromUtf8(szResultShpPath);
        qDebug() << "  log:" << QString::fromUtf8(szLogPath);
        qDebug() << "  json:" << QString::fromUtf8(szReportPath);
        
        // 调用C接口（新接口：直接传JSON路径，不再使用缓冲区）
        SE_Error error = ::AnnotationFontCheck(
            szShpFilePath,
            szSchemaXmlPath,
            scale,
            szResultShpPath,
            szLogPath,
            szReportPath
        );
        
        qDebug() << "[算法201] AnnotationFontCheck 返回:" << error;
        
        if (error == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                // 解析失败时的处理
                result.id = "201";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
                errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
                errorDetail["scale"] = scale;
                result.errors["error1"] = errorDetail;
            } else {
                // 确保ID正确
                result.id = "201";
            }
        } else {
            result.id = "201";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "201";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 202 - 注记字号检查
void AnnotationFontSizeCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    
    // 智能筛选逻辑：优先使用带几何类型后缀的注记文件，如果没有则使用所有文件
    QString annotShpSemicolon;
    QStringList annotShpList = filePaths.annotShpSemicolon.split(";", Qt::SkipEmptyParts);
    QStringList filteredAnnotShps;
    for (const QString& shp : annotShpList) {
        QString trimmedShp = shp.trimmed();
        // 注记文件通常是Polygon类型
        if (trimmedShp.endsWith("_Polygon.shp", Qt::CaseInsensitive)) {
            filteredAnnotShps.append(trimmedShp);
        }
    }
    // 如果找到了带几何类型后缀的文件，使用筛选结果；否则使用所有文件
    annotShpSemicolon = filteredAnnotShps.isEmpty() ? filePaths.annotShpSemicolon : filteredAnnotShps.join(";");
    
     if (annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "202";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 根据XML文件名查找200算法生成的匹配结果文件(算法202依赖算法200)
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "202";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记字号_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("注记字号_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("注记字号_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szShpFilePath = matchResultBytes.constData();  // 使用200算法的匹配结果
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szResultShpPath = resultLayerPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szShpFilePath = "D:\\Desktop\\a测试数据\\永登县shp新版\\泉_标注.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\泉_标注.xml";
    */
    
    try {
        SE_Error error = ::AnnotationFontSizeCheck(
            szShpFilePath,
            szSchemaXmlPath,
            scale,
            szResultShpPath,
            szLogPath,
            szReportPath
        );
        
        if (error == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "202";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
                errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
                errorDetail["scale"] = scale;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "202";
            }
        } else {
            result.id = "202";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "202";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 203 - 注记颜色检查
void AnnotationColorCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    
    // 智能筛选逻辑：优先使用带几何类型后缀的注记文件，如果没有则使用所有文件
    QString annotShpSemicolon;
    QStringList annotShpList = filePaths.annotShpSemicolon.split(";", Qt::SkipEmptyParts);
    QStringList filteredAnnotShps;
    for (const QString& shp : annotShpList) {
        QString trimmedShp = shp.trimmed();
        // 注记文件通常是Polygon类型
        if (trimmedShp.endsWith("_Polygon.shp", Qt::CaseInsensitive)) {
            filteredAnnotShps.append(trimmedShp);
        }
    }
    // 如果找到了带几何类型后缀的文件，使用筛选结果；否则使用所有文件
    annotShpSemicolon = filteredAnnotShps.isEmpty() ? filePaths.annotShpSemicolon : filteredAnnotShps.join(";");
    
     if (annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "203";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 根据XML文件名查找200算法生成的匹配结果文件(算法203依赖算法200)
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "203";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记颜色_%1.json").arg(standardLayer));
    QString resultLayerPath = QDir(outPaths.errorResultDir).filePath(QString("注记颜色_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("注记颜色_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultLayerPathBytes = resultLayerPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szShpFilePath = matchResultBytes.constData();  // 使用200算法的匹配结果
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szResultShpPath = resultLayerPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    /* 原硬编码测试路径(已禁用):
    const char* szShpFilePath = "D:\\Desktop\\a测试数据\\永登县shp新版\\泉_标注.shp";
    const char* szSchemaXmlPath = "D:\\Desktop\\a测试数据\\xml测试\\泉_标注.xml";
    */
    
    try {
        SE_Error error = ::AnnotationColorCheck(
            szShpFilePath,
            szSchemaXmlPath,
            scale,
            szResultShpPath,
            szLogPath,
            szReportPath
        );
        
        if (error == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "203";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
                errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
                errorDetail["scale"] = scale;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "203";
            }
        } else {
            result.id = "203";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            errorDetail["anno_shp"] = QString::fromUtf8(szShpFilePath);
            errorDetail["xml_path"] = QString::fromUtf8(szSchemaXmlPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "203";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 205 - 注记斜体检查
void AnnotationItalicCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "204";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    // TODO: 实现注记斜体检查
    result.status = "质检失败";
    result.err_exist = true;
    QJsonObject errorDetail;
    errorDetail["description"] = "注记斜体检查算法暂未实现";
    result.errors["not_implemented"] = errorDetail;
}

// 206 - 注记重叠检查
void AnnotationOverlapCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "206";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    // 从 shpPath 下读取 layer_classification.json
    QString shpPath = filePaths.shpPath;
    QString jsonFilePath = QDir(shpPath).filePath("layer_classification.json");
    
    // 检查 JSON 文件是否存在
    if (!QFile::exists(jsonFilePath)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("找不到图层分类配置文件: %1").arg(jsonFilePath);
        result.errors["error1"] = errorDetail;
        return;
    }
    
    // 读取并解析 JSON 文件
    QFile jsonFile(jsonFilePath);
    if (!jsonFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("无法打开图层分类配置文件: %1").arg(jsonFilePath);
        result.errors["error1"] = errorDetail;
        return;
    }
    
    QJsonParseError parseError;
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);
    jsonFile.close();
    
    if (parseError.error != QJsonParseError::NoError) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("JSON解析失败: %1").arg(parseError.errorString());
        result.errors["error1"] = errorDetail;
        return;
    }
    
    QJsonObject jsonObj = jsonDoc.object();
    QJsonArray symbolLayers = jsonObj["symbol_layers"].toArray();
    QJsonArray annoLayers = jsonObj["anno_layers"].toArray();
    
    // 拼接符号图层路径
    QStringList symbolLayerPaths;
    for (const QJsonValue& val : symbolLayers) {
        QString layerName = val.toString();
        if (!layerName.isEmpty()) {
            QString fullPath = QDir(shpPath).filePath(layerName);
            symbolLayerPaths.append(fullPath);
        }
    }
    
    // 拼接注记图层路径
    QStringList annoLayerPaths;
    for (const QJsonValue& val : annoLayers) {
        QString layerName = val.toString();
        if (!layerName.isEmpty()) {
            QString fullPath = QDir(shpPath).filePath(layerName);
            annoLayerPaths.append(fullPath);
        }
    }
    
    // 用分号连接路径
    QString szSymbolLayerPaths = symbolLayerPaths.join(";");
    QString szAnnotationLayerPaths = annoLayerPaths.join(";");
    
    // 获取标准图层名称并匹配XML
    QString standardLayer = filePaths.standardLayer;
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    /* 原硬编码测试路径(已禁用):
    QString xmlPaths = QString("D:/Desktop/a测试数据/xml测试/全局重叠检查规则.xml");
    */
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记重叠_%1.json").arg(standardLayer));
    QString resultPath = QDir(outPaths.errorResultDir).filePath(QString("注记重叠_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("注记重叠_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray symbolLayerPathsBytes = szSymbolLayerPaths.toUtf8();
    QByteArray annoLayerPathsBytes = szAnnotationLayerPaths.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultPathBytes = resultPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szSymbolPaths = symbolLayerPathsBytes.constData();
    const char* szAnnoPaths = annoLayerPathsBytes.constData();
    const char* szXmlPath = xmlPathBytes.constData();
    const char* szResultPath = resultPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    try {
        SE_Error error = ::AnnotationOverlapCheck(
            szSymbolPaths,
            szAnnoPaths,
            szXmlPath,
            scale,
            szResultPath,
            szLogPath,
            szReportPath
        );
        
        if (error == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "206";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["symbol_layers"] = szSymbolLayerPaths;
                errorDetail["anno_layers"] = szAnnotationLayerPaths;
                errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
                errorDetail["scale"] = scale;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "206";
            }
        } else {
            result.id = "206";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("注记压盖检查失败，错误码: %1").arg(error);
            errorDetail["symbol_layers"] = szSymbolLayerPaths;
            errorDetail["anno_layers"] = szAnnotationLayerPaths;
            errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "206";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//飞地漏绘检查
void EnclaveMissingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // GDB文件夹路径 + 成果SHP路径
    QString gdbFolderPath = filePaths.gdbShpPath;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;

    // 从CSV获取GDB图层名称（第2列：gdb图层），必传
    QString gdbLayerName = filePaths.gdbLayer.split('|', Qt::SkipEmptyParts).value(0).trimmed();

    if (gdbFolderPath.isEmpty() || actualShpSemicolon.isEmpty() || gdbLayerName.isEmpty()) {
        result.id = "117";
        result.name = checkItemName;
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info["description"] = QStringLiteral("GDB路径、成果SHP或CSV的gdb图层为空");
        return;
    }

    // 按质检项名称匹配XML（兼容文件名不匹配的情况）
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, checkItemName);

    OutputPaths outPaths = CreateOutputDirectories(outputDir);

    QString shpPath = QDir(outPaths.errorResultDir).filePath(QStringLiteral("飞地漏绘_%1_%2_errors.shp").arg(gdbLayerName, filePaths.standardLayer));
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QStringLiteral("飞地漏绘_%1_%2.json").arg(gdbLayerName, filePaths.standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QStringLiteral("飞地漏绘_%1_%2_%3.log")
        .arg(gdbLayerName, filePaths.standardLayer, QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));

    QByteArray gdbBytes = gdbFolderPath.toUtf8();
    QByteArray shpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlBytes = selectedXmlPath.toUtf8();
    QByteArray outShpBytes = shpPath.toUtf8();
    QByteArray jsonBytes = jsonPath.toUtf8();
    QByteArray logBytes = logPath.toUtf8();
    QByteArray gdbLayerBytes = gdbLayerName.toUtf8();

    SE_Error error = EnclaveMissingCheck(
        gdbBytes.constData(),
        shpBytes.constData(),
        xmlBytes.constData(),
        scale,
        outShpBytes.constData(),
        jsonBytes.constData(),
        logBytes.constData(),
        gdbLayerBytes.constData());

    if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
        result.id = "117";
        result.name = checkItemName;
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject errorDetail;
        if (error != SE_ERROR_NONE) {
            errorDetail["error"] = QStringLiteral("算法执行失败，错误码: %1").arg(error);
        }
        else {
            errorDetail["error"] = QStringLiteral("无法解析结果文件，路径: %1").arg(jsonPath);
        }
        errorDetail["gdb_path"] = gdbFolderPath;
        errorDetail["gdb_layer"] = gdbLayerName;
        errorDetail["shp_path"] = actualShpSemicolon;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["scale"] = scale;
        result.errors["error1"] = errorDetail;
    }
    else {
        result.id = "117";
    }
}

// 200 - 要素-注记匹配
void AnnotationMatchFeaturesWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    if (filePaths.annotShpSemicolon.isEmpty()) {
         //实际注记图层不存在，直接返回成功状态
        result.id = "200";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }

    result.id = "200";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    // 使用实际路径参数
    QString standardLayer = filePaths.standardLayer;  // 第1列：标准图层（与子任务名称匹配），从CSV读取
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    
    // 获取标准图层名称并匹配XML
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 智能筛选逻辑：优先使用带几何类型后缀的文件，如果没有则使用所有文件
    QString actualShpSemicolon;
    QStringList actualShpList = filePaths.actualShpSemicolon.split(";", Qt::SkipEmptyParts);
    QStringList filteredActualShps;
    for (const QString& shp : actualShpList) {
        if (shp.trimmed().endsWith("_LineString.shp", Qt::CaseInsensitive)) {
            filteredActualShps.append(shp.trimmed());
        }
    }
    // 如果找到了带几何类型后缀的文件，使用筛选结果；否则使用所有文件
    actualShpSemicolon = filteredActualShps.isEmpty() ? filePaths.actualShpSemicolon : filteredActualShps.join(";");
    
    QString annotShpSemicolon;
    QStringList annotShpList = filePaths.annotShpSemicolon.split(";", Qt::SkipEmptyParts);
    QStringList filteredAnnotShps;
    for (const QString& shp : annotShpList) {
        if (shp.trimmed().endsWith("_Polygon.shp", Qt::CaseInsensitive)) {
            filteredAnnotShps.append(shp.trimmed());
        }
    }
    // 如果找到了带几何类型后缀的文件，使用筛选结果；否则使用所有文件
    annotShpSemicolon = filteredAnnotShps.isEmpty() ? filePaths.annotShpSemicolon : filteredAnnotShps.join(";");
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 从XML路径提取文件名（不含扩展名）作为前缀
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 临时输出文件放到错误结果目录(供204/207/208/210算法使用)
    QString outputTempLayerPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    
    // QByteArray生命周期管理
    QByteArray standardLayerBytes = standardLayer.toUtf8();
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray annotShpBytes = annotShpSemicolon.toUtf8();
    QByteArray outputTempLayerPathBytes = outputTempLayerPath.toUtf8();
    
    const char* szStandardLayer = standardLayerBytes.constData();
    const char* szGdbLayerPath = gdbShpBytes.constData();
    const char* szRuleXmlPath = xmlPathBytes.constData();
    const char* szActualLayerPath = actualShpBytes.constData();
    const char* szActualAnnoLayerPath = annotShpBytes.constData();
    const char* szOutputTempLayerPath = outputTempLayerPathBytes.constData();
    
    try {
        // 调试日志：记录传入C函数的参数
        qDebug() << "[200算法] 参数详情:";
        qDebug() << "  standardLayer:" << QString::fromUtf8(szStandardLayer);
        qDebug() << "  gdbLayerPath:" << QString::fromUtf8(szGdbLayerPath);
        qDebug() << "  xmlPath:" << QString::fromUtf8(szRuleXmlPath);
        qDebug() << "  actualLayerPath:" << QString::fromUtf8(szActualLayerPath);
        qDebug() << "  annoLayerPath:" << QString::fromUtf8(szActualAnnoLayerPath);
        qDebug() << "  scale:" << scale;
        qDebug() << "  outputPath:" << QString::fromUtf8(szOutputTempLayerPath);
        
        // 调用C接口
        SE_Error error = ::AnnotationMatchFeatures(
            szStandardLayer,
            szGdbLayerPath,
            szRuleXmlPath,
            szActualLayerPath,
            szActualAnnoLayerPath,
            scale,
            szOutputTempLayerPath
        );
        
        if (error == SE_ERROR_NONE) {
            // 验证输出文件是否真的被创建
            if (!QFile::exists(outputTempLayerPath)) {
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = QString("算法执行成功但未生成输出文件: %1").arg(outputTempLayerPath);
                errorDetail["standard_layer"] = QString::fromUtf8(szStandardLayer);
                errorDetail["output_expected"] = outputTempLayerPath;
                result.errors["file_not_created"] = errorDetail;
            } else {
                // 匹配成功,生成JSON结果
                result.id = "200";
                result.status = "质检成功";
                result.err_exist = false;
                
                // 保存JSON结果文件（用于统一的结果记录）
                QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记匹配_%1.json").arg(standardLayer));
                QJsonObject resultJson;
                resultJson["id"] = "200";
                resultJson["name"] = checkItemName;
                resultJson["status"] = "质检成功";
                resultJson["err_exist"] = false;
                
                QJsonObject info;
                info["output_layer"] = outputTempLayerPath;
                info["standard_layer"] = QString::fromUtf8(szStandardLayer);
                info["gdb_layer"] = QString::fromUtf8(szGdbLayerPath);
                info["xml_path"] = QString::fromUtf8(szRuleXmlPath);
                info["actual_layer"] = QString::fromUtf8(szActualLayerPath);
                info["anno_layer"] = QString::fromUtf8(szActualAnnoLayerPath);
                info["result_json"] = jsonPath;
                info["scale"] = scale;
                resultJson["info"] = info;
                
                // 写入JSON文件
                QFile jsonFile(jsonPath);
                if (jsonFile.open(QIODevice::WriteOnly)) {
                    QJsonDocument doc(resultJson);
                    jsonFile.write(doc.toJson(QJsonDocument::Indented));
                    jsonFile.close();
                }
                
                result.info = info;
            }
        }
        else {
            // 匹配失败
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("要素-注记匹配失败，错误码: %1").arg(error);
            errorDetail["standard_layer"] = QString::fromUtf8(szStandardLayer);
            errorDetail["gdb_layer"] = QString::fromUtf8(szGdbLayerPath);
            errorDetail["actual_layer"] = QString::fromUtf8(szActualLayerPath);
            errorDetail["anno_layer"] = QString::fromUtf8(szActualAnnoLayerPath);
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 207 - 线要素注记间距
void GroupSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
     if (filePaths.annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "207";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }

    result.id = "207";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    
    // 获取标准图层名称并匹配XML
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    /* 原硬编码测试路径(已禁用):
    QString xmlPaths = QString("D:/Desktop/a测试数据/xml测试/干渠_标注.xml");
    */
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 从XML路径提取文件名（不含扩展名）作为前缀，与200算法保持一致
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "207";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("注记组距_%1.json").arg(standardLayer));
    QString checkResultPath = QDir(outPaths.errorResultDir).filePath(QString("注记组距_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("注记组距_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultPathBytes = checkResultPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMatchResult = matchResultBytes.constData();
    const char* szXmlPath = xmlPathBytes.constData();
    const char* szResultPath = resultPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    try {
        SE_Error err = GroupSpacingCheck(
            szMatchResult,
            szXmlPath,
            scale,
            szResultPath,
            szLogPath,
            szReportPath
        );

        if (err == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "207";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["json_path"] = jsonPath;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "207";
            }
        } else {
            result.id = "207";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("注记组距检查失败，错误码: %1").arg(err);
            errorDetail["match_result"] = QString::fromUtf8(szMatchResult);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
            errorDetail["scale"] = scale;
            result.errors["algorithm_error"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "207";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 208 - 面要素等间距标注检查
void EqualSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    if (filePaths.annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "208";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }
    
    result.id = "208";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    
    // 获取标准图层名称并匹配XML
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    /* 原硬编码测试路径(已禁用):
    QString xmlPaths = QString("D:/Desktop/a测试数据/xml测试/乡镇级表面标注_标注.xml");
    */
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 从XML路径提取文件名（不含扩展名）作为前缀，与200算法保持一致
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "208";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("等间距标注_%1.json").arg(standardLayer));
    QString checkResultPath = QDir(outPaths.errorResultDir).filePath(QString("等间距标注_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("等间距标注_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultPathBytes = checkResultPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMatchResult = matchResultBytes.constData();
    const char* szGdbShp = gdbShpBytes.constData();
    const char* szXmlPath = xmlPathBytes.constData();
    const char* szResultPath = resultPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    try {
        SE_Error err = EqualSpacingCheck(
            szMatchResult,
            szGdbShp,
            szXmlPath,
            scale,
            szResultPath,
            szLogPath,
            szReportPath
        );

        if (err == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "208";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["json_path"] = jsonPath;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "208";
            }
        } else {
            result.id = "208";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("等间距标注检查失败，错误码: %1").arg(err);
            errorDetail["match_result"] = QString::fromUtf8(szMatchResult);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
            errorDetail["scale"] = scale;
            result.errors["algorithm_error"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "208";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 209 - 符号方向性检查
void SymbolDirectionCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
   
    // 先获取符号图层路径，并提取基名用于匹配XML
    QString symbolPaths = filePaths.actualShpSemicolon;
    if (symbolPaths.trimmed().isEmpty()) {
        result.id = "209";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString::fromUtf8("符号图层路径为空，请检查CSV与SHP目录配置");
        result.errors["error1"] = errorDetail;
        return;
    }
    auto normalizeBaseName = [](const QString& base)->QString {
        QString b = base;
        for (const QString& suf : QStringList{ "_Point", "_LineString", "_Polygon" }) {
            if (b.endsWith(suf, Qt::CaseInsensitive)) {
                b.chop(suf.size());
            }
        }
        return b;
    };
    QStringList symbolBaseNames;
    for (const QString& p : symbolPaths.split(';', Qt::SkipEmptyParts)) {
        QString bn = QFileInfo(p).baseName();
        symbolBaseNames << normalizeBaseName(bn);
    }
    // 1) 在XML中定位当前质检项并读取“关联图层”规则（按符号基名优先匹配）
    QString xmlPath;
    QString assocRuleValue;
    struct XmlCandidate { QString path; QString rule; };
    QVector<XmlCandidate> candidates;
    for (const QString& path : filePaths.xmlPaths) {
        QString v = getRuleValueFromXml(path, checkItemName, QString::fromUtf8("关联图层"), scale);
        if (!v.isEmpty()) {
            candidates.push_back({ path, v });
        }
    }
    if (!candidates.isEmpty()) {
        // 先尝试精确匹配
        for (const auto& c : candidates) {
            QString xb = QFileInfo(c.path).baseName();
            for (const QString& sb : symbolBaseNames) {
                if (xb.compare(sb, Qt::CaseInsensitive) == 0) {
                    xmlPath = c.path;
                    assocRuleValue = c.rule;
                    break;
                }
            }
            if (!xmlPath.isEmpty()) break;
        }
        // 再尝试子串匹配（如 xml: 隧道口, symbol: 铁路隧道口）
        if (xmlPath.isEmpty()) {
            for (const auto& c : candidates) {
                QString xb = QFileInfo(c.path).baseName();
                for (const QString& sb : symbolBaseNames) {
                    if (sb.contains(xb, Qt::CaseInsensitive) || xb.contains(sb, Qt::CaseInsensitive)) {
                        xmlPath = c.path;
                        assocRuleValue = c.rule;
                        break;
                    }
                }
                if (!xmlPath.isEmpty()) break;
            }
        }
        // 如果仍未匹配，回退到第一个候选
        if (xmlPath.isEmpty()) {
            xmlPath = candidates.first().path;
            assocRuleValue = candidates.first().rule;
        }
    }

    if (xmlPath.isEmpty()) {
        result.id = "209";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString::fromUtf8("未找到包含‘关联图层’规则的符号方向性检查配置文件");
        result.errors["error1"] = errorDetail;
        return;
    }

    // 2) 解析关联图层路径

    // 关联图层：规则值可能是标准图层名或直接的 .shp 路径；两种情况都支持
    auto looksLikeFilePath = [](const QString& s) {
        QString lower = s.toLower();
        return lower.contains(":/") || lower.contains(":\\") || lower.endsWith(".shp");
    };

    QString associatedPaths;
    if (looksLikeFilePath(assocRuleValue)) {
        associatedPaths = assocRuleValue;
    } else {
        // 优先用实际SHP目录映射标准图层；若找不到，再回退到GDB目录
        associatedPaths = getPathsForStdLayers(assocRuleValue, filePaths.csvPath, filePaths.shpPath);
        if (associatedPaths.isEmpty()) {
            associatedPaths = getGdbPathsForStdLayers(assocRuleValue, filePaths.csvPath, filePaths.gdbShpPath);
        }
    }

    // 仅选用 _LineString 后缀的关联图层
    associatedPaths = filterPathsBySuffixPreferBase(associatedPaths, "_LineString");
    if (associatedPaths.trimmed().isEmpty()) {
        result.id = "209";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString::fromUtf8("无法解析或定位‘关联图层’路径，请检查XML与CSV配置");
        errorDetail["rule_value"] = assocRuleValue;
        result.errors["error1"] = errorDetail;
        return;
    }

    // 3) 输出路径（使用统一的 CreateOutputDirectories 输出结构）
    QString xmlBaseName = QFileInfo(xmlPath).baseName();
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    QString problemShpPath = QDir(outPaths.errorResultDir).filePath(QString::fromUtf8("符号方向性检查_%1_errors.shp").arg(xmlBaseName));
    QString jsonOutPath = QDir(outPaths.jsonDir).filePath(QString::fromUtf8("符号方向性检查_%1.json").arg(xmlBaseName));
    QString logPath = QDir(outPaths.logDir).filePath(QString::fromUtf8("符号方向性检查_%1.log").arg(xmlBaseName));

    // 4) 调用算法（直接传递JSON文件路径）
    QByteArray symbolPathsBytes = symbolPaths.toUtf8();
    QByteArray assocPathsBytes = associatedPaths.toUtf8();
    QByteArray xmlPathBytes = xmlPath.toUtf8();
    QByteArray problemShpPathBytes = problemShpPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonOutPathBytes = jsonOutPath.toUtf8();

    const char* szSymbolLayerPaths = symbolPathsBytes.constData();
    const char* szAssociatedLayerPaths = assocPathsBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputCheckResultLayerPath = problemShpPathBytes.constData();
    const char* szOutputLogFilePath = logPathBytes.constData();
    const char* szReportPath = jsonOutPathBytes.constData();

    SE_Error rc = SymbolDirectionCheck(
        szSymbolLayerPaths,
        szAssociatedLayerPaths,
        szSchemaXmlPath,
        scale,
        szOutputCheckResultLayerPath,
        szOutputLogFilePath,
        szReportPath
    );

    // 解析JSON到结果结构；若算法失败则填充错误信息
    if (rc == SE_ERROR_NONE) {
        if (!ParseJsonToCheckItemResult(jsonOutPath, result, checkItemName)) {
            result.id = "209";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString::fromUtf8("无法解析算法返回的JSON结果");
            errorDetail["scale"] = scale;
            errorDetail["xml"] = xmlPath;
            errorDetail["symbol_paths"] = symbolPaths;
            errorDetail["associated_paths"] = associatedPaths;
            result.errors["error1"] = errorDetail;
        }
    } else {
        result.id = "209";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(rc);
        errorDetail["scale"] = scale;
        errorDetail["xml"] = xmlPath;
        errorDetail["symbol_paths"] = symbolPaths;
        errorDetail["associated_paths"] = associatedPaths;
        result.errors["error1"] = errorDetail;
    }
}

// 210 - 符号注记匹配检查
void SymbolAnnotationMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    if (filePaths.annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "210";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }

    result.id = "210";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString gdbLayerPath = filePaths.gdbShpSemicolon.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    QString annotationLayerPath = filterPathsBySuffixPreferBase(filePaths.annotShpSemicolon, "_Polygon");
    if (annotationLayerPath.contains(';')) {
        annotationLayerPath = annotationLayerPath.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }
    if (annotationLayerPath.isEmpty()) {
        annotationLayerPath = filePaths.annotShp;
    }
    
    // 获取标准图层名称并匹配XML
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    /* 原硬编码测试路径(已禁用):
    QString xmlPaths = QString("D:/Desktop/a测试数据/xml测试/行政村_标注.xml");
    */
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 从XML路径提取文件名（不含扩展名）作为前缀，与200算法保持一致
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "210";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("符号注记匹配_%1.json").arg(standardLayer));
    QString checkResultPath = QDir(outPaths.errorResultDir).filePath(QString("符号注记匹配_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("符号注记匹配_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray gdbLayerBytes = gdbLayerPath.toUtf8();
    QByteArray annotationLayerBytes = annotationLayerPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultPathBytes = checkResultPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMatchResult = matchResultBytes.constData();
    const char* szActualShp = actualShpBytes.constData();
    const char* szXmlPath = xmlPathBytes.constData();
    const char* szResultPath = resultPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    try {
        SE_Error err = SymbolAnnotationMatchCheck(
            szMatchResult,
            szActualShp,
            gdbLayerBytes.constData(),
            annotationLayerBytes.constData(),
            szXmlPath,
            scale,
            szResultPath,
            szLogPath,
            szReportPath
        );

        if (err == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "210";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["json_path"] = jsonPath;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "210";
            }
        } else {
            result.id = "210";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("符号注记匹配检查失败，错误码: %1").arg(err);
            errorDetail["match_result"] = QString::fromUtf8(szMatchResult);
            errorDetail["actual_shp"] = QString::fromUtf8(szActualShp);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
            errorDetail["scale"] = scale;
            result.errors["algorithm_error"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "210";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//政区名称注记错误/缺失检查（ID=211）
void AdminNameAnnotationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "211";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    QString standardLayer = filePaths.standardLayer;
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);

    // 政区面图层优先使用 Polygon，其次使用无后缀基础路径
    QString actualAdminShp = filterPathsBySuffixPreferBase(filePaths.actualShpSemicolon, "_Polygon");
    if (actualAdminShp.contains(';')) {
        actualAdminShp = actualAdminShp.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }

    // 注记图层保留旧系统习惯：优先基础路径，无则取第一个实际注记路径
    QString actualAnnoShp = filePaths.annotShp;
    if (actualAnnoShp.isEmpty() && !filePaths.annotShpSemicolon.isEmpty()) {
        actualAnnoShp = filePaths.annotShpSemicolon.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }

    // 标准图层优先使用 CSV 配置生成的 gdbShp，第一个有效路径即可
    QString gdbAdminShp = filePaths.gdbShp;
    if (gdbAdminShp.isEmpty() && !filePaths.gdbShpSemicolon.isEmpty()) {
        gdbAdminShp = filePaths.gdbShpSemicolon.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }

    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("政区名称注记_%1.json").arg(standardLayer));
    QString resultShpPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_errors.shp").arg(standardLayer));
    QString resultGeoJsonPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_errors.geojson").arg(standardLayer));
    QString tempAdminMatchPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_temp_admin_layer.shp").arg(standardLayer));
    QString tempAnnotationPointPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_temp_annotation_point.shp").arg(standardLayer));
    QString tempMergedSingleCharPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_temp_merged_single_char.shp").arg(standardLayer));
    QString tempStandardMatchPath = QDir(outPaths.errorResultDir).filePath(QString("政区名称注记_%1_temp_standard_layer.shp").arg(standardLayer));

    QByteArray actualAdminBytes = actualAdminShp.toUtf8();
    QByteArray actualAnnoBytes = actualAnnoShp.toUtf8();
    QByteArray gdbAdminBytes = gdbAdminShp.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray labelFieldBytes = filePaths.labelField.toUtf8();
    QByteArray resultShpBytes = resultShpPath.toUtf8();
    QByteArray resultGeoJsonBytes = resultGeoJsonPath.toUtf8();
    QByteArray tempAdminMatchBytes = tempAdminMatchPath.toUtf8();
    QByteArray tempAnnotationPointBytes = tempAnnotationPointPath.toUtf8();
    QByteArray tempMergedSingleCharBytes = tempMergedSingleCharPath.toUtf8();
    QByteArray tempStandardMatchBytes = tempStandardMatchPath.toUtf8();

    try {
        bool bPassed = false;
        SE_Error error = ::AdminNameAnnotationCheck(
            actualAdminBytes.constData(),
            actualAnnoBytes.constData(),
            gdbAdminBytes.constData(),
            xmlPathBytes.constData(),
            jsonPathBytes.constData(),
            labelFieldBytes.constData(),
            resultShpBytes.constData(),
            resultGeoJsonBytes.constData(),
            tempAdminMatchBytes.constData(),
            tempAnnotationPointBytes.constData(),
            tempMergedSingleCharBytes.constData(),
            tempStandardMatchBytes.constData(),
            scale,
            bPassed);

        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "211";
            result.status = "质检失败";
            result.err_exist = true;

            QJsonObject errorDetail;
            errorDetail["description"] = QString("政区名称注记错误/缺失检查失败，错误码: %1").arg(error);
            errorDetail["actual_admin_layer"] = actualAdminShp;
            errorDetail["actual_annotation_layer"] = actualAnnoShp;
            errorDetail["gdb_admin_layer"] = gdbAdminShp;
            errorDetail["label_field"] = filePaths.labelField;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["result_shp"] = resultShpPath;
            errorDetail["result_geojson"] = resultGeoJsonPath;
            errorDetail["temp_admin_layer"] = tempAdminMatchPath;
            errorDetail["temp_annotation_point"] = tempAnnotationPointPath;
            errorDetail["temp_merged_single_char_layer"] = tempMergedSingleCharPath;
            errorDetail["temp_standard_layer"] = tempStandardMatchPath;
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "211";
        }
    }
    catch (const std::exception& e) {
        result.id = "211";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 敏感文字合规性检查（ID=212）
void SensitiveContentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int /*scale*/, const QString& outputDir, CheckItemResult& result)
{
    result.id = "212";
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // SensitiveContentCheck 的输入是单个注记 SHP。对于同一注记层同时导出的
    // LineString/Polygon 文件，文字内容字段位于 Polygon 中，优先选择它；没有
    // Polygon 时再保留原先的首个有效文件作为兼容兜底。
    QStringList annotationCandidates = filePaths.annotShpSemicolon.split(
        QStringLiteral(";"), Qt::SkipEmptyParts);
    if (annotationCandidates.isEmpty()) {
        annotationCandidates = filePaths.annotShpList;
    }
    if (annotationCandidates.isEmpty() && !filePaths.annotShp.isEmpty()) {
        annotationCandidates.append(filePaths.annotShp);
    }

    QString annotationShp;
    for (const QString& candidate : annotationCandidates) {
        const QString path = candidate.trimmed();
        if (path.endsWith(QStringLiteral("_Polygon.shp"), Qt::CaseInsensitive)) {
            annotationShp = path;
            break;
        }
    }
    if (annotationShp.isEmpty() && !annotationCandidates.isEmpty()) {
        annotationShp = annotationCandidates.first().trimmed();
    }
    if (annotationShp.isEmpty()) {
        result.status = QStringLiteral("质检成功");
        result.info["description"] = QStringLiteral("实际注记图层不存在");
        return;
    }

    const QString xmlPath = FindSensitiveContentCheckXml(filePaths.xmlPaths);
    if (xmlPath.isEmpty()) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QStringLiteral("未找到包含质检项 ID=212 的敏感词库 XML 配置");
        errorDetail["annotation_shp"] = annotationShp;
        errorDetail["xml_candidates"] = QJsonArray::fromStringList(filePaths.xmlPaths);
        result.errors["configuration_error"] = errorDetail;
        return;
    }

    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString outputPrefix = filePaths.standardLayer.isEmpty()
        ? QStringLiteral("sensitive_content")
        : filePaths.standardLayer;
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(
        QStringLiteral("敏感内容校核_%1.json").arg(outputPrefix));
    const QString errorShpPath = QDir(outPaths.errorResultDir).filePath(
        QStringLiteral("敏感内容校核_%1_errors.shp").arg(outputPrefix));
    const QString logPath = QDir(outPaths.logDir).filePath(
        QStringLiteral("敏感内容校核_%1.log").arg(outputPrefix));

    const QByteArray annotationShpBytes = annotationShp.toUtf8();
    const QByteArray xmlPathBytes = xmlPath.toUtf8();
    const QByteArray jsonPathBytes = jsonPath.toUtf8();
    const QByteArray errorShpPathBytes = errorShpPath.toUtf8();
    const QByteArray logPathBytes = logPath.toUtf8();

    try {
        const SE_Error error = SensitiveContentCheck(
            annotationShpBytes.constData(),
            xmlPathBytes.constData(),
            jsonPathBytes.constData(),
            errorShpPathBytes.constData(),
            logPathBytes.constData());

        if (error == SE_ERROR_NONE && ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "212";
            return;
        }

        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QStringLiteral("敏感文字合规性检查执行失败，错误码: %1").arg(error);
        errorDetail["annotation_shp"] = annotationShp;
        errorDetail["xml_path"] = xmlPath;
        errorDetail["result_json"] = jsonPath;
        errorDetail["result_shp"] = errorShpPath;
        errorDetail["log_path"] = logPath;
        result.errors["algorithm_error"] = errorDetail;
    }
    catch (const std::exception& e) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QStringLiteral("敏感文字合规性检查异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 注记内容合规性检查（ID=213）。该检查依赖 ID=200 的匹配结果，并使用 groups 格式输出。
void AnnotationContentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "213";
    result.name = checkItemName;
    result.status = QStringLiteral("未质检");
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    const QString standardLayer = filePaths.standardLayer;
    const QString schemaXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    const OutputPaths outPaths = CreateOutputDirectories(outputDir);
    // 由任务管理器从 CSV 第4列“实际注记图层”传入，用作跨类型校验分组键。
    const QString crossCheckAnnotLayer = filePaths.correspondingAnnotLayer.trimmed();

    if (crossCheckAnnotLayer.isEmpty()) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject detail;
        detail["description"] = QStringLiteral("CSV 配置缺少第4列‘实际注记图层’，无法定位 ID=213 跨类型校验中间文件");
        detail["standard_layer"] = standardLayer;
        result.errors["configuration_error"] = detail;
        return;
    }

    // ID=200 以规则文件名为前缀；这里必须采用完全相同的命名规则取得前置结果。
    QString matchPrefix = standardLayer;
    if (!schemaXmlPath.isEmpty()) {
        matchPrefix = QFileInfo(schemaXmlPath).completeBaseName();
    }
    const QString matchResultPath = filePaths.annotationMatchResultPath.isEmpty()
        ? QDir(outPaths.errorResultDir).filePath(
            QStringLiteral("%1_temp_standard_layer.shp").arg(matchPrefix))
        : filePaths.annotationMatchResultPath;

    if (!QFile::exists(matchResultPath)) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject detail;
        detail["description"] = QStringLiteral("缺少 ID=200 注记匹配结果，请先执行‘注记匹配’检查项");
        detail["expected_match_result"] = matchResultPath;
        detail["standard_layer"] = standardLayer;
        result.errors["prerequisite_error"] = detail;
        return;
    }

    // 213 的 QGIS 接口一次只接收一个图层。优先使用与 200 一致的实际图层和面注记图层。
    const QString actualLayerPath = filePaths.actualShpList.isEmpty()
        ? filePaths.actualShp
        : filePaths.actualShpList.first();
    QString annotationLayerPath = filterPathsBySuffixPreferBase(filePaths.annotShpSemicolon, "_Polygon");
    if (annotationLayerPath.contains(';')) {
        annotationLayerPath = annotationLayerPath.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }
    if (annotationLayerPath.isEmpty()) {
        annotationLayerPath = filePaths.annotShp;
    }
    const QString gdbLayerPath = filePaths.gdbShpSemicolon.split(';', Qt::SkipEmptyParts).value(0).trimmed();

    const QString outputPrefix = standardLayer.isEmpty()
        ? QStringLiteral("annotation_content")
        : standardLayer;
    const QString errorShpPath = QDir(outPaths.errorResultDir).filePath(
        QStringLiteral("注记内容合规性_%1_errors.shp").arg(outputPrefix));
    const QString jsonPath = QDir(outPaths.jsonDir).filePath(
        QStringLiteral("注记内容合规性_%1.json").arg(outputPrefix));
    const QString logPath = QDir(outPaths.logDir).filePath(
        QStringLiteral("注记内容合规性_%1.log").arg(outputPrefix));

    const QByteArray matchResultBytes = matchResultPath.toUtf8();
    const QByteArray actualLayerBytes = actualLayerPath.toUtf8();
    const QByteArray gdbLayerBytes = gdbLayerPath.toUtf8();
    const QByteArray annotationLayerBytes = annotationLayerPath.toUtf8();
    const QByteArray schemaXmlBytes = schemaXmlPath.toUtf8();
    const QByteArray errorShpBytes = errorShpPath.toUtf8();
    const QByteArray jsonBytes = jsonPath.toUtf8();
    const QByteArray logBytes = logPath.toUtf8();

    try {
        const SE_Error error = ::AnnotationContentCheck(
            matchResultBytes.constData(), actualLayerBytes.constData(), gdbLayerBytes.constData(),
            annotationLayerBytes.constData(), schemaXmlBytes.constData(), scale,
            errorShpBytes.constData(), jsonBytes.constData(), logBytes.constData());
        if (error != SE_ERROR_NONE || !QFile::exists(jsonPath)) {
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            QJsonObject detail;
            detail["description"] = QStringLiteral("注记内容合规性检查执行失败，错误码: %1").arg(error);
            detail["match_result"] = matchResultPath;
            detail["result_json"] = jsonPath;
            detail["result_shp"] = errorShpPath;
            detail["log_path"] = logPath;
            result.errors["algorithm_error"] = detail;
            return;
        }

        // 多 type 共享同一注记图层时，按集成说明执行 FULLY_MATCHED 交叉校验。
        saveFullMatchedToCrossCheck(outPaths.errorResultDir, crossCheckAnnotLayer, standardLayer,
            jsonPath, matchResultPath, errorShpPath);
        QString crossCheckLog;
        performCrossTypeCheck(outPaths.errorResultDir, crossCheckAnnotLayer, standardLayer,
            errorShpPath, jsonPath, crossCheckLog);

        QFile jsonFile(jsonPath);
        if (!jsonFile.open(QIODevice::ReadOnly)) {
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            result.errors["result_error"] = QStringLiteral("无法读取 213 结果文件: %1").arg(jsonPath);
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(jsonFile.readAll());
        jsonFile.close();
        if (!document.isObject()) {
            result.status = QStringLiteral("质检失败");
            result.err_exist = true;
            result.errors["result_error"] = QStringLiteral("213 结果文件不是有效的 JSON 对象: %1").arg(jsonPath);
            return;
        }

        const QJsonObject report = document.object();
        // 213 单独报告使用 results 保存空间聚类后的错误组。
        const QJsonArray groups = report.value("results").toArray();
        const QJsonObject summary = report.value("summary").toObject();
        int totalItems = summary.value("total_items").toInt(-1);
        if (totalItems < 0) {
            totalItems = 0;
            for (const QJsonValue& groupValue : groups) {
                totalItems += groupValue.toObject().value("items").toArray().size();
            }
        }

        result.status = QStringLiteral("质检成功");
        result.err_exist = totalItems > 0;
        // 213 单项报告在过滤及跨类型校验后会生成包含统计信息的 info 文本。
        // 汇总 JSON 必须沿用该文本，不能用固定文案将其覆盖。
        const QJsonValue reportInfo = report.value("info");
        const QString reportDescription = (reportInfo.isObject()
            ? reportInfo.toObject().value("description")
            : reportInfo).toString().trimmed();
        result.info["description"] = reportDescription.isEmpty()
            ? QStringLiteral("注记内容合规性检查完成")
            : reportDescription;

        result.info["total_groups"] = summary.value("total_groups").toInt(groups.size());
        result.info["total_items"] = totalItems;
        result.info["match_result"] = matchResultPath;
        result.info["result_json"] = jsonPath;
        // 无错误时 213 不保留空 SHP，汇总中也不输出无效图层路径。
        if (totalItems > 0 && QFile::exists(errorShpPath))
            result.info["result_shp"] = errorShpPath;
        else
            result.info.remove("result_shp");
        result.info["log_path"] = logPath;
        if (!crossCheckLog.isEmpty()) {
            result.info["cross_type_check"] = crossCheckLog;
        }
        for (int i = 0; i < groups.size(); ++i) {
            result.errors[QStringLiteral("group_%1").arg(i + 1)] = groups.at(i);
        }
    }
    catch (const std::exception& e) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.errors["exception"] = QStringLiteral("注记内容合规性检查异常: %1").arg(e.what());
    }
}

void BoundaryBandRenderCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "214";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    QString adminPolygonShp = filePaths.gdbShp;
    if (adminPolygonShp.isEmpty() && !filePaths.gdbShpSemicolon.isEmpty()) {
        adminPolygonShp = filePaths.gdbShpSemicolon.split(';', Qt::SkipEmptyParts).value(0).trimmed();
    }
    if ((adminPolygonShp.isEmpty() || !QFileInfo::exists(adminPolygonShp)) && !filePaths.gdbShpPath.isEmpty()) {
        const QString fallbackBoua = QDir(filePaths.gdbShpPath).filePath(QStringLiteral("BOUA5.shp"));
        if (QFileInfo::exists(fallbackBoua)) {
            adminPolygonShp = fallbackBoua;
        }
    }

    QString selectedXmlPath;
    if (!filePaths.xmlPaths.isEmpty()) {
        selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, filePaths.standardLayer);
        if (selectedXmlPath.isEmpty()) {
            selectedXmlPath = filePaths.xmlPaths.first();
        }
    }

    // 色带检查不使用任务管理器覆盖后的 actualShp 作为边界线：
    // 晕带面从 CSV 实际图层列识别，境界线从 XML 规则识别，两类输入在这里重新分开。
    QStringList boundaryLineShps = resolveBoundaryBandLinePaths(filePaths, selectedXmlPath, scale);
    QString bandPolygonShp = findBoundaryBandPolygonPath(filePaths, selectedXmlPath, scale);

    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    const QString timeTag = QDateTime::currentDateTime().toString("yyyyMMddhhmmsszzz");
    const QString outerSummaryErrorPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_outer_errors.shp").arg(filePaths.standardLayer));
    const QString innerSummaryErrorPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_inner_errors.shp").arg(filePaths.standardLayer));

    const QString outerBandPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_outer_parsed_band_%2.shp").arg(filePaths.standardLayer, timeTag));
    const QString innerBandPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_inner_parsed_band_%2.shp").arg(filePaths.standardLayer, timeTag));

    const QString bandCheckName = QStringLiteral("境界线色带渲染合规性检查");
    const QString bandColorField = getFirstRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("内外色带提取字段") << QStringLiteral("颜色字段") << QStringLiteral("填充颜色字段"), scale).trimmed();
    const QString outerBandColor = resolveBoundaryBandColorFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("外圈填充颜色") << QStringLiteral("外层填充颜色") << QStringLiteral("标准外圈填充颜色") << QStringLiteral("标准外层填充颜色"), scale);
    const QString innerBandColor = resolveBoundaryBandColorFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("内圈填充颜色") << QStringLiteral("内层填充颜色") << QStringLiteral("标准内圈填充颜色") << QStringLiteral("标准内层填充颜色"), scale);
    const double outerBandWidthMm = getDoubleRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("标准外圈宽度mm") << QStringLiteral("标准外层宽度mm"), scale, 3.0);
    const double innerBandWidthMm = getDoubleRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("标准内圈宽度mm") << QStringLiteral("标准内层宽度mm"), scale, 2.0);
    const double boundaryBandWidthToleranceMm = getDoubleRuleValueFromXml(selectedXmlPath, bandCheckName, QStringList()
        << QStringLiteral("宽度容差mm") << QStringLiteral("宽度容差"), scale, 0.6);

    QJsonObject runErrors;
    QJsonObject runInfo;
    runInfo["description"] = QString();
    runInfo["source_parsed_band"] = bandPolygonShp;
    runInfo["rule_xml"] = selectedXmlPath;
    runInfo["band_color_field"] = bandColorField;
    runInfo["outer_band_color"] = outerBandColor;
    runInfo["inner_band_color"] = innerBandColor;
    runInfo["outer_band_width_mm"] = outerBandWidthMm;
    runInfo["inner_band_width_mm"] = innerBandWidthMm;
    runInfo["width_tolerance_mm"] = boundaryBandWidthToleranceMm;
    runInfo["outer_parsed_band"] = QString();
    runInfo["inner_parsed_band"] = QString();
    if (bandColorField.isEmpty() || outerBandColor.isEmpty() || innerBandColor.isEmpty()) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;

        if (bandColorField.isEmpty()) {
            QJsonObject configError;
            configError["description"] = QStringLiteral("XML缺少内外色带提取字段配置，无法按颜色字段拆分内外色带。");
            configError["xml_path"] = selectedXmlPath;
            runErrors["band_color_field_missing"] = configError;
        }
        if (outerBandColor.isEmpty()) {
            QJsonObject configError;
            configError["description"] = QStringLiteral("XML缺少外圈色带颜色配置，无法自动提取外圈色带。");
            configError["xml_path"] = selectedXmlPath;
            runErrors["outer_band_color_missing"] = configError;
        }
        if (innerBandColor.isEmpty()) {
            QJsonObject configError;
            configError["description"] = QStringLiteral("XML缺少内圈色带颜色配置，无法自动提取内圈色带。");
            configError["xml_path"] = selectedXmlPath;
            runErrors["inner_band_color_missing"] = configError;
        }

        result.info = runInfo;
        result.info["description"] = QStringLiteral("境界线色带渲染合规性检查配置不完整，无法按颜色字段拆分内外晕带。");
        result.errors = runErrors;
        return;
    }

    SE_BoundaryBandRenderRuleConfig styleConfig;
    const QByteArray selectedXmlBytes = selectedXmlPath.toUtf8();
    const SE_Error styleRuleError = ParseXmlForBoundaryBandRender(selectedXmlBytes.constData(), styleConfig);
    if (styleRuleError != SE_ERROR_NONE) {
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        result.info = runInfo;
        result.info["description"] = QStringLiteral("图例规则XML无法读取或解析");
        QJsonObject errorDetail;
        errorDetail["description"] = result.info["description"];
        runErrors["BAND_STYLE_RULE_ERROR"] = errorDetail;
        result.errors = runErrors;
        return;
    }
    const QString legendJson = QDir(outPaths.jsonDir).filePath(QString("境界线色带_%1_legend_style.json").arg(filePaths.standardLayer));
    const QString legendShp = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_legend_style_errors.shp").arg(filePaths.standardLayer));
    const QString legendGeoJson = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_legend_style_errors.geojson").arg(filePaths.standardLayer));
    const QByteArray bandSourceBytes = bandPolygonShp.toUtf8();
    const QByteArray legendJsonBytes = legendJson.toUtf8();
    const QByteArray legendShpBytes = legendShp.toUtf8();
    const QByteArray legendGeoJsonBytes = legendGeoJson.toUtf8();
    bool legendPassed = false;
    QJsonObject legendReport;
    const SE_Error legendError = BoundaryBandLegendStyleCheck(bandSourceBytes.constData(), selectedXmlBytes.constData(),
        legendJsonBytes.constData(), legendShpBytes.constData(), legendGeoJsonBytes.constData(), legendPassed, legendReport);
    runInfo["legend_style"] = legendReport;
    runInfo["legend_style_json"] = legendJson;
    if (legendError != SE_ERROR_NONE || !legendPassed) runErrors["legend_style"] = legendReport;

    QJsonObject outerIdentification, innerIdentification;
    const bool outerPrepared = !bandPolygonShp.isEmpty() && QFileInfo::exists(bandPolygonShp)
        && PrepareBoundaryBandGeometry(bandPolygonShp, outerBandPath, bandColorField, "outer", outerIdentification);
    const bool innerPrepared = !bandPolygonShp.isEmpty() && QFileInfo::exists(bandPolygonShp)
        && PrepareBoundaryBandGeometry(bandPolygonShp, innerBandPath, bandColorField, "inner", innerIdentification);
    runInfo["outer_identification"] = outerIdentification;
    runInfo["inner_identification"] = innerIdentification;

    // 派生出的 inner/outer rule 只作为算法内部临时输入，不写入质检结果目录，也不出现在最终 info 中。
    QTemporaryDir tempRuleDir(QDir::tempPath() + QDir::separator() + QStringLiteral("boundary_band_rule_XXXXXX"));
    const QString tempRuleDirPath = tempRuleDir.isValid() ? tempRuleDir.path() : outPaths.errorResultDir;
    const QString outerRuleXml = CreateBoundaryBandPartRuleXml(selectedXmlPath, QDir(tempRuleDirPath).filePath(QString("境界线色带_%1_outer_rule.xml").arg(filePaths.standardLayer)), bandColorField, outerBandColor, outerBandWidthMm, boundaryBandWidthToleranceMm);
    const QString innerRuleXml = CreateBoundaryBandPartRuleXml(selectedXmlPath, QDir(tempRuleDirPath).filePath(QString("境界线色带_%1_inner_rule.xml").arg(filePaths.standardLayer)), bandColorField, innerBandColor, innerBandWidthMm, boundaryBandWidthToleranceMm);
    const bool outerBandLayerExists = QFileInfo::exists(outerBandPath);
    const bool innerBandLayerExists = QFileInfo::exists(innerBandPath);
    runInfo["outer_parsed_band"] = outerBandLayerExists ? outerBandPath : QString();
    runInfo["inner_parsed_band"] = innerBandLayerExists ? innerBandPath : QString();
    if (!innerPrepared) {
        QJsonObject innerPrepareError;
        innerPrepareError["description"] = QStringLiteral("内圈晕带解析失败：按层次属性或几何嵌套识别并差集裁切后未得到有效窄条带，可能是晕带解析算法得到的内圈面不正确。");
        innerPrepareError["source_parsed_band"] = bandPolygonShp;
        innerPrepareError["color_field"] = bandColorField;
        innerPrepareError["expected_fill_color"] = innerBandColor;
        innerPrepareError["cutter_fill_color"] = outerBandColor;
        innerPrepareError["expected_band_part"] = QStringLiteral("inner");
        runInfo["inner_prepare_error"] = innerPrepareError;
        runErrors["inner_prepare_failed"] = innerPrepareError;
    }
    if (!outerPrepared) {
        QJsonObject outerPrepareError;
        outerPrepareError["description"] = QStringLiteral("外圈晕带解析失败：按层次属性或几何嵌套识别并差集裁切后未得到有效窄条带。");
        outerPrepareError["source_parsed_band"] = bandPolygonShp;
        outerPrepareError["color_field"] = bandColorField;
        outerPrepareError["expected_fill_color"] = outerBandColor;
        outerPrepareError["cutter_fill_color"] = innerBandColor;
        outerPrepareError["expected_band_part"] = QStringLiteral("outer");
        runInfo["outer_prepare_error"] = outerPrepareError;
        runErrors["outer_prepare_failed"] = outerPrepareError;
    }

    auto runBandCheck = [&](const QString& boundaryLineShp, const QString& partName, const QString& preparedBandPath, const QString& ruleXmlPath, bool onlyCheckSide)->bool {
        const QString boundaryTag = QFileInfo(boundaryLineShp).completeBaseName();
        const QString jsonPath = QDir(outPaths.jsonDir).filePath(onlyCheckSide
            ? QString("境界线色带_%1_%2_%3_side.json").arg(filePaths.standardLayer, boundaryTag, partName)
            : QString("境界线色带_%1_%2.json").arg(filePaths.standardLayer, partName));
        const QString resultShpPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_%2_errors.shp").arg(filePaths.standardLayer, partName));
        const QString resultGeoJsonPath = QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_%2_errors.geojson").arg(filePaths.standardLayer, partName));
        const QString tempSampleLinePath = onlyCheckSide ? QString() : QDir(outPaths.errorResultDir).filePath(QString("境界线色带_%1_%2_sample_line.shp").arg(filePaths.standardLayer, partName));

        QByteArray boundaryBytes = boundaryLineShp.toUtf8();
        QByteArray adminBytes = adminPolygonShp.toUtf8();
        QByteArray bandBytes = preparedBandPath.toUtf8();
        QByteArray xmlBytes = ruleXmlPath.toUtf8();
        QByteArray jsonBytes = jsonPath.toUtf8();
        QByteArray shpBytes = resultShpPath.toUtf8();
        QByteArray geoJsonBytes = resultGeoJsonPath.toUtf8();
        QByteArray tempBytes = tempSampleLinePath.toUtf8();

        bool bPassed = false;
        SE_Error error = ::BoundaryBandRenderCheck(
            boundaryBytes.constData(),
            adminBytes.constData(),
            bandBytes.constData(),
            xmlBytes.constData(),
            jsonBytes.constData(),
            shpBytes.constData(),
            geoJsonBytes.constData(),
            tempBytes.constData(),
            scale,
            bPassed,
            onlyCheckSide);

        QJsonObject item;
        item["boundary_layer"] = boundaryLineShp;
        item["boundary_level"] = boundaryTag;
        item["band_layer"] = preparedBandPath;
        item["rule_xml"] = selectedXmlPath;
        item["json"] = jsonPath;
        item["result_shp"] = resultShpPath;
        item["result_geojson"] = resultGeoJsonPath;
        item["temp_sample_line"] = tempSampleLinePath;
        item["only_check_side"] = onlyCheckSide;
        item["error_code"] = static_cast<int>(error);
        item["passed"] = bPassed;
        const QString resultKey = QStringLiteral("%1_%2").arg(boundaryTag, partName);
        runInfo[resultKey] = item;
        if (error != SE_ERROR_NONE || !bPassed) {
            runErrors[resultKey] = item;
        }
        return error == SE_ERROR_NONE;
    };

    try {
        bool outerOk = false;
        bool innerOk = false;
        const QString primaryBoundaryShp = boundaryLineShps.value(0);
        if (!primaryBoundaryShp.isEmpty()) {
            if (outerBandLayerExists) outerOk = runBandCheck(primaryBoundaryShp, QStringLiteral("outer"), outerBandPath, outerRuleXml, false) || outerOk;
            if (innerBandLayerExists) innerOk = runBandCheck(primaryBoundaryShp, QStringLiteral("inner"), innerBandPath, innerRuleXml, false) || innerOk;
        }
        for (const QString& boundaryLineShp : boundaryLineShps) {
            const QString boundaryTag = QFileInfo(boundaryLineShp).completeBaseName();
            if (boundaryLineShp != primaryBoundaryShp) {
                if (outerBandLayerExists) outerOk = runBandCheck(boundaryLineShp, QStringLiteral("outer"), outerBandPath, outerRuleXml, true) || outerOk;
                if (innerBandLayerExists) innerOk = runBandCheck(boundaryLineShp, QStringLiteral("inner"), innerBandPath, innerRuleXml, true) || innerOk;
            }
            runInfo[QStringLiteral("boundary_%1").arg(boundaryTag)] = boundaryLineShp;
        }

        result.id = "214";
        result.name = checkItemName;
        result.info = runInfo;
        result.errors = runErrors;
        if (boundaryLineShps.isEmpty()) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QStringLiteral("未配置境界线图层，无法执行多级境界线色带检查。 ");
            result.errors["boundary_layer_missing"] = errorDetail;
            result.info["description"] = errorDetail["description"].toString();
            return;
        }
        if (!outerBandLayerExists && !innerBandLayerExists) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QStringLiteral("未能从解析晕带中提取内圈或外圈晕带，请检查层次属性、几何嵌套关系和碎片过滤结果。");
            errorDetail["source_parsed_band"] = bandPolygonShp;
            result.errors["parsed_band_prepare_failed"] = errorDetail;
            result.info["description"] = errorDetail["description"].toString();
            return;
        }
        const bool hasExecutedCheck = (outerBandLayerExists && outerOk) || (innerBandLayerExists && innerOk);
        result.err_exist = !runErrors.isEmpty();
        result.status = hasExecutedCheck ? "质检成功" : "质检失败";
        result.errors = runErrors;
        if (result.err_exist) {
            result.info["description"] = QStringLiteral("境界线色带渲染合规性检查发现错误，详见 errors、错误结果图层和统计信息。");
        } else {
            result.info["description"] = QStringLiteral("境界线色带渲染合规性检查通过，未发现挂接方向、宽度、几何、颜色或渐变错误。");
        }
    }
    catch (const std::exception& e) {
        result.id = "214";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        errorDetail["source_parsed_band"] = bandPolygonShp;
        result.errors["exception"] = errorDetail;
    }
}

// 204 字距检查
void CharSpacingCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    if (filePaths.annotShpSemicolon.isEmpty()) {
        // 实际注记图层不存在，直接返回成功状态
        result.id = "203";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际注记图层不存在";
        return;
    }


    result.id = "204";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();

    // 使用传入的路径参数
    QString standardLayer = filePaths.standardLayer;
    QString gdbShpSemicolon = filePaths.gdbShpSemicolon;
    
    // 获取标准图层名称并匹配XML
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 根据XML文件名查找200算法生成的匹配结果文件(算法204依赖算法200)
    QString xmlPrefix;
    if (!selectedXmlPath.isEmpty()) {
        QFileInfo xmlFileInfo(selectedXmlPath);
        xmlPrefix = xmlFileInfo.completeBaseName();
    }
    if (xmlPrefix.isEmpty()) {
        xmlPrefix = standardLayer;
    }
    
    // 从输出目录的错误结果目录查找200算法生成的临时文件
    QString matchResultPath = QDir(outPaths.errorResultDir).filePath(xmlPrefix + "_temp_standard_layer.shp");
    if (!filePaths.annotationMatchResultPath.isEmpty()) matchResultPath = filePaths.annotationMatchResultPath;
    
    // 检查前置结果文件是否存在
    if (!QFile::exists(matchResultPath)) {
        result.id = "204";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["error"] = "前置注记匹配结果文件不存在";
        errorDetail["solution"] = "请确保质检方案中包含'注记匹配'检查项，且执行顺序在当前检查项之前";
        errorDetail["expected_file"] = matchResultPath;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["prerequisite"] = "算法200-注记匹配";
        result.errors["prerequisite_error"] = errorDetail;
        return;
    }
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("字距检查_%1.json").arg(standardLayer));
    QString checkResultPath = QDir(outPaths.errorResultDir).filePath(QString("字距检查_%1_errors.shp").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("字距检查_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray matchResultBytes = matchResultPath.toUtf8();
    QByteArray gdbShpBytes = gdbShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray resultPathBytes = checkResultPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMatchResult = matchResultBytes.constData();
    const char* szGdbShp = gdbShpBytes.constData();
    const char* szXmlPath = xmlPathBytes.constData();
    const char* szResultPath = resultPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    const char* szReportPath = jsonPathBytes.constData();
    
    try {
        SE_Error err = CharSpacingCheck(
            szMatchResult,
            szGdbShp,
            szXmlPath,
            scale,
            szResultPath,
            szLogPath,
            szReportPath
        );

        if (err == SE_ERROR_NONE) {
            // 使用通用解析函数解析JSON
            if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
                result.id = "204";
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["error"] = "无法解析结果JSON";
                errorDetail["json_path"] = jsonPath;
                result.errors["error1"] = errorDetail;
            } else {
                result.id = "204";
            }
        } else {
            result.id = "204";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["error"] = QString("字距检查失败，错误码: %1").arg(err);
            errorDetail["match_result"] = QString::fromUtf8(szMatchResult);
            errorDetail["xml_path"] = QString::fromUtf8(szXmlPath);
            errorDetail["scale"] = scale;
            result.errors["algorithm_error"] = errorDetail;
        }
    }
    catch (const std::exception& e) {
        result.id = "204";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// ========== 图面整饰检查算法实现 ==========

// 304 - 图名内容准确性检查
void MapStyleCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "304";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("图名内容准确性_%1.json").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMapNameShpPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();

    try {
        bool bPassed = false;
        
        // 调用C接口
        SE_Error error = ::MapStyleCheck(
            szMapNameShpPath,
            szSchemaXmlPath,
            szOutputJsonPath,
            bPassed
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "304";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["shp_path"] = actualShpSemicolon;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "304";
        }
    }
    catch (const std::exception& e) {
        result.id = "304";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 305 - 制图说明检查检查
void MapAnnotationCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString shpPath = filePaths.shpPath;
    QString standardLayer = filePaths.standardLayer;
    
    // 构建三个固定的shp文件路径
    QString decorationLinePath = QDir(shpPath).filePath("整饰层_LineString.shp");
    QString decorationPolyPath = QDir(shpPath).filePath("整饰层_Polygon.shp");
    QString innerFramePath = QDir(shpPath).filePath("内图廓.shp");
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("制图说明_%1.json").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray decorationLineBytes = decorationLinePath.toUtf8();
    QByteArray decorationPolyBytes = decorationPolyPath.toUtf8();
    QByteArray innerFrameBytes = innerFramePath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szDecorationLinePath = decorationLineBytes.constData();
    const char* szDecorationPolyPath = decorationPolyBytes.constData();
    const char* szInnerFramePath = innerFrameBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();

    try {
        bool bPassed = false;
        
        // 调用C接口
        SE_Error error = ::MapAnnotationCheck(
            szDecorationLinePath,
            szDecorationPolyPath,
            szInnerFramePath,
            szSchemaXmlPath,
            szOutputJsonPath,
            bPassed
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "305";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["decoration_line"] = decorationLinePath;
            errorDetail["decoration_poly"] = decorationPolyPath;
            errorDetail["inner_frame"] = innerFramePath;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "305";
        }
    }
    catch (const std::exception& e) {
        result.id = "305";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 306 - 行政区划面颜色检查
void AdminColorCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "306";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("行政区划面颜色_%1.json").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szShpPath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();

    try {
        bool bPassed = false;
        
        // 调用C接口
        SE_Error error = ::AdminColorCheck(
            szShpPath,
            szSchemaXmlPath,
            szOutputJsonPath,
            bPassed
        );

        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "306";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["shp_path"] = actualShpSemicolon;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "306";
        }
    }
    catch (const std::exception& e) {
        result.id = "306";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 311 - 内容与图廓错位检查
void ContentFrameMisalignmentCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "311";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    try {
        // 使用传入的路径参数
        QString shpPath = filePaths.shpPath;
        QString standardLayer = filePaths.standardLayer;
        
        // 构建两个固定的shp文件路径
        QString adminAreaShpPath = QDir(shpPath).filePath("行政区划面普色.shp");
        QString innerFrameShpPath = QDir(shpPath).filePath("内图廓.shp");
        
        // 检查文件是否存在，如果找不到行政区划面普色.shp就尝试找行政区域面普染.shp
        if (!QFile::exists(adminAreaShpPath)) {
            QString alternativePath = QDir(shpPath).filePath("行政区域面普染.shp");
            if (QFile::exists(alternativePath)) {
                adminAreaShpPath = alternativePath;
            } else {
                result.status = "质检失败";
                result.err_exist = true;
                QJsonObject errorDetail;
                errorDetail["description"] = "行政区划面图层文件不存在";
                errorDetail["expected_paths"] = "行政区划面普色.shp 或 行政区域面普染.shp";
                errorDetail["shp_path"] = shpPath;
                result.errors["error1"] = errorDetail;
                return;
            }
        }
        
        // 检查内图廓文件
        if (!QFile::exists(innerFrameShpPath)) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "内图廓图层文件不存在";
            errorDetail["expected_file"] = "内图廓.shp";
            errorDetail["shp_path"] = shpPath;
            result.errors["error1"] = errorDetail;
            return;
        }
        
        // 根据标准图层名称匹配对应的XML文件
        QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
        
        // 创建输出目录结构
        OutputPaths outPaths = CreateOutputDirectories(outputDir);
        
        // 构建输出文件路径
        QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("内容与图廓错位_%1.json").arg(standardLayer));
        QString errorShpPath = QDir(outPaths.errorResultDir).filePath(QString("内容与图廓错位_%1_errors.shp").arg(standardLayer));
        QString logPath = QDir(outPaths.logDir).filePath(QString("内容与图廓错位_%1.log").arg(standardLayer));
        
        // QByteArray生命周期管理
        QByteArray adminAreaBytes = adminAreaShpPath.toUtf8();
        QByteArray innerFrameBytes = innerFrameShpPath.toUtf8();
        QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
        QByteArray errorShpPathBytes = errorShpPath.toUtf8();
        QByteArray jsonPathBytes = jsonPath.toUtf8();
        QByteArray logPathBytes = logPath.toUtf8();
        
        const char* szAdminAreaShpPath = adminAreaBytes.constData();
        const char* szInnerFrameShpPath = innerFrameBytes.constData();
        const char* szSchemaXmlPath = xmlPathBytes.constData();
        const char* szOutputErrorLayerPath = errorShpPathBytes.constData();
        const char* szOutputJsonPath = jsonPathBytes.constData();
        const char* szOutputLogPath = logPathBytes.constData();
        
        // 调用C接口
        SE_Error error = ::ContentFrameMisalignmentCheck(
            szAdminAreaShpPath,
            szInnerFrameShpPath,
            szSchemaXmlPath,
            szOutputErrorLayerPath,
            szOutputJsonPath,
            szOutputLogPath,
            scale
        );
        
        // 使用通用解析函数解析JSON并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "311";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QString("内容与图廓错位检查失败，错误码: %1").arg(error);
            errorDetail["admin_area_shp"] = adminAreaShpPath;
            errorDetail["inner_frame_shp"] = innerFrameShpPath;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "311";
        }
    }
    catch (const std::exception& e) {
        result.id = "311";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}



// 401 - 投影参数准确性检查
void ProjectionParameterCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    // 使用传入的路径参数
    QString actualShpSemicolon = filePaths.actualShpSemicolon;
    QString standardLayer = filePaths.standardLayer;

    // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "401";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("投影参数准确性_%1.json").arg(standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QString("投影参数准确性_%1.log").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray actualShpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    QByteArray logPathBytes = logPath.toUtf8();
    
    const char* szShpFilePath = actualShpBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    const char* szLogPath = logPathBytes.constData();
    
    try {
        // 调用C接口
        SE_Error error = ::ProjectionParameterCheck(
            szShpFilePath,
            szSchemaXmlPath,
            scale,
            szOutputJsonPath,
            szLogPath
        );
        
        // 使用通用解析函数解析 JSON 并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            // 解析失败时的默认处理
            result.id = "401";
            result.name = checkItemName;
            result.status = "质检失败";
            result.err_exist = true;

            // --- 使用 QJsonObject 来构建错误详情 ---
            QJsonObject errorDetail;
            if (error != SE_ERROR_NONE) {
                errorDetail["error"] = QString("算法执行失败，错误码: %1").arg(error);
            }
            else {
                errorDetail["error"] = QString("无法解析结果文件，路径: %1").arg(jsonPath);
            }
            errorDetail["shp_path"] = actualShpSemicolon;
            errorDetail["xml_path"] = selectedXmlPath;
            errorDetail["scale"] = scale;

            result.errors["error1"] = errorDetail;
        }
        else {
            // 解析成功，确保ID正确
            result.id = "401";
        }
    }
    catch (const std::exception& e) {
        result.id = "401";
        result.name = checkItemName;
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 308 - 标准图例匹配检查
void LegendMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "308";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    try {
        // 使用传入的路径参数
        QString shpPath = filePaths.shpPath;
        QString actualShpSemicolon = filePaths.actualShpSemicolon;
        QString standardLayer = filePaths.standardLayer;
        QString legendField = filePaths.legendField;  // element_na值

       

        // 判断实际图层是否为空或不存在
    if (actualShpSemicolon.isEmpty()) {
        // 实际图层不存在，直接返回成功状态
        result.id = "308";
        result.name = checkItemName;
        result.status = "质检成功";
        result.err_exist = false;
        result.info["description"] = "实际图层不存在";
        return;
    }
        
        // 检查图例字段是否为空
        if (legendField.isEmpty()) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "CSV中未配置图例字段，无法执行标准图例匹配检查";
            errorDetail["layer"] = standardLayer;
            result.errors["error1"] = errorDetail;
            return;
        }
        
        // 处理可能包含分号的路径
        QString actualShpPath;
        if (actualShpSemicolon.contains(";")) {
            // 对于街区面图层，特殊处理：只选择包含"_Polygon"的文件
            if (standardLayer == "街区面") {
                QStringList pathList = actualShpSemicolon.split(";", QString::SkipEmptyParts);
                for (const QString& path : pathList) {
                    QString trimmedPath = path.trimmed();
                    if (trimmedPath.contains("_Polygon")) {
                        actualShpPath = trimmedPath;
                        break;
                    }
                }
                // 如果没找到_Polygon文件，则使用第一个
                if (actualShpPath.isEmpty()) {
                    actualShpPath = pathList.first().trimmed();
                }
            } else {
                // 其他图层使用默认逻辑：只取第一个
                actualShpPath = actualShpSemicolon.split(";").first().trimmed();
            }
        } else {
            actualShpPath = actualShpSemicolon;
        }
        
        if (actualShpPath.isEmpty()) {
            actualShpPath = actualShpSemicolon;
        }
        
        // 优先使用任务表中指定的标准图例文件夹；未指定时保持原有的自动查找行为。
        QString standardLegendDir = filePaths.legendDir.trimmed();
        if (standardLegendDir.isEmpty()) {
            standardLegendDir = QDir(shpPath).filePath(QString::fromUtf8(u8"标准图例"));
        }
        
        // 检查标准图例文件夹是否存在
        if (!QDir(standardLegendDir).exists()) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "标准图例文件夹不存在";
            errorDetail["standard_legend_dir"] = standardLegendDir;
            errorDetail["base_shp_path"] = shpPath;
            result.errors["error1"] = errorDetail;
            return;
        }
        
        // 检查待检查的图层文件是否存在
        if (!QFile::exists(actualShpPath)) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "待检查的地图图层文件不存在";
            errorDetail["map_layer_path"] = actualShpPath;
            errorDetail["original_path"] = actualShpSemicolon;
            result.errors["error1"] = errorDetail;
            return;
        }
    
        // 根据标准图层名称匹配对应的XML文件
        QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
        
        // 创建输出目录结构
        OutputPaths outPaths = CreateOutputDirectories(outputDir);
        
        // 构建输出文件路径
        QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("标准图例匹配_%1.json").arg(standardLayer));
        QString logPath = QDir(outPaths.logDir).filePath(QString("标准图例匹配_%1.log").arg(standardLayer));
        QString errorShpPath = QDir(outPaths.errorResultDir).filePath(QString("标准图例匹配错误_%1.shp").arg(standardLayer));
        
        // QByteArray生命周期管理
        QByteArray standardLegendDirBytes = standardLegendDir.toUtf8();
        QByteArray actualShpBytes = actualShpPath.toUtf8();  // 使用处理后的单个路径
        QByteArray legendFieldBytes = legendField.toUtf8();
        QByteArray errorShpPathBytes = errorShpPath.toUtf8();
        QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
        QByteArray jsonPathBytes = jsonPath.toUtf8();
        QByteArray logPathBytes = logPath.toUtf8();
        
        const char* szStandardLayerDir = standardLegendDirBytes.constData();
        const char* szMapLayerPath = actualShpBytes.constData();
        const char* szElementNaFilter = legendFieldBytes.constData();
        const char* szOutputShpPath = errorShpPathBytes.constData();
        const char* szSchemaXmlPath = xmlPathBytes.isEmpty() ? nullptr : xmlPathBytes.constData();
        const char* szOutputJsonPath = jsonPathBytes.constData();
        const char* szOutputLogPath = logPathBytes.constData();
        
        bool bPassed = false;
        
        // 调用C接口
        SE_Error error = ::LegendMatchCheck(
            szStandardLayerDir,      // 标准图例文件夹路径
            szMapLayerPath,          // 待检查的地图图层文件
            szElementNaFilter,       // element_na筛选值
            szOutputShpPath,         // 输出错误要素SHP路径
            szSchemaXmlPath,         // XML规则文件路径
            szOutputJsonPath,        // 输出JSON文件路径
            szOutputLogPath,         // 输出日志文件路径
            bPassed                  // 输出参数：检查是否通过
        );
        
        // 使用通用解析函数解析JSON并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "308";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QString("标准图例匹配检查失败，错误码: %1").arg(error);
            errorDetail["standard_legend_dir"] = standardLegendDir;
            errorDetail["map_layer"] = actualShpPath;
            errorDetail["element_na"] = legendField;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "308";
            // 如果算法返回了bPassed状态，可以根据需要设置
            if (!bPassed) {
                result.err_exist = true;
            }
        }
    }
    catch (const std::exception& e) {
        result.id = "308";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
    catch (...) {
        result.id = "308";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "发生未知异常（可能是spdlog日志初始化失败或GDAL崩溃）";
        errorDetail["suggestion"] = "请检查: 1)日志文件路径权限 2)GDAL是否正确初始化 3)输入文件格式";
        result.errors["exception"] = errorDetail;
    }
}

// 309 - 图名与行政区划匹配检查
void MapNameMatchCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "309";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    // 使用传入的路径参数
    QString shpPath = filePaths.shpPath;
    QString gdbShpPath = filePaths.gdbShpPath;
    QString standardLayer = filePaths.standardLayer;
    
    // 构建三个固定的shp文件路径
    QString mapNameShpPath = QDir(shpPath).filePath("图名.shp");
    QString adminColorShpPath = QDir(shpPath).filePath("行政区划面普色.shp");
    QString bouaDataPath = QDir(gdbShpPath).filePath("BOUA5.shp");
    
    // 检查文件是否存在，如果找不到行政区划面普色.shp就尝试找行政区域面普染.shp
    if (!QFile::exists(adminColorShpPath)) {
        adminColorShpPath = QDir(shpPath).filePath("行政区域面普染.shp");
        if (!QFile::exists(adminColorShpPath)) {
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = "行政区划面普色.shp和行政区域面普染.shp文件都不存在";
            errorDetail["path1"] = QDir(shpPath).filePath("行政区划面普色.shp");
            errorDetail["path2"] = adminColorShpPath;
            result.errors["error1"] = errorDetail;
            return;
        }
    }
    
    if (!QFile::exists(bouaDataPath)) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "BOUA5 gdb文件不存在";
        errorDetail["path"] = bouaDataPath;
        result.errors["error1"] = errorDetail;
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("图名与行政区划匹配_%1.json").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray mapNameBytes = mapNameShpPath.toUtf8();
    QByteArray adminColorBytes = adminColorShpPath.toUtf8();
    QByteArray bouaDataBytes = bouaDataPath.toUtf8();
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szMapNameShpPath = mapNameBytes.constData();
    const char* szAdminColorShpPath = adminColorBytes.constData();
    const char* szBouaDataPath = bouaDataBytes.constData();
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    
    try {
        bool bMatched = false;
        
        // 调用C接口
        SE_Error error = ::MapNameMatchCheck(
            szMapNameShpPath,
            szAdminColorShpPath,
            szBouaDataPath,
            szSchemaXmlPath,
            jsonPathBytes.constData(),
            bMatched
        );
        
        // 使用通用解析函数解析JSON并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "309";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QString("图名与行政区划匹配检查失败，错误码: %1").arg(error);
            errorDetail["map_name_shp"] = mapNameShpPath;
            errorDetail["admin_color_shp"] = adminColorShpPath;
            errorDetail["boua_data"] = bouaDataPath;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "309";
            // 如果算法返回了bMatched状态，可以根据需要设置
            if (!bMatched) {
                result.err_exist = true;
            }
        }
    }
    catch (const std::exception& e) {
        result.id = "309";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

// 310 - 必需图层检查
void RequiredLayersCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    result.id = "310";
    result.name = checkItemName;
    result.status = "未质检";
    result.err_exist = false;
    result.info = QJsonObject();
    result.errors = QJsonObject();
    
    // 使用传入的路径参数
    QString shpPath = filePaths.shpPath;
    QString standardLayer = filePaths.standardLayer;
    
    // 检查shp文件夹是否存在
    if (!QDir(shpPath).exists()) {
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = "shp文件夹不存在";
        errorDetail["path"] = shpPath;
        result.errors["error1"] = errorDetail;
        return;
    }
    
    // 根据标准图层名称匹配对应的XML文件
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, standardLayer);
    
    // 创建输出目录结构
    OutputPaths outPaths = CreateOutputDirectories(outputDir);
    
    // 构建输出文件路径
    QString jsonPath = QDir(outPaths.jsonDir).filePath(QString("必需图层检查_%1.json").arg(standardLayer));
    
    // QByteArray生命周期管理
    QByteArray xmlPathBytes = selectedXmlPath.toUtf8();
    QByteArray shpPathBytes = shpPath.toUtf8();
    QByteArray jsonPathBytes = jsonPath.toUtf8();
    
    const char* szSchemaXmlPath = xmlPathBytes.constData();
    const char* szDirectoryPath = shpPathBytes.constData();
    const char* szOutputJsonPath = jsonPathBytes.constData();
    
    try {
        bool bPassed = false;
        
        // 调用C接口
        SE_Error error = ::RequiredLayersCheck(
            szSchemaXmlPath,
            szDirectoryPath,
            szOutputJsonPath,
            bPassed
        );
        
        // 使用通用解析函数解析JSON并填充结果
        if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
            result.id = "310";
            result.status = "质检失败";
            result.err_exist = true;
            QJsonObject errorDetail;
            errorDetail["description"] = QString("必需图层检查失败，错误码: %1").arg(error);
            errorDetail["shp_path"] = shpPath;
            result.errors["error1"] = errorDetail;
        }
        else {
            result.id = "310";
            // 如果算法返回了bPassed状态，可以根据需要设置
            if (!bPassed) {
                result.err_exist = true;
            }
        }
    }
    catch (const std::exception& e) {
        result.id = "310";
        result.status = "质检失败";
        result.err_exist = true;
        QJsonObject errorDetail;
        errorDetail["description"] = QString("异常：%1").arg(e.what());
        result.errors["exception"] = errorDetail;
    }
}

//312 - 行政区划面设色检查
void AdminColorStyleCheckWrapper(const QString& checkItemName, const AlgorithmFilePaths& filePaths, int scale, const QString& outputDir, CheckItemResult& result)
{
    QString actualShpSemicolon = filePaths.actualShpSemicolon;

    if (actualShpSemicolon.isEmpty()) {
        result.id = "312";
        result.name = checkItemName;
        result.status = QStringLiteral("质检成功");
        result.err_exist = false;
        result.info["description"] = QStringLiteral("实际图层不存在");
        return;
    }

    // 按质检项名称匹配XML（兼容文件名不匹配的情况）
    QString selectedXmlPath = MatchXmlByLayerName(filePaths.xmlPaths, checkItemName);

    // 从CSV获取GDB图层名称（第2列：gdb图层），用于GDB名称反查
    QString gdbLayerName = filePaths.gdbLayer.split('|', Qt::SkipEmptyParts).value(0).trimmed();

    OutputPaths outPaths = CreateOutputDirectories(outputDir);

    QString jsonPath = QDir(outPaths.jsonDir).filePath(QStringLiteral("行政区划面设色检查_%1.json").arg(filePaths.standardLayer));
    QString logPath = QDir(outPaths.logDir).filePath(QStringLiteral("行政区划面设色检查_%1_%2.log")
        .arg(filePaths.standardLayer, QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));

    QByteArray shpBytes = actualShpSemicolon.toUtf8();
    QByteArray xmlBytes = selectedXmlPath.toUtf8();
    QByteArray jsonBytes = jsonPath.toUtf8();
    QByteArray gdbBytes = filePaths.gdbShpPath.toUtf8();
    QByteArray gdbLayerBytes = gdbLayerName.toUtf8();

    bool passed = false;
    QString algorithmLog;

    SE_Error error = AdminColorStyleCheck(
        shpBytes.constData(),
        xmlBytes.constData(),
        jsonBytes.constData(),
        passed,
        gdbBytes.constData(),
        &algorithmLog,
        gdbLayerBytes.constData());

    if (!algorithmLog.isEmpty()) {
        QFile logFile(logPath);
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&logFile);
            stream.setCodec("UTF-8");
            stream << algorithmLog;
            logFile.close();
        }
    }

    if (!ParseJsonToCheckItemResult(jsonPath, result, checkItemName)) {
        result.id = "312";
        result.name = checkItemName;
        result.status = QStringLiteral("质检失败");
        result.err_exist = true;
        QJsonObject errorDetail;
        if (error != SE_ERROR_NONE) {
            errorDetail["error"] = QStringLiteral("算法执行失败，错误码: %1").arg(error);
        } else {
            errorDetail["error"] = QStringLiteral("无法解析结果文件，路径: %1").arg(jsonPath);
        }
        errorDetail["shp_path"] = actualShpSemicolon;
        errorDetail["xml_path"] = selectedXmlPath;
        errorDetail["scale"] = scale;
        result.errors["error1"] = errorDetail;
    } else {
        result.id = "312";
    }
}
