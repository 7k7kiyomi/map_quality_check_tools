#include "se_feature_matching.h"
#include <QgsVectorLayer.h>
#include <QgsFeature.h>
#include <QgsFields.h>
#include <QgsGeometry.h>
#include <QgsSpatialIndex.h>
#include <QgsFeatureRequest.h>
#include <QgsVectorFileWriter.h>
#include <qgsmessagelog.h>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDir>
#include <QgsUnitTypes.h> // 需要包含这个头文件
#include <QgsLineString.h>
#include <QgsMultiLineString.h>
#include <QStack>
#include <QQueue>
#include <functional>
#include <limits>
#include <cmath>
#include <numeric> // 新增

// 新增：用于携带合并后的GDB几何及其原始FID列表的结构体
struct MergedGdbGroup
{
    QgsGeometry geometry;
    QList<qint64> fids;
};

FeatureMatcher &FeatureMatcher::instance()
{
    static FeatureMatcher inst;
    return inst;
}

void FeatureMatcher::clear()
{
    qDeleteAll(m_tempLayers);
    m_tempLayers.clear();
}

// 归一化：去空格、转小写
static inline QString normToken(QString s)
{
    s.replace(" ", "");
    s = s.trimmed().toLower();
    return s;
}

// 辅助结构，用于返回匹配结果
struct PartialMatchResult
{
    bool isMatch = false;
    double score = 0.0;
};

// 分层属性过滤
static bool matchesLevelValue(const QString &levelValue, const QSet<QString> &levelFilters)
{
    if (levelFilters.isEmpty())
        return true;

    const QString normValue = normToken(levelValue);

    for (const QString &filter : levelFilters)
    {
        if (filter.endsWith('*'))
        {
            const QString prefix = filter.left(filter.length() - 1);
            if (normValue.startsWith(prefix))
                return true;
        }
        else if (normValue == filter)
        {
            return true;
        }
    }
    return false;
}

// 新增：去除字符串外层配对括号（支持多种括号并递归去除）
static QString stripOuterBrackets(const QString &s)
{
    QString t = s.trimmed();
    if (t.length() < 2)
        return t;

    static const QMap<QChar, QChar> pairs = {
        {QChar('('), QChar(')')}, {QChar('（'), QChar('）')}, {QChar('['), QChar(']')}, {QChar('【'), QChar('】')}, {QChar('{'), QChar('}')}, {QChar('<'), QChar('>')}, {QChar('「'), QChar('」')}, {QChar('『'), QChar('』')}};

    bool changed = true;
    while (changed && t.length() >= 2)
    {
        changed = false;
        QChar first = t.at(0);
        QChar last = t.at(t.length() - 1);
        if (pairs.contains(first) && pairs.value(first) == last)
        {
            t = t.mid(1, t.length() - 2).trimmed();
            changed = true;
        }
    }
    return t;
}

// === 新增：数字等价与规范化函数（中文数字 <-> 阿拉伯数字） ===
static QChar toNormalizedDigitChar(QChar ch)
{
    // 如果已经是阿拉伯数字，直接返回
    if (ch.isDigit())
        return ch;

    // 常用中文数字映射（可按需扩展）
    static const QMap<QChar, QChar> cn2digit = {
        {QChar(u'零'), QChar('0')},
        {QChar(u'一'), QChar('1')},
        {QChar(u'二'), QChar('2')},
        {QChar(u'两'), QChar('2')},
        {QChar(u'三'), QChar('3')},
        {QChar(u'四'), QChar('4')},
        {QChar(u'五'), QChar('5')},
        {QChar(u'六'), QChar('6')},
        {QChar(u'七'), QChar('7')},
        {QChar(u'八'), QChar('8')},
        {QChar(u'九'), QChar('9')}};

    if (cn2digit.contains(ch))
        return cn2digit[ch];

    return ch; // 非数字字符，原样返回
}

static QString normalizeNumberChars(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (QChar ch : s)
        out.append(toNormalizedDigitChar(ch));
    return out;
}

// 判断 name 中是否包含与 singleChar 等价的字符（考虑中/阿数字互通）
// singleCharStr 应为长度1的字符串
static bool nameContainsCharEquivalent(const QString &name, const QString &singleCharStr)
{
    if (singleCharStr.isEmpty())
        return false;
    // 兼容外层括号
    QString strippedName = stripOuterBrackets(name);
    QString strippedChar = stripOuterBrackets(singleCharStr);
    QString normName = normalizeNumberChars(strippedName);
    QString normChar = normalizeNumberChars(strippedChar);
    return normName.contains(normChar);
}

// 在多字注记预处理时，找出 mergedGdbGeometriesByName 中是否有等价名称（数值等价）
static QString findNormalizedMatchedMergedName(const QMap<QString, QList<MergedGdbGroup>> &map, const QString &text)
{
    QString normText = normalizeNumberChars(stripOuterBrackets(text));
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
    {
        if (normalizeNumberChars(stripOuterBrackets(it.key())) == normText)
            return it.key();
    }
    return QString();
}

// 辅助函数：地理名称词元化（一个简化的实现）
static QStringList tokenizeGeoName(const QString &name)
{
    // 定义常见的地理后缀和方向词
    static const QStringList suffixes = {
        "水库", "干渠", "支渠", "总干渠", "河", "江", "湖", "海", "沟", "渠", "山", "峰", "岭",
        "省", "市", "县", "区", "乡", "镇", "村", "街道", "路", "大道",
        "东", "南", "西", "北", "中", "上", "下", "左", "右", "前", "后"};

    QString tempName = name;
    QStringList tokens;

    // 从后向前贪心匹配后缀
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const QString &suffix : suffixes)
        {
            if (tempName.endsWith(suffix))
            {
                tokens.prepend(suffix);
                tempName = tempName.left(tempName.length() - suffix.length());
                changed = true;
                break;
            }
        }
    }

    if (!tempName.isEmpty())
    {
        tokens.prepend(tempName);
    }

    return tokens;
}

// 仅有通用类别词不能证明注记与具体地名相关，例如“高速”不能匹配“连霍高速”。
static bool isGenericCategoryOnlyPartial(const QString &annoText, const QString &gdbText)
{
    QString normalizedAnno = normalizeNumberChars(stripOuterBrackets(annoText)).trimmed();
    QString normalizedGdb = normalizeNumberChars(stripOuterBrackets(gdbText)).trimmed();
    if (normalizedAnno.isEmpty() || normalizedAnno == normalizedGdb)
        return false;

    static const QSet<QString> genericSuffixes = {
        "高速公路", "高速", "公路", "铁路", "道路", "大道", "街道", "路",
        "河流", "河", "江", "湖", "海", "沟", "渠", "干渠", "支渠", "总干渠", "水库",
        "山", "峰", "大桥", "桥", "隧道", "机场", "港", "站",
        "省", "市", "县", "区", "乡", "镇", "村"};
    return genericSuffixes.contains(normalizedAnno);
}

static double jaroWinklerSimilarity(const QString &s1, const QString &s2)
{
    if (s1.isEmpty() && s2.isEmpty())
        return 1.0;
    if (s1.isEmpty() || s2.isEmpty())
        return 0.0;

    int len1 = s1.length();
    int len2 = s2.length();
    int match_distance = std::max(len1, len2) / 2 - 1;

    QVector<bool> s1_matches(len1, false);
    QVector<bool> s2_matches(len2, false);
    int matches = 0;
    for (int i = 0; i < len1; ++i)
    {
        int start = std::max(0, i - match_distance);
        int end = std::min(i + match_distance + 1, len2);
        for (int j = start; j < end; ++j)
        {
            if (!s2_matches[j] && s1[i] == s2[j])
            {
                s1_matches[i] = true;
                s2_matches[j] = true;
                matches++;
                break;
            }
        }
    }

    if (matches == 0)
        return 0.0;

    double t = 0;
    int k = 0;
    for (int i = 0; i < len1; ++i)
    {
        if (s1_matches[i])
        {
            while (!s2_matches[k])
                k++;
            if (s1[i] != s2[k])
                t += 0.5;
            k++;
        }
    }

    double jaro = ((double)matches / len1 + (double)matches / len2 + (double)(matches - t) / matches) / 3.0;

    // Winkler modification
    int prefix = 0;
    for (int i = 0; i < std::min(len1, len2) && i < 4; ++i)
    {
        if (s1[i] == s2[i])
            prefix++;
        else
            break;
    }

    return jaro + prefix * 0.1 * (1.0 - jaro);
}

static PartialMatchResult checkPartialMatch(const QString &annoText, const QString &gdbText)
{
    if (annoText.isEmpty() || gdbText.isEmpty())
        return {false, 0.0};

    // 先去掉外层括号，去除空格，再做数字规范化
    QString annoStripped = stripOuterBrackets(annoText).replace(" ", "");
    QString gdbStripped = stripOuterBrackets(gdbText).replace(" ", "");

    QString normAnno = normalizeNumberChars(annoStripped);
    QString normGdb = normalizeNumberChars(gdbStripped);

    // 规则1 & 2 保持不变
    if (normAnno == normGdb)
        return {true, 1.0};
    if (isGenericCategoryOnlyPartial(normAnno, normGdb))
        return {false, 0.0};

    if ((normAnno.endsWith(normGdb) || normAnno.startsWith(normGdb)) && normGdb.length() >= 2)
    {
        return {true, 0.85 + 0.15 * (static_cast<double>(normGdb.length()) / normAnno.length())};
    }
    if ((normGdb.endsWith(normAnno) || normGdb.startsWith(normAnno)) && normAnno.length() >= 2)
    {
        return {true, 0.85 + 0.15 * (static_cast<double>(normAnno.length()) / normGdb.length())};
    }

    // 规则3: 字符集高重合度匹配
    QSet<QChar> annoChars = QSet<QChar>(normAnno.begin(), normAnno.end());
    QSet<QChar> gdbChars = QSet<QChar>(normGdb.begin(), normGdb.end());

    int minLen = qMin(annoChars.size(), gdbChars.size());
    if (minLen <= 1)
        return {false, 0.0};

    QSet<QChar> intersection = annoChars;
    intersection.intersect(gdbChars);
    int intersectionSize = intersection.size();

    double overlapRatio = static_cast<double>(intersectionSize) / minLen;
    double gdbCoverageRatio = static_cast<double>(intersectionSize) / gdbChars.size();
    double annoCoverageRatio = static_cast<double>(intersectionSize) / annoChars.size();

    const double MIN_OVERLAP_RATIO = 0.50;
    const double MIN_COVERAGE_RATIO = 0.50;

    if (overlapRatio >= MIN_OVERLAP_RATIO &&
        gdbCoverageRatio >= MIN_COVERAGE_RATIO &&
        annoCoverageRatio >= MIN_COVERAGE_RATIO)
    {
        // 字符集检查通过，现在引入 Jaro-Winkler 作为顺序性检查
        double sequenceScore = jaroWinklerSimilarity(normAnno, normGdb);

        // 如果顺序非常差（例如<0.5），则拒绝匹配
        if (sequenceScore < 0.5)
        {
            return {false, 0.0};
        }

        // 最终分数是字符集得分和顺序性得分的加权平均
        double charsetScore = (overlapRatio + gdbCoverageRatio + annoCoverageRatio) / 3.0;
        double finalScore = charsetScore * 0.7 + sequenceScore * 0.3;

        return {true, qMin(finalScore, 0.8)};
    }

    return {false, 0.0};
}

QgsVectorLayer *FeatureMatcher::loadLayer(const QString &path, QString &errMsg)
{
    QFileInfo fi(path);
    if (!fi.exists())
    {
        errMsg = QString("图层文件不存在: %1").arg(path);
        qWarning() << errMsg;
        return nullptr;
    }

    QString baseName = fi.completeBaseName();
    // 恢复为简单的加载方式，与其它接口一致
    QgsVectorLayer *layer = new QgsVectorLayer(path, baseName, "ogr");

    if (!layer->isValid())
    {
        errMsg = QString("图层加载失败: %1").arg(path);
        qWarning() << errMsg << "Error:" << layer->error().message();
        delete layer;
        return nullptr;
    }

    qDebug() << "Successfully loaded layer:" << path << "with" << layer->featureCount() << "features.";
    m_tempLayers.append(layer);
    return layer;
}

// 获取注记中心点(从Polygon外接框) - 修复版
static QgsPointXY getAnnoCenter(const QgsGeometry &geom)
{
    if (geom.isEmpty())
        return QgsPointXY();

    // 无论什么几何类型,统一获取质心
    QgsGeometry centroid = geom.centroid();
    if (centroid.isEmpty())
        return geom.boundingBox().center();

    return centroid.asPoint();
}

// 构建符号聚合索引: symbol_id -> 所有组成部分的FID列表
static QMap<QString, QList<qint64>> buildSymbolIndex(QgsVectorLayer *actualLayer)
{
    QMap<QString, QList<qint64>> index;

    int symIdIdx = actualLayer->fields().indexOf("symbol_id");
    if (symIdIdx < 0)
        return index; // 不是符号图层

    QgsFeatureIterator it = actualLayer->getFeatures();
    QgsFeature feat;

    while (it.nextFeature(feat))
    {
        QString symId = feat.attribute(symIdIdx).toString();
        if (!symId.isEmpty())
            index[symId].append(feat.id());
    }

    return index;
}

// 计算符号的中心点(基于所有组成部分的质心)
static QgsPointXY calcSymbolCenter(QgsVectorLayer *actualLayer, const QList<qint64> &fids)
{
    double sumX = 0, sumY = 0;
    int count = 0;

    for (qint64 fid : fids)
    {
        QgsFeature feat = actualLayer->getFeature(fid);
        if (!feat.isValid())
            continue;

        QgsGeometry geom = feat.geometry();
        if (geom.isEmpty())
            continue;

        QgsPointXY pt = geom.centroid().asPoint();
        sumX += pt.x();
        sumY += pt.y();
        count++;
    }

    if (count == 0)
        return QgsPointXY();

    return QgsPointXY(sumX / count, sumY / count);
}

ActualMatchResult FeatureMatcher::matchActualBySpatial(
    QgsVectorLayer *gdbLayer, qint64 gdbFid,
    QgsVectorLayer *actualLayer, const QString &mapScale,
    // --- 接收预计算的缓存 ---
    const QMap<QString, QList<qint64>> &symbolIdToFidsMap,
    const QgsSpatialIndex &symbolComponentIndex,
    const QMap<QgsFeatureId, QString> &componentFidToSymbolIdMap,
    const QMap<QString, QgsPointXY> &symbolIdToCenterMap)
{
    ActualMatchResult result; // <-- 使用新的返回类型
    QList<qint64> out;
    if (!gdbLayer || !actualLayer || gdbFid < 0)
        return result;

    QgsFeature gdbFeature = gdbLayer->getFeature(gdbFid);
    if (!gdbFeature.isValid())
        return result;
    QgsGeometry gdbGeom = gdbFeature.geometry();
    if (gdbGeom.isEmpty())
        return result;

    bool isSymbolLayer = !symbolIdToFidsMap.isEmpty();

    if (isSymbolLayer)
    {
        // **【性能优化】**: 所有索引和缓存都由外部传入，这里只做计算和查询
        QgsPointXY gdbCenter;
        QgsWkbTypes::Type type = gdbGeom.wkbType();
        if (type == QgsWkbTypes::Point || type == QgsWkbTypes::MultiPoint)
        {
            gdbCenter = gdbGeom.centroid().asPoint();
        }
        else
        {
            gdbCenter = gdbGeom.centroid().asPoint();
        }

        bool ok;
        double scaleDenominator = mapScale.toDouble(&ok); // 直接用分母字符串
        if (!ok || scaleDenominator <= 0)
        {
            scaleDenominator = 50000;
        }
        const double searchRadiusInMapUnits = (5.0 / 1000.0) * scaleDenominator;

        QgsRectangle searchBox(gdbCenter, gdbCenter);
        searchBox.grow(searchRadiusInMapUnits);

        QList<QgsFeatureId> candidateComponentFids = symbolComponentIndex.intersects(searchBox);

        if (candidateComponentFids.isEmpty())
            return result;

        double minDistance = std::numeric_limits<double>::max();
        QString bestSymbolId;
        QSet<QString> candidateSymbolIds;
        for (QgsFeatureId fid : candidateComponentFids)
        {
            candidateSymbolIds.insert(componentFidToSymbolIdMap.value(fid));
        }

        for (const QString &symbolId : candidateSymbolIds)
        {
            if (symbolIdToCenterMap.contains(symbolId))
            {
                double dist = gdbCenter.distance(symbolIdToCenterMap.value(symbolId));
                if (dist < minDistance)
                {
                    minDistance = dist;
                    bestSymbolId = symbolId;
                }
            }
        }
        if (!bestSymbolId.isEmpty() && minDistance < searchRadiusInMapUnits)
        {
            result.fids = symbolIdToFidsMap.value(bestSymbolId);
            result.symbolId = bestSymbolId;
            return result;
        }
        else
        {
            return result;
        }
    }
    else // 线/面图层的匹配逻辑 (不变)
    {
        QgsSpatialIndex sidx(actualLayer->getFeatures());
        QgsRectangle bbox = gdbGeom.boundingBox();
        bbox.grow(10.0);
        QList<qint64> candidates = sidx.intersects(bbox);
        for (qint64 fid : candidates)
        {
            QgsFeature af = actualLayer->getFeature(fid);
            if (!af.isValid() || af.geometry().isEmpty())
                continue;
            if (gdbGeom.intersects(af.geometry()) || gdbGeom.distance(af.geometry()) < 10.0)
            {
                out.append(fid);
            }
        }
        result.fids = out; // out 是您之前计算的 fid 列表
    }
    return result;
}

// =============================================================================================
// === 辅助函数：GDB数据预处理
// =============================================================================================

/**
 * @brief 预处理GDB图层数据，进行级别过滤并构建索引
 * @param gdbLayer GDB图层
 * @param gdbLabelIdx 名称字段索引
 * @param gdbLevelIdx 级别字段索引（-1表示无级别字段）
 * @param levelValues 允许的级别值集合
 * @param nameToFids 输出：名称到FID列表的映射
 * @param gdbGeomCache 输出：FID到几何的缓存
 * @param gdbLevelCache 输出：FID到级别的缓存
 */
