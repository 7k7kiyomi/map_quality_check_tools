#include "se_annotation_content_check_algorithm.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsGeometry.h>
#include <QgsPointXY.h>
#include <QgsFields.h>
#include <QgsSpatialIndex.h>
#include <QSet>

SE_AnnotationContentCheckAlgorithm::SE_AnnotationContentCheckAlgorithm(std::shared_ptr<spdlog::logger> logger)
    : m_logger(logger)
{
}

SE_AnnotationContentCheckAlgorithm::~SE_AnnotationContentCheckAlgorithm()
{
}

bool SE_AnnotationContentCheckAlgorithm::loadRules(const QString &schemaPath, double mapScale)
{
    m_mapScale = mapScale;

    if (schemaPath.isEmpty())
    {
        m_logger->warn("[211] 规则文件路径为空，使用默认参数（最小间距=2mm, 最大间距=5mm）");
        return true;
    }

    QFile file(schemaPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        m_logger->warn("[211] 无法打开规则文件: {}，使用默认参数", schemaPath.toStdString());
        return true; // 非致命错误，使用默认值
    }

    QXmlStreamReader xml(&file);
    bool found211 = false;
    QMap<QString, QString> rules;

    while (!xml.atEnd() && !xml.hasError())
    {
        xml.readNext();
        if (xml.isStartElement() && xml.name().toString() == "check_item")
        {
            // 搜索 id=213 的 check_item
            QString id;
            while (!(xml.isEndElement() && xml.name().toString() == "check_item"))
            {
                xml.readNext();
                if (xml.isStartElement())
                {
                    if (xml.name().toString() == "id")
                        id = xml.readElementText().trimmed();
                }
            }
            if (id == "213")
            {
                found211 = true;
                break; // 找到后退出XML读取，只解析需要的规则
            }
        }
    }

    if (!found211)
    {
        m_logger->info("[213] 未在规则文件中找到id=213的check_item，使用默认参数");
        file.close();
        return true; // 非致命
    }

    // 重新读取，定位到211的check_item
    file.seek(0);
    QXmlStreamReader xml2(&file);
    bool in211 = false;

    while (!xml2.atEnd() && !xml2.hasError())
    {
        xml2.readNext();
        if (xml2.isStartElement())
        {
            if (xml2.name().toString() == "check_item")
            {
                if (in211) break; // 已经退出211
                // 直接在当前reader中读取id子元素（QXmlStreamReader不可复制）
                while (!xml2.atEnd())
                {
                    xml2.readNext();
                    if (xml2.isStartElement() && xml2.name().toString() == "id")
                    {
                        QString id = xml2.readElementText().trimmed();
                        if (id == "213")
                            in211 = true;
                        break;
                    }
                    if (xml2.isEndElement() && xml2.name().toString() == "check_item")
                        break;
                }
            }

            if (in211 && xml2.name().toString() == "rule")
            {
                QString ruleName, ruleValue;
                while (!(xml2.isEndElement() && xml2.name().toString() == "rule"))
                {
                    xml2.readNext();
                    if (xml2.isStartElement())
                    {
                        if (xml2.name().toString() == "name")
                            ruleName = xml2.readElementText().trimmed();
                        else if (xml2.name().toString() == "value")
                            ruleValue = xml2.readElementText().trimmed();
                    }
                }
                if (!ruleName.isEmpty())
                    rules[ruleName] = ruleValue;
            }

            if (in211 && xml2.isEndElement() && xml2.name().toString() == "check_item")
                break;
        }
    }

    file.close();

    // 应用规则
    if (rules.contains("最小间距"))
    {
        bool ok;
        double val = rules["最小间距"].toDouble(&ok);
        if (ok) m_minDistanceMm = val;
    }
    if (rules.contains("最大间距"))
    {
        bool ok;
        double val = rules["最大间距"].toDouble(&ok);
        if (ok) m_maxDistanceMm = val;
    }

    m_logger->info("[211] 规则加载完成: 最小间距={}mm, 最大间距={}mm", m_minDistanceMm, m_maxDistanceMm);
    return true;
}

// 判断是否为纯通用类别词（与200算法中isGenericCategoryOnlyPartial逻辑一致）
static bool isGenericCategoryWord(const QString &text)
{
    if (text.isEmpty()) return false;

    // 去除括号和数字（大部分情况下单字注记不会有这些，但安全起见）
    QString cleaned = text;
    // 去除括号内容
    QRegExp bracketRx("\\([^)]*\\)");
    cleaned.replace(bracketRx, "");
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty()) return false;

    static const QSet<QString> genericWords = {
        "高速公路", "高速", "公路", "铁路", "道路", "大道", "街道", "路",
        "河流", "河", "江", "湖", "海", "沟", "渠", "干渠", "支渠", "总干渠", "水库",
        "山", "峰", "大桥", "桥", "隧道", "机场", "港", "站",
        "省", "市", "县", "区", "乡", "镇", "村"};
    return genericWords.contains(cleaned);
}

