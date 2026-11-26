#include "se_annotation_content_check_interface.h"
#include "../algorithm/se_annotation_advanced_algorithm.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <QString>
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QFile>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <qgsmessagelog.h>
#include <qgsvectorlayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsFields.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsPointXY.h>
#include <QgsWkbTypes.h>
#include <QgsCoordinateReferenceSystem.h>

// ======================================================================
// 以下函数从210（符号注记匹配）原样复制，确保集成时不依赖210文件
// ======================================================================

// -------------------------------------------------------
// 从 se_annotation_check_interface.cpp 复制
// -------------------------------------------------------
static std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName)
{
    try
    {
#ifdef _WIN32
        QString originalPath = QString::fromUtf8(szOutputLogFilePath);
        QString logPath = originalPath;
        std::string nativePath = logPath.toLocal8Bit().toStdString();
#else
        QString originalPath = QString::fromUtf8(szOutputLogFilePath);
        QString logPath = originalPath;
        if (logPath.endsWith(".log", Qt::CaseInsensitive))
        {
            logPath.chop(4);
            logPath += ".txt";
        }
        std::string nativePath = logPath.toStdString();
#endif
        QString uniqueLoggerName = QString("%1_%2")
                                       .arg(loggerName)
                                       .arg(QDateTime::currentMSecsSinceEpoch());

        if (auto existing = spdlog::get(uniqueLoggerName.toStdString()))
        {
            spdlog::drop(uniqueLoggerName.toStdString());
        }

        QFileInfo fi(logPath);
        QDir dir = fi.dir();
        if (!dir.exists())
        {
            dir.mkpath(".");
        }

        // 预创建文件并写入 BOM（避免后续 truncate 覆盖）
        {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
                f.write(reinterpret_cast<const char *>(bom), 3);
                f.close();
            }
        }

#ifdef _WIN32
        spdlog::file_event_handlers handlers;
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(nativePath, false, handlers);
        auto logger = std::make_shared<spdlog::logger>(uniqueLoggerName.toStdString(), sink);
        spdlog::register_logger(logger);
#else
        auto logger = spdlog::basic_logger_mt(uniqueLoggerName.toStdString(), nativePath);
#endif

        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");

        logger->info("日志文件已创建 (UTF-8+BOM): {}", logPath.toUtf8().constData());

        return logger;
    }
    catch (const spdlog::spdlog_ex &ex)
    {
        QgsMessageLog::logMessage(QString("创建日志记录器失败: %1").arg(ex.what()),
                                  "AnnotationCheck", Qgis::Critical);
        return nullptr;
    }
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：spatialCluster
// -------------------------------------------------------
static QList<int> spatialCluster(const QList<AnnotationCheckResult> &items, double thresholdMeters = 3000.0)
{
    const int n = items.size();
    QList<int> parent;
    parent.reserve(n);
    for (int i = 0; i < n; ++i)
        parent.append(i);

    // 查找根
    auto find = [&parent](int x) -> int {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]];  // 路径压缩
            x = parent[x];
        }
        return x;
    };

    auto unite = [&find, &parent](int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb)
            parent[rb] = ra;
    };

    const double thresh2 = thresholdMeters * thresholdMeters;
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            double dx = items[i].position.x() - items[j].position.x();
            double dy = items[i].position.y() - items[j].position.y();
            if (dx * dx + dy * dy <= thresh2)
            {
                unite(i, j);
            }
        }
    }

    return parent;
}