static void preprocessGdbLayer(
    QgsVectorLayer *gdbLayer,
    int gdbLabelIdx,
    int gdbLevelIdx,
    const QSet<QString> &levelValues,
    QMap<QString, QList<qint64>> &nameToFids,
    QMap<qint64, QgsGeometry> &gdbGeomCache,
    QMap<qint64, QString> &gdbLevelCache)
{
    QgsFeatureIterator gdbIt = gdbLayer->getFeatures();
    QgsFeature gdbFeat;
    while (gdbIt.nextFeature(gdbFeat))
    {
        // 级别过滤
        QString levelValue = gdbLevelIdx >= 0 ? gdbFeat.attribute(gdbLevelIdx).toString() : QString();
        if (gdbLevelIdx >= 0 && !matchesLevelValue(levelValue, levelValues))
        {
            continue;
        }

        QString name = gdbFeat.attribute(gdbLabelIdx).toString().trimmed().replace(" ", "");
        gdbGeomCache[gdbFeat.id()] = gdbFeat.geometry();
        gdbLevelCache[gdbFeat.id()] = levelValue;

        // 只将非空名称的要素加入索引
        if (!name.isEmpty())
            nameToFids[name].append(gdbFeat.id());
    }

    qDebug() << "信息: GDB图层预处理完成。有名称的要素组数:" << nameToFids.size();
}

/**
 * @brief 收集NULL名称的要素（用于回退匹配）
 * @param gdbLayer GDB图层
 * @param gdbLabelIdx 名称字段索引
 * @param gdbLevelIdx 级别字段索引
 * @param levelValues 允许的级别值集合
 * @param nullNameIndex 输出：空间索引
 * @param nullGeomCache 输出：几何缓存
 * @param nullNameFids 输出：FID列表
 */
static void collectNullNameFeatures(
    QgsVectorLayer *gdbLayer,
    int gdbLabelIdx,
    int gdbLevelIdx,
    const QSet<QString> &levelValues,
    QgsSpatialIndex &nullNameIndex,
    QMap<qint64, QgsGeometry> &nullGeomCache,
    QList<qint64> &nullNameFids)
{
    QgsFeatureIterator itNull = gdbLayer->getFeatures();
    QgsFeature f;
    while (itNull.nextFeature(f))
    {
        QString nm = f.attribute(gdbLabelIdx).toString().trimmed();
        if (!nm.isEmpty())
            continue;

        QString lv = (gdbLevelIdx >= 0) ? f.attribute(gdbLevelIdx).toString() : QString();
        if (gdbLevelIdx >= 0 && !matchesLevelValue(lv, levelValues))
            continue;

        if (!f.geometry().isEmpty())
        {
            nullNameIndex.addFeature(f);
            nullGeomCache.insert(f.id(), f.geometry());
            nullNameFids.append(f.id());
        }
    }
    qDebug() << "信息: 为回退匹配准备的 NULL 名称要素数量:" << nullNameFids.size();
}

/**
 * @brief 按空间连通性对同名要素分组并合并几何
 * @param nameToFids 名称到FID列表的映射
 * @param gdbGeomCache FID到几何的缓存
 * @param scaleDenominator 比例尺分母
 * @return 名称到合并几何列表的映射
 */
static QMap<QString, QList<MergedGdbGroup>> mergeGdbGeometriesByProximity(
    const QMap<QString, QList<qint64>> &nameToFids,
    const QMap<qint64, QgsGeometry> &gdbGeomCache,
    double scaleDenominator)
{
    QMap<QString, QList<MergedGdbGroup>> mergedGdbGroupsByName;

    for (auto it = nameToFids.constBegin(); it != nameToFids.constEnd(); ++it)
    {
        const QList<qint64> &fids = it.value();
        if (fids.isEmpty())
            continue;

        // 按空间邻近性将同名要素分组
        QList<QList<qint64>> spatialGroups;
        QSet<qint64> processed;

        for (qint64 seedFid : fids)
        {
            if (processed.contains(seedFid))
                continue;

            QList<qint64> group;
            QQueue<qint64> queue;
            queue.enqueue(seedFid);
            processed.insert(seedFid);

            while (!queue.isEmpty())
            {
                qint64 currentFid = queue.dequeue();
                group.append(currentFid);

                // 查找与当前要素空间相邻的同名要素（距离阈值：图上5mm）
                double proximityThreshold = (5.0 / 1000.0) * scaleDenominator;
                for (qint64 otherFid : fids)
                {
                    if (processed.contains(otherFid))
                        continue;

                    if (gdbGeomCache[currentFid].distance(gdbGeomCache[otherFid]) < proximityThreshold)
                    {
                        queue.enqueue(otherFid);
                        processed.insert(otherFid);
                    }
                }
            }

            spatialGroups.append(group);
        }

        // 为每个空间分组合并几何
        for (const QList<qint64> &groupFids : spatialGroups)
        {
            QList<QgsGeometry> geomsToMerge;
            for (qint64 fid : groupFids)
                geomsToMerge.append(gdbGeomCache[fid]);

            QgsGeometry mergedGeom = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(geomsToMerge));

            if (!mergedGeom.isEmpty())
            {
                qDebug().nospace() << "  [Debug] Merged GDB Name: \"" << it.key()
                                   << "\", FIDs in group: " << groupFids
                                   << ", IsSinglePart: " << (!mergedGeom.isMultipart() ? "Yes" : "No")
                                   << ", GeomType: " << QgsWkbTypes::displayString(mergedGeom.wkbType());

                // 【核心修改】将合并后的几何与对应的FID列表一起存储
                mergedGdbGroupsByName[it.key()].append({mergedGeom, groupFids});
            }
        }
    }

    qDebug() << "信息: 按名称和空间邻近合并完成。总地名组数:"
             << std::accumulate(mergedGdbGroupsByName.begin(), mergedGdbGroupsByName.end(), 0,
                                [](int sum, const QList<MergedGdbGroup> &list)
                                { return sum + list.size(); });

    return mergedGdbGroupsByName;
}

// =============================================================================================
// === 辅助函数：注记预处理
// =============================================================================================

/**
 * @brief 预处理注记图层，直接处理多字注记并构建单字注记索引
 * @param annoLayer 注记图层
 * @param annoTextIdx "text_conte"字段索引
 * @param annoFontSizeIdx "font_size"字段索引
 * @param mergedGdbGeometriesByName 合并后的GDB几何映射
 * @param nameToFids GDB名称到FID的映射
 * @param allAnnos 输出：所有注记信息
 * @param result 输出：匹配结果（用于直接匹配的多字注记）
 * @param globalGroupId 输入输出：全局组ID计数器
 */
static void preprocessAnnotations(
    QgsVectorLayer *gdbLayer,
    QgsVectorLayer *actualLayer,
    const QString &mapScale,
    QgsVectorLayer *annoLayer,
    int annoTextIdx,
    int annoFontSizeIdx,
    int annoFontFamilyIdx,
    int gdbLabelIdx,
    const QMap<QString, QList<MergedGdbGroup>> &mergedGdbGroupsByName, // 【改回】接收新的数据结构
    const QMap<QString, QList<qint64>> &nameToFids,
    const QMap<qint64, QgsGeometry> &gdbGeomCache,
    QMap<qint64, AnnotationInfo> &allAnnos,
    MatchResult &result,
    qint64 &globalGroupId,
    // --- 符号缓存 ---
    const QMap<QString, QList<qint64>> &symbolIdToFidsMap_cache,
    const QgsSpatialIndex &symbolComponentIndex_cache,
    const QMap<QgsFeatureId, QString> &componentFidToSymbolIdMap_cache,
    const QMap<QString, QgsPointXY> &symbolIdToCenterMap_cache,
    const QSet<qint64> &excludedAnnoFids)
{
    // --- 步骤 1: 填充 allAnnos 缓存 (不变) ---
    QgsFeatureIterator annoIt_fillCache = annoLayer->getFeatures();
    QgsFeature annoFeat_fillCache;
    while (annoIt_fillCache.nextFeature(annoFeat_fillCache))
    {
        if (excludedAnnoFids.contains(annoFeat_fillCache.id()))
            continue;
        QString text = annoFeat_fillCache.attribute(annoTextIdx).toString().trimmed().replace(" ", "");
        if (text.isEmpty() || annoFeat_fillCache.geometry().isEmpty())
            continue;
        QString fontFamily = (annoFontFamilyIdx >= 0) ? annoFeat_fillCache.attribute(annoFontFamilyIdx).toString().trimmed() : QString();
        AnnotationInfo info{annoFeat_fillCache.id(), text, annoFeat_fillCache.geometry(), annoFeat_fillCache.geometry().centroid().asPoint(), annoFeat_fillCache.attribute(annoFontSizeIdx).toDouble(), fontFamily};
        allAnnos.insert(annoFeat_fillCache.id(), info);
    }
    qDebug() << "信息: 注记信息缓存完成。总注记数:" << allAnnos.size();

    // --- 步骤 2:【新】解耦的第一阶段：完成所有“注记-GDB”的初步匹配 ---
    qDebug() << "--- 开始进行所有多字注记与GDB要素的初步匹配 ---";

    QList<FeatureMatchLink> initialLinks; // 存储所有初步的“注记-GDB”匹配
    QSet<qint64> processedAnnoFids;       // 跟踪已完成初步匹配的注记

    // 原代码：const double maxMatchRadius = (20.0 / 1000.0) * mapScale.split(':').last().toDouble();
    // 替换为：
    bool ok;
    double scaleDenominator = mapScale.toDouble(&ok);
    const double maxMatchRadius = (20.0 / 1000.0) * scaleDenominator;

    for (const auto &info : allAnnos)
    {
        if (info.text.length() <= 1)
            continue;

        // 为当前注记，在所有GDB候选中找到最佳匹配 (无论点线面)
        qint64 bestGdbFid = -1;
        QString bestGdbName;
        MatchStatus bestStatus = NOT_MATCHED;
        double bestScore = -1.0;

        for (auto it = nameToFids.constBegin(); it != nameToFids.constEnd(); ++it)
        {
            PartialMatchResult pmResult = checkPartialMatch(info.text, it.key());
            if (!pmResult.isMatch)
                continue;

            for (qint64 fid : it.value())
            {
                const QgsGeometry &gdbGeom = gdbGeomCache.value(fid);
                if (gdbGeom.isEmpty())
                    continue;

                double distance = info.geom.distance(gdbGeom);
                if (distance < maxMatchRadius)
                {
                    double score = pmResult.score + (1.0 - distance / maxMatchRadius);
                    if (score > bestScore)
                    {
                        bestScore = score;
                        bestGdbFid = fid;
                        bestGdbName = it.key();
                        bestStatus = (pmResult.score > 0.99) ? FULLY_MATCHED : PARTIALLY_MATCHED;
                    }
                }
            }
        }

        if (bestGdbFid != -1)
        {
            // 创建一个基础的 FeatureMatchLink
            FeatureMatchLink link;
            link.status = bestStatus;
            link.matchScore = bestScore;
            link.annoFid = info.fid;
            link.gdbFid = bestGdbFid;
            link.annoText = info.text;
            link.assoc_name = bestGdbName;

            initialLinks.append(link);
            processedAnnoFids.insert(info.fid);
        }
    }
    qDebug() << "--- 初步匹配完成，共找到" << initialLinks.size() << "个“注记-GDB”对 ---";

    // --- 步骤 3:【新】对所有与“点”要素关联的初步匹配，进行符号的优先级分配 ---
    qDebug() << "--- 开始对点要素的匹配进行符号优先级分配 ---";

    struct PotentialSymbolMatch
    {
        int initialLinkIndex; // 指向 initialLinks 列表中的索引
        QString symbolId;
        double finalScore;
        bool operator<(const PotentialSymbolMatch &other) const { return finalScore > other.finalScore; }
    };
    QList<PotentialSymbolMatch> allPotentialSymbolMatches;

    const double symbolSearchRadius = (5.0 / 1000.0) * scaleDenominator;

    for (int i = 0; i < initialLinks.size(); ++i)
    {
        const FeatureMatchLink &link = initialLinks[i];
        const QgsGeometry &gdbGeom = gdbGeomCache.value(link.gdbFid);

        // 只处理关联到GDB点要素的link
        if (QgsWkbTypes::geometryType(gdbGeom.wkbType()) == QgsWkbTypes::PointGeometry)
        {
            QgsPointXY gdbCenter = gdbGeom.centroid().asPoint();
            QgsRectangle searchBox(gdbCenter, gdbCenter);
            searchBox.grow(symbolSearchRadius);

            QList<QgsFeatureId> candidateComponentFids = symbolComponentIndex_cache.intersects(searchBox);
            QSet<QString> candidateSymbolIds;
            for (QgsFeatureId compFid : candidateComponentFids)
            {
                candidateSymbolIds.insert(componentFidToSymbolIdMap_cache.value(compFid));
            }

            for (const QString &symbolId : candidateSymbolIds)
            {
                double dist = gdbCenter.distance(symbolIdToCenterMap_cache.value(symbolId));
                if (dist < symbolSearchRadius)
                {
                    PotentialSymbolMatch psm;
                    psm.initialLinkIndex = i;
                    psm.symbolId = symbolId;
                    double spatialScore = 1.0 - (dist / symbolSearchRadius);
                    psm.finalScore = link.matchScore * 0.7 + spatialScore * 0.3; // 综合“注记-GDB”和“GDB-符号”的分数
                    allPotentialSymbolMatches.append(psm);
                }
            }
        }
    }

    // 全局排序和唯一分配
    std::sort(allPotentialSymbolMatches.begin(), allPotentialSymbolMatches.end());

    QMap<qint64, QString> gdbToSymbolMap; // 存储最终确认的 GDB FID -> Symbol ID 映射
    QSet<QString> assignedSymbolIds;

    for (const PotentialSymbolMatch &psm : allPotentialSymbolMatches)
    {
        qint64 gdbFid = initialLinks[psm.initialLinkIndex].gdbFid;
        if (!gdbToSymbolMap.contains(gdbFid) && !assignedSymbolIds.contains(psm.symbolId))
        {
            gdbToSymbolMap[gdbFid] = psm.symbolId;
            assignedSymbolIds.insert(psm.symbolId);
        }
    }
    qDebug() << "--- 符号优先级分配完成，共确认" << gdbToSymbolMap.size() << "个“GDB-符号”对 ---";

    // --- 步骤 4:【新】遍历所有初步匹配，补充 actual 信息并生成最终结果 ---
    qDebug() << "--- 开始生成最终匹配结果 ---";
    for (const FeatureMatchLink &initialLink : initialLinks)
    {
        globalGroupId++;
        result.annoToGroupMap[initialLink.annoFid] = globalGroupId;

        FeatureMatchLink finalLink = initialLink; // 拷贝基础信息
        const QgsGeometry &gdbGeom = gdbGeomCache.value(finalLink.gdbFid);
        QgsWkbTypes::GeometryType geomType = QgsWkbTypes::geometryType(gdbGeom.wkbType());

        // 【核心修复】根据匹配到的 gdbFid，反向查找其所属的空间分组，并获取正确的 fids 列表
        if (!finalLink.assoc_name.isEmpty() && mergedGdbGroupsByName.contains(finalLink.assoc_name))
        {
            const QList<MergedGdbGroup> &groups = mergedGdbGroupsByName.value(finalLink.assoc_name);
            for (const MergedGdbGroup &group : groups)
            {
                if (group.fids.contains(finalLink.gdbFid))
                {
                    finalLink.gdbFids = group.fids;
                    break; // 找到后立即退出
                }
            }
        }
        // 如果上面的逻辑没找到（例如，对于没有合并过的点要素），则回退到使用 nameToFids
        if (finalLink.gdbFids.isEmpty() && !finalLink.assoc_name.isEmpty())
        {
            finalLink.gdbFids = nameToFids.value(finalLink.assoc_name);
        }

        if (geomType == QgsWkbTypes::PointGeometry)
        {
            // 如果是点，去查找已确认的符号分配
            if (gdbToSymbolMap.contains(finalLink.gdbFid))
            {
                finalLink.symbolId = gdbToSymbolMap.value(finalLink.gdbFid);
                finalLink.actualFids = symbolIdToFidsMap_cache.value(finalLink.symbolId);
            }
            // **【核心修复】**: 如果没找到符号，finalLink.symbolId 和 actualFids 保持为空，但 link 本身依然被保留
            // --- 新增回退逻辑 ---
            else if (actualLayer)
            {
                // 如果在优先级分配中未匹配到，则执行一次独立的最佳空间匹配
                ActualMatchResult fallbackResult = FeatureMatcher::instance().matchActualBySpatial(
                    gdbLayer, finalLink.gdbFid, actualLayer, mapScale,
                    symbolIdToFidsMap_cache, symbolComponentIndex_cache,
                    componentFidToSymbolIdMap_cache, symbolIdToCenterMap_cache);

                if (!fallbackResult.symbolId.isEmpty())
                {
                    finalLink.symbolId = fallbackResult.symbolId;
                    finalLink.actualFids = fallbackResult.fids;
                }
            }
        }
        else if (geomType == QgsWkbTypes::LineGeometry || geomType == QgsWkbTypes::PolygonGeometry)
        {
            // 如果是线或面，执行简单的空间匹配
            ActualMatchResult actualResult = FeatureMatcher::instance().matchActualBySpatial(
                gdbLayer, finalLink.gdbFid, actualLayer, mapScale,
                symbolIdToFidsMap_cache, symbolComponentIndex_cache, // 传递空缓存也可以，因为该函数内部会判断
                componentFidToSymbolIdMap_cache, symbolIdToCenterMap_cache);
            finalLink.actualFids = actualResult.fids;
            finalLink.symbolId = actualResult.symbolId;
        }

        result.links.append(finalLink);
    }
    qDebug() << "--- 所有多字注记处理完成，最终生成" << result.links.size() << "条匹配链接 ---";
}

// =============================================================================================
// === 辅助结构：优先级处理队列
// =============================================================================================

