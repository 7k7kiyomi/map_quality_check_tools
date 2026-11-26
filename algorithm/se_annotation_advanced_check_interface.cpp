#include "se_annotation_advanced_check_interface.h"
#include "se_annotation_advanced_algorithm.h"
#include "se_symbol_direction_algorithm.h"
#include <QString>
#include <QStringList>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <qgsmessagelog.h>
#include <qgsvectorlayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsFields.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsPointXY.h>
#include <QgsWkbTypes.h>
#include <QSet>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue> // 新增 include
#include <QJsonArray>

static SE_Error writeSymbolDirectionProblemLayer(const QList<AnnotationCheckResult> &results, QgsVectorLayer *symbolLayer, const QString &symbolIdField, bool useSymbolIds, const char *szOutputCheckResultLayerPath, std::shared_ptr<spdlog::logger> logger);
static SE_Error writeJsonReportToFile(const QString &jsonString, const char *szOutputJsonPath, std::shared_ptr<spdlog::logger> logger);

//// 仅有通用类别词不能证明注记与具体地名相关，例如仅有的一个"沟"不能匹配"泉儿沟"。
//// 与200算法中isGenericCategoryOnlyPartial逻辑一致，用于210输出阶段过滤孤立通用词单字。
// static bool isGenericCategoryOnlyIn210(const QString &annoText, const QString &gdbText)
//{
//     if (annoText.isEmpty() || annoText == gdbText)
//         return false;
//
//     static const QSet<QString> genericSuffixes = {
//         "高速公路", "高速", "公路", "铁路", "道路", "大道", "街道", "路",
//         "河流", "河", "江", "湖", "海", "沟", "渠", "干渠", "支渠", "总干渠", "水库",
//         "山", "峰", "大桥", "桥", "隧道", "机场", "港", "站",
//         "省", "市", "县", "区", "乡", "镇", "村"};
//     return genericSuffixes.contains(annoText);
// }

/**
 * @brief 通用JSON报告生成函数 - 返回JSON字符串
 * @param results 检查结果列表
 * @param statistics 统计信息（可选）
 * @param checkId 检查项ID
 * @param checkName 检查项名称
 * @param logger 日志记录器
 * @return QString JSON字符串
 */
static QString generateJsonReport(
    const QList<AnnotationCheckResult> &results,
    const QMap<QString, QString> &statistics,
    const QString &checkId,
    const QString &checkName,
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

        // 构造 info 文本（包含统计信息）
        QString infoText;
        if (results.isEmpty())
        {
            infoText = QString("未发现%1问题").arg(checkName);
        }
        else
        {
            infoText = QString("发现%1个%2问题").arg(results.size()).arg(checkName);
        }

        // 将统计结果拼接到 info（仅当存在且组数>=2的统计已在上游筛选）
        if (!statistics.isEmpty())
        {
            infoText += "\n统计信息：";
            for (auto it = statistics.constBegin(); it != statistics.constEnd(); ++it)
            {
                // 逐行附加，例如："- 河流/道路[xxx]: 共N段,总计M组注记 (第1段有K组, ...)"
                infoText += QString("\n- %1: %2").arg(it.key(), it.value());
            }
        }
        root["info"] = infoText;

        // 错误信息部分（保持 check_item4 结构：errors 为对象，包含 error1,error2,...）
        if (!results.isEmpty())
        {
            QJsonObject errors;
            for (int i = 0; i < results.size(); ++i)
            {
                const AnnotationCheckResult &result = results[i];

                QJsonObject errorDetail;
                errorDetail["description"] = result.errorMessage;
                errorDetail["actual_value"] = result.actualValue;
                errorDetail["expected_value"] = result.expectedValue;
                errorDetail["feature_id"] = result.featureId;
                errorDetail["position_x"] = result.position.x();
                errorDetail["position_y"] = result.position.y();

                errors[QString("error%1").arg(i + 1)] = errorDetail;
            }
            root["errors"] = errors;
        }

        // 注意：不再输出 root["statistics"] 顶层键，严格遵循示例格式

        // 生成JSON字符串
        QJsonDocument doc(root);
        QString jsonString = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));

        logger->info("成功生成JSON报告，大小: {} 字节", jsonString.toUtf8().size());
        return jsonString;
    }
    catch (const std::exception &e)
    {
        logger->error("生成JSON报告时发生异常: {}", e.what());
        return QString("{}"); // 返回空JSON对象
    }
}

/**
 * @brief 空间聚类辅助：并查集，将距离<阈值的散字归入同一空间簇
 * @param items 同一 expected_value 下的散字列表
 * @param thresholdMeters 距离阈值（米），默认 3000m
 * @return QList<int> parent数组，parent[i]==i 的为簇根
 */
