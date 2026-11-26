#include "se_admin_name_annotation_check_algorithm.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QVector>
#include <QtXml/QDomDocument>
#include <QgsFeature.h>
#include <QgsField.h>
#include <QgsFeatureRequest.h>
#include <QgsFields.h>
#include <QgsGeometry.h>
#include <QgsPointXY.h>
#include <QgsSpatialIndex.h>
#include <QgsVectorLayer.h>
#include <QgsVectorFileWriter.h>
#include <QgsWkbTypes.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

struct AnnotationCandidate
{
    qint64 fid = -1;
    QString text;
    QString normalizedText;
    QgsPointXY anchor;
    QgsGeometry geometry;
    bool anchorValid = false;
    double fontSize = 0.0;
};

struct StandardAdminCandidate
{
    qint64 fid = -1;
    QString name;
    QString normalizedName;
    QgsGeometry geometry;
};

struct AdminAnnotationIssue
{
    QString type;
    QString expectedName;
    QString actualText;
    QString matchSource;
    QString matchNote;
    double similarity = 0.0;
    qint64 adminFid = -1;
    qint64 annotationFid = -1;
    qint64 gdbFid = -1;
    qint64 groupId = -1;
    QString groupText;
    int groupCount = 0;
};

struct AdminMatchDebugRecord
{
    qint64 adminFid = -1;
    qint64 annoFid = -1;
    qint64 gdbFid = -1;
    qint64 groupId = -1;
    QString textConte;
    QString groupText;
    QString assocName;
    QString matchStat;
    QString matchSource;
    QString matchNote;
    QString errorType;
    QString errorMsg;
    QString actualVal;
    QString expectVal;
    int groupCount = 0;
    int charSeq = 0;
    double matchScore = 0.0;
    double overlapArea = 0.0;
};

struct AnnotationPointDebugRecord
{
    qint64 annoFid = -1;
    qint64 adminFid = -1;
    qint64 gdbFid = -1;
    qint64 groupId = -1;
    QString textConte;
    QString groupText;
    QString assocName;
    QString matchStat;
    QString matchSource;
    QString matchNote;
    QString errorType;
    QString errorMsg;
    QString actualVal;
    QString expectVal;
    int groupCount = 0;
    int charSeq = 0;
    double matchScore = 0.0;
    int insideFlg = 0;
    QgsPointXY anchor;
};

struct StandardMatchDebugRecord
{
    qint64 gdbFid = -1;
    qint64 adminFid = -1;
    qint64 annoFid = -1;
    qint64 groupId = -1;
    QString textConte;
    QString groupText;
    QString assocName;
    QString matchStat;
    QString matchSource;
    QString matchNote;
    QString errorType;
    QString errorMsg;
    QString actualVal;
    QString expectVal;
    int groupCount = 0;
    int charSeq = 0;
    double matchScore = 0.0;
    double overlapArea = 0.0;
};

struct MergedSingleCharGroupRecord
{
    qint64 groupId = -1;
    qint64 adminFid = -1;
    qint64 gdbFid = -1;
    qint64 firstAnnoFid = -1;
    QString groupText;
    QString expectedName;
    QString matchStat;
    QString matchSource;
    QString matchNote;
    double matchScore = 0.0;
    int groupCount = 0;
    QString annoFids;
    QgsPointXY anchor;
};

struct PartialMatchResult
{
    bool matched = false;
    double score = 0.0;
    QString source;
    QString detail;
};

enum NameVariantType
{
    NameVariantOriginal = 0,
    NameVariantAlias = 1,
    NameVariantAbbreviation = 2,
    NameVariantNonstandardAbbreviation = 3
};

struct NameVariant
{
    QString normalizedText;
    QString displayText;
    int type = NameVariantOriginal;
    bool fromConfiguredDictionary = false;
};

static QMap<QString, QList<NameVariant>> g_projectConfiguredVariants;
static int g_projectAliasDictionaryEntryCount = 0;
static int g_projectAbbreviationDictionaryEntryCount = 0;
static int g_projectNonstandardDictionaryEntryCount = 0;

struct SingleCharGroupMatch
{
    bool matched = false;
    QList<qint64> fids;
    QString groupText;
    double score = 0.0;
};

struct SpatialAcceptanceResult
{
    bool accepted = false;
    bool inside = false;
    int insideFlag = 0;
    double distanceToAdmin = 0.0;
    double centroidDistance = 0.0;
    double nearestOtherAdminDistance = -1.0;
    QString note;
};

static QgsPointXY anchorPointFromGeometry(const QgsGeometry& geom);

static void copyCString(char* dest, size_t size, const QString& value)
{
    if (!dest || size == 0) return;
    QByteArray bytes = value.toUtf8();
    std::strncpy(dest, bytes.constData(), size - 1);
    dest[size - 1] = '\0';
}

static QString firstNonEmpty(const QStringList& candidates)
{
    for (const QString& value : candidates) {
        if (!value.trimmed().isEmpty()) {
            return value.trimmed();
        }
    }
    return QString();
}

static inline QString normToken(QString s)
{
    s.remove(QRegularExpression("\\s+"));
    s = s.trimmed().toLower();
    return s;
}

static QString stripOuterBrackets(const QString& s)
{
    QString t = s.trimmed();
    if (t.length() < 2) {
        return t;
    }

    static const QMap<QChar, QChar> pairs = {
        {QChar('('), QChar(')')},
        {QChar('['), QChar(']')},
        {QChar('{'), QChar('}')},
        {QChar('<'), QChar('>')},
        {QChar(0xFF08), QChar(0xFF09)},
        {QChar(0x3010), QChar(0x3011)},
        {QChar(0x300C), QChar(0x300D)},
        {QChar(0x300E), QChar(0x300F)}
    };

    bool changed = true;
    while (changed && t.length() >= 2) {
        changed = false;
        const QChar first = t.at(0);
        const QChar last = t.at(t.length() - 1);
        if (pairs.contains(first) && pairs.value(first) == last) {
            t = t.mid(1, t.length() - 2).trimmed();
            changed = true;
        }
    }
    return t;
}

static QChar toNormalizedDigitChar(QChar ch)
{
    if (ch.isDigit()) {
        return ch;
    }

    static const QMap<QChar, QChar> cn2digit = {
        {QChar(0x96F6), QChar('0')},
        {QChar(0x4E00), QChar('1')},
        {QChar(0x4E8C), QChar('2')},
        {QChar(0x4E24), QChar('2')},
        {QChar(0x4E09), QChar('3')},
        {QChar(0x56DB), QChar('4')},
        {QChar(0x4E94), QChar('5')},
        {QChar(0x516D), QChar('6')},
        {QChar(0x4E03), QChar('7')},
        {QChar(0x516B), QChar('8')},
        {QChar(0x4E5D), QChar('9')}
    };

    return cn2digit.contains(ch) ? cn2digit.value(ch) : ch;
}

static QString normalizeNumberChars(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (QChar ch : s) {
        out.append(toNormalizedDigitChar(ch));
    }
    return out;
}

static QString normalizeNameText(const QString& text)
{
    QString normalized = stripOuterBrackets(text);
    normalized = normalizeNumberChars(normalized);
    normalized = normToken(normalized);
    return normalized;
}

static void appendNameVariant(QList<NameVariant>& variants, const QString& text, int type)
{
    const QString normalized = normalizeNameText(text);
    if (normalized.isEmpty()) {
        return;
    }
    for (const NameVariant& variant : variants) {
        if (variant.normalizedText == normalized) {
            return;
        }
    }

    NameVariant variant;
    variant.normalizedText = normalized;
    variant.displayText = text.trimmed();
    variant.type = type;
    variants.append(variant);
}

static void appendNormalizedNameVariant(
    QList<NameVariant>& variants,
    const QString& normalizedText,
    const QString& displayText,
    int type)
{
    const QString normalized = normalizedText.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    for (const NameVariant& variant : variants) {
        if (variant.normalizedText == normalized) {
            return;
        }
    }

    NameVariant variant;
    variant.normalizedText = normalized;
    variant.displayText = displayText.trimmed();
    variant.type = type;
    variants.append(variant);
}

static QString classifyMatchSourceLabel(
    int leftType,
    int rightType,
    bool leftFromConfiguredDictionary,
    bool rightFromConfiguredDictionary)
{
    if (leftFromConfiguredDictionary || rightFromConfiguredDictionary) {
        return QStringLiteral("项目字典命中");
    }
    if (leftType == NameVariantOriginal && rightType == NameVariantOriginal) {
        return QStringLiteral("原名命中");
    }
    return QStringLiteral("内建简称命中");
}

static QString configuredDictionaryTypeLabel(int type)
{
    if (type == NameVariantAlias) {
        return QStringLiteral("项目别名字典");
    }
    if (type == NameVariantNonstandardAbbreviation) {
        return QStringLiteral("项目不规范简称字典");
    }
    return QStringLiteral("项目简称字典");
}

static QString builtinVariantRuleLabel(int leftType, int rightType)
{
    if (leftType == NameVariantAlias || rightType == NameVariantAlias) {
        return QStringLiteral("内建别名规则");
    }
    if (leftType == NameVariantNonstandardAbbreviation ||
        rightType == NameVariantNonstandardAbbreviation) {
        return QStringLiteral("内建不规范简称规则");
    }
    if (leftType == NameVariantAbbreviation || rightType == NameVariantAbbreviation) {
        return QStringLiteral("内建简称规则");
    }
    return QStringLiteral("原名");
}

static QString buildMatchDetail(const QString& ruleLabel, const QString& expectedText, const QString& actualText)
{
    return QStringLiteral("%1: %2 -> %3").arg(ruleLabel, expectedText.trimmed(), actualText.trimmed());
}

static QString appendDetailNote(const QString& baseNote, const QString& extraNote)
{
    if (baseNote.trimmed().isEmpty()) {
        return extraNote.trimmed();
    }
    if (extraNote.trimmed().isEmpty()) {
        return baseNote.trimmed();
    }
    return baseNote.trimmed() + QStringLiteral(" | ") + extraNote.trimmed();
}

static QStringList splitDictionaryItems(const QString& text)
{
    QStringList items =
        text.split(QRegularExpression("[\\|,，、/／]"), QString::SkipEmptyParts);
    QStringList cleaned;
    for (const QString& item : items) {
        const QString trimmed = item.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed)) {
            cleaned.append(trimmed);
        }
    }
    return cleaned;
}

static void clearConfiguredNameVariantDictionary()
{
    g_projectConfiguredVariants.clear();
    g_projectAliasDictionaryEntryCount = 0;
    g_projectAbbreviationDictionaryEntryCount = 0;
    g_projectNonstandardDictionaryEntryCount = 0;
}

static void registerConfiguredVariantGroup(const QString& canonicalText, const QStringList& variantTexts, int type)
{
    QStringList members;
    const QString canonical = canonicalText.trimmed();
    if (!canonical.isEmpty()) {
        members.append(canonical);
    }
    for (const QString& variantText : variantTexts) {
        const QString trimmed = variantText.trimmed();
        if (!trimmed.isEmpty() && !members.contains(trimmed)) {
            members.append(trimmed);
        }
    }

    if (members.size() < 2) {
        return;
    }

    for (const QString& source : members) {
        const QString sourceNormalized = normalizeNameText(source);
        if (sourceNormalized.isEmpty()) {
            continue;
        }

        QList<NameVariant>& mappedVariants = g_projectConfiguredVariants[sourceNormalized];
        for (const QString& target : members) {
            const QString targetNormalized = normalizeNameText(target);
            if (targetNormalized.isEmpty() || targetNormalized == sourceNormalized) {
                continue;
            }

            bool exists = false;
            for (const NameVariant& existing : mappedVariants) {
                if (existing.normalizedText == targetNormalized) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                NameVariant mappedVariant;
                mappedVariant.normalizedText = targetNormalized;
                mappedVariant.displayText = target.trimmed();
                mappedVariant.type = type;
                mappedVariant.fromConfiguredDictionary = true;
                mappedVariants.append(mappedVariant);
            }
        }
    }
}

static int parseConfiguredVariantDictionary(const QString& ruleValue, int type)
{
    int parsedEntryCount = 0;
    const QStringList entries =
        ruleValue.split(QRegularExpression("[;；\\r\\n]+"), QString::SkipEmptyParts);
    for (const QString& entry : entries) {
        const QString trimmedEntry = entry.trimmed();
        if (trimmedEntry.isEmpty()) {
            continue;
        }

        int separatorPos = trimmedEntry.indexOf('=');
        if (separatorPos < 0) {
            separatorPos = trimmedEntry.indexOf(QChar(0xFF1D));
        }
        if (separatorPos < 0) {
            separatorPos = trimmedEntry.indexOf(':');
        }
        if (separatorPos < 0) {
            separatorPos = trimmedEntry.indexOf(QChar(0xFF1A));
        }
        if (separatorPos <= 0) {
            continue;
        }

        const QString canonicalText = trimmedEntry.left(separatorPos).trimmed();
        const QString variantsText = trimmedEntry.mid(separatorPos + 1).trimmed();
        const QStringList variantItems = splitDictionaryItems(variantsText);
        if (canonicalText.isEmpty() || variantItems.isEmpty()) {
            continue;
        }

        registerConfiguredVariantGroup(canonicalText, variantItems, type);
        ++parsedEntryCount;
    }

    return parsedEntryCount;
}

static QString findAdminNameVariantCsvPath(const QString& schemaXmlPath)
{
    QStringList startDirs;
    if (!schemaXmlPath.trimmed().isEmpty()) {
        startDirs.append(QFileInfo(schemaXmlPath).absolutePath());
    }
    startDirs.append(QDir::currentPath());

    const QString fileName = QStringLiteral("政区名称检查_标准地名简称别名字典模板.csv");
    for (const QString& startDir : startDirs) {
        QDir dir(startDir);
        for (int i = 0; i < 10; ++i) {
            const QString candidate = dir.filePath(fileName);
            if (QFileInfo::exists(candidate)) {
                return candidate;
            }
            if (!dir.cdUp()) {
                break;
            }
        }
    }
    return QString();
}

static QStringList splitCsvLineSimple(const QString& line)
{
    QStringList values;
    QString current;
    bool inQuote = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (inQuote && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                current.append(ch);
                ++i;
            } else {
                inQuote = !inQuote;
            }
        } else if (ch == QLatin1Char(',') && !inQuote) {
            values.append(current.trimmed());
            current.clear();
        } else {
            current.append(ch);
        }
    }
    values.append(current.trimmed());
    return values;
}