struct GdbProcessItem
{
    QString name;
    int geomIdx;
    QgsGeometry geom;
    int priority; // 优先级分数，越高越优先
};

/**
 * @brief 构建优先级排序的GDB地名处理队列
 * @param mergedGdbGeometriesByName 合并后的GDB几何映射
 * @param allAnnos 所有注记信息
 * @param annoSpatialIndex 注记空间索引
 * @param candidateSearchRadius 搜索半径
 * @param scaleDenominator 比例尺分母
 * @return 优先级排序的处理队列
 */
static QList<GdbProcessItem> buildPriorityQueue(
    const QMap<QString, QList<MergedGdbGroup>> &mergedGdbGroupsByName, // 【修改】接收新的数据结构
    // --- 【修改】直接接收缓存，而不是整个图层 ---
    const QMap<qint64, AnnotationInfo> &allAnnos,
    const QgsSpatialIndex &annoSpatialIndex,
    double candidateSearchRadius,
    double scaleDenominator)
{
    QList<GdbProcessItem> processingQueue;

    for (auto nameIt = mergedGdbGroupsByName.constBegin();
         nameIt != mergedGdbGroupsByName.constEnd(); ++nameIt)
    {
        const QString &gdbName = nameIt.key();
        const QList<MergedGdbGroup> &groupList = nameIt.value(); // 【修改】变量名

        for (int geomIdx = 0; geomIdx < groupList.size(); ++geomIdx)
        {
            const QgsGeometry &gdbGeom = groupList[geomIdx].geometry; // 【修改】从 group 中获取 geometry

            // 【修改1】放宽几何类型限制：允许线、面、点要素
            if (gdbGeom.isEmpty() || gdbName.length() < 2)
                continue;

            QgsWkbTypes::GeometryType geomType = gdbGeom.type();
            if (geomType != QgsWkbTypes::LineGeometry &&
                geomType != QgsWkbTypes::PolygonGeometry &&
                geomType != QgsWkbTypes::PointGeometry)
                continue;

            int priority = 0;

            // 1. 长度权重
            priority += gdbName.length() * 1000;

            // 2. 候选数量精确度权重
            int candidateCount = 0;
            QgsRectangle searchRect = gdbGeom.boundingBox();
            searchRect.grow(candidateSearchRadius);
            QList<qint64> candidateAnnoFids = annoSpatialIndex.intersects(searchRect);

            for (qint64 annoFid : candidateAnnoFids)
            {
                const auto &annoInfo = allAnnos[annoFid];
                // 【放宽】对于线要素，不再严格限制字符必须在GDB名中
                // 这样可以捕获"皇"→"湟"这类缺部首错误
                bool charMatchesForPriority = nameContainsCharEquivalent(gdbName, annoInfo.text);
                bool isSingleCharCandidate = (annoInfo.text.length() == 1);
                if (geomType == QgsWkbTypes::LineGeometry)
                {
                    // 线要素：放宽过滤，允许非GDB名称内的字符
                    isSingleCharCandidate = isSingleCharCandidate;
                }
                else
                {
                    // 面/点要素：保持严格过滤
                    isSingleCharCandidate = isSingleCharCandidate && charMatchesForPriority;
                }
                
                if (isSingleCharCandidate)
                {
                    // 【修改2】根据几何类型调整距离判断
                    bool isCandidate = false;

                    if (geomType == QgsWkbTypes::PolygonGeometry)
                    {
                        // 面要素：注记在面内部或边界附近
                        if (gdbGeom.contains(annoInfo.geom) || gdbGeom.distance(annoInfo.geom) < candidateSearchRadius)
                        {
                            isCandidate = true;
                        }
                    }
                    else if (geomType == QgsWkbTypes::PointGeometry)
                    {
                        // 点要素：注记在点周围一定半径内
                        if (gdbGeom.distance(annoInfo.geom) < candidateSearchRadius)
                        {
                            isCandidate = true;
                        }
                    }
                    else // LineGeometry
                    {
                        // 线要素：保持原有逻辑
                        if (gdbGeom.distance(annoInfo.geom) < candidateSearchRadius)
                        {
                            isCandidate = true;
                        }
                    }

                    if (isCandidate)
                    {
                        candidateCount++;
                    }
                }
            }

            if (candidateCount == gdbName.length())
            {
                priority += 500;
            }
            else if (candidateCount > gdbName.length() * 2)
            {
                priority -= 100;
            }

            // 3. 几何大小权重
            if (geomType == QgsWkbTypes::LineGeometry)
            {
                // 线要素：按长度
                priority += static_cast<int>(gdbGeom.length() / 1000.0);
            }
            else if (geomType == QgsWkbTypes::PolygonGeometry)
            {
                // 面要素：按面积的平方根（模拟线性尺度）
                priority += static_cast<int>(std::sqrt(gdbGeom.area()) / 1000.0);
            }
            else // PointGeometry
            {
                // 点要素：固定权重（较低优先级）
                priority += 10;
            }

            GdbProcessItem item;
            item.name = gdbName;
            item.geomIdx = geomIdx;
            item.geom = gdbGeom;
            item.priority = priority;

            processingQueue.append(item);
        }
    }

    std::sort(processingQueue.begin(), processingQueue.end(),
              [](const GdbProcessItem &a, const GdbProcessItem &b)
              {
                  return a.priority > b.priority;
              });

    qDebug() << "信息: 构建了包含" << processingQueue.size() << "个GDB地名组的处理队列（已按优先级排序）";

    return processingQueue;
}

// =============================================================================================
// === 辅助函数：最终结果生成
// =============================================================================================

/**
 * @brief 根据注记分组生成最终匹配结果
 * @param finalAnnoGroups 最终的注记分组
 * @param allAnnos 所有注记信息
 * @param mergedGdbGeometriesByName 合并后的GDB几何映射
 * @param nameToFids GDB名称到FID的映射
 * @param gdbLayer GDB图层
 * @param actualLayer 实际图层
 * @param annoLayer 注记图层
 * @param annoAngleIdx 角度字段索引
 * @param result 输出：匹配结果
 * @param globalGroupId 输入输出：全局组ID计数器
 */
static void generateFinalMatchResult(
    const QList<QList<qint64>> &finalAnnoGroups,
    const QMap<qint64, AnnotationInfo> &allAnnos,
    const QMap<QString, QList<MergedGdbGroup>> &mergedGdbGroupsByName, // 【修改】接收新的数据结构
    const QMap<QString, QList<qint64>> &nameToFids,
    QgsVectorLayer *gdbLayer,
    QgsVectorLayer *actualLayer,
    QgsVectorLayer *annoLayer,
    int annoAngleIdx,
    const QString &mapScale,
    MatchResult &result,
    qint64 &globalGroupId,
    // --- 【新增】接收符号缓存的参数 ---
    const QMap<QString, QList<qint64>> &symbolIdToFidsMap_cache,
    const QgsSpatialIndex &symbolComponentIndex_cache,
    const QMap<QgsFeatureId, QString> &componentFidToSymbolIdMap_cache,
    const QMap<QString, QgsPointXY> &symbolIdToCenterMap_cache)
{
    for (const auto &groupFids : finalAnnoGroups)
    {
        globalGroupId++;

        QString groupText;
        for (qint64 fid : groupFids)
            groupText += allAnnos[fid].text;

        // 在所有空间分组中查找最佳匹配
        QString bestGdbName;
        const MergedGdbGroup *bestGroup = nullptr; // 【修改】直接保存指向最佳组的指针
        double minDistance = std::numeric_limits<double>::max();

        for (auto nameIt = mergedGdbGroupsByName.constBegin();
             nameIt != mergedGdbGroupsByName.constEnd(); ++nameIt)
        {
            const QString &name = nameIt.key();
            if (name.length() != groupText.length())
                continue;

            // 检查字符集匹配（支持中文数字与阿拉伯数字等价）
            QString normName = normalizeNumberChars(stripOuterBrackets(name));
            QString normGroupText = normalizeNumberChars(stripOuterBrackets(groupText));

            if (normName.length() != normGroupText.length())
                continue;

            QList<QChar> nameChars;
            for (const QChar &ch : normName)
                nameChars.append(ch);
            bool match = true;
            for (const QChar &ch : normGroupText)
            {
                if (!nameChars.removeOne(ch))
                {
                    match = false;
                    break;
                }
            }
            if (!match || !nameChars.isEmpty())
                continue;

            // 在该名称的所有空间分组中，找距离最近的一组
            QgsPointXY groupCenter(0, 0);
            for (qint64 fid : groupFids)
            {
                groupCenter.setX(groupCenter.x() + allAnnos[fid].center.x());
                groupCenter.setY(groupCenter.y() + allAnnos[fid].center.y());
            }
            groupCenter.setX(groupCenter.x() / groupFids.size());
            groupCenter.setY(groupCenter.y() / groupFids.size());

            for (const MergedGdbGroup &group : nameIt.value()) // 【修改】遍历 MergedGdbGroup
            {
                double dist = group.geometry.distance(QgsGeometry::fromPointXY(groupCenter));
                if (dist < minDistance)
                {
                    minDistance = dist;
                    bestGdbName = name;
                    bestGroup = &group; // 【修改】记录最佳组的地址
                }
            }
        }

        if (bestGdbName.isEmpty() || !bestGroup) // 【修改】增加对 bestGroup 的检查
            continue;

        qDebug() << "  成功匹配: 组 " << globalGroupId << " (文本: \"" << groupText << "\") -> GDB地名 \"" << bestGdbName << "\"";

        // 【核心修改】从匹配到的 bestGroup 获取正确的 FID 列表
        const QList<qint64> &originalFids = bestGroup->fids;
        qint64 representativeGdbFid = originalFids.isEmpty() ? -1 : originalFids.first();

        ActualMatchResult actualResult = FeatureMatcher::instance().matchActualBySpatial(
            gdbLayer, representativeGdbFid, actualLayer, mapScale,
            symbolIdToFidsMap_cache, symbolComponentIndex_cache,
            componentFidToSymbolIdMap_cache, symbolIdToCenterMap_cache);
        QList<qint64> actualFids = actualResult.fids;
        QPointF gdbCentroid = QPointF(bestGroup->geometry.centroid().asPoint().x(), bestGroup->geometry.centroid().asPoint().y());

        for (qint64 annoFid : groupFids)
        {
            result.annoToGroupMap[annoFid] = globalGroupId;
            FeatureMatchLink newLink;
            newLink.annoFid = annoFid;
            newLink.gdbFid = representativeGdbFid;
            newLink.gdbFids = originalFids;
            newLink.actualFids = actualFids;
            newLink.symbolId = actualResult.symbolId;
            newLink.annoText = allAnnos[annoFid].text;
            newLink.annoPosition = QPointF(allAnnos[annoFid].center.x(), allAnnos[annoFid].center.y());
            newLink.gdbCentroid = gdbCentroid;
            newLink.assoc_name = bestGdbName;
            // 比较注记组文字与GDB名称（去空格后），检测顺序错误
            QString normCmpGroup = groupText;
            normCmpGroup.replace(" ", "");
            QString normCmpGdb = bestGdbName;
            normCmpGdb.replace(" ", "");

            if (normCmpGroup == normCmpGdb)
            {
                newLink.status = FULLY_MATCHED;
                newLink.matchScore = 1.0;
            }
            else
            {
                // 检查是否为精确反向（注记沿河流反向排列，属正常情况）
                int len = normCmpGroup.length();
                bool isReversed = (len == normCmpGdb.length());
                if (isReversed)
                {
                    for (int i = 0; i < len; ++i)
                    {
                        if (normCmpGroup[i] != normCmpGdb[len - 1 - i])
                        {
                            isReversed = false;
                            break;
                        }
                    }
                }
                if (isReversed)
                {
                    // 完全反向排列：注记沿河流反方向，不算错误
                    newLink.status = FULLY_MATCHED;
                    newLink.matchScore = 1.0;
                }
                else
                {
                    // 中间乱序（如"明先峡河" vs "先明峡河"），属内容错误
                    // 设为PARTIALLY_MATCHED使210规则能检出顺序错误
                    newLink.status = PARTIALLY_MATCHED;
                    newLink.matchScore = 0.95;
                    qDebug() << "    > [OrderMismatch] Group text \"" << groupText
                             << "\" differs from GDB name \"" << bestGdbName
                             << "\", marking as PARTIALLY_MATCHED for 210 detection.";
                }
            }
            if (annoAngleIdx >= 0)
            {
                QVariant angle = annoLayer->getFeature(annoFid).attribute(annoAngleIdx);
                if (angle.isValid())
                    newLink.annoAngle = angle.toDouble();
            }
            result.links.append(newLink);
        }
    }
}

// =============================================================================================
// === 重构后的主流程（大幅简化）
// =============================================================================================

