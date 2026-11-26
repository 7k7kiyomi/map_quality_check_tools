#define MAKING_MY_PLUGIN_DLL
#include "ReadShp_interface.h"
#include "ReadShp.h"
#include "road_topology_network.h"
#include "water_result_classification.h"
#include "water_reference_chain.h"

#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSet>
#include <memory>
#include <functional>
#include <algorithm>
#include <QJsonArray> 
#include <QFile>
#include <QTextStream>

static void annotateWaterReferences(ReadShpDialog *dlg, QgsVectorLayer *parsed,
                                    QgsVectorLayer *reference, const QString &source,
                                    std::vector<QualityCheckResult> &results, bool direction)
{
    if (!dlg || !parsed || !reference || !dlg->isRiverFile(source)) return;
    double tolerance = dlg->m_directionDatabaseMatchTolerance;
    if (!direction) {
        const QString checkName = QStringLiteral("线要素拓扑检查");
        const QString parameter = QStringLiteral("database_match_tolerance");
        if (dlg->m_config && dlg->m_config->getCheckItemConfig(checkName).parameters.contains(parameter)) {
            tolerance = dlg->getConvertedParameter(checkName, parameter);
        } else {
            tolerance = dlg->m_connectivityTolerance;
            dlg->logTextEdit->append(QStringLiteral("水系归类：XML 未配置库体数据匹配容差，沿用原连通性容差 %1 米").arg(tolerance));
        }
    }
    const QString topologyFallbackParameter = QStringLiteral("topology_classification_fallback_tolerance");
    const bool hasTopologyFallback = !direction && dlg->m_config &&
        dlg->m_config->getCheckItemConfig(QStringLiteral("线要素拓扑检查")).parameters.contains(topologyFallbackParameter);
    const double topologyFallback = hasTopologyFallback ? dlg->getConvertedParameter(
        QStringLiteral("线要素拓扑检查"), topologyFallbackParameter) : 0;
    for (auto &result : results) {
        if (result.sourceFile != source || !result.feature.hasGeometry() ||
            !result.referenceSource.isEmpty()) continue;
        auto match = WaterResultClassification::classify(
            result.feature.geometry(), parsed->crs(), reference, tolerance);
        if (match.category.isEmpty() && !match.candidates.isEmpty())
            match = WaterResultClassification::classifyWithNeighbors(
                result.feature, parsed, reference, tolerance, match);
        if (hasTopologyFallback && match.category.isEmpty() && match.candidates.isEmpty())
            match = WaterResultClassification::classifyDirectionFallback(
                result.feature.geometry(), parsed->crs(), reference, tolerance, topologyFallback);
        const QString fallbackParameter = QStringLiteral("direction_classification_fallback_tolerance");
        if (direction && match.category.isEmpty() && match.candidates.isEmpty() && dlg->m_config &&
            dlg->m_config->getCheckItemConfig(QStringLiteral("几何走向异常检查")).parameters.contains(fallbackParameter)) {
            const double fallbackTolerance = dlg->getConvertedParameter(
                QStringLiteral("几何走向异常检查"), fallbackParameter);
            match = WaterResultClassification::classifyDirectionFallback(
                result.feature.geometry(), parsed->crs(), reference, tolerance, fallbackTolerance);
        }
        QString chainEvidence;
        if (!direction && !match.category.isEmpty() && match.referenceId < 0) {
            const auto chain = WaterReferenceChain::find(result.feature.geometry(), parsed->crs(),
                                                         reference, match.category, tolerance);
            if (chain.features.size() > 1) {
                QStringList ids;
                for (const auto& feature : chain.features) ids.append(QString::number(feature.id()));
                chainEvidence = QStringLiteral("已匹配连续库体河段FID %1").arg(ids.join(QStringLiteral(",")));
            }
        }
        result.referenceCategory = match.category;
        result.referenceValue = match.rawValue;
        result.referenceSource = reference->source();
        result.referenceFeatureId = match.referenceId;
        if (!match.evidence.isEmpty()) result.description += QStringLiteral("；库体归类依据：") + match.evidence;
        if (!chainEvidence.isEmpty()) result.description += QStringLiteral("；") + chainEvidence;
        if (hasTopologyFallback && match.category.isEmpty() && match.candidates.isEmpty()) {
            const QString evidence = WaterResultClassification::partialReferenceEvidence(
                result.feature.geometry(), parsed->crs(), reference, topologyFallback);
            if (!evidence.isEmpty()) result.description += QStringLiteral("；") + evidence;
        }
        if (!match.category.isEmpty() && match.referenceId < 0 && chainEvidence.isEmpty())
            result.description += QStringLiteral("；类别已确定，具体库体河段待确认");
        else if (match.category.isEmpty() && !match.candidates.isEmpty()) {
            QStringList candidates;
            for (auto it = match.candidates.cbegin(); it != match.candidates.cend(); ++it)
                candidates.append(QStringLiteral("%1(库体FID %2)").arg(it.key()).arg(it.value()));
            result.description += QStringLiteral("；候选类别：%1；相邻河段证据不足，待归类").arg(candidates.join(QStringLiteral("、")));
        }
    }
}
// All four task wrappers explicitly request check_standard_errors.shp.
static void bindOutputStandard(ReadShpDialog& dlg, const QString& outputPath,
    bool lineTopology, bool direction, bool polygon, bool rightAngle)
{
    QStringList prefixes;
    if (lineTopology) prefixes << QStringLiteral("线要素拓扑检查_") << QStringLiteral("线要素拓扑_");
    if (direction) prefixes << QStringLiteral("几何走向异常检查_") << QStringLiteral("几何走向异常_");
    if (polygon) prefixes << QStringLiteral("面要素重叠_间隙检查_") << QStringLiteral("面要素重叠间隙_");
    if (rightAngle) prefixes << QStringLiteral("直角化角度阈值控制_") << QStringLiteral("直角化角度阈值_");
    const QString requested = QFileInfo(outputPath).completeBaseName();
    const QString suffix = QStringLiteral("_errors");
    if (!requested.endsWith(suffix)) return;
    for (const auto& prefix : prefixes) {
        if (!requested.startsWith(prefix)) continue;
        const QString standard = requested.mid(prefix.size(),requested.size()-prefix.size()-suffix.size());
        if (!standard.isEmpty()) dlg.setProperty("quality/output_standard_layer",standard);
        return;
    }
}