static void loadAdminNameVariantCsvDictionary(const QString& schemaXmlPath)
{
    const QString csvPath = findAdminNameVariantCsvPath(schemaXmlPath);
    if (csvPath.isEmpty()) {
        return;
    }

    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QString content = QString::fromUtf8(file.readAll());
    file.close();
    if (content.startsWith(QChar(0xFEFF))) {
        content.remove(0, 1);
    }

    const QStringList lines = content.split(QRegularExpression("\\r?\\n"), QString::SkipEmptyParts);
    if (lines.size() < 2) {
        return;
    }

    const QStringList headers = splitCsvLineSimple(lines.first());
    auto indexOfHeader = [&](const QString& name) -> int {
        for (int i = 0; i < headers.size(); ++i) {
            if (headers.at(i).trimmed() == name) {
                return i;
            }
        }
        return -1;
    };

    const int standardIdx = indexOfHeader(QStringLiteral("标准名称"));
    const int abbrIdx = indexOfHeader(QStringLiteral("简称"));
    const int aliasIdx = indexOfHeader(QStringLiteral("别名列表"));
    const int levelIdx = indexOfHeader(QStringLiteral("行政级别"));
    const int enabledIdx = indexOfHeader(QStringLiteral("是否启用"));
    if (standardIdx < 0) {
        return;
    }

    for (int i = 1; i < lines.size(); ++i) {
        const QStringList values = splitCsvLineSimple(lines.at(i));
        auto valueAt = [&](int idx) -> QString {
            return (idx >= 0 && idx < values.size()) ? values.at(idx).trimmed() : QString();
        };

        const QString enabled = valueAt(enabledIdx).toLower();
        if (!enabled.isEmpty() && enabled != QStringLiteral("1") &&
            enabled != QStringLiteral("true") && enabled != QStringLiteral("是")) {
            continue;
        }

        const QString level = valueAt(levelIdx);
        if (!level.isEmpty() && level != QStringLiteral("县级")) {
            continue;
        }

        const QString standardName = valueAt(standardIdx);
        if (standardName.isEmpty()) {
            continue;
        }

        const QString abbr = valueAt(abbrIdx);
        if (!abbr.isEmpty()) {
            registerConfiguredVariantGroup(standardName, QStringList() << abbr, NameVariantAbbreviation);
            ++g_projectAbbreviationDictionaryEntryCount;
        }

        const QStringList aliases = splitDictionaryItems(valueAt(aliasIdx));
        if (!aliases.isEmpty()) {
            registerConfiguredVariantGroup(standardName, aliases, NameVariantAlias);
            g_projectAliasDictionaryEntryCount += aliases.size();
        }
    }
}

static QString removeBracketedSegments(const QString& text)
{
    QString cleaned = text;
    cleaned.remove(QRegularExpression("[\\(\\[<\\{\\uFF08\\u3010\\u300C\\u300E][^\\)\\]>\\}\\uFF09\\u3011\\u300D\\u300F]*[\\)\\]>\\}\\uFF09\\u3011\\u300D\\u300F]"));
    return cleaned.trimmed();
}

static QStringList extractBracketContents(const QString& text)
{
    QStringList aliases;
    const QRegularExpression bracketPattern(
        "[\\(\\[<\\{\\uFF08\\u3010\\u300C\\u300E]([^\\)\\]>\\}\\uFF09\\u3011\\u300D\\u300F]{2,})[\\)\\]>\\}\\uFF09\\u3011\\u300D\\u300F]");
    QRegularExpressionMatchIterator it = bracketPattern.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString inner = match.captured(1).trimmed();
        if (!inner.isEmpty()) {
            aliases.append(inner);
        }
    }
    return aliases;
}

static QString stripKnownAdminSuffix(const QString& text)
{
    static const QStringList suffixes = QStringList()
        << QStringLiteral("特别行政区")
        << QStringLiteral("自治州")
        << QStringLiteral("自治县")
        << QStringLiteral("自治旗")
        << QStringLiteral("自治区")
        << QStringLiteral("地区")
        << QStringLiteral("盟")
        << QStringLiteral("省")
        << QStringLiteral("市")
        << QStringLiteral("州")
        << QStringLiteral("县")
        << QStringLiteral("区")
        << QStringLiteral("旗");

    const QString trimmed = stripOuterBrackets(text).trimmed();
    for (const QString& suffix : suffixes) {
        if (trimmed.endsWith(suffix) && trimmed.length() > suffix.length()) {
            return trimmed.left(trimmed.length() - suffix.length()).trimmed();
        }
    }
    return QString();
}

static QString stripTrailingEthnicDesignators(const QString& text)
{
    static const QStringList ethnicTokens = QStringList()
        << QStringLiteral("柯尔克孜族")
        << QStringLiteral("乌孜别克族")
        << QStringLiteral("哈萨克族")
        << QStringLiteral("维吾尔族")
        << QStringLiteral("塔吉克族")
        << QStringLiteral("朝鲜族")
        << QStringLiteral("俄罗斯族")
        << QStringLiteral("鄂温克族")
        << QStringLiteral("鄂伦春族")
        << QStringLiteral("达斡尔族")
        << QStringLiteral("土家族")
        << QStringLiteral("布依族")
        << QStringLiteral("仡佬族")
        << QStringLiteral("傈僳族")
        << QStringLiteral("景颇族")
        << QStringLiteral("仫佬族")
        << QStringLiteral("布朗族")
        << QStringLiteral("撒拉族")
        << QStringLiteral("毛南族")
        << QStringLiteral("锡伯族")
        << QStringLiteral("阿昌族")
        << QStringLiteral("普米族")
        << QStringLiteral("保安族")
        << QStringLiteral("裕固族")
        << QStringLiteral("塔塔尔族")
        << QStringLiteral("独龙族")
        << QStringLiteral("赫哲族")
        << QStringLiteral("门巴族")
        << QStringLiteral("珞巴族")
        << QStringLiteral("基诺族")
        << QStringLiteral("蒙古族")
        << QStringLiteral("回族")
        << QStringLiteral("藏族")
        << QStringLiteral("苗族")
        << QStringLiteral("彝族")
        << QStringLiteral("壮族")
        << QStringLiteral("满族")
        << QStringLiteral("侗族")
        << QStringLiteral("瑶族")
        << QStringLiteral("白族")
        << QStringLiteral("哈尼族")
        << QStringLiteral("傣族")
        << QStringLiteral("黎族")
        << QStringLiteral("佤族")
        << QStringLiteral("畲族")
        << QStringLiteral("拉祜族")
        << QStringLiteral("水族")
        << QStringLiteral("东乡族")
        << QStringLiteral("纳西族")
        << QStringLiteral("土族")
        << QStringLiteral("羌族")
        << QStringLiteral("怒族")
        << QStringLiteral("京族")
        << QStringLiteral("德昂族")
        << QStringLiteral("鄂温克")
        << QStringLiteral("鄂伦春")
        << QStringLiteral("达斡尔");

    QString stripped = stripOuterBrackets(text).trimmed();
    bool changed = true;
    while (changed) {
        changed = false;
        for (const QString& token : ethnicTokens) {
            if (stripped.endsWith(token) && stripped.length() > token.length()) {
                const QString candidate = stripped.left(stripped.length() - token.length()).trimmed();
                if (!candidate.isEmpty()) {
                    stripped = candidate;
                    changed = true;
                    break;
                }
            }
        }
    }
    return stripped;
}

static QList<NameVariant> buildAdminNameVariants(const QString& text)
{
    QList<NameVariant> variants;
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return variants;
    }

    appendNameVariant(variants, trimmed, NameVariantOriginal);

    const QString noBracketText = removeBracketedSegments(trimmed);
    if (!noBracketText.isEmpty() && noBracketText != trimmed) {
        appendNameVariant(variants, noBracketText, NameVariantOriginal);
    }

    const QStringList separatorAliases =
        noBracketText.split(QRegularExpression("[/／,，、;；]"), QString::SkipEmptyParts);
    if (separatorAliases.size() > 1) {
        for (const QString& alias : separatorAliases) {
            appendNameVariant(variants, alias, NameVariantAlias);
        }
    }

    for (const QString& alias : extractBracketContents(trimmed)) {
        appendNameVariant(variants, alias, NameVariantAlias);
    }

    const QString baseText = noBracketText.isEmpty() ? trimmed : noBracketText;
    const QString shortName = stripKnownAdminSuffix(baseText);
    if (!shortName.isEmpty() && shortName.length() >= 2) {
        appendNameVariant(variants, shortName, NameVariantAbbreviation);
    }

    static const QMap<QString, QString> specialRegionAbbreviations = {
        {QStringLiteral("内蒙古自治区"), QStringLiteral("内蒙古")},
        {QStringLiteral("广西壮族自治区"), QStringLiteral("广西")},
        {QStringLiteral("西藏自治区"), QStringLiteral("西藏")},
        {QStringLiteral("宁夏回族自治区"), QStringLiteral("宁夏")},
        {QStringLiteral("新疆维吾尔自治区"), QStringLiteral("新疆")},
        {QStringLiteral("香港特别行政区"), QStringLiteral("香港")},
        {QStringLiteral("澳门特别行政区"), QStringLiteral("澳门")}
    };

    const QString normalizedBaseText = stripOuterBrackets(baseText).trimmed();
    if (specialRegionAbbreviations.contains(normalizedBaseText)) {
        appendNameVariant(
            variants,
            specialRegionAbbreviations.value(normalizedBaseText),
            NameVariantAbbreviation);
    }

    struct AutonomousSuffixRule
    {
        QString fullSuffix;
        QString shortSuffix;
    };

    static const QList<AutonomousSuffixRule> autonomousSuffixRules = QList<AutonomousSuffixRule>()
        << AutonomousSuffixRule{QStringLiteral("自治州"), QStringLiteral("州")}
        << AutonomousSuffixRule{QStringLiteral("自治县"), QStringLiteral("县")}
        << AutonomousSuffixRule{QStringLiteral("自治旗"), QStringLiteral("旗")};

    for (const AutonomousSuffixRule& rule : autonomousSuffixRules) {
        if (!normalizedBaseText.endsWith(rule.fullSuffix) ||
            normalizedBaseText.length() <= rule.fullSuffix.length()) {
            continue;
        }

        const QString fullBase = normalizedBaseText.left(
            normalizedBaseText.length() - rule.fullSuffix.length()).trimmed();
        if (fullBase.isEmpty()) {
            continue;
        }

        const QString strippedEthnicBase = stripTrailingEthnicDesignators(fullBase);
        const QString abbreviatedBase =
            (!strippedEthnicBase.isEmpty() && strippedEthnicBase.length() >= 2) ? strippedEthnicBase : fullBase;

        appendNameVariant(variants, abbreviatedBase, NameVariantAbbreviation);
        appendNameVariant(variants, abbreviatedBase + rule.shortSuffix, NameVariantAbbreviation);
        appendNameVariant(variants, abbreviatedBase + rule.fullSuffix, NameVariantNonstandardAbbreviation);
    }

    for (int i = 0; i < variants.size(); ++i) {
        const QList<NameVariant> configuredVariants =
            g_projectConfiguredVariants.value(variants.at(i).normalizedText);
        for (const NameVariant& configuredVariant : configuredVariants) {
            appendNormalizedNameVariant(
                variants,
                configuredVariant.normalizedText,
                configuredVariant.displayText,
                configuredVariant.type);
        }
    }

    return variants;
}

static double variantMatchScore(int leftType, int rightType, const SE_AdminNameAnnotationRuleConfig& config)
{
    if (leftType == NameVariantOriginal && rightType == NameVariantOriginal) {
        return 1.0;
    }
    if (leftType == NameVariantAlias || rightType == NameVariantAlias) {
        return config.fAliasMatchScore;
    }
    if (leftType == NameVariantNonstandardAbbreviation ||
        rightType == NameVariantNonstandardAbbreviation) {
        return config.fNonstandardAbbreviationScore;
    }
    if (leftType == NameVariantAbbreviation || rightType == NameVariantAbbreviation) {
        return config.fAbbreviationMatchScore;
    }
    return config.fAbbreviationMatchScore;
}

static bool nameContainsCharEquivalent(const QString& name, const QString& singleCharStr)
{
    if (singleCharStr.isEmpty()) {
        return false;
    }

    const QString strippedName = stripOuterBrackets(name);
    const QString strippedChar = stripOuterBrackets(singleCharStr);
    const QString normName = normalizeNumberChars(strippedName);
    const QString normChar = normalizeNumberChars(strippedChar);
    return normName.contains(normChar);
}

static bool singleCharEquivalent(const QString& leftChar, const QString& rightChar)
{
    if (leftChar.isEmpty() || rightChar.isEmpty()) {
        return false;
    }
    return normalizeNumberChars(stripOuterBrackets(leftChar)) ==
           normalizeNumberChars(stripOuterBrackets(rightChar));
}

static double jaroWinklerSimilarity(const QString& s1, const QString& s2)
{
    if (s1.isEmpty() && s2.isEmpty()) {
        return 1.0;
    }
    if (s1.isEmpty() || s2.isEmpty()) {
        return 0.0;
    }

    const int len1 = s1.length();
    const int len2 = s2.length();
    const int matchDistance = std::max(0, std::max(len1, len2) / 2 - 1);

    QVector<bool> s1Matches(len1, false);
    QVector<bool> s2Matches(len2, false);
    int matches = 0;

    for (int i = 0; i < len1; ++i) {
        const int start = std::max(0, i - matchDistance);
        const int end = std::min(i + matchDistance + 1, len2);
        for (int j = start; j < end; ++j) {
            if (!s2Matches[j] && s1.at(i) == s2.at(j)) {
                s1Matches[i] = true;
                s2Matches[j] = true;
                ++matches;
                break;
            }
        }
    }

    if (matches == 0) {
        return 0.0;
    }

    double transpositions = 0.0;
    int k = 0;
    for (int i = 0; i < len1; ++i) {
        if (!s1Matches[i]) {
            continue;
        }
        while (!s2Matches[k]) {
            ++k;
        }
        if (s1.at(i) != s2.at(k)) {
            transpositions += 0.5;
        }
        ++k;
    }

    const double jaro = (
        static_cast<double>(matches) / len1 +
        static_cast<double>(matches) / len2 +
        static_cast<double>(matches - transpositions) / matches) / 3.0;

    int prefix = 0;
    for (int i = 0; i < std::min(len1, len2) && i < 4; ++i) {
        if (s1.at(i) == s2.at(i)) {
            ++prefix;
        } else {
            break;
        }
    }

    return jaro + prefix * 0.1 * (1.0 - jaro);
}