// 213 的错误结果以符号 ID 标识，而交叉校验以 200 匹配结果中的 anno_fid 为主键。
// 在生成 JSON 时同时写出 anno_fid，避免两套 ID 无法直接比较。
static QMap<QString, QString> buildFeatureIdToAnnoFidMap(QgsVectorLayer *matchResultLayer)
{
    QMap<QString, QString> result;
    if (!matchResultLayer || !matchResultLayer->isValid())
        return result;

    const QgsFields fields = matchResultLayer->fields();
    const int annoFidIdx = fields.indexOf("anno_fid");
    if (annoFidIdx < 0)
        return result;

    const int symbolIdIdx = fields.indexOf("symbol_id");
    const int shortSymbolIdIdx = fields.indexOf("symbol_short_id");
    QgsFeatureIterator it = matchResultLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat))
    {
        const QString annoFid = feat.attribute(annoFidIdx).toString();
        if (annoFid.isEmpty())
            continue;

        const QString symbolId = symbolIdIdx >= 0 ? feat.attribute(symbolIdIdx).toString() : QString();
        const QString shortSymbolId = shortSymbolIdIdx >= 0 ? feat.attribute(shortSymbolIdIdx).toString() : QString();
        if (!symbolId.isEmpty())
            result.insert(symbolId, annoFid);
        if (!shortSymbolId.isEmpty())
            result.insert(shortSymbolId, annoFid);
    }
    return result;
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：generateGroupedJsonReport
// -------------------------------------------------------
static QString generateGroupedJsonReport(
    const QList<AnnotationCheckResult> &results,
    const QMap<QString, QString> &statistics,
    const QString &checkId,
    const QString &checkName,
    const QMap<QString, QString> &featureIdToAnnoFid,
    QgsVectorLayer *matchResultLayer,
    std::shared_ptr<spdlog::logger> logger)
{
    try
    {
        QJsonObject root;

        // 基本信息
        root["id"] = checkId;
        root["name"] = checkName;
        root["status"] = "质检完成";
        root["err_exist"] = !results.isEmpty();

        // 间距错误是“一个注记—一个符号”的独立关系，不能参加名称拼字或空间聚类。
        auto isSpacingError = [](const AnnotationCheckResult &r) -> bool {
            return r.expectedValue.startsWith(QString::fromUtf8("≥")) ||
                   r.expectedValue.startsWith(QString::fromUtf8("≤"));
        };

        // 为间距错误补充注记文本和关联符号 ID。坐标直接使用结果中的注记坐标。
        QMap<QString, QString> annotationTextByFid;
        QMap<QString, QString> symbolIdByAnnoFid;
        if (matchResultLayer && matchResultLayer->isValid())
        {
            const QgsFields fields = matchResultLayer->fields();
            const int annoFidIdx = fields.indexOf(QStringLiteral("anno_fid"));
            const int textIdx = fields.indexOf(QStringLiteral("text_conte"));
            const int symbolIdIdx = fields.indexOf(QStringLiteral("symbol_id"));
            if (annoFidIdx >= 0)
            {
                QgsFeatureIterator it = matchResultLayer->getFeatures();
                QgsFeature feat;
                while (it.nextFeature(feat))
                {
                    const QString annoFid = feat.attribute(annoFidIdx).toString().trimmed();
                    if (annoFid.isEmpty())
                        continue;
                    if (textIdx >= 0)
                        annotationTextByFid[annoFid] = feat.attribute(textIdx).toString();
                    if (symbolIdIdx >= 0)
                        symbolIdByAnnoFid[annoFid] = feat.attribute(symbolIdIdx).toString();
                }
            }
        }

        // 按 expected_value 分组（去除 UNMATCHED_FID 后缀，同名源合并）
        QMap<QString, QList<AnnotationCheckResult>> sourceGroups;
        QList<AnnotationCheckResult> otherErrors;
        QList<AnnotationCheckResult> spacingErrors;

        auto stripKey = [](const QString &name) -> QString {
            int idx = name.indexOf(" [UNMATCHED_FID:");
            return (idx >= 0) ? name.left(idx) : name;
        };

        for (const AnnotationCheckResult &r : results)
        {
            if (isSpacingError(r))
            {
                spacingErrors.append(r);
                continue;
            }
            if (!r.expectedValue.isEmpty())
                sourceGroups[stripKey(r.expectedValue)].append(r);
            else
                otherErrors.append(r);
        }

        // 空间聚类结构
        struct SpatialGroup {
            QString expVal;
            QList<AnnotationCheckResult> items;
            double centroidX;
            double centroidY;
        };
        QList<SpatialGroup> allSpatialGroups;

        for (auto sit = sourceGroups.constBegin(); sit != sourceGroups.constEnd(); ++sit)
        {
            const QString &expVal = sit.key();
            const QList<AnnotationCheckResult> &items = sit.value();

            QList<int> parent = spatialCluster(items, 3000.0);

            QMap<int, QList<int>> clusters;
            for (int i = 0; i < parent.size(); ++i)
            {
                int root = parent[i];
                while (parent[root] != root)
                    root = parent[root];
                clusters[root].append(i);
            }

            for (auto cit = clusters.constBegin(); cit != clusters.constEnd(); ++cit)
            {
                const QList<int> &indices = cit.value();
                SpatialGroup sg;
                sg.expVal = expVal;
                double sumX = 0, sumY = 0;
                for (int idx : indices)
                {
                    sg.items.append(items[idx]);
                    sumX += items[idx].position.x();
                    sumY += items[idx].position.y();
                }
                sg.centroidX = sumX / indices.size();
                sg.centroidY = sumY / indices.size();
                allSpatialGroups.append(sg);
            }
        }

        // 排序
        std::sort(allSpatialGroups.begin(), allSpatialGroups.end(),
            [](const SpatialGroup &a, const SpatialGroup &b) {
                if (a.expVal != b.expVal)
                    return a.expVal < b.expVal;
                if (std::abs(a.centroidX - b.centroidX) > 1.0)
                    return a.centroidX < b.centroidX;
                return a.centroidY < b.centroidY;
            });

        int totalSpatialGroups = allSpatialGroups.size();

        // ---------- 辅助函数：去除UNMATCHED唯一后缀，恢复可读名称 ----------
        auto stripUnmatchedSuffix = [](const QString &name) -> QString {
            int idx = name.indexOf(" [UNMATCHED_FID:");
            if (idx >= 0)
                return name.left(idx);
            return name;
        };

        // ---------- 辅助函数：判断错误类型 ----------
        auto classifyError = [](const QString &expected, const QStringList &chars) -> QString {
            bool allNoAnnotation = !chars.isEmpty();
            for (const QString &c : chars) {
                if (c != QString::fromUtf8("无注记")) { allNoAnnotation = false; break; }
            }
            if (allNoAnnotation)
                return "missing_annotation";
            QStringList sortedChars;
            for (const QString &s : chars) {
                for (const QChar &c : s)
                    sortedChars.append(QString(c));
            }
            sortedChars.sort();
            QStringList expectedChars;
            for (const QChar &c : expected)
                expectedChars.append(QString(c));
            expectedChars.sort();
            if (sortedChars.join("") == expectedChars.join(""))
                return "order_mismatch";
            return "content_mismatch";
        };

        // ---------- 构造 summary ----------
        int contentErrGroups = 0, orderErrGroups = 0, missingAnnoGroups = 0, missingSymbolGroups = 0;
        for (const SpatialGroup &sg : allSpatialGroups)
        {
            QStringList chars;
            for (const AnnotationCheckResult &r : sg.items)
                chars.append(r.actualValue);
            QString errType = classifyError(sg.expVal, chars);
            bool isAnnoMissingSymbol = false;
            for (const AnnotationCheckResult &r : sg.items)
            {
                if (r.errorMessage.contains(QString::fromUtf8("注记缺失符号")))
                {
                    isAnnoMissingSymbol = true;
                    break;
                }
            }
            if (isAnnoMissingSymbol)
                ++missingSymbolGroups;
            else if (errType == "order_mismatch")
                ++orderErrGroups;
            else if (errType == "missing_annotation")
                ++missingAnnoGroups;
            else
                ++contentErrGroups;
        }

        QJsonObject summaryObj;
        summaryObj["total_items"] = results.size();
        summaryObj["total_groups"] = totalSpatialGroups + spacingErrors.size() +
                                     (otherErrors.isEmpty() ? 0 : otherErrors.size());
        summaryObj["content_error_groups"] = contentErrGroups;
        summaryObj["order_error_groups"] = orderErrGroups;
        if (missingAnnoGroups > 0)
            summaryObj["missing_annotation_groups"] = missingAnnoGroups;
        if (missingSymbolGroups > 0)
            summaryObj["annotation_missing_symbol_groups"] = missingSymbolGroups;
        if (!spacingErrors.isEmpty())
            summaryObj["spacing_violation_items"] = spacingErrors.size();
        if (!otherErrors.isEmpty())
            summaryObj["other_errors"] = otherErrors.size();
        root["summary"] = summaryObj;

        // ---------- 构造 info ----------
        QString infoText;
        if (results.isEmpty())
        {
            infoText = QString("未发现%1问题").arg(checkName);
        }
        else
        {
            int totalGroups = summaryObj["total_groups"].toInt();
            QStringList parts;
            if (contentErrGroups > 0)
                parts.append(QString("内容不匹配%1组").arg(contentErrGroups));
            if (orderErrGroups > 0)
                parts.append(QString("顺序不匹配%1组").arg(orderErrGroups));
            if (missingAnnoGroups > 0)
                parts.append(QString("缺失注记%1组").arg(missingAnnoGroups));
            if (missingSymbolGroups > 0)
                parts.append(QString("注记缺失符号%1组").arg(missingSymbolGroups));
            if (!spacingErrors.isEmpty())
                parts.append(QString("间距错误%1条").arg(spacingErrors.size()));
            if (!parts.isEmpty())
                infoText = QString("发现%1个%2问题（聚类后%3组：%4）")
                    .arg(results.size()).arg(checkName).arg(totalGroups).arg(parts.join("、"));
            else
                infoText = QString("发现%1个%2问题（空间聚类后分组为%3组）")
                    .arg(results.size()).arg(checkName).arg(totalGroups);
        }

        if (!statistics.isEmpty())
        {
            infoText += "\n统计信息：";
            for (auto it = statistics.constBegin(); it != statistics.constEnd(); ++it)
                infoText += QString("\n- %1: %2").arg(it.key(), it.value());
        }
        root["info"] = infoText;

        // ---------- 构造 results 数组 ----------
        if (!results.isEmpty())
        {
            QJsonArray resultsArray;

            // 间距错误一条注记输出一条记录，便于直接定位；不使用内容检查的
            // matched_text/items/cluster_index 等聚类字段。
            for (const AnnotationCheckResult &r : spacingErrors)
            {
                const QString annotationFid = r.featureId;
                const QString annotationText = annotationTextByFid.value(annotationFid);
                const QString spacingKind = r.expectedValue.startsWith(QString::fromUtf8("≤"))
                    ? QString::fromUtf8("间距过大")
                    : QString::fromUtf8("间距过小");
                QJsonObject resultObj;
                resultObj["error_type"] = QStringLiteral("spacing_violation");
                resultObj["description"] = QString::fromUtf8(
                    "【%1】注记'%2'（FID=%3）与对应符号的间距为%4，要求%5")
                    .arg(spacingKind, annotationText, annotationFid,
                         r.actualValue, r.expectedValue);
                resultObj["annotation_fid"] = annotationFid;
                resultObj["annotation_text"] = annotationText;
                resultObj["annotation_x"] = r.position.x();
                resultObj["annotation_y"] = r.position.y();
                resultObj["symbol_id"] = symbolIdByAnnoFid.value(annotationFid);
                resultObj["expected_distance"] = r.expectedValue;
                resultObj["actual_distance"] = r.actualValue;
                resultsArray.append(resultObj);
            }

            QMap<QString, int> expValClusterCounter;

            for (int gi = 0; gi < totalSpatialGroups; ++gi)
            {
                const SpatialGroup &sg = allSpatialGroups[gi];
                const QList<AnnotationCheckResult> &groupItems = sg.items;

                int clusterSeq = expValClusterCounter[sg.expVal] + 1;
                expValClusterCounter[sg.expVal] = clusterSeq;

                int totalClustersForExpVal = 0;
                for (int gj = 0; gj < totalSpatialGroups; ++gj)
                    if (allSpatialGroups[gj].expVal == sg.expVal)
                        ++totalClustersForExpVal;

                QString displayName = stripUnmatchedSuffix(sg.expVal);

                QStringList chars;
                for (const AnnotationCheckResult &r : groupItems)
                    chars.append(r.actualValue);
                QString matchedText = chars.join("");
                QString errType = classifyError(displayName, chars);

                bool isAnnotationMissingSymbol = false;
                for (const AnnotationCheckResult &r : groupItems)
                {
                    if (r.errorMessage.contains(QString::fromUtf8("注记缺失符号")))
                    {
                        isAnnotationMissingSymbol = true;
                        break;
                    }
                }
                if (isAnnotationMissingSymbol)
                    errType = "annotation_missing_symbol";

                QJsonObject resultObj;
                resultObj["source_name"] = displayName;
                resultObj["matched_text"] = matchedText;
                resultObj["expected_text"] = displayName;
                resultObj["error_type"] = errType;
                resultObj["item_count"] = groupItems.size();
                resultObj["centroid_x"] = sg.centroidX;
                resultObj["centroid_y"] = sg.centroidY;

                if (totalClustersForExpVal > 1)
                {
                    resultObj["cluster_index"] = clusterSeq;
                    resultObj["total_clusters"] = totalClustersForExpVal;
                }

                QSet<QString> uniqueSymbolIds;
                for (const AnnotationCheckResult &r : groupItems)
                {
                    if (!r.featureId.isEmpty())
                        uniqueSymbolIds.insert(r.featureId);
                }
                if (!uniqueSymbolIds.isEmpty())
                {
                    QJsonArray symIdsArr;
                    for (const QString &sid : uniqueSymbolIds)
                        symIdsArr.append(sid);
                    resultObj["symbol_ids"] = symIdsArr;
                }

                // 描述：包含更多诊断信息
                if (errType == "missing_annotation")
                {
                    QStringList symIdShortList;
                    for (const QString &sid : uniqueSymbolIds)
                        symIdShortList.append(sid.section('_', -1));
                    QString symInfo = symIdShortList.join(", ");
                    resultObj["description"] = QString("【缺失注记】GDB要素'%1'（符号ID:%2）名称不为空，但没有任何注记标注")
                        .arg(displayName, symInfo);
                }
                else if (errType == "annotation_missing_symbol")
                {
                    resultObj["description"] = QString("【注记缺失符号】注记拼出'%1'，与源名称'%2'相关联，但该注记没有对应的地图符号")
                        .arg(matchedText, displayName);
                }
                else if (errType == "order_mismatch")
                {
                    int totalChars = displayName.length();
                    bool allCharsFound = (groupItems.size() >= totalChars);
                    if (allCharsFound)
                    {
                        resultObj["description"] = QString("【顺序不匹配】注记按空间排列拼出'%1'，与源名称'%2'字符相同但顺序不一致（全部%3字已找到，位置需调整）")
                            .arg(matchedText, displayName).arg(totalChars);
                    }
                    else
                    {
                        resultObj["description"] = QString("【顺序不匹配】注记拼出'%1'，与源名称'%2'字符相同但顺序不一致")
                            .arg(matchedText, displayName);
                    }
                }
                else
                {
                    QSet<QString> charSet;
                    for (const QString &c : chars) {
                        for (const QChar &ch : c)
                            charSet.insert(QString(ch));
                    }
                    QStringList missingChars;
                    for (const QChar &c : displayName)
                    {
                        QString cs(c);
                        if (!charSet.contains(cs))
                            missingChars.append(cs);
                    }
                    if (totalClustersForExpVal > 1)
                    {
                        resultObj["description"] = QString("【内容不匹配】注记拼出'%1'，与源名称'%2'不匹配%3（位置组%4/%5）")
                            .arg(matchedText, displayName)
                            .arg(missingChars.isEmpty() ? "" : QString("，缺少'%1'").arg(missingChars.join("")))
                            .arg(clusterSeq).arg(totalClustersForExpVal);
                    }
                    else
                    {
                        resultObj["description"] = QString("【内容不匹配】注记拼出'%1'，与源名称'%2'不匹配%3")
                            .arg(matchedText, displayName)
                            .arg(missingChars.isEmpty() ? "" : QString("，缺少'%1'").arg(missingChars.join("")));
                    }
                }

                QJsonArray itemsArray;
                for (const AnnotationCheckResult &r : groupItems)
                {
                    QJsonObject item;
                    item["character"] = r.actualValue;
                    item["feature_id"] = r.featureId;
                    const QString annoFid = featureIdToAnnoFid.value(r.featureId);
                    if (!annoFid.isEmpty())
                        item["anno_fid"] = annoFid;
                    if (!r.featureId.isEmpty())
                        item["symbol_short_id"] = r.featureId.section('_', -1);
                    item["position_x"] = r.position.x();
                    item["position_y"] = r.position.y();
                    itemsArray.append(item);
                }
                resultObj["items"] = itemsArray;

                resultsArray.append(resultObj);
            }

            for (const AnnotationCheckResult &r : otherErrors)
            {
                QJsonObject resultObj;
                resultObj["error_type"] = "other";
                resultObj["description"] = r.errorMessage;
                resultObj["feature_id"] = r.featureId;
                const QString annoFid = featureIdToAnnoFid.value(r.featureId);
                if (!annoFid.isEmpty())
                    resultObj["anno_fid"] = annoFid;
                resultObj["position_x"] = r.position.x();
                resultObj["position_y"] = r.position.y();
                if (!r.actualValue.isEmpty())
                    resultObj["actual_value"] = r.actualValue;
                if (!r.expectedValue.isEmpty())
                    resultObj["expected_value"] = r.expectedValue;
                resultsArray.append(resultObj);
            }

            root["results"] = resultsArray;
        }

        QJsonDocument doc(root);
        QString jsonString = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));

        logger->info("成功生成JSON报告（内容错误聚类{}组，间距错误{}条），大小: {} 字节",
            totalSpatialGroups, spacingErrors.size(), jsonString.toUtf8().size());
        return jsonString;
    }
    catch (const std::exception &e)
    {
        logger->error("生成分组JSON报告时发生异常: {}", e.what());
        return QString("{}");
    }
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：writeJsonReportToFile
// -------------------------------------------------------
static SE_Error writeJsonReportToFile(const QString &jsonString, const char *szOutputJsonPath, std::shared_ptr<spdlog::logger> logger)
{
    QFile file(QString::fromUtf8(szOutputJsonPath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        logger->error("无法创建JSON报告文件: {}", szOutputJsonPath);
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    stream << jsonString;
    file.close();

    logger->info("JSON报告已写入: {}", szOutputJsonPath);
    return SE_ERROR_NONE;
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：writeSymbolMissingAnnoLayer
// -------------------------------------------------------
static SE_Error writeSymbolMissingAnnoLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *actualLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (!actualLayer)
    {
        logger->error("写入无符号图层时：实际图层为空");
        return SE_ERROR_INVALID_PARAMETER;
    }

    QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
    QFileInfo fi(outputPath);
    // 无论主输出是 .shp 还是 .json，这里统一在 SHP 目录下输出附属图层
    QString symbolMissingPath = fi.absolutePath() + "/" + fi.completeBaseName() + "_symbol_missing.shp";

    // 输出字段来自 actualLayer，但可能包含无关的关联列，只需基础ID即可
    QgsFields outFields;
    outFields.append(QgsField("symbol_id", QVariant::String));
    QgsCoordinateReferenceSystem crs = actualLayer->crs();
    QgsWkbTypes::Type geomType = actualLayer->wkbType();

    std::unique_ptr<QgsVectorFileWriter> writer(
        new QgsVectorFileWriter(symbolMissingPath,
                                "UTF-8", outFields, geomType, crs, "ESRI Shapefile"));

    if (writer->hasError() != QgsVectorFileWriter::NoError)
    {
        logger->error("无法创建无符号附属图层文件: {} error={}",
                      symbolMissingPath.toStdString(), writer->errorMessage().toStdString());
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 收集需要导出的实际要素ID（去重）
    QSet<QString> exportFeatureIds;
    for (const auto &r : results)
    {
        // 找出符号缺失相关的注记 -> 需要导出对应的实际符号要素
        if (r.errorType == AnnotationErrorType::FontError &&
            r.errorMessage.contains(QString::fromUtf8("注记缺失符号")))
        {
            exportFeatureIds.insert(r.featureId);
        }
    }

    if (exportFeatureIds.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    // 从 actualLayer 中获取需导出要素的几何
    // 用字段名查找 symbol_id，不要假设首个字段
    QgsFields actFields = actualLayer->fields();
    int actSymIdIdx = actFields.indexOf("symbol_id");
    if (actSymIdIdx < 0) actSymIdIdx = actFields.indexOf("symbol_short_id");
    if (actSymIdIdx < 0) actSymIdIdx = 0;
    QgsFeatureIterator it = actualLayer->getFeatures();
    QgsFeature feat;
    int writtenCount = 0;
    while (it.nextFeature(feat))
    {
        QString featureId = feat.attribute(actSymIdIdx).toString();
        if (!exportFeatureIds.contains(featureId))
            continue;

        // 输出要素：使用原始几何，仅保留符号ID属性
        QgsFeature outFeat(outFields);
        outFeat.setGeometry(feat.geometry());
        outFeat.setAttribute(0, featureId);
        writer->addFeature(outFeat);
        writtenCount++;

        // 已找到所有则提前退出
        exportFeatureIds.remove(featureId);
        if (exportFeatureIds.isEmpty())
            break;
    }

    logger->info("无符号附属图层已写入 {} 条记录（输出路径: {}）", writtenCount, symbolMissingPath.toStdString());
    return SE_ERROR_NONE;
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：writeGdbMissingAnnoLayer
// -------------------------------------------------------
static SE_Error writeGdbMissingAnnoLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *gdbLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (!gdbLayer)
    {
        logger->error("写入无注记图层时：GDB图层为空");
        return SE_ERROR_INVALID_PARAMETER;
    }

    QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
    QFileInfo fi(outputPath);
    QString gdbMissingPath = fi.absolutePath() + "/" + fi.completeBaseName() + "_gdb_missing.shp";

    // 输出字段：使用GDB图层的字段结构
    QgsFields outFields = gdbLayer->fields();
    QgsCoordinateReferenceSystem crs = gdbLayer->crs();
    QgsWkbTypes::Type geomType = gdbLayer->wkbType();

    std::unique_ptr<QgsVectorFileWriter> writer(
        new QgsVectorFileWriter(gdbMissingPath,
                                "UTF-8", outFields, geomType, crs, "ESRI Shapefile"));

    if (writer->hasError() != QgsVectorFileWriter::NoError)
    {
        logger->error("无法创建GDB缺失注记附属图层文件: {} error={}",
                      gdbMissingPath.toStdString(), writer->errorMessage().toStdString());
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 收集需要导出的GDB要素FID
    QSet<int> exportFids;
    for (const auto &r : results)
    {
        // 缺失注记：GDB面/线要素上有名称，但没有注记
        if (r.errorType == AnnotationErrorType::FontError &&
            r.errorMessage.contains(QString::fromUtf8("缺失注记")))
        {
            int gdbFid = -1;
            // 从注释中解析GDB FID
            // errorMessage 格式: "src_fid:xxx ..."
            int posGdbFid = r.errorMessage.indexOf("src_fid:");
            if (posGdbFid >= 0)
            {
                posGdbFid += 8; // 跳过 "src_fid:"
                QString fidStr;
                while (posGdbFid < r.errorMessage.length() && r.errorMessage[posGdbFid].isDigit())
                {
                    fidStr += r.errorMessage[posGdbFid];
                    posGdbFid++;
                }
                if (!fidStr.isEmpty())
                    gdbFid = fidStr.toInt();
            }
            if (gdbFid >= 0)
                exportFids.insert(gdbFid);
        }
    }

    if (exportFids.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    // 从 gdbLayer 中获取需导出要素并写入
    QgsFeature feat;
    int writtenCount = 0;
    for (int gdbFid : exportFids)
    {
        if (gdbLayer->getFeatures(QgsFeatureRequest().setFilterFid(gdbFid)).nextFeature(feat))
        {
            writer->addFeature(feat);
            writtenCount++;
        }
        else
        {
            logger->warn("无法获取GDB FID={}的要素（无注记附属图层）", gdbFid);
        }
    }

    logger->info("GDB缺失注记附属图层已写入 {} 条记录（输出路径: {}）", writtenCount, gdbMissingPath.toStdString());
    return SE_ERROR_NONE;
}

// -------------------------------------------------------
// 从 se_annotation_advanced_check_interface.cpp 复制：writeSymbolAnnotationMatchResultLayer
// -------------------------------------------------------
static SE_Error writeSymbolAnnotationMatchResultLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *actualLayer,
    QgsVectorLayer *gdbLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (!matchResultLayer)
    {
        logger->error("写入结果图层时：输入图层为空");
        return SE_ERROR_INVALID_PARAMETER;
    }

    QgsFields inFields = matchResultLayer->fields();
    // 追加 error_type / error_msg / expected_value / actual_value 列
    QgsFields outFields = inFields;
    outFields.append(QgsField("error_type", QVariant::String));
    outFields.append(QgsField("error_msg",  QVariant::String));
    outFields.append(QgsField("exp_val",    QVariant::String));
    outFields.append(QgsField("act_val",    QVariant::String));

    int errorTypeIdx = outFields.indexOf("error_type");
    int errorMsgIdx  = outFields.indexOf("error_msg");
    int expValIdx    = outFields.indexOf("exp_val");
    int actValIdx    = outFields.indexOf("act_val");

    QgsCoordinateReferenceSystem crs = matchResultLayer->crs();
    QgsWkbTypes::Type geomType = matchResultLayer->wkbType();

    std::unique_ptr<QgsVectorFileWriter> writer(
        new QgsVectorFileWriter(QString::fromUtf8(szOutputCheckResultLayerPath),
                                "UTF-8", outFields, geomType, crs, "ESRI Shapefile"));

    if (writer->hasError() != QgsVectorFileWriter::NoError)
    {
        logger->error("无法创建输出图层文件: {} error={}",
                      szOutputCheckResultLayerPath, writer->errorMessage().toStdString());
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 预处理：收集可用于定位的全部 ID 并映射到 matchResultLayer 的 FID。
    // 算法对“内容/间距”错误使用 anno_fid，对“缺失注记”使用 symbol_id；
    // 因此不能只用 symbol_id 建索引，否则 JSON 有错误而 SHP 无要素。
    QMap<QString, QgsFeatureId> resultIdToFid;
    {
        QgsFields mFields = matchResultLayer->fields();
        QList<int> idFieldIndexes;
        const QStringList idFieldNames = {
            QStringLiteral("symbol_id"),
            QStringLiteral("symbol_short_id"),
            // DBF 字段名最长 10 个字符，部分历史输出会截断为该名称。
            QStringLiteral("symbol_shor"),
            QStringLiteral("anno_fid")
        };
        for (const QString &fieldName : idFieldNames)
        {
            const int fieldIndex = mFields.indexOf(fieldName);
            if (fieldIndex >= 0 && !idFieldIndexes.contains(fieldIndex))
                idFieldIndexes.append(fieldIndex);
        }
        QgsFeatureIterator idIt = matchResultLayer->getFeatures();
        QgsFeature idFeat;
        while (idIt.nextFeature(idFeat))
        {
            for (const int fieldIndex : idFieldIndexes)
            {
                const QString idValue = idFeat.attribute(fieldIndex).toString().trimmed();
                if (idValue.isEmpty())
                    continue;

                resultIdToFid[idValue] = idFeat.id();

                // 兼容完整 symbol_id（如 "xxx_107"）与其短 ID（"107"）混用。
                const QString shortId = idValue.section('_', -1);
                if (!shortId.isEmpty())
                    resultIdToFid[shortId] = idFeat.id();
            }
        }
    }

    // 去重：同一symbol_id可能同时有内容错误和缺失符号错误，各保留一个
    QSet<QString> writtenSymbolIds;
    int writtenCount = 0;

    for (const auto &r : results)
    {
        // 跳过缺失注记错误（在GDB附属图层输出）
        if (r.errorType == AnnotationErrorType::FontError &&
            r.errorMessage.contains(QString::fromUtf8("缺失注记")))
            continue;

        QString symId = r.featureId;
        if (symId.isEmpty())
            continue;

        if (writtenSymbolIds.contains(symId))
            continue;
        writtenSymbolIds.insert(symId);

        if (!resultIdToFid.contains(symId))
        {
            logger->warn("结果要素未回写：无法在200匹配图层中定位 ID '{}'", symId.toStdString());
            continue;
        }
        const QgsFeatureId matchFid = resultIdToFid.value(symId);

        QgsFeature feat;
        if (!matchResultLayer->getFeatures(QgsFeatureRequest().setFilterFid(matchFid)).nextFeature(feat))
            continue;

        QgsFeature outFeat(outFields);
        outFeat.setGeometry(feat.geometry());
        for (int i = 0; i < inFields.size(); ++i)
            outFeat.setAttribute(i, feat.attribute(i));

        // 判断是内容错误、间距错误还是注记缺失符号。
        if (r.errorMessage.contains(QString::fromUtf8("注记缺失符号")))
            outFeat.setAttribute(errorTypeIdx, QString("annotation_missing_symbol"));
        else if (r.expectedValue.startsWith(QString::fromUtf8("≥")) ||
                 r.expectedValue.startsWith(QString::fromUtf8("≤")))
            outFeat.setAttribute(errorTypeIdx, QString("spacing_violation"));
        else
            outFeat.setAttribute(errorTypeIdx, QString("content_mismatch"));
        outFeat.setAttribute(errorMsgIdx, r.errorMessage);
        outFeat.setAttribute(expValIdx, r.expectedValue);
        outFeat.setAttribute(actValIdx, r.actualValue);

        writer->addFeature(outFeat);
        writtenCount++;
    }

    logger->info("主结果图层已写入 {} 条记录，输出路径: {}", writtenCount, szOutputCheckResultLayerPath);

    // 写入无符号附属图层
    if (actualLayer)
    {
        writeSymbolMissingAnnoLayer(results, actualLayer,
                                    szOutputCheckResultLayerPath, logger);
    }

    // 写入无注记附属图层
    if (gdbLayer)
    {
        writeGdbMissingAnnoLayer(results, gdbLayer,
                                 szOutputCheckResultLayerPath, logger);
    }

    return SE_ERROR_NONE;
}

// -------------------------------------------------------
// 213专用：基于QFile的简易日志器
// -------------------------------------------------------
struct SimpleLogger
{
    QFile file;
    QTextStream stream;

    bool open(const QString &path)
    {
        file.setFileName(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        stream.setDevice(&file);
        stream.setCodec("UTF-8");
        return true;
    }

    void info(const QString &msg)
    {
        QString ts = QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss] ");
        stream << ts << msg << "\n";
        stream.flush();
    }

    void error(const QString &msg)
    {
        QString ts = QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss] ");
        stream << ts << "ERROR " << msg << "\n";
        stream.flush();
    }

    void debug(const QString &msg)
    {
        QString ts = QDateTime::currentDateTime().toString("[yyyy-MM-dd hh:mm:ss] ");
        stream << ts << "DEBUG " << msg << "\n";
        stream.flush();
    }

    ~SimpleLogger()
    {
        if (file.isOpen())
            file.close();
    }
};

// -------------------------------------------------------
// 辅助：判断一个结果条目是否应该过滤掉（无符号/无注记）
// -------------------------------------------------------
static bool shouldFilterOut(const QString &errorType, const QString &matchStat, const QString &expVal)
{
    Q_UNUSED(matchStat);
    Q_UNUSED(expVal);
    if (errorType == "MISSING_SYMBOL")           return true;
    if (errorType == "MISSING_ANNOTATION")       return true;
    return false;
}

// -------------------------------------------------------
// 辅助：重写单组的description（计算实际匹配字数）
// -------------------------------------------------------
static void rewriteGroupDescription(QJsonObject &group)
{
    QString matchedText = group["matched_text"].toString();
    QString expectedText = group["expected_text"].toString();
    int expectedLen = expectedText.length();
    QString errorType = group["error_type"].toString();

    int matchCount = 0;
    for (int ci = 0; ci < expectedLen; ++ci)
    {
        if (matchedText.contains(expectedText.at(ci)))
            matchCount++;
    }

    QString newDesc;
    if (errorType == "order_mismatch")
    {
        newDesc = QString::fromUtf8(
            "注记文字'%1'疑似顺序错乱：与标准名称'%2'对比，"
            "文字相同但排列顺序不一致")
            .arg(matchedText, expectedText);
    }
    else if (errorType == "content_mismatch")
    {
        if (matchCount == expectedLen)
        {
            newDesc = QString::fromUtf8(
                "注记文字'%1'疑似多字：与标准名称'%2'对比，"
                "标准名称%3字全部出现，但注记中可能包含多余字符")
                .arg(matchedText, expectedText)
                .arg(expectedLen);
        }
        else
        {
            newDesc = QString::fromUtf8(
                "注记文字'%1'疑似错字：与标准名称'%2'对比，"
                "仅能匹配%3/%4字，字符可能遗漏或错误")
                .arg(matchedText, expectedText)
                .arg(matchCount).arg(expectedLen);
        }
    }
    else
    {
        newDesc = QString::fromUtf8(
            "注记文字'%1'无法匹配标准名称'%2'，文字差异较大")
            .arg(matchedText, expectedText);
    }
    group["description"] = newDesc;
}

// -------------------------------------------------------
// 辅助：过滤JSON结果（含同源名称空间合并）
// -------------------------------------------------------
static QJsonObject filterJsonResults(const QJsonObject &srcJson,
                                      SimpleLogger &logger)
{
    QJsonObject dstJson = srcJson;

    QJsonArray srcResults = srcJson["results"].toArray();
    QJsonArray rawResults;
    QJsonArray directSpacingResults;
    int filteredCount = 0;

    for (int i = 0; i < srcResults.size(); ++i)
    {
        QJsonObject group = srcResults[i].toObject();

        QString errorType = group["error_type"].toString();

        const QString expText = group["expected_text"].toString();
        const bool isSpacingViolation = errorType == "spacing_violation" ||
                                        expText.startsWith(QString::fromUtf8("≤")) ||
                                        expText.startsWith(QString::fromUtf8("≥"));

        // 间距错误必须逐条保留，不能按 source_name 或空间距离再聚合。
        if (isSpacingViolation)
        {
            directSpacingResults.append(group);
            continue;
        }

        // 213 保留名称内容/顺序错误；其余类型仍按原规则排除。
        if (errorType != "content_mismatch" && errorType != "order_mismatch")
        {
            filteredCount++;
            QString desc = group["description"].toString();
            logger.info(QString::fromUtf8("[213] JSON过滤: 排除 [%1] %2")
                            .arg(errorType, desc.left(60)));
            continue;
        }

        rawResults.append(group);
    }

    // 按 source_name 分组，再按空间距离合并
    QMap<QString, QVector<QJsonObject>> sourceGroups;
    for (int i = 0; i < rawResults.size(); ++i)
    {
        QJsonObject g = rawResults[i].toObject();
        sourceGroups[g["source_name"].toString()].append(g);
    }

    QJsonArray mergedResults = directSpacingResults;
    const double GAP_THRESHOLD = 5000.0;

    for (auto it = sourceGroups.begin(); it != sourceGroups.end(); ++it)
    {
        QVector<QJsonObject> &groups = it.value();
        QString sourceName = it.key();

        if (groups.size() == 1)
        {
            QJsonObject g = groups[0];
            if (g["error_type"].toString() != "spacing_violation")
                rewriteGroupDescription(g);
            mergedResults.append(g);
            continue;
        }

        std::sort(groups.begin(), groups.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a["centroid_y"].toDouble() < b["centroid_y"].toDouble();
        });

        QVector<QVector<QJsonObject>> subClusters;
        QVector<QJsonObject> cur;

        for (int j = 0; j < groups.size(); ++j)
        {
            if (cur.isEmpty())
            {
                cur.append(groups[j]);
            }
            else
            {
                double prevY = cur.last()["centroid_y"].toDouble();
                double thisY = groups[j]["centroid_y"].toDouble();
                if (std::abs(thisY - prevY) > GAP_THRESHOLD)
                {
                    subClusters.append(cur);
                    cur.clear();
                }
                cur.append(groups[j]);
            }
        }
        if (!cur.isEmpty())
            subClusters.append(cur);

        for (int c = 0; c < subClusters.size(); ++c)
        {
            QVector<QJsonObject> &sub = subClusters[c];

            if (sub.size() == 1)
            {
                QJsonObject g = sub[0];
                if (g["error_type"].toString() != "spacing_violation")
                    rewriteGroupDescription(g);
                mergedResults.append(g);
                continue;
            }

            QJsonObject merged = sub[0];
            QJsonArray allItems = merged["items"].toArray();
            QStringList matchedChars;
            matchedChars << merged["matched_text"].toString();
            QJsonArray allSymbolIds = merged["symbol_ids"].toArray();
            double sumX = merged["centroid_x"].toDouble();
            double sumY = merged["centroid_y"].toDouble();

            for (int k = 1; k < sub.size(); ++k)
            {
                QJsonObject &sg = sub[k];
                QJsonArray items = sg["items"].toArray();
                for (int m = 0; m < items.size(); ++m)
                    allItems.append(items[m]);

                matchedChars << sg["matched_text"].toString();

                QJsonArray sids = sg["symbol_ids"].toArray();
                for (int m = 0; m < sids.size(); ++m)
                    allSymbolIds.append(sids[m]);

                sumX += sg["centroid_x"].toDouble();
                sumY += sg["centroid_y"].toDouble();
            }

            QString mergedMatched = matchedChars.join("");

            merged["items"] = allItems;
            merged["matched_text"] = mergedMatched;
            merged["symbol_ids"] = allSymbolIds;
            merged["item_count"] = allItems.size();
            merged["centroid_x"] = sumX / sub.size();
            merged["centroid_y"] = sumY / sub.size();

            QString expectedText = merged["expected_text"].toString();
            int expectedLen = expectedText.length();
            int matchCount = 0;
            for (int ci = 0; ci < expectedLen; ++ci)
            {
                if (mergedMatched.contains(expectedText.at(ci)))
                    matchCount++;
            }

            QString errorType = merged["error_type"].toString();
            if (errorType == "spacing_violation")
            {
                // 保留第一条原始间距描述；items 中仍完整保留本聚类的全部错误项。
                mergedResults.append(merged);
                continue;
            }
            QString newDesc;
            if (errorType == "order_mismatch")
            {
                newDesc = QString::fromUtf8(
                    "注记拼出'%1'疑似顺序错乱：与标准名称'%2'对比，"
                    "文字相同但排列顺序不一致")
                    .arg(mergedMatched, expectedText);
            }
            else if (matchCount == expectedLen)
            {
                newDesc = QString::fromUtf8(
                    "注记拼出'%1'疑似多字：与标准名称'%2'对比，"
                    "标准名称%3字全部出现，但注记中可能包含多余字符")
                    .arg(mergedMatched, expectedText)
                    .arg(expectedLen);
            }
            else
            {
                newDesc = QString::fromUtf8(
                    "注记拼出'%1'疑似错字：与标准名称'%2'对比，"
                    "仅能匹配%3/%4字，字符可能遗漏或错误")
                    .arg(mergedMatched, expectedText)
                    .arg(matchCount).arg(expectedLen);
            }
            merged["description"] = newDesc;

            mergedResults.append(merged);
        }
    }

    dstJson["results"] = mergedResults;

    QJsonObject summary;
    int totalItems = 0;
    int spacingItemCount = 0;
    for (int i = 0; i < mergedResults.size(); ++i)
    {
        const QJsonObject item = mergedResults[i].toObject();
        if (item["error_type"].toString() == "spacing_violation")
        {
            // 新格式一条记录代表一个间距错误；兼容旧格式时仍采用 item_count。
            const int itemCount = item["item_count"].toInt(1);
            totalItems += itemCount;
            spacingItemCount += itemCount;
        }
        else
        {
            totalItems += item["item_count"].toInt();
        }
    }
    summary["total_items"] = totalItems;
    summary["total_groups"] = mergedResults.size();
    dstJson["summary"] = summary;

    dstJson["id"] = "213";
    dstJson["name"] = QString::fromUtf8("注记内容合规性");

    int originalGroups = rawResults.size() + directSpacingResults.size();
    int mergedGroupCount = mergedResults.size();
    QString mergeInfo;
    if (mergedGroupCount < originalGroups)
    {
        mergeInfo = QString::fromUtf8("（同源名称空间合并：%1组→%2组）")
                        .arg(originalGroups).arg(mergedGroupCount);
    }

    QString info = QString::fromUtf8(
        "注记内容及间距检查完成：共发现%1条问题（%2个结果组，其中间距问题%3条）。"
        "%4已自动排除%5个无效情况（缺注记/无符号等）。")
        .arg(totalItems).arg(mergedGroupCount).arg(spacingItemCount)
        .arg(mergeInfo).arg(filteredCount);
    QJsonObject infoObject;
    infoObject["description"] = info;
    dstJson["info"] = infoObject;

    logger.info(QString("[213] JSON过滤完成: 保留%1组/%2条(内容/顺序/间距错误, 合并前%3组), 过滤%4组")
                    .arg(mergedGroupCount).arg(totalItems).arg(originalGroups).arg(filteredCount));
    return dstJson;
}

// -------------------------------------------------------
// 辅助：过滤SHP
// -------------------------------------------------------
static SE_Error filterShpLayer(const QString &srcShpPath,
                                const QString &dstShpPath,
                                SimpleLogger &logger)
{
    if (!QFile::exists(srcShpPath))
    {
        logger.info(QString("[213] temp SHP不存在，无需过滤: %1").arg(srcShpPath));
        return SE_ERROR_NONE;
    }

    QgsVectorLayer *srcLayer = new QgsVectorLayer(srcShpPath, "temp", "ogr");
    if (!srcLayer || !srcLayer->isValid())
    {
        logger.error(QString("[213] 无法打开temp SHP: %1").arg(srcShpPath));
        delete srcLayer;
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QgsFields srcFields = srcLayer->fields();
    QgsCoordinateReferenceSystem srcCrs = srcLayer->crs();
    QgsWkbTypes::Type geomType = srcLayer->wkbType();

    std::unique_ptr<QgsVectorFileWriter> writer(
        new QgsVectorFileWriter(dstShpPath, "UTF-8", srcFields, geomType, srcCrs, "ESRI Shapefile"));

    if (writer->hasError() != QgsVectorFileWriter::NoError)
    {
        logger.error(QString("[213] 无法创建输出SHP: %1 error=%2")
                         .arg(dstShpPath, writer->errorMessage()));
        delete srcLayer;
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    int keptCount = 0;
    int filteredCount = 0;

    QgsFeatureIterator it = srcLayer->getFeatures();
    QgsFeature feat;
    while (it.nextFeature(feat))
    {
        QString errorType = feat.attribute("error_type").toString();
        QString matchStat = feat.attribute("match_stat").toString();
        QString expVal = feat.attribute("expected_v").toString();

        if (shouldFilterOut(errorType, matchStat, expVal))
        {
            filteredCount++;
            continue;
        }

        writer->addFeature(feat);
        keptCount++;
    }

    delete srcLayer;

    logger.info(QString("[213] SHP过滤完成: 保留%1条, 过滤%2条(无符号/无注记)").arg(keptCount).arg(filteredCount));
    return SE_ERROR_NONE;
}

// -------------------------------------------------------
// 辅助：构建temp路径
// -------------------------------------------------------
static void buildTempPaths(const QString &outputShpPath,
                           const QString &outputJsonPath,
                           QString &tempShpPath,
                           QString &tempJsonPath)
{
    QFileInfo shpInfo(outputShpPath);
    QString tempDir = QDir::tempPath() + "/mapcheck_213";
    QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    QDir().mkpath(tempDir);
    tempShpPath = QDir(tempDir).filePath(
        shpInfo.completeBaseName() + "_" + ts + "_tmp.shp");
    tempJsonPath = QDir(tempDir).filePath(
        QFileInfo(outputJsonPath).completeBaseName() + "_" + ts + "_tmp.json");
}

// -------------------------------------------------------
// 辅助：清理temp文件
// -------------------------------------------------------
static void cleanTempFiles(const QString &shpPath, const QString &jsonPath)
{
    QStringList shpExts = { ".shp", ".shx", ".dbf", ".prj", ".cpg", ".qpj" };
    QFileInfo fi(shpPath);
    QString base = fi.absolutePath() + "/" + fi.completeBaseName();

    for (const QString &ext : shpExts) {
        QString f = base + ext;
        if (QFile::exists(f)) QFile::remove(f);
    }
    if (QFile::exists(jsonPath))
        QFile::remove(jsonPath);

    QString gdbBase = base + "_gdb_missing";
    for (const QString &ext : shpExts) {
        QString f = gdbBase + ext;
        if (QFile::exists(f)) QFile::remove(f);
    }

    QString tmpLog = shpPath + ".log";
    if (QFile::exists(tmpLog))
        QFile::remove(tmpLog);
}

// ======================================================================
// 主接口：AnnotationContentCheck
// 以200匹配结果为核心输入，内部实现散字拼词+回扫+过滤
// ======================================================================
SE_Error AnnotationContentCheck(
    const char *sz200MatchShp,
    const char *szActualShp,
    const char *szGdbLayer,
    const char *szAnnotLayer,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputShpPath,
    const char *szOutputJsonPath,
    const char *szOutputLogPath)
{
    // ---------- 参数校验 ----------
    if (!sz200MatchShp || !sz200MatchShp[0])
        return SE_ERROR_INVALID_PARAMETER;
    if (!szSchemaXmlPath || !szSchemaXmlPath[0])
        return SE_ERROR_INVALID_PARAMETER;
    if (!szOutputShpPath || !szOutputShpPath[0])
        return SE_ERROR_INVALID_PARAMETER;
    if (!szOutputJsonPath || !szOutputJsonPath[0])
        return SE_ERROR_INVALID_PARAMETER;
    if (!szOutputLogPath || !szOutputLogPath[0])
        return SE_ERROR_INVALID_PARAMETER;

    // ---------- 日志初始化 ----------
    SimpleLogger logger;
    QString logPath = QString::fromUtf8(szOutputLogPath);
    QDir().mkpath(QFileInfo(logPath).absolutePath());
    if (!logger.open(logPath))
        return SE_ERROR_FAILURE;

    logger.info("==========================================");
    logger.info(QString::fromUtf8("[213] 注记内容合规性检查开始"));
    logger.info(QString::fromUtf8("  200匹配结果: %1").arg(QString::fromUtf8(sz200MatchShp)));
    logger.info(QString::fromUtf8("  实际要素图层: %1").arg(
        szActualShp && szActualShp[0] ? QString::fromUtf8(szActualShp) : QString::fromUtf8("(空，回扫跳过)")));
    logger.info(QString::fromUtf8("  GDB图层: %1").arg(
        szGdbLayer && szGdbLayer[0] ? QString::fromUtf8(szGdbLayer) : QString::fromUtf8("(空，回扫跳过)")));
    logger.info(QString::fromUtf8("  注记图层: %1").arg(
        szAnnotLayer && szAnnotLayer[0] ? QString::fromUtf8(szAnnotLayer) : QString::fromUtf8("(空，回扫跳过)")));
    logger.info(QString::fromUtf8("  规则XML: %1").arg(
        szSchemaXmlPath && szSchemaXmlPath[0] ? QString::fromUtf8(szSchemaXmlPath) : QString::fromUtf8("(空)")));
    logger.info(QString::fromUtf8("  比例尺: %1").arg(scale));
    logger.info(QString::fromUtf8("  输出SHP: %1").arg(QString::fromUtf8(szOutputShpPath)));
    logger.info(QString::fromUtf8("  输出JSON: %1").arg(QString::fromUtf8(szOutputJsonPath)));

    // ---------- 构建temp路径 ----------
    QString outputShp = QString::fromUtf8(szOutputShpPath);
    QString outputJson = QString::fromUtf8(szOutputJsonPath);
    QString tempShpPath, tempJsonPath;
    buildTempPaths(outputShp, outputJson, tempShpPath, tempJsonPath);

    logger.info(QString::fromUtf8("  temp SHP: %1").arg(tempShpPath));
    logger.info(QString::fromUtf8("  temp JSON: %1").arg(tempJsonPath));

    QDir().mkpath(QFileInfo(tempShpPath).absolutePath());
    cleanTempFiles(tempShpPath, tempJsonPath);

    // ---------- 步骤1：散字拼词+回扫（从210原样复制的逻辑）→ temp ----------
    logger.info(QString::fromUtf8("[213] 步骤1：散字拼词+回扫..."));

    QString tempLogPath = tempShpPath + ".log";

    // 创建spdlog日志（给拼词/回扫引擎用）
    QByteArray tempLogBytes = tempLogPath.toUtf8();
    auto spdLogger = createLogger(tempLogBytes.constData(), "AnnotationContentCheck");
    if (!spdLogger)
    {
        logger.error(QString::fromUtf8("[213] 无法创建拼词引擎日志"));
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    spdLogger->info("开始执行注记内容合规性检查（ID=213）——散字拼词+回扫阶段");
    spdLogger->info("匹配结果图层: {}", sz200MatchShp);
    spdLogger->info("实际要素图层: {}", szActualShp ? szActualShp : "");
    spdLogger->info("GDB图层: {}", szGdbLayer ? szGdbLayer : "");
    spdLogger->info("原始注记图层: {}", szAnnotLayer ? szAnnotLayer : "");
    spdLogger->info("质检方案: {}", szSchemaXmlPath ? szSchemaXmlPath : "");
    spdLogger->info("地图比例尺: {}", scale);

    SE_Error err = SE_ERROR_NONE;

    try
    {
        // 1.1 加载图层
        QgsVectorLayer *matchResultLayer = new QgsVectorLayer(
            QString::fromUtf8(sz200MatchShp), "match_result", "ogr");

        QgsVectorLayer *actualLayer = nullptr;
        QgsVectorLayer *gdbLayer = nullptr;
        QgsVectorLayer *annoPolygonLayer = nullptr;

        if (szActualShp && strlen(szActualShp) > 0)
        {
            actualLayer = new QgsVectorLayer(
                QString::fromUtf8(szActualShp), "actual", "ogr");
            if (!actualLayer->isValid())
            {
                spdLogger->error("无法加载实际要素图层: {}", szActualShp);
                delete matchResultLayer;
                delete actualLayer;
                delete gdbLayer;
                delete annoPolygonLayer;
                spdlog::drop(spdLogger->name());
                logger.error(QString::fromUtf8("[213] 步骤1失败：无法加载实际要素图层"));
                return SE_ERROR_CORRUPT_DATA;
            }
        }

        if (szGdbLayer && strlen(szGdbLayer) > 0)
        {
            gdbLayer = new QgsVectorLayer(
                QString::fromUtf8(szGdbLayer), "gdb", "ogr");
            if (!gdbLayer->isValid())
            {
                spdLogger->error("无法加载GDB图层: {}", szGdbLayer);
                delete matchResultLayer;
                delete actualLayer;
                delete gdbLayer;
                delete annoPolygonLayer;
                spdlog::drop(spdLogger->name());
                logger.error(QString::fromUtf8("[213] 步骤1失败：无法加载GDB图层"));
                return SE_ERROR_CORRUPT_DATA;
            }
        }

        if (szAnnotLayer && strlen(szAnnotLayer) > 0)
        {
            annoPolygonLayer = new QgsVectorLayer(
                QString::fromUtf8(szAnnotLayer), "anno_polygon", "ogr");
            if (!annoPolygonLayer->isValid())
            {
                spdLogger->warn("无法加载原始注记图层（将跳过孤儿注记检测）: {}", szAnnotLayer);
                delete annoPolygonLayer;
                annoPolygonLayer = nullptr;
            }
            else
            {
                spdLogger->info("成功加载原始注记图层，要素数量: {}", annoPolygonLayer->featureCount());
            }
        }
        else
        {
            spdLogger->info("未提供原始注记图层路径，跳过孤儿注记检测");
        }

        if (!matchResultLayer->isValid())
        {
            spdLogger->error("无法加载输入图层: {}", sz200MatchShp);
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(spdLogger->name());
            logger.error(QString::fromUtf8("[213] 步骤1失败：无法加载匹配结果图层"));
            return SE_ERROR_CORRUPT_DATA;
        }

        spdLogger->info("成功加载匹配结果图层，要素数量: {}", matchResultLayer->featureCount());

        // 1.2 创建算法实例并加载规则
        SE_AnnotationAdvancedAlgorithm algorithm(spdLogger);
        double mapScale = static_cast<double>(scale);

        if (!algorithm.loadSymbolAnnotationMatchRules(QString::fromUtf8(szSchemaXmlPath), mapScale, "213"))
        {
            spdLogger->error("加载符号注记匹配规则失败");
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(spdLogger->name());
            logger.error(QString::fromUtf8("[213] 步骤1失败：加载规则失败"));
            return SE_ERROR_CORRUPT_DATA;
        }

        // 1.3 执行散字拼词+回扫
        spdLogger->info("开始执行检查...");
        QString algorithmError;
        QList<AnnotationCheckResult> results = algorithm.checkSymbolAnnotationMatch(
            matchResultLayer, actualLayer, gdbLayer, &algorithmError, annoPolygonLayer);
        if (!algorithmError.isEmpty())
        {
            spdLogger->error("符号注记匹配检查失败: {}", algorithmError.toStdString());
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(spdLogger->name());
            logger.error(QString::fromUtf8("[213] 步骤1失败：算法执行错误: %1").arg(algorithmError));
            return SE_ERROR_CORRUPT_DATA;
        }
        spdLogger->info("检查完成，发现 {} 个问题", results.size());

        // 1.4 过滤孤立通用类别词（与210完全一致）
        {
            QMap<QString, QList<int>> baseSourceGroups;
            for (int i = 0; i < results.size(); ++i)
            {
                const auto &r = results[i];
                if (r.errorMessage.contains(QString::fromUtf8("【注记不匹配】")) ||
                    r.errorMessage.contains(QString::fromUtf8("【注记内容错误】")) ||
                    r.errorMessage.contains(QString::fromUtf8("【内容不匹配】")))
                {
                    QString baseName = r.expectedValue;
                    int fidTagIdx = baseName.indexOf(QString::fromUtf8(" [UNMATCHED_FID:"));
                    if (fidTagIdx >= 0)
                        baseName = baseName.left(fidTagIdx);
                    baseSourceGroups[baseName].append(i);
                }
            }
            QSet<int> toRemove;
            for (auto it = baseSourceGroups.constBegin(); it != baseSourceGroups.constEnd(); ++it)
            {
                const QString &baseName = it.key();
                const QList<int> &indices = it.value();

                bool hasRealName = false;
                for (int idx : indices)
                {
                    const auto &r = results[idx];
                    if (r.actualValue == baseName)
                    {
                        hasRealName = true;
                        break;
                    }
                }
                if (hasRealName)
                    continue;

                bool allGeneric = true;
                for (int idx : indices)
                {
                    const auto &r = results[idx];
                    if (r.actualValue == baseName)
                    {
                        hasRealName = true;
                        break;
                    }
                    allGeneric = false;
                    break;
                }
                if (hasRealName)
                    continue;
                if (allGeneric)
                {
                    for (int idx : indices)
                    {
                        toRemove.insert(idx);
                        spdLogger->info("过滤孤立通用类别词: actual='{}' source='{}'",
                                        results[idx].actualValue.toStdString(), baseName.toStdString());
                    }
                }
            }
            if (!toRemove.isEmpty())
            {
                QList<AnnotationCheckResult> filtered;
                filtered.reserve(results.size() - toRemove.size());
                for (int i = 0; i < results.size(); ++i)
                {
                    if (!toRemove.contains(i))
                        filtered.append(results[i]);
                }
                results.swap(filtered);
                spdLogger->info("过滤孤立通用类别词完成: 移除 {} 条, 剩余 {} 条",
                                toRemove.size(), results.size());
            }
        }

        // 1.5 生成JSON（写入temp）
        QMap<QString, QString> statistics;
        const QMap<QString, QString> featureIdToAnnoFid = buildFeatureIdToAnnoFidMap(matchResultLayer);
        QString jsonString = generateGroupedJsonReport(results, statistics, "213", "注记内容合规性",
                                                       featureIdToAnnoFid, matchResultLayer, spdLogger);
        writeJsonReportToFile(jsonString, tempJsonPath.toUtf8().constData(), spdLogger);

        // 1.6 写入temp SHP
        if (!results.isEmpty())
        {
            writeSymbolAnnotationMatchResultLayer(
                results, matchResultLayer, actualLayer, gdbLayer,
                tempShpPath.toUtf8().constData(), spdLogger);
        }

        // 清理图层
        delete matchResultLayer;
        delete actualLayer;
        delete gdbLayer;
        delete annoPolygonLayer;

        spdLogger->info("散字拼词+回扫阶段完成");
        spdlog::drop(spdLogger->name());

        logger.info(QString::fromUtf8("[213] 步骤1完成：散字拼词+回扫结束"));
    }
    catch (const std::exception &e)
    {
        spdLogger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(spdLogger->name());
        logger.error(QString::fromUtf8("[213] 步骤1异常: %1").arg(e.what()));
        cleanTempFiles(tempShpPath, tempJsonPath);
        return SE_ERROR_FAILURE;
    }

    // ---------- 步骤2：过滤SHP（temp → 最终输出）----------
    logger.info(QString::fromUtf8("[213] 步骤2：过滤SHP，移除无符号/无注记..."));
    SE_Error errFilter = filterShpLayer(tempShpPath, outputShp, logger);
    if (errFilter != SE_ERROR_NONE) {
        logger.error(QString::fromUtf8("[213] SHP过滤失败"));
        cleanTempFiles(tempShpPath, tempJsonPath);
        return errFilter;
    }

    // ---------- 步骤3：过滤JSON + 同源空间合并（temp → 最终输出）----------
    logger.info(QString::fromUtf8("[213] 步骤3：过滤JSON（含同源空间合并）..."));
    {
        QFile srcJsonFile(tempJsonPath);
        if (!srcJsonFile.open(QIODevice::ReadOnly)) {
            logger.error(QString::fromUtf8("[213] 无法打开temp JSON: %1").arg(tempJsonPath));
            cleanTempFiles(tempShpPath, tempJsonPath);
            return SE_ERROR_FILE_NOT_FOUND;
        }
        QJsonDocument srcDoc = QJsonDocument::fromJson(srcJsonFile.readAll());
        srcJsonFile.close();
        QJsonObject srcRoot = srcDoc.object();
        QJsonObject dstRoot = filterJsonResults(srcRoot, logger);

        QFile dstJsonFile(outputJson);
        if (!dstJsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            logger.error(QString::fromUtf8("[213] 无法写入输出JSON: %1").arg(outputJson));
            cleanTempFiles(tempShpPath, tempJsonPath);
            return SE_ERROR_WRITE_FILE_FAILED;
        }
        QJsonDocument dstDoc(dstRoot);
        dstJsonFile.write(dstDoc.toJson(QJsonDocument::Indented));
        dstJsonFile.close();
    }

    // ---------- 步骤4：清理temp ----------
    logger.info(QString::fromUtf8("[213] 步骤4：清理临时文件"));
    cleanTempFiles(tempShpPath, tempJsonPath);

    logger.info(QString::fromUtf8("[213] 注记内容合规性检查完成"));
    logger.info(QString::fromUtf8("  结果SHP: %1").arg(QString::fromUtf8(szOutputShpPath)));
    logger.info(QString::fromUtf8("  结果JSON: %1").arg(QString::fromUtf8(szOutputJsonPath)));

    return SE_ERROR_NONE;
}