// 返回 true 表示更像目录（不存在或不是 .shp 后缀）
static bool looksLikeDirPath(const QString& path) {
    QFileInfo fi(path);
    if (fi.exists()) return fi.isDir();
    const QString lower = path.toLower();
    if (lower.endsWith(".shp")) return false;
    return true;
}

// 确保 parentDir/subdirName 子目录存在并返回其绝对路径
static QString ensureSubfolderCreated(const QString& parentDir, const QString& subdirName) {
    if (parentDir.isEmpty()) return parentDir;
    // 先确保父目录存在
    if (!QDir(parentDir).exists()) {
        QDir().mkpath(parentDir);
    }
    // 拼接子目录并确保存在
    QString full = QDir(parentDir).filePath(subdirName);
    if (!QDir(full).exists()) {
        QDir().mkpath(full);
    }
    return full;
}

// 从 shp 输出参数推导“质检结果”目录（支持传入目录或 .shp 文件路径）
static QString computeResultsDirFromShpParam(const QString& shpParam) {
    QFileInfo fi(shpParam);
    return looksLikeDirPath(shpParam) ? fi.absoluteFilePath() : fi.dir().absolutePath();
}
static QString computeLogsDirFromLogParam(const QString& logParam);
static ReadShp_error validatePathsOneLayerBatch(
    const char** parsed, int parsedCount,
    const char* xml, const char* outShp, const char* outJson, const char* outLogDir)
{
    if (!parsed || parsedCount <= 0 || !xml || !outShp || !outJson || !outLogDir)
        return READSHP_INVALID_PARAM;

    if (!QFileInfo::exists(QString::fromUtf8(xml))) return READSHP_FILE_NOT_FOUND;
    for (int i = 0; i < parsedCount; ++i) {
        if (!parsed[i] || !QFileInfo::exists(QString::fromUtf8(parsed[i])))
            return READSHP_FILE_NOT_FOUND;
    }

    const QString outShpParent = computeResultsDirFromShpParam(QString::fromUtf8(outShp));
    const QString outLogParent = computeLogsDirFromLogParam(QString::fromUtf8(outLogDir));
    QDir outDirShp(outShpParent);
    QDir outDirJson = QFileInfo(QString::fromUtf8(outJson)).dir();
    QDir outDirLog(outLogParent);
    if (!outDirShp.exists()) outDirShp.mkpath(outShpParent);
    if (!outDirJson.exists()) outDirJson.mkpath(outDirJson.absolutePath());
    if (!outDirLog.exists()) outDirLog.mkpath(outLogParent);

    return READSHP_SUCCESS;
}
// 从日志参数推导“质检日志”目录（支持传入目录或日志文件路径）
static QString computeLogsDirFromLogParam(const QString& logParam) {
    QFileInfo fi(logParam);
    if (fi.exists()) {
        return fi.isDir() ? fi.absoluteFilePath() : fi.dir().absolutePath();
    }
    const QString suffix = fi.suffix().toLower();
    const bool looksLikeFile = !suffix.isEmpty();
    return looksLikeFile ? fi.dir().absolutePath() : fi.absoluteFilePath();
}
static bool exportActiveLogs(ReadShpDialog* dlg,
    const QString& dirPath,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle)
{
    QDir outDir(dirPath);
    if (!outDir.exists()) {
        QDir().mkpath(outDir.absolutePath());
    }

    auto safeName = [](QString s) {
        s.replace("/", "_").replace("\\", "_").replace(":", "_")
            .replace("*", "_").replace("?", "_").replace("\"", "_")
            .replace("<", "_").replace(">", "_").replace("|", "_");
        return s.trimmed().isEmpty() ? QString::fromUtf8(u8"未命名图层") : s.trimmed();
    };

    QString standardName = dlg->standardLayerCombo ? dlg->standardLayerCombo->currentText() : "";
    if (standardName.trimmed().isEmpty()) {
        if (!dlg->lastCheckResults.empty() && !dlg->lastCheckResults.front().sourceFile.isEmpty()) {
            standardName = QFileInfo(dlg->lastCheckResults.front().sourceFile).completeBaseName();
        }
    }
    const QString safeStandardName = safeName(standardName);
    const QString content = dlg->logTextEdit ? dlg->logTextEdit->toPlainText() : QString();

    auto writeOne = [&](const QString& itemLabel) -> bool {
        const QString base = safeName(QString("%1_%2").arg(safeStandardName).arg(itemLabel));
        const QString path = outDir.filePath(base + ".log");
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            // 不抛错到 UI，只返回失败
            return false;
        }
        QTextStream out(&f);
        out.setCodec("UTF-8");
        out << content;
        f.close();
        return true;
    };

    // 优先用内部算法设置的执行标记
    if (dlg->executedLineTopology) return writeOne(QString::fromUtf8(u8"线要素拓扑检查"));
    if (dlg->executedDirection)    return writeOne(QString::fromUtf8(u8"几何走向异常检查"));
    if (dlg->executedPolygon)      return writeOne(QString::fromUtf8(u8"面要素重叠/间隙检查"));
    if (dlg->executedRightAngle)   return writeOne(QString::fromUtf8(u8"直角化角度阈值控制"));

    // 早期失败场景回退到接口层的选择标记
    if (markLineTopo)   return writeOne(QString::fromUtf8(u8"线要素拓扑检查"));
    if (markDirection)  return writeOne(QString::fromUtf8(u8"几何走向异常检查"));
    if (markPolygon)    return writeOne(QString::fromUtf8(u8"面要素重叠/间隙检查"));
    if (markRightAngle) return writeOne(QString::fromUtf8(u8"直角化角度阈值控制"));

    // 没有任何标记，导出一次通用日志
    return writeOne(QString::fromUtf8(u8"通用日志"));
}
static ReadShp_error validatePathsTwoLayersBatch(
    const char** parsed, int parsedCount,
    const char** reference, int referenceCount,
    const char* xml, const char* outShp, const char* outJson, const char* outLogDir)
{
    if (!parsed || parsedCount <= 0 || !reference || referenceCount <= 0 ||
        !xml || !outShp || !outJson || !outLogDir) {
        return READSHP_INVALID_PARAM;
    }
    if (!QFileInfo::exists(QString::fromUtf8(xml))) return READSHP_FILE_NOT_FOUND;
    for (int i = 0; i < parsedCount; ++i) {
        if (!parsed[i] || !QFileInfo::exists(QString::fromUtf8(parsed[i])))
            return READSHP_FILE_NOT_FOUND;
    }
    for (int j = 0; j < referenceCount; ++j) {
        if (!reference[j] || !QFileInfo::exists(QString::fromUtf8(reference[j])))
            return READSHP_FILE_NOT_FOUND;
    }

    const QString outShpParent = computeResultsDirFromShpParam(QString::fromUtf8(outShp));
    const QString outLogParent = computeLogsDirFromLogParam(QString::fromUtf8(outLogDir));
    QDir outDirShp(outShpParent);
    QDir outDirJson = QFileInfo(QString::fromUtf8(outJson)).dir();
    QDir outDirLog(outLogParent);
    if (!outDirShp.exists()) outDirShp.mkpath(outShpParent);
    if (!outDirJson.exists()) outDirJson.mkpath(outDirJson.absolutePath());
    if (!outDirLog.exists()) outDirLog.mkpath(outLogParent);

    return READSHP_SUCCESS;
}