static PartialMatchResult checkPartialMatch(
    const QString& annoText,
    const QString& gdbText,
    const SE_AdminNameAnnotationRuleConfig& config)
{
    if (annoText.isEmpty() || gdbText.isEmpty()) {
        return {false, 0.0, QString(), QString()};
    }

    const QString annoStripped = stripOuterBrackets(annoText);
    const QString gdbStripped = stripOuterBrackets(gdbText);
    const QString normAnno = normalizeNumberChars(annoStripped);
    const QString normGdb = normalizeNumberChars(gdbStripped);

    if (normAnno == normGdb) {
        return {
            true,
            1.0,
            QStringLiteral("原名命中"),
            buildMatchDetail(QStringLiteral("原名"), gdbText, annoText)
        };
    }

    const QList<NameVariant> annoVariants = buildAdminNameVariants(annoText);
    const QList<NameVariant> gdbVariants = buildAdminNameVariants(gdbText);
    double bestVariantScore = 0.0;
    QString bestVariantSource;
    QString bestVariantDetail;
    for (const NameVariant& annoVariant : annoVariants) {
        for (const NameVariant& gdbVariant : gdbVariants) {
            if (annoVariant.normalizedText != gdbVariant.normalizedText) {
                continue;
            }
            const double currentScore =
                variantMatchScore(annoVariant.type, gdbVariant.type, config);
            if (currentScore > bestVariantScore) {
                bestVariantScore = currentScore;
                bestVariantSource = classifyMatchSourceLabel(
                    annoVariant.type,
                    gdbVariant.type,
                    annoVariant.fromConfiguredDictionary,
                    gdbVariant.fromConfiguredDictionary);
                const QString ruleLabel =
                    (annoVariant.fromConfiguredDictionary || gdbVariant.fromConfiguredDictionary)
                        ? configuredDictionaryTypeLabel(
                            annoVariant.fromConfiguredDictionary ? annoVariant.type : gdbVariant.type)
                        : builtinVariantRuleLabel(annoVariant.type, gdbVariant.type);
                bestVariantDetail = buildMatchDetail(
                    ruleLabel,
                    gdbVariant.displayText.isEmpty() ? gdbText : gdbVariant.displayText,
                    annoVariant.displayText.isEmpty() ? annoText : annoVariant.displayText);
            }
        }
    }
    if (bestVariantScore > 0.0) {
        return {true, bestVariantScore, bestVariantSource, bestVariantDetail};
    }

    if ((normAnno.endsWith(normGdb) || normAnno.startsWith(normGdb)) && normGdb.length() >= 2) {
        return {true,
            config.fPrefixSuffixBaseScore +
                config.fPrefixSuffixLengthWeight * (static_cast<double>(normGdb.length()) / normAnno.length()),
            QStringLiteral("内建简称命中"),
            buildMatchDetail(QStringLiteral("前后缀规则"), gdbText, annoText)};
    }
    if ((normGdb.endsWith(normAnno) || normGdb.startsWith(normAnno)) && normAnno.length() >= 2) {
        return {true,
            config.fPrefixSuffixBaseScore +
                config.fPrefixSuffixLengthWeight * (static_cast<double>(normAnno.length()) / normGdb.length()),
            QStringLiteral("内建简称命中"),
            buildMatchDetail(QStringLiteral("前后缀规则"), gdbText, annoText)};
    }

    QSet<QChar> annoChars = QSet<QChar>(normAnno.begin(), normAnno.end());
    QSet<QChar> gdbChars = QSet<QChar>(normGdb.begin(), normGdb.end());
    const int minLen = qMin(annoChars.size(), gdbChars.size());
    if (minLen <= 1) {
        return {false, 0.0, QString(), QString()};
    }

    QSet<QChar> intersection = annoChars;
    intersection.intersect(gdbChars);
    const int intersectionSize = intersection.size();
    const double overlapRatio = static_cast<double>(intersectionSize) / minLen;
    const double gdbCoverageRatio = static_cast<double>(intersectionSize) / gdbChars.size();
    const double annoCoverageRatio = static_cast<double>(intersectionSize) / annoChars.size();

    if (overlapRatio >= config.fCharsetOverlapRatio &&
        gdbCoverageRatio >= config.fCharsetCoverageRatio &&
        annoCoverageRatio >= config.fCharsetCoverageRatio) {
        const double sequenceScore = jaroWinklerSimilarity(normAnno, normGdb);
        if (sequenceScore < config.fJaroSequenceMin) {
            return {false, 0.0, QString(), QString()};
        }

        const double charsetScore = (overlapRatio + gdbCoverageRatio + annoCoverageRatio) / 3.0;
        const double finalScore = charsetScore * 0.7 + sequenceScore * 0.3;
        return {true,
            qMin(finalScore, 0.8),
            QStringLiteral("相似规则命中"),
            buildMatchDetail(QStringLiteral("相似规则"), gdbText, annoText)};
    }

    return {false, 0.0, QString(), QString()};
}

static SingleCharGroupMatch tryBuildStandardDrivenCharGroup(
    const QList<AnnotationCandidate>& candidates,
    const QString& expectedName,
    const SE_AdminNameAnnotationRuleConfig& config,
    int scaleDenominator)
{
    SingleCharGroupMatch result;
    const QString strippedExpected = stripOuterBrackets(expectedName).trimmed();
    if (candidates.isEmpty() || strippedExpected.isEmpty()) {
        return result;
    }

    QStringList expectedChars;
    for (const QChar& ch : strippedExpected) {
        if (!ch.isSpace()) {
            expectedChars.append(QString(ch));
        }
    }
    if (expectedChars.size() < 2) {
        return result;
    }

    struct OrderedCandidate
    {
        AnnotationCandidate candidate;
    };

    QList<OrderedCandidate> orderedBase;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
    bool firstPoint = true;

    for (const AnnotationCandidate& candidate : candidates) {
        if (!candidate.anchorValid || candidate.text.trimmed().length() != 1) {
            continue;
        }
        if (!nameContainsCharEquivalent(strippedExpected, candidate.text)) {
            continue;
        }

        if (firstPoint) {
            minX = maxX = candidate.anchor.x();
            minY = maxY = candidate.anchor.y();
            firstPoint = false;
        } else {
            minX = qMin(minX, candidate.anchor.x());
            maxX = qMax(maxX, candidate.anchor.x());
            minY = qMin(minY, candidate.anchor.y());
            maxY = qMax(maxY, candidate.anchor.y());
        }

        OrderedCandidate ordered;
        ordered.candidate = candidate;
        orderedBase.append(ordered);
    }

    if (orderedBase.size() < expectedChars.size()) {
        return result;
    }

    const bool horizontalDominant = (maxX - minX) >= (maxY - minY);
    QList<OrderedCandidate> orderedCandidates = orderedBase;
    if (horizontalDominant) {
        std::sort(orderedCandidates.begin(), orderedCandidates.end(), [](const OrderedCandidate& a, const OrderedCandidate& b) {
            if (!qFuzzyCompare(a.candidate.anchor.x(), b.candidate.anchor.x())) {
                return a.candidate.anchor.x() < b.candidate.anchor.x();
            }
            return a.candidate.anchor.y() < b.candidate.anchor.y();
        });
    } else {
        std::sort(orderedCandidates.begin(), orderedCandidates.end(), [](const OrderedCandidate& a, const OrderedCandidate& b) {
            if (!qFuzzyCompare(a.candidate.anchor.y(), b.candidate.anchor.y())) {
                return a.candidate.anchor.y() > b.candidate.anchor.y();
            }
            return a.candidate.anchor.x() < b.candidate.anchor.x();
        });
    }

    QList<int> selectedIndexes;
    QList<double> selectedPrimaries;
    QList<double> selectedSecondaries;
    QList<double> selectedFontSizes;
    QStringList selectedTexts;

    std::function<bool(int, int)> backtrack = [&](int expectedIndex, int startIndex) -> bool {
        if (expectedIndex == expectedChars.size()) {
            return true;
        }

        for (int i = startIndex; i < orderedCandidates.size(); ++i) {
            const AnnotationCandidate& candidate = orderedCandidates.at(i).candidate;
            if (!singleCharEquivalent(expectedChars.at(expectedIndex), candidate.text)) {
                continue;
            }

            const double primary = horizontalDominant ? candidate.anchor.x() : candidate.anchor.y();
            const double secondary = horizontalDominant ? candidate.anchor.y() : candidate.anchor.x();

            if (!selectedSecondaries.isEmpty()) {
                double minSecondary = selectedSecondaries.first();
                double maxSecondary = selectedSecondaries.first();
                for (double val : selectedSecondaries) {
                    minSecondary = qMin(minSecondary, val);
                    maxSecondary = qMax(maxSecondary, val);
                }
                minSecondary = qMin(minSecondary, secondary);
                maxSecondary = qMax(maxSecondary, secondary);

                const double primarySpan = std::abs(primary - selectedPrimaries.first());
                if (primarySpan > 0.0) {
                    const double secondarySpan = maxSecondary - minSecondary;
                    if (secondarySpan > primarySpan * 0.35) {
                        continue;
                    }
                }
            }

            if (!selectedPrimaries.isEmpty()) {
                const double prevPrimary = selectedPrimaries.last();
                const double gap = std::abs(primary - prevPrimary);
                const double prevFontSize = selectedFontSizes.last();
                if (prevFontSize > 0.0 && scaleDenominator > 0) {
                    const double absoluteThreshold =
                        ((prevFontSize * 0.352778 * config.fSingleCharSpacingRatio) / 1000.0) * scaleDenominator;
                    if (gap > absoluteThreshold) {
                        continue;
                    }
                }
            }

            if (!selectedFontSizes.isEmpty()) {
                const double prevFontSize = selectedFontSizes.first();
                if (prevFontSize > 0.0 && candidate.fontSize > 0.0 &&
                    std::abs(candidate.fontSize - prevFontSize) > config.fSingleCharFontTolerance) {
                    continue;
                }
            }

            selectedIndexes.append(i);
            selectedPrimaries.append(primary);
            selectedSecondaries.append(secondary);
            selectedFontSizes.append(candidate.fontSize);
            selectedTexts.append(candidate.text);

            if (backtrack(expectedIndex + 1, i + 1)) {
                return true;
            }

            selectedIndexes.removeLast();
            selectedPrimaries.removeLast();
            selectedSecondaries.removeLast();
            selectedFontSizes.removeLast();
            selectedTexts.removeLast();
        }

        return false;
    };

    if (!backtrack(0, 0)) {
        return result;
    }

    QList<qint64> fids;
    for (int idx : selectedIndexes) {
        fids.append(orderedCandidates.at(idx).candidate.fid);
    }

    result.matched = true;
    result.fids = fids;
    result.groupText = selectedTexts.join(QString());
    result.score = qMin(0.999, config.fSingleCharGroupScore + 0.04);
    return result;
}

static SingleCharGroupMatch tryBuildSingleCharGroup(
    const QList<AnnotationCandidate>& candidates,
    const QString& expectedName,
    const SE_AdminNameAnnotationRuleConfig& config,
    int scaleDenominator)
{
    SingleCharGroupMatch result;
    const QString strippedExpected = stripOuterBrackets(expectedName).trimmed();
    if (candidates.isEmpty() || strippedExpected.isEmpty()) {
        return result;
    }

    QStringList expectedChars;
    for (const QChar& ch : strippedExpected) {
        if (!ch.isSpace()) {
            expectedChars.append(QString(ch));
        }
    }
    if (expectedChars.size() < 2) {
        return result;
    }

    struct OrderedCandidate
    {
        AnnotationCandidate candidate;
        double primary = 0.0;
        double secondary = 0.0;
    };

    QList<OrderedCandidate> orderedBase;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
    bool firstPoint = true;

    for (const AnnotationCandidate& candidate : candidates) {
        if (!candidate.anchorValid || candidate.text.trimmed().length() != 1) {
            continue;
        }
        if (!nameContainsCharEquivalent(strippedExpected, candidate.text)) {
            continue;
        }

        if (firstPoint) {
            minX = maxX = candidate.anchor.x();
            minY = maxY = candidate.anchor.y();
            firstPoint = false;
        } else {
            minX = qMin(minX, candidate.anchor.x());
            maxX = qMax(maxX, candidate.anchor.x());
            minY = qMin(minY, candidate.anchor.y());
            maxY = qMax(maxY, candidate.anchor.y());
        }

        OrderedCandidate ordered;
        ordered.candidate = candidate;
        orderedBase.append(ordered);
    }

    if (orderedBase.size() < expectedChars.size()) {
        return result;
    }

    const bool horizontalDominant = (maxX - minX) >= (maxY - minY);
    QList<QList<OrderedCandidate>> orders;
    QList<OrderedCandidate> forward = orderedBase;
    if (horizontalDominant) {
        std::sort(forward.begin(), forward.end(), [](const OrderedCandidate& a, const OrderedCandidate& b) {
            if (!qFuzzyCompare(a.candidate.anchor.x(), b.candidate.anchor.x())) {
                return a.candidate.anchor.x() < b.candidate.anchor.x();
            }
            return a.candidate.anchor.y() < b.candidate.anchor.y();
        });
    } else {
        std::sort(forward.begin(), forward.end(), [](const OrderedCandidate& a, const OrderedCandidate& b) {
            if (!qFuzzyCompare(a.candidate.anchor.y(), b.candidate.anchor.y())) {
                return a.candidate.anchor.y() > b.candidate.anchor.y();
            }
            return a.candidate.anchor.x() < b.candidate.anchor.x();
        });
    }

    QList<OrderedCandidate> reverse = forward;
    std::reverse(reverse.begin(), reverse.end());
    orders.append(forward);
    orders.append(reverse);

    double bestRankScore = -1.0;
    double bestCompactness = std::numeric_limits<double>::max();
    for (const QList<OrderedCandidate>& orderedCandidates : orders) {
        QList<int> currentIndexes;
        std::function<void(int, int)> backtrack = [&](int expectedIndex, int startIndex) {
            if (expectedIndex == expectedChars.size()) {
                double minPrimary = 0.0;
                double maxPrimary = 0.0;
                double minSecondary = 0.0;
                double maxSecondary = 0.0;
                bool first = true;
                QList<qint64> fids;
                QStringList texts;
                QList<OrderedCandidate> selectedOrdered;
                for (int idx : currentIndexes) {
                    const OrderedCandidate& ordered = orderedCandidates.at(idx);
                    const double primary = horizontalDominant ? ordered.candidate.anchor.x() : ordered.candidate.anchor.y();
                    const double secondary = horizontalDominant ? ordered.candidate.anchor.y() : ordered.candidate.anchor.x();
                    if (first) {
                        minPrimary = maxPrimary = primary;
                        minSecondary = maxSecondary = secondary;
                        first = false;
                    } else {
                        minPrimary = qMin(minPrimary, primary);
                        maxPrimary = qMax(maxPrimary, primary);
                        minSecondary = qMin(minSecondary, secondary);
                        maxSecondary = qMax(maxSecondary, secondary);
                    }
                    fids.append(ordered.candidate.fid);
                    texts.append(ordered.candidate.text);
                    selectedOrdered.append(ordered);
                }

                double gapConsistencyScore = 1.0;
                if (selectedOrdered.size() > 1) {
                    const double firstFontSize = selectedOrdered.first().candidate.fontSize;
                    if (firstFontSize > 0.0) {
                        for (int i = 1; i < selectedOrdered.size(); ++i) {
                            const double currentFontSize = selectedOrdered.at(i).candidate.fontSize;
                            if (currentFontSize > 0.0 &&
                                std::abs(currentFontSize - firstFontSize) > config.fSingleCharFontTolerance) {
                                return;
                            }
                        }
                    }

                    QList<double> gaps;
                    for (int i = 0; i < selectedOrdered.size() - 1; ++i) {
                        const double primary1 = horizontalDominant ? selectedOrdered.at(i).candidate.anchor.x() : selectedOrdered.at(i).candidate.anchor.y();
                        const double primary2 = horizontalDominant ? selectedOrdered.at(i + 1).candidate.anchor.x() : selectedOrdered.at(i + 1).candidate.anchor.y();
                        gaps.append(std::abs(primary2 - primary1));
                    }

                    if (!gaps.isEmpty()) {
                        double gapSum = 0.0;
                        double maxGap = 0.0;
                        for (double gap : gaps) {
                            gapSum += gap;
                            maxGap = qMax(maxGap, gap);
                        }
                        const double avgGap = gapSum / gaps.size();
                        if (avgGap > 0.0) {
                            for (double gap : gaps) {
                                if (gap > avgGap * config.fSingleCharSpacingRatio) {
                                    return;
                                }
                            }
                            gapConsistencyScore = qBound(0.0, avgGap / maxGap, 1.0);
                        } else if (scaleDenominator > 0 && firstFontSize > 0.0) {
                            const double absoluteThreshold =
                                ((firstFontSize * 0.352778 * config.fSingleCharSpacingRatio) / 1000.0) * scaleDenominator;
                            for (double gap : gaps) {
                                if (gap > absoluteThreshold) {
                                    return;
                                }
                            }
                        }
                    }
                }

                const double primarySpan = qMax(0.0, maxPrimary - minPrimary);
                const double secondarySpan = qMax(0.0, maxSecondary - minSecondary);
                const double compactness = maxPrimary - minPrimary;
                const double alignmentScore =
                    primarySpan > 0.0 ? qBound(0.0, 1.0 - (secondarySpan / primarySpan), 1.0) : 1.0;
                const double rankScore = gapConsistencyScore * 0.6 + alignmentScore * 0.4;

                if (rankScore > bestRankScore ||
                    (qFuzzyCompare(rankScore + 1.0, bestRankScore + 1.0) && compactness < bestCompactness)) {
                    bestRankScore = rankScore;
                    bestCompactness = compactness;
                    result.matched = true;
                    result.fids = fids;
                    result.groupText = texts.join(QString());
                    result.score = qMin(
                        0.999,
                        config.fSingleCharGroupScore + gapConsistencyScore * 0.03 + alignmentScore * 0.02);
                }
                return;
            }

            for (int i = startIndex; i < orderedCandidates.size(); ++i) {
                const QString candidateText = orderedCandidates.at(i).candidate.text;
                if (!singleCharEquivalent(expectedChars.at(expectedIndex), candidateText)) {
                    continue;
                }
                currentIndexes.append(i);
                backtrack(expectedIndex + 1, i + 1);
                currentIndexes.removeLast();
            }
        };

        backtrack(0, 0);
    }

    return result;
}