MatchResult FeatureMatcher::matchSingleRule(
    const QString &standardLayer, const QString &gdbLayerPath,
    const QString &levelAttr, const QString &levelValuesCsv,
    const QString &actualLayerPath, const QString &actualAnnoLayerPath,
    const QString &gdbLabelField, const QString &mapScale, QString &errMsg,
    const QSet<qint64> &excludedAnnoFids)
{
    MatchResult result;
    qint64 globalGroupId = 0;

    // 【新增】用于延迟决策的数据结构
    // Key: GDB地名
    // Value: 该地名在匹配失败时，收集到的所有可用候选注记的FID列表
    QMap<QString, QList<qint64>> potentialPartialMatches;
    struct DeferredPartialGroup
    {
        QString gdbName;
        int geomIdx;
        QList<qint64> fids;
        double matchScore;
        bool requireZeroOutput;
    };
    QList<DeferredPartialGroup> deferredPartialGroups;

    // --- 辅助数据结构：投影注记 ---（提前声明，供 UnmatchedGdbWithCandidates 引用）
    struct ProjectedAnno
    {
        qint64 fid;
        double distanceAlong;
        QString text;
    };

    // 【新增】跟踪在主匹配中失败、但仍有投影候选注记的GDB条目
    // 这些条目即使拼不出完整名称，也应输出给210做进一步检查
    struct UnmatchedGdbWithCandidates
    {
        QString gdbName;
        int geomIdx;
        QList<ProjectedAnno> candidates;
    };
    QList<UnmatchedGdbWithCandidates> unmatchedGdbEntries;

    // =============================================================================================
    // === 步骤 0: 加载图层和准备工作
    // =============================================================================================
    QgsVectorLayer *gdbLayer = loadLayer(gdbLayerPath, errMsg);
    QgsVectorLayer *actualLayer = nullptr;
    if (!actualLayerPath.isEmpty())
    {
        actualLayer = loadLayer(actualLayerPath, errMsg);
    }
    QgsVectorLayer *annoLayer = loadLayer(actualAnnoLayerPath, errMsg);

    if (!gdbLayer || /* actualLayer can be null */ !annoLayer)
    {
        errMsg = "错误: 加载一个或多个必需图层失败。";
        qWarning() << errMsg;
        return result;
    }

    int gdbLabelIdx = gdbLayer->fields().indexOf(gdbLabelField);
    int gdbLevelIdx = levelAttr.isEmpty() ? -1 : gdbLayer->fields().indexOf(levelAttr);
    int annoTextIdx = annoLayer->fields().indexOf("text_conte");
    int annoFontSizeIdx = annoLayer->fields().indexOf("font_size");
    int annoFontFamilyIdx = annoLayer->fields().indexOf("font_famil");
    int annoAngleIdx = annoLayer->fields().indexOf("shear_angl");

    if (gdbLabelIdx < 0 || annoTextIdx < 0 || annoFontSizeIdx < 0 || (!levelAttr.isEmpty() && gdbLevelIdx < 0))
    {
        errMsg = QString("错误: 必需字段缺失。").arg(gdbLabelField, levelAttr);
        qWarning() << errMsg;
        return result;
    }

    bool ok;
    double scaleDenominator = mapScale.toDouble(&ok); // 直接用分母字符串
    if (!ok || scaleDenominator <= 0)
    {
        errMsg = "错误: 无效的地图比例尺格式。";
        qWarning() << errMsg;
        return result;
    }

    const double candidateSearchRadius = (20.0 / 1000.0) * scaleDenominator;
    const double endpointTolerance = (1.0 / 1000.0) * scaleDenominator;

    qDebug() << "比例尺分母:" << scaleDenominator
             << "| 搜索半径:" << candidateSearchRadius
             << "| 端点容差:" << endpointTolerance;

    // =============================================================================================
    // === 【性能优化】步骤 0.5: 预计算所有需要重复使用的索引和缓存
    // =============================================================================================
    qDebug() << "\n--- 开始预计算所有索引和缓存 ---";

    // --- 缓存 1: 符号图层 (actualLayer) 的相关索引 ---
    QMap<QString, QList<qint64>> symbolIdToFidsMap_cache;
    QgsSpatialIndex symbolComponentIndex_cache;
    QMap<QgsFeatureId, QString> componentFidToSymbolIdMap_cache;
    QMap<QString, QgsPointXY> symbolIdToCenterMap_cache;

    if (actualLayer)
    {
        symbolIdToFidsMap_cache = buildSymbolIndex(actualLayer);
        if (!symbolIdToFidsMap_cache.isEmpty())
        {
            qDebug() << "信息: 检测到符号图层，开始构建符号索引...";
            for (auto it = symbolIdToFidsMap_cache.constBegin(); it != symbolIdToFidsMap_cache.constEnd(); ++it)
            {
                const QString &symbolId = it.key();
                const QList<qint64> &componentFids = it.value();
                symbolIdToCenterMap_cache[symbolId] = calcSymbolCenter(actualLayer, componentFids);
                for (qint64 fid : componentFids)
                {
                    QgsFeature componentFeature = actualLayer->getFeature(fid);
                    if (componentFeature.isValid())
                    {
                        symbolComponentIndex_cache.addFeature(componentFeature);
                        componentFidToSymbolIdMap_cache[fid] = symbolId;
                    }
                }
            }
            qDebug() << "信息: 符号索引构建完成。";
        }
    }

    // =============================================================================================
    // === 步骤 1: GDB 数据预处理（调用独立函数）
    // =============================================================================================
    QSet<QString> levelValues;
    if (!levelValuesCsv.trimmed().isEmpty())
    {
        const QStringList parts = levelValuesCsv.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
        for (const QString &p : parts)
            levelValues.insert(normToken(p));
    }
    QMap<QString, QList<qint64>> nameToFids;
    QMap<qint64, QgsGeometry> gdbGeomCache;
    QMap<qint64, QString> gdbLevelCache;

    preprocessGdbLayer(gdbLayer, gdbLabelIdx, gdbLevelIdx, levelValues, nameToFids, gdbGeomCache, gdbLevelCache);

    QgsSpatialIndex nullNameIndex;
    QMap<qint64, QgsGeometry> nullGeomCache;
    QList<qint64> nullNameFids;
    collectNullNameFeatures(gdbLayer, gdbLabelIdx, gdbLevelIdx, levelValues, nullNameIndex, nullGeomCache, nullNameFids);

    // 【修改】变量类型
    QMap<QString, QList<MergedGdbGroup>> mergedGdbGroupsByName = mergeGdbGeometriesByProximity(nameToFids, gdbGeomCache, scaleDenominator);

    // =============================================================================================
    // === 步骤 2: 注记预处理（调用独立函数）
    // =============================================================================================
    QMap<qint64, AnnotationInfo> allAnnos;
    preprocessAnnotations(gdbLayer, actualLayer, mapScale, annoLayer, annoTextIdx, annoFontSizeIdx,
                          annoFontFamilyIdx,
                          gdbLabelIdx, mergedGdbGroupsByName, nameToFids, gdbGeomCache, allAnnos, // 【修改】传入 mergedGdbGroupsByName
                          result, globalGroupId,
                          // -- 传递缓存 --
                          symbolIdToFidsMap_cache, symbolComponentIndex_cache,
                          componentFidToSymbolIdMap_cache, symbolIdToCenterMap_cache, excludedAnnoFids);

    QgsSpatialIndex annoSpatialIndex;
    for (const auto &info : allAnnos)
    {
        QgsFeature f;
        f.setId(info.fid);
        f.setGeometry(info.geom);
        annoSpatialIndex.addFeature(f);
    }

    // =============================================================================================
    // === 步骤 3: 构建优先级队列（调用独立函数）
    // =============================================================================================
    QList<GdbProcessItem> processingQueue = buildPriorityQueue(
        mergedGdbGroupsByName,
        allAnnos,
        annoSpatialIndex,
        candidateSearchRadius,
        scaleDenominator);

    // =============================================================================================
    // === 步骤 4: 基于序列验证的注记追踪分组（保留在主流程，因为逻辑复杂）
    // =============================================================================================
    QList<QList<qint64>> finalAnnoGroups;
    QSet<qint64> groupedAnnoFids; // 全局已分组注记集合

    // 使用优先级队列进行处理
    for (const GdbProcessItem &item : processingQueue)
    {
        const QString &gdbName = item.name;
        const QgsGeometry &gdbGeom = item.geom;
        const int geomIdx = item.geomIdx;

        // 获取该地名的所有空间分组数量（用于日志）
        int totalGroupsForName = mergedGdbGroupsByName[gdbName].size();

        // --- 辅助函数：构建投影注记列表 ---
        auto buildProjected = [&](const QgsGeometry &lineGeom, double nearRadius, double distFactor) -> QList<ProjectedAnno>
        {
            QList<ProjectedAnno> list;
            QSet<qint64> addedFids;

            if (lineGeom.isEmpty())
                return list;

            // 【修改3】判断几何类型
            QgsWkbTypes::GeometryType geomType = lineGeom.type();

            // 【修复】将MultiLineString的parts按端点连通性排序，
            // 确保lineLocatePoint返回的距离对应真实地理顺序而非unaryUnion的存储顺序。
            auto sortMultiLineParts = [](const QgsGeometry &geom) -> QgsGeometry
            {
                if (geom.isEmpty() || !geom.isMultipart())
                    return geom;
                if (geom.type() != QgsWkbTypes::LineGeometry)
                    return geom;

                QVector<QgsPolylineXY> parts = geom.asMultiPolyline();
                if (parts.size() <= 1)
                    return geom;

                struct PartInfo {
                    int index;
                    QgsPolylineXY poly;
                    QgsPointXY startPt;
                    QgsPointXY endPt;
                };
                QList<PartInfo> infos;
                for (int i = 0; i < parts.size(); i++)
                {
                    if (parts[i].isEmpty())
                        continue;
                    PartInfo info;
                    info.index = i;
                    info.poly = parts[i];
                    info.startPt = parts[i].first();
                    info.endPt = parts[i].last();
                    infos.append(info);
                }
                if (infos.size() <= 1)
                    return geom;

                // 改进的贪心链式排序：尝试每个part作为起点构建链，
                // 选端点总间隙最小的那条链（避免从中间段开始导致分支错位）。
                auto buildChainFrom = [&](int startIdx) -> QPair<QList<PartInfo>, double>
                {
                    QList<PartInfo> chain;
                    QSet<int> used;
                    PartInfo first = infos[startIdx];
                    chain.append(first);
                    used.insert(first.index);
                    double totalGap = 0.0;

                    while (chain.size() < infos.size())
                    {
                        const QgsPointXY &chainEnd = chain.last().endPt;
                        int bestIdx = -1;
                        double bestDist = std::numeric_limits<double>::max();
                        bool needFlip = false;

                        for (int i = 0; i < infos.size(); i++)
                        {
                            if (used.contains(infos[i].index))
                                continue;
                            double d1 = chainEnd.distance(infos[i].startPt);
                            double d2 = chainEnd.distance(infos[i].endPt);
                            if (d1 < bestDist) { bestDist = d1; bestIdx = i; needFlip = false; }
                            if (d2 < bestDist) { bestDist = d2; bestIdx = i; needFlip = true; }
                        }

                        if (bestIdx < 0)
                            break;

                        totalGap += bestDist;
                        PartInfo next = infos[bestIdx];
                        if (needFlip)
                        {
                            QgsPolylineXY rev;
                            rev.reserve(next.poly.size());
                            for (int j = next.poly.size() - 1; j >= 0; j--)
                                rev.append(next.poly[j]);
                            next.poly = rev;
                            std::swap(next.startPt, next.endPt);
                        }
                        chain.append(next);
                        used.insert(next.index);
                    }
                    return qMakePair(chain, totalGap);
                };

                // 从每个part出发构建链，选总间隙最小的
                QList<PartInfo> bestChain;
                double bestTotalGap = std::numeric_limits<double>::max();
                for (int start = 0; start < infos.size(); start++)
                {
                    auto result = buildChainFrom(start);
                    if (result.second < bestTotalGap)
                    {
                        bestTotalGap = result.second;
                        bestChain = result.first;
                    }
                }

                QgsMultiLineString *mls = new QgsMultiLineString();
                for (const auto &part : bestChain)
                    mls->addGeometry(new QgsLineString(part.poly));
                return QgsGeometry(mls);
            };

            // 对MultiLineString先排序parts再用于投影
            QgsGeometry sortedLineGeom = lineGeom;
            if (lineGeom.isMultipart() && geomType == QgsWkbTypes::LineGeometry)
            {
                sortedLineGeom = sortMultiLineParts(lineGeom);
                qDebug() << "    [PartSort] Reordered MultiLineString parts from"
                         << lineGeom.asMultiPolyline().size() << "parts"
                         << "(total length:" << lineGeom.length() << "m)";
            }

            auto processPart = [&](const QgsGeometry &partGeom, double lengthOffset)
            {
                if (partGeom.isEmpty())
                    return;

                for (const auto &annoInfo : allAnnos)
                {
                    // 检查本次构建中的重复和全局已分组
                    if (addedFids.contains(annoInfo.fid) || groupedAnnoFids.contains(annoInfo.fid))
                    {
                        if (annoInfo.fid == 42 || annoInfo.fid == 44)
                            qDebug() << "    [DEBUG FID" << annoInfo.fid << "] SKIPPED in processPart for GDB" << gdbName
                                     << "| anno text:" << annoInfo.text
                                     << "| in addedFids:" << addedFids.contains(annoInfo.fid)
                                     << "| in groupedAnnoFids:" << groupedAnnoFids.contains(annoInfo.fid);
                        continue;
                    }

                    if (annoInfo.text.length() == 1)
                    {
                        bool charMatches = nameContainsCharEquivalent(gdbName, annoInfo.text);
                        bool isNearby = false;
                        double distAlong = 0.0;

                        // 【修改4】根据几何类型调整判断逻辑
                        if (geomType == QgsWkbTypes::PolygonGeometry)
                        {
                            // 面要素：仅允许GDB名称内的字符，避免噪声
                            if (!charMatches)
                                continue;
                            if (partGeom.contains(annoInfo.geom) || partGeom.distance(annoInfo.geom) < nearRadius)
                            {
                                isNearby = true;
                                // 对于面要素（如行政区划、山脉等），注记散落在面内，不应投影到边界。
                                // 改用 X 坐标排序，模拟左→右阅读顺序。
                                // 这样"红古区"的三个散字（红在左、区在右）按 X 升序自然排序为 红→古→区
                                // X*1000 作为主键，Y/1000 作为 tiebreaker：
                                //   同 X 时自动按 Y 升序（上→下），兼容垂直排列的注记
                                distAlong = annoInfo.center.x() * 1000.0 + annoInfo.center.y() / 1000.0;
                            }
                        }
                        else if (geomType == QgsWkbTypes::PointGeometry)
                        {
                            // 点要素：仅允许GDB名称内的字符，避免噪声
                            if (!charMatches)
                                continue;
                            if (partGeom.distance(annoInfo.geom) < nearRadius)
                            {
                                isNearby = true;
                                // 对于点要素，使用到点的距离作为排序依据
                                distAlong = partGeom.asPoint().distance(annoInfo.center);
                            }
                        }
                        else // LineGeometry
                        {
                            // 线要素候选池：只收录名称内的字符，排除无关字符避免污染候选池
                            if (!charMatches)
                            {
                                if (annoInfo.fid == 42 || annoInfo.fid == 44)
                                    qDebug() << "    [DEBUG FID" << annoInfo.fid << "] LineGeometry charMatches=false for GDB" << gdbName << ", anno text:" << annoInfo.text;
                                continue;
                            }

                            if (partGeom.distance(annoInfo.geom) < nearRadius)
                            {
                                distAlong = partGeom.lineLocatePoint(QgsGeometry::fromPointXY(annoInfo.center));
                                QgsPointXY projPt = partGeom.interpolate(distAlong).asPoint();
                                double d = annoInfo.center.distance(projPt);

                                double maxDistToLine = ((annoInfo.fontSize * 0.352778 * distFactor) / 1000.0) * scaleDenominator;
                                if (d < maxDistToLine)
                                {
                                    isNearby = true;
                                }
                            }

                            if (annoInfo.fid == 42 || annoInfo.fid == 44)
                            {
                                double rawDist = partGeom.distance(annoInfo.geom);
                                qDebug() << "    [DEBUG FID" << annoInfo.fid << "] GDB:" << gdbName
                                         << "| charMatches:" << charMatches
                                         << "| rawDist:" << rawDist
                                         << "| nearRadius:" << nearRadius
                                         << "| isNearby:" << isNearby
                                         << "| in addedFids:" << addedFids.contains(annoInfo.fid)
                                         << "| in groupedAnnoFids:" << groupedAnnoFids.contains(annoInfo.fid);
                            }
                        }

                        if (isNearby)
                        {
                            list.append(ProjectedAnno{annoInfo.fid, distAlong + lengthOffset, annoInfo.text});
                            addedFids.insert(annoInfo.fid);
                        }
                    }
                }
            };

            if (!sortedLineGeom.isMultipart())
            {
                processPart(sortedLineGeom, 0.0);
            }
            else
            {
                // 线要素MultiLineString：parts已通过sortMultiLineParts按端点连通性排序，
                // 整体传给lineLocatePoint即可得到正确的地理沿线距离。
                // 面要素MultiPolygon：各part独立处理，用X坐标全局排序。
                if (geomType == QgsWkbTypes::LineGeometry)
                {
                    processPart(sortedLineGeom, 0.0);
                }
                else if (geomType == QgsWkbTypes::PolygonGeometry)
                {
                    // 面要素用 X 坐标排序，不需要用累积面积作为偏移量
                    const auto multi = sortedLineGeom.asMultiPolygon();
                    for (const QgsPolygonXY &polygon : multi)
                    {
                        if (polygon.isEmpty())
                            continue;
                        QgsGeometry partGeom = QgsGeometry::fromPolygonXY(polygon);
                        processPart(partGeom, 0.0);
                    }
                }
                else // PointGeometry (通常不会是多点，但为完整性处理)
                {
                    processPart(sortedLineGeom, 0.0);
                }
            }

            std::sort(list.begin(), list.end(), [](const ProjectedAnno &a, const ProjectedAnno &b)
                      { return a.distanceAlong < b.distanceAlong; });
            return list;
        };

        // --- 核心函数：在候选注记中查找有效的字符序列组合 ---
        std::function<void(const QList<qint64> &, const QList<ProjectedAnno> &, const QString &, QList<QList<qint64>> &)> findGroupsOnLine =
            [&](const QList<qint64> &alreadyGrouped, const QList<ProjectedAnno> &proj,
                const QString &targetName, QList<QList<qint64>> &outGroups)
        {
            QSet<qint64> groupedSet(alreadyGrouped.begin(), alreadyGrouped.end());
            int nameLen = targetName.length();

            if (proj.size() < nameLen)
                return;

            // 使用回溯算法查找所有字符组合
            QMap<QChar, QList<qint64>> charToFids;
            for (const auto &p : proj)
            {
                if (!groupedSet.contains(p.fid))
                {
                    charToFids[p.text.at(0)].append(p.fid);
                }
            }

            // 检查是否每个必需的字符都有候选
            QStringList missingChars;
            for (const QChar &ch : targetName)
            {
                if (!charToFids.contains(ch))
                    missingChars.append(ch);
            }
            if (!missingChars.isEmpty())
            {
                QStringList availableChars;
                for (auto it = charToFids.constBegin(); it != charToFids.constEnd(); ++it)
                    availableChars.append(it.key());
                qDebug() << "    > [CharMissing] For \"" << targetName
                         << "\", missing chars:" << missingChars.join(",")
                         << "| candidates available:" << availableChars.join(",")
                         << "(total" << proj.size() << "candidates)";
                return;
            }

            QList<QList<qint64>> allCombinations;
            QList<qint64> currentCombination;
            QSet<qint64> usedFidsInCombination;

            std::function<void(int)> findCombinations =
                [&](int charIndex)
            {
                if (charIndex == nameLen)
                {
                    allCombinations.append(currentCombination);
                    return;
                }

                QChar targetChar = targetName.at(charIndex);
                for (qint64 fid : charToFids[targetChar])
                {
                    if (!usedFidsInCombination.contains(fid))
                    {
                        currentCombination.push_back(fid);
                        usedFidsInCombination.insert(fid);

                        findCombinations(charIndex + 1);

                        usedFidsInCombination.remove(fid);
                        currentCombination.pop_back();
                    }
                }
            };

            findCombinations(0);

            if (allCombinations.isEmpty())
                return;

            QMap<qint64, int> projectedOrder;
            for (int i = 0; i < proj.size(); ++i)
                projectedOrder.insert(proj.at(i).fid, i);

            // 从所有组合中迭代选择多个不重叠的最佳组合
            struct CombinationScore
            {
                QList<qint64> sequence;
                double compactness;
                bool operator<(const CombinationScore &other) const { return compactness < other.compactness; }
            };
            QList<CombinationScore> validCombinations;

            // 子步骤 4.1: 预筛选和评分所有组合
            for (const auto &rawSeq : allCombinations)
            {
                QList<qint64> seq = rawSeq;
                // 检查与全局已分组的冲突
                bool conflict = false;
                for (qint64 fid : seq)
                {
                    if (groupedSet.contains(fid))
                    {
                        conflict = true;
                        break;
                    }
                }
                if (conflict)
                    continue;

                // 检查组合的空间顺序 — 防止注记顺序与GDB名称不一致
                {
                    QList<int> sequenceOrder;
                    for (qint64 fid : seq)
                        sequenceOrder.append(projectedOrder.value(fid, -1));

                    bool increasing = true;
                    bool decreasing = true;
                    for (int i = 1; i < sequenceOrder.size(); ++i)
                    {
                        increasing = increasing && sequenceOrder.at(i) > sequenceOrder.at(i - 1);
                        decreasing = decreasing && sequenceOrder.at(i) < sequenceOrder.at(i - 1);
                    }

                    auto minmaxOrder = std::minmax_element(sequenceOrder.constBegin(), sequenceOrder.constEnd());
                    int orderSpan = *minmaxOrder.second - *minmaxOrder.first + 1;

                    // 空间顺序必须单调递增或递减，非单调组合直接拒绝
                    // - 线要素：projectedOrder 基于沿线距离，单调=沿河流/道路方向顺序阅读
                    // - 面要素：projectedOrder 基于 X 坐标，单调=左→右（或右→左）自然阅读顺序
                    // 理由：非单调说明注记空间排列与GDB名称顺序不一致，很可能是不同图层的注记被跨组拼凑，
                    // 强行保留会导致下游210检测也难以区分真实错误与拼凑噪声
                    // 【修改】移除 orderSpan 限制：后续 Step5/6 的距离校验（注记到线平均距离、总跨度上限）
                    // 已充分防止跨层噪声拼凑，span=seq.size()+1 阈值对散字注记（如"碱沟"中间夹"九""支"两字）过严导致误杀
                    if (!increasing && !decreasing)
                    {
                        qDebug() << "    > [OrderReject] Spatial order non-monotonic for \""
                                 << gdbName << "\", rejecting combination (orderSpan="
                                 << orderSpan << ").";
                        continue;
                    }
                }

                // 字体大小和字体族一致性验证
                if (seq.size() > 1)
                {
                    double firstFontSize = allAnnos[seq.first()].fontSize;
                    QString firstFontFamily = allAnnos[seq.first()].fontFamily;
                    bool fontConsistent = true;
                    for (int i = 1; i < seq.size(); ++i)
                    {
                        if (std::abs(allAnnos[seq[i]].fontSize - firstFontSize) > 0.1)
                        {
                            fontConsistent = false;
                            break;
                        }
                        // 【新增】字体族一致性：防止不同河的注记因同字号被拼到一起
                        // 场景："大沙沟"的"大"(fontA)被拼到"太冰沟"的"冰""沟"(fontB)
                        if (!firstFontFamily.isEmpty() && allAnnos[seq[i]].fontFamily != firstFontFamily)
                        {
                            fontConsistent = false;
                            qDebug() << "    > [FontFamilyReject] Font family mismatch for \""
                                     << gdbName << "\": '" << allAnnos[seq.first()].text
                                     << "'(" << firstFontFamily << ") vs '"
                                     << allAnnos[seq[i]].text << "'(" << allAnnos[seq[i]].fontFamily << ")";
                            break;
                        }
                    }
                    if (!fontConsistent)
                        continue;
                }

                // 验证注记到GDB几何的平均距离
                double totalDist = 0.0;
                for (qint64 fid : seq)
                {
                    double dist = gdbGeom.distance(QgsGeometry::fromPointXY(allAnnos[fid].center));
                    totalDist += dist;
                }
                double avgDistToLine = totalDist / seq.size();

                // 计算合理的距离阈值
                double firstFontSize = allAnnos[seq.first()].fontSize;
                double maxReasonableDistance = ((firstFontSize * 0.352778 * 3.5) / 1000.0) * scaleDenominator;

                // 如果平均距离超过合理阈值，说明这些注记不是真正属于这条河流的
                if (avgDistToLine > maxReasonableDistance)
                {
                    qDebug() << "    > Skipped combination for \"" << gdbName
                             << "\" due to excessive average distance to line:"
                             << avgDistToLine << "meters (threshold:" << maxReasonableDistance << ")";
                    continue;
                }

                // 计算紧凑度（用于后续排序）
                QgsRectangle bbox;
                for (qint64 fid : seq)
                {
                    bbox.combineExtentWith(allAnnos[fid].geom.boundingBox());
                }
                double compactness = bbox.width() * bbox.height();

                // 间距一致性验证
                if (seq.size() > 1)
                {
                    QList<ProjectedAnno> sortedSeq;
                    for (const auto &p : proj)
                    {
                        if (seq.contains(p.fid))
                        {
                            sortedSeq.append(p);
                        }
                    }

                    // 【修改6】对于面要素和点要素，跳过间距一致性检查（因为它们没有明确的"沿线方向"）
                    QgsWkbTypes::GeometryType geomType = gdbGeom.type();
                    if (geomType == QgsWkbTypes::LineGeometry && sortedSeq.size() > 1)
                    {
                        QList<double> distances;
                        for (int j = 0; j < sortedSeq.size() - 1; ++j)
                        {
                            distances.append(sortedSeq[j + 1].distanceAlong - sortedSeq[j].distanceAlong);
                        }
                        double avgDist = std::accumulate(distances.begin(), distances.end(), 0.0) / distances.size();

                        bool consistent = true;
                        for (double dist : distances)
                        {
                            double absoluteThreshold = ((allAnnos[seq[0]].fontSize * 0.352778 * 50.0) / 1000.0) * scaleDenominator;
                            if (dist > avgDist * 4.0 && dist > absoluteThreshold)
                            {
                                consistent = false;
                                break;
                            }
                        }
                        if (!consistent)
                            continue;

                        // 总跨度检查：防止相隔数十公里的字被拼成一组
                        // 例如"庄浪河"3个字合理跨度应在~30km内，而非51.8km
                        // maxSpan = nameLen * fontSize(mm) * 15 / 1000 * scaleDenominator
                        double totalSpan = sortedSeq.last().distanceAlong - sortedSeq.first().distanceAlong;
                        double maxSpan = (nameLen * allAnnos[seq[0]].fontSize * 0.352778 * 15.0) / 1000.0 * scaleDenominator;
                        if (totalSpan > maxSpan)
                        {
                            qDebug() << "    > [SpanReject] Total span" << totalSpan << "m exceeds max"
                                     << maxSpan << "m for" << nameLen << "-char name \"" << gdbName
                                     << "\", rejecting.";
                            continue;
                        }
                    }
                }

                CombinationScore scored;
                scored.sequence = seq;
                scored.compactness = compactness;
                validCombinations.append(scored);
            }

            if (validCombinations.isEmpty())
                return;

            // 子步骤 4.2: 按紧凑度排序
            std::sort(validCombinations.begin(), validCombinations.end());

            // 子步骤 4.3: 贪心选择不重叠的组合
            QSet<qint64> localUsedFids;

            for (const CombinationScore &scored : validCombinations)
            {
                // 检查是否与已选择的组合重叠
                bool overlap = false;
                for (qint64 fid : scored.sequence)
                {
                    if (localUsedFids.contains(fid))
                    {
                        overlap = true;
                        break;
                    }
                }

                if (!overlap)
                {
                    // 选择这个组合
                    outGroups.append(scored.sequence);
                    for (qint64 fid : scored.sequence)
                    {
                        localUsedFids.insert(fid);
                        groupedSet.insert(fid);
                    }
                }
            }
        };

        // --- 主匹配流程：初次尝试 ---
        QList<qint64> currentGrouped = groupedAnnoFids.values();
        QList<ProjectedAnno> projectedAnnos = buildProjected(gdbGeom, candidateSearchRadius, 5.0);

        QString debugPrefix = (totalGroupsForName > 1)
                                  ? QString("  [Debug] For GDB \"%1\" (spatial group %2/%3, priority=%4)").arg(gdbName).arg(geomIdx + 1).arg(totalGroupsForName).arg(item.priority)
                                  : QString("  [Debug] For GDB \"%1\" (priority=%2)").arg(gdbName).arg(item.priority);

        qDebug().nospace() << debugPrefix << ", found " << projectedAnnos.size() << " projected candidate annos.";

        if (!projectedAnnos.isEmpty())
        {
            QStringList texts;
            QStringList distances;
            for (const auto &pa : projectedAnnos)
            {
                texts << pa.text;
                distances << QString("%1(%2)").arg(pa.text).arg(pa.distanceAlong, 0, 'f', 1);
            }
            qDebug() << "    > Candidates (sorted by distance along line):" << texts.join(" -> ");
            qDebug() << "    > Distances (meters):" << distances.join(" -> ");
        }

        int groupsBefore = finalAnnoGroups.size();
        findGroupsOnLine(currentGrouped, projectedAnnos, gdbName, finalAnnoGroups);

        bool foundInPrimary = (finalAnnoGroups.size() > groupsBefore);

        if (finalAnnoGroups.size() > groupsBefore)
        {
            // 【新增】主匹配出组后做逐FID垂直距离校验
            // 防止远处单字注记被错误纳入（如远处"大"字被纳入"大冰沟"组）
            const double primaryDistThreshold = (10.0 / 1000.0) * scaleDenominator; // 10mm图上距离

            int validGroupCount = 0;
            for (int g = groupsBefore; g < finalAnnoGroups.size(); g++)
            {
                QList<qint64> &group = finalAnnoGroups[g];
                QStringList removedLog;
                QList<qint64> kept;
                for (qint64 fid : group)
                {
                    double dist = gdbGeom.distance(allAnnos[fid].geom);
                    if (dist >= primaryDistThreshold)
                    {
                        removedLog << QString("%1(FID%2,%.0fm)").arg(allAnnos[fid].text).arg(fid).arg(dist);
                    }
                    else
                    {
                        kept.append(fid);
                    }
                }

                if (!removedLog.isEmpty())
                {
                    qDebug().nospace() << "  [主匹配逐出-距离] GDB \"" << gdbName
                                       << "\": " << removedLog.join(", ")
                                       << " (阈值" << primaryDistThreshold << "m)";
                }

                if (kept.size() < 2)
                {
                    qDebug().nospace() << "  [主匹配拒绝] GDB \"" << gdbName
                                       << "\": 距离过滤后仅剩" << kept.size() << "字，整组不输出";
                    group.clear();
                    continue;
                }

                // 更新group为过滤后的fids
                group = kept;
                validGroupCount++;

                QStringList groupTexts;
                for (qint64 fid : group)
                {
                    groupTexts << allAnnos[fid].text;
                    groupedAnnoFids.insert(fid);
                }
                qDebug() << "    > SUCCESS: Formed group" << (g + 1) << " with text: \"" << groupTexts.join("") << "\"";
            }
            qDebug() << "    > Total new groups formed:" << validGroupCount;

            // 如果所有组都被距离过滤拒绝，回退foundInPrimary=进入兜底路径
            if (validGroupCount == 0)
            {
                foundInPrimary = false;
                // 追加到unmatchedGdbEntries供兜底输出
                UnmatchedGdbWithCandidates unmatched;
                unmatched.gdbName = gdbName;
                unmatched.geomIdx = geomIdx;
                unmatched.candidates = projectedAnnos;
                unmatchedGdbEntries.append(unmatched);
            }
        }
        else if (!projectedAnnos.isEmpty())
        {
            qDebug() << "    > FAILURE: Could not form a valid sequence from the candidates.";
            // 【新增】记录失败但有候选的GDB条目，供后续兜底输出
            UnmatchedGdbWithCandidates unmatched;
            unmatched.gdbName = gdbName;
            unmatched.geomIdx = geomIdx;
            unmatched.candidates = projectedAnnos;
            unmatchedGdbEntries.append(unmatched);
        }

        QgsWkbTypes::GeometryType currentGeomType = gdbGeom.type();
        bool foundInFallback = false;
        if (!foundInPrimary && !nullNameFids.isEmpty() && currentGeomType == QgsWkbTypes::LineGeometry)
        {
            // 【修复】直接使用 mergedGdbGeometriesByName，不再创建新的引用变量
            const QList<qint64> &nameFids = mergedGdbGroupsByName[gdbName][geomIdx].fids;

            if (nameFids.isEmpty())
            {
                qDebug() << "    > Fallback aborted: Cannot determine original FIDs for this geometry.";
                continue;
            }

            QString groupLevel = gdbLevelCache[nameFids.first()];

            // 子函数：查找与种子要素端点相接的 NULL 段
            auto findConnectedNullSegments = [&](const QList<qint64> &seedFids) -> QList<qint64>
            {
                QList<qint64> result;
                QSet<qint64> visited;
                QQueue<qint64> queue;

                for (qint64 fid : seedFids)
                {
                    queue.enqueue(fid);
                    visited.insert(fid);
                }

                // 收集所有种子要素的端点
                QList<QgsPointXY> seedEndpoints;
                for (qint64 fid : seedFids)
                {
                    const QgsGeometry &geom = gdbGeomCache[fid];

                    // 正确处理多部分几何
                    if (geom.isMultipart())
                    {
                        auto multiLine = geom.asMultiPolyline();
                        for (const QgsPolylineXY &line : multiLine)
                        {
                            if (!line.isEmpty())
                            {
                                seedEndpoints.append(line.first());
                                seedEndpoints.append(line.last());
                            }
                        }
                    }
                    else
                    {
                        QgsPolylineXY line = geom.asPolyline();
                        if (!line.isEmpty())
                        {
                            seedEndpoints.append(line.first());
                            seedEndpoints.append(line.last());
                        }
                    }
                }

                qDebug() << "      [Fallback-Debug] Seed FIDs:" << seedFids
                         << ", Total seed endpoints:" << seedEndpoints.size()
                         << ", Group level:" << groupLevel;

                int nullCandidateCount = 0;
                int levelMismatchCount = 0;
                int notConnectedCount = 0;

                while (!queue.isEmpty())
                {
                    qint64 currentFid = queue.dequeue();
                    const QgsGeometry &currentGeom = gdbGeomCache[currentFid];

                    // 获取当前要素的端点
                    QList<QgsPointXY> currentEndpoints;
                    if (currentGeom.isMultipart())
                    {
                        auto multiLine = currentGeom.asMultiPolyline();
                        for (const QgsPolylineXY &line : multiLine)
                        {
                            if (!line.isEmpty())
                            {
                                currentEndpoints.append(line.first());
                                currentEndpoints.append(line.last());
                            }
                        }
                    }
                    else
                    {
                        QgsPolylineXY line = currentGeom.asPolyline();
                        if (!line.isEmpty())
                        {
                            currentEndpoints.append(line.first());
                            currentEndpoints.append(line.last());
                        }
                    }

                    for (qint64 nullFid : nullNameFids)
                    {
                        nullCandidateCount++;

                        if (visited.contains(nullFid))
                            continue;

                        // 检查级别
                        QString nullLevel = gdbLevelCache.value(nullFid, "");
                        if (nullLevel != groupLevel)
                        {
                            levelMismatchCount++;
                            continue;
                        }

                        const QgsGeometry &nullGeom = nullGeomCache[nullFid];

                        // 获取NULL段的端点
                        QList<QgsPointXY> nullEndpoints;
                        if (nullGeom.isMultipart())
                        {
                            auto multiLine = nullGeom.asMultiPolyline();
                            for (const QgsPolylineXY &line : multiLine)
                            {
                                if (!line.isEmpty())
                                {
                                    nullEndpoints.append(line.first());

                                    nullEndpoints.append(line.last());
                                }
                            }
                        }
                        else
                        {
                            QgsPolylineXY nullLine = nullGeom.asPolyline();
                            if (!nullLine.isEmpty())
                            {
                                nullEndpoints.append(nullLine.first());
                                nullEndpoints.append(nullLine.last());
                            }
                        }

                        if (nullEndpoints.isEmpty())
                            continue;

                        // 检查端点连接
                        bool connected = false;
                        double minDist = std::numeric_limits<double>::max();

                        for (const QgsPointXY &currPt : currentEndpoints)
                        {
                            for (const QgsPointXY &nullPt : nullEndpoints)
                            {
                                double dist = currPt.distance(nullPt);
                                minDist = std::min(minDist, dist);

                                if (dist < endpointTolerance)
                                {
                                    connected = true;
                                    break;
                                }
                            }
                            if (connected)
                                break;
                        }

                        if (connected)
                        {
                            qDebug() << "      [Fallback-Debug] Found connected NULL segment, FID:" << nullFid
                                     << ", min distance:" << minDist;
                            result.append(nullFid);
                            queue.enqueue(nullFid);
                            visited.insert(nullFid);
                        }
                        else
                        {
                            notConnectedCount++;
                        }
                    }
                }

                qDebug() << "      [Fallback-Debug] Search completed. Connected NULL segments:" << result.size()
                         << ", Total NULL candidates checked:" << nullCandidateCount
                         << ", Level mismatches:" << levelMismatchCount
                         << ", Not connected:" << notConnectedCount;

                return result;
            };

            QList<qint64> connectedNulls = findConnectedNullSegments(nameFids);

            if (!connectedNulls.isEmpty())
            {
                qDebug() << "  > Fallback: \"" << gdbName << "\" attempting to merge" << connectedNulls.size() << "NULL segment(s)";

                QList<QgsGeometry> unionPieces;
                for (qint64 fid : nameFids)
                    unionPieces.append(gdbGeomCache[fid]);
                for (qint64 fid : connectedNulls)
                    unionPieces.append(nullGeomCache[fid]);

                QgsGeometry tempLine = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(unionPieces));

                if (!tempLine.isEmpty())
                {
                    // 在扩展后的几何上重新构建候选列表
                    QList<ProjectedAnno> projected2 = buildProjected(tempLine, candidateSearchRadius, 5.0);

                    qDebug().nospace() << "    [Debug-Fallback] After merging NULL segments, found "
                                       << projected2.size() << " projected candidates.";
                    if (!projected2.isEmpty())
                    {
                        QStringList texts;
                        for (const auto &pa : projected2)
                            texts << pa.text;
                        qDebug() << "      > Fallback Candidates:" << texts.join(" -> ");
                    }

                    int groupsBefore2 = finalAnnoGroups.size();

                    // 使用当前的全局已分组集合
                    QList<qint64> currentGrouped2 = groupedAnnoFids.values();
                    findGroupsOnLine(currentGrouped2, projected2, gdbName, finalAnnoGroups);

                    if (finalAnnoGroups.size() > groupsBefore2)
                    {
                        foundInFallback = true;
                        // 成功形成了新组，更新全局集合
                        for (int g = groupsBefore2; g < finalAnnoGroups.size(); g++)
                        {
                            QStringList groupTexts;
                            for (qint64 fid : finalAnnoGroups[g])
                            {
                                groupTexts << allAnnos[fid].text;
                                groupedAnnoFids.insert(fid);
                            }
                            qDebug() << "      > Fallback SUCCESS: Formed group" << (g + 1) << " with text: \"" << groupTexts.join("") << "\"";
                        }
                        qDebug() << "      > Total new groups formed in fallback:" << (finalAnnoGroups.size() - groupsBefore2);
                    }
                    else if (!projected2.isEmpty())
                    {
                        qDebug() << "      > Fallback FAILURE: Still could not form a valid sequence after merging NULL segments.";
                    }
                }
            }
            else
            {
                qDebug() << "  > Fallback: No connected NULL segments found for \"" << gdbName << "\"";
            }
        }

        // --- 4.3 原部分匹配入口保持不变 ---
        const bool originalPartialEligible = !foundInPrimary && !foundInFallback;
        QList<qint64> availableCandidateFids;
        for (const auto &pa : projectedAnnos)
        {
            if (!groupedAnnoFids.contains(pa.fid))
                availableCandidateFids.append(pa.fid);
        }

        int minRequiredCandidates = qMax(2, gdbName.length() / 2);
        QMap<QChar, int> initialAvailableCharCounts;
        for (const auto &pa : projectedAnnos)
        {
            if (!groupedAnnoFids.contains(pa.fid))
                initialAvailableCharCounts[pa.text.at(0)]++;
        }

        bool hasCollectiveCompleteName = true;
        QMap<QChar, int> completeNameCharCounts;
        for (const QChar &ch : gdbName)
        {
            int requiredCount = completeNameCharCounts.value(ch) + 1;
            completeNameCharCounts[ch] = requiredCount;
            if (initialAvailableCharCounts.value(ch) < requiredCount)
                hasCollectiveCompleteName = false;
        }

        if (originalPartialEligible && !hasCollectiveCompleteName &&
            availableCandidateFids.size() >= minRequiredCandidates)
        {
            potentialPartialMatches[gdbName].append(availableCandidateFids);
        }

        // 完整组成功后仍可能剩余一组或多组错误注记。这里只收集延迟结果，不占用原匹配候选。
        if ((foundInPrimary || originalPartialEligible) && availableCandidateFids.size() >= minRequiredCandidates)
        {
            QSet<qint64> locallyDeferredFids;
            while (true)
            {
                QMap<QChar, int> availableCharCounts;
                for (const auto &pa : projectedAnnos)
                {
                    if (!groupedAnnoFids.contains(pa.fid) && !locallyDeferredFids.contains(pa.fid))
                        availableCharCounts[pa.text.at(0)]++;
                }

                QString partialTargetName;
                QMap<QChar, int> requiredCharCounts;
                for (const QChar &ch : gdbName)
                {
                    int requiredCount = requiredCharCounts.value(ch) + 1;
                    requiredCharCounts[ch] = requiredCount;
                    if (availableCharCounts.value(ch) >= requiredCount)
                        partialTargetName.append(ch);
                }

                if (partialTargetName.length() < minRequiredCandidates)
                    break;

                QList<qint64> alreadyGroupedFids = groupedAnnoFids.values();
                alreadyGroupedFids.append(locallyDeferredFids.values());

                QStringList candidateTargets;
                if (partialTargetName.length() < gdbName.length())
                {
                    candidateTargets.append(partialTargetName);
                }
                else
                {
                    QSet<QString> uniqueTargets;
                    for (int i = 0; i < partialTargetName.length(); ++i)
                    {
                        QString target = partialTargetName;
                        target.remove(i, 1);
                        if (target.length() >= minRequiredCandidates && !uniqueTargets.contains(target))
                        {
                            uniqueTargets.insert(target);
                            candidateTargets.append(target);
                        }
                    }
                }

                QList<QList<qint64>> partialGroups;
                QString selectedTarget;
                int bestGroupedFidCount = 0;
                for (const QString &candidateTarget : candidateTargets)
                {
                    if (isGenericCategoryOnlyPartial(candidateTarget, gdbName))
                        continue;

                    QList<QList<qint64>> candidateGroups;
                    findGroupsOnLine(alreadyGroupedFids, projectedAnnos, candidateTarget, candidateGroups);

                    int groupedFidCount = 0;
                    for (const QList<qint64> &candidateGroup : candidateGroups)
                        groupedFidCount += candidateGroup.size();

                    if (groupedFidCount > bestGroupedFidCount)
                    {
                        bestGroupedFidCount = groupedFidCount;
                        selectedTarget = candidateTarget;
                        partialGroups = candidateGroups;
                    }
                }

                if (partialGroups.isEmpty())
                    break;

                qDebug() << "  部分匹配拆组: GDB \"" << gdbName
                         << "\", 选用子名称:\"" << selectedTarget
                         << "\", 分组数:" << partialGroups.size();

                bool addedGroup = false;
                for (const QList<qint64> &partialGroup : partialGroups)
                {
                    QString partialText;
                    for (qint64 fid : partialGroup)
                        partialText += allAnnos.value(fid).text;

                    if (isGenericCategoryOnlyPartial(partialText, gdbName))
                        continue;

                    PartialMatchResult partialMatch = checkPartialMatch(partialText, gdbName);
                    if (!partialMatch.isMatch && !foundInPrimary)
                        continue;

                    double deferredScore = partialMatch.isMatch
                                               ? partialMatch.score
                                               : static_cast<double>(partialText.length()) / gdbName.length();

                    deferredPartialGroups.append(
                        {gdbName, geomIdx, partialGroup, deferredScore,
                         originalPartialEligible && !hasCollectiveCompleteName});
                    for (qint64 fid : partialGroup)
                        locallyDeferredFids.insert(fid);
                    addedGroup = true;

                    if (!partialMatch.isMatch)
                    {
                        qDebug() << "  部分匹配 (同组完整注记佐证): GDB \"" << gdbName
                                 << "\", 候选文本:\"" << partialText
                                 << "\", 候选FID:" << partialGroup
                                 << "覆盖率:" << deferredScore;
                    }
                }

                if (!addedGroup)
                    break;
            }
        }
    }

    // =============================================================================================
    // === 【新增】步骤 4.4.5: 兜底输出 —— 对主匹配失败、但有投影候选的GDB条目生成部分匹配
    // =============================================================================================
    if (!unmatchedGdbEntries.isEmpty())
    {
        qDebug() << "\n--- 不匹配输出：有投影注记但无法拼组完整GDB名称的条目 ---";
        qDebug() << "信息:" << unmatchedGdbEntries.size() << "个GDB条目找到部分注记，但无法拼出完整名称，标记为UNMATCHED。";

        // 【修复】构建 FID→(GDB条目, 距离) 映射，按最近距离分配而非先到先得
        struct AnnoGdbDist
        {
            int unmatchedIdx;
            double distance;
        };
        QMap<qint64, QList<AnnoGdbDist>> fidToGdbDists;

        for (int u = 0; u < unmatchedGdbEntries.size(); u++)
        {
            const UnmatchedGdbWithCandidates &unmatched = unmatchedGdbEntries[u];
            const QgsGeometry &gdbGeom = mergedGdbGroupsByName[unmatched.gdbName][unmatched.geomIdx].geometry;

            for (const ProjectedAnno &pa : unmatched.candidates)
            {
                if (groupedAnnoFids.contains(pa.fid))
                    continue;

                double dist = gdbGeom.distance(allAnnos[pa.fid].geom);
                fidToGdbDists[pa.fid].append({u, dist});
            }
        }

        if (fidToGdbDists.isEmpty())
        {
            qDebug() << "  所有候选注记已被其他匹配占用，无需兜底输出。";
        }
        else
        {
            // 按距离最近分配：每个 FID 归到最近的 GDB
            struct GdbCandidateGroup
            {
                QString gdbName;
                int geomIdx;
                QList<qint64> fids;
                QStringList texts;
            };
            QMap<QString, GdbCandidateGroup> gdbGroups; // key = gdbName + "_" + geomIdx

            for (auto it = fidToGdbDists.begin(); it != fidToGdbDists.end(); ++it)
            {
                qint64 annoFid = it.key();
                QList<AnnoGdbDist> &dists = it.value();

                // 按距离升序，最近的排第一个
                std::sort(dists.begin(), dists.end(),
                          [](const AnnoGdbDist &a, const AnnoGdbDist &b) { return a.distance < b.distance; });

                int bestIdx = dists.first().unmatchedIdx;
                const UnmatchedGdbWithCandidates &bestUnmatched = unmatchedGdbEntries[bestIdx];

                QString key = bestUnmatched.gdbName + "_" + QString::number(bestUnmatched.geomIdx);
                if (!gdbGroups.contains(key))
                {
                    GdbCandidateGroup group;
                    group.gdbName = bestUnmatched.gdbName;
                    group.geomIdx = bestUnmatched.geomIdx;
                    gdbGroups[key] = group;
                }
                gdbGroups[key].fids.append(annoFid);
                gdbGroups[key].texts.append(allAnnos[annoFid].text);

                // 日志：如果有多个候选GDB，打印竞争情况
                if (dists.size() > 1)
                {
                    QStringList altNames;
                    for (int d = 1; d < dists.size(); d++)
                    {
                        altNames << QString("%1(%2)").arg(unmatchedGdbEntries[dists[d].unmatchedIdx].gdbName)
                                        .arg(dists[d].distance, 0, 'f', 1);
                    }
                    qDebug().nospace() << "  [距离竞争] FID " << annoFid
                                       << " (\"" << allAnnos[annoFid].text << "\"): "
                                       << QString("选中 \"%1\"(%2)").arg(bestUnmatched.gdbName).arg(dists.first().distance, 0, 'f', 1)
                                       << ", 放弃 [" << altNames.join(", ") << "]";
                }

                groupedAnnoFids.insert(annoFid);
            }

            // =====================================================================
            // 【兜底校验 - 逐FID细粒度过滤】
            // 不做全组拒绝，而是逐FID检查、只踢掉不合格个体，保留合法匹配
            // Phase 1: 逐FID垂直距离过滤  Phase 2: 逐FID顺序修复  Phase 3: 跨度检查
            // =====================================================================
            const double fallbackNearRadius = (10.0 / 1000.0) * scaleDenominator; // 10mm图上距离

            // 构建 FID→(distanceAlong, rawDist) 映射
            struct FallbackProjInfo { double distAlong; double rawDist; };
            QMap<QString, QMap<qint64, FallbackProjInfo>> gdbFidProjInfo;
            for (int u = 0; u < unmatchedGdbEntries.size(); u++)
            {
                const UnmatchedGdbWithCandidates &unmatched = unmatchedGdbEntries[u];
                QString key = unmatched.gdbName + "_" + QString::number(unmatched.geomIdx);
                const QgsGeometry &gdbGeom = mergedGdbGroupsByName[unmatched.gdbName][unmatched.geomIdx].geometry;
                for (const ProjectedAnno &pa : unmatched.candidates)
                {
                    double rawDist = gdbGeom.distance(allAnnos[pa.fid].geom);
                    gdbFidProjInfo[key][pa.fid] = {pa.distanceAlong, rawDist};
                }
            }

            for (auto git = gdbGroups.begin(); git != gdbGroups.end(); ++git)
            {
                const QString &groupKey = git.key();
                GdbCandidateGroup &group = git.value();
                const auto &projMap = gdbFidProjInfo.value(groupKey);

                if (group.fids.isEmpty()) continue;

                // ========== Phase 1: 逐FID垂直距离过滤 ==========
                // 踢掉距离GDB线超过阈值的单个注记，不牵连同组其他FID
                {
                    QList<qint64> distKept;
                    QStringList distRemovedTexts;
                    for (qint64 fid : group.fids)
                    {
                        double rawDist = projMap.value(fid).rawDist;
                        if (rawDist >= fallbackNearRadius)
                        {
                            distRemovedTexts << QString("%1(FID%2,%.0fm)").arg(allAnnos[fid].text).arg(fid).arg(rawDist);
                            groupedAnnoFids.remove(fid);
                        }
                        else
                        {
                            distKept.append(fid);
                        }
                    }
                    if (!distRemovedTexts.isEmpty())
                    {
                        qDebug().nospace() << "  [兜底逐出-距离] GDB \"" << group.gdbName
                                           << "\": " << distRemovedTexts.join(", ")
                                           << " (阈值" << fallbackNearRadius << "m)";
                        // 重建 group
                        group.fids = distKept;
                        group.texts.clear();
                        for (qint64 fid : distKept)
                            group.texts.append(allAnnos[fid].text);
                    }
                }

                if (group.fids.isEmpty()) continue;

                // ========== Phase 2: 仅剔除不在GDB名称中的字符 ==========
                // 顺序异常（正/逆向均不成立）仅记录不踢人，避免误伤交换字符的场景
                // 真正的兜底防线依赖 Phase 1 距离 + Phase 3 跨度
                {
                    QList<qint64> cleanedFids;
                    QList<qint64> removedFids;
                    QStringList removedDetail;
                    for (qint64 fid : group.fids)
                    {
                        QString ch = allAnnos[fid].text;
                        if (group.gdbName.indexOf(ch) < 0)
                        {
                            removedFids.append(fid);
                            removedDetail << QString("%1(不在名称中)").arg(ch);
                        }
                        else
                        {
                            cleanedFids.append(fid);
                        }
                    }
                    if (!removedFids.isEmpty())
                    {
                        qDebug().nospace() << "  [兜底逐出-字符] GDB \"" << group.gdbName
                                           << "\": " << removedDetail.join(", ");
                        for (qint64 fid : removedFids)
                            groupedAnnoFids.remove(fid);
                        group.fids = cleanedFids;
                        group.texts.clear();
                        for (qint64 fid : cleanedFids)
                            group.texts.append(allAnnos[fid].text);
                    }
                }

                if (group.fids.isEmpty()) continue;

                // ========== Phase 3: 间隙异常检测 ==========
                // 不等间距视为异常：若相邻间隙中有某个间隙远大于其他间隙，
                // 且孤立侧只有1个FID，则踢掉该孤立FID（多为远处误收的单字）
                {
                    QList<qint64> sortedFids = group.fids;
                    std::sort(sortedFids.begin(), sortedFids.end(),
                              [&](qint64 a, qint64 b) {
                                  return projMap.value(a).distAlong < projMap.value(b).distAlong;
                              });

                    if (sortedFids.size() >= 3)  // 至少3个FID才有意义
                    {
                        // 计算相邻间隙
                        QVector<double> gaps;
                        for (int i = 0; i < sortedFids.size() - 1; i++)
                            gaps.append(projMap.value(sortedFids[i + 1]).distAlong
                                      - projMap.value(sortedFids[i]).distAlong);

                        // 找最大间隙的位置和值
                        int maxGapIdx = 0;
                        double maxGap = gaps[0];
                        for (int i = 1; i < gaps.size(); i++)
                        {
                            if (gaps[i] > maxGap) { maxGap = gaps[i]; maxGapIdx = i; }
                        }

                        // 其他间隙的均值
                        double sumOther = 0;
                        for (int i = 0; i < gaps.size(); i++)
                            if (i != maxGapIdx) sumOther += gaps[i];
                        double meanOther = sumOther / (gaps.size() - 1);

                        // 孤立侧FID数量：maxGapIdx左侧有 maxGapIdx+1 个，右侧有 sortedFids.size()-maxGapIdx-1 个
                        int leftCount = maxGapIdx + 1;
                        int rightCount = sortedFids.size() - maxGapIdx - 1;

                        // 异常条件：最大间隙 > 5倍其他间隙均值，且孤立侧只有1个FID
                        if (maxGap > 5.0 * meanOther && (leftCount == 1 || rightCount == 1))
                        {
                            // 确定孤立FID
                            qint64 outlierFid = (leftCount == 1) ? sortedFids.first() : sortedFids.last();
                            double outlierDist = projMap.value(outlierFid).distAlong;

                            qDebug().nospace() << "  [兜底逐出-间隙] GDB \"" << group.gdbName
                                               << "\": " << allAnnos[outlierFid].text
                                               << "(FID" << outlierFid << ","
                                               << outlierDist << "m)"
                                               << " 最大间隙" << maxGap << "m远超均值" << meanOther << "m";

                            groupedAnnoFids.remove(outlierFid);
                            group.fids.removeAll(outlierFid);
                            group.texts.clear();
                            for (qint64 fid : group.fids)
                                group.texts.append(allAnnos[fid].text);
                        }
                    }
                }

                if (group.fids.isEmpty()) continue;

                // ========== 输出 ==========
                // Phase 2 已确保 group.fids 中的字符都在 GDB 名称中
                // 只是拼不出完整名称 → PARTIALLY_MATCHED，而非 UNMATCHED
                {
                    const QList<qint64> &originalFids = nameToFids.value(group.gdbName);
                    qint64 representativeGdbFid = originalFids.isEmpty() ? -1 : originalFids.first();

                    // 计算字符覆盖率作为匹配分数：找到的字符数 / GDB名称字符数
                    QSet<QString> foundChars;
                    for (const QString &txt : group.texts)
                        foundChars.insert(txt);
                    double charCoverage = group.gdbName.isEmpty() ? 0.0
                        : (double)foundChars.size() / (double)group.gdbName.length();
                    double partialScore = qBound(0.3, charCoverage * 0.8, 0.79);

                    QSet<QString> requiredChars;
                    for (const QChar &ch : group.gdbName)
                        requiredChars.insert(QString(ch));

                    QStringList missingChars;
                    for (const QString &ch : requiredChars)
                    {
                        if (!foundChars.contains(ch))
                            missingChars.append(ch);
                    }

                    globalGroupId++;
                    for (qint64 annoFid : group.fids)
                    {
                        result.annoToGroupMap[annoFid] = globalGroupId;

                        FeatureMatchLink newLink;
                        newLink.status = PARTIALLY_MATCHED;
                        newLink.matchScore = partialScore;
                        newLink.annoFid = annoFid;
                        newLink.gdbFid = representativeGdbFid;
                        newLink.gdbFids = originalFids;
                        newLink.annoText = allAnnos.value(annoFid).text;
                        newLink.assoc_name = group.gdbName;
                        result.links.append(newLink);
                    }

                    qDebug().nospace() << "  不匹配: GDB \"" << group.gdbName
                                       << "\" -> 已找到: [" << group.texts.join(",")
                                       << "] 缺失: [" << missingChars.join(",")
                                       << "] group_id:" << globalGroupId
                                       << " FID:" << group.fids;
                }
            }
        }
    }

    qDebug() << "--- Sequence Validation Summary ---";
    if (finalAnnoGroups.isEmpty())
    {
        qDebug() << "  No high-confidence annotation groups were formed.";
    }
    else
    {
        qDebug() << "  Total high-confidence groups found:" << finalAnnoGroups.size();
        for (int i = 0; i < finalAnnoGroups.size(); i++)
        {
            QStringList texts;
            for (qint64 fid : finalAnnoGroups[i])
            {
                texts << allAnnos[fid].text;
            }
            qDebug().nospace() << "    - Group " << i + 1 << ": Text=\"" << texts.join("") << "\", FIDs=" << finalAnnoGroups[i];
        }
    }
    qDebug() << "---------------------------------";

    // =============================================================================================
    // ===           【最终版】步骤 4.5: 最终部分匹配决策阶段           ===
    // =============================================================================================
    qDebug() << "\n--- 开始最终部分匹配决策阶段 ---";
    qDebug() << "信息: " << potentialPartialMatches.size() << "个GDB地名有部分匹配潜力。";
    qDebug() << "信息: 当前已有" << groupedAnnoFids.size() << "个注记被完全匹配占用。";
    QMap<QString, QSet<qint64>> duplicateZeroOutputFids;

    auto pickBestMergedGroupForAnno = [&](const QString &gdbName, const QList<qint64> &annoFids) -> const MergedGdbGroup *
    {
        auto groupsIt = mergedGdbGroupsByName.constFind(gdbName);
        if (groupsIt == mergedGdbGroupsByName.constEnd() || groupsIt->isEmpty())
            return nullptr;

        const QList<MergedGdbGroup> &groups = groupsIt.value();
        if (annoFids.isEmpty())
            return &groups.first();

        QPointF centroid(0.0, 0.0);
        int count = 0;
        for (qint64 fid : annoFids)
        {
            auto annoIt = allAnnos.constFind(fid);
            if (annoIt == allAnnos.constEnd())
                continue;
            centroid.rx() += annoIt->center.x();
            centroid.ry() += annoIt->center.y();
            ++count;
        }
        if (count == 0)
            return &groups.first();

        centroid.rx() /= count;
        centroid.ry() /= count;

        QgsGeometry centroidGeom = QgsGeometry::fromPointXY(QgsPointXY(centroid.x(), centroid.y()));
        double bestDist = std::numeric_limits<double>::max();
        const MergedGdbGroup *bestGroup = nullptr;
        for (const MergedGdbGroup &group : groups)
        {
            double dist = group.geometry.distance(centroidGeom);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestGroup = &group;
            }
        }
        return bestGroup ? bestGroup : &groups.first();
    };

    for (auto it = potentialPartialMatches.constBegin(); it != potentialPartialMatches.constEnd(); ++it)
    {
        const QString &gdbName = it.key();
        const QList<qint64> &allPotentialFids = it.value();

        // 1. 过滤掉已被完全匹配占用的注记
        QList<qint64> finalAvailableFids;
        QSet<qint64> uniqueFids;
        for (qint64 fid : allPotentialFids)
        {
            if (!groupedAnnoFids.contains(fid) && !uniqueFids.contains(fid))
            {
                finalAvailableFids.append(fid);
                uniqueFids.insert(fid);
            }
        }

        // 2. 最小候选数量再次检查
        int minRequiredCandidates = qMax(2, gdbName.length() / 2);
        if (finalAvailableFids.size() < minRequiredCandidates)
            continue;

        // 3. 构建【完整】候选文本，用于最准确的匹配检查
        QString collectedTextForCheck;
        for (qint64 fid : finalAvailableFids)
        {
            collectedTextForCheck += allAnnos.value(fid).text;
        }

        PartialMatchResult pmResult = checkPartialMatch(collectedTextForCheck, gdbName);

        if (pmResult.isMatch)
        {
            // 4. 最终验证 (子集检查，保持不变)
            QSet<QChar> gdbChars;
            for (const QChar &ch : normalizeNumberChars(gdbName))
                gdbChars.insert(ch);

            bool isSubset = true;
            for (qint64 fid : finalAvailableFids)
            {
                QChar annoChar = normalizeNumberChars(allAnnos.value(fid).text).at(0);
                if (!gdbChars.contains(annoChar))
                {
                    isSubset = false;
                    break;
                }
            }

            if (isSubset)
            {
                // 5. 【核心修改】在生成结果前，执行您提议的去重逻辑
                QMap<QString, QList<qint64>> charToFidsMap;
                for (qint64 fid : finalAvailableFids)
                {
                    charToFidsMap[allAnnos.value(fid).text].append(fid);
                }

                QList<qint64> fidsToOutput;
                QString textForLog;
                for (auto mapIt = charToFidsMap.constBegin(); mapIt != charToFidsMap.constEnd(); ++mapIt)
                {
                    // 如果一个字符只出现了一次，就保留它的FID用于输出
                    if (mapIt.value().size() == 1)
                    {
                        fidsToOutput.append(mapIt.value().first());
                        textForLog += mapIt.key();
                    }
                    // 如果出现多次，则全部丢弃
                }

                // 如果去重后仍然有有意义的结果，才最终输出
                if (!fidsToOutput.isEmpty())
                {
                    qDebug() << "  部分匹配 (最终决策): GDB \"" << gdbName << "\" -> 候选 \"" << collectedTextForCheck
                             << "\", 得分:" << pmResult.score << "。去重后输出: \"" << textForLog << "\"";

                    const MergedGdbGroup *bestGroup = pickBestMergedGroupForAnno(gdbName, fidsToOutput);
                    QList<qint64> originalFids = bestGroup ? bestGroup->fids : nameToFids.value(gdbName);
                    qint64 representativeGdbFid = originalFids.isEmpty() ? -1 : originalFids.first();

                    globalGroupId++;

                    // 只为去重后留下的注记生成 link
                    for (qint64 annoFid : fidsToOutput)
                    {
                        groupedAnnoFids.insert(annoFid);

                        result.annoToGroupMap[annoFid] = globalGroupId;
                        FeatureMatchLink newLink;
                        newLink.status = PARTIALLY_MATCHED;
                        newLink.matchScore = pmResult.score;
                        newLink.annoFid = annoFid;
                        newLink.gdbFid = representativeGdbFid;
                        newLink.gdbFids = originalFids; // 新增：写入完整的原始 GDB FID 列表
                        newLink.annoText = allAnnos.value(annoFid).text;
                        // 新增：记录关联的完整 GDB 名称
                        newLink.assoc_name = gdbName;
                        // ... 填充其他 link 属性 ...
                        result.links.append(newLink);
                    }
                }
                else
                {
                    QSet<qint64> &zeroOutputFids = duplicateZeroOutputFids[gdbName];
                    for (qint64 fid : finalAvailableFids)
                        zeroOutputFids.insert(fid);
                }
            }
        }
    }

    // =============================================================================================
    // === 步骤 5: 生成最终匹配结果并填充 MatchResult
    // =============================================================================================
    generateFinalMatchResult(finalAnnoGroups, allAnnos, mergedGdbGroupsByName, nameToFids,
                             gdbLayer, actualLayer, annoLayer, annoAngleIdx, mapScale,
                             result, globalGroupId,
                             // -- 传递缓存 --
                             symbolIdToFidsMap_cache, symbolComponentIndex_cache,
                             componentFidToSymbolIdMap_cache, symbolIdToCenterMap_cache);

    QMap<qint64, QSet<QString>> deferredFidNames;
    for (const DeferredPartialGroup &partialGroup : deferredPartialGroups)
    {
        for (qint64 fid : partialGroup.fids)
            deferredFidNames[fid].insert(partialGroup.gdbName);
    }

    // 后置输出由原沿线分组算法验证的部分匹配组，避免影响既有匹配的候选占用和group_id。
    for (const DeferredPartialGroup &partialGroup : deferredPartialGroups)
    {
        if (partialGroup.requireZeroOutput)
        {
            const QSet<qint64> zeroOutputFids = duplicateZeroOutputFids.value(partialGroup.gdbName);
            bool belongsToZeroOutput = !zeroOutputFids.isEmpty();
            for (qint64 fid : partialGroup.fids)
            {
                if (!zeroOutputFids.contains(fid))
                {
                    belongsToZeroOutput = false;
                    break;
                }
            }
            if (!belongsToZeroOutput)
                continue;
        }

        bool alreadyGroupedOrAmbiguous = false;
        for (qint64 fid : partialGroup.fids)
        {
            if (groupedAnnoFids.contains(fid) || deferredFidNames.value(fid).size() > 1)
            {
                alreadyGroupedOrAmbiguous = true;
                break;
            }
        }
        if (alreadyGroupedOrAmbiguous)
            continue;

        auto groupsIt = mergedGdbGroupsByName.constFind(partialGroup.gdbName);
        if (groupsIt == mergedGdbGroupsByName.constEnd() || groupsIt->isEmpty() ||
            partialGroup.geomIdx < 0 || partialGroup.geomIdx >= groupsIt->size())
            continue;

        const MergedGdbGroup *sourceGroup = &groupsIt->at(partialGroup.geomIdx);

        QgsPointXY groupCenter(0.0, 0.0);
        for (qint64 fid : partialGroup.fids)
        {
            groupCenter.setX(groupCenter.x() + allAnnos.value(fid).center.x());
            groupCenter.setY(groupCenter.y() + allAnnos.value(fid).center.y());
        }
        groupCenter.setX(groupCenter.x() / partialGroup.fids.size());
        groupCenter.setY(groupCenter.y() / partialGroup.fids.size());
        QgsGeometry centerGeometry = QgsGeometry::fromPointXY(groupCenter);

        const MergedGdbGroup *bestGroup = nullptr;
        double bestDistance = std::numeric_limits<double>::max();
        double secondBestDistance = std::numeric_limits<double>::max();
        for (const MergedGdbGroup &group : groupsIt.value())
        {
            double distance = group.geometry.distance(centerGeometry);
            if (distance < bestDistance)
            {
                secondBestDistance = bestDistance;
                bestDistance = distance;
                bestGroup = &group;
            }
            else if (distance < secondBestDistance)
            {
                secondBestDistance = distance;
            }
        }

        if (!bestGroup || bestGroup != sourceGroup ||
            (secondBestDistance < std::numeric_limits<double>::max() &&
             secondBestDistance - bestDistance < endpointTolerance))
            continue;

        const QList<qint64> &originalFids = sourceGroup->fids;
        qint64 representativeGdbFid = originalFids.isEmpty() ? -1 : originalFids.first();

        globalGroupId++;
        for (qint64 annoFid : partialGroup.fids)
        {
            groupedAnnoFids.insert(annoFid);
            result.annoToGroupMap[annoFid] = globalGroupId;

            FeatureMatchLink newLink;
            newLink.status = PARTIALLY_MATCHED;
            newLink.matchScore = partialGroup.matchScore;
            newLink.annoFid = annoFid;
            newLink.gdbFid = representativeGdbFid;
            newLink.gdbFids = originalFids;
            newLink.annoText = allAnnos.value(annoFid).text;
            newLink.assoc_name = partialGroup.gdbName;
            result.links.append(newLink);
        }

        qDebug() << "  部分匹配 (复用沿线分组): GDB \"" << partialGroup.gdbName
                 << "\", 空间组:" << partialGroup.geomIdx
                 << "group_id:" << globalGroupId
                 << "候选FID:" << partialGroup.fids
                 << "得分:" << partialGroup.matchScore;
    }


    // 部分匹配只缺首/尾一字时，按真实阅读方向核验，避免跨段投影错序造成漏字。
    QMap<qint64, QList<int>> repairGroups;
    for (int i = 0; i < result.links.size(); ++i)
    {
        const auto &link = result.links.at(i);
        if (link.status == PARTIALLY_MATCHED)
            repairGroups[result.annoToGroupMap.value(link.annoFid, -1)].append(i);
    }
    struct EndpointRepair { QList<int> links; qint64 candidate; };
    QList<EndpointRepair> repairs;
    QMap<qint64, int> candidateUses;
    QSet<qint64> occupied;
    for (const auto &link : result.links)
        occupied.insert(link.annoFid);
    for (const auto &indices : repairGroups)
    {
        if (indices.size() < 2)
            continue;
        const auto &reference = result.links.at(indices.first());
        const QString name = reference.assoc_name;
        if (indices.size() + 1 != name.size())
            continue;
        QList<qint64> fids;
        QgsRectangle box;
        bool first = true;
        for (int index : indices)
        {
            qint64 fid = result.links.at(index).annoFid;
            fids.append(fid);
            if (first) { box = allAnnos.value(fid).geom.boundingBox(); first = false; }
            else box.combineExtentWith(allAnnos.value(fid).geom.boundingBox());
        }
        const bool horizontal = box.width() >= box.height();
        auto coordinate = [&](qint64 fid) {
            const auto center = allAnnos.value(fid).center;
            return horizontal ? center.x() : -center.y();
        };
        std::sort(fids.begin(), fids.end(), [&](qint64 a, qint64 b) { return coordinate(a) < coordinate(b); });
        QString text;
        QList<double> gaps;
        bool consistent = true;
        const auto &style = allAnnos.value(fids.first());
        for (int i = 0; i < fids.size(); ++i)
        {
            const auto &anno = allAnnos.value(fids.at(i));
            text += anno.text;
            consistent = consistent && anno.text.size() == 1 &&
                std::abs(anno.fontSize - style.fontSize) <= 0.1 && anno.fontFamily == style.fontFamily;
            if (i) gaps.append(allAnnos.value(fids.at(i - 1)).center.distance(anno.center));
        }
        const bool missingFirst = text == name.mid(1);
        const bool missingLast = text == name.left(name.size() - 1);
        if (!consistent || missingFirst == missingLast)
            continue;
        std::sort(gaps.begin(), gaps.end());
        const double typicalGap = gaps.at(gaps.size() / 2);
        if (typicalGap <= 0)
            continue;
        QList<QgsGeometry> sourceGeometries;
        const auto sourceFids = reference.gdbFids.isEmpty() ? QList<qint64>{reference.gdbFid} : reference.gdbFids;
        for (qint64 fid : sourceFids)
            sourceGeometries.append(gdbGeomCache.value(fid));
        const QgsGeometry source = QgsGeometry::unaryUnion(QVector<QgsGeometry>::fromList(sourceGeometries));
        const qint64 endFid = missingFirst ? fids.first() : fids.last();
        const auto end = allAnnos.value(endFid).center;
        const auto neighbor = allAnnos.value(missingFirst ? fids.at(1) : fids.at(fids.size() - 2)).center;
        const double vx = end.x() - neighbor.x(), vy = end.y() - neighbor.y();
        const double directionLength = std::hypot(vx, vy);
        QList<qint64> candidates;
        for (const auto &anno : allAnnos)
        {
            if (occupied.contains(anno.fid) || anno.text != (missingFirst ? name.left(1) : name.right(1)) ||
                std::abs(anno.fontSize - style.fontSize) > 0.1 || anno.fontFamily != style.fontFamily)
                continue;
            if ((missingFirst && coordinate(anno.fid) >= coordinate(endFid)) ||
                (missingLast && coordinate(anno.fid) <= coordinate(endFid)))
                continue;
            const double dx = anno.center.x() - end.x(), dy = anno.center.y() - end.y();
            const double gap = std::hypot(dx, dy);
            // 间距为组内典型间距的0.5~2倍，偏离首尾接续方向不超过45度。
            if (gap < typicalGap * 0.5 || gap > typicalGap * 2.0 || directionLength <= 0 ||
                (dx * vx + dy * vy) < 0.7071067811865476 * gap * directionLength)
                continue;
            const double maxDistance = (10.0 / 1000.0) * scaleDenominator;
            if (source.isEmpty() || source.distance(anno.geom) >= maxDistance)
                continue;
            candidates.append(anno.fid);
        }
        if (candidates.size() == 1)
        {
            repairs.append({indices, candidates.first()});
            candidateUses[candidates.first()]++;
        }
    }
    for (const auto &repair : repairs)
    {
        if (candidateUses.value(repair.candidate) != 1)
            continue;
        FeatureMatchLink added = result.links.at(repair.links.first());
        for (int index : repair.links)
        {
            result.links[index].status = FULLY_MATCHED;
            result.links[index].matchScore = 1.0;
        }
        added.annoFid = repair.candidate;
        const auto &anno = allAnnos.value(repair.candidate);
        added.annoText = anno.text;
        added.annoPosition = QPointF(anno.center.x(), anno.center.y());
        added.status = FULLY_MATCHED;
        added.matchScore = 1.0;
        result.annoToGroupMap[added.annoFid] = result.annoToGroupMap.value(result.links.at(repair.links.first()).annoFid);
        result.links.append(added);
        qDebug() << "首尾补字核验通过:" << added.assoc_name << "补入FID:" << added.annoFid;
    }

    qDebug() << "=== 匹配完成 ===";
    qDebug() << "总共生成" << result.links.size() << "条匹配链接。";

    return result;
}