// validate for two layers
static ReadShp_error validatePathsTwoLayers(const char* parsed, const char* reference,
    const char* xml, const char* outShp, const char* outJson, const char* outLogDir) {
    if (!parsed || !reference || !xml || !outShp || !outJson || !outLogDir) return READSHP_INVALID_PARAM;

    if (!QFileInfo::exists(QString::fromUtf8(parsed))) return READSHP_FILE_NOT_FOUND;
    if (!QFileInfo::exists(QString::fromUtf8(reference))) return READSHP_FILE_NOT_FOUND;
    if (!QFileInfo::exists(QString::fromUtf8(xml))) return READSHP_FILE_NOT_FOUND;

    // 结果与日志固定子目录
    const QString outShpParent = computeResultsDirFromShpParam(QString::fromUtf8(outShp));
    const QString outLogParent = computeLogsDirFromLogParam(QString::fromUtf8(outLogDir));

    // 创建目录：质检结果、JSON所在目录、质检日志
    QDir outDirShp(outShpParent);
    QDir outDirJson = QFileInfo(QString::fromUtf8(outJson)).dir();
    QDir outDirLog(outLogParent);
    if (!outDirShp.exists()) outDirShp.mkpath(outShpParent);
    if (!outDirJson.exists()) outDirJson.mkpath(outDirJson.absolutePath());
    if (!outDirLog.exists()) outDirLog.mkpath(outLogParent);

    return READSHP_SUCCESS;
}

// validate for one layer
static ReadShp_error validatePathsOneLayer(const char* parsed,
    const char* xml, const char* outShp, const char* outJson, const char* outLogDir) {
    if (!parsed || !xml || !outShp || !outJson || !outLogDir) return READSHP_INVALID_PARAM;

    if (!QFileInfo::exists(QString::fromUtf8(parsed))) return READSHP_FILE_NOT_FOUND;
    if (!QFileInfo::exists(QString::fromUtf8(xml))) return READSHP_FILE_NOT_FOUND;

    const QString outShpParent = computeResultsDirFromShpParam(QString::fromUtf8(outShp));
    const QString outLogParent = computeLogsDirFromLogParam(QString::fromUtf8(outLogDir));

    QDir outDirShp(outShpParent);
    QDir outDirJson = QFileInfo(QString::fromUtf8(outJson)).dir();
    QDir outDirLog(outLogParent);
    if (!outDirShp.exists()) outDirShp.mkpath(outShpParent);
    if (!outDirJson.exists()) outDirJson.mkpath(outDirJson.absolutePath());
    if (!outDirLog.exists()) outDirLog.mkpath(outLogParent);

    return READSHP_SUCCESS;
}

// load layer
static QgsVectorLayer* loadVectorLayer(const QString& path) {
    QString name = QFileInfo(path).baseName();
    QgsVectorLayer* layer = new QgsVectorLayer(path, name, "ogr");
    if (!layer || !layer->isValid()) {
        if (layer) delete layer;
        return nullptr;
    }
    return layer;
}

static QString numToStr(double v) {
    return std::isfinite(v) ? QString::number(v, 'f', 6) : QString("");
}