static SingleCharGroupMatch tryBuildSpatialSortedCharGroup(
    const QList<AnnotationCandidate>& candidates,
    const QString& expectedName,
    const SE_AdminNameAnnotationRuleConfig& config,
    int scaleDenominator)
{
    SingleCharGroupMatch result;
    const QString strippedExpected = stripOuterBrackets(expectedName).trimmed();
    if (candidates.isEmpty() || strippedExpected.length() < 2) {
        return result;
    }

    struct CharItem
    {
        AnnotationCandidate candidate;
        double x = 0.0;
        double y = 0.0;
    };

    QList<CharItem> chars;
    for (const AnnotationCandidate& candidate : candidates) {
        if (!candidate.anchorValid || candidate.text.trimmed().length() != 1) {
            continue;
        }
        CharItem item;
        item.candidate = candidate;
        item.x = candidate.anchor.x();
        item.y = candidate.anchor.y();
        chars.append(item);
    }
    if (chars.size() < strippedExpected.length()) {
        return result;
    }

    std::sort(chars.begin(), chars.end(), [](const CharItem& a, const CharItem& b) {
        if (std::abs(a.x - b.x) < 1.0) {
            return a.y < b.y;
        }
        return a.x < b.x;
    });

    QList<double> gaps;
    for (int i = 0; i < chars.size() - 1; ++i) {
        const double dx = chars.at(i + 1).x - chars.at(i).x;
        const double dy = chars.at(i + 1).y - chars.at(i).y;
        gaps.append(std::sqrt(dx * dx + dy * dy));
    }

    QList<QList<CharItem>> spatialGroups;
    if (gaps.isEmpty()) {
        spatialGroups.append(chars);
    } else {
        QList<double> sortedGaps = gaps;
        std::sort(sortedGaps.begin(), sortedGaps.end());
        const double medianGap = sortedGaps.at(sortedGaps.size() / 2);
        double splitThreshold = std::max(medianGap * 3.0, 5000.0);
        if (scaleDenominator > 0) {
            splitThreshold = std::max(splitThreshold, static_cast<double>(scaleDenominator) * 0.1);
        }

        QList<CharItem> currentGroup;
        currentGroup.append(chars.first());
        for (int i = 0; i < gaps.size(); ++i) {
            if (gaps.at(i) > splitThreshold) {
                spatialGroups.append(currentGroup);
                currentGroup.clear();
            }
            currentGroup.append(chars.at(i + 1));
        }
        if (!currentGroup.isEmpty()) {
            spatialGroups.append(currentGroup);
        }
    }

    double bestScore = -1.0;
    for (QList<CharItem> group : spatialGroups) {
        if (group.size() < strippedExpected.length()) {
            continue;
        }

        double minX = group.first().x;
        double maxX = group.first().x;
        double minY = group.first().y;
        double maxY = group.first().y;
        double firstFontSize = group.first().candidate.fontSize;
        bool fontConsistent = true;
        for (const CharItem& item : group) {
            minX = qMin(minX, item.x);
            maxX = qMax(maxX, item.x);
            minY = qMin(minY, item.y);
            maxY = qMax(maxY, item.y);
            if (firstFontSize > 0.0 && item.candidate.fontSize > 0.0 &&
                std::abs(firstFontSize - item.candidate.fontSize) > config.fSingleCharFontTolerance) {
                fontConsistent = false;
                break;
            }
        }
        if (!fontConsistent) {
            continue;
        }

        const bool horizontal = (maxX - minX) >= (maxY - minY);
        std::sort(group.begin(), group.end(), [horizontal](const CharItem& a, const CharItem& b) {
            if (horizontal) {
                if (std::abs(a.x - b.x) >= 1.0) {
                    return a.x < b.x;
                }
                return a.y < b.y;
            }
            if (std::abs(a.y - b.y) >= 1.0) {
                return a.y > b.y;
            }
            return a.x < b.x;
        });

        QString formedWord;
        QList<qint64> fids;
        for (const CharItem& item : group) {
            formedWord += item.candidate.text;
            fids.append(item.candidate.fid);
        }

        QString reversedExpected;
        for (int i = strippedExpected.size() - 1; i >= 0; --i) {
            reversedExpected.append(strippedExpected.at(i));
        }
        if (formedWord != strippedExpected && formedWord != reversedExpected) {
            continue;
        }

        double gapScore = 1.0;
        if (group.size() > 1) {
            QList<double> groupGaps;
            for (int i = 0; i < group.size() - 1; ++i) {
                const double primary1 = horizontal ? group.at(i).x : group.at(i).y;
                const double primary2 = horizontal ? group.at(i + 1).x : group.at(i + 1).y;
                groupGaps.append(std::abs(primary2 - primary1));
            }
            double gapSum = 0.0;
            double maxGap = 0.0;
            for (double gap : groupGaps) {
                gapSum += gap;
                maxGap = qMax(maxGap, gap);
            }
            if (maxGap > 0.0) {
                gapScore = qBound(0.0, (gapSum / groupGaps.size()) / maxGap, 1.0);
            }
        }

        const double primarySpan = horizontal ? (maxX - minX) : (maxY - minY);
        const double secondarySpan = horizontal ? (maxY - minY) : (maxX - minX);
        const double alignmentScore = primarySpan > 0.0
            ? qBound(0.0, 1.0 - (secondarySpan / primarySpan), 1.0)
            : 1.0;
        const double score = qMin(0.999, config.fSingleCharGroupScore + gapScore * 0.03 + alignmentScore * 0.02);
        if (score > bestScore) {
            bestScore = score;
            result.matched = true;
            result.fids = fids;
            result.groupText = formedWord;
            result.score = score;
        }
    }

    return result;
}

static bool spatialResultHasSupplement(const SpatialAcceptanceResult& spatial)
{
    return spatial.accepted && spatial.insideFlag != 1;
}

static QgsPointXY anchorPointFromGeometry(const QgsGeometry& geom)
{
    if (geom.isEmpty()) {
        return QgsPointXY();
    }

    QgsGeometry centroid = geom.centroid();
    if (!centroid.isEmpty()) {
        return centroid.asPoint();
    }
    return geom.boundingBox().center();
}

static int fieldIndexWithFallback(QgsVectorLayer* layer, const QStringList& candidates)
{
    if (!layer) {
        return -1;
    }

    const QgsFields fields = layer->fields();
    for (const QString& candidate : candidates) {
        const QString trimmed = candidate.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const int exactIndex = fields.indexOf(trimmed);
        if (exactIndex >= 0) {
            return exactIndex;
        }
    }

    for (const QString& candidate : candidates) {
        const QString trimmed = candidate.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QString normalizedCandidate = trimmed.toLower();
        for (int i = 0; i < fields.count(); ++i) {
            if (fields.at(i).name().trimmed().toLower() == normalizedCandidate) {
                return i;
            }
        }
    }

    return -1;
}

static QgsVectorLayer* loadLayer(const QString& path, QString& errMsg)
{
    QFileInfo fi(path);
    if (!fi.exists()) {
        errMsg = QStringLiteral("图层文件不存在: %1").arg(path);
        return nullptr;
    }

    QgsVectorLayer* layer = new QgsVectorLayer(path, fi.completeBaseName(), "ogr");
    if (!layer->isValid()) {
        errMsg = QStringLiteral("图层加载失败: %1").arg(path);
        delete layer;
        return nullptr;
    }

    return layer;
}

static SpatialAcceptanceResult evaluateAnnotationSpatialRelation(
    qint64 currentAdminFid,
    const QgsGeometry& adminGeom,
    const QgsPointXY& anchor,
    double toleranceMapUnits,
    const QgsSpatialIndex& actualAdminIndex,
    const QMap<qint64, QgsGeometry>& actualAdminGeometryByFid,
    const SE_AdminNameAnnotationRuleConfig& config,
    const QgsGeometry* annotationGeom = nullptr,
    bool allowBoundaryOverlap = false,
    bool allowOutsideShift = true)
{
    SpatialAcceptanceResult result;
    if (adminGeom.isEmpty()) {
        return result;
    }

    const QgsGeometry anchorGeom = QgsGeometry::fromPointXY(anchor);
    if (adminGeom.contains(anchorGeom) || adminGeom.intersects(anchorGeom)) {
        result.accepted = true;
        result.inside = true;
        result.insideFlag = 1;
        return result;
    }

    if (allowBoundaryOverlap && annotationGeom && !annotationGeom->isEmpty() && adminGeom.intersects(*annotationGeom)) {
        result.accepted = true;
        result.inside = false;
        result.insideFlag = 3;
        result.note = QStringLiteral("错误补充验证: 注记几何与行政边界重合/相交");
        return result;
    }

    if (!allowOutsideShift) {
        return result;
    }

    const QgsGeometry& distanceGeom = (annotationGeom && !annotationGeom->isEmpty()) ? *annotationGeom : anchorGeom;
    result.distanceToAdmin = adminGeom.distance(distanceGeom);
    if (toleranceMapUnits <= 0.0 || result.distanceToAdmin > toleranceMapUnits) {
        return result;
    }

    const QgsPointXY centroidPoint = anchorPointFromGeometry(adminGeom);
    result.centroidDistance = centroidPoint.distance(anchor);
    const QgsRectangle bbox = adminGeom.boundingBox();
    const double bboxHalfDiagonal =
        0.5 * std::sqrt(bbox.width() * bbox.width() + bbox.height() * bbox.height());
    const double centroidDistanceLimit = qMax(
        toleranceMapUnits * config.fReasonableShiftCentroidRatio,
        bboxHalfDiagonal * config.fReasonableShiftCentroidRatio);

    if (centroidDistanceLimit <= 0.0 || result.centroidDistance > centroidDistanceLimit) {
        return result;
    }

    QgsRectangle neighborSearchRect(anchor.x(), anchor.y(), anchor.x(), anchor.y());
    neighborSearchRect.grow(toleranceMapUnits);
    const QList<QgsFeatureId> nearbyAdminIds = actualAdminIndex.intersects(neighborSearchRect);
    for (QgsFeatureId otherAdminFid : nearbyAdminIds) {
        if (otherAdminFid == currentAdminFid || !actualAdminGeometryByFid.contains(otherAdminFid)) {
            continue;
        }

        const QgsGeometry& otherAdminGeom = actualAdminGeometryByFid.value(otherAdminFid);
        if (otherAdminGeom.isEmpty()) {
            continue;
        }

        if (otherAdminGeom.contains(anchorGeom) || otherAdminGeom.intersects(anchorGeom)) {
            return result;
        }

        const double otherDistance = otherAdminGeom.distance(distanceGeom);
        if (result.nearestOtherAdminDistance < 0.0 || otherDistance < result.nearestOtherAdminDistance) {
            result.nearestOtherAdminDistance = otherDistance;
        }
    }

    if (result.nearestOtherAdminDistance >= 0.0 &&
        result.nearestOtherAdminDistance <= result.distanceToAdmin * config.fReasonableShiftNeighborRatio) {
        return result;
    }

    result.accepted = true;
    result.inside = false;
    result.insideFlag = 2;
    result.note = QStringLiteral("合理移位: 边界外扩%1, 中心距%2")
        .arg(QString::number(result.distanceToAdmin, 'f', 3))
        .arg(QString::number(result.centroidDistance, 'f', 3));
    if (result.nearestOtherAdminDistance >= 0.0) {
        result.note += QStringLiteral(", 邻区距%1")
            .arg(QString::number(result.nearestOtherAdminDistance, 'f', 3));
    }
    return result;
}

static QString issueDescription(const AdminAnnotationIssue& issue)
{
    if (issue.type == QStringLiteral("missing_annotation")) {
        return QStringLiteral("行政区面内未找到对应注记");
    }
    if (issue.type == QStringLiteral("wrong_annotation")) {
        return QStringLiteral("行政区注记名称与标准名称不一致");
    }
    return QStringLiteral("未匹配到可用的标准行政区名称");
}

static QString issueTypeToken(const QString& issueType)
{
    if (issueType == QStringLiteral("missing_annotation") || issueType == QStringLiteral("MISSING_ANNO")) {
        return QStringLiteral("MISSING_ANNO");
    }
    if (issueType == QStringLiteral("wrong_annotation") || issueType == QStringLiteral("WRONG_ANNO")) {
        return QStringLiteral("WRONG_ANNO");
    }
    if (issueType == QStringLiteral("missing_standard_name") || issueType == QStringLiteral("MISSING_STANDARD")) {
        return QStringLiteral("MISSING_STANDARD");
    }
    return QStringLiteral("OTHER_ERR");
}

static QString issueMatchStatus(const QString& issueType)
{
    if (issueType == QStringLiteral("wrong_annotation")) {
        return QStringLiteral("PARTIALLY_MATCHED");
    }
    if (issueType == QStringLiteral("missing_annotation")) {
        return QStringLiteral("UNMATCHED");
    }
    if (issueType == QStringLiteral("missing_standard_name")) {
        return QStringLiteral("UNMATCHED");
    }
    return QStringLiteral("UNMATCHED");
}

static QString currentCheckTime()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm");
}

static void removeVectorOutputFiles(const QString& outputPath, const QString& driverName)
{
    if (outputPath.trimmed().isEmpty()) {
        return;
    }

    if (driverName.compare(QStringLiteral("ESRI Shapefile"), Qt::CaseInsensitive) == 0) {
        const QFileInfo info(outputPath);
        const QDir dir = info.dir();
        const QString baseName = info.completeBaseName();
        const QStringList suffixes = QStringList()
            << QStringLiteral("shp")
            << QStringLiteral("shx")
            << QStringLiteral("dbf")
            << QStringLiteral("prj")
            << QStringLiteral("cpg")
            << QStringLiteral("qpj")
            << QStringLiteral("fix");
        for (const QString& suffix : suffixes) {
            QFile::remove(dir.filePath(baseName + QStringLiteral(".") + suffix));
        }
        return;
    }

    QFile::remove(outputPath);
}

static QgsFields createLegacyMatchFields(bool includeInsideFlg, bool includeOverArea)
{
    QgsFields outputFields;
    outputFields.append(QgsField("anno_fid", QVariant::LongLong));
    outputFields.append(QgsField("group_id", QVariant::LongLong));
    outputFields.append(QgsField("admin_fid", QVariant::LongLong));
    outputFields.append(QgsField("gdb_fid", QVariant::LongLong));
    outputFields.append(QgsField("text_conte", QVariant::String, "String", 100));
    outputFields.append(QgsField("group_text", QVariant::String, "String", 100));
    outputFields.append(QgsField("char_seq", QVariant::Int));
    outputFields.append(QgsField("group_cnt", QVariant::Int));
    outputFields.append(QgsField("assoc_name", QVariant::String, "String", 100));
    outputFields.append(QgsField("match_stat", QVariant::String, "String", 30));
    outputFields.append(QgsField("match_src", QVariant::String, "String", 30));
    outputFields.append(QgsField("match_note", QVariant::String, "String", 254));
    outputFields.append(QgsField("error_type", QVariant::String, "String", 50));
    outputFields.append(QgsField("error_msg", QVariant::String, "String", 254));
    outputFields.append(QgsField("actual_val", QVariant::String, "String", 100));
    outputFields.append(QgsField("expect_val", QVariant::String, "String", 100));
    outputFields.append(QgsField("match_scor", QVariant::Double, "Double", 20, 6));
    if (includeInsideFlg) {
        outputFields.append(QgsField("inside_flg", QVariant::Int));
    }
    if (includeOverArea) {
        outputFields.append(QgsField("over_area", QVariant::Double, "Double", 20, 6));
    }
    outputFields.append(QgsField("check_time", QVariant::String, "String", 20));
    return outputFields;
}