// =================================================================================
// ===           请用下面的函数替换 se_feature_matching.cpp 中的           ===
// ===       `bool FeatureMatcher::createTempStandardLayer` (安全版本)         ===
// =================================================================================
bool FeatureMatcher::createTempStandardLayer(
    const QList<FeatureMatchLink> &matches,
    const QMap<qint64, qint64> &annoToGroupMap,
    QgsVectorLayer *annoLayer,
    QgsVectorLayer *gdbLayer,
    QgsVectorLayer *actualLayer,
    const QString &outputPath,
    bool outputUnmatchedAnnotations,
    double nearbyRadius,
    int gdbLevelIdx,
    const QSet<QString> &levelValues,
    QString &errMsg,
    const QSet<qint64> &excludedAnnoFids)
{
    // --- 步骤 1: 输入验证 ---
    if (!annoLayer || !gdbLayer /* actualLayer can be null */)
    {
        errMsg = "错误: 创建临时图层失败，一个或多个输入图层为空。";
        qWarning() << errMsg;
        return false;
    }

    // --- 步骤 2: 准备输出文件和字段 ---
    QFileInfo ofi(outputPath);
    QDir dir = ofi.dir();
    if (!dir.exists() && !dir.mkpath("."))
    {
        errMsg = QString("错误: 无法创建输出目录: %1").arg(dir.absolutePath());
        qWarning() << errMsg;
        return false;
    }

    // SHP字符串字段最多存254字节；数字编号按完整项分段，不能截断编号。
    const auto actualFidParts = [](const QList<qint64> &fids) {
        QStringList parts;
        QString current;
        for (qint64 fid : fids) {
            const QString token = QString::number(fid);
            if (!current.isEmpty() && current.size() + 1 + token.size() > 254) {
                parts.append(current);
                current.clear();
            }
            if (!current.isEmpty()) current += ',';
            current += token;
        }
        parts.append(current);
        return parts;
    };
    int actualPartCount = 1;
    for (const FeatureMatchLink &link : matches)
        actualPartCount = qMax(actualPartCount, actualFidParts(link.actualFids).size());

    QgsFields fields;

    // 2.1 添加我们自己生成的匹配字段
    fields.append(QgsField("match_status", QVariant::String, "String", 20));
    fields.append(QgsField("match_score", QVariant::Double, "Real", 10, 3)); // 增加精度定义
    fields.append(QgsField("anno_fid", QVariant::LongLong, "Integer64"));
    fields.append(QgsField("group_id", QVariant::LongLong, "Integer64"));
    fields.append(QgsField("gdb_fid", QVariant::LongLong, "Integer64"));
    // 新增：完整的构成 GDB FID 列表（逗号分隔）
    fields.append(QgsField("gdb_fids", QVariant::String, "String", 254));
    fields.append(QgsField("actual_fids", QVariant::String, "String", 254));
    // 第一段沿用原字段（写入SHP后为actual_fid），续段字段名不超过10字符。
    for (int part = 2; part <= actualPartCount; ++part)
        fields.append(QgsField(QString("act_fid%1").arg(part), QVariant::String, "String", 254));
    // --- 【新增】: 增加 symbol_id 字段 ---
    fields.append(QgsField("symbol_id", QVariant::String, "String", 254));
    // 新增输出字段 assoc_name
    fields.append(QgsField("assoc_name", QVariant::String, "String", 254));
    // 2.2 【核心】根据白名单，动态地从原始注记图层拷贝字段定义
    QStringList fieldWhitelist = {
        "text_conte", "font_size", "shear_angl", "font_famil", "font_color"
        // 您可以在此添加其他需要保留的原始字段名
    };

    const QgsFields &originalFields = annoLayer->fields();
    for (const QString &fieldNameToKeep : fieldWhitelist)
    {
        int originalIndex = originalFields.indexOf(fieldNameToKeep);
        if (originalIndex != -1)
        {
            // 如果在原始图层中找到了该字段，就将其定义添加到输出字段中
            fields.append(originalFields.at(originalIndex));
        }
        else
        {
            qWarning() << "警告: 白名单字段 '" << fieldNameToKeep << "' 在原始注记图层中未找到，将被忽略。";
        }
    }

    if (fields.count() > 255) {
        errMsg = QStringLiteral("组成线编号续存字段过多，超过SHP字段数量限制，未生成匹配结果");
        return false;
    }

    // 创建 Shapefile 写入器
    QgsVectorFileWriter writer(
        outputPath, "UTF-8", fields,
        annoLayer->wkbType(), annoLayer->crs(), "ESRI Shapefile");

    if (writer.hasError() != QgsVectorFileWriter::NoError)
    {
        errMsg = QString("错误: 初始化临时图层写入器失败: %1").arg(writer.errorMessage());
        qWarning() << errMsg;
        return false;
    }

    // --- 【新增】步骤 2.5: 收集所有已匹配的注记FID ---
    QSet<qint64> matchedAnnoFids;
    for (const FeatureMatchLink &link : matches)
    {
        if (link.annoFid >= 0)
        {
            matchedAnnoFids.insert(link.annoFid);
        }
    }

    // --- 步骤 3: 遍历匹配结果并安全地写入要素 ---
    int successCount = 0;
    int skipCount = 0;

    for (const FeatureMatchLink &link : matches)
    {
        if (link.annoFid < 0)
        {
            skipCount++;
            continue;
        }

        QgsFeature annoFeat = annoLayer->getFeature(link.annoFid);
        if (!annoFeat.isValid())
        {
            qWarning() << "警告: 找不到原始注记要素，跳过 anno_fid:" << link.annoFid;
            skipCount++;
            continue;
        }

        QgsFeature outFeat(fields);
        outFeat.setGeometry(annoFeat.geometry());

        // **【核心修复】: 按名称安全地设置所有属性**

        // 3.1 拷贝白名单中的原始属性
        for (const QString &fieldNameToCopy : fieldWhitelist)
        {
            // 检查输出字段中是否真的存在这个字段 (以防万一)
            if (fields.indexOf(fieldNameToCopy) != -1)
            {
                outFeat.setAttribute(fieldNameToCopy, annoFeat.attribute(fieldNameToCopy));
            }
        }

        // 3.2 设置我们自己生成的匹配属性
        QString statusStr;
        if (link.status == FULLY_MATCHED)
            statusStr = "FULLY_MATCHED";
        else if (link.status == UNMATCHED)
            statusStr = "UNMATCHED";
        else
            statusStr = "PARTIALLY_MATCHED";
        outFeat.setAttribute("match_status", statusStr);
        outFeat.setAttribute("match_score", link.matchScore);
        outFeat.setAttribute("anno_fid", QVariant::fromValue(link.annoFid));
        outFeat.setAttribute("group_id", QVariant::fromValue(annoToGroupMap.value(link.annoFid, -1)));
        outFeat.setAttribute("gdb_fid", QVariant::fromValue(link.gdbFid));

        // 写入完整 gdb_fids 列表（逗号分隔）
        if (!link.gdbFids.isEmpty())
        {
            QStringList s;
            for (qint64 id : link.gdbFids)
                s << QString::number(id);
            outFeat.setAttribute("gdb_fids", s.join(","));
        }
        else
        {
            outFeat.setAttribute("gdb_fids", QVariant());
        }

        // 写入 actual_fids
        const QStringList componentParts = actualFidParts(link.actualFids);
        outFeat.setAttribute("actual_fids", componentParts.first());
        for (int part = 1; part < componentParts.size(); ++part)
            outFeat.setAttribute(QString("act_fid%1").arg(part + 1), componentParts.at(part));

        // 写入 symbol_id 与 assoc_name（已存在的代码）
        if (link.symbolId.isEmpty())
            outFeat.setAttribute("symbol_id", QVariant());
        else
            outFeat.setAttribute("symbol_id", link.symbolId);

        outFeat.setAttribute("assoc_name", link.assoc_name);

        // 将要素写入文件
        if (!writer.addFeature(outFeat))
        {
            if (errMsg.isEmpty()) errMsg = writer.errorMessage();
            qWarning() << "错误: 写入要素失败，anno_fid:" << link.annoFid << "错误信息:" << writer.errorMessage();
            skipCount++;
        }
        else
        {
            successCount++;
        }
    }

    // --- 【修改】步骤 4: 根据配置决定是否输出未匹配的注记 ---
    //
    // 应用场景说明：
    // 1. 多标准图层共享一个实际图层（如一级河流_标注、二级河流_标注共享"水系_标注"）：
    //    - outputUnmatchedAnnotations = false（默认）
    //    - 未匹配的注记实际上属于其他标准图层，不应输出，避免误报
    //
    // 2. 标准图层与实际图层一一对应（如地理名称点符号及其注记）：
    //    - outputUnmatchedAnnotations = true（通过外部接口参数）
    //    - 未匹配的注记可能是算法问题或真实错误，需要输出以便检查
    //
    int unmatchedCount = 0;
    QgsMessageLog::logMessage(
        QString("临时注记匹配图层写出：matches=%1，matchedAnnoFids=%2，outputUnmatchedAnnotations=%3，outputPath=%4")
            .arg(matches.size())
            .arg(matchedAnnoFids.size())
            .arg(outputUnmatchedAnnotations ? "true" : "false", outputPath),
        "AnnotationCheck", Qgis::Info);

    if (outputUnmatchedAnnotations)
    {
        // 输出未匹配注记（用于一一对应场景）
        // 【新增】构建GDB空间索引，为未匹配注记通过几何位置反向查找所属面
        QgsSpatialIndex gdbSpatialIndex;
        QMap<qint64, QString> gdbFidToName;
        QMap<qint64, QgsGeometry> gdbFidToGeom;
        int gdbNameIdx = gdbLayer->fields().indexOf(QStringLiteral("名称"));
        if (gdbNameIdx < 0)
            gdbNameIdx = gdbLayer->fields().indexOf(QStringLiteral("NAME"));
        int annoTextIdx = annoLayer->fields().indexOf(QStringLiteral("text_conte"));
        {
            QgsFeatureIterator gdbIt = gdbLayer->getFeatures();
            QgsFeature gdbFeat;
            while (gdbIt.nextFeature(gdbFeat))
            {
                if (gdbFeat.geometry().isEmpty())
                    continue;
                // TYPE过滤：与匹配阶段保持一致，只纳入符合XML规则的要素
                QString lv = (gdbLevelIdx >= 0) ? gdbFeat.attribute(gdbLevelIdx).toString() : QString();
                if (gdbLevelIdx >= 0 && !matchesLevelValue(lv, levelValues))
                    continue;
                gdbSpatialIndex.addFeature(gdbFeat);
                gdbFidToGeom[gdbFeat.id()] = gdbFeat.geometry();
                if (gdbNameIdx >= 0)
                {
                    gdbFidToName[gdbFeat.id()] = gdbFeat.attribute(gdbNameIdx).toString().trimmed();
                }
            }
        }

        // 【2026-07-23】收集本类GDB所有非空名称的字符集（如高速路所有名称的汉字集合）
        // 用于后续空间兜底时判断：落在null-name段上的注记字，只有出现在标准名称中才算疑似
        QSet<QString> standardNameCharSet;
        for (auto it = gdbFidToName.constBegin(); it != gdbFidToName.constEnd(); ++it)
        {
            if (!it.value().isEmpty())
            {
                for (const QChar &ch : it.value())
                {
                    standardNameCharSet.insert(QString(ch));
                }
            }
        }
        qDebug().nospace() << "  [标准名称字集] 共 " << standardNameCharSet.size()
                           << " 个不同字符（来自 " << gdbFidToName.size() << " 个GDB要素）";

        int elementTypeIdx = annoLayer->fields().indexOf("element_ty");
        QgsFeatureIterator annoIt = annoLayer->getFeatures();
        QgsFeature annoFeat;

        while (annoIt.nextFeature(annoFeat))
        {
            if (excludedAnnoFids.contains(annoFeat.id()))
                continue;
            // 跳过已匹配的注记
            if (matchedAnnoFids.contains(annoFeat.id()))
            {
                continue;
            }

            // 跳过无效要素
            if (!annoFeat.isValid() || annoFeat.geometry().isEmpty())
            {
                continue;
            }

            // 跳过非注记要素（element_ty 不为 "text" 的要素）
            if (elementTypeIdx >= 0)
            {
                QString elementType = annoFeat.attribute(elementTypeIdx).toString().trimmed().toLower();
                if (elementType != "text")
                {
                    continue;
                }
            }

            QgsFeature outFeat(fields);
            outFeat.setGeometry(annoFeat.geometry());

            // 拷贝白名单中的原始属性
            for (const QString &fieldNameToCopy : fieldWhitelist)
            {
                if (fields.indexOf(fieldNameToCopy) != -1)
                {
                    outFeat.setAttribute(fieldNameToCopy, annoFeat.attribute(fieldNameToCopy));
                }
            }

            // match_status 和 match_score 在空间查找后根据字符是否在GDB名中区分赋值
            // 先留空，避免二义性
            outFeat.setAttribute("anno_fid", QVariant::fromValue(annoFeat.id()));
            outFeat.setAttribute("group_id", QVariant::fromValue(-1LL));
            outFeat.setAttribute("gdb_fid", QVariant::fromValue(-1LL));
            outFeat.setAttribute("gdb_fids", QVariant());
            outFeat.setAttribute("actual_fids", QVariant());
            outFeat.setAttribute("symbol_id", QVariant());
            // 【新增】通过空间位置反向查找未匹配注记所属的GDB面，填入assoc_name
            // 这样210阶段的面内路径就能正确收走同一面内多个同名注记
            // 【2026-07-23】新增GDB名称缺失疑似标记：
            //   注记落在GDB要素内但NAME字段为空 → 标记为"【GDB名称缺失-疑似】"供人工复查
            bool foundGdbFeature = false;
            bool gdbNameIsEmpty = false;
            QString spatialAssocName;
            {
                QgsGeometry annoGeom = annoFeat.geometry();
                QList<qint64> candidateFids = gdbSpatialIndex.intersects(annoGeom.boundingBox());
                for (qint64 gdbFid : candidateFids)
                {
                    auto geomIt = gdbFidToGeom.find(gdbFid);
                    if (geomIt == gdbFidToGeom.end())
                        continue;

                    bool isContained = geomIt->contains(annoGeom);

                    // 线要素：contains(点)几乎永远为false（点很难精确落在线上）
                    // 改用距离检查，用正常匹配的搜索半径阈值
                    if (!isContained && geomIt->type() == QgsWkbTypes::LineGeometry)
                    {
                        double dist = geomIt->distance(annoGeom);
                        if (dist < nearbyRadius)
                            isContained = true;
                    }

                    if (isContained)
                    {
                        foundGdbFeature = true;
                        auto nameIt = gdbFidToName.find(gdbFid);
                        if (nameIt != gdbFidToName.end())
                        {
                            spatialAssocName = nameIt.value();
                            if (spatialAssocName.isEmpty())
                                gdbNameIsEmpty = true;
                        }
                        break;
                    }
                }
            }
            if (!spatialAssocName.isEmpty())
            {
                outFeat.setAttribute("assoc_name", spatialAssocName);
                qDebug().nospace() << "  [空间归属] FID=" << annoFeat.id()
                                   << " 字=\"" << (annoTextIdx >= 0 ? annoFeat.attribute(annoTextIdx).toString() : QString())
                                   << "\" -> assoc_name=\"" << spatialAssocName << "\"";
            }
            else if (foundGdbFeature && gdbNameIsEmpty)
            {
                // 【2026-07-23新增】GDB要素存在但名称为空 → 检查字符是否在标准名称中出现过
                // 只有出现在同类GDB标准名称中的字才算疑似，避免输出无关字符
                QString annoChar = (annoTextIdx >= 0) ? annoFeat.attribute(annoTextIdx).toString() : QString();
                if (standardNameCharSet.contains(annoChar))
                {
                    outFeat.setAttribute("assoc_name", QString::fromUtf8("【GDB名称缺失-疑似】"));
                    qDebug().nospace() << "  [GDB名称缺失-疑似] FID=" << annoFeat.id()
                                       << " 字=\"" << annoChar
                                       << "\" -> 附近GDB要素名称为空，且字在标准名称字集中 → 标记为疑似";
                }
                else
                {
                    // 字符不在标准名称字集中，与该类GDB无关，静默跳过
                    outFeat.setAttribute("assoc_name", QVariant());
                    qDebug().nospace() << "  [GDB名称缺失-跳过] FID=" << annoFeat.id()
                                       << " 字=\"" << annoChar
                                       << "\" -> 不在标准名称字集中，跳过";
                }
            }
            else
            {
                outFeat.setAttribute("assoc_name", QVariant());
            }

            // ========== 区分 PARTIALLY_MATCHED vs UNMATCHED ==========
            // 如果注记字符在空间关联的GDB名称中 → PARTIALLY_MATCHED（属于该GDB但匹配不完整）
            // 如果注记字符不在GDB名称中或找不到GDB → UNMATCHED（完全无关）
            {
                bool charInGdbName = false;
                if (!spatialAssocName.isEmpty())
                {
                    QString annoChar = (annoTextIdx >= 0) ? annoFeat.attribute(annoTextIdx).toString().trimmed() : QString();
                    if (!annoChar.isEmpty() && spatialAssocName.contains(annoChar))
                        charInGdbName = true;
                }

                if (charInGdbName)
                {
                    outFeat.setAttribute("match_status", "PARTIALLY_MATCHED");
                    // 得分：字符覆盖率 = 1 / GDB名称长度，范围 [0.2, 0.7]
                    double covScore = spatialAssocName.isEmpty() ? 0.3 : qBound(0.2, 1.0 / (double)spatialAssocName.length(), 0.7);
                    outFeat.setAttribute("match_score", covScore);
                }
                else
                {
                    outFeat.setAttribute("match_status", "UNMATCHED");
                    outFeat.setAttribute("match_score", 0.0);
                }
            }

            // 将要素写入文件
            if (!writer.addFeature(outFeat))
            {
                qWarning() << "错误: 写入未匹配要素失败，anno_fid:" << annoFeat.id() << "错误信息:" << writer.errorMessage();
            }
            else
            {
                unmatchedCount++;
            }
        }
        QgsMessageLog::logMessage(
            QString("临时注记匹配图层写出：已写入未匹配注记，unmatchedCount=%1，outputPath=%2")
                .arg(unmatchedCount)
                .arg(outputPath),
            "AnnotationCheck", Qgis::Info);
        qDebug() << "信息: 已输出" << unmatchedCount << "个未匹配的注记（outputUnmatchedAnnotations=true）";
    }
    else
    {
        // 不输出未匹配注记，仅统计（用于多对一场景）
        int totalAnnoCount = 0;
        QgsFeatureIterator countIt = annoLayer->getFeatures();
        QgsFeature tempFeat;
        while (countIt.nextFeature(tempFeat))
        {
            totalAnnoCount++;
        }
        unmatchedCount = totalAnnoCount - matchedAnnoFids.size();

        if (unmatchedCount > 0)
        {
            QgsMessageLog::logMessage(
                QString("临时注记匹配图层写出：检测到未匹配注记但按开关不写入，unmatchedCount=%1，outputPath=%2")
                    .arg(unmatchedCount)
                    .arg(outputPath),
                "AnnotationCheck", Qgis::Info);
            qDebug() << "信息: 共有" << unmatchedCount << "个注记未匹配到当前标准图层，这些注记不会写入临时图层（outputUnmatchedAnnotations=false）。";
        }
    }

    QgsMessageLog::logMessage(
        QString("临时注记匹配图层写出完成：successCount=%1，unmatchedCount=%2，skipCount=%3，outputUnmatchedAnnotations=%4，outputPath=%5")
            .arg(successCount)
            .arg(unmatchedCount)
            .arg(skipCount)
            .arg(outputUnmatchedAnnotations ? "true" : "false", outputPath),
        "AnnotationCheck", Qgis::Info);

    qDebug() << "信息: 临时图层写入完成。已匹配写入: " << successCount << "个, 未匹配"
             << (outputUnmatchedAnnotations ? "写入" : "（未输出）") << ": " << unmatchedCount << "个, 跳过: " << skipCount << "个。";
    qDebug() << "信息: 输出文件路径: " << outputPath;

    if (skipCount > 0)
    {
        errMsg = QStringLiteral("临时匹配图层有%1条记录未能写出，结果不完整：%2").arg(skipCount).arg(errMsg);
        return false;
    }

    return true;
}