// build JSON root (match UI wording)
static QJsonObject buildJsonRootForResults(
    const std::vector<QualityCheckResult>& allResults,
    bool lineTopoSelected,
    bool directionSelected,
    bool polygonSelected,
    bool rightAngleSelected,
    bool lineTopoExecuted,
    bool directionExecuted,
    bool polygonExecuted,
    bool rightAngleExecuted)
{
    auto collectErrors = [&](std::function<bool(const QualityCheckResult&)> pred) -> QList<QualityCheckResult> {
        QList<QualityCheckResult> errors;
        for (const auto& r : allResults) {
            if (pred(r) && !r.passed) {
                errors.append(r);
            }
        }
        return errors;
    };

    auto linePred = [](const QualityCheckResult& r) {
        return r.checkType == CheckType::LINE_TOPOLOGY_ERROR
            || r.checkType == CheckType::NETWORK_CONNECTIVITY_ERROR;
    };
    auto dirPred = [](const QualityCheckResult& r) {
        return r.checkType == CheckType::DIRECTION_ERROR;
    };
    auto polyPred = [](const QualityCheckResult& r) {
        return r.checkType == CheckType::POLYGON_TOPOLOGY_ERROR;
    };
    auto rightPred = [](const QualityCheckResult& r) {
        return r.checkType == CheckType::RIGHT_ANGLE_THRESHOLD_ERROR;
    };

    QList<QualityCheckResult> lineErrors = collectErrors(linePred);
    QList<QualityCheckResult> dirErrors = collectErrors(dirPred);
    QList<QualityCheckResult> polyErrors = collectErrors(polyPred);
    QList<QualityCheckResult> rightErrors = collectErrors(rightPred);

    auto buildCheckItemDetailed = [&](const QString& id, const QString& name,
        bool selected, bool executed, const QList<QualityCheckResult>& errors) -> QJsonObject {
            QJsonObject item;
            item.insert("id", id);
            item.insert("name", name);

            // 新语义：status 仅由 selected + executed 决定
            const QString status = !selected
                ? QString::fromUtf8("未质检")
                : (executed ? QString::fromUtf8("质检成功") : QString::fromUtf8("质检失败"));
            item.insert("status", status);
            // 是否存在错误仍然由错误数组决定
            item.insert("err_exist", (selected && !errors.isEmpty()) ? "true" : "false");

            // info 对象
            QJsonObject infoObj;
            if (!selected) {
                infoObj.insert("summary", "");
                infoObj.insert("error_count", 0);
                infoObj.insert("error_types", QJsonArray{});
            }
            else if (!executed) {
                infoObj.insert("summary", QString::fromUtf8(u8"未执行检查或执行失败，请查看输入、配置和日志"));
                infoObj.insert("error_count", 0);
                infoObj.insert("error_types", QJsonArray{});
            }
            else if (!errors.isEmpty()) {
                QSet<QString> cats;
                for (const auto& e : errors) {
                    if (!e.errorType.isEmpty()) cats.insert(e.errorType);
                }
                const QString summary = cats.isEmpty()
                    ? QString::fromUtf8(u8"发现错误，共%1条").arg(errors.size())
                    : QString::fromUtf8(u8"出现%1类，共%2条")
                    .arg(QStringList(cats.begin(), cats.end()).join(QString::fromUtf8(u8"、")))
                    .arg(errors.size());

                infoObj.insert("summary", summary);
                infoObj.insert("error_count", errors.size());

                QJsonArray typesArr;
                for (const auto& t : QStringList(cats.begin(), cats.end())) {
                    typesArr.append(t);
                }
                infoObj.insert("error_types", typesArr);
            }
            else {
                infoObj.insert("summary", QString::fromUtf8(u8"未发现错误"));
                infoObj.insert("error_count", 0);
                infoObj.insert("error_types", QJsonArray{});
            }
            item.insert("info", infoObj);

            // 错误明细数组
            QJsonArray errorsArr;
            if (selected && !errors.isEmpty()) {
                for (const auto& e : errors) {
                    QJsonObject err;
                    qint64 fid = e.featureId != 0 ? e.featureId : e.feature.id();

                    err.insert("error_type", e.errorType);
                    err.insert("description", e.description);
                    err.insert("calc_value", e.calculatedValue);
                    err.insert("std_value", e.standardValue);
                    err.insert("feature_id", static_cast<int>(fid));
                    err.insert("source_file", e.sourceFile);
                    if (!e.referenceSource.isEmpty()) {
                        err.insert("reference_source", e.referenceSource);
                        err.insert("reference_fid", static_cast<double>(e.referenceFeatureId));
                        err.insert("reference_category", e.referenceCategory);
                        err.insert("reference_value", e.referenceValue);
                    }
                    err.insert("check_type", static_cast<int>(e.checkType));

                    errorsArr.append(err);
                }
            }
            item.insert("errors", errorsArr);
            return item;
    };

    QJsonObject root;
    root.insert("check_item1",
        buildCheckItemDetailed("112", QString::fromUtf8(u8"线要素拓扑检查"),
            lineTopoSelected, lineTopoExecuted, lineErrors));
    root.insert("check_item2",
        buildCheckItemDetailed("105", QString::fromUtf8(u8"几何走向异常检查"),
            directionSelected, directionExecuted, dirErrors));
    root.insert("check_item3",
        buildCheckItemDetailed("111", QString::fromUtf8(u8"面要素重叠/间隙检查"),
            polygonSelected, polygonExecuted, polyErrors));
    root.insert("check_item4",
        buildCheckItemDetailed("109", QString::fromUtf8(u8"直角化角度阈值控制"),
            rightAngleSelected, rightAngleExecuted, rightErrors));
    return root;
}
// exportStatusOnValidationFailure
static void exportStatusOnValidationFailure(
    const QString& outJsonPath,
    const QString& outLogDir,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle)
{
    ReadShpDialog dlg(nullptr);
    // JSON 目录
    QDir outDirJson = QFileInfo(outJsonPath).dir();
    if (!outDirJson.exists()) outDirJson.mkpath(outDirJson.absolutePath());

    // 推导并创建“质检日志”目录
    const QString outLogParent = computeLogsDirFromLogParam(outLogDir);
    QDir outDirLog(outLogParent);
    if (!outDirLog.exists()) outDirLog.mkpath(outLogParent);

    // 校验失败：执行标记全部 false
    QJsonObject root = buildJsonRootForResults(
        std::vector<QualityCheckResult>{},
        markLineTopo, markDirection, markPolygon, markRightAngle,
        false, false, false, false);

    dlg.exportResultsToJson(outJsonPath, root);
    // 按选择标记导出一次对应检查日志（校验失败没有执行标记）
    exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
}
static ReadShp_error runTwoLayerCheckAndExportBatch(
    const QStringList& parsedPaths,
    const QStringList& referencePaths,
    const QString& xmlPath,
    const QString& outShpPath,
    const QString& outJsonPath,
    const QString& outLogDir,
    int scale,
    std::function<std::vector<QualityCheckResult>(ReadShpDialog*, QgsVectorLayer*, QgsVectorLayer*, const QString&)> checkFn,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle,
    std::function<bool(ReadShpDialog*, QgsVectorLayer*, QgsVectorLayer*)> configureLayers = {}
) {
    ReadShpDialog dlg(nullptr);
    dlg.m_configFilePath = xmlPath;
    dlg.loadConfigFromXML();
    dlg.m_mapScale = scale;  // 以外部传入的scale为准
    dlg.logXMLDebugInfo(QString("外部比例尺生效: 1:%1").arg(dlg.m_mapScale));
    dlg.updateParametersFromConfig();  // 重新做单位换算
    dlg.displayXMLConfigInfo();        // 刷新XML调试区，展示转换后参数
    dlg.resetExecutionFlags();

    const QString outShpParent = computeResultsDirFromShpParam(outShpPath);
    const QString outLogParent = computeLogsDirFromLogParam(outLogDir);

    bindOutputStandard(dlg,outShpPath,markLineTopo,markDirection,markPolygon,markRightAngle);

    auto fail = [&](ReadShp_error code, const QString& reason) {
        dlg.executedLineTopology = false;
        dlg.logTextEdit->append(reason);
        QJsonObject root = buildJsonRootForResults({},markLineTopo,markDirection,markPolygon,markRightAngle,false,false,false,false);
        if (markLineTopo) {
            QJsonObject item = root.value("check_item1").toObject();
            QJsonObject info = item.value("info").toObject();
            info["summary"] = reason; item["info"] = info; root["check_item1"] = item;
        }
        dlg.exportResultsToJson(outJsonPath,root);
        exportActiveLogs(&dlg,outLogParent,markLineTopo,markDirection,markPolygon,markRightAngle);
        return code;
    };
    if (markLineTopo && !dlg.m_config->isCheckItemEnabled(QStringLiteral("线要素拓扑检查")))
        return fail(READSHP_INVALID_PARAM,QStringLiteral("未执行：所选XML缺少线要素拓扑检查112规则"));

    std::vector<std::unique_ptr<QgsVectorLayer>> parsedLayers;
    std::vector<std::unique_ptr<QgsVectorLayer>> referenceLayers;
    for (const QString& p : parsedPaths) {
        std::unique_ptr<QgsVectorLayer> l(loadVectorLayer(p));
        if (!l) return fail(READSHP_LAYER_LOAD_ERROR,QStringLiteral("未执行：无法加载成果图层 %1").arg(p));
        parsedLayers.push_back(std::move(l));
    }
    for (const QString& r : referencePaths) {
        std::unique_ptr<QgsVectorLayer> l(loadVectorLayer(r));
        if (!l) return fail(READSHP_LAYER_LOAD_ERROR,QStringLiteral("未执行：无法加载参考图层 %1").arg(r));
        referenceLayers.push_back(std::move(l));
    }
    dlg.parseFilePaths = parsedPaths;
    dlg.referenceFilePaths = referencePaths;
    if (parsedLayers.empty() || referenceLayers.empty()) {
        QJsonObject root = buildJsonRootForResults(std::vector<QualityCheckResult>{},
            markLineTopo, markDirection, markPolygon, markRightAngle,
            false, false, false, false);
        dlg.exportResultsToJson(outJsonPath, root);
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_LAYER_LOAD_ERROR;
    }

    std::vector<QualityCheckResult> results;
    QSet<QString> seen;
    qint64 inputLineFeatures = 0;
    int inputLineLayers = 0;
    QJsonArray skippedNonLineLayers;
    for (int i = 0; i < parsedPaths.size(); ++i) {
        if (markLineTopo && parsedLayers[i]->geometryType() != QgsWkbTypes::LineGeometry) {
            skippedNonLineLayers.append(parsedPaths[i]);
            dlg.logTextEdit->append(QStringLiteral("线拓扑排除非线成果图层：%1").arg(parsedPaths[i]));
            continue;
        }
        if (markLineTopo) { ++inputLineLayers; inputLineFeatures += parsedLayers[i]->featureCount(); }
        for (int j = 0; j < referencePaths.size(); ++j) {
            const QString cp = QFileInfo(parsedPaths[i]).canonicalFilePath();
            const QString cr = QFileInfo(referencePaths[j]).canonicalFilePath();
            if (cp.isEmpty() || cr.isEmpty()) continue;
            const QString key = cp + "||" + cr;
            if (seen.contains(key)) continue;
            seen.insert(key);

            const QString src = QFileInfo(parsedPaths[i]).fileName();
            // The result-road network is independent of reference pairing.
            if (markLineTopo && j > 0 && RoadTopology::family(src) == RoadTopology::Road) continue;
            if (configureLayers && !configureLayers(&dlg,parsedLayers[i].get(),referenceLayers[j].get()))
                return fail(READSHP_INVALID_PARAM,QStringLiteral("未执行：无法绑定CSV标准图层语义"));
            std::vector<QualityCheckResult> r;
            try { r = checkFn(&dlg, parsedLayers[i].get(), referenceLayers[j].get(), src); }
            catch (const std::exception& e) {
                return fail(READSHP_INVALID_PARAM,QStringLiteral("拓扑执行失败：%1").arg(QString::fromUtf8(e.what())));
            }
            if (markLineTopo || markDirection)
                annotateWaterReferences(&dlg, parsedLayers[i].get(), referenceLayers[j].get(),
                                        src, r, markDirection);
            if (markLineTopo) {
                for (QualityCheckResult& issue : r) {
                    // Basenames can be identical in different input directories.
                    if (issue.sourceFile == src) issue.sourceFile = cp;
                }
            }
            results.insert(results.end(), r.begin(), r.end());
        }
    }

    if (markLineTopo && !dlg.executedLineTopology)
        return fail(READSHP_GEOMETRY_INVALID,QStringLiteral("未执行：没有可检查的成果线图层"));
    if (markLineTopo) mergeDuplicateLineTopologyErrorRecords(results);
    dlg.lastCheckResults = results;
    const bool hasErrors = std::any_of(results.begin(), results.end(), [](const QualityCheckResult& r) { return !r.passed; });
    if (hasErrors) {
        if (!dlg.exportErrorsToShp(outShpParent)) {
            exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
            return READSHP_GEOMETRY_INVALID;
        }
    }

    QJsonObject root = buildJsonRootForResults(results,
        markLineTopo, markDirection, markPolygon, markRightAngle,
        dlg.executedLineTopology, dlg.executedDirection, dlg.executedPolygon, dlg.executedRightAngle);
    if (markLineTopo) {
        QJsonObject item = root.value("check_item1").toObject();
        QJsonObject info = item.value("info").toObject();
        int invalidCount = 0;
        for (const auto& issue : results) if (issue.errorType == QStringLiteral("几何体无效")) ++invalidCount;
        info["input_line_layer_count"] = inputLineLayers;
        info["input_line_feature_count"] = static_cast<double>(inputLineFeatures);
        info["participating_line_feature_count"] = static_cast<double>(inputLineFeatures - invalidCount);
        info["skipped_invalid_feature_count"] = invalidCount;
        info["skipped_non_line_layers"] = skippedNonLineLayers;
        item["info"] = info; root["check_item1"] = item;
    }
    if (!dlg.exportResultsToJson(outJsonPath, root)) {
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_FAIL;
    }

    exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
    return READSHP_SUCCESS;
}
static ReadShp_error runOneLayerCheckAndExportBatch(
    const QStringList& parsedPaths,
    const QString& xmlPath,
    const QString& outShpPath,
    const QString& outJsonPath,
    const QString& outLogDir,
    int scale,
    std::function<std::vector<QualityCheckResult>(ReadShpDialog*, QgsVectorLayer*, const QString&)> checkFn,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle
) {
    ReadShpDialog dlg(nullptr);
    dlg.m_configFilePath = xmlPath;
    dlg.loadConfigFromXML();
    dlg.m_mapScale = scale;  // 以外部传入的scale为准
    dlg.logXMLDebugInfo(QString("外部比例尺生效: 1:%1").arg(dlg.m_mapScale));
    dlg.updateParametersFromConfig();
    dlg.displayXMLConfigInfo();
    dlg.resetExecutionFlags();

    const QString outShpParent = computeResultsDirFromShpParam(outShpPath);
    const QString outLogParent = computeLogsDirFromLogParam(outLogDir);

    bindOutputStandard(dlg,outShpPath,markLineTopo,markDirection,markPolygon,markRightAngle);
    std::vector<std::unique_ptr<QgsVectorLayer>> parsedLayers;
    for (const QString& p : parsedPaths) {
        std::unique_ptr<QgsVectorLayer> l(loadVectorLayer(p));
        if (l) parsedLayers.push_back(std::move(l));
    }
    if (parsedLayers.empty()) {
        QJsonObject root = buildJsonRootForResults(std::vector<QualityCheckResult>{},
            markLineTopo, markDirection, markPolygon, markRightAngle,
            false, false, false, false);
        dlg.exportResultsToJson(outJsonPath, root);
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_LAYER_LOAD_ERROR;
    }

    std::vector<QualityCheckResult> results;
    for (int i = 0; i < parsedPaths.size(); ++i) {
        const QString src = QFileInfo(parsedPaths[i]).fileName();
        std::vector<QualityCheckResult> r = checkFn(&dlg, parsedLayers[i].get(), src);
        if (markLineTopo) mergeDuplicateLineTopologyErrorRecords(r);
        results.insert(results.end(), r.begin(), r.end());
    }

    dlg.lastCheckResults = results;
    const bool hasErrors = std::any_of(results.begin(), results.end(), [](const QualityCheckResult& r) { return !r.passed; });
    if (hasErrors) {
        if (!dlg.exportErrorsToShp(outShpParent)) {
            exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
            return READSHP_GEOMETRY_INVALID;
        }
    }

    QJsonObject root = buildJsonRootForResults(results,
        markLineTopo, markDirection, markPolygon, markRightAngle,
        dlg.executedLineTopology, dlg.executedDirection, dlg.executedPolygon, dlg.executedRightAngle);
    if (!dlg.exportResultsToJson(outJsonPath, root)) {
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_FAIL;
    }

    exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
    return READSHP_SUCCESS;
}
// run (two-layer)
static ReadShp_error runTwoLayerCheckAndExport(
    const QString& parsedPath,
    const QString& referencePath,
    const QString& xmlPath,
    const QString& outShpPath,
    const QString& outJsonPath,
    const QString& outLogDir,
    int scale,
    std::function<std::vector<QualityCheckResult>(ReadShpDialog*, QgsVectorLayer*, QgsVectorLayer*, const QString&)> checkFn,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle,
    std::function<bool(ReadShpDialog*, QgsVectorLayer*, QgsVectorLayer*)> configureLayers = std::function<bool(ReadShpDialog*, QgsVectorLayer*, QgsVectorLayer*)>()
) {
    ReadShpDialog dlg(nullptr);
    dlg.m_configFilePath = xmlPath;
    dlg.loadConfigFromXML();
    dlg.m_mapScale = scale;  // 以外部传入的scale为准
    dlg.logXMLDebugInfo(QString("外部比例尺生效: 1:%1").arg(dlg.m_mapScale));
    dlg.updateParametersFromConfig();
    dlg.displayXMLConfigInfo();
    dlg.resetExecutionFlags();

    // 推导固定子目录
    const QString outShpParent = computeResultsDirFromShpParam(outShpPath);
    const QString outLogParent = computeLogsDirFromLogParam(outLogDir);

    bindOutputStandard(dlg,outShpPath,markLineTopo,markDirection,markPolygon,markRightAngle);
    std::unique_ptr<QgsVectorLayer> parsed(loadVectorLayer(parsedPath));
    std::unique_ptr<QgsVectorLayer> reference(loadVectorLayer(referencePath));
    dlg.parseFilePaths = QStringList{ parsedPath };
    dlg.referenceFilePaths = QStringList{ referencePath };
    if (!parsed || !reference) {
        QJsonObject root = buildJsonRootForResults(
            std::vector<QualityCheckResult>{},
            markLineTopo, markDirection, markPolygon, markRightAngle,
            false, false, false, false);
        dlg.exportResultsToJson(outJsonPath, root);
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_LAYER_LOAD_ERROR;
    }

    if (configureLayers && !configureLayers(&dlg, parsed.get(), reference.get())) {
        QJsonObject root = buildJsonRootForResults(
            std::vector<QualityCheckResult>{},
            markLineTopo, markDirection, markPolygon, markRightAngle,
            false, false, false, false);
        dlg.exportResultsToJson(outJsonPath, root);
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_INVALID_PARAM;
    }

    const QString sourceFile = QFileInfo(parsed->source()).fileName();
    std::vector<QualityCheckResult> results = checkFn(&dlg, parsed.get(), reference.get(), sourceFile);
    if (markLineTopo || markDirection)
        annotateWaterReferences(&dlg, parsed.get(), reference.get(), sourceFile, results, markDirection);
    if (markLineTopo) mergeDuplicateLineTopologyErrorRecords(results);

    dlg.lastCheckResults = results;
    const bool hasErrors = std::any_of(results.begin(), results.end(),
        [](const QualityCheckResult& r) { return !r.passed; });

    if (hasErrors) {
        if (!dlg.exportErrorsToShp(outShpParent)) {
            exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
            return READSHP_GEOMETRY_INVALID;
        }
    }

    QJsonObject root = buildJsonRootForResults(
        results,
        markLineTopo, markDirection, markPolygon, markRightAngle,
        dlg.executedLineTopology, dlg.executedDirection, dlg.executedPolygon, dlg.executedRightAngle);
    if (!dlg.exportResultsToJson(outJsonPath, root)) {
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_FAIL;
    }

    // 成功完成后也只导出“实际执行”的检查日志
    exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
    return READSHP_SUCCESS;
}