static bool writeIssueLayer(
    const QString& outputPath,
    const QString& driverName,
    QgsVectorLayer* adminLayer,
    const QMap<qint64, QgsGeometry>& adminGeometryByFid,
    const QList<AdminAnnotationIssue>& issues,
    QString& errorMessage)
{
    if (outputPath.trimmed().isEmpty() || !adminLayer) {
        errorMessage = QStringLiteral("错误结果输出路径或行政区图层无效");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.dir().path());
    removeVectorOutputFiles(outputPath, driverName);

    const QgsFields outputFields = createLegacyMatchFields(false, false);
    const QString checkTime = currentCheckTime();

    QgsVectorFileWriter writer(
        outputPath,
        "UTF-8",
        outputFields,
        adminLayer->wkbType(),
        adminLayer->crs(),
        driverName);

    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        errorMessage = writer.errorMessage();
        return false;
    }

    for (const AdminAnnotationIssue& issue : issues) {
        if (!adminGeometryByFid.contains(issue.adminFid)) {
            continue;
        }

        QgsFeature outputFeature(outputFields);
        outputFeature.setGeometry(adminGeometryByFid.value(issue.adminFid));

        QgsAttributes attrs;
        attrs.append(issue.annotationFid >= 0 ? QVariant::fromValue(issue.annotationFid) : QVariant());
        attrs.append(issue.groupId >= 0 ? QVariant::fromValue(issue.groupId) : QVariant());
        attrs.append(issue.adminFid >= 0 ? QVariant::fromValue(issue.adminFid) : QVariant());
        attrs.append(issue.gdbFid >= 0 ? QVariant::fromValue(issue.gdbFid) : QVariant());
        attrs.append(issue.actualText);
        attrs.append(issue.groupText);
        attrs.append(issue.groupId >= 0 ? 1 : 0);
        attrs.append(issue.groupCount);
        attrs.append(issue.expectedName);
        attrs.append(issueMatchStatus(issue.type));
        attrs.append(issue.matchSource);
        attrs.append(issue.matchNote);
        attrs.append(issueTypeToken(issue.type));
        attrs.append(issueDescription(issue));
        attrs.append(issue.actualText);
        attrs.append(issue.expectedName);
        attrs.append(issue.similarity);
        attrs.append(checkTime);
        outputFeature.setAttributes(attrs);

        if (!writer.addFeature(outputFeature)) {
            errorMessage = writer.errorMessage();
            return false;
        }
    }

    return true;
}

static bool writeAdminDebugLayer(
    const QString& outputPath,
    QgsVectorLayer* adminLayer,
    const QMap<qint64, QgsGeometry>& adminGeometryByFid,
    const QList<AdminMatchDebugRecord>& records,
    QString& errorMessage)
{
    if (outputPath.trimmed().isEmpty() || !adminLayer) {
        errorMessage = QStringLiteral("行政区中间结果输出路径或图层无效");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.dir().path());
    removeVectorOutputFiles(outputPath, QStringLiteral("ESRI Shapefile"));

    const QgsFields outputFields = createLegacyMatchFields(false, true);
    const QString checkTime = currentCheckTime();
    QgsVectorFileWriter writer(
        outputPath,
        "UTF-8",
        outputFields,
        adminLayer->wkbType(),
        adminLayer->crs(),
        "ESRI Shapefile");

    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        errorMessage = writer.errorMessage();
        return false;
    }

    for (const AdminMatchDebugRecord& record : records) {
        if (!adminGeometryByFid.contains(record.adminFid)) {
            continue;
        }

        QgsFeature feature(outputFields);
        feature.setGeometry(adminGeometryByFid.value(record.adminFid));
        QgsAttributes attrs;
        attrs << (record.annoFid >= 0 ? QVariant::fromValue(record.annoFid) : QVariant())
              << (record.groupId >= 0 ? QVariant::fromValue(record.groupId) : QVariant())
              << (record.adminFid >= 0 ? QVariant::fromValue(record.adminFid) : QVariant())
              << (record.gdbFid >= 0 ? QVariant::fromValue(record.gdbFid) : QVariant())
              << record.textConte
              << record.groupText
              << record.charSeq
              << record.groupCount
              << record.assocName
              << record.matchStat
              << record.matchSource
              << record.matchNote
              << record.errorType
              << record.errorMsg
              << record.actualVal
              << record.expectVal
              << record.matchScore
              << record.overlapArea
              << checkTime;
        feature.setAttributes(attrs);
        if (!writer.addFeature(feature)) {
            errorMessage = writer.errorMessage();
            return false;
        }
    }

    return true;
}

static bool writeAnnotationPointDebugLayer(
    const QString& outputPath,
    QgsVectorLayer* adminLayer,
    const QList<AnnotationPointDebugRecord>& records,
    QString& errorMessage)
{
    if (outputPath.trimmed().isEmpty() || !adminLayer) {
        errorMessage = QStringLiteral("注记点中间结果输出路径或图层无效");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.dir().path());
    removeVectorOutputFiles(outputPath, QStringLiteral("ESRI Shapefile"));

    const QgsFields outputFields = createLegacyMatchFields(true, false);
    const QString checkTime = currentCheckTime();
    QgsVectorFileWriter writer(
        outputPath,
        "UTF-8",
        outputFields,
        QgsWkbTypes::Point,
        adminLayer->crs(),
        "ESRI Shapefile");

    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        errorMessage = writer.errorMessage();
        return false;
    }

    for (const AnnotationPointDebugRecord& record : records) {
        QgsFeature feature(outputFields);
        feature.setGeometry(QgsGeometry::fromPointXY(record.anchor));
        QgsAttributes attrs;
        attrs << (record.annoFid >= 0 ? QVariant::fromValue(record.annoFid) : QVariant())
              << (record.groupId >= 0 ? QVariant::fromValue(record.groupId) : QVariant())
              << (record.adminFid >= 0 ? QVariant::fromValue(record.adminFid) : QVariant())
              << (record.gdbFid >= 0 ? QVariant::fromValue(record.gdbFid) : QVariant())
              << record.textConte
              << record.groupText
              << record.charSeq
              << record.groupCount
              << record.assocName
              << record.matchStat
              << record.matchSource
              << record.matchNote
              << record.errorType
              << record.errorMsg
              << record.actualVal
              << record.expectVal
              << record.matchScore
              << record.insideFlg
              << checkTime;
        feature.setAttributes(attrs);
        if (!writer.addFeature(feature)) {
            errorMessage = writer.errorMessage();
            return false;
        }
    }

    return true;
}

static bool writeMergedSingleCharGroupLayer(
    const QString& outputPath,
    QgsVectorLayer* adminLayer,
    const QList<MergedSingleCharGroupRecord>& records,
    QString& errorMessage)
{
    if (outputPath.trimmed().isEmpty() || !adminLayer) {
        errorMessage = QStringLiteral("合并单字注记中间结果输出路径或图层无效");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.dir().path());
    removeVectorOutputFiles(outputPath, QStringLiteral("ESRI Shapefile"));

    QgsFields outputFields;
    outputFields.append(QgsField("group_id", QVariant::LongLong));
    outputFields.append(QgsField("admin_fid", QVariant::LongLong));
    outputFields.append(QgsField("gdb_fid", QVariant::LongLong));
    outputFields.append(QgsField("anno_fid", QVariant::LongLong));
    outputFields.append(QgsField("group_text", QVariant::String, "String", 100));
    outputFields.append(QgsField("expect_val", QVariant::String, "String", 100));
    outputFields.append(QgsField("group_cnt", QVariant::Int));
    outputFields.append(QgsField("anno_fids", QVariant::String, "String", 254));
    outputFields.append(QgsField("match_stat", QVariant::String, "String", 30));
    outputFields.append(QgsField("match_src", QVariant::String, "String", 30));
    outputFields.append(QgsField("match_note", QVariant::String, "String", 254));
    outputFields.append(QgsField("match_scor", QVariant::Double, "Double", 20, 6));
    outputFields.append(QgsField("check_time", QVariant::String, "String", 20));

    const QString checkTime = currentCheckTime();
    QgsVectorFileWriter writer(
        outputPath,
        "UTF-8",
        outputFields,
        QgsWkbTypes::Point,
        adminLayer->crs(),
        "ESRI Shapefile");

    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        errorMessage = writer.errorMessage();
        return false;
    }

    for (const MergedSingleCharGroupRecord& record : records) {
        QgsFeature feature(outputFields);
        feature.setGeometry(QgsGeometry::fromPointXY(record.anchor));
        QgsAttributes attrs;
        attrs << (record.groupId >= 0 ? QVariant::fromValue(record.groupId) : QVariant())
              << (record.adminFid >= 0 ? QVariant::fromValue(record.adminFid) : QVariant())
              << (record.gdbFid >= 0 ? QVariant::fromValue(record.gdbFid) : QVariant())
              << (record.firstAnnoFid >= 0 ? QVariant::fromValue(record.firstAnnoFid) : QVariant())
              << record.groupText
              << record.expectedName
              << record.groupCount
              << record.annoFids
              << record.matchStat
              << record.matchSource
              << record.matchNote
              << record.matchScore
              << checkTime;
        feature.setAttributes(attrs);
        if (!writer.addFeature(feature)) {
            errorMessage = writer.errorMessage();
            return false;
        }
    }

    return true;
}

static bool writeStandardDebugLayer(
    const QString& outputPath,
    QgsVectorLayer* gdbLayer,
    const QMap<qint64, QgsGeometry>& gdbGeometryByFid,
    const QList<StandardMatchDebugRecord>& records,
    QString& errorMessage)
{
    if (outputPath.trimmed().isEmpty() || !gdbLayer) {
        errorMessage = QStringLiteral("标准面中间结果输出路径或图层无效");
        return false;
    }

    const QFileInfo outputInfo(outputPath);
    QDir().mkpath(outputInfo.dir().path());
    removeVectorOutputFiles(outputPath, QStringLiteral("ESRI Shapefile"));

    const QgsFields outputFields = createLegacyMatchFields(false, true);
    const QString checkTime = currentCheckTime();
    QgsVectorFileWriter writer(
        outputPath,
        "UTF-8",
        outputFields,
        gdbLayer->wkbType(),
        gdbLayer->crs(),
        "ESRI Shapefile");

    if (writer.hasError() != QgsVectorFileWriter::NoError) {
        errorMessage = writer.errorMessage();
        return false;
    }

    for (const StandardMatchDebugRecord& record : records) {
        if (!gdbGeometryByFid.contains(record.gdbFid)) {
            continue;
        }

        QgsFeature feature(outputFields);
        feature.setGeometry(gdbGeometryByFid.value(record.gdbFid));
        QgsAttributes attrs;
        attrs << (record.annoFid >= 0 ? QVariant::fromValue(record.annoFid) : QVariant())
              << (record.groupId >= 0 ? QVariant::fromValue(record.groupId) : QVariant())
              << (record.adminFid >= 0 ? QVariant::fromValue(record.adminFid) : QVariant())
              << (record.gdbFid >= 0 ? QVariant::fromValue(record.gdbFid) : QVariant())
              << record.textConte
              << record.groupText
              << record.charSeq
              << record.groupCount
              << record.assocName
              << record.matchStat
              << record.matchSource
              << record.matchNote
              << record.errorType
              << record.errorMsg
              << record.actualVal
              << record.expectVal
              << record.matchScore
              << record.overlapArea
              << checkTime;
        feature.setAttributes(attrs);
        if (!writer.addFeature(feature)) {
            errorMessage = writer.errorMessage();
            return false;
        }
    }

    return true;
}

static void writeJsonResult(
    const char* szOutputJsonPath,
    bool success,
    bool passed,
    const QString& infoText,
    const QJsonObject& inputInfo,
    const QJsonObject& errors)
{
    QJsonObject item;
    item["id"] = "211";
    item["name"] = QStringLiteral("政区名称注记错误/缺失检查");
    item["status"] = success ? QStringLiteral("质检成功") : QStringLiteral("质检失败");
    item["err_exist"] = passed ? QStringLiteral("false") : QStringLiteral("true");

    QJsonObject info = inputInfo;
    if (!infoText.isEmpty()) {
        info["description"] = infoText;
    }
    item["info"] = info;

    if (!errors.isEmpty()) {
        item["errors"] = errors;
    }

    QJsonObject root;
    root["check_item"] = item;
    root["ok"] = success;
    root["passed"] = passed;

    QFile jsonFile(QString::fromUtf8(szOutputJsonPath));
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        jsonFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        jsonFile.close();
    }
}