static QList<int> spatialCluster(const QList<AnnotationCheckResult> &items, double thresholdMeters = 3000.0)
{
    const int n = items.size();
    QList<int> parent;
    parent.reserve(n);
    for (int i = 0; i < n; ++i)
        parent.append(i);

    // 查找根
    auto find = [&parent](int x) -> int
    {
        while (parent[x] != x)
        {
            parent[x] = parent[parent[x]]; // 路径压缩
            x = parent[x];
        }
        return x;
    };

    auto unite = [&find, &parent](int a, int b)
    {
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

/**
 * @brief 分组JSON报告生成函数 - 按源名称（expected_value）将相同来源的错误分组输出
 *        并加入空间聚类：同一源名称但空间距离远的散字自动拆分为独立子组
 *        适用于单字注记匹配检查等场景，避免散字错误逐个输出，方便人工复核
 * @param results 检查结果列表
 * @param statistics 统计信息（可选）
 * @param checkId 检查项ID
 * @param checkName 检查项名称
 * @param logger 日志记录器
 * @return QString JSON字符串
 */
static QString generateGroupedJsonReport(
    const QList<AnnotationCheckResult> &results,
    const QMap<QString, QString> &statistics,
    const QString &checkId,
    const QString &checkName,
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

        // 按 expected_value 分组（去除 UNMATCHED_FID 后缀，同名源合并）
        QMap<QString, QList<AnnotationCheckResult>> sourceGroups;
        QList<AnnotationCheckResult> otherErrors;

        auto stripKey = [](const QString &name) -> QString
        {
            int idx = name.indexOf(" [UNMATCHED_FID:");
            return (idx >= 0) ? name.left(idx) : name;
        };

        for (const AnnotationCheckResult &r : results)
        {
            if (!r.expectedValue.isEmpty())
                sourceGroups[stripKey(r.expectedValue)].append(r);
            else
                otherErrors.append(r);
        }

        // 空间聚类结构
        struct SpatialGroup
        {
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
                  [](const SpatialGroup &a, const SpatialGroup &b)
                  {
                      if (a.expVal != b.expVal)
                          return a.expVal < b.expVal;
                      if (std::abs(a.centroidX - b.centroidX) > 1.0)
                          return a.centroidX < b.centroidX;
                      return a.centroidY < b.centroidY;
                  });

        int totalSpatialGroups = allSpatialGroups.size();

        // ---------- 辅助函数：去除UNMATCHED唯一后缀，恢复可读名称 ----------
        auto stripUnmatchedSuffix = [](const QString &name) -> QString
        {
            int idx = name.indexOf(" [UNMATCHED_FID:");
            if (idx >= 0)
                return name.left(idx);
            return name;
        };

        // ---------- 辅助函数：判断错误类型 ----------
        // 通过期望值及文字比对分类为 spacing_error / content_mismatch /
        // order_mismatch / missing_annotation。
        auto classifyError = [](const QString &expected, const QStringList &chars) -> QString
        {
            // 间距错误的期望值由引擎写为“≤...”或“≥...”。它不是文字内容错误，
            // 必须在文字拆分、排序比较之前单独归类。
            const QString normalizedExpected = expected.trimmed();
            if (normalizedExpected.startsWith(QString::fromUtf8("≤")) ||
                normalizedExpected.startsWith(QString::fromUtf8("≥")))
            {
                return "spacing_error";
            }

            // GDB线/面缺失注记：所有项 actualValue 全为 "无注记" 即判定为缺失注记
            // （不仅限1个，多个符号都没注记时也属于缺失注记，而非内容不匹配）
            bool allNoAnnotation = !chars.isEmpty();
            for (const QString &c : chars)
            {
                if (c != QString::fromUtf8("无注记"))
                {
                    allNoAnnotation = false;
                    break;
                }
            }
            if (allNoAnnotation)
                return "missing_annotation";
            // 按字符排序后比较，若内容相同则说明只是顺序问题
            // chars中可能包含多字整词（如"水墨丹霞"），需拆成单字再比
            QStringList sortedChars;
            for (const QString &s : chars)
            {
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
        int contentErrGroups = 0, orderErrGroups = 0, spacingErrGroups = 0,
            missingAnnoGroups = 0, missingSymbolGroups = 0;
        for (const SpatialGroup &sg : allSpatialGroups)
        {
            QStringList chars;
            for (const AnnotationCheckResult &r : sg.items)
                chars.append(r.actualValue);
            QString errType = classifyError(sg.expVal, chars);
            // 纠正注记缺失符号的类型
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
            else if (errType == "spacing_error")
                ++spacingErrGroups;
            else if (errType == "order_mismatch")
                ++orderErrGroups;
            else if (errType == "missing_annotation")
                ++missingAnnoGroups;
            else
                ++contentErrGroups;
        }

        QJsonObject summaryObj;
        summaryObj["total_items"] = results.size();
        summaryObj["total_groups"] = totalSpatialGroups + (otherErrors.isEmpty() ? 0 : otherErrors.size());
        summaryObj["content_error_groups"] = contentErrGroups;
        summaryObj["order_error_groups"] = orderErrGroups;
        if (spacingErrGroups > 0)
            summaryObj["spacing_error_groups"] = spacingErrGroups;
        if (missingAnnoGroups > 0)
            summaryObj["missing_annotation_groups"] = missingAnnoGroups;
        if (missingSymbolGroups > 0)
            summaryObj["annotation_missing_symbol_groups"] = missingSymbolGroups;
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
            if (spacingErrGroups > 0)
                parts.append(QString("间距错误%1组").arg(spacingErrGroups));
            if (missingAnnoGroups > 0)
                parts.append(QString("缺失注记%1组").arg(missingAnnoGroups));
            if (missingSymbolGroups > 0)
                parts.append(QString("注记缺失符号%1组").arg(missingSymbolGroups));
            if (!parts.isEmpty())
                infoText = QString("发现%1个%2问题（聚类后%3组：%4）")
                               .arg(results.size())
                               .arg(checkName)
                               .arg(totalGroups)
                               .arg(parts.join("、"));
            else
                infoText = QString("发现%1个%2问题（空间聚类后分组为%3组）")
                               .arg(results.size())
                               .arg(checkName)
                               .arg(totalGroups);
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

            QMap<QString, int> expValClusterCounter;

            for (int gi = 0; gi < totalSpatialGroups; ++gi)
            {
                const SpatialGroup &sg = allSpatialGroups[gi];
                const QList<AnnotationCheckResult> &groupItems = sg.items;

                int clusterSeq = expValClusterCounter[sg.expVal] + 1;
                expValClusterCounter[sg.expVal] = clusterSeq;

                // 计算该源名称的总簇数
                int totalClustersForExpVal = 0;
                for (int gj = 0; gj < totalSpatialGroups; ++gj)
                    if (allSpatialGroups[gj].expVal == sg.expVal)
                        ++totalClustersForExpVal;

                // 去除UNMATCHED后缀得到可读名称
                QString displayName = stripUnmatchedSuffix(sg.expVal);

                // 拼出实际匹配到的文本
                QStringList chars;
                for (const AnnotationCheckResult &r : groupItems)
                    chars.append(r.actualValue);
                QString matchedText = chars.join("");
                QString errType = classifyError(displayName, chars);

                // 纠正：检查原始errorMessage以识别"注记缺失符号"（符号缺少的检查项）
                // classifyError会将此类误判为content_mismatch
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

                // 收集本组所有唯一symbol_id（用于缺失注记/注记缺失符号的溯源）
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
                    // 构建符号标识信息：优先用GDB名称，附带symbol_id用于溯源
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
                else if (errType == "spacing_error")
                {
                    // 间距错误不重新拼接“内容不匹配”文案。直接复用底层结果的
                    // errorMessage，它与错误 SHP 对应要素写入的 error_msg 完全一致。
                    resultObj["description"] = groupItems.first().errorMessage;
                }
                else if (errType == "order_mismatch")
                {
                    // 判断全部字符是否已找到（面内路径直接收集或UNMATCHED合并）
                    int totalChars = displayName.length();
                    bool allCharsFound = (groupItems.size() >= totalChars);
                    if (allCharsFound)
                    {
                        resultObj["description"] = QString("【顺序不匹配】注记按空间排列拼出'%1'，与源名称'%2'字符相同但顺序不一致（全部%3字已找到，位置需调整）")
                                                       .arg(matchedText, displayName)
                                                       .arg(totalChars);
                    }
                    else
                    {
                        resultObj["description"] = QString("【顺序不匹配】注记拼出'%1'，与源名称'%2'字符相同但顺序不一致")
                                                       .arg(matchedText, displayName);
                    }
                }
                else
                {
                    // 内容不匹配：找出缺失的字符
                    QSet<QString> charSet;
                    for (const QString &c : chars)
                    {
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
                                                       .arg(clusterSeq)
                                                       .arg(totalClustersForExpVal);
                    }
                    else
                    {
                        resultObj["description"] = QString("【内容不匹配】注记拼出'%1'，与源名称'%2'不匹配%3")
                                                       .arg(matchedText, displayName)
                                                       .arg(missingChars.isEmpty() ? "" : QString("，缺少'%1'").arg(missingChars.join("")));
                    }
                }

                // 输出每个散字的信息（数组格式）
                QJsonArray itemsArray;
                for (const AnnotationCheckResult &r : groupItems)
                {
                    QJsonObject item;
                    item["character"] = r.actualValue;
                    item["feature_id"] = r.featureId;
                    // 简化的symbol_id短尾号，方便在大量结果中快速定位
                    if (!r.featureId.isEmpty())
                        item["symbol_short_id"] = r.featureId.section('_', -1);
                    item["position_x"] = r.position.x();
                    item["position_y"] = r.position.y();
                    itemsArray.append(item);
                }
                resultObj["items"] = itemsArray;

                resultsArray.append(resultObj);
            }

            // 输出非分组结果
            for (const AnnotationCheckResult &r : otherErrors)
            {
                QJsonObject resultObj;
                resultObj["error_type"] = "other";
                resultObj["description"] = r.errorMessage;
                resultObj["feature_id"] = r.featureId;
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

        logger->info("成功生成分组JSON报告（空间聚类后{}组），大小: {} 字节",
                     totalSpatialGroups, jsonString.toUtf8().size());
        return jsonString;
    }
    catch (const std::exception &e)
    {
        logger->error("生成分组JSON报告时发生异常: {}", e.what());
        return QString("{}");
    }
}

static SE_Error writeJsonReportToFile(const QString &jsonString, const char *szOutputJsonPath, std::shared_ptr<spdlog::logger> logger)
{
    try
    {
        QString path = QString::fromUtf8(szOutputJsonPath);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        {
            logger->error("无法打开JSON报告文件进行写入: {}", szOutputJsonPath);
            return SE_ERROR_WRITE_FILE_FAILED;
        }
        QTextStream out(&file);
        out.setCodec("UTF-8");
        out << jsonString;
        file.close();
        logger->info("成功将JSON报告写入文件: {}", szOutputJsonPath);
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("写入JSON报告文件时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

// 使用 extern 声明在其他 .cpp 文件中定义的函数
extern std::shared_ptr<spdlog::logger> createLogger(const char *szOutputLogFilePath, const QString &loggerName);
extern SE_Error writeCheckReport(const QList<AnnotationCheckResult> &results, const char *szOutputCheckReportPath, const QString &checkType);

// 新增辅助函数：写入包含问题要素的独立图层（完全保持原样）
static SE_Error writeProblemElementsLayer(
    const QSet<qint64> &elementIds,
    const QList<ElementInfo> &allElements,
    const QList<QgsVectorLayer *> &loadedLayers,
    const QString &outputPath,
    const QString &elementType,
    std::shared_ptr<spdlog::logger> logger)
{
    if (elementIds.isEmpty())
        return SE_ERROR_NONE;

    // 1. 去重集合 (layerPtr, fid)
    struct Key
    {
        const QgsVectorLayer *layer;
        qint64 fid;
        bool operator==(const Key &o) const { return layer == o.layer && fid == o.fid; }
    };
    struct KeyHash
    {
        std::size_t operator()(const Key &k) const noexcept
        {
            return std::hash<const void *>()(k.layer) ^ std::hash<qint64>()(k.fid);
        }
    };
    QHash<QString, QgsVectorLayer *> layerNameMap;
    for (auto *ly : loadedLayers)
        layerNameMap.insert(ly->name(), ly);

    QSet<QString> needLower = {
        "symbol_id", "symbol_nam", "symbol_pat", "total_symb", "text_conte"};

    // 2. 预收集所有要导出的 (layer, fid)
    QSet<QPair<const QgsVectorLayer *, qint64>> allFids;
    for (qint64 eid : elementIds)
    {
        const ElementInfo &elem = allElements[eid];
        auto it = layerNameMap.find(elem.layerName);
        if (it == layerNameMap.end())
            continue;
        QgsVectorLayer *layer = it.value();

        if (elem.elementType == "symbol")
        {
            // 添加组成要素
            for (const QVariant &fidVar : elem.componentFeatureIds)
                allFids.insert(qMakePair(static_cast<const QgsVectorLayer *>(layer),
                                         fidVar.toLongLong()));
        }
        else
        {
            allFids.insert(qMakePair(static_cast<const QgsVectorLayer *>(layer),
                                     elem.originalFeatureId.toLongLong()));
        }
    }
    if (allFids.isEmpty())
        return SE_ERROR_NONE;

    // 3. 选择一个模板图层（第一个）
    const QgsVectorLayer *templateLayer = allFids.constBegin()->first;
    QgsCoordinateReferenceSystem crs = templateLayer->crs();
    QgsWkbTypes::Type wkbType = templateLayer->wkbType();

    // 4. 构建输出字段：仅保留白名单 + 元信息
    QgsFields outFields;
    QVector<QString> copySrcNames; // 记录来源字段名（按名称获取属性，避免索引不一致）
    const QgsFields &srcFields = templateLayer->fields();
    for (int i = 0; i < srcFields.count(); ++i)
    {
        QString lname = srcFields.at(i).name().toLower();
        if (needLower.contains(lname))
        {
            outFields.append(srcFields.at(i));
            copySrcNames.append(srcFields.at(i).name());
        }
    }
    // 元信息字段
    outFields.append(QgsField("SRC_LAYER", QVariant::String, "String", 80));
    outFields.append(QgsField("SRC_FID", QVariant::LongLong));
    outFields.append(QgsField("ELEM_TYPE", QVariant::String, "String", 16));
    outFields.append(QgsField("ERROR_TYPE", QVariant::String, "String", 32));

    QgsVectorFileWriter writer(outputPath, "UTF-8", outFields, wkbType, crs, "ESRI Shapefile");
    if (writer.hasError() != QgsVectorFileWriter::NoError)
    {
        logger->error("创建问题要素图层失败: {} -> {}", outputPath.toStdString(),
                      writer.errorMessage().toStdString());
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 5. 分图层批量拉取，减少 getFeature 调用
    QHash<const QgsVectorLayer *, QList<qint64>> perLayerIds;
    for (auto it = allFids.constBegin(); it != allFids.constEnd(); ++it)
        perLayerIds[it->first].append(it->second);

    int added = 0;
    // 为加速判断，预构建每个符号 ElementInfo 的 component 集合映射（layerName -> QSet<fid>）
    QHash<QString, QSet<qint64>> symbolComponentMap; // key: layerName, value: set of fids belonging to any symbol in elementIds
    for (qint64 eid : elementIds)
    {
        const ElementInfo &e = allElements[eid];
        if (e.elementType == "symbol")
        {
            QSet<qint64> &s = symbolComponentMap[e.layerName];
            for (const QVariant &v : e.componentFeatureIds)
                s.insert(v.toLongLong());
        }
    }

    for (auto layerIt = perLayerIds.constBegin(); layerIt != perLayerIds.constEnd(); ++layerIt)
    {
        const QgsVectorLayer *layer = layerIt.key();
        QSet<qint64> wanted = QSet<qint64>(layerIt.value().cbegin(), layerIt.value().cend());

        QgsFeatureIterator iterator = layer->getFeatures();
        QgsFeature f;
        while (iterator.nextFeature(f))
        {
            if (!wanted.contains(f.id()))
                continue;

            QgsFeature outFeat(outFields);
            outFeat.setGeometry(f.geometry());

            QgsAttributes attrs;
            // 按字段名复制白名单字段（避免索引错位）
            for (const QString &fname : copySrcNames)
                attrs.append(f.attribute(fname));

            // 元信息
            attrs.append(layer->name());
            attrs.append(static_cast<qint64>(f.id()));

            // 判断 elementType：使用预构建的 symbolComponentMap 判断是否为 symbol
            QString elemType = "text";
            auto symIt = symbolComponentMap.find(layer->name());
            if (symIt != symbolComponentMap.end() && symIt.value().contains(f.id()))
                elemType = "symbol";

            attrs.append(elemType);
            attrs.append("OVERLAP_ERROR");

            outFeat.setAttributes(attrs);
            if (!writer.addFeature(outFeat))
            {
                logger->warn("添加要素失败 fid={} : {}", (long long)f.id(), writer.errorMessage().toStdString());
            }
            else
            {
                ++added;
            }
        }
    }

    logger->info("输出 {} 个 {} 问题要素至 {}", added, elementType.toStdString(), outputPath.toStdString());
    return SE_ERROR_NONE;
}

// 修改后的主函数：负责生成三种输出图层
static SE_Error writeOverlapResultLayer(
    const QList<AnnotationCheckResult> &results,
    const QList<ElementInfo> &allElements,
    const QList<QgsVectorLayer *> &loadedLayers,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (results.isEmpty() || allElements.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    // --- 1. 收集所有涉及重叠的要素ID，并按类型分组 ---
    QSet<qint64> problemAnnotationIds;
    QSet<qint64> problemSymbolIds;
    for (const auto &result : results)
    {
        qint64 id1 = result.featureId.toLongLong();
        qint64 id2 = result.actualValue.toLongLong();

        if (allElements[id1].elementType == "text")
            problemAnnotationIds.insert(id1);
        else
            problemSymbolIds.insert(id1);

        if (allElements[id2].elementType == "text")
            problemAnnotationIds.insert(id2);
        else
            problemSymbolIds.insert(id2);
    }

    // --- 2. 生成新的文件路径 ---
    QFileInfo basePathInfo(szOutputCheckResultLayerPath);
    QString baseName = basePathInfo.absolutePath() + "/" + basePathInfo.baseName();
    QString annoOutputPath = baseName + "_anno.shp";
    QString symbolOutputPath = baseName + "_symbol.shp";

    // --- 3. 写入独立的注记和符号问题图层 ---
    SE_Error annoLayerResult = writeProblemElementsLayer(
        problemAnnotationIds, allElements, loadedLayers,
        annoOutputPath, "注记", logger);
    if (annoLayerResult != SE_ERROR_NONE)
        return annoLayerResult;

    SE_Error symbolLayerResult = writeProblemElementsLayer(
        problemSymbolIds, allElements, loadedLayers,
        symbolOutputPath, "符号", logger);
    if (symbolLayerResult != SE_ERROR_NONE)
        return symbolLayerResult;

    // --- 4. 写入原有的连接线图层 ---
    try
    {
        QgsFields fields;
        fields.append(QgsField("error_msg", QVariant::String, "String", 254));
        fields.append(QgsField("elem1_type", QVariant::String, "String", 20));
        fields.append(QgsField("elem1_id", QVariant::String, "String", 50));
        fields.append(QgsField("elem1_layer", QVariant::String, "String", 80));
        fields.append(QgsField("elem2_type", QVariant::String, "String", 20));
        fields.append(QgsField("elem2_id", QVariant::String, "String", 50));
        fields.append(QgsField("elem2_layer", QVariant::String, "String", 80));

        QgsCoordinateReferenceSystem crs;
        if (!loadedLayers.isEmpty() && loadedLayers.first())
        {
            crs = loadedLayers.first()->crs();
        }

        QgsVectorFileWriter writer(
            QString::fromUtf8(szOutputCheckResultLayerPath), "UTF-8", fields,
            QgsWkbTypes::LineString, crs, "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            logger->error("创建连接线图层写入器失败: {}", writer.errorMessage().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        for (const auto &result : results)
        {
            qint64 id1 = result.featureId.toLongLong();
            qint64 id2 = result.actualValue.toLongLong();

            const ElementInfo &elem1 = allElements[id1];
            const ElementInfo &elem2 = allElements[id2];

            QgsPointXY point1 = elem1.geometry->centroid().asPoint();
            QgsPointXY point2 = elem2.geometry->centroid().asPoint();

            QgsFeature feature;
            feature.setGeometry(QgsGeometry::fromPolylineXY({point1, point2}));

            QgsAttributes attributes;
            attributes << result.errorMessage
                       << elem1.elementType
                       << elem1.originalFeatureId.toString()
                       << elem1.layerName
                       << elem2.elementType
                       << elem2.originalFeatureId.toString()
                       << elem2.layerName;
            feature.setAttributes(attributes);

            if (!writer.addFeature(feature))
            {
                logger->warn("向连接线图层添加要素失败: {}", writer.errorMessage().toStdString());
            }
        }
        logger->info("成功写入 {} 条重叠连接线到图层: {}", results.size(), szOutputCheckResultLayerPath);
    }
    catch (const std::exception &e)
    {
        logger->error("写入连接线图层时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    return SE_ERROR_NONE;
}

// 清理 ElementInfo 列表中的几何对象
void cleanupElements(QList<ElementInfo> &elements)
{
    for (auto &element : elements)
    {
        delete element.geometry;
        element.geometry = nullptr;
        delete element.effectiveGeometry;
        element.effectiveGeometry = nullptr;
    }
}

MY_PLUGIN_API SE_Error AnnotationOverlapCheck(
    const char *szSymbolLayerPaths,
    const char *szAnnotationLayerPaths,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 1. 参数验证
    if (!szSymbolLayerPaths || !szAnnotationLayerPaths || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 2. 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "AnnotationOverlapCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行全局重叠检查");
    logger->info("符号图层: {}", szSymbolLayerPaths);
    logger->info("注记图层: {}", szAnnotationLayerPaths);
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    // 声明资源变量（在 try 块外部，以便在 catch 块中也能清理）
    QList<ElementInfo> allElements;
    QList<QgsVectorLayer *> loadedLayers;

    try
    {
        // 3. 解析图层路径
        QString symbolPathsStr = QString::fromUtf8(szSymbolLayerPaths);
        QStringList symbolLayerPaths = symbolPathsStr.split(';', QString::SkipEmptyParts);
        QString annotationPathsStr = QString::fromUtf8(szAnnotationLayerPaths);
        QStringList annotationLayerPaths = annotationPathsStr.split(';', QString::SkipEmptyParts);

        if (symbolLayerPaths.isEmpty() && annotationLayerPaths.isEmpty())
        {
            logger->warn("没有提供任何符号或注记图层，检查终止。");
            spdlog::drop(logger->name());
            return SE_ERROR_NONE;
        }

        // 4. 创建并初始化高级算法实例
        SE_AnnotationAdvancedAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        // 加载规则
        if (!algorithm.loadOverlapRules(QString::fromUtf8(szSchemaXmlPath), mapScale))
        {
            logger->error("加载重叠检查规则失败: {}", szSchemaXmlPath);
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 5. 执行重叠检查
        logger->info("开始加载图层并执行检查...");
        QList<AnnotationCheckResult> results = algorithm.checkOverlap(
            symbolLayerPaths,
            annotationLayerPaths,
            allElements,
            loadedLayers);
        logger->info("重叠检查完成，发现 {} 处重叠问题。", results.size());

        // 6. 生成JSON报告
        QMap<QString, QString> statistics; // 全局重叠检查不涉及统计信息
        QString jsonString = generateJsonReport(results, statistics, "206", "全局重叠检查", logger);

        // 7. 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            cleanupElements(allElements);
            qDeleteAll(loadedLayers);
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 8. 写入问题图层
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeOverlapResultLayer(
                results,
                allElements,
                loadedLayers,
                szOutputCheckResultLayerPath,
                logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入重叠问题图层失败");
                // 清理资源
                cleanupElements(allElements);
                qDeleteAll(loadedLayers);
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        // 9. 清理资源
        cleanupElements(allElements);
        qDeleteAll(loadedLayers);

        logger->info("全局重叠检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        // 异常情况下也要清理资源
        cleanupElements(allElements);
        qDeleteAll(loadedLayers);
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

// 新增：为符号缺失注记问题导出符号图层
static SE_Error writeSymbolMissingAnnoLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *actualLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    // 筛选出符号缺失注记的错误
    QMap<QString, AnnotationCheckResult> missingAnnoResults;
    for (const auto &result : results)
    {
        if (result.errorMessage.contains("【符号缺失注记】"))
        {
            missingAnnoResults.insert(result.featureId, result); // featureId存储的是symbol_id
        }
    }

    if (missingAnnoResults.isEmpty())
    {
        logger->info("没有符号缺失注记问题，跳过符号图层导出");
        return SE_ERROR_NONE;
    }

    try
    {
        QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
        QFileInfo outputInfo(outputPath);
        QString symbolOutputPath = outputInfo.absolutePath() + "/" + outputInfo.baseName() + "_symbol.shp";

        // 确保输出目录存在
        QDir outputDir = outputInfo.dir();
        if (!outputDir.exists() && !outputDir.mkpath("."))
        {
            logger->error("无法创建输出目录: {}", outputDir.absolutePath().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 删除已存在的相关文件
        QStringList extensions = {"shp", "shx", "dbf", "prj", "cpg"};
        QString baseName = outputInfo.baseName() + "_symbol";
        for (const QString &ext : extensions)
        {
            QString filePath = outputInfo.absolutePath() + "/" + baseName + "." + ext;
            if (QFile::exists(filePath))
                QFile::remove(filePath);
        }

        // 获取symbol_id字段索引
        int symbolIdIdx = actualLayer->fields().indexOf("symbol_id");
        if (symbolIdIdx < 0)
        {
            logger->error("实际图层缺少 symbol_id 字段");
            return SE_ERROR_CORRUPT_DATA;
        }

        // 每个 symbol_id 只保留一个代表要素用于补充符号名称等概要属性。
        QMap<QString, QgsFeature> symbolToFeature;
        QgsFeatureIterator it = actualLayer->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f))
        {
            QString symbolId = f.attribute(symbolIdIdx).toString();
            if (missingAnnoResults.contains(symbolId) && !symbolToFeature.contains(symbolId))
            {
                symbolToFeature.insert(symbolId, f);
            }
        }

        // 输出符号级点图层，不复制源组件图层的 wkt_geom、symbol_pat 等逐组件字段。
        QgsFields outputFields;
        outputFields.append(QgsField("symbol_id", QVariant::String, "String", 80));
        outputFields.append(QgsField("symbol_nam", QVariant::String, "String", 100));
        outputFields.append(QgsField("center_x", QVariant::Double));
        outputFields.append(QgsField("center_y", QVariant::Double));
        outputFields.append(QgsField("ERROR_TYPE", QVariant::String, "String", 32));
        outputFields.append(QgsField("ERROR_MSG", QVariant::String, "String", 254));

        // 创建写入器
        QgsVectorFileWriter writer(
            symbolOutputPath,
            "UTF-8",
            outputFields,
            QgsWkbTypes::Point,
            actualLayer->crs(),
            "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            logger->error("创建符号问题图层失败: {}", writer.errorMessage().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 写入要素：一个缺失注记的符号只输出一个点，避免复合符号的组成线造成大量重复错误。
        int addedCount = 0;
        int symbolNameIdx = actualLayer->fields().indexOf("symbol_nam");
        for (auto symIt = missingAnnoResults.constBegin(); symIt != missingAnnoResults.constEnd(); ++symIt)
        {
            const QString &symbolId = symIt.key();
            const AnnotationCheckResult &result = symIt.value();

            if (!symbolToFeature.contains(symbolId))
            {
                logger->warn("未找到 symbol_id={} 的代表要素，跳过符号点导出", symbolId.toStdString());
                continue;
            }

            QgsFeature outFeature(outputFields);
            outFeature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(result.position.x(), result.position.y())));

            const QgsFeature &srcFeature = symbolToFeature[symbolId];

            QgsAttributes attrs;
            attrs << symbolId
                  << (symbolNameIdx >= 0 ? srcFeature.attribute(symbolNameIdx) : QVariant())
                  << result.position.x()
                  << result.position.y()
                  << "MISSING_ANNO"
                  << QString("符号 '%1' 缺失注记").arg(symbolId);
            outFeature.setAttributes(attrs);

            if (!writer.addFeature(outFeature))
            {
                logger->warn("添加符号点要素失败 symbol_id={}: {}", symbolId.toStdString(),
                             writer.errorMessage().toStdString());
            }
            else
            {
                addedCount++;
            }
        }

        logger->info("成功导出 {} 个符号点要素（对应 {} 个缺失注记的符号）到: {}",
                     addedCount, missingAnnoResults.size(), symbolOutputPath.toStdString());

        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("导出符号问题图层时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

static SE_Error writeGdbMissingAnnoLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *gdbLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    QMap<QString, AnnotationCheckResult> missingAnnoResults;
    for (const auto &result : results)
    {
        if (result.errorMessage.contains("【GDB要素缺失注记】"))
        {
            missingAnnoResults.insert(result.featureId, result);
        }
    }

    if (missingAnnoResults.isEmpty())
    {
        logger->info("没有GDB线/面缺失注记问题，跳过GDB问题图层导出");
        return SE_ERROR_NONE;
    }

    if (!gdbLayer || !gdbLayer->isValid())
    {
        logger->warn("GDB图层无效，无法导出GDB线/面缺失注记问题图层");
        return SE_ERROR_NONE;
    }

    try
    {
        QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
        QFileInfo outputInfo(outputPath);
        QString gdbOutputPath = outputInfo.absolutePath() + "/" + outputInfo.baseName() + "_gdb_missing.shp";

        QDir outputDir = outputInfo.dir();
        if (!outputDir.exists() && !outputDir.mkpath("."))
        {
            logger->error("无法创建输出目录: {}", outputDir.absolutePath().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        QStringList extensions = {"shp", "shx", "dbf", "prj", "cpg"};
        QString baseName = outputInfo.baseName() + "_gdb_missing";
        for (const QString &ext : extensions)
        {
            QString filePath = outputInfo.absolutePath() + "/" + baseName + "." + ext;
            if (QFile::exists(filePath))
                QFile::remove(filePath);
        }

        QgsFields outputFields;
        outputFields.append(QgsField("gdb_fids", QVariant::String, "String", 254));
        outputFields.append(QgsField("ERROR_TYPE", QVariant::String, "String", 32));
        outputFields.append(QgsField("ERROR_MSG", QVariant::String, "String", 254));
        outputFields.append(QgsField("actual_val", QVariant::String, "String", 100));
        outputFields.append(QgsField("expect_val", QVariant::String, "String", 100));

        QgsVectorFileWriter writer(
            gdbOutputPath,
            "UTF-8",
            outputFields,
            gdbLayer->wkbType(),
            gdbLayer->crs(),
            "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            logger->error("创建GDB问题图层失败: {}", writer.errorMessage().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        int addedCount = 0;
        for (auto it = missingAnnoResults.constBegin(); it != missingAnnoResults.constEnd(); ++it)
        {
            QString gdbFids = it.key();
            const AnnotationCheckResult &result = it.value();

            QList<QgsGeometry> geomsToMerge;
            QStringList fidParts = gdbFids.split(',', Qt::SkipEmptyParts);
            for (const QString &part : fidParts)
            {
                bool ok = false;
                qint64 fid = part.trimmed().toLongLong(&ok);
                if (!ok)
                    continue;

                QgsFeature srcFeature = gdbLayer->getFeature(fid);
                if (srcFeature.isValid() && srcFeature.hasGeometry())
                    geomsToMerge.append(srcFeature.geometry());
            }

            if (geomsToMerge.isEmpty())
            {
                logger->warn("未找到GDB FIDs={} 的要素，跳过GDB问题图层导出", gdbFids.toStdString());
                continue;
            }

            QgsGeometry outputGeometry = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(geomsToMerge));
            if (outputGeometry.isEmpty())
                outputGeometry = geomsToMerge.first();

            QgsFeature outFeature(outputFields);
            outFeature.setGeometry(outputGeometry);
            QgsAttributes attrs;
            attrs << gdbFids.left(254)
                  << "MISSING_ANNO"
                  << result.errorMessage.left(254)
                  << result.actualValue.left(100)
                  << result.expectedValue.left(100);
            outFeature.setAttributes(attrs);

            if (!writer.addFeature(outFeature))
            {
                logger->warn("添加GDB问题要素失败 gdb_fids={}: {}", gdbFids.toStdString(), writer.errorMessage().toStdString());
            }
            else
            {
                addedCount++;
            }
        }

        logger->info("成功导出 {} 个GDB线/面缺失注记要素到: {}", addedCount, gdbOutputPath.toStdString());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("导出GDB问题图层时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

// 更新：符号注记匹配检查结果图层写入函数
static SE_Error writeSymbolAnnotationMatchResultLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *actualLayer,
    QgsVectorLayer *gdbLayer,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (results.isEmpty())
    {
        logger->info("没有发现问题，无需创建结果图层");
        return SE_ERROR_NONE;
    }

    if (!matchResultLayer)
    {
        logger->error("matchResultLayer 为空，无法写入结果图层");
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 1. 先导出符号缺失注记的符号图层
    if (actualLayer)
    {
        SE_Error symbolLayerResult = writeSymbolMissingAnnoLayer(
            results, actualLayer, szOutputCheckResultLayerPath, logger);
        if (symbolLayerResult != SE_ERROR_NONE)
        {
            logger->warn("导出符号问题图层失败，继续处理注记问题图层");
        }
    }

    if (gdbLayer)
    {
        SE_Error gdbLayerResult = writeGdbMissingAnnoLayer(
            results, gdbLayer, szOutputCheckResultLayerPath, logger);
        if (gdbLayerResult != SE_ERROR_NONE)
        {
            logger->warn("导出GDB线/面问题图层失败，继续处理注记问题图层");
        }
    }

    // 2. 导出注记相关问题图层（排除符号缺失注记的问题）
    QList<AnnotationCheckResult> annoResults;
    for (const auto &result : results)
    {
        if (!result.errorMessage.contains("【符号缺失注记】") &&
            !result.errorMessage.contains("【GDB要素缺失注记】"))
        {
            annoResults.append(result);
        }
    }

    if (annoResults.isEmpty())
    {
        logger->info("没有注记相关问题，跳过注记问题图层导出");
        return SE_ERROR_NONE;
    }

    try
    {
        QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);

        // 确保输出目录存在
        QFileInfo outputInfo(outputPath);
        QDir outputDir = outputInfo.dir();
        if (!outputDir.exists() && !outputDir.mkpath("."))
        {
            logger->error("无法创建输出目录: {}", outputDir.absolutePath().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 删除已存在的相关文件（增强健壮性：GDAL可能锁定残留文件）
        QStringList extensions = {"shp", "shx", "dbf", "prj", "cpg"};
        QString baseName = outputInfo.baseName();
        QString dirPath = outputInfo.dir().absolutePath();

        QSet<QString> failedDeletes;
        for (const QString &ext : extensions)
        {
            QString filePath = QString("%1/%2.%3").arg(dirPath).arg(baseName).arg(ext);
            if (QFile::exists(filePath))
            {
                // 先尝试确保文件可写（Windows只读属性会导致remove失败）
                QFile::setPermissions(filePath, QFile::ReadOwner | QFile::WriteOwner);
                if (!QFile::remove(filePath))
                {
                    failedDeletes.insert(ext);
                    logger->warn("无法删除旧文件: {}", filePath.toStdString());
                }
            }
        }

        // 如果还有残留文件（GDAL/QGIS持有句柄），尝试重命名为.bak兜底
        if (!failedDeletes.isEmpty())
        {
            qint64 pid = QCoreApplication::applicationPid();
            QString ts = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");
            for (const QString &ext : failedDeletes)
            {
                QString filePath = QString("%1/%2.%3").arg(dirPath).arg(baseName).arg(ext);
                if (QFile::exists(filePath))
                {
                    QString bakPath = QString("%1/%2_pid%3_%4_old.%5")
                                          .arg(dirPath)
                                          .arg(baseName)
                                          .arg(pid)
                                          .arg(ts)
                                          .arg(ext);
                    if (QFile::rename(filePath, bakPath))
                    {
                        logger->info("已将锁定文件重命名为: {}", bakPath.toStdString());
                    }
                    else
                    {
                        logger->error("无法重命名/删除锁定文件: {}", filePath.toStdString());
                    }
                }
            }
        }

        // 定义输出字段
        QgsFields outputFields;
        outputFields.append(QgsField("anno_fid", QVariant::LongLong));
        outputFields.append(QgsField("group_id", QVariant::LongLong));
        outputFields.append(QgsField("symbol_id", QVariant::String, "String", 80));
        outputFields.append(QgsField("text_conte", QVariant::String, "String", 100));
        outputFields.append(QgsField("assoc_name", QVariant::String, "String", 100));
        outputFields.append(QgsField("match_stat", QVariant::String, "String", 30));
        outputFields.append(QgsField("error_type", QVariant::String, "String", 50));
        outputFields.append(QgsField("error_msg", QVariant::String, "String", 254));
        outputFields.append(QgsField("actual_val", QVariant::String, "String", 100));
        outputFields.append(QgsField("expect_val", QVariant::String, "String", 100));
        outputFields.append(QgsField("check_time", QVariant::String, "String", 20));

        // 获取坐标系统和几何类型
        QgsCoordinateReferenceSystem crs = matchResultLayer->crs();
        QgsWkbTypes::Type wkbType = matchResultLayer->wkbType();

        // 创建写入器
        QgsVectorFileWriter writer(
            outputPath, "UTF-8", outputFields, wkbType, crs, "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            logger->error("创建注记问题图层失败: {}", writer.errorMessage().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");

        // 获取字段索引
        int annoFidIdx = matchResultLayer->fields().indexOf("anno_fid");
        int groupIdIdx = matchResultLayer->fields().indexOf("group_id");
        int symbolIdIdx = matchResultLayer->fields().indexOf("symbol_id");
        int textConteIdx = matchResultLayer->fields().indexOf("text_conte");
        int assocNameIdx = matchResultLayer->fields().indexOf("assoc_name");
        int matchStatIdx = matchResultLayer->fields().indexOf("match_stat");

        if (annoFidIdx < 0)
        {
            logger->error("匹配结果图层缺少 anno_fid 字段");
            return SE_ERROR_CORRUPT_DATA;
        }

        // 【关键修复】按 anno_fid 分组结果（而不是 featureId）
        QMap<qint64, QList<AnnotationCheckResult>> groupedResults;
        for (const auto &result : annoResults)
        {
            qint64 annoFid = result.featureId.toLongLong();
            groupedResults[annoFid].append(result);
        }

        // 【关键修复】构建 anno_fid -> Feature 的映射，避免重复遍历
        QMap<qint64, QgsFeature> annoFidToFeature;
        QgsFeatureIterator it = matchResultLayer->getFeatures();
        QgsFeature f;
        while (it.nextFeature(f))
        {
            qint64 annoFid = f.attribute(annoFidIdx).toLongLong();
            if (groupedResults.contains(annoFid))
            {
                annoFidToFeature[annoFid] = f;
            }
        }

        // 遍历每个有问题的要素并写入
        int addedCount = 0;
        for (auto resultIt = groupedResults.constBegin(); resultIt != groupedResults.constEnd(); ++resultIt)
        {
            qint64 annoFid = resultIt.key();
            const QList<AnnotationCheckResult> &featureErrors = resultIt.value();

            // 在映射中查找对应的要素
            if (!annoFidToFeature.contains(annoFid))
            {
                logger->warn("在匹配结果图层中未找到 anno_fid={} 的要素", annoFid);
                continue;
            }

            const QgsFeature &matchFeature = annoFidToFeature[annoFid];

            // 收集所有错误信息
            QStringList errorTypes, errorMsgs, actualVals, expectVals;
            for (const auto &error : featureErrors)
            {
                QString errorType;
                switch (error.errorType)
                {
                case AnnotationErrorType::FontError:
                    if (error.errorMessage.contains("内容错误"))
                        errorType = "CONTENT_ERROR";
                    else if (error.errorMessage.contains("缺失符号"))
                        errorType = "MISSING_SYMBOL";
                    else
                        errorType = "MATCH_ERROR";
                    break;
                case AnnotationErrorType::OverlapError:
                    errorType = "DISTANCE_ERROR";
                    break;
                case AnnotationErrorType::SizeError:
                    if (error.errorMessage.contains("字距"))
                        errorType = "CHAR_SPACING_ERROR";
                    else if (error.errorMessage.contains("组距"))
                        errorType = "GROUP_SPACING_ERROR";
                    else
                        errorType = "SIZE_ERROR";
                    break;
                default:
                    errorType = "OTHER_ERROR";
                    break;
                }

                errorTypes.append(errorType);
                errorMsgs.append(error.errorMessage);
                actualVals.append(error.actualValue);
                expectVals.append(error.expectedValue);
            }

            // 创建输出要素
            QgsFeature outFeature(outputFields);
            QgsGeometry geom = matchFeature.geometry();
            if (!geom.isNull() && !geom.isEmpty())
            {
                outFeature.setGeometry(geom);
            }
            else
            {
                // 如果几何无效，使用错误位置创建点几何
                if (!featureErrors.isEmpty())
                {
                    QPointF pos = featureErrors.first().position;
                    outFeature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(pos.x(), pos.y())));
                }
            }

            // 设置属性
            QString joinedErrType = errorTypes.join("; ").left(50);
            QString joinedErrMsg = errorMsgs.join("; ").left(254);
            QString joinedActual = actualVals.join("; ").left(100);
            QString joinedExpect = expectVals.join("; ").left(100);

            QgsAttributes attrs;
            attrs << annoFid
                  << (groupIdIdx >= 0 ? matchFeature.attribute(groupIdIdx) : QVariant())
                  << (symbolIdIdx >= 0 ? matchFeature.attribute(symbolIdIdx) : QVariant())
                  << joinedActual
                  << (assocNameIdx >= 0 ? matchFeature.attribute(assocNameIdx) : QVariant())
                  << (matchStatIdx >= 0 ? matchFeature.attribute(matchStatIdx) : QVariant())
                  << joinedErrType
                  << joinedErrMsg
                  << joinedActual
                  << joinedExpect
                  << currentTime;

            outFeature.setAttributes(attrs);

            if (!writer.addFeature(outFeature))
            {
                logger->warn("添加要素失败 anno_fid={}: {}", annoFid, writer.errorMessage().toStdString());
            }
            else
            {
                addedCount++;
            }
        }

        logger->info("成功写入 {} 个注记问题要素到结果图层", addedCount);
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("写入注记问题图层时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}

MY_PLUGIN_API SE_Error SymbolAnnotationMatchCheck(
    const char *szMatchResultLayerPath,
    const char *szActualLayerPath,
    const char *szGdbLayerPath,
    const char *szAnnotationLayerPath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 1. 参数验证
    if (!szMatchResultLayerPath || /* szActualLayerPath can be empty */ !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 2. 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "SymbolAnnotationMatchCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行符号注记匹配检查（ID=210）");
    logger->info("匹配结果图层: {}", szMatchResultLayerPath);
    logger->info("实际要素图层: {}", szActualLayerPath);
    logger->info("GDB图层: {}", szGdbLayerPath ? szGdbLayerPath : "");
    logger->info("原始注记图层: {}", szAnnotationLayerPath ? szAnnotationLayerPath : "");
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 3. 加载图层
        QgsVectorLayer *matchResultLayer = new QgsVectorLayer(
            QString::fromUtf8(szMatchResultLayerPath), "match_result", "ogr");

        QgsVectorLayer *actualLayer = nullptr;
        QgsVectorLayer *gdbLayer = nullptr;
        QgsVectorLayer *annoPolygonLayer = nullptr;
        if (szActualLayerPath && strlen(szActualLayerPath) > 0)
        {
            actualLayer = new QgsVectorLayer(
                QString::fromUtf8(szActualLayerPath), "actual", "ogr");
            if (!actualLayer->isValid())
            {
                logger->error("无法加载实际要素图层: {}", szActualLayerPath);
                delete matchResultLayer;
                delete actualLayer; // it's safe to delete nullptr
                delete gdbLayer;
                delete annoPolygonLayer;
                spdlog::drop(logger->name());
                return SE_ERROR_CORRUPT_DATA;
            }
        }

        if (szGdbLayerPath && strlen(szGdbLayerPath) > 0)
        {
            gdbLayer = new QgsVectorLayer(
                QString::fromUtf8(szGdbLayerPath), "gdb", "ogr");
            if (!gdbLayer->isValid())
            {
                logger->error("无法加载GDB图层: {}", szGdbLayerPath);
                delete matchResultLayer;
                delete actualLayer;
                delete gdbLayer;
                delete annoPolygonLayer;
                spdlog::drop(logger->name());
                return SE_ERROR_CORRUPT_DATA;
            }
        }

        // 加载原始注记Polygon图层（用于孤儿注记检测）
        if (szAnnotationLayerPath && strlen(szAnnotationLayerPath) > 0)
        {
            annoPolygonLayer = new QgsVectorLayer(
                QString::fromUtf8(szAnnotationLayerPath), "anno_polygon", "ogr");
            if (!annoPolygonLayer->isValid())
            {
                logger->warn("无法加载原始注记图层（将跳过孤儿注记检测）: {}", szAnnotationLayerPath);
                delete annoPolygonLayer;
                annoPolygonLayer = nullptr;
            }
            else
            {
                logger->info("成功加载原始注记图层，要素数量: {}", annoPolygonLayer->featureCount());
            }
        }
        else
        {
            logger->info("未提供原始注记图层路径，跳过孤儿注记检测");
        }

        if (!matchResultLayer->isValid())
        {
            logger->error("无法加载输入图层: {}", szMatchResultLayerPath);
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("成功加载匹配结果图层，要素数量: {}", matchResultLayer->featureCount());

        // 4. 创建算法实例并加载规则
        SE_AnnotationAdvancedAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        if (!algorithm.loadSymbolAnnotationMatchRules(QString::fromUtf8(szSchemaXmlPath), mapScale))
        {
            logger->warn("未在XML中找到id=210的check_item，跳过符号注记匹配检查");
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_NONE;
        }

        // 5. 执行检查
        logger->info("开始执行检查...");
        QString algorithmError;
        QList<AnnotationCheckResult> results = algorithm.checkSymbolAnnotationMatch(
            matchResultLayer, actualLayer, gdbLayer, &algorithmError, annoPolygonLayer);
        if (!algorithmError.isEmpty())
        {
            logger->error("符号注记匹配检查失败: {}", algorithmError.toStdString());
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }
        logger->info("检查完成，发现 {} 个问题", results.size());

        // 过滤孤立通用类别词（单字尾缀无法证明与具体地名相关）
        // 在结果级别过滤，同时影响JSON报告和SHP图层
        {
            // 按基名（去掉[UNMATCHED_FID:XXX]后缀）分组，确保多字群不被拆散
            // 如"乌"+"沟"都关联"乌玛沟"→同组size=2→不过滤
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

                // 检查全组是否都由纯通用类别词构成
                // 例如"沟" vs "泉儿沟" → 全组只有"沟"（通用词）→ 过滤
                // 例如"剪"+"沟" vs "剪支沟" → "剪"不是通用词 → 保留
                bool allGeneric = true;
                bool hasRealName = false;
                for (int idx : indices)
                {
                    const auto &r = results[idx];
                    if (r.actualValue == baseName)
                    {
                        hasRealName = true;
                        break;
                    }
                    // if (!isGenericCategoryOnlyIn210(r.actualValue, r.expectedValue))
                    //{
                    //     allGeneric = false;
                    //     break;
                    // }
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
                        logger->info("过滤孤立通用类别词: actual='{}' source='{}'",
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
                logger->info("过滤孤立通用类别词完成: 移除 {} 条, 剩余 {} 条",
                             toRemove.size(), results.size());
            }
        }

        // 6. 生成JSON报告（按源名称分组输出，避免散字逐个罗列）
        QMap<QString, QString> statistics; // 符号注记匹配检查不涉及统计信息
        QString jsonString = generateGroupedJsonReport(results, statistics, "210", "符号注记匹配检查", logger);

        // 7. 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete matchResultLayer;
            delete actualLayer;
            delete gdbLayer;
            delete annoPolygonLayer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 8. 写入问题图层（传入actualLayer用于导出符号问题）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeSymbolAnnotationMatchResultLayer(
                results, matchResultLayer, actualLayer, gdbLayer, szOutputCheckResultLayerPath, logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题图层失败");
                delete matchResultLayer;
                delete actualLayer;
                delete gdbLayer;
                delete annoPolygonLayer;
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        // 清理资源
        delete matchResultLayer;
        delete actualLayer;
        delete gdbLayer;
        delete annoPolygonLayer;

        logger->info("符号注记匹配检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

MY_PLUGIN_API SE_Error CharSpacingCheck(
    const char *szMatchResultLayerPath,
    const char *szGdbLayerPath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    // 1. 参数验证（与头文件保持一致，包含 JSON 输出缓冲区与大小）
    if (!szMatchResultLayerPath || !szGdbLayerPath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 2. 创建日志记录器
    auto logger = createLogger(szOutputLogFilePath, "CharSpacingCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行字距检查（ID=204）");
    logger->info("匹配结果图层: {}", szMatchResultLayerPath);
    logger->info("GDB图层: {}", szGdbLayerPath);
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    try
    {
        // 3. 加载图层
        QgsVectorLayer *matchResultLayer = new QgsVectorLayer(
            QString::fromUtf8(szMatchResultLayerPath), "match_result", "ogr");
        QgsVectorLayer *gdbLayer = new QgsVectorLayer(
            QString::fromUtf8(szGdbLayerPath), "gdb", "ogr");

        if (!matchResultLayer->isValid() || !gdbLayer->isValid())
        {
            logger->error("无法加载输入图层");
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("成功加载匹配结果图层，要素数量: {}", matchResultLayer->featureCount());
        logger->info("成功加载GDB图层，要素数量: {}", gdbLayer->featureCount());

        // 4. 创建算法实例并加载规则
        SE_AnnotationAdvancedAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        if (!algorithm.loadCharSpacingRules(QString::fromUtf8(szSchemaXmlPath), mapScale))
        {
            logger->error("加载字距检查规则失败");
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 5. 执行检查
        logger->info("开始执行检查...");
        QList<AnnotationCheckResult> results = algorithm.checkCharSpacing(matchResultLayer, gdbLayer);
        logger->info("检查完成，发现 {} 个问题", results.size());

        // 6. 生成JSON报告
        QMap<QString, QString> statistics; // 字距检查不涉及统计信息
        QString jsonString = generateJsonReport(results, statistics, "204", "字距检查", logger);

        // 7. 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 8. 写入问题图层（复用210的函数，只导出注记）
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeSymbolAnnotationMatchResultLayer(
                results, matchResultLayer, nullptr, nullptr, szOutputCheckResultLayerPath, logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题图层失败");
                delete matchResultLayer;
                delete gdbLayer;
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        // 9. 清理资源
        delete matchResultLayer;
        delete gdbLayer;

        logger->info("字距检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

MY_PLUGIN_API SE_Error GroupSpacingCheck(
    const char *szMatchResultLayerPath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    if (!szMatchResultLayerPath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    auto logger = createLogger(szOutputLogFilePath, "GroupSpacingCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行注记组距检查（ID=207）");
    logger->info("匹配结果图层: {}", szMatchResultLayerPath);
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    try
    {
        QgsVectorLayer *matchResultLayer = new QgsVectorLayer(
            QString::fromUtf8(szMatchResultLayerPath), "match_result", "ogr");

        if (!matchResultLayer->isValid())
        {
            logger->error("无法加载匹配结果图层");
            delete matchResultLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("成功加载匹配结果图层，要素数量: {}", matchResultLayer->featureCount());

        SE_AnnotationAdvancedAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        if (!algorithm.loadGroupSpacingRules(QString::fromUtf8(szSchemaXmlPath), mapScale))
        {
            logger->error("加载注记组距检查规则失败");
            delete matchResultLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("开始执行检查...");
        QMap<QString, QString> statistics;
        QList<AnnotationCheckResult> results = algorithm.checkGroupSpacing(matchResultLayer, statistics);
        logger->info("检查完成，发现 {} 个问题", results.size());

        // 生成JSON报告字符串
        QString jsonString = generateJsonReport(results, statistics, "207", "注记组距检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete matchResultLayer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题图层
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeSymbolAnnotationMatchResultLayer(
                results, matchResultLayer, nullptr, nullptr, szOutputCheckResultLayerPath, logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题图层失败");
                delete matchResultLayer;
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        delete matchResultLayer;
        logger->info("注记组距检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

MY_PLUGIN_API SE_Error EqualSpacingCheck(
    const char *szMatchResultLayerPath,
    const char *szGdbLayerPath,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    if (!szMatchResultLayerPath || !szGdbLayerPath || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    auto logger = createLogger(szOutputLogFilePath, "EqualSpacingCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行面要素等间距标注检查（ID=208）");
    logger->info("匹配结果图层: {}", szMatchResultLayerPath);
    logger->info("GDB图层: {}", szGdbLayerPath);
    logger->info("质检方案: {}", szSchemaXmlPath);
    logger->info("地图比例尺: {}", scale);

    try
    {
        QgsVectorLayer *matchResultLayer = new QgsVectorLayer(
            QString::fromUtf8(szMatchResultLayerPath), "match_result", "ogr");
        QgsVectorLayer *gdbLayer = new QgsVectorLayer(
            QString::fromUtf8(szGdbLayerPath), "gdb", "ogr");

        if (!matchResultLayer->isValid() || !gdbLayer->isValid())
        {
            logger->error("无法加载输入图层");
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("成功加载匹配结果图层，要素数量: {}", matchResultLayer->featureCount());
        logger->info("成功加载GDB图层，要素数量: {}", gdbLayer->featureCount());

        SE_AnnotationAdvancedAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        if (!algorithm.loadEqualSpacingRules(QString::fromUtf8(szSchemaXmlPath), mapScale))
        {
            logger->error("加载等间距标注检查规则失败");
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("开始执行检查...");
        QMap<QString, QString> statistics; // 当前检查项不需要统计信息
        QList<AnnotationCheckResult> results = algorithm.checkEqualSpacing(matchResultLayer, gdbLayer);
        logger->info("检查完成，发现 {} 个问题", results.size());

        // 生成JSON报告字符串
        QString jsonString = generateJsonReport(results, statistics, "208", "面要素等间距标注检查", logger);

        // 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            delete matchResultLayer;
            delete gdbLayer;
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 写入问题图层
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeSymbolAnnotationMatchResultLayer(
                results, matchResultLayer, nullptr, nullptr, szOutputCheckResultLayerPath, logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题图层失败");
                delete matchResultLayer;
                delete gdbLayer;
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        delete matchResultLayer;
        delete gdbLayer;
        logger->info("面要素等间距标注检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

MY_PLUGIN_API SE_Error SymbolDirectionCheck(
    const char *szSymbolLayerPaths,
    const char *szAssociatedLayerPaths,
    const char *szSchemaXmlPath,
    int scale,
    const char *szOutputCheckResultLayerPath,
    const char *szOutputLogFilePath,
    const char *szOutputJsonPath)
{
    if (!szSymbolLayerPaths || !szAssociatedLayerPaths || !szSchemaXmlPath || scale <= 0 ||
        !szOutputCheckResultLayerPath || !szOutputLogFilePath ||
        !szOutputJsonPath)
    {
        return SE_ERROR_INVALID_PARAMETER;
    }

    auto logger = createLogger(szOutputLogFilePath, "SymbolDirectionCheck");
    if (!logger)
    {
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    logger->info("开始执行符号方向性检查（ID=209）");
    logger->info("符号图层: {}", szSymbolLayerPaths);
    logger->info("关联图层: {}", szAssociatedLayerPaths);

    try
    {
        // 1. 创建算法实例并加载规则
        SE_SymbolDirectionAlgorithm algorithm(logger);
        double mapScale = static_cast<double>(scale);

        DirectionCheckRule rule;
        if (!algorithm.loadDirectionCheckRules(
                QString::fromUtf8(szSchemaXmlPath), mapScale, rule))
        {
            logger->error("加载符号方向检查规则失败");
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        logger->info("符号类型: {}", rule.symbolType.toStdString());

        // 2. 加载符号图层（支持多图层输入）
        QStringList symbolLayerPathList = QString::fromUtf8(szSymbolLayerPaths).split(';', Qt::SkipEmptyParts);
        QList<QgsVectorLayer *> symbolLayers;
        for (const QString &path : symbolLayerPathList)
        {
            QString trimmedPath = path.trimmed();
            if (trimmedPath.isEmpty())
                continue;
            QFileInfo fileInfo(trimmedPath);
            QString layerName = fileInfo.completeBaseName();
            QgsVectorLayer *layer = new QgsVectorLayer(trimmedPath, layerName, "ogr");
            if (layer->isValid())
            {
                symbolLayers.append(layer);
                logger->info("成功加载目标符号图层: {} ({}个要素)", layerName.toStdString(), layer->featureCount());
            }
            else
            {
                logger->warn("无法加载目标符号图层: {}", trimmedPath.toStdString());
                delete layer;
            }
        }
        if (symbolLayers.isEmpty())
        {
            logger->error("没有成功加载任何目标符号图层");
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 3. 合并/过滤目标图层
        QgsVectorLayer *mergedSymbolLayer = nullptr;
        QString tempLayerName = "merged_symbol_layer";
        if (rule.symbolType == "渠流向")
        {
            // 只选取后缀为 _LineString 的图层
            mergedSymbolLayer = nullptr;
            for (QgsVectorLayer *layer : symbolLayers)
            {
                if (layer->name().endsWith("_LineString", Qt::CaseInsensitive))
                {
                    mergedSymbolLayer = layer;
                    break;
                }
            }
            if (!mergedSymbolLayer)
            {
                logger->error("渠流向类型未找到后缀为 _LineString 的图层");
                qDeleteAll(symbolLayers);
                spdlog::drop(logger->name());
                return SE_ERROR_CORRUPT_DATA;
            }
        }
        else if (rule.symbolType == "隧道口")
        {
            QgsVectorLayer *first = symbolLayers.first();
            QgsWkbTypes::Type wkbType = first->wkbType();
            QgsCoordinateReferenceSystem crs = first->crs();

            mergedSymbolLayer = new QgsVectorLayer(
                QString("%1?crs=%2").arg(QgsWkbTypes::displayString(wkbType), crs.isValid() ? crs.authid() : QString()),
                tempLayerName,
                "memory");

            if (mergedSymbolLayer->dataProvider())
            {
                mergedSymbolLayer->dataProvider()->addAttributes(first->fields().toList());
                mergedSymbolLayer->updateFields();

                for (QgsVectorLayer *layer : symbolLayers)
                {
                    QgsFeature f;
                    QgsFeatureIterator it = layer->getFeatures();
                    while (it.nextFeature(f))
                        mergedSymbolLayer->dataProvider()->addFeature(f);
                }
                mergedSymbolLayer->updateExtents();

                if (crs.isValid())
                    mergedSymbolLayer->setCrs(crs);
            }
        }
        else
        {
            // 其他类型，默认只用第一个有效图层
            mergedSymbolLayer = symbolLayers.first();
        }

        if (!mergedSymbolLayer || !mergedSymbolLayer->isValid())
        {
            logger->error("合并后的目标符号图层无效");
            qDeleteAll(symbolLayers);
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 4. 加载关联图层（不变）
        QMap<QString, QgsVectorLayer *> associatedLayers;
        QString layerPathsStr = QString::fromUtf8(szAssociatedLayerPaths);
        QStringList layerPaths = layerPathsStr.split(';', Qt::SkipEmptyParts);

        int layerIndex = 0;
        for (const QString &layerPath : layerPaths)
        {
            QString trimmedPath = layerPath.trimmed();
            if (trimmedPath.isEmpty())
                continue;
            QFileInfo fileInfo(trimmedPath);
            QString layerName = fileInfo.completeBaseName();
            QgsVectorLayer *layer = new QgsVectorLayer(trimmedPath, layerName, "ogr");
            if (layer->isValid())
            {
                associatedLayers[layerName] = layer;
                logger->info("成功加载关联图层[{}]: {} ({}个要素)", layerIndex, layerName.toStdString(), layer->featureCount());
                layerIndex++;
            }
            else
            {
                logger->warn("无法加载关联图层: {}", trimmedPath.toStdString());
                delete layer;
            }
        }
        if (associatedLayers.isEmpty())
        {
            logger->error("没有成功加载任何关联图层");
            if (mergedSymbolLayer != symbolLayers.first())
                delete mergedSymbolLayer;
            qDeleteAll(symbolLayers);
            spdlog::drop(logger->name());
            return SE_ERROR_CORRUPT_DATA;
        }

        // 5. 执行检查
        logger->info("开始执行检查...");
        QList<AnnotationCheckResult> results = algorithm.checkSymbolDirection(
            mergedSymbolLayer, associatedLayers, rule);
        logger->info("检查完成，发现 {} 个问题", results.size());

        // 6. 生成JSON报告
        QMap<QString, QString> statistics;
        QString jsonString = generateJsonReport(
            results, statistics, "209", "符号方向性检查", logger);

        // 7. 写入JSON文件
        SE_Error jsonResult = writeJsonReportToFile(jsonString, szOutputJsonPath, logger);
        if (jsonResult != SE_ERROR_NONE)
        {
            if (mergedSymbolLayer != symbolLayers.first())
                delete mergedSymbolLayer;
            qDeleteAll(symbolLayers);
            qDeleteAll(associatedLayers);
            spdlog::drop(logger->name());
            return jsonResult;
        }

        // 8. 写入问题图层
        if (!results.isEmpty())
        {
            SE_Error layerResult = writeSymbolDirectionProblemLayer(
                results,
                mergedSymbolLayer,
                rule.symbolIdField,
                rule.symbolType != "道路代码" && rule.symbolType != "渠流向",
                szOutputCheckResultLayerPath,
                logger);
            if (layerResult != SE_ERROR_NONE)
            {
                logger->error("写入问题图层失败");
                if (mergedSymbolLayer != symbolLayers.first())
                    delete mergedSymbolLayer;
                qDeleteAll(symbolLayers);
                qDeleteAll(associatedLayers);
                spdlog::drop(logger->name());
                return layerResult;
            }
        }

        // 9. 清理资源
        if (mergedSymbolLayer != symbolLayers.first())
            delete mergedSymbolLayer;
        qDeleteAll(symbolLayers);
        qDeleteAll(associatedLayers);

        logger->info("符号方向性检查执行完成");
        spdlog::drop(logger->name());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("执行过程中发生异常: {}", e.what());
        spdlog::drop(logger->name());
        return SE_ERROR_FAILURE;
    }
}

// 新增：为符号方向性检查（ID=209）导出问题图层
static SE_Error writeSymbolDirectionProblemLayer(
    const QList<AnnotationCheckResult> &results,
    QgsVectorLayer *symbolLayer,
    const QString &symbolIdField,
    bool useSymbolIds,
    const char *szOutputCheckResultLayerPath,
    std::shared_ptr<spdlog::logger> logger)
{
    if (results.isEmpty())
    {
        return SE_ERROR_NONE;
    }

    try
    {
        // 1. 准备输出图层
        QString outputPath = QString::fromUtf8(szOutputCheckResultLayerPath);
        QFileInfo outputInfo(outputPath);
        QDir outputDir = outputInfo.dir();
        if (!outputDir.exists() && !outputDir.mkpath("."))
        {
            logger->error("无法创建输出目录: {}", outputDir.absolutePath().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 2. 定义输出字段
        QgsFields outputFields = symbolLayer->fields(); // 保留原始字段
        outputFields.append(QgsField("error_type", QVariant::String, "String", 50));
        outputFields.append(QgsField("error_msg", QVariant::String, "String", 254));
        outputFields.append(QgsField("actual_val", QVariant::String, "String", 100));
        outputFields.append(QgsField("expect_val", QVariant::String, "String", 100));

        // 3. 创建写入器
        QgsVectorFileWriter writer(
            outputPath, "UTF-8", outputFields, QgsWkbTypes::Point,
            symbolLayer->crs(), "ESRI Shapefile");

        if (writer.hasError() != QgsVectorFileWriter::NoError)
        {
            logger->error("创建问题图层失败: {}", writer.errorMessage().toStdString());
            return SE_ERROR_WRITE_FILE_FAILED;
        }

        // 4. 按一次检查结果聚合为一个错误点
        int symbolIdIdx = symbolLayer->fields().indexOf(symbolIdField);
        if (useSymbolIds && symbolIdIdx < 0)
        {
            logger->error("符号方向检查结果图层缺少 {} 字段", symbolIdField.toStdString());
            return SE_ERROR_CORRUPT_DATA;
        }

        QMap<QString, QList<AnnotationCheckResult>> groupedResults;
        for (const auto &result : results)
        {
            groupedResults[result.featureId].append(result);
        }

        QMap<QString, QgsAttributes> sourceAttributes;
        QgsFeatureIterator it = symbolLayer->getFeatures();
        QgsFeature feature;
        while (it.nextFeature(feature))
        {
            QString currentId = useSymbolIds
                                    ? feature.attribute(symbolIdIdx).toString()
                                    : QString::number(feature.id());
            if (groupedResults.contains(currentId) && !sourceAttributes.contains(currentId))
                sourceAttributes.insert(currentId, feature.attributes());
        }

        // 5. 为每个错误结果写入一个点要素
        int addedCount = 0;
        for (auto it = groupedResults.constBegin(); it != groupedResults.constEnd(); ++it)
        {
            const QList<AnnotationCheckResult> &featureErrors = it.value();
            if (featureErrors.isEmpty())
                continue;

            const AnnotationCheckResult &firstError = featureErrors.first();
            QgsFeature outFeature(outputFields);
            outFeature.setGeometry(QgsGeometry::fromPointXY(
                QgsPointXY(firstError.position.x(), firstError.position.y())));

            QgsAttributes attrs = sourceAttributes.value(it.key());
            while (attrs.size() < symbolLayer->fields().size())
            {
                attrs.append(QVariant());
            }
            attrs.append("DIRECTION_ERROR");
            attrs.append(firstError.errorMessage);
            attrs.append(firstError.actualValue);
            attrs.append(firstError.expectedValue);

            outFeature.setAttributes(attrs);
            if (writer.addFeature(outFeature))
            {
                addedCount++;
            }
        }

        logger->info("成功导出 {} 个问题点到: {}", addedCount, outputPath.toStdString());
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        logger->error("导出符号方向问题图层时发生异常: {}", e.what());
        return SE_ERROR_WRITE_FILE_FAILED;
    }
}