// run (one-layer)
static ReadShp_error runOneLayerCheckAndExport(
    const QString& parsedPath,
    const QString& xmlPath,
    const QString& outShpPath,
    const QString& outJsonPath,
    const QString& outLogDir,
    int scale,
    std::function<std::vector<QualityCheckResult>(ReadShpDialog*, QgsVectorLayer*, const QString&)> checkFn,
    bool markLineTopo, bool markDirection, bool markPolygon, bool markRightAngle
) {
    ReadShpDialog dlg(nullptr);
    dlg.m_configFilePath = xmlPath;
    dlg.loadConfigFromXML();
    dlg.m_mapScale = scale;  // 以外部传入的scale为准
    dlg.logXMLDebugInfo(QString("外部比例尺生效: 1:%1").arg(dlg.m_mapScale));
    dlg.updateParametersFromConfig();
    dlg.displayXMLConfigInfo();
    dlg.resetExecutionFlags();

    // 推导固定子目录
    const QString outShpParent = computeResultsDirFromShpParam(outShpPath);
    const QString outLogParent = computeLogsDirFromLogParam(outLogDir);

    bindOutputStandard(dlg,outShpPath,markLineTopo,markDirection,markPolygon,markRightAngle);
    std::unique_ptr<QgsVectorLayer> parsed(loadVectorLayer(parsedPath));
    if (!parsed) {
        QJsonObject root = buildJsonRootForResults(
            std::vector<QualityCheckResult>{},
            markLineTopo, markDirection, markPolygon, markRightAngle,
            false, false, false, false);
        dlg.exportResultsToJson(outJsonPath, root);
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_LAYER_LOAD_ERROR;
    }

    const QString sourceFile = QFileInfo(parsed->source()).fileName();
    std::vector<QualityCheckResult> results = checkFn(&dlg, parsed.get(), sourceFile);

    dlg.lastCheckResults = results;
    const bool hasErrors = std::any_of(results.begin(), results.end(),
        [](const QualityCheckResult& r) { return !r.passed; });
    if (hasErrors) {
        if (!dlg.exportErrorsToShp(outShpParent)) {
            exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
            return READSHP_GEOMETRY_INVALID;
        }
    }


    QJsonObject root = buildJsonRootForResults(
        results,
        markLineTopo, markDirection, markPolygon, markRightAngle,
        dlg.executedLineTopology, dlg.executedDirection, dlg.executedPolygon, dlg.executedRightAngle);
    if (!dlg.exportResultsToJson(outJsonPath, root)) {
        exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
        return READSHP_FAIL;
    }

    exportActiveLogs(&dlg, outLogParent, markLineTopo, markDirection, markPolygon, markRightAngle);
    return READSHP_SUCCESS;
}
extern "C" {

    // 函数：checkRightAngleThreshold
    MY_PLUGIN_API ReadShp_error checkRightAngleThreshold(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayers(szParsedShpFilePath, szReferenceShpFilePath, szXmlConfigPath,
            szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, false, false, true);
            return v;
        }

        return runTwoLayerCheckAndExport(
            QString::fromUtf8(szParsedShpFilePath),
            QString::fromUtf8(szReferenceShpFilePath),
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                return dlg->checkRightAngleThreshold(pl, rl, src);
            },
            false, false, false, true
                );
    }

    // 函数：checkPolygonOverlapGap
    MY_PLUGIN_API ReadShp_error checkPolygonOverlapGap(
        const char* szParsedShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsOneLayer(szParsedShpFilePath, szXmlConfigPath,
            szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, false, true, false);
            return v;
        }

        return runOneLayerCheckAndExport(
            QString::fromUtf8(szParsedShpFilePath),
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, const QString& src) {
                return dlg->checkPolygonOverlapGap(pl, src);
            },
            false, false, true, false
                );
    }

    // 函数：checkLineTopology
    MY_PLUGIN_API ReadShp_error checkLineTopology(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayers(szParsedShpFilePath, szReferenceShpFilePath, szXmlConfigPath,
            szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                true, false, false, false);
            return v;
        }

        return runTwoLayerCheckAndExport(
            QString::fromUtf8(szParsedShpFilePath),
            QString::fromUtf8(szReferenceShpFilePath),
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                const QString layerName = pl ? pl->name() : QString();
                if ((pl && dlg->isRoadFile(layerName)) || dlg->isRoadFile(src)) {
                    return dlg->checkRoadLineTopology(pl, rl, src);
                }
                if (dlg->isRiverFile(src)) {
                    std::vector<QualityCheckResult> topo = dlg->checkRiverLineTopology(pl, rl, src);
                    std::vector<QualityCheckResult> connectivity = dlg->checkRiverNetworkConnectivity(pl, rl, src);
                    topo.insert(topo.end(), connectivity.begin(), connectivity.end());
                    return topo;
                }
                return std::vector<QualityCheckResult>{};
            },
            true, false, false, false
                 );
    }

    MY_PLUGIN_API ReadShp_error checkLineTopologyWithStandardLayer(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szStandardLayerCsvPath,
        const char* szStandardLayerName,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        if (!szParsedShpFilePath || !szReferenceShpFilePath) return READSHP_INVALID_PARAM;
        QStringList parsedPaths, referencePaths;
        for (const QString& path : QString::fromUtf8(szParsedShpFilePath).split(';', Qt::SkipEmptyParts))
            if (!path.trimmed().isEmpty()) parsedPaths.append(path.trimmed());
        for (const QString& path : QString::fromUtf8(szReferenceShpFilePath).split(';', Qt::SkipEmptyParts))
            if (!path.trimmed().isEmpty()) referencePaths.append(path.trimmed());
        std::vector<QByteArray> parsedBytes, referenceBytes;
        for (const QString& path : parsedPaths) parsedBytes.push_back(path.trimmed().toUtf8());
        for (const QString& path : referencePaths) referenceBytes.push_back(path.trimmed().toUtf8());
        std::vector<const char*> parsedPointers, referencePointers;
        for (const auto& path : parsedBytes) parsedPointers.push_back(path.constData());
        for (const auto& path : referenceBytes) referencePointers.push_back(path.constData());
        ReadShp_error v = validatePathsTwoLayersBatch(
            parsedPointers.data(), static_cast<int>(parsedPointers.size()),
            referencePointers.data(), static_cast<int>(referencePointers.size()), szXmlConfigPath,
            szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                true, false, false, false);
            return v;
        }

        if (!szStandardLayerCsvPath || !szStandardLayerName) {
            return READSHP_INVALID_PARAM;
        }

        const QString csvPath = QString::fromUtf8(szStandardLayerCsvPath).trimmed();
        const QString standardLayerName = QString::fromUtf8(szStandardLayerName).trimmed();
        if (csvPath.isEmpty() || standardLayerName.isEmpty() || !QFileInfo::exists(csvPath)) {
            return READSHP_INVALID_PARAM;
        }

        return runTwoLayerCheckAndExportBatch(
            parsedPaths,
            referencePaths,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                const QString layerName = pl ? pl->name() : QString();
                if ((pl && dlg->isRoadFile(layerName)) || dlg->isRoadFile(src)) {
                    return dlg->checkRoadLineTopology(pl, rl, src);
                }
                if (dlg->isRiverFile(src)) {
                    std::vector<QualityCheckResult> topo = dlg->checkRiverLineTopology(pl, rl, src);
                    std::vector<QualityCheckResult> connectivity = dlg->checkRiverNetworkConnectivity(pl, rl, src);
                    topo.insert(topo.end(), connectivity.begin(), connectivity.end());
                    return topo;
                }
                return std::vector<QualityCheckResult>{};
            },
            true, false, false, false,
            [csvPath, standardLayerName](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl) {
                if (!dlg->loadStandardLayersFromCsv(csvPath)) {
                    return false;
                }

                const auto it = std::find_if(dlg->m_standardLayers.cbegin(), dlg->m_standardLayers.cend(),
                    [&standardLayerName](const ReadShpDialog::StandardLayerEntry& entry) {
                        return entry.standardLayerName.trimmed() == standardLayerName;
                    });
                if (it == dlg->m_standardLayers.cend()) {
                    return false;
                }

                // 仅将 CSV 的标准图层语义附加到当前检查图层；不调用
                // applyStandardLayerEntry，因此不会额外加载图层或触碰地图画布。
                if (pl) pl->setCustomProperty(QStringLiteral("quality/standard_layer_name"), it->standardLayerName);
                if (rl) rl->setCustomProperty(QStringLiteral("quality/standard_layer_name"), it->standardLayerName);
                // Different rule tasks can share the same parsed filename. Their exports must not overwrite.
                dlg->setProperty("quality/output_standard_layer", it->standardLayerName);
                return true;
            });
    }

    // 函数：checkWaterSystemDirection
    MY_PLUGIN_API ReadShp_error checkWaterSystemDirection(
        const char* szParsedShpFilePath,
        const char* szReferenceShpFilePath,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayers(szParsedShpFilePath, szReferenceShpFilePath, szXmlConfigPath,
            szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, true, false, false);
            return v;
        }

        return runTwoLayerCheckAndExport(
            QString::fromUtf8(szParsedShpFilePath),
            QString::fromUtf8(szReferenceShpFilePath),
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                return dlg->checkWaterSystemDirection(pl, rl, src);
            },
            false, true, false, false
                );
    }
    MY_PLUGIN_API ReadShp_error checkLineTopologyBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayersBatch(szParsedShpFilePaths, parsedCount,
            szReferenceShpFilePaths, referenceCount,
            szXmlConfigPath, szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                true, false, false, false);
            return v;
        }

        QStringList parsedPaths, referencePaths;
        for (int i = 0; i < parsedCount; ++i) parsedPaths.append(QString::fromUtf8(szParsedShpFilePaths[i]));
        for (int j = 0; j < referenceCount; ++j) referencePaths.append(QString::fromUtf8(szReferenceShpFilePaths[j]));

        return runTwoLayerCheckAndExportBatch(
            parsedPaths, referencePaths,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                const QString layerName = pl ? pl->name() : QString();
                if ((pl && dlg->isRoadFile(layerName)) || dlg->isRoadFile(src)) {
                    return dlg->checkRoadLineTopology(pl, rl, src);
                }
                if (dlg->isRiverFile(src)) {
                    std::vector<QualityCheckResult> topo = dlg->checkRiverLineTopology(pl, rl, src);
                    std::vector<QualityCheckResult> connectivity = dlg->checkRiverNetworkConnectivity(pl, rl, src);
                    topo.insert(topo.end(), connectivity.begin(), connectivity.end());
                    return topo;
                }
                return std::vector<QualityCheckResult>{};
            },
            true, false, false, false
                );
    }
    MY_PLUGIN_API ReadShp_error checkWaterSystemDirectionBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayersBatch(szParsedShpFilePaths, parsedCount,
            szReferenceShpFilePaths, referenceCount,
            szXmlConfigPath, szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, true, false, false);
            return v;
        }

        QStringList parsedPaths, referencePaths;
        for (int i = 0; i < parsedCount; ++i) parsedPaths.append(QString::fromUtf8(szParsedShpFilePaths[i]));
        for (int j = 0; j < referenceCount; ++j) referencePaths.append(QString::fromUtf8(szReferenceShpFilePaths[j]));

        return runTwoLayerCheckAndExportBatch(
            parsedPaths, referencePaths,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                return dlg->checkWaterSystemDirection(pl, rl, src);
            },
            false, true, false, false
                );
    }
    MY_PLUGIN_API ReadShp_error checkPolygonOverlapGapBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsOneLayerBatch(szParsedShpFilePaths, parsedCount,
            szXmlConfigPath, szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, false, true, false);
            return v;
        }

        QStringList parsedPaths;
        for (int i = 0; i < parsedCount; ++i) parsedPaths.append(QString::fromUtf8(szParsedShpFilePaths[i]));

        return runOneLayerCheckAndExportBatch(
            parsedPaths,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, const QString& src) {
                return dlg->checkPolygonOverlapGap(pl, src);
            },
            false, false, true, false
                );
    }
    MY_PLUGIN_API ReadShp_error checkRightAngleThresholdBatch(
        const char** szParsedShpFilePaths, int parsedCount,
        const char** szReferenceShpFilePaths, int referenceCount,
        const char* szXmlConfigPath,
        const char* szOutputProblemShpFilePath,
        const char* szOutputJsonFilePath,
        const char* szOutputLogFolderPath,
        int scale)
    {
        ReadShp_error v = validatePathsTwoLayersBatch(szParsedShpFilePaths, parsedCount,
            szReferenceShpFilePaths, referenceCount,
            szXmlConfigPath, szOutputProblemShpFilePath, szOutputJsonFilePath, szOutputLogFolderPath);
        if (v != READSHP_SUCCESS) {
            exportStatusOnValidationFailure(QString::fromUtf8(szOutputJsonFilePath),
                QString::fromUtf8(szOutputLogFolderPath),
                false, false, false, true);
            return v;
        }

        QStringList parsedPaths, referencePaths;
        for (int i = 0; i < parsedCount; ++i) parsedPaths.append(QString::fromUtf8(szParsedShpFilePaths[i]));
        for (int j = 0; j < referenceCount; ++j) referencePaths.append(QString::fromUtf8(szReferenceShpFilePaths[j]));

        return runTwoLayerCheckAndExportBatch(
            parsedPaths, referencePaths,
            QString::fromUtf8(szXmlConfigPath),
            QString::fromUtf8(szOutputProblemShpFilePath),
            QString::fromUtf8(szOutputJsonFilePath),
            QString::fromUtf8(szOutputLogFolderPath),
            scale,
            [](ReadShpDialog* dlg, QgsVectorLayer* pl, QgsVectorLayer* rl, const QString& src) {
                return dlg->checkRightAngleThreshold(pl, rl, src);
            },
            false, false, false, true
                );
    }

} // extern "C"