SE_Error ParseXmlForAdminNameAnnotation(
    const char* szSchemaXmlPath,
    SE_AdminNameAnnotationRuleConfig& config)
{
    clearConfiguredNameVariantDictionary();
    std::memset(&config, 0, sizeof(SE_AdminNameAnnotationRuleConfig));
    copyCString(config.szGdbLabelField, sizeof(config.szGdbLabelField), "XQ");
    copyCString(config.szAnnoTextField, sizeof(config.szAnnoTextField), "text_conte");
    config.fSimilarityThreshold = 0.92;
    config.fAllowOutsideDistanceMm = 2.0;
    config.fPrefixSuffixBaseScore = 0.85;
    config.fPrefixSuffixLengthWeight = 0.15;
    config.fCharsetOverlapRatio = 0.80;
    config.fCharsetCoverageRatio = 0.60;
    config.fJaroSequenceMin = 0.50;
    config.fSingleCharEquivalentScore = 0.60;
    config.fSingleCharGroupScore = 0.95;
    config.fSingleCharGroupPriorityBoost = 0.01;
    config.fFallbackJaroCap = 0.49;
    config.fSingleCharFontTolerance = 0.10;
    config.fSingleCharSpacingRatio = 4.0;
    config.fAliasMatchScore = 0.97;
    config.fAbbreviationMatchScore = 0.95;
    config.fNonstandardAbbreviationScore = 0.93;
    config.fReasonableShiftCentroidRatio = 1.2;
    config.fReasonableShiftPenalty = 0.02;
    config.fReasonableShiftNeighborRatio = 1.0;

    if (!szSchemaXmlPath || std::strlen(szSchemaXmlPath) == 0) {
        return SE_ERROR_NONE;
    }

    QFile file(QString::fromUtf8(szSchemaXmlPath));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return SE_ERROR_FILE_NOT_FOUND;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine = 0;
    int errorColumn = 0;
    if (!doc.setContent(&file, &errorMsg, &errorLine, &errorColumn)) {
        file.close();
        return SE_ERROR_CORRUPT_DATA;
    }
    file.close();

    auto applyRule = [&](const QString& ruleName, const QString& ruleValue) {
        const QString trimmedName = ruleName.trimmed();
        const QString trimmedValue = ruleValue.trimmed();
        auto applyDouble = [&](double& target) {
            bool ok = false;
            const double value = trimmedValue.toDouble(&ok);
            if (ok) {
                target = value;
            }
        };
        if (trimmedName == QStringLiteral("标准名称字段")) {
            copyCString(config.szGdbLabelField, sizeof(config.szGdbLabelField), trimmedValue);
        } else if (trimmedName == QStringLiteral("注记文本字段")) {
            copyCString(config.szAnnoTextField, sizeof(config.szAnnoTextField), trimmedValue);
        } else if (trimmedName == QStringLiteral("相似度阈值")) {
            applyDouble(config.fSimilarityThreshold);
        } else if (trimmedName == QStringLiteral("允许外扩距离")) {
            applyDouble(config.fAllowOutsideDistanceMm);
        } else if (trimmedName == QStringLiteral("前后缀基础得分")) {
            applyDouble(config.fPrefixSuffixBaseScore);
        } else if (trimmedName == QStringLiteral("前后缀长度权重")) {
            applyDouble(config.fPrefixSuffixLengthWeight);
        } else if (trimmedName == QStringLiteral("字符重合阈值")) {
            applyDouble(config.fCharsetOverlapRatio);
        } else if (trimmedName == QStringLiteral("字符覆盖阈值")) {
            applyDouble(config.fCharsetCoverageRatio);
        } else if (trimmedName == QStringLiteral("Jaro最小顺序分")) {
            applyDouble(config.fJaroSequenceMin);
        } else if (trimmedName == QStringLiteral("单字等价得分")) {
            applyDouble(config.fSingleCharEquivalentScore);
        } else if (trimmedName == QStringLiteral("单字组名基础得分")) {
            applyDouble(config.fSingleCharGroupScore);
        } else if (trimmedName == QStringLiteral("单字组名优先增益")) {
            applyDouble(config.fSingleCharGroupPriorityBoost);
        } else if (trimmedName == QStringLiteral("Jaro兜底上限")) {
            applyDouble(config.fFallbackJaroCap);
        } else if (trimmedName == QStringLiteral("单字组名字体容差")) {
            applyDouble(config.fSingleCharFontTolerance);
        } else if (trimmedName == QStringLiteral("单字组名间距倍数阈值")) {
            applyDouble(config.fSingleCharSpacingRatio);
        } else if (trimmedName == QStringLiteral("别名匹配得分")) {
            applyDouble(config.fAliasMatchScore);
        } else if (trimmedName == QStringLiteral("简称匹配得分")) {
            applyDouble(config.fAbbreviationMatchScore);
        } else if (trimmedName == QStringLiteral("不规范简称得分")) {
            applyDouble(config.fNonstandardAbbreviationScore);
        } else if (trimmedName == QStringLiteral("合理移位中心距倍数")) {
            applyDouble(config.fReasonableShiftCentroidRatio);
        } else if (trimmedName == QStringLiteral("合理移位排序扣分")) {
            applyDouble(config.fReasonableShiftPenalty);
        } else if (trimmedName == QStringLiteral("合理移位邻区竞争倍数")) {
            applyDouble(config.fReasonableShiftNeighborRatio);
        } else if (trimmedName == QStringLiteral("项目别名字典")) {
            g_projectAliasDictionaryEntryCount =
                parseConfiguredVariantDictionary(trimmedValue, NameVariantAlias);
        } else if (trimmedName == QStringLiteral("项目简称字典")) {
            g_projectAbbreviationDictionaryEntryCount =
                parseConfiguredVariantDictionary(trimmedValue, NameVariantAbbreviation);
        } else if (trimmedName == QStringLiteral("项目不规范简称字典")) {
            g_projectNonstandardDictionaryEntryCount =
                parseConfiguredVariantDictionary(trimmedValue, NameVariantNonstandardAbbreviation);
        }
    };

    const QDomNodeList checkItems = doc.elementsByTagName("check_item");
    for (int i = 0; i < checkItems.count(); ++i) {
        const QDomElement itemElement = checkItems.at(i).toElement();
        const QString checkName = itemElement.firstChildElement("name").text().trimmed();
        if (!checkName.isEmpty() &&
            checkName != QStringLiteral("政区名称注记错误/缺失检查") &&
            checkName != QStringLiteral("政区名称注记检查")) {
            continue;
        }

        const QDomNodeList ruleNodes = itemElement.elementsByTagName("rule");
        for (int j = 0; j < ruleNodes.count(); ++j) {
            const QDomElement ruleElement = ruleNodes.at(j).toElement();
            applyRule(
                ruleElement.firstChildElement("name").text(),
                ruleElement.firstChildElement("value").text());
        }
    }

    loadAdminNameVariantCsvDictionary(QString::fromUtf8(szSchemaXmlPath));

    return SE_ERROR_NONE;
}