QList<AnnotationCheckResult> SE_AnnotationContentCheckAlgorithm::checkContent(
    QgsVectorLayer *matchResultLayer,
    QgsVectorLayer *gdbLayer)
{
    QList<AnnotationCheckResult> results;

    if (!matchResultLayer || !matchResultLayer->isValid())
    {
        m_logger->error("[211] 匹配结果图层无效");
        return results;
    }

    // ---------- 字段索引 ----------
    const QgsFields &fields = matchResultLayer->fields();
    int idxMatchStatus = fields.indexOf("match_stat");
    int idxTextConte   = fields.indexOf("text_conte");
    int idxAssocName   = fields.indexOf("assoc_name");
    int idxSymbolId    = fields.indexOf("symbol_id");
    int idxMatchScore  = fields.indexOf("match_scor");
    int idxGdbFid      = fields.indexOf("gdb_fid");

    if (idxMatchStatus < 0 || idxTextConte < 0 || idxAssocName < 0)
    {
        m_logger->error("[211] 匹配结果图层缺少必要字段 (match_stat/text_conte/assoc_name)");
        return results;
    }

    // ---------- GDB图层索引（可选） ----------
    int gdbNameIdx = -1;
    if (gdbLayer && gdbLayer->isValid())
    {
        gdbNameIdx = gdbLayer->fields().indexOf("NAME");
        if (gdbNameIdx < 0)
            gdbNameIdx = gdbLayer->fields().indexOf("name");
        if (gdbNameIdx < 0)
            m_logger->warn("[211] GDB图层无NAME字段，将跳过间距检查");
    }

    // ---------- 第一遍：收集数据 ----------
    struct AnnoRow {
        qint64 fid;
        QString matchStatus;
        QString textConte;
        QString assocName;
        QString symbolId;
        double matchScore;
        QgsPointXY position;
        qint64 gdbFid;
    };
    QList<AnnoRow> rows;
    QSet<qint64> seenFids;

    QgsFeature feat;
    QgsFeatureIterator it = matchResultLayer->getFeatures();
    while (it.nextFeature(feat))
    {
        QString matchStat = feat.attribute(idxMatchStatus).toString();
        // 只关注非完全匹配的行
        if (matchStat == "FULLY_MATCHED")
            continue;

        QString textConte = feat.attribute(idxTextConte).toString().trimmed();
        QString assocName = feat.attribute(idxAssocName).toString().trimmed();

        // 跳过无关联名称的（空间兜底注记、GDB名称缺失-疑似等）
        if (assocName.isEmpty())
            continue;
        if (assocName == QString::fromUtf8("【GDB名称缺失-疑似】"))
            continue;

        qint64 fid = feat.id();
        if (seenFids.contains(fid))
            continue;
        seenFids.insert(fid);

        QgsGeometry geom = feat.geometry();
        QgsPointXY pt;
        if (!geom.isNull())
        {
            if (geom.type() == QgsWkbTypes::PointGeometry)
                pt = geom.asPoint();
            else
                pt = geom.centroid().asPoint();
        }

        AnnoRow row;
        row.fid = fid;
        row.matchStatus = matchStat;
        row.textConte = textConte;
        row.assocName = assocName;
        row.symbolId = feat.attribute(idxSymbolId).toString();
        row.matchScore = feat.attribute(idxMatchScore).toDouble();
        row.position = pt;
        row.gdbFid = (idxGdbFid >= 0) ? feat.attribute(idxGdbFid).toLongLong() : -1;
        rows.append(row);
    }

    m_logger->info("[211] 从匹配结果中读取 {} 个非完全匹配记录", rows.size());

    if (rows.isEmpty())
        return results;

    // ---------- 辅助：剥掉 [UNMATCHED_FID:XXX] 后缀 ----------
    auto stripFidSuffix = [](const QString &name) -> QString {
        int idx = name.indexOf(" [UNMATCHED_FID:");
        if (idx >= 0) return name.left(idx);
        return name;
    };

    // ---------- 第二遍：按源名称（基名）分组 ----------
    // 每个组：baseName → {行索引列表}
    QMap<QString, QList<int>> sourceGroups;
    for (int i = 0; i < rows.size(); ++i)
    {
        QString baseName = stripFidSuffix(rows[i].assocName);
        sourceGroups[baseName].append(i);
    }

    m_logger->info("[211] 按源名称分组：{} 个源名称", sourceGroups.size());

    // ---------- 第三遍：检查每个源名称组 ----------
    int filteredCount = 0;
    int spacerIssueCount = 0;

    for (auto itg = sourceGroups.constBegin(); itg != sourceGroups.constEnd(); ++itg)
    {
        const QString &baseName = itg.key();
        const QList<int> &indices = itg.value();

        // 收集该组内所有字符
        QStringList allChars;
        for (int idx : indices)
        {
            QString tc = rows[idx].textConte;
            if (!tc.isEmpty())
                allChars.append(tc);
        }

        // ---- 通用类别词过滤 ----
        // 检查全组是否都由纯通用类别词构成
        bool allGeneric = true;
        bool hasRealMatch = false;
        for (const QString &ch : allChars)
        {
            if (ch == baseName)
            {
                hasRealMatch = true;
                break;
            }
            if (!isGenericCategoryWord(ch))
            {
                allGeneric = false;
                break;
            }
        }
        if (hasRealMatch)
            continue; // 注记恰好等于源名称，保护
        if (allGeneric)
        {
            filteredCount += indices.size();
            continue; // 全组通用词，过滤
        }

        // ---- 生成结果项 ----
        for (int idx : indices)
        {
            const AnnoRow &row = rows[idx];
            AnnotationCheckResult result;
            result.featureId = row.symbolId.isEmpty() ? QString::number(row.fid) : row.symbolId;
            result.errorType = AnnotationErrorType::FontError;
            result.actualValue = row.textConte;
            result.expectedValue = row.assocName;
            result.position = QPointF(row.position.x(), row.position.y());

            // 构建错误消息
            if (row.matchStatus == "UNMATCHED")
            {
                result.errorMessage = QString::fromUtf8("【注记不匹配】注记内容 '%1' 与源名称 '%2' 不匹配")
                    .arg(row.textConte, baseName);
            }
            else // PARTIALLY_MATCHED
            {
                result.errorMessage = QString::fromUtf8("【注记内容错误】注记内容 '%1' 与源名称 '%2' 不完全匹配")
                    .arg(row.textConte, baseName);
            }

            results.append(result);
        }

        // ---- 间距检查（如果有GDB图层） ----
        if (gdbLayer && gdbLayer->isValid() && gdbNameIdx >= 0)
        {
            double minDistMap = mmToMapUnits(m_minDistanceMm);
            double maxDistMap = mmToMapUnits(m_maxDistanceMm);

            for (int idx : indices)
            {
                const AnnoRow &row = rows[idx];
                if (row.gdbFid < 0)
                    continue;

                // 查找GDB要素
                QgsFeature gdbFeat;
                QgsFeatureIterator gdbIt = gdbLayer->getFeatures(
                    QString("FID = %1").arg(row.gdbFid));
                if (!gdbIt.nextFeature(gdbFeat))
                    continue;

                QgsGeometry gdbGeom = gdbFeat.geometry();
                if (gdbGeom.isNull())
                    continue;

                QgsGeometry annoGeom = QgsGeometry::fromPointXY(row.position);
                double dist = annoGeom.distance(gdbGeom);

                if (dist < minDistMap)
                {
                    AnnotationCheckResult sr;
                    sr.featureId = row.symbolId.isEmpty() ? QString::number(row.fid) : row.symbolId;
                    sr.errorType = AnnotationErrorType::SizeError;
                    sr.actualValue = QString::number(dist, 'f', 2);
                    sr.expectedValue = QString("%1-%2").arg(m_minDistanceMm).arg(m_maxDistanceMm);
                    sr.position = QPointF(row.position.x(), row.position.y());
                    sr.errorMessage = QString::fromUtf8("【间距过小】注记'%1'与GDB要素'%2'间距%.2f米，小于最小允许间距%.2f米")
                        .arg(row.textConte, baseName).arg(dist).arg(minDistMap);
                    results.append(sr);
                    spacerIssueCount++;
                }
                else if (dist > maxDistMap)
                {
                    AnnotationCheckResult sr;
                    sr.featureId = row.symbolId.isEmpty() ? QString::number(row.fid) : row.symbolId;
                    sr.errorType = AnnotationErrorType::SizeError;
                    sr.actualValue = QString::number(dist, 'f', 2);
                    sr.expectedValue = QString("%1-%2").arg(m_minDistanceMm).arg(m_maxDistanceMm);
                    sr.position = QPointF(row.position.x(), row.position.y());
                    sr.errorMessage = QString::fromUtf8("【间距过大】注记'%1'与GDB要素'%2'间距%.2f米，超过最大允许间距%.2f米")
                        .arg(row.textConte, baseName).arg(dist).arg(maxDistMap);
                    results.append(sr);
                    spacerIssueCount++;
                }
            }
        }
    }

    m_logger->info("[211] 检查完成: 内容问题 {} 条, 间距问题 {} 条, 已过滤通用词 {} 条",
        results.size() - spacerIssueCount, spacerIssueCount, filteredCount);

    return results;
}