SE_Error AdminNameAnnotationCheck(
    const char* szActualAdminShpPath,
    const char* szActualAnnoShpPath,
    const char* szGdbAdminShpPath,
    const char* szSchemaXmlPath,
    const char* szOutputJsonPath,
    const char* szLabelField,
    const char* szOutputShpPath,
    const char* szOutputGeoJsonPath,
    const char* szTempAdminMatchShpPath,
    const char* szTempAnnotationPointShpPath,
    const char* szTempMergedSingleCharShpPath,
    const char* szTempStandardMatchShpPath,
    int nScale,
    bool& bPassed)
{
    bPassed = false;
    if (!szOutputJsonPath || std::strlen(szOutputJsonPath) == 0) {
        return SE_ERROR_INVALID_PARAMETER;
    }

    SE_AdminNameAnnotationRuleConfig config;
    const SE_Error parseError = ParseXmlForAdminNameAnnotation(szSchemaXmlPath, config);
    if (parseError != SE_ERROR_NONE && parseError != SE_ERROR_FILE_NOT_FOUND) {
        QJsonObject errors;
        errors["error1"] = QStringLiteral("规则XML解析失败");
        writeJsonResult(
            szOutputJsonPath,
            false,
            false,
            QStringLiteral("政区名称注记检查初始化失败"),
            QJsonObject(),
            errors);
        return parseError;
    }

    const QString actualAdminPath = QString::fromUtf8(szActualAdminShpPath ? szActualAdminShpPath : "").trimmed();
    const QString actualAnnoPath = QString::fromUtf8(szActualAnnoShpPath ? szActualAnnoShpPath : "").trimmed();
    const QString gdbAdminPath = QString::fromUtf8(szGdbAdminShpPath ? szGdbAdminShpPath : "").trimmed();
    const QString ruleXmlPath = QString::fromUtf8(szSchemaXmlPath ? szSchemaXmlPath : "").trimmed();
    const QString outputShpPath = QString::fromUtf8(szOutputShpPath ? szOutputShpPath : "").trimmed();
    const QString outputGeoJsonPath = QString::fromUtf8(szOutputGeoJsonPath ? szOutputGeoJsonPath : "").trimmed();
    const QString tempAdminMatchShpPath = QString::fromUtf8(szTempAdminMatchShpPath ? szTempAdminMatchShpPath : "").trimmed();
    const QString tempAnnotationPointShpPath = QString::fromUtf8(szTempAnnotationPointShpPath ? szTempAnnotationPointShpPath : "").trimmed();
    const QString tempMergedSingleCharShpPath = QString::fromUtf8(szTempMergedSingleCharShpPath ? szTempMergedSingleCharShpPath : "").trimmed();
    const QString tempStandardMatchShpPath = QString::fromUtf8(szTempStandardMatchShpPath ? szTempStandardMatchShpPath : "").trimmed();

    const QString configuredGdbLabelField = firstNonEmpty({
        QString::fromUtf8(config.szGdbLabelField),
        QString::fromUtf8(szLabelField ? szLabelField : "")
    });
    const QString annoTextField = firstNonEmpty({
        QString::fromUtf8(config.szAnnoTextField),
        QStringLiteral("text_conte")
    });

    QJsonObject inputInfo;
    inputInfo["actual_admin_layer"] = actualAdminPath;
    inputInfo["actual_annotation_layer"] = actualAnnoPath;
    inputInfo["gdb_admin_layer"] = gdbAdminPath;
    inputInfo["rule_xml"] = ruleXmlPath;
    inputInfo["gdb_label_field"] = configuredGdbLabelField;
    inputInfo["annotation_text_field"] = annoTextField;
    inputInfo["error_result_shp"] = outputShpPath;
    inputInfo["error_result_geojson"] = outputGeoJsonPath;
    inputInfo["temp_admin_layer"] = tempAdminMatchShpPath;
    inputInfo["temp_annotation_point"] = tempAnnotationPointShpPath;
    inputInfo["temp_merged_single_char_layer"] = tempMergedSingleCharShpPath;
    inputInfo["temp_standard_layer"] = tempStandardMatchShpPath;
    inputInfo["scale"] = nScale;
    inputInfo["similarity_threshold"] = config.fSimilarityThreshold;
    inputInfo["allow_outside_distance_mm"] = config.fAllowOutsideDistanceMm;
    inputInfo["prefix_suffix_base_score"] = config.fPrefixSuffixBaseScore;
    inputInfo["prefix_suffix_length_weight"] = config.fPrefixSuffixLengthWeight;
    inputInfo["charset_overlap_ratio"] = config.fCharsetOverlapRatio;
    inputInfo["charset_coverage_ratio"] = config.fCharsetCoverageRatio;
    inputInfo["jaro_sequence_min"] = config.fJaroSequenceMin;
    inputInfo["single_char_equivalent_score"] = config.fSingleCharEquivalentScore;
    inputInfo["single_char_group_score"] = config.fSingleCharGroupScore;
    inputInfo["single_char_group_priority_boost"] = config.fSingleCharGroupPriorityBoost;
    inputInfo["fallback_jaro_cap"] = config.fFallbackJaroCap;
    inputInfo["single_char_font_tolerance"] = config.fSingleCharFontTolerance;
    inputInfo["single_char_spacing_ratio"] = config.fSingleCharSpacingRatio;
    inputInfo["alias_match_score"] = config.fAliasMatchScore;
    inputInfo["abbreviation_match_score"] = config.fAbbreviationMatchScore;
    inputInfo["nonstandard_abbreviation_score"] = config.fNonstandardAbbreviationScore;
    inputInfo["reasonable_shift_centroid_ratio"] = config.fReasonableShiftCentroidRatio;
    inputInfo["reasonable_shift_penalty"] = config.fReasonableShiftPenalty;
    inputInfo["reasonable_shift_neighbor_ratio"] = config.fReasonableShiftNeighborRatio;
    inputInfo["project_alias_dict_entry_count"] = g_projectAliasDictionaryEntryCount;
    inputInfo["project_abbreviation_dict_entry_count"] = g_projectAbbreviationDictionaryEntryCount;
    inputInfo["project_nonstandard_dict_entry_count"] = g_projectNonstandardDictionaryEntryCount;
    inputInfo["configured_variant_dictionary_enabled"] =
        (g_projectAliasDictionaryEntryCount + g_projectAbbreviationDictionaryEntryCount +
         g_projectNonstandardDictionaryEntryCount) > 0;
    inputInfo["configured_variant_dictionary_format"] =
        QStringLiteral("标准名=别名1|别名2; 标准名2=别名3|别名4");
    inputInfo["match_rule_mode"] = QStringLiteral("spatial_sorted_single_char");
    inputInfo["match_rule_detail"] = QStringLiteral("stripOuterBrackets+normalizeNumberChars+prefixSuffix+charsetOverlap+JaroWinkler+spatialSortedSingleCharGroup+postErrorBoundaryOverlapReasonableShift");

    QJsonObject errors;

    auto validateInputFile = [&](const QString& path, const QString& key, const QString& title) {
        if (path.isEmpty()) {
            errors[key] = QString(title + QStringLiteral("为空"));
            return;
        }
        if (!QFileInfo::exists(path)) {
            errors[key] = QString(title + QStringLiteral("不存在: ") + path);
        }
    };

    validateInputFile(actualAdminPath, "error1", QStringLiteral("实际行政区面图层"));
    validateInputFile(actualAnnoPath, "error2", QStringLiteral("实际注记图层"));
    validateInputFile(gdbAdminPath, "error3", QStringLiteral("标准行政区图层"));

    if (nScale <= 0) {
        errors["error4"] = QStringLiteral("比例尺分母无效");
    }

    if (!errors.isEmpty()) {
        writeJsonResult(
            szOutputJsonPath,
            false,
            false,
            QStringLiteral("政区名称注记错误/缺失检查输入不完整"),
            inputInfo,
            errors);
        return SE_ERROR_INVALID_PARAMETER;
    }

    QString loadError;
    QgsVectorLayer* actualAdminLayer = loadLayer(actualAdminPath, loadError);
    if (!actualAdminLayer) {
        errors["error5"] = loadError;
    }

    QgsVectorLayer* actualAnnoLayer = loadLayer(actualAnnoPath, loadError);
    if (!actualAnnoLayer) {
        errors["error6"] = loadError;
    }

    QgsVectorLayer* gdbAdminLayer = loadLayer(gdbAdminPath, loadError);
    if (!gdbAdminLayer) {
        errors["error7"] = loadError;
    }

    if (!errors.isEmpty()) {
        delete actualAdminLayer;
        delete actualAnnoLayer;
        delete gdbAdminLayer;
        writeJsonResult(
            szOutputJsonPath,
            false,
            false,
            QStringLiteral("政区名称注记错误/缺失检查图层加载失败"),
            inputInfo,
            errors);
        return SE_ERROR_FAILURE;
    }

    const int annoTextIdx = fieldIndexWithFallback(actualAnnoLayer, QStringList()
        << annoTextField
        << QStringLiteral("text_conte"));
    const int annoFontSizeIdx = fieldIndexWithFallback(actualAnnoLayer, QStringList()
        << QStringLiteral("font_size")
        << QStringLiteral("fontSize")
        << QStringLiteral("fontsize")
        << QStringLiteral("FONT_SIZE"));
    const int gdbNameIdx = fieldIndexWithFallback(gdbAdminLayer, QStringList()
        << configuredGdbLabelField
        << QStringLiteral("XQ")
        << QStringLiteral("NAME")
        << QStringLiteral("名称")
        << QStringLiteral("name")
        << QStringLiteral("NAME_"));

    if (annoTextIdx < 0) {
        errors["error8"] = QStringLiteral("实际注记图层缺少文本字段，优先字段: %1").arg(annoTextField);
    }
    if (gdbNameIdx < 0) {
        const QString candidateFields =
            (QStringList() << configuredGdbLabelField << "XQ" << "NAME" << "名称" << "name" << "NAME_")
                .join(QStringLiteral(", "));
        errors["error9"] = QStringLiteral("标准行政区图层缺少名称字段，候选字段: %1")
            .arg(candidateFields);
    }

    inputInfo["resolved_annotation_text_field"] =
        annoTextIdx >= 0 ? actualAnnoLayer->fields().at(annoTextIdx).name() : QString();
    inputInfo["resolved_annotation_font_field"] =
        annoFontSizeIdx >= 0 ? actualAnnoLayer->fields().at(annoFontSizeIdx).name() : QString();
    inputInfo["resolved_gdb_name_field"] =
        gdbNameIdx >= 0 ? gdbAdminLayer->fields().at(gdbNameIdx).name() : QString();
    inputInfo["actual_admin_feature_count"] = static_cast<int>(actualAdminLayer->featureCount());
    inputInfo["actual_annotation_feature_count"] = static_cast<int>(actualAnnoLayer->featureCount());
    inputInfo["gdb_admin_feature_count"] = static_cast<int>(gdbAdminLayer->featureCount());

    if (!errors.isEmpty()) {
        delete actualAdminLayer;
        delete actualAnnoLayer;
        delete gdbAdminLayer;
        writeJsonResult(
            szOutputJsonPath,
            false,
            false,
            QStringLiteral("政区名称注记错误/缺失检查字段准备失败"),
            inputInfo,
            errors);
        return SE_ERROR_INVALID_PARAMETER;
    }

    const double toleranceMapUnits =
        config.fAllowOutsideDistanceMm > 0.0
            ? (config.fAllowOutsideDistanceMm * static_cast<double>(nScale) / 1000.0)
            : 0.0;
    inputInfo["allow_outside_distance_map_units"] = toleranceMapUnits;

    QMap<qint64, QgsGeometry> actualAdminGeometryByFid;
    QgsSpatialIndex actualAdminIndex;
    QgsFeature actualAdminIndexFeature;
    QgsFeatureIterator actualAdminIndexIt = actualAdminLayer->getFeatures();
    while (actualAdminIndexIt.nextFeature(actualAdminIndexFeature)) {
        const QgsGeometry geom = actualAdminIndexFeature.geometry();
        if (geom.isEmpty()) {
            continue;
        }
        actualAdminGeometryByFid.insert(actualAdminIndexFeature.id(), geom);
        actualAdminIndex.insertFeature(actualAdminIndexFeature);
    }

    QMap<qint64, QgsGeometry> gdbGeometryByFid;
    QList<StandardAdminCandidate> standardAdmins;
    QgsSpatialIndex standardIndex;
    QgsFeature gdbFeature;
    QgsFeatureIterator gdbIt = gdbAdminLayer->getFeatures();
    while (gdbIt.nextFeature(gdbFeature)) {
        const QgsGeometry geom = gdbFeature.geometry();
        if (geom.isEmpty()) {
            continue;
        }

        StandardAdminCandidate candidate;
        candidate.fid = gdbFeature.id();
        candidate.geometry = geom;
        candidate.name = gdbFeature.attribute(gdbNameIdx).toString().trimmed();
        candidate.normalizedName = normalizeNameText(candidate.name);
        standardAdmins.append(candidate);
        gdbGeometryByFid.insert(candidate.fid, geom);
        standardIndex.insertFeature(gdbFeature);
    }

    QList<AnnotationCandidate> annotations;
    QMap<qint64, AnnotationCandidate> annotationByFid;
    QgsSpatialIndex annotationIndex;
    QgsFeature annoFeature;
    QgsFeatureIterator annoIt = actualAnnoLayer->getFeatures();
    while (annoIt.nextFeature(annoFeature)) {
        const QgsGeometry geom = annoFeature.geometry();
        if (geom.isEmpty()) {
            continue;
        }

        AnnotationCandidate candidate;
        candidate.fid = annoFeature.id();
        candidate.text = annoFeature.attribute(annoTextIdx).toString().trimmed();
        candidate.normalizedText = normalizeNameText(candidate.text);
        candidate.anchor = anchorPointFromGeometry(geom);
        candidate.geometry = geom;
        candidate.anchorValid = !geom.isEmpty();
        if (annoFontSizeIdx >= 0) {
            candidate.fontSize = annoFeature.attribute(annoFontSizeIdx).toDouble();
        }
        annotations.append(candidate);
        annotationByFid.insert(candidate.fid, candidate);
        annotationIndex.insertFeature(annoFeature);
    }

    inputInfo["loaded_standard_feature_count"] = standardAdmins.size();
    inputInfo["loaded_annotation_feature_count"] = annotations.size();

    QList<AdminAnnotationIssue> issues;
    QMap<qint64, QgsGeometry> adminGeometryByFid;
    QList<AdminMatchDebugRecord> adminDebugRecords;
    QList<AnnotationPointDebugRecord> annotationDebugRecords;
    QList<MergedSingleCharGroupRecord> mergedSingleCharGroupRecords;
    QList<StandardMatchDebugRecord> standardDebugRecords;
    QSet<qint64> selectedAnnotationFids;
    int nextGroupId = 1;
    int groupedNameCount = 0;
    int groupedCharCount = 0;
    int processedAdminCount = 0;
    int matchedStandardCount = 0;
    int reasonableShiftAcceptedCount = 0;

    QgsFeature adminFeature;
    QgsFeatureIterator adminIt = actualAdminLayer->getFeatures();
    while (adminIt.nextFeature(adminFeature)) {
        const QgsGeometry adminGeom = adminFeature.geometry();
        if (adminGeom.isEmpty()) {
            continue;
        }

        adminGeometryByFid.insert(adminFeature.id(), adminGeom);
        ++processedAdminCount;

        qint64 bestGdbFid = -1;
        QString expectedName;
        QString normalizedExpectedName;
        double bestOverlapArea = -1.0;
        AdminMatchDebugRecord adminRecord;
        adminRecord.adminFid = adminFeature.id();

        const QList<QgsFeatureId> standardCandidateIds = standardIndex.intersects(adminGeom.boundingBox());
        for (QgsFeatureId candidateId : standardCandidateIds) {
            QgsFeature candidateFeature;
            if (!gdbAdminLayer->getFeatures(QgsFeatureRequest(candidateId)).nextFeature(candidateFeature)) {
                continue;
            }

            const QgsGeometry candidateGeom = candidateFeature.geometry();
            if (candidateGeom.isEmpty() || !adminGeom.intersects(candidateGeom)) {
                continue;
            }

            const QgsGeometry overlapGeom = adminGeom.intersection(candidateGeom);
            const double overlapArea = overlapGeom.isEmpty() ? 0.0 : overlapGeom.area();
            if (overlapArea > bestOverlapArea) {
                bestOverlapArea = overlapArea;
                bestGdbFid = candidateFeature.id();
                expectedName = candidateFeature.attribute(gdbNameIdx).toString().trimmed();
                normalizedExpectedName = normalizeNameText(expectedName);
            }
        }

        if (bestGdbFid >= 0) {
            ++matchedStandardCount;
        }

        adminRecord.gdbFid = bestGdbFid;
        adminRecord.assocName = expectedName;
        adminRecord.expectVal = expectedName;
        adminRecord.overlapArea = bestOverlapArea > 0.0 ? bestOverlapArea : 0.0;

        if (bestGdbFid < 0 || normalizedExpectedName.isEmpty()) {
            AdminAnnotationIssue issue;
            issue.type = QStringLiteral("missing_standard_name");
            issue.adminFid = adminFeature.id();
            issue.gdbFid = bestGdbFid;
            issues.append(issue);
            adminRecord.matchStat = QStringLiteral("UNMATCHED");
            adminRecord.errorType = issueTypeToken(issue.type);
            adminRecord.errorMsg = issueDescription(issue);
            adminDebugRecords.append(adminRecord);
            continue;
        }

        qint64 bestAnnoFid = -1;
        QString bestAnnoText;
        double bestSimilarity = -1.0;
        bool bestStructuredMatch = false;
        QString bestMatchSource;
        QString bestMatchNote;
        qint64 bestGroupId = -1;
        QString bestGroupText;
        QList<qint64> bestGroupFids;
        int bestInsideFlag = 0;
        QString bestSpatialNote;
        QList<AnnotationCandidate> insideCandidates;
        QMap<qint64, SpatialAcceptanceResult> acceptedSpatialByFid;

        QgsRectangle annoSearchRect = adminGeom.boundingBox();
        const double searchGrow = toleranceMapUnits > 0.0 ? toleranceMapUnits * 1.5 : 0.0;
        if (searchGrow > 0.0) {
            annoSearchRect.grow(searchGrow);
        }
        const QList<QgsFeatureId> annoCandidateIds = annotationIndex.intersects(annoSearchRect);
        for (QgsFeatureId annoFid : annoCandidateIds) {
            if (!annotationByFid.contains(annoFid)) {
                continue;
            }

            const AnnotationCandidate candidate = annotationByFid.value(annoFid);
            if (!candidate.anchorValid || candidate.normalizedText.isEmpty()) {
                continue;
            }
            const SpatialAcceptanceResult spatialResult =
                evaluateAnnotationSpatialRelation(
                    adminFeature.id(),
                    adminGeom,
                    candidate.anchor,
                    toleranceMapUnits,
                    actualAdminIndex,
                    actualAdminGeometryByFid,
                    config,
                    nullptr,
                    false,
                    false);
            if (!spatialResult.accepted) {
                continue;
            }

            acceptedSpatialByFid.insert(candidate.fid, spatialResult);
            insideCandidates.append(candidate);

            const PartialMatchResult partialMatch = checkPartialMatch(candidate.text, expectedName, config);
            double similarity = 0.0;
            bool structuredMatch = false;

            if (partialMatch.matched) {
                similarity = partialMatch.score;
                structuredMatch = true;
            } else if (candidate.text.length() == 1 && nameContainsCharEquivalent(expectedName, candidate.text)) {
                similarity = config.fSingleCharEquivalentScore;
                structuredMatch = true;
            } else {
                similarity = qMin(jaroWinklerSimilarity(candidate.normalizedText, normalizedExpectedName), config.fFallbackJaroCap);
            }

            if (!spatialResult.inside) {
                similarity = qMax(0.0, similarity - config.fReasonableShiftPenalty);
            }

            if (similarity > bestSimilarity) {
                bestSimilarity = similarity;
                bestAnnoFid = candidate.fid;
                bestAnnoText = candidate.text;
                bestStructuredMatch = structuredMatch;
                bestInsideFlag = spatialResult.insideFlag;
                bestSpatialNote = spatialResult.note;
                bestMatchSource = partialMatch.matched
                    ? partialMatch.source
                    : candidate.text.length() == 1 && nameContainsCharEquivalent(expectedName, candidate.text)
                        ? QStringLiteral("单字等价命中")
                        : QStringLiteral("相似度兜底");
                bestMatchNote = partialMatch.matched
                    ? partialMatch.detail
                    : candidate.text.length() == 1 && nameContainsCharEquivalent(expectedName, candidate.text)
                        ? buildMatchDetail(QStringLiteral("单字等价"), expectedName, candidate.text)
                        : buildMatchDetail(QStringLiteral("相似度兜底"), expectedName, candidate.text);
                bestMatchNote = appendDetailNote(bestMatchNote, bestSpatialNote);
            }
        }

        const SingleCharGroupMatch chosenGroup =
            tryBuildSpatialSortedCharGroup(insideCandidates, expectedName, config, nScale);
        const QString chosenGroupSource = QStringLiteral("空间收字排序");

        const bool preferSingleCharGroup =
            chosenGroup.matched &&
            (bestAnnoFid < 0 ||
             chosenGroup.score > bestSimilarity + config.fSingleCharGroupPriorityBoost ||
             (bestAnnoText.trimmed().length() == 1 && chosenGroup.fids.size() > 1));

        if (preferSingleCharGroup) {
            bestAnnoFid = chosenGroup.fids.isEmpty() ? -1 : chosenGroup.fids.first();
            bestAnnoText = chosenGroup.groupText;
            bestSimilarity = chosenGroup.score;
            bestStructuredMatch = true;
            bestMatchSource = chosenGroupSource + QStringLiteral("命中");
            bestMatchNote = buildMatchDetail(chosenGroupSource, expectedName, chosenGroup.groupText);
            bestInsideFlag = 1;
            bestSpatialNote.clear();
            for (qint64 groupFid : chosenGroup.fids) {
                if (!acceptedSpatialByFid.contains(groupFid)) {
                    continue;
                }
                const SpatialAcceptanceResult groupSpatial = acceptedSpatialByFid.value(groupFid);
                if (!groupSpatial.inside) {
                    bestInsideFlag = 2;
                    bestSpatialNote = groupSpatial.note;
                    break;
                }
            }
            bestMatchNote = appendDetailNote(bestMatchNote, bestSpatialNote);
            bestGroupId = nextGroupId++;
            bestGroupText = chosenGroup.groupText;
            bestGroupFids = chosenGroup.fids;
            ++groupedNameCount;
            groupedCharCount += bestGroupFids.size();

            double sumX = 0.0;
            double sumY = 0.0;
            QStringList fidTexts;
            int validPointCount = 0;
            for (qint64 groupFid : bestGroupFids) {
                fidTexts.append(QString::number(groupFid));
                if (!annotationByFid.contains(groupFid)) {
                    continue;
                }
                const AnnotationCandidate& groupCandidate = annotationByFid.value(groupFid);
                sumX += groupCandidate.anchor.x();
                sumY += groupCandidate.anchor.y();
                ++validPointCount;
            }

            MergedSingleCharGroupRecord mergedRecord;
            mergedRecord.groupId = bestGroupId;
            mergedRecord.adminFid = adminFeature.id();
            mergedRecord.gdbFid = bestGdbFid;
            mergedRecord.firstAnnoFid = bestAnnoFid;
            mergedRecord.groupText = bestGroupText;
            mergedRecord.expectedName = expectedName;
            mergedRecord.matchStat = QStringLiteral("FULLY_MATCHED");
            mergedRecord.matchSource = bestMatchSource;
            mergedRecord.matchNote = bestMatchNote;
            mergedRecord.matchScore = bestSimilarity;
            mergedRecord.groupCount = bestGroupFids.size();
            mergedRecord.annoFids = fidTexts.join(QStringLiteral(","));
            mergedRecord.anchor = validPointCount > 0
                ? QgsPointXY(sumX / validPointCount, sumY / validPointCount)
                : anchorPointFromGeometry(adminGeom);
            mergedSingleCharGroupRecords.append(mergedRecord);
        }

        if (bestAnnoFid < 0 || !bestStructuredMatch || bestSimilarity < config.fSimilarityThreshold) {
            QList<AnnotationCandidate> supplementalCandidates;
            QMap<qint64, SpatialAcceptanceResult> supplementalSpatialByFid;
            for (QgsFeatureId annoFid : annoCandidateIds) {
                if (!annotationByFid.contains(annoFid)) {
                    continue;
                }

                const AnnotationCandidate candidate = annotationByFid.value(annoFid);
                if (!candidate.anchorValid || candidate.normalizedText.isEmpty()) {
                    continue;
                }

                const SpatialAcceptanceResult spatialResult =
                    evaluateAnnotationSpatialRelation(
                        adminFeature.id(),
                        adminGeom,
                        candidate.anchor,
                        toleranceMapUnits,
                        actualAdminIndex,
                        actualAdminGeometryByFid,
                        config,
                        &candidate.geometry,
                        true,
                        true);
                if (!spatialResultHasSupplement(spatialResult)) {
                    continue;
                }

                supplementalCandidates.append(candidate);
                supplementalSpatialByFid.insert(candidate.fid, spatialResult);

                const PartialMatchResult partialMatch = checkPartialMatch(candidate.text, expectedName, config);
                if (!partialMatch.matched || partialMatch.score < config.fSimilarityThreshold) {
                    continue;
                }

                bestAnnoFid = candidate.fid;
                bestAnnoText = candidate.text;
                bestSimilarity = partialMatch.score;
                bestStructuredMatch = true;
                bestMatchSource = partialMatch.source + QStringLiteral("(错误补充验证)");
                bestSpatialNote = spatialResult.note;
                bestMatchNote = appendDetailNote(partialMatch.detail, bestSpatialNote);
                bestInsideFlag = spatialResult.insideFlag;
                bestGroupId = -1;
                bestGroupText.clear();
                bestGroupFids.clear();
                break;
            }

            if (bestAnnoFid < 0 || !bestStructuredMatch || bestSimilarity < config.fSimilarityThreshold) {
                const SingleCharGroupMatch supplementalGroup =
                    tryBuildSpatialSortedCharGroup(supplementalCandidates, expectedName, config, nScale);
                if (supplementalGroup.matched) {
                    bestAnnoFid = supplementalGroup.fids.isEmpty() ? -1 : supplementalGroup.fids.first();
                    bestAnnoText = supplementalGroup.groupText;
                    bestSimilarity = supplementalGroup.score;
                    bestStructuredMatch = true;
                    bestMatchSource = QStringLiteral("空间收字排序命中(错误补充验证)");
                    bestMatchNote = buildMatchDetail(QStringLiteral("空间收字排序-错误补充验证"), expectedName, supplementalGroup.groupText);
                    bestInsideFlag = 2;
                    QStringList supplementNotes;
                    for (qint64 groupFid : supplementalGroup.fids) {
                        if (!supplementalSpatialByFid.contains(groupFid)) {
                            continue;
                        }
                        const SpatialAcceptanceResult spatial = supplementalSpatialByFid.value(groupFid);
                        if (spatial.insideFlag == 3) {
                            bestInsideFlag = 3;
                        }
                        if (!spatial.note.trimmed().isEmpty() && !supplementNotes.contains(spatial.note.trimmed())) {
                            supplementNotes.append(spatial.note.trimmed());
                        }
                    }
                    bestSpatialNote = supplementNotes.join(QStringLiteral(" | "));
                    bestMatchNote = appendDetailNote(bestMatchNote, bestSpatialNote);
                    bestGroupId = nextGroupId++;
                    bestGroupText = supplementalGroup.groupText;
                    bestGroupFids = supplementalGroup.fids;
                    ++groupedNameCount;
                    groupedCharCount += bestGroupFids.size();

                    double sumX = 0.0;
                    double sumY = 0.0;
                    QStringList fidTexts;
                    int validPointCount = 0;
                    for (qint64 groupFid : bestGroupFids) {
                        fidTexts.append(QString::number(groupFid));
                        if (!annotationByFid.contains(groupFid)) {
                            continue;
                        }
                        const AnnotationCandidate& groupCandidate = annotationByFid.value(groupFid);
                        sumX += groupCandidate.anchor.x();
                        sumY += groupCandidate.anchor.y();
                        ++validPointCount;
                    }

                    MergedSingleCharGroupRecord mergedRecord;
                    mergedRecord.groupId = bestGroupId;
                    mergedRecord.adminFid = adminFeature.id();
                    mergedRecord.gdbFid = bestGdbFid;
                    mergedRecord.firstAnnoFid = bestAnnoFid;
                    mergedRecord.groupText = bestGroupText;
                    mergedRecord.expectedName = expectedName;
                    mergedRecord.matchStat = QStringLiteral("FULLY_MATCHED");
                    mergedRecord.matchSource = bestMatchSource;
                    mergedRecord.matchNote = bestMatchNote;
                    mergedRecord.matchScore = bestSimilarity;
                    mergedRecord.groupCount = bestGroupFids.size();
                    mergedRecord.annoFids = fidTexts.join(QStringLiteral(","));
                    mergedRecord.anchor = validPointCount > 0
                        ? QgsPointXY(sumX / validPointCount, sumY / validPointCount)
                        : anchorPointFromGeometry(adminGeom);
                    mergedSingleCharGroupRecords.append(mergedRecord);
                }
            }

            for (auto it = supplementalSpatialByFid.constBegin(); it != supplementalSpatialByFid.constEnd(); ++it) {
                acceptedSpatialByFid.insert(it.key(), it.value());
            }
        }

        if (bestAnnoFid < 0) {
            AdminAnnotationIssue issue;
            issue.type = QStringLiteral("missing_annotation");
            issue.expectedName = expectedName;
            issue.adminFid = adminFeature.id();
            issue.gdbFid = bestGdbFid;
            issues.append(issue);
            adminRecord.matchStat = QStringLiteral("UNMATCHED");
            adminRecord.errorType = issueTypeToken(issue.type);
            adminRecord.errorMsg = issueDescription(issue);
            adminDebugRecords.append(adminRecord);

            StandardMatchDebugRecord standardRecord;
            standardRecord.gdbFid = bestGdbFid;
            standardRecord.adminFid = adminFeature.id();
            standardRecord.assocName = expectedName;
            standardRecord.matchStat = QStringLiteral("UNMATCHED");
            standardRecord.errorType = issueTypeToken(issue.type);
            standardRecord.errorMsg = issueDescription(issue);
            standardRecord.actualVal = QString();
            standardRecord.expectVal = expectedName;
            standardRecord.overlapArea = bestOverlapArea > 0.0 ? bestOverlapArea : 0.0;
            standardDebugRecords.append(standardRecord);
            continue;
        }

        adminRecord.annoFid = bestAnnoFid;
        adminRecord.textConte = bestAnnoText;
        adminRecord.actualVal = bestAnnoText;
        adminRecord.matchScore = bestSimilarity;
        adminRecord.matchSource = bestMatchSource;
        adminRecord.matchNote = bestMatchNote;
        adminRecord.groupId = bestGroupId;
        adminRecord.groupText = bestGroupText;
        adminRecord.groupCount = bestGroupFids.size();
        if (bestInsideFlag == 2) {
            ++reasonableShiftAcceptedCount;
        }

        StandardMatchDebugRecord standardRecord;
        standardRecord.gdbFid = bestGdbFid;
        standardRecord.adminFid = adminFeature.id();
        standardRecord.annoFid = bestAnnoFid;
        standardRecord.textConte = bestAnnoText;
        standardRecord.groupId = bestGroupId;
        standardRecord.groupText = bestGroupText;
        standardRecord.assocName = expectedName;
        standardRecord.actualVal = bestAnnoText;
        standardRecord.expectVal = expectedName;
        standardRecord.groupCount = bestGroupFids.size();
        standardRecord.matchScore = bestSimilarity;
        standardRecord.matchSource = bestMatchSource;
        standardRecord.matchNote = bestMatchNote;
        standardRecord.overlapArea = bestOverlapArea > 0.0 ? bestOverlapArea : 0.0;

        if (!bestStructuredMatch || bestSimilarity < config.fSimilarityThreshold) {
            AdminAnnotationIssue issue;
            issue.type = QStringLiteral("wrong_annotation");
            issue.expectedName = expectedName;
            issue.actualText = bestAnnoText;
            issue.matchSource = bestMatchSource;
            issue.matchNote = bestMatchNote;
            issue.similarity = bestSimilarity;
            issue.adminFid = adminFeature.id();
            issue.annotationFid = bestAnnoFid;
            issue.gdbFid = bestGdbFid;
            issue.groupId = bestGroupId;
            issue.groupText = bestGroupText;
            issue.groupCount = bestGroupFids.size();
            issues.append(issue);
            adminRecord.matchStat = QStringLiteral("PARTIALLY_MATCHED");
            adminRecord.errorType = issueTypeToken(issue.type);
            adminRecord.errorMsg = issueDescription(issue);
            standardRecord.matchStat = QStringLiteral("PARTIALLY_MATCHED");
            standardRecord.errorType = issueTypeToken(issue.type);
            standardRecord.errorMsg = issueDescription(issue);
        } else {
            adminRecord.matchStat = QStringLiteral("FULLY_MATCHED");
            standardRecord.matchStat = QStringLiteral("FULLY_MATCHED");
        }

        adminDebugRecords.append(adminRecord);
        standardDebugRecords.append(standardRecord);
        if (!bestGroupFids.isEmpty()) {
            for (int i = 0; i < bestGroupFids.size(); ++i) {
                const qint64 groupFid = bestGroupFids.at(i);
                if (!annotationByFid.contains(groupFid)) {
                    continue;
                }
                const AnnotationCandidate& groupCandidate = annotationByFid.value(groupFid);
                AnnotationPointDebugRecord groupRecord;
                groupRecord.annoFid = groupFid;
                groupRecord.adminFid = adminFeature.id();
                groupRecord.gdbFid = bestGdbFid;
                groupRecord.groupId = bestGroupId;
                groupRecord.textConte = groupCandidate.text;
                groupRecord.groupText = bestGroupText;
                groupRecord.assocName = expectedName;
                groupRecord.actualVal = groupCandidate.text;
                groupRecord.expectVal = expectedName;
                groupRecord.groupCount = bestGroupFids.size();
                groupRecord.charSeq = i + 1;
                groupRecord.matchScore = bestSimilarity;
                const SpatialAcceptanceResult spatialResult = acceptedSpatialByFid.value(groupFid);
                groupRecord.insideFlg = spatialResult.insideFlag;
                groupRecord.anchor = groupCandidate.anchor;
                groupRecord.matchStat = adminRecord.matchStat;
                groupRecord.matchSource = adminRecord.matchSource;
                groupRecord.matchNote = adminRecord.matchNote;
                groupRecord.errorType = adminRecord.errorType;
                groupRecord.errorMsg = adminRecord.errorMsg;
                annotationDebugRecords.append(groupRecord);
                selectedAnnotationFids.insert(groupFid);
            }
        } else {
            AnnotationPointDebugRecord annotationRecord;
            annotationRecord.annoFid = bestAnnoFid;
            annotationRecord.adminFid = adminFeature.id();
            annotationRecord.gdbFid = bestGdbFid;
            annotationRecord.textConte = bestAnnoText;
            annotationRecord.assocName = expectedName;
            annotationRecord.actualVal = bestAnnoText;
            annotationRecord.expectVal = expectedName;
            annotationRecord.matchScore = bestSimilarity;
            annotationRecord.insideFlg = bestInsideFlag;
            annotationRecord.anchor = annotationByFid.value(bestAnnoFid).anchor;
            annotationRecord.matchStat = adminRecord.matchStat;
            annotationRecord.matchSource = adminRecord.matchSource;
            annotationRecord.matchNote = adminRecord.matchNote;
            annotationRecord.errorType = adminRecord.errorType;
            annotationRecord.errorMsg = adminRecord.errorMsg;
            annotationDebugRecords.append(annotationRecord);
            selectedAnnotationFids.insert(bestAnnoFid);
        }
    }

    for (const AnnotationCandidate& annotation : annotations) {
        if (!annotation.anchorValid || selectedAnnotationFids.contains(annotation.fid)) {
            continue;
        }

        AnnotationPointDebugRecord annotationRecord;
        annotationRecord.annoFid = annotation.fid;
        annotationRecord.textConte = annotation.text;
        annotationRecord.actualVal = annotation.text;
        annotationRecord.matchStat = QStringLiteral("UNMATCHED");
        annotationRecord.errorType = QStringLiteral("UNUSED_ANNO");
        annotationRecord.errorMsg = QStringLiteral("该注记点未被任何行政区匹配结果使用");
        annotationRecord.anchor = annotation.anchor;
        annotationDebugRecords.append(annotationRecord);
    }

    int missingAnnotationCount = 0;
    int wrongAnnotationCount = 0;
    int missingStandardCount = 0;
    QJsonObject issueJson;
    for (int i = 0; i < issues.size(); ++i) {
        const AdminAnnotationIssue& issue = issues.at(i);
        if (issue.type == QStringLiteral("missing_annotation")) {
            ++missingAnnotationCount;
        } else if (issue.type == QStringLiteral("wrong_annotation")) {
            ++wrongAnnotationCount;
        } else if (issue.type == QStringLiteral("missing_standard_name")) {
            ++missingStandardCount;
        }

        QJsonObject item;
        item["type"] = issueTypeToken(issue.type);
        item["description"] = issueDescription(issue);
        item["admin_fid"] = QString::number(issue.adminFid);
        item["annotation_fid"] = issue.annotationFid >= 0 ? QString::number(issue.annotationFid) : QString();
        item["gdb_fid"] = issue.gdbFid >= 0 ? QString::number(issue.gdbFid) : QString();
        item["expected_name"] = issue.expectedName;
        item["annotation_text"] = issue.actualText;
        item["group_id"] = issue.groupId >= 0 ? QString::number(issue.groupId) : QString();
        item["group_text"] = issue.groupText;
        item["group_count"] = issue.groupCount;
        item["similarity"] = issue.similarity;
        issueJson[QStringLiteral("error%1").arg(i + 1)] = item;
    }

    inputInfo["processed_admin_feature_count"] = processedAdminCount;
    inputInfo["matched_standard_feature_count"] = matchedStandardCount;
    inputInfo["missing_annotation_count"] = missingAnnotationCount;
    inputInfo["wrong_annotation_count"] = wrongAnnotationCount;
    inputInfo["missing_standard_name_count"] = missingStandardCount;
    inputInfo["spatial_match_status"] = QStringLiteral("done");
    inputInfo["jaro_winkler_status"] = QStringLiteral("done");
    inputInfo["temp_admin_feature_count"] = adminDebugRecords.size();
    inputInfo["temp_annotation_point_count"] = annotationDebugRecords.size();
    inputInfo["temp_merged_single_char_count"] = mergedSingleCharGroupRecords.size();
    inputInfo["temp_standard_feature_count"] = standardDebugRecords.size();
    inputInfo["single_char_group_count"] = groupedNameCount;
    inputInfo["single_char_group_char_count"] = groupedCharCount;
    inputInfo["reasonable_shift_match_count"] = reasonableShiftAcceptedCount;

    QString outputError;
    if (!writeIssueLayer(outputShpPath, QStringLiteral("ESRI Shapefile"), actualAdminLayer, adminGeometryByFid, issues, outputError)) {
        errors["error10"] = QStringLiteral("错误结果SHP输出失败: %1").arg(outputError);
    }
    if (!writeIssueLayer(outputGeoJsonPath, QStringLiteral("GeoJSON"), actualAdminLayer, adminGeometryByFid, issues, outputError)) {
        errors["error11"] = QStringLiteral("错误结果GeoJSON输出失败: %1").arg(outputError);
    }
    if (!writeAdminDebugLayer(tempAdminMatchShpPath, actualAdminLayer, adminGeometryByFid, adminDebugRecords, outputError)) {
        errors["error12"] = QStringLiteral("行政区中间结果层输出失败: %1").arg(outputError);
    }
    if (!writeAnnotationPointDebugLayer(tempAnnotationPointShpPath, actualAdminLayer, annotationDebugRecords, outputError)) {
        errors["error13"] = QStringLiteral("注记点中间结果层输出失败: %1").arg(outputError);
    }
    if (!writeMergedSingleCharGroupLayer(tempMergedSingleCharShpPath, actualAdminLayer, mergedSingleCharGroupRecords, outputError)) {
        errors["error14"] = QStringLiteral("合并单字注记中间结果层输出失败: %1").arg(outputError);
    }
    if (!writeStandardDebugLayer(tempStandardMatchShpPath, gdbAdminLayer, gdbGeometryByFid, standardDebugRecords, outputError)) {
        errors["error15"] = QStringLiteral("标准面中间结果层输出失败: %1").arg(outputError);
    }

    bPassed = issues.isEmpty() && errors.isEmpty();

    const QString infoText = bPassed
        ? QStringLiteral("政区名称注记错误/缺失检查已完成，空间包含判定、text_conte读取、标准名称字段读取和Jaro-Winkler匹配已接通")
        : errors.isEmpty()
            ? QStringLiteral("政区名称注记错误/缺失检查已完成，发现注记缺失或名称不一致问题")
            : QStringLiteral("政区名称注记错误/缺失检查已执行，但错误结果图层输出失败");

    writeJsonResult(
        szOutputJsonPath,
        errors.isEmpty(),
        bPassed,
        infoText,
        inputInfo,
        errors.isEmpty() ? issueJson : [issueJson, errors]() {
            QJsonObject merged = issueJson;
            for (auto it = errors.begin(); it != errors.end(); ++it) {
                merged.insert(it.key(), it.value());
            }
            return merged;
        }());

    delete actualAdminLayer;
    delete actualAnnoLayer;
    delete gdbAdminLayer;
    return SE_ERROR_NONE;
}
