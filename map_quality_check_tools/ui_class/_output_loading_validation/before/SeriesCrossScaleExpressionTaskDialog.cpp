#include "SeriesCrossScaleExpressionTaskDialog.h"
#include "SeriesMapConfigurationDeduplication.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include "series_layer_mapping.h"
#include "series_mapping_tree_groups.h"
#include "searchable_layer_combo_box.h"
#include "map_path_display_edit.h"
#include "series_layer_exclusions.h"
#include "cross_scale_support.h"
#include "cross_scale_finding_presentation.h"
#include "cross_scale_tolerance.h"
#include "series_source_audit.h"
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVBoxLayout>
#include <QDomDocument>

#include <qgscoordinatereferencesystem.h>
#include <qgsattributetableconfig.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsfields.h>
#include <qgsgeometry.h>
#include <qgsrectangle.h>
#include <qgsspatialindex.h>
#include <qgsvectorlayer.h>
#include <qgsvectorfilewriter.h>
#include <qgswkbtypes.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {
constexpr int CrossColMainMap = 0;
constexpr int CrossColMainScale = 1;
constexpr int CrossColAssocMap = 2;
constexpr int CrossColAssocScale = 3;
constexpr int CrossColRelationType = 4;
constexpr int CrossColStatus = 5;
const QString kCrossScaleCheckItemId = QStringLiteral("502");

QStringList splitList(const QString& text)
{
    QStringList values = text.split(QRegularExpression(QStringLiteral("[;；、\\n\\r\\t]+")), Qt::SkipEmptyParts);
    for (QString& value : values) value = value.trimmed();
    values.removeAll(QString());
    return values;
}

int parseScaleString(QString text)
{
    text = text.trimmed();
    text.remove(QStringLiteral("比例尺"));
    text.replace(QStringLiteral("："), QStringLiteral(":"));
    text.replace(QStringLiteral("／"), QStringLiteral("/"));
    text.remove(QRegularExpression(QStringLiteral("[,，\\s]")));
    QRegularExpression re(QStringLiteral("^(?:1[:/])?(\\d+)$"));
    QRegularExpressionMatch match = re.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

int parseScaleFromPdfName(const QString& pdfPath)
{
    QString baseName = QFileInfo(pdfPath).completeBaseName();
    baseName.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）]+")));

    QRegularExpression wanRe(QStringLiteral("([1-9]\\d{0,3})(?:[wW]|万)"));
    QRegularExpressionMatch wanMatch = wanRe.match(baseName);
    if (wanMatch.hasMatch()) {
        bool ok = false;
        const int value = wanMatch.captured(1).toInt(&ok);
        if (ok && value > 0) return value * 10000;
    }

    QRegularExpression qianRe(QStringLiteral("([1-9]\\d{0,3})(?:[kK]|千)"));
    QRegularExpressionMatch qianMatch = qianRe.match(baseName);
    if (qianMatch.hasMatch()) {
        bool ok = false;
        const int value = qianMatch.captured(1).toInt(&ok);
        if (ok && value > 0) return value * 1000;
    }

    QRegularExpression denomRe(QStringLiteral("(?:1[:：/]?)?([1-9]\\d{2,7})(?!\\d)"));
    QRegularExpressionMatchIterator it = denomRe.globalMatch(baseName);
    int best = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const int value = match.captured(1).toInt(&ok);
        if (ok && value >= 500) best = value;
    }
    return best;
}

int inferScaleDenominatorFromShpDir(const QString& shpDir)
{
    if (shpDir.isEmpty() || !QDir(shpDir).exists()) return 0;

    QDir dir(shpDir);
    QStringList allShps = dir.entryList(QStringList() << QStringLiteral("*.shp"), QDir::Files);
    QStringList candidateShps;
    const QStringList priorities = {
        QStringLiteral("图例_Polygon.shp"),
        QStringLiteral("比例尺_Polygon.shp"),
        QStringLiteral("注记_Polygon.shp"),
        QStringLiteral("整饰_Polygon.shp")
    };

    for (const QString& priority : priorities) {
        for (const QString& shp : allShps) {
            if (shp.compare(priority, Qt::CaseInsensitive) == 0) {
                candidateShps << dir.absoluteFilePath(shp);
            }
        }
    }
    for (const QString& shp : allShps) {
        const QString absPath = dir.absoluteFilePath(shp);
        if (!candidateShps.contains(absPath)) candidateShps << absPath;
    }

    QSet<int> foundScales;
    QRegularExpression scaleRe(QStringLiteral("1\\s*[:：/]\\s*([1-9]\\d*(?:\\s*\\d+)*)"));
    QRegularExpression fallbackRe(QStringLiteral("比例尺\\s*([1-9]\\d*(?:\\s*\\d+)*)"));

    for (const QString& shpPath : candidateShps) {
        QgsVectorLayer layer(shpPath, QStringLiteral("cross_scale_auto_scale"), QStringLiteral("ogr"));
        if (!layer.isValid()) continue;

        int textFieldIndex = -1;
        const QgsFields fields = layer.fields();
        for (int i = 0; i < fields.count(); ++i) {
            const QString fieldName = fields.at(i).name().toLower();
            if (fieldName == QStringLiteral("text_conte") || fieldName == QStringLiteral("text_content")) {
                textFieldIndex = i;
                break;
            }
        }
        if (textFieldIndex < 0) continue;

        QgsFeatureIterator fit = layer.getFeatures();
        QgsFeature feature;
        while (fit.nextFeature(feature)) {
            const QString text = feature.attribute(textFieldIndex).toString().trimmed();
            if (text.isEmpty()) continue;

            QRegularExpressionMatch match = scaleRe.match(text);
            if (!match.hasMatch()) match = fallbackRe.match(text);
            if (!match.hasMatch()) continue;

            QString denomText = match.captured(1);
            denomText.remove(QRegularExpression(QStringLiteral("[,，\\s]")));
            bool ok = false;
            const int denom = denomText.toInt(&ok);
            if (ok && denom >= 500) foundScales.insert(denom);
        }

        if (!foundScales.isEmpty()) break;
    }

    if (foundScales.size() == 1) return *foundScales.begin();
    return foundScales.size() > 1 ? -1 : 0;
}

QString normalizeLayerName(QString text)
{
    text = SeriesLayerMapping::layerBaseName(text);
    text.remove(QRegularExpression(QStringLiteral("[\\s　_\\-—()（）\\[\\]【】]+")));
    return text.toLower();
}

QString displayLayerName(const QString& path)
{
    return SeriesLayerMapping::layerBaseName(path);
}

QStringList collectGeoPdfFiles(const QString& inputPath)
{
    QStringList pdfFiles;
    QFileInfo fi(inputPath);
    if (fi.isFile() && fi.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0) {
        pdfFiles << fi.absoluteFilePath();
    } else if (fi.isDir()) {
        QDirIterator it(inputPath, QStringList() << QStringLiteral("*.pdf"), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) pdfFiles << it.next();
    }
    pdfFiles.removeDuplicates();
    return pdfFiles;
}

QStringList collectShpFiles(const QString& shpRootDir)
{
    QStringList paths;
    const QStringList dirs = shpRootDir.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    for (const QString& dirPath : dirs) {
        QDir dir(dirPath.trimmed());
        if (!dir.exists()) continue;
        const QStringList files = dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files, QDir::Name);
        for (const QString& fileName : files) {
            if (QFileInfo(fileName).completeBaseName() == QStringLiteral("内图廓")) continue;
            const QString path = dir.filePath(fileName);
            if (SeriesLayerExclusions::excluded(path, true)) continue;
            if (!paths.contains(path)) paths << path;
        }
    }
    return paths;
}

bool validatePreprocessedShpDir(const QString& shpDir)
{
    QDir dir(shpDir);
    if (!dir.exists()) return false;
    return !dir.entryList(QStringList() << QStringLiteral("*.shp") << QStringLiteral("*.SHP"), QDir::Files).isEmpty();
}


QString xmlParameterText(const QString& xmlPath, const QStringList& keys)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    QDomDocument doc;
    if (!doc.setContent(&file)) return QString();

    std::function<QString(const QDomElement&)> visit = [&](const QDomElement& elem) -> QString {
        QString haystack = elem.tagName();
        const QDomNamedNodeMap attrs = elem.attributes();
        for (int i = 0; i < attrs.count(); ++i) {
            haystack += QStringLiteral(" ") + attrs.item(i).nodeValue();
        }
        for (const QString& key : keys) {
            if (haystack.contains(key, Qt::CaseInsensitive)) {
                const QString text = elem.text().trimmed();
                if (!text.isEmpty()) return text;
            }
        }
        QDomNode child = elem.firstChild();
        while (!child.isNull()) {
            if (child.isElement()) {
                const QString found = visit(child.toElement());
                if (!found.isEmpty()) return found;
            }
            child = child.nextSibling();
        }
        return QString();
    };

    return visit(doc.documentElement());
}

QStringList splitRuleList(const QString& text)
{
    QStringList values = text.split(QRegularExpression(QStringLiteral("[,，;；、|\\n\\r\\t]+")), Qt::SkipEmptyParts);
    for (QString& value : values) value = value.trimmed();
    values.removeAll(QString());
    values.removeDuplicates();
    return values;
}

QMap<QString, QString> parseAliasText(const QString& text)
{
    QMap<QString, QString> aliases;
    const QStringList groups = text.split(QRegularExpression(QStringLiteral("[;；\\n\\r]+")), Qt::SkipEmptyParts);
    for (const QString& group : groups) {
        const QStringList parts = group.split(QRegularExpression(QStringLiteral("[:：=]")), Qt::SkipEmptyParts);
        if (parts.size() < 2) continue;
        const QString canonical = parts.first().trimmed();
        const QStringList names = splitRuleList(parts.mid(1).join(QStringLiteral(":")));
        for (const QString& name : names) aliases[normalizeLayerName(name)] = canonical;
        aliases[normalizeLayerName(canonical)] = canonical;
    }
    return aliases;
}

QString aliasCanonical(const QString& name, const QMap<QString, QString>& aliases)
{
    const QString key = normalizeLayerName(name);
    return aliases.value(key, key);
}

QStringList matchLayerPaths(const QString& target, const QMap<QString, QString>& aliases, const QString& rootDir)
{
    QStringList matches;
    const QString targetKey = aliasCanonical(target, aliases);
    for (const QString& path : collectShpFiles(rootDir)) {
        const QString pathKey = aliasCanonical(QFileInfo(path).fileName(), aliases);
        if (pathKey.compare(targetKey, Qt::CaseInsensitive) == 0 && !matches.contains(path)) {
            matches << path;
        }
    }
    return matches;
}

QJsonArray stringListToJson(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values) array.append(value);
    return array;
}

QString displayCrossScaleLayerStatus(const QString& status)
{
    if (status == QStringLiteral("manual_actual_shp")) return QStringLiteral("人工调整");
    if (status == QStringLiteral("unmapped")) return QStringLiteral("未选择图层");
    if (status == QStringLiteral("auto_matched_actual_shp")) return QStringLiteral("已自动匹配");
    if (status == QStringLiteral("missing_large_scale_layer")) return QStringLiteral("缺少大比例尺对应图层");
    if (status == QStringLiteral("large_only")) return QStringLiteral("仅大比例尺存在");
    if (status.isEmpty()) return QStringLiteral("待确认");
    return status;
}

QString findRuleValueByName(const QDomElement& root, const QStringList& names, const QString& fallback = QString())
{
    const QDomNodeList rules = root.elementsByTagName(QStringLiteral("rule"));
    for (int i = 0; i < rules.count(); ++i) {
        const QDomElement rule = rules.at(i).toElement();
        const QString ruleName = rule.firstChildElement(QStringLiteral("name")).text().trimmed();
        for (const QString& name : names) {
            if (ruleName == name) {
                return rule.firstChildElement(QStringLiteral("value")).text().trimmed();
            }
        }
    }
    return fallback;
}

QString crossScaleRuleValue(const QString& xmlPath, const QStringList& names, const QString& fallback = QString(), int scale = 0)
{
    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return fallback;
    QDomDocument doc;
    if (!doc.setContent(&file)) return fallback;

    const QDomNodeList items = doc.elementsByTagName(QStringLiteral("check_item"));
    QDomElement selected, generic;
    for (int i = 0; i < items.count(); ++i) {
        const QDomElement item = items.at(i).toElement();
        const QString id = item.firstChildElement(QStringLiteral("id")).text().trimmed();
        const QString name = item.firstChildElement(QStringLiteral("name")).text().trimmed();
        if (id == kCrossScaleCheckItemId || name.contains(QStringLiteral("跨比例尺"))) {
            if (scale<=0) return findRuleValueByName(item,names,fallback);
            int itemScale=0;
            QDomNode parent=item.parentNode();
            while(!parent.isNull()) {
                if(parent.toElement().tagName()==QStringLiteral("scale")) {
                    itemScale=parseScaleString(parent.toElement().firstChildElement(QStringLiteral("value")).text());break;
                }
                parent=parent.parentNode();
            }
            if(itemScale==scale) { selected=item;break; }
            if(itemScale==0 && generic.isNull()) generic=item;
        }
    }
    if(selected.isNull()) selected=generic;
    if(!selected.isNull()) return findRuleValueByName(selected,names,fallback);
    return items.isEmpty()?findRuleValueByName(doc.documentElement(),names,fallback):fallback;
}

double crossScaleRuleDouble(const QString& xmlPath, const QStringList& names, double fallback)
{
    bool ok = false;
    const double value = crossScaleRuleValue(xmlPath, names).toDouble(&ok);
    return ok ? value : fallback;
}

int crossScaleRuleInt(const QString& xmlPath, const QStringList& names, int fallback)
{
    bool ok = false;
    const int value = crossScaleRuleValue(xmlPath, names).toInt(&ok);
    return ok ? value : fallback;
}

bool crossScaleRuleBool(const QString& xmlPath, const QStringList& names, bool fallback)
{
    const QString value = crossScaleRuleValue(xmlPath, names).trimmed().toLower();
    if (value.isEmpty()) return fallback;
    if (value == QStringLiteral("true") || value == QStringLiteral("1") || value == QStringLiteral("yes") || value == QStringLiteral("是")) return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("0") || value == QStringLiteral("no") || value == QStringLiteral("否")) return false;
    return fallback;
}

QString requiredRuleName(const QStringList& names)
{
    return names.isEmpty() ? QStringLiteral("unknown_cross_scale_rule") : names.first();
}

void appendMissingCrossScaleRule(QStringList* errors, const QStringList& names)
{
    if (errors) errors->append(QStringLiteral("%1").arg(requiredRuleName(names)));
}

double requiredCrossScaleRuleDouble(const QString& xmlPath, const QStringList& names, QStringList* errors)
{
    bool ok = false;
    const QString text = crossScaleRuleValue(xmlPath, names).trimmed();
    const double value = text.toDouble(&ok);
    if (!ok) {
        appendMissingCrossScaleRule(errors, names);
        return 0.0;
    }
    return value;
}

int requiredCrossScaleRuleInt(const QString& xmlPath, const QStringList& names, QStringList* errors)
{
    bool ok = false;
    const QString text = crossScaleRuleValue(xmlPath, names).trimmed();
    const int value = text.toInt(&ok);
    if (!ok) {
        appendMissingCrossScaleRule(errors, names);
        return 0;
    }
    return value;
}

bool requiredCrossScaleRuleBool(const QString& xmlPath, const QStringList& names, QStringList* errors)
{
    const QString value = crossScaleRuleValue(xmlPath, names).trimmed().toLower();
    if (value == QStringLiteral("true") || value == QStringLiteral("1") || value == QStringLiteral("yes") || value == QStringLiteral("是")) return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("0") || value == QStringLiteral("no") || value == QStringLiteral("否")) return false;
    appendMissingCrossScaleRule(errors, names);
    return false;
}

QSet<QString> normalizedLayerKeySet(const QStringList& values)
{
    QSet<QString> keys;
    for (const QString& value : values) {
        const QString key = normalizeLayerName(value);
        if (!key.isEmpty()) keys.insert(key);
    }
    return keys;
}

QMap<QString, QStringList> groupActualShpFiles(const QStringList& shpPaths, const QMap<QString, QString>& aliases, const QSet<QString>& excludedLayerKeys)
{
    QMap<QString, QStringList> grouped;
    for (const QString& path : shpPaths) {
        const QString key = aliasCanonical(QFileInfo(path).fileName(), aliases);
        if (key.isEmpty() || excludedLayerKeys.contains(key) || excludedLayerKeys.contains(normalizeLayerName(QFileInfo(path).fileName()))) continue;
        QStringList paths = grouped.value(key);
        if (!paths.contains(path)) paths << path;
        grouped[key] = paths;
    }
    return grouped;
}

double normalizedEditSimilarity(const QString& a, const QString& b)
{
    if (a == b) return 1.0;
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    QVector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j) prev[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    return 1.0 - static_cast<double>(prev[b.size()]) / static_cast<double>(std::max(a.size(), b.size()));
}

double lineDirectionDeg(const QgsGeometry& geometry)
{
    if (geometry.isNull()) return 0.0;
    double dx = 0.0;
    double dy = 0.0;
    auto collect = [&](const QgsPolylineXY& points) {
        for (int i = 1; i < points.size(); ++i) {
            dx += points[i].x() - points[i - 1].x();
            dy += points[i].y() - points[i - 1].y();
        }
    };
    if (geometry.isMultipart()) {
        const QgsMultiPolylineXY lines = geometry.asMultiPolyline();
        for (const QgsPolylineXY& line : lines) collect(line);
    } else {
        collect(geometry.asPolyline());
    }
    return std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
}

double compactness(const QgsGeometry& geometry)
{
    const double area = geometry.area();
    const double perimeter = geometry.length();
    if (area <= 0.0 || perimeter <= 0.0) return 0.0;
    return 4.0 * 3.14159265358979323846 * area / (perimeter * perimeter);
}

QStringList resolveGeoPdfAttributeFields(const QgsFields& smallFields, const QgsFields& largeFields)
{
    QStringList fields;
    auto appendIfComparable = [&](const QStringList& aliases) {
        for (const QString& alias : aliases) {
            if (smallFields.indexOf(alias) >= 0 && largeFields.indexOf(alias) >= 0) {
                fields << alias;
                return;
            }
        }
    };

    appendIfComparable(QStringList() << QStringLiteral("CODE") << QStringLiteral("code"));
    appendIfComparable(QStringList() << QStringLiteral("NAME") << QStringLiteral("name"));
    appendIfComparable(QStringList() << QStringLiteral("text_conte") << QStringLiteral("text_content") << QStringLiteral("text"));
    fields.removeDuplicates();
    return fields;
}

int geoPdfAnnotationTextFieldIndex(const QgsFields& fields)
{
    const QStringList names = {
        QStringLiteral("text_conte"),
        QStringLiteral("text_content"),
        QStringLiteral("text")
    };
    for (const QString& name : names) {
        const int index = fields.indexOf(name);
        if (index >= 0) return index;
    }
    return -1;
}

QString geoPdfAnnotationText(const QgsFeature& feature)
{
    return CrossScaleSupport::annotationText(feature);
}

bool isGeoPdfAnnotationFeature(const QgsFeature& feature)
{
    return !geoPdfAnnotationText(feature).isEmpty();
}

double featureAttributeScore(const QgsFeature& smallFeature, const QgsFeature& largeFeature, bool* hasComparableAttribute)
{
    if (hasComparableAttribute) *hasComparableAttribute = false;
    const QStringList fields = resolveGeoPdfAttributeFields(smallFeature.fields(), largeFeature.fields());
    double best = 0.0;
    for (const QString& field : fields) {
        const int smallIndex = smallFeature.fields().indexOf(field);
        const int largeIndex = largeFeature.fields().indexOf(field);
        if (smallIndex < 0 || largeIndex < 0) continue;
        const QString smallValue = smallFeature.attribute(smallIndex).toString().trimmed();
        const QString largeValue = largeFeature.attribute(largeIndex).toString().trimmed();
        if (smallValue.isEmpty() || largeValue.isEmpty()) continue;
        if (hasComparableAttribute) *hasComparableAttribute = true;
        best = std::max(best, smallValue == largeValue ? 1.0 : normalizedEditSimilarity(smallValue, largeValue));
    }
    return best;
}

struct CrossScaleCandidate
{
    QString path;
    QStringList sourceFids;
    QString logicalModel;
    QgsFeature feature;
    double distance = std::numeric_limits<double>::max();
    double spatialScore = 0.0;
    double geometryScore = 0.0;
    double attributeScore = 0.0;
    double totalScore = 0.0;
    bool hasComparableAttribute = false;
    bool textFallbackMatch = false;
    bool groupMatch = false;
    int candidateCount = 1;
    QJsonObject evidence;
};

double clampScore(double value)
{
    return std::max(0.0, std::min(1.0, value));
}

double centroidDistance(const QgsGeometry& first, const QgsGeometry& second)
{
    const QgsGeometry firstCenter = first.centroid();
    const QgsGeometry secondCenter = second.centroid();
    if (firstCenter.isNull() || secondCenter.isNull()) return first.distance(second);
    return firstCenter.distance(secondCenter);
}

double directionScore(const QgsGeometry& first, const QgsGeometry& second, double maxAngle)
{
    if (maxAngle <= 0.0) return 1.0;
    double diff = std::fabs(lineDirectionDeg(first) - lineDirectionDeg(second));
    while (diff > 180.0) diff -= 180.0;
    diff = std::min(diff, 180.0 - diff);
    return clampScore(1.0 - diff / maxAngle);
}

QgsGeometry combineGeometries(const QVector<QgsGeometry>& geometries)
{
    QgsGeometry combined;
    for (const QgsGeometry& geometry : geometries) {
        if (geometry.isNull() || geometry.isEmpty()) continue;
        combined = combined.isNull() ? geometry : combined.combine(geometry);
    }
    return combined;
}

struct LineEndpoint
{
    int candidateIndex=-1;
    QgsPointXY point;
    QgsPointXY inward;
};

QVector<LineEndpoint> lineEndpoints(const QgsGeometry& geometry,int candidateIndex)
{
    QVector<LineEndpoint> endpoints;
    if (geometry.isNull() || geometry.isEmpty() || geometry.type()!=QgsWkbTypes::LineGeometry) return endpoints;
    const auto append=[&](const QgsPolylineXY& line) {
        if (line.size()<2) return;
        endpoints.push_back({candidateIndex,line.first(),line.at(1)});
        endpoints.push_back({candidateIndex,line.last(),line.at(line.size()-2)});
    };
    if (geometry.isMultipart()) {
        for (const QgsPolylineXY& line:geometry.asMultiPolyline()) append(line);
    } else append(geometry.asPolyline());
    return endpoints;
}

QVector<QPair<int,int>> unambiguousLineEndpointJoins(const QVector<CrossScaleCandidate>& candidates,
    double joinTolerance,double maxJoinAngle)
{
    QVector<LineEndpoint> endpoints;
    for (int i=0;i<candidates.size();++i) endpoints+=lineEndpoints(candidates.at(i).feature.geometry(),i);
    struct Link { int first; int second; };
    QVector<Link> links;
    QVector<int> degree(endpoints.size(),0);
    for (int i=0;i<endpoints.size();++i) {
        for (int j=i+1;j<endpoints.size();++j) {
            const LineEndpoint& a=endpoints.at(i);
            const LineEndpoint& b=endpoints.at(j);
            if (a.candidateIndex==b.candidateIndex || a.point.distance(b.point)>joinTolerance) continue;
            const double ax=a.inward.x()-a.point.x(), ay=a.inward.y()-a.point.y();
            const double bx=b.inward.x()-b.point.x(), by=b.inward.y()-b.point.y();
            const double aLength=std::hypot(ax,ay), bLength=std::hypot(bx,by);
            if (aLength<=1e-8 || bLength<=1e-8) continue;
            const double continuationCos=qBound(-1.0,-(ax*bx+ay*by)/(aLength*bLength),1.0);
            const double bend=std::acos(continuationCos)*180.0/3.14159265358979323846;
            if (bend>maxJoinAngle) continue;
            links.push_back({i,j}); ++degree[i]; ++degree[j];
        }
    }
    QVector<QPair<int,int>> joins;
    for (const Link& link:links) {
        if (degree.at(link.first)!=1 || degree.at(link.second)!=1) continue;
        const int first=endpoints.at(link.first).candidateIndex;
        const int second=endpoints.at(link.second).candidateIndex;
        joins.push_back(qMakePair(qMin(first,second),qMax(first,second)));
    }
    std::sort(joins.begin(),joins.end(),[](const QPair<int,int>& a,const QPair<int,int>& b) {
        return a.first==b.first?a.second<b.second:a.first<b.first;
    });
    joins.erase(std::unique(joins.begin(),joins.end()),joins.end());
    return joins;
}

double weightedCrossScaleScore(double spatialScore, double geometryScore, double attributeScore,
    double spatialWeight, double geometryWeight, double attributeWeight)
{
    const double weightSum = std::max(0.001, spatialWeight + geometryWeight + attributeWeight);
    return (spatialScore * spatialWeight + geometryScore * geometryWeight + attributeScore * attributeWeight) / weightSum;
}

double polygonGeometryScore(const QgsGeometry& smallGeom, const QgsGeometry& largeGeom, double tolerance, QJsonObject* evidence)
{
    const double smallArea = smallGeom.area();
    const double largeArea = largeGeom.area();
    if (smallArea <= 0.0 || largeArea <= 0.0) return 0.0;

    const QgsGeometry intersection = smallGeom.intersection(largeGeom);
    const double overlapRatio = (!intersection.isNull() && !intersection.isEmpty())
        ? clampScore(intersection.area() / std::max(0.0001, smallArea))
        : 0.0;
    const double areaRatio = std::min(smallArea, largeArea) / std::max(smallArea, largeArea);
    const double centerDistance = centroidDistance(smallGeom, largeGeom);
    const double centerScore = tolerance > 0.0 ? clampScore(1.0 - centerDistance / tolerance) : 0.0;
    const double compactnessScore = clampScore(1.0 - std::fabs(compactness(smallGeom) - compactness(largeGeom)));
    const double score = 0.55 * overlapRatio + 0.20 * areaRatio + 0.15 * centerScore + 0.10 * compactnessScore;

    if (evidence) {
        (*evidence)[QStringLiteral("geometry_mode")] = QStringLiteral("polygon");
        (*evidence)[QStringLiteral("overlap_ratio")] = overlapRatio;
        (*evidence)[QStringLiteral("area_ratio")] = areaRatio;
        (*evidence)[QStringLiteral("centroid_distance")] = centerDistance;
        (*evidence)[QStringLiteral("center_score")] = centerScore;
        (*evidence)[QStringLiteral("compactness_score")] = compactnessScore;
    }
    return clampScore(score);
}

double lineGeometryScore(const QgsGeometry& smallGeom, const QgsGeometry& largeGeom, double tolerance, double maxDirectionAngle, QJsonObject* evidence)
{
    const double smallLength = smallGeom.length();
    const double largeLength = largeGeom.length();
    if (smallLength <= 0.0 || largeLength <= 0.0) return 0.0;

    const QgsGeometry bufferedLarge = largeGeom.buffer(std::max(0.001, tolerance), 8);
    const QgsGeometry covered = smallGeom.intersection(bufferedLarge);
    const double coverageRatio = (!covered.isNull() && !covered.isEmpty())
        ? clampScore(covered.length() / std::max(0.0001, smallLength))
        : 0.0;
    const double lengthRatio = std::min(smallLength, largeLength) / std::max(smallLength, largeLength);
    const double angleScore = directionScore(smallGeom, largeGeom, maxDirectionAngle);
    const double score = 0.65 * coverageRatio + 0.20 * lengthRatio + 0.15 * angleScore;

    if (evidence) {
        (*evidence)[QStringLiteral("geometry_mode")] = QStringLiteral("line");
        (*evidence)[QStringLiteral("coverage_ratio")] = coverageRatio;
        (*evidence)[QStringLiteral("length_ratio")] = lengthRatio;
        (*evidence)[QStringLiteral("direction_score")] = angleScore;
    }
    return clampScore(score);
}

double geometryScoreByType(const QgsGeometry& smallGeom, const QgsGeometry& largeGeom,
    double tolerance, double maxDirectionAngle, QJsonObject* evidence)
{
    if (smallGeom.type() == QgsWkbTypes::PolygonGeometry || largeGeom.type() == QgsWkbTypes::PolygonGeometry) {
        return polygonGeometryScore(smallGeom, largeGeom, tolerance, evidence);
    }
    if (smallGeom.type() == QgsWkbTypes::LineGeometry || largeGeom.type() == QgsWkbTypes::LineGeometry) {
        return lineGeometryScore(smallGeom, largeGeom, tolerance, maxDirectionAngle, evidence);
    }
    if (evidence) (*evidence)[QStringLiteral("geometry_mode")] = QStringLiteral("point_or_unknown");
    return 1.0;
}

CrossScaleCandidate buildSingleCandidate(const QgsFeature& smallFeature, const QgsGeometry& smallGeom,
    const QString& largePath, const QgsFeature& largeFeature, double positionTolerance,
    double geometryTolerance,
    double annotationSearchTolerance, bool textFallbackMatch,
    double spatialWeight, double geometryWeight, double attributeWeight,
    double maxDirectionAngle)
{
    CrossScaleCandidate candidate;
    candidate.path = largePath;
    candidate.feature = largeFeature;
    const QgsGeometry largeGeom = largeFeature.geometry();
    candidate.distance = smallGeom.distance(largeGeom);
    candidate.textFallbackMatch = textFallbackMatch;
    const double scoringTolerance = candidate.textFallbackMatch ? annotationSearchTolerance : positionTolerance;
    candidate.spatialScore = clampScore(1.0 - candidate.distance / std::max(0.001, scoringTolerance));
    candidate.geometryScore = geometryScoreByType(smallGeom, largeGeom, geometryTolerance, maxDirectionAngle, &candidate.evidence);
    candidate.evidence[QStringLiteral("position_tolerance_m")]=positionTolerance;
    candidate.evidence[QStringLiteral("geometry_tolerance_m")]=geometryTolerance;
    candidate.attributeScore = featureAttributeScore(smallFeature, largeFeature, &candidate.hasComparableAttribute);
    candidate.evidence[QStringLiteral("attribute_match_mode")] = candidate.hasComparableAttribute
        ? QStringLiteral("geopdf_parsed_fields")
        : QStringLiteral("none");
    if (candidate.textFallbackMatch) {
        candidate.spatialScore = std::max(candidate.spatialScore, 0.85);
        candidate.geometryScore = std::max(candidate.geometryScore, 0.85);
        candidate.evidence[QStringLiteral("match_strategy")] = QStringLiteral("cross_scale_same_text_annotation_fallback");
        candidate.evidence[QStringLiteral("annotation_search_tolerance")] = annotationSearchTolerance;
        candidate.evidence[QStringLiteral("annotation_text")] = geoPdfAnnotationText(smallFeature);
    }
    const double effectiveAttributeWeight = candidate.hasComparableAttribute ? attributeWeight : 0.0;
    candidate.totalScore = weightedCrossScaleScore(candidate.spatialScore, candidate.geometryScore, candidate.attributeScore,
        spatialWeight, geometryWeight, effectiveAttributeWeight);
    return candidate;
}

}

SeriesCrossScaleExpressionTaskDialog::SeriesCrossScaleExpressionTaskDialog(QWidget* parent, Qt::WindowFlags f)
    : QDialog(parent, f)
{
    setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowSystemMenuHint);
    buildUi();
}

void SeriesCrossScaleExpressionTaskDialog::setReportExportHandler(const std::function<void(const QStringList&)>& handler)
{
    m_reportExportHandler = handler;
}

void SeriesCrossScaleExpressionTaskDialog::buildUi()
{
    setWindowTitle(QStringLiteral("系列图内容检查 - 同系列跨比例尺表达检查"));
    resize(960, 720);

    auto* mainLayout = new QVBoxLayout(this);

    auto* pathLayout = new QGridLayout();
    pathLayout->addWidget(new QLabel(QStringLiteral("质检规则 ："), this), 0, 0);
    m_xmlEdit = new QLineEdit(this);
    m_xmlEdit->setPlaceholderText(QStringLiteral("请选择 XML 质检规则文件"));
    auto* xmlButton = new QPushButton(QStringLiteral("选择文件"), this);
    pathLayout->addWidget(m_xmlEdit, 0, 1);
    pathLayout->addWidget(xmlButton, 0, 2);

    pathLayout->addWidget(new QLabel(QStringLiteral("结果输出目录："), this), 1, 0);
    m_outputDirEdit = new QLineEdit(this);
    m_outputDirEdit->setPlaceholderText(QStringLiteral("请选择结果输出根目录"));
    auto* outputDirButton = new QPushButton(QStringLiteral("选择目录"), this);
    pathLayout->addWidget(m_outputDirEdit, 1, 1);
    pathLayout->addWidget(outputDirButton, 1, 2);
    mainLayout->addLayout(pathLayout);

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
    m_configTree->setColumnWidth(CrossColMainMap, 210);
    m_configTree->setColumnWidth(CrossColMainScale, 115);
    m_configTree->setColumnWidth(CrossColAssocMap, 250);
    m_configTree->setColumnWidth(CrossColAssocScale, 140);
    m_configTree->setColumnWidth(CrossColRelationType, 90);
    m_configTree->setColumnWidth(CrossColStatus, 150);
    m_configTree->header()->setStretchLastSection(false);
    m_configTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableLayout->addWidget(m_configTree);
    mainLayout->addWidget(tableGroup, 1);

    auto* buttonLayout = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始检查"), this);
    m_reportExportButton = new QPushButton(QStringLiteral("质检报告导出"), this);
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(m_reportExportButton);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QStringLiteral("等待开始..."));
    mainLayout->addWidget(m_progressBar);

    connect(xmlButton, &QPushButton::clicked, this, [this]() { browseXmlRule(); });
    connect(outputDirButton, &QPushButton::clicked, this, [this]() { browseOutputDirectory(); });
    connect(addRowBtn, &QPushButton::clicked, this, &SeriesCrossScaleExpressionTaskDialog::addConfigRow);
    connect(removeRowBtn, &QPushButton::clicked, this, &SeriesCrossScaleExpressionTaskDialog::removeConfigRow);
    connect(clearAllRowsBtn, &QPushButton::clicked, this, [this]() {
        if (m_configTree->topLevelItemCount() > 0 && QMessageBox::question(this, QStringLiteral("确认"), QStringLiteral("确定要清空所有地图配置吗？")) == QMessageBox::Yes) {
            m_configTree->clear();
            checkTableStatus();
        }
    });
    connect(m_startButton, &QPushButton::clicked, this, &SeriesCrossScaleExpressionTaskDialog::startCheck);
    connect(m_reportExportButton, &QPushButton::clicked, this, &SeriesCrossScaleExpressionTaskDialog::exportReport);
}

void SeriesCrossScaleExpressionTaskDialog::browseXmlRule()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择 XML 质检规则"), QString(), QStringLiteral("XML 文件 (*.xml);;所有文件 (*)"));
    if (!path.isEmpty()) m_xmlEdit->setText(path);
}

void SeriesCrossScaleExpressionTaskDialog::browseOutputDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择结果输出根目录"), m_outputDirEdit ? m_outputDirEdit->text() : QString());
    if (!path.isEmpty()) m_outputDirEdit->setText(path);
}

void SeriesCrossScaleExpressionTaskDialog::addConfigRow()
{
    const QStringList mainPaths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择主检查地图 GeoPDF"), QString(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    if (mainPaths.isEmpty()) {
        addEmptyConfigRow();
        checkTableStatus();
        return;
    }

    const QStringList assocPaths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择关联地图 GeoPDF（可多选）"), QFileInfo(mainPaths.first()).absolutePath(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    addConfigRowsFromPaths(mainPaths, assocPaths);
}

void SeriesCrossScaleExpressionTaskDialog::addConfigRowsFromPaths(const QStringList& mainPdfPaths, const QStringList& assocPdfPaths)
{
    QStringList duplicateGroups;
    QStringList duplicateGroupKeys;
    bool removedDuplicateRow = false;
    for (const QString& mainPdf : mainPdfPaths) {
        const int row = addEmptyConfigRow();
        if (row < 0) continue;
        if (auto* mainEdit = configLineEdit(row, CrossColMainMap)) mainEdit->setText(mainPdf);
        if (auto* assocEdit = configLineEdit(row, CrossColAssocMap)) assocEdit->setText(assocPdfPaths.join(QStringLiteral(";")));
        autoFillRowScales(row);

        const auto* candidateMainEdit = configLineEdit(row, CrossColMainMap);
        const auto* candidateAssocEdit = configLineEdit(row, CrossColAssocMap);
        const QString candidateMainPath = candidateMainEdit ? candidateMainEdit->text().trimmed() : QString();
        const QStringList candidateAssocPaths = candidateAssocEdit
            ? splitList(candidateAssocEdit->text()) : QStringList();
        bool duplicate = false;
        for (int existingRow = 0; existingRow < m_configTree->topLevelItemCount(); ++existingRow) {
            if (existingRow == row) continue;
            const auto* existingMainEdit = configLineEdit(existingRow, CrossColMainMap);
            const auto* existingAssocEdit = configLineEdit(existingRow, CrossColAssocMap);
            if (existingMainEdit && SeriesMapConfigurationDeduplication::sameMapGroup(
                    candidateMainPath, candidateAssocPaths,
                    existingMainEdit->text(), existingAssocEdit ? splitList(existingAssocEdit->text()) : QStringList())) {
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

int SeriesCrossScaleExpressionTaskDialog::addEmptyConfigRow()
{
    if (!m_configTree) return -1;

    auto* item = new QTreeWidgetItem();
    item->setText(CrossColRelationType, QStringLiteral("待判断"));
    item->setText(CrossColStatus, QStringLiteral("未完成"));
    m_configTree->addTopLevelItem(item);
    const int row = m_configTree->indexOfTopLevelItem(item);

    auto makePathWidget = [&](const QString& placeholder, const std::function<void()>& browseFn) {
        auto* widget = new QWidget();
        auto* layout = new QHBoxLayout(widget);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* edit = new MapPathDisplayEdit();
        edit->setPlaceholderText(placeholder);
        auto* button = new QPushButton(QStringLiteral("选择"));
        button->setFixedWidth(52);
        layout->addWidget(edit);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, this, browseFn);
        connect(edit, &QLineEdit::textChanged, this, [this, row]() {
            autoFillRowScales(row);
            checkTableStatus();
        });
        return widget;
    };

    m_configTree->setItemWidget(item, CrossColMainMap, makePathWidget(QStringLiteral("选择主检查地图 GeoPDF"), [this, row]() { browseTableMainMap(row); }));
    m_configTree->setItemWidget(item, CrossColAssocMap, makePathWidget(QStringLiteral("选择关联地图 GeoPDF"), [this, row]() { browseTableAssocMap(row); }));

    auto* mainScaleEdit = new QLineEdit();
    mainScaleEdit->setPlaceholderText(QStringLiteral("如 50000"));
    auto* assocScaleEdit = new QLineEdit();
    assocScaleEdit->setPlaceholderText(QStringLiteral("多值用分号分隔"));
    m_configTree->setItemWidget(item, CrossColMainScale, mainScaleEdit);
    m_configTree->setItemWidget(item, CrossColAssocScale, assocScaleEdit);
    connect(mainScaleEdit, &QLineEdit::textChanged, this, [this]() { checkTableStatus(); });
    connect(assocScaleEdit, &QLineEdit::textChanged, this, [this]() { checkTableStatus(); });

    return row;
}

void SeriesCrossScaleExpressionTaskDialog::removeConfigRow()
{
    if (!m_configTree) return;
    const QList<QTreeWidgetItem*> selectedItems = m_configTree->selectedItems();
    if (selectedItems.isEmpty()) return;

    QSet<int> parentRowsToRemove;
    QMap<int, QSet<int>> childRowsToRemove;
    for (auto* item : selectedItems) {
        if (!item) continue;
        if (QTreeWidgetItem* parent = item->parent()) {
            const int row = m_configTree->indexOfTopLevelItem(parent);
            const int childIndex = parent->indexOfChild(item);
            if (row >= 0 && childIndex >= 0) childRowsToRemove[row].insert(childIndex);
        } else {
            const int row = m_configTree->indexOfTopLevelItem(item);
            if (row >= 0) parentRowsToRemove.insert(row);
        }
    }

    for (auto it = childRowsToRemove.constBegin(); it != childRowsToRemove.constEnd(); ++it) {
        const int row = it.key();
        if (parentRowsToRemove.contains(row)) continue;

        auto* assocEdit = configLineEdit(row, CrossColAssocMap);
        auto* assocScaleEdit = configLineEdit(row, CrossColAssocScale);
        if (!assocEdit) continue;

        QStringList assocPaths = splitList(assocEdit->text());
        QStringList assocScaleTexts = assocScaleEdit ? splitList(assocScaleEdit->text()) : QStringList();
        QList<int> childIndexes = it.value().values();
        std::sort(childIndexes.begin(), childIndexes.end(), std::greater<int>());
        for (int childIndex : childIndexes) {
            if (childIndex >= 0 && childIndex < assocPaths.size()) assocPaths.removeAt(childIndex);
            if (childIndex >= 0 && childIndex < assocScaleTexts.size()) assocScaleTexts.removeAt(childIndex);
        }

        {
            QSignalBlocker blocker(assocEdit);
            assocEdit->setText(assocPaths.join(QStringLiteral(";")));
        }
        if (assocScaleEdit) {
            QSignalBlocker blocker(assocScaleEdit);
            assocScaleEdit->setText(assocScaleTexts.join(QStringLiteral(";")));
        }
    }

    QList<int> rowList = parentRowsToRemove.values();
    std::sort(rowList.begin(), rowList.end(), std::greater<int>());
    for (int row : rowList) delete m_configTree->takeTopLevelItem(row);

    rebindConfigTreeButtons();
    checkTableStatus();
}

void SeriesCrossScaleExpressionTaskDialog::browseTableMainMap(int row)
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择主检查地图 GeoPDF"), QString(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    if (paths.isEmpty()) return;
    if (auto* edit = configLineEdit(row, CrossColMainMap)) edit->setText(paths.first());
    autoFillRowScales(row);
    if (paths.size() > 1) {
        QStringList extraPaths = paths;
        extraPaths.removeFirst();
        QStringList assocPaths;
        if (auto* assocEdit = configLineEdit(row, CrossColAssocMap)) assocPaths = splitList(assocEdit->text().trimmed());
        addConfigRowsFromPaths(extraPaths, assocPaths);
    } else {
        checkTableStatus();
    }
}

void SeriesCrossScaleExpressionTaskDialog::browseTableAssocMap(int row)
{
    auto* edit = configLineEdit(row, CrossColAssocMap);
    if (!edit) return;
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择关联地图 GeoPDF"), QString(), QStringLiteral("GeoPDF 文件 (*.pdf);;所有文件 (*)"));
    if (paths.isEmpty()) return;
    QStringList current = splitList(edit->text());
    for (const QString& path : paths) {
        if (!current.contains(path)) current << path;
    }
    edit->setText(current.join(QStringLiteral(";")));
    autoFillRowScales(row);
    checkTableStatus();
}

void SeriesCrossScaleExpressionTaskDialog::rebindConfigTreeButtons()
{
    if (!m_configTree) return;
    for (int i = 0; i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_configTree->topLevelItem(i);
        auto* mainWidget = m_configTree->itemWidget(item, CrossColMainMap);
        auto* assocWidget = m_configTree->itemWidget(item, CrossColAssocMap);
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

QLineEdit* SeriesCrossScaleExpressionTaskDialog::configLineEdit(int row, int col) const
{
    if (!m_configTree) return nullptr;
    QTreeWidgetItem* item = m_configTree->topLevelItem(row);
    if (!item) return nullptr;
    QWidget* widget = m_configTree->itemWidget(item, col);
    if (!widget) return nullptr;
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) return edit;
    return widget->findChild<QLineEdit*>();
}

int SeriesCrossScaleExpressionTaskDialog::scaleForGeoPdf(const QString& pdfPath) const
{
    const int nameScale = parseScaleFromPdfName(pdfPath);
    if (nameScale > 0) return nameScale;

    const QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
    if (!shpDir.isEmpty()) {
        const int inferredScale = inferScaleDenominatorFromShpDir(shpDir);
        if (inferredScale > 0) return inferredScale;
    }
    return 0;
}

void SeriesCrossScaleExpressionTaskDialog::autoFillRowScales(int row)
{
    if (!m_configTree || row < 0 || row >= m_configTree->topLevelItemCount()) return;

    auto* mainEdit = configLineEdit(row, CrossColMainMap);
    auto* mainScaleEdit = configLineEdit(row, CrossColMainScale);
    if (mainEdit && mainScaleEdit && parseScaleString(mainScaleEdit->text()) <= 0) {
        const int mainScale = scaleForGeoPdf(mainEdit->text().trimmed());
        if (mainScale > 0) {
            mainScaleEdit->setText(QString::number(mainScale));
        } else if (!mainEdit->text().trimmed().isEmpty()) {
            mainScaleEdit->setPlaceholderText(QStringLiteral("主图比例尺未识别"));
        }
    }

    auto* assocEdit = configLineEdit(row, CrossColAssocMap);
    auto* assocScaleEdit = configLineEdit(row, CrossColAssocScale);
    if (!assocEdit || !assocScaleEdit) return;

    const QStringList assocPdfs = splitList(assocEdit->text().trimmed());
    if (assocPdfs.isEmpty()) return;

    QStringList existingScales = splitList(assocScaleEdit->text().trimmed());
    QStringList assocScales;
    bool changed = existingScales.size() != assocPdfs.size();
    for (int i = 0; i < assocPdfs.size(); ++i) {
        QString scaleText = i < existingScales.size() ? existingScales[i].trimmed() : QString();
        if (parseScaleString(scaleText) <= 0) {
            const int scale = scaleForGeoPdf(assocPdfs[i]);
            if (scale > 0) {
                scaleText = QString::number(scale);
                changed = true;
            }
        }
        assocScales << scaleText;
    }

    if (changed) {
        assocScaleEdit->setText(assocScales.join(QStringLiteral(";")));
    }
    if (assocScaleEdit->text().trimmed().isEmpty()) {
        assocScaleEdit->setPlaceholderText(QStringLiteral("关联图比例尺未识别"));
    }
}

void SeriesCrossScaleExpressionTaskDialog::checkTableStatus()
{
    for (int i = 0; m_configTree && i < m_configTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* rowItem = m_configTree->topLevelItem(i);
        if (!rowItem) continue;

        const QString mainPath = configLineEdit(i, CrossColMainMap) ? configLineEdit(i, CrossColMainMap)->text().trimmed() : QString();
        const QString assocText = configLineEdit(i, CrossColAssocMap) ? configLineEdit(i, CrossColAssocMap)->text().trimmed() : QString();
        const int mainScale = configLineEdit(i, CrossColMainScale) ? parseScaleString(configLineEdit(i, CrossColMainScale)->text()) : 0;
        const QStringList assocPaths = splitList(assocText);
        const QStringList assocScaleTexts = configLineEdit(i, CrossColAssocScale) ? splitList(configLineEdit(i, CrossColAssocScale)->text()) : QStringList();

        bool valid = QFileInfo(mainPath).exists() && !assocPaths.isEmpty() && mainScale > 0 && assocScaleTexts.size() >= assocPaths.size();
        for (const QString& path : assocPaths) {
            if (!QFileInfo(path).exists()) valid = false;
        }

        bool hasDifferentScale = false;
        for (int j = 0; j < assocPaths.size(); ++j) {
            const int assocScale = j < assocScaleTexts.size() ? parseScaleString(assocScaleTexts[j]) : 0;
            if (assocScale > 0 && assocScale != mainScale) hasDifferentScale = true;
            if (assocScale <= 0) valid = false;
        }

        rowItem->setText(CrossColRelationType, hasDifferentScale ? QStringLiteral("跨比例尺") : QStringLiteral("待判断"));
        if (mainPath.isEmpty() && assocText.isEmpty()) rowItem->setText(CrossColStatus, QStringLiteral("空白"));
        else if (mainScale <= 0) rowItem->setText(CrossColStatus, QStringLiteral("待填写主图比例尺"));
        else if (assocPaths.isEmpty()) rowItem->setText(CrossColStatus, QStringLiteral("待填写关联地图"));
        else if (assocScaleTexts.size() < assocPaths.size()) rowItem->setText(CrossColStatus, QStringLiteral("待填写关联图比例尺"));
        else if (!hasDifferentScale) rowItem->setText(CrossColStatus, QStringLiteral("无跨比例尺组合"));
        else rowItem->setText(CrossColStatus, valid ? QStringLiteral("已完成") : QStringLiteral("未完成"));
    }
    refreshConfigTreeChildren();
}

void SeriesCrossScaleExpressionTaskDialog::refreshConfigTreeChildren()
{
    if (!m_configTree) return;

    for (int row = 0; row < m_configTree->topLevelItemCount(); ++row) {
        QTreeWidgetItem* parent = m_configTree->topLevelItem(row);
        if (!parent) continue;

        while (parent->childCount() > 0) {
            delete parent->takeChild(0);
        }

        const QString mainPath = configLineEdit(row, CrossColMainMap) ? configLineEdit(row, CrossColMainMap)->text().trimmed() : QString();
        const QString mainScaleText = configLineEdit(row, CrossColMainScale) ? configLineEdit(row, CrossColMainScale)->text().trimmed() : QString();
        const QString assocText = configLineEdit(row, CrossColAssocMap) ? configLineEdit(row, CrossColAssocMap)->text().trimmed() : QString();
        const QString assocScaleText = configLineEdit(row, CrossColAssocScale) ? configLineEdit(row, CrossColAssocScale)->text().trimmed() : QString();

        const int mainScale = parseScaleString(mainScaleText);
        const QStringList assocPaths = splitList(assocText);
        const QStringList assocScaleTexts = splitList(assocScaleText);

        parent->setText(CrossColMainMap, QFileInfo(mainPath).fileName());
        parent->setToolTip(CrossColMainMap, mainPath);
        parent->setExpanded(!assocPaths.isEmpty());

        for (int i = 0; i < assocPaths.size(); ++i) {
            const QString assocPath = assocPaths[i];
            const QString assocScaleTextForRow = i < assocScaleTexts.size() ? assocScaleTexts[i] : QString();
            const int assocScale = parseScaleString(assocScaleTextForRow);

            QString relationText = QStringLiteral("待判断");
            if (mainScale > 0 && assocScale > 0) {
                relationText = (mainScale == assocScale) ? QStringLiteral("同比例尺") : QStringLiteral("跨比例尺");
            }

            QString statusText = QStringLiteral("未完成");
            if (mainScale <= 0) {
                statusText = QStringLiteral("待填写主图比例尺");
            } else if (assocScale <= 0) {
                statusText = QStringLiteral("待填写关联图比例尺");
            } else if (!QFileInfo(mainPath).exists() || !QFileInfo(assocPath).exists()) {
                statusText = QStringLiteral("文件不存在");
            } else if (assocScale == mainScale) {
                statusText = QStringLiteral("无跨比例尺组合");
            } else {
                statusText = QStringLiteral("已完成");
            }

            auto* child = new QTreeWidgetItem();
            child->setText(CrossColMainMap, QStringLiteral("关联地图 %1").arg(i + 1));
            child->setText(CrossColMainScale, mainScale > 0 ? QString::number(mainScale) : mainScaleText);
            child->setText(CrossColAssocMap, QFileInfo(assocPath).fileName());
            child->setToolTip(CrossColAssocMap, assocPath);
            child->setText(CrossColAssocScale, assocScaleTextForRow);
            child->setText(CrossColRelationType, relationText);
            child->setText(CrossColStatus, statusText);
            parent->addChild(child);
        }
    }
}
QVector<SeriesCrossScaleExpressionTaskDialog::MapTask> SeriesCrossScaleExpressionTaskDialog::buildConfiguredTasks() const
{
    QVector<MapTask> tasks;
    for (int row = 0; m_configTree && row < m_configTree->topLevelItemCount(); ++row) {
        QTreeWidgetItem* rowItem = m_configTree->topLevelItem(row);
        if (!rowItem || rowItem->text(CrossColStatus) != QStringLiteral("已完成")) continue;

        const QString mainPdf = configLineEdit(row, CrossColMainMap) ? configLineEdit(row, CrossColMainMap)->text().trimmed() : QString();
        const int mainScale = configLineEdit(row, CrossColMainScale) ? parseScaleString(configLineEdit(row, CrossColMainScale)->text()) : 0;
        const QStringList assocPdfs = configLineEdit(row, CrossColAssocMap) ? splitList(configLineEdit(row, CrossColAssocMap)->text()) : QStringList();
        const QStringList assocScaleTexts = configLineEdit(row, CrossColAssocScale) ? splitList(configLineEdit(row, CrossColAssocScale)->text()) : QStringList();
        const QString mainName = QFileInfo(mainPdf).completeBaseName();
        for (int i = 0; i < assocPdfs.size(); ++i) {
            const int assocScale = i < assocScaleTexts.size() ? parseScaleString(assocScaleTexts[i]) : 0;
            if (assocScale <= 0 || assocScale == mainScale) continue;
            const QString assocName = QFileInfo(assocPdfs[i]).completeBaseName();
            const bool mainIsSmall = mainScale > assocScale;
            MapTask task;
            task.mainPdfPath = mainPdf;
            task.assocPdfPath = assocPdfs[i];
            task.mainScale = mainScale;
            task.assocScale = assocScale;
            task.smallScaleName = mainIsSmall ? mainName : assocName;
            task.largeScaleName = mainIsSmall ? assocName : mainName;
            task.smallScale = mainIsSmall ? mainScale : assocScale;
            task.largeScale = mainIsSmall ? assocScale : mainScale;
            task.pairName = QStringLiteral("主检查地图：%1 -> 关联地图：%2（检查关系：%3 - %4）")
                .arg(mainName, assocName, task.largeScaleName, task.smallScaleName);
            tasks.push_back(task);
        }
    }
    return tasks;
}

void SeriesCrossScaleExpressionTaskDialog::appendRuntimeLog(const QString& text)
{
    if (text.trimmed().isEmpty()) return;
    m_runtimeLogLines.append(text);
    QCoreApplication::processEvents();
}

QString SeriesCrossScaleExpressionTaskDialog::outputRootDir() const
{
    QString root = m_outputDirEdit ? m_outputDirEdit->text().trimmed() : QString();
    return root.isEmpty() ? QDir::current().filePath(QStringLiteral("output_results")) : root;
}

QStringList SeriesCrossScaleExpressionTaskDialog::currentReportResultJsonPaths() const
{
    QStringList paths;
    QDir root(outputRootDir());
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

void SeriesCrossScaleExpressionTaskDialog::exportReport()
{
    if (!m_reportExportHandler) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("质检报告导出功能未初始化。"));
        return;
    }
    m_reportExportHandler(currentReportResultJsonPaths());
}

QString SeriesCrossScaleExpressionTaskDialog::getPreprocessPythonPath() const
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/apps/Python39/python.exe"));
}

QString SeriesCrossScaleExpressionTaskDialog::getPreprocessScriptPath(const QString& scriptName) const
{
    const QString baseDir = QString::fromUtf8(qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01"));
    return baseDir.isEmpty() ? QString() : QDir(baseDir).filePath(QStringLiteral("MPQIS/preprocess_scripts/%1").arg(scriptName));
}

bool SeriesCrossScaleExpressionTaskDialog::runPreprocessScript(const QString& scriptName, const QStringList& args, QString* message)
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
        env.insert(QStringLiteral("PATH"), QDir(mpqisDir).filePath(QStringLiteral("MPQIS/bin")) + QStringLiteral(";") + env.value(QStringLiteral("PATH")));
    }
    proc.setProcessEnvironment(env);
    proc.start(pythonPath, QStringList() << scriptPath << args);
    if (!proc.waitForStarted(5000)) {
        if (message) *message = QStringLiteral("无法启动脚本：%1").arg(scriptName);
        return false;
    }

    while (proc.state() != QProcess::NotRunning) {
        proc.waitForFinished(200);
        const QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (!out.isEmpty()) appendRuntimeLog(out);
        if (!err.isEmpty()) appendRuntimeLog(QStringLiteral("[stderr] %1").arg(err));
        QCoreApplication::processEvents();
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (message) *message = QStringLiteral("%1 执行失败").arg(scriptName);
        return false;
    }
    if (message) *message = QStringLiteral("%1 执行成功").arg(scriptName);
    return true;
}

QString SeriesCrossScaleExpressionTaskDialog::resolvePreprocessedShpDir(const QString& pdfPath, const QString& outputRootDir) const
{
    const QFileInfo fi(pdfPath);
    const QString baseName = fi.completeBaseName();
    const QString pdfDir = fi.absolutePath();
    const QStringList searchDirs = { outputRootDir, pdfDir };

    for (const QString& root : searchDirs) {
        const QString exact = QDir(root).filePath(baseName + QStringLiteral("_shp"));
        if (validatePreprocessedShpDir(exact)) return exact;
    }
    for (const QString& root : searchDirs) {
        QDir dir(root);
        const QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& subDir : subDirs) {
            if (!subDir.endsWith(QStringLiteral("_shp"), Qt::CaseInsensitive)) continue;
            const QString subBase = subDir.left(subDir.length() - 4);
            if (baseName.contains(subBase, Qt::CaseInsensitive) || subBase.contains(baseName, Qt::CaseInsensitive)) {
                const QString fuzzyDir = dir.filePath(subDir);
                if (validatePreprocessedShpDir(fuzzyDir)) return fuzzyDir;
            }
        }
    }
    return QString();
}

SeriesCrossScaleExpressionTaskDialog::PreprocessResult SeriesCrossScaleExpressionTaskDialog::preprocessGeoPdfIfNeeded(const QString& pdfPath, const QString& outputRootDir)
{
    PreprocessResult result;
    result.pdfPath = pdfPath;
    const QFileInfo fi(pdfPath);
    if (!fi.exists()) {
        result.errorMessage = QStringLiteral("文件不存在");
        return result;
    }

    const QString baseName = fi.completeBaseName();
    QString geojsonDir = QDir(outputRootDir).filePath(baseName + QStringLiteral("_geojson"));
    QString shpDir = QDir(outputRootDir).filePath(baseName + QStringLiteral("_shp"));
    result.shpDir = shpDir;
    QDir().mkpath(geojsonDir);
    QDir().mkpath(shpDir);

    const QString jsxTemplatePath = getPreprocessScriptPath(QStringLiteral("export_to_geojson.jsx"));
    QString msg;
    QStringList pdfToGeojsonArgs;
    pdfToGeojsonArgs << QStringLiteral("--pdf-files") << pdfPath
        << QStringLiteral("--output-dir") << outputRootDir
        << QStringLiteral("--jsx-template") << jsxTemplatePath;
    appendRuntimeLog(QStringLiteral("开始解析 PDF: %1").arg(pdfPath));
    if (!runPreprocessScript(QStringLiteral("launch_ai2.py"), pdfToGeojsonArgs, &msg)) {
        result.errorStage = QStringLiteral("pdf_to_geojson");
        result.errorMessage = msg;
        return result;
    }

    QStringList geojsonFiles = QDir(geojsonDir).entryList(QStringList() << QStringLiteral("*.geojson"), QDir::Files);
    if (geojsonFiles.isEmpty()) {
        QString nestedDir = QDir(geojsonDir).filePath(QFileInfo(geojsonDir).fileName());
        if (QDir(nestedDir).exists()) {
            geojsonFiles = QDir(nestedDir).entryList(QStringList() << QStringLiteral("*.geojson"), QDir::Files);
            if (!geojsonFiles.isEmpty()) geojsonDir = nestedDir;
        }
    }
    if (geojsonFiles.isEmpty()) {
        result.errorStage = QStringLiteral("pdf_to_geojson");
        result.errorMessage = QStringLiteral("PDF 已调用解析，但未生成任何 GeoJSON 文件。");
        return result;
    }

    QStringList geojsonToShpArgs;
    geojsonToShpArgs << QStringLiteral("--geopdf") << pdfPath
        << QStringLiteral("--geojson-folder") << geojsonDir
        << QStringLiteral("--output-folder") << shpDir;
    appendRuntimeLog(QStringLiteral("开始转换 GeoJSON 为 SHP: %1").arg(pdfPath));
    if (!runPreprocessScript(QStringLiteral("geojson2shp2.py"), geojsonToShpArgs, &msg)) {
        result.errorStage = QStringLiteral("geojson_to_shp");
        result.errorMessage = msg;
        return result;
    }
    if (!validatePreprocessedShpDir(shpDir)) {
        result.errorStage = QStringLiteral("validate_shp_output");
        result.errorMessage = QStringLiteral("SHP 输出目录为空或不可用：%1").arg(shpDir);
        return result;
    }
    QDir dirToRemove(geojsonDir);
    if (dirToRemove.exists()) dirToRemove.removeRecursively();
    result.success = true;
    return result;
}

bool SeriesCrossScaleExpressionTaskDialog::prepareTaskShpDirs(QVector<MapTask>* tasks)
{
    if (!tasks) return false;

    QMap<QString, QString> pdfToShpDir;
    QMap<QString, QJsonObject> pdfAudits;
    QStringList pdfPaths;
    for (const MapTask& task : *tasks) {
        pdfPaths << task.mainPdfPath << task.assocPdfPath;
    }
    pdfPaths.removeDuplicates();

    int current = 0;
    for (const QString& pdfPath : pdfPaths) {
        QString shpDir = resolvePreprocessedShpDir(pdfPath, outputRootDir());
        if (shpDir.isEmpty()) {
            PreprocessResult result = preprocessGeoPdfIfNeeded(pdfPath, outputRootDir());
            if (!result.success) {
                QMessageBox::critical(this, QStringLiteral("预处理失败"),
                    QStringLiteral("GeoPDF 预处理失败。\n文件：%1\n阶段：%2\n原因：%3")
                    .arg(pdfPath, result.errorStage, result.errorMessage));
                return false;
            }
            shpDir = result.shpDir;
        }
        pdfToShpDir[pdfPath] = shpDir;
        pdfAudits[pdfPath]=SeriesSourceAudit::run(getPreprocessPythonPath(),getPreprocessScriptPath(QStringLiteral("audit_series_pdf_source.py")),
            pdfPath,shpDir,QDir(outputRootDir()).filePath(QStringLiteral("系列图检查/.series_cache/source_audits")));
        ++current;
        if (m_progressBar) m_progressBar->setValue(qMin(50, current * 50 / qMax(1, pdfPaths.size())));
    }

    for (MapTask& task : *tasks) {
        const bool mainIsSmall = task.mainScale > task.assocScale;
        task.smallShpDir = mainIsSmall ? pdfToShpDir.value(task.mainPdfPath) : pdfToShpDir.value(task.assocPdfPath);
        task.largeShpDir = mainIsSmall ? pdfToShpDir.value(task.assocPdfPath) : pdfToShpDir.value(task.mainPdfPath);
        task.sourceAudits=QJsonArray{pdfAudits.value(task.mainPdfPath),pdfAudits.value(task.assocPdfPath)};
    }
    return true;
}

SeriesCrossScaleExpressionTaskDialog::CrossScaleRules SeriesCrossScaleExpressionTaskDialog::readCrossScaleRules(const QString& xmlPath, int scale) const
{
    CrossScaleRules rules;
    QStringList errors;
    auto number = [&](const QStringList& names,double fallback,bool required=false) {
        const QString text=crossScaleRuleValue(xmlPath,names,QString(),scale);
        if(text.isEmpty() && !required) return fallback;
        bool ok=false;const double value=text.toDouble(&ok);
        if(!ok || !std::isfinite(value)) { errors << names.first();return fallback; }
        return value;
    };
    rules.toleranceModel=crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_tolerance_model")},QStringLiteral("legacy_fixed_m"),scale).trimmed();
    rules.spaceToleranceMm=number({QStringLiteral("series_cross_scale_space_tolerance_mm"),QStringLiteral("空间容差")},.5,rules.toleranceModel==QStringLiteral("legacy_fixed_m"));
    rules.candidateSearchToleranceMm=number({QStringLiteral("series_cross_scale_candidate_search_tolerance_mm")},1.0);
    const QString toleranceProfilesText=crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_tolerance_profiles")},QString(),scale);
    if(rules.toleranceModel==QStringLiteral("scale_pair_v1")) {
        QJsonParseError parseError;
        const auto document=QJsonDocument::fromJson(toleranceProfilesText.toUtf8(),&parseError);
        if(parseError.error!=QJsonParseError::NoError || !document.isObject()) errors << QStringLiteral("series_cross_scale_tolerance_profiles");
        else {
            const auto profiles=document.object();
            for(auto it=profiles.constBegin();it!=profiles.constEnd();++it) {
                const QString key=CrossScaleSupport::key(it.key());
                if(rules.toleranceProfiles.contains(key)) errors << QStringLiteral("series_cross_scale_tolerance_profiles duplicate class: ")+key;
                else rules.toleranceProfiles[key]=it.value();
            }
        }
    } else if(rules.toleranceModel!=QStringLiteral("legacy_fixed_m") || !toleranceProfilesText.trimmed().isEmpty())
        errors << QStringLiteral("series_cross_scale_tolerance_model/profiles");
    rules.geomToleranceM=number({QStringLiteral("series_cross_scale_geometry_tolerance_m"),QStringLiteral("几何判定容差")},0,rules.toleranceModel==QStringLiteral("legacy_fixed_m"));
    rules.annotationAnchorToleranceMm=number({QStringLiteral("series_cross_scale_annotation_anchor_tolerance_mm")},.5);
    rules.matchedThreshold=number({QStringLiteral("series_cross_scale_matched_threshold"),QStringLiteral("匹配阈值")},0,true);
    // Legacy weights describe diagnostic similarity only, never the support decision.
    rules.spatialWeight=number({QStringLiteral("series_cross_scale_spatial_weight"),QStringLiteral("空间权重")},.5);
    rules.geometryWeight=number({QStringLiteral("series_cross_scale_geometry_weight"),QStringLiteral("几何权重")},.5);
    rules.attributeWeight=number({QStringLiteral("series_cross_scale_attribute_weight"),QStringLiteral("属性权重")},0);
    rules.directionAngleMax=number({QStringLiteral("series_cross_scale_direction_angle_max"),QStringLiteral("走向夹角阈值")},180);
    rules.uniqueScoreMargin=number({QStringLiteral("series_cross_scale_unique_score_margin")},.10);
    if(rules.toleranceModel==QStringLiteral("scale_pair_v1"))
        rules.directionAngleMax=number({QStringLiteral("series_cross_scale_direction_angle_max")},30);
    rules.candidateLimit=static_cast<int>(qBound(0.0,number({QStringLiteral("series_cross_scale_candidate_limit"),QStringLiteral("候选遍历上限")},0),1000000.0));
    rules.maxUncoveredSegmentLength=number({QStringLiteral("series_cross_scale_max_uncovered_segment_length_m")},-1);
    rules.maxUncoveredSegmentLengthMm=number({QStringLiteral("series_cross_scale_max_uncovered_segment_length_mm")},-1);
    rules.lineJoinToleranceM=number({QStringLiteral("series_cross_scale_line_join_tolerance_m")},5.0);
    rules.lineJoinAngleMax=number({QStringLiteral("series_cross_scale_line_join_angle_max")},75.0);
    const QString aggregation=crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_polygon_aggregation_allowed")},QStringLiteral("false"),scale).toLower();
    rules.polygonAggregationAllowed=aggregation==QStringLiteral("true") || aggregation==QStringLiteral("1") || aggregation==QStringLiteral("是");
    if(!QStringList{QStringLiteral("true"),QStringLiteral("false"),QStringLiteral("1"),QStringLiteral("0"),QStringLiteral("是"),QStringLiteral("否")}.contains(aggregation))
        errors << QStringLiteral("series_cross_scale_polygon_aggregation_allowed");
    QString semanticClassRules=crossScaleRuleValue(xmlPath,
        {QStringLiteral("series_cross_scale_semantic_classes"),QStringLiteral("跨比例尺语义类别")},QString(),scale);
    for (const QString& group:semanticClassRules.split(QRegularExpression(QStringLiteral("[;；\\n\\r]+")),Qt::SkipEmptyParts)) {
        const QStringList parts=group.split(QChar('='),Qt::KeepEmptyParts);
        if (parts.size()!=2 || parts.first().trimmed().isEmpty() || splitRuleList(parts.value(1)).isEmpty())
            errors << QStringLiteral("series_cross_scale_semantic_classes");
    }
    const QString additionalSemanticClassRules=crossScaleRuleValue(xmlPath,
        {QStringLiteral("series_cross_scale_semantic_class_aliases_extra")},QString(),scale);
    if (!additionalSemanticClassRules.trimmed().isEmpty()) semanticClassRules+=QStringLiteral(";")+additionalSemanticClassRules;
    rules.semanticClassAliases=parseAliasText(semanticClassRules);
    if (!semanticClassRules.trimmed().isEmpty() && rules.semanticClassAliases.isEmpty())
        errors << QStringLiteral("series_cross_scale_semantic_classes");
    for (auto it=rules.semanticClassAliases.begin();it!=rules.semanticClassAliases.end();++it)
        it.value()=CrossScaleSupport::key(it.value());
    const QSet<QString> canonicalClasses(rules.semanticClassAliases.cbegin(),rules.semanticClassAliases.cend());
    if(rules.toleranceModel==QStringLiteral("scale_pair_v1")) errors << CrossScaleTolerance::validate(rules.toleranceProfiles,canonicalClasses);
    for (const QString& canonicalClass:canonicalClasses)
        rules.semanticClassAliases.insert(canonicalClass,canonicalClass);
    rules.stableIdentityFields=splitRuleList(crossScaleRuleValue(xmlPath,
        {QStringLiteral("series_cross_scale_identity_fields"),QStringLiteral("稳定标识字段")},
        QStringLiteral("CODE|要素编码|NAME|名称|text_conte|text_content|text"),scale));
    auto classSet=[&](const QStringList& names) {
        QSet<QString> values;
        const QString raw=crossScaleRuleValue(xmlPath,names,QString(),scale);
        for (const QString& value:splitRuleList(raw)) values.insert(CrossScaleSupport::key(value));
        return values;
    };
    rules.lineAggregationClasses=classSet({QStringLiteral("series_cross_scale_line_aggregation_classes"),QStringLiteral("允许线要素组合支撑的类别")});
    rules.pointSymbolClasses=classSet({QStringLiteral("series_cross_scale_point_symbol_classes")});
    rules.polygonAggregationClasses=classSet({QStringLiteral("series_cross_scale_polygon_aggregation_classes"),QStringLiteral("允许面要素组合支撑的类别")});
    rules.strictMissingSupportClasses=classSet({QStringLiteral("series_cross_scale_strict_missing_support_classes"),QStringLiteral("严格缺失判错类别")});
    rules.mandatoryRetentionClasses=classSet({QStringLiteral("series_cross_scale_mandatory_retention_classes"),QStringLiteral("小比例尺强制保留类别")});
    QSet<QString> declaredClasses;
    for (const QString& value:rules.semanticClassAliases) declaredClasses.insert(value);
    auto validateClasses=[&](const QSet<QString>& values,const QString& ruleName) {
        for (const QString& value:values)
            if (!declaredClasses.contains(value)) errors << ruleName;
    };
    validateClasses(rules.lineAggregationClasses,QStringLiteral("series_cross_scale_line_aggregation_classes"));
    validateClasses(rules.pointSymbolClasses,QStringLiteral("series_cross_scale_point_symbol_classes"));
    validateClasses(rules.polygonAggregationClasses,QStringLiteral("series_cross_scale_polygon_aggregation_classes"));
    validateClasses(rules.strictMissingSupportClasses,QStringLiteral("series_cross_scale_strict_missing_support_classes"));
    validateClasses(rules.mandatoryRetentionClasses,QStringLiteral("series_cross_scale_mandatory_retention_classes"));
    if (rules.polygonAggregationAllowed) {
        errors << QStringLiteral("请按语义类别配置 series_cross_scale_polygon_aggregation_classes，不再接受全局面聚合开关");
        rules.polygonAggregationAllowed=false;
    }
    const QStringList allowedTransitions{QStringLiteral("point->line"),QStringLiteral("point->polygon"),
        QStringLiteral("line->point"),QStringLiteral("line->polygon"),QStringLiteral("polygon->point"),QStringLiteral("polygon->line")};
    for (QString transition:splitRuleList(crossScaleRuleValue(xmlPath,
            {QStringLiteral("series_cross_scale_geometry_transitions"),QStringLiteral("允许几何类型转换")},QString(),scale))) {
        transition.remove(QRegularExpression(QStringLiteral("\\s+")));
        transition=transition.toLower();
        if (!allowedTransitions.contains(transition)) errors << QStringLiteral("series_cross_scale_geometry_transitions");
        else rules.geometryTransitions.insert(transition);
    }
    rules.outputSeverity=crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_output_severity"),QStringLiteral("错误输出等级")},rules.outputSeverity,scale);
    if(rules.matchedThreshold<=0 || rules.matchedThreshold>1
        || rules.spaceToleranceMm<0 || rules.candidateSearchToleranceMm<rules.spaceToleranceMm
        || rules.geomToleranceM<0 || rules.annotationAnchorToleranceMm<0
        || rules.lineJoinToleranceM<0 || rules.lineJoinAngleMax<0 || rules.lineJoinAngleMax>180
        || rules.uniqueScoreMargin<0 || rules.uniqueScoreMargin>1
        || (!crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_max_uncovered_segment_length_m")},QString(),scale).isEmpty()
            && rules.maxUncoveredSegmentLength<0)
        || (!crossScaleRuleValue(xmlPath,{QStringLiteral("series_cross_scale_max_uncovered_segment_length_mm")},QString(),scale).isEmpty()
            && rules.maxUncoveredSegmentLengthMm<0)
        || rules.spatialWeight<0 || rules.geometryWeight<0 || rules.attributeWeight<0)
        errors << QStringLiteral("覆盖率阈值须在 0 到 1 之间，疑似阈值不得超过匹配阈值，容差及权重须为非负数");
    if(rules.toleranceModel==QStringLiteral("scale_pair_v1") && (rules.directionAngleMax<0 || rules.directionAngleMax>=90))
        errors << QStringLiteral("series_cross_scale_direction_angle_max must be >=0 and <90 for scale_pair_v1");
    rules.valid=errors.isEmpty();
    if(!rules.valid) rules.errorMessage=QStringLiteral("XML 缺少有效的跨比例尺判定参数：%1").arg(errors.join(QStringLiteral("，")));
    return rules;
}

QSet<QString> SeriesCrossScaleExpressionTaskDialog::readExcludedLayerKeys(const QString& xmlPath, int scale) const
{
    QStringList excludes = splitRuleList(crossScaleRuleValue(xmlPath, QStringList()
        << QStringLiteral("series_cross_scale_excluded_layers")
        << QStringLiteral("排除图层"), QString(), scale));
    excludes << QStringLiteral("内图廓") << QStringLiteral("图廓") << QStringLiteral("接图表") << QStringLiteral("公里网")
             << QStringLiteral("注记辅助线") << QStringLiteral("晕带") << QStringLiteral("图例")
             << QStringLiteral("指北针") << QStringLiteral("比例尺") << QStringLiteral("标题");
    excludes << QStringLiteral("\u5916\u533a\u666e\u8272");
    return normalizedLayerKeySet(excludes);
}

QMap<QString, QString> SeriesCrossScaleExpressionTaskDialog::readLayerAliases(const QString& xmlPath, int scale) const
{
    const QString text = crossScaleRuleValue(xmlPath, QStringList()
        << QStringLiteral("series_layer_name_aliases") << QStringLiteral("同类要素名称扩展")
        << QStringLiteral("图层名称别名"), QString(), scale);
    return parseAliasText(text);
}

QJsonObject SeriesCrossScaleExpressionTaskDialog::buildActualShpLayerMappingForTask(const MapTask& task, const QMap<QString, QString>& aliases, const QSet<QString>& excludedLayerKeys) const
{
    QJsonObject root;
    root[QStringLiteral("pair_name")] = task.pairName;
    root[QStringLiteral("source_audits")] = task.sourceAudits;
    QJsonObject layers;
    const QMap<QString, QStringList> smallGroups = groupActualShpFiles(collectShpFiles(task.smallShpDir), aliases, excludedLayerKeys);
    const QMap<QString, QStringList> largeGroups = groupActualShpFiles(collectShpFiles(task.largeShpDir), aliases, excludedLayerKeys);
    QSet<QString> keys;
    for (const QString& key : smallGroups.keys()) keys.insert(key);
    for (const QString& key : largeGroups.keys()) keys.insert(key);

    QStringList sortedKeys = keys.values();
    sortedKeys.sort();
    for (const QString& target : sortedKeys) {
        const QStringList largePaths = largeGroups.value(target);
        const QStringList smallPaths = smallGroups.value(target);
        QJsonObject layer;
        layer[QStringLiteral("large")] = stringListToJson(largePaths);
        layer[QStringLiteral("small")] = stringListToJson(smallPaths);
        layer[QStringLiteral("left")] = stringListToJson(largePaths);
        layer[QStringLiteral("right")] = stringListToJson(smallPaths);
        if (smallPaths.isEmpty()) {
            layer[QStringLiteral("status")] = QStringLiteral("large_only");
        } else if (largePaths.isEmpty()) {
            layer[QStringLiteral("status")] = QStringLiteral("missing_large_scale_layer");
        } else {
            layer[QStringLiteral("status")] = QStringLiteral("auto_matched_actual_shp");
        }
        layers[target] = layer;
    }
    root[QStringLiteral("layers")] = layers;
    return root;
}

bool SeriesCrossScaleExpressionTaskDialog::confirmLayerMappings(const QVector<MapTask>& tasks, QMap<QString, QJsonObject>* confirmedLayerMappings)
{
    if (!confirmedLayerMappings) return false;
    confirmedLayerMappings->clear();

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("跨比例尺图层对应确认"));
    dlg.resize(980, 640);
    auto* layout = new QVBoxLayout(&dlg);
    auto* tree = new QTreeWidget(&dlg);
    tree->setColumnCount(4);
    tree->setHeaderLabels({
        QStringLiteral("检查图层"),
        QStringLiteral("大比例尺实际图层"),
        QStringLiteral("小比例尺实际图层"),
        QStringLiteral("状态")
    });
    tree->setRootIsDecorated(true);
    tree->setItemsExpandable(true);
    tree->setAllColumnsShowFocus(true);
    tree->header()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(tree, 1);

    auto collectLayerCandidates = [&](const QString& shpRootDir, const QMap<QString,QString>& aliases, const QSet<QString>& excludedLayerKeys) {
        QStringList candidates;
        for (const QString& path : collectShpFiles(shpRootDir)) {
            const QString key = aliasCanonical(QFileInfo(path).fileName(), aliases);
            if (key.isEmpty() || excludedLayerKeys.contains(key)
                || excludedLayerKeys.contains(normalizeLayerName(QFileInfo(path).fileName())) || candidates.contains(path)) continue;
            candidates << path;
        }
        return candidates;
    };

    auto appendUniqueJsonString = [](QJsonArray* array, const QString& value) {
        if (!array || value.isEmpty()) return;
        for (const QJsonValue& existing : *array) {
            if (existing.toString() == value) return;
        }
        array->append(value);
    };

    bool cellWidgetsReady = false;
    auto attachActualLayerSelectors = [&](QTreeWidgetItem* row) {
        auto makeLayerSelector = [&](const QStringList& candidates, const QString& selectedPath, const QString& objectName) {
            auto* selector = new LayerSelection::SearchableLayerComboBox(tree);
            selector->setObjectName(objectName);
            selector->setMaxVisibleItems(20);
            selector->addItem(QStringLiteral("（不对应）"), QString());
            for (const QString& path : candidates) {
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

        QTreeWidgetItem* taskItem = row;
        while (taskItem->parent()) taskItem = taskItem->parent();
        const QString largePath = row->data(0, Qt::UserRole + 3).toString();
        const QString smallPath = row->data(0, Qt::UserRole + 5).toString();
        const QStringList largeCandidates = taskItem->data(0, Qt::UserRole + 2).toStringList();
        const QStringList smallCandidates = taskItem->data(0, Qt::UserRole + 3).toStringList();
        auto* largeSelector = makeLayerSelector(largeCandidates, largePath, QStringLiteral("crossScaleLargeLayerSelector"));
        auto* smallSelector = makeLayerSelector(smallCandidates, smallPath, QStringLiteral("crossScaleSmallLayerSelector"));
        tree->setItemWidget(row, 1, largeSelector);
        tree->setItemWidget(row, 2, smallSelector);

        auto refreshRowMapping = [row, largeSelector, smallSelector]() {
            const QString selectedLargePath = largeSelector->currentData().toString();
            const QString selectedSmallPath = smallSelector->currentData().toString();
            QString selectedStatus;
            if (selectedLargePath.isEmpty() && selectedSmallPath.isEmpty()) {
                selectedStatus = QStringLiteral("unmapped");
            } else if (selectedLargePath.isEmpty()) {
                selectedStatus = QStringLiteral("missing_large_scale_layer");
            } else if (selectedSmallPath.isEmpty()) {
                selectedStatus = QStringLiteral("large_only");
            } else {
                const QString originalLargePath = row->data(0, Qt::UserRole + 6).toString();
                const QString originalSmallPath = row->data(0, Qt::UserRole + 7).toString();
                const bool unchanged = selectedLargePath.compare(originalLargePath, Qt::CaseInsensitive) == 0
                    && selectedSmallPath.compare(originalSmallPath, Qt::CaseInsensitive) == 0;
                selectedStatus = unchanged
                    ? QStringLiteral("auto_matched_actual_shp")
                    : QStringLiteral("manual_actual_shp");
            }
            const QString detailName = SeriesMappingTreeGroups::detailName(selectedLargePath, selectedSmallPath);
            row->setText(0, detailName.isEmpty() ? QStringLiteral("未选择图层") : detailName);
            row->setData(0, Qt::UserRole + 3, selectedLargePath);
            row->setData(0, Qt::UserRole + 4, selectedStatus);
            row->setData(0, Qt::UserRole + 5, selectedSmallPath);
            row->setToolTip(1, selectedLargePath);
            row->setToolTip(2, selectedSmallPath);
            largeSelector->setToolTip(selectedLargePath);
            smallSelector->setToolTip(selectedSmallPath);
            row->setText(3, displayCrossScaleLayerStatus(selectedStatus));
        };
        connect(largeSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), tree,
            [refreshRowMapping](int) { refreshRowMapping(); });
        connect(smallSelector, QOverload<int>::of(&QComboBox::currentIndexChanged), tree,
            [refreshRowMapping](int) { refreshRowMapping(); });
    };

    auto addActualLayerMappingRow = [&](QTreeWidgetItem* taskItem, const QString& target, const QString& largePath, const QString& smallPath) -> QTreeWidgetItem* {
        if (!taskItem) return nullptr;
        QString statusCode;
        if (largePath.isEmpty() && smallPath.isEmpty()) {
            statusCode = QStringLiteral("unmapped");
        } else if (largePath.isEmpty()) {
            statusCode = QStringLiteral("missing_large_scale_layer");
        } else if (smallPath.isEmpty()) {
            statusCode = QStringLiteral("large_only");
        } else {
            statusCode = QStringLiteral("auto_matched_actual_shp");
        }
        auto* row = new QTreeWidgetItem(taskItem);
        row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
        row->setCheckState(0, statusCode == QStringLiteral("auto_matched_actual_shp") ? Qt::Checked : Qt::Unchecked);
        row->setText(1, largePath.isEmpty() ? QString() : QFileInfo(largePath).fileName());
        row->setText(2, smallPath.isEmpty() ? QString() : QFileInfo(smallPath).fileName());
        row->setText(3, displayCrossScaleLayerStatus(statusCode));
        row->setToolTip(1, largePath);
        row->setToolTip(2, smallPath);
        row->setData(0, Qt::UserRole, QStringLiteral("MappingRow"));
        row->setData(0, Qt::UserRole + 1, taskItem->data(0, Qt::UserRole + 1).toString());
        row->setData(0, Qt::UserRole + 2, target);
        row->setData(0, Qt::UserRole + 3, largePath);
        row->setData(0, Qt::UserRole + 4, statusCode);
        row->setData(0, Qt::UserRole + 5, smallPath);
        row->setText(0, target);
        row->setData(0, Qt::UserRole + 6, largePath);
        row->setData(0, Qt::UserRole + 7, smallPath);

        if (cellWidgetsReady) attachActualLayerSelectors(row);
        return row;
    };

    auto addActualLayerRows = [&](QTreeWidgetItem* taskItem, const QString& target, const QStringList& largePaths,
                                  const QStringList& smallPaths, const QMap<QString,QString>& semanticAliases) {
        if (!taskItem) return;
        QVector<int> matches(largePaths.size(),-1);
        QSet<int> used;
        // Reserve original names before normalized-name and semantic fallback.
        QMap<QString, QString> geometryKeys;
        auto matchKey = [&](const QString& path, int priority) -> QString {
            if (!geometryKeys.contains(path)) {
                QgsVectorLayer layer(path, QFileInfo(path).fileName(), QStringLiteral("ogr"));
                geometryKeys[path] = layer.isValid() ? QString::number(static_cast<int>(layer.geometryType())) : QString();
            }
            const QString geometryKey = geometryKeys.value(path);
            if (geometryKey.isEmpty()) return QString();
            const QString nameKey = priority == 0 ? QFileInfo(path).fileName().toCaseFolded()
                : priority == 1 ? normalizeLayerName(path)
                : CrossScaleSupport::businessClass(path, QgsFeature(), semanticAliases);
            return nameKey.isEmpty() ? QString() : nameKey + QChar(0x1f) + geometryKey;
        };
        auto matchUnique=[&](int priority) {
            QMap<QString,QVector<int>> leftGroups,rightGroups;
            for(int i=0;i<largePaths.size();++i) if(matches[i]<0) {
                const QString key=matchKey(largePaths[i],priority);
                if(!key.isEmpty()) leftGroups[key] << i;
            }
            for(int i=0;i<smallPaths.size();++i) if(!used.contains(i)) {
                const QString key=matchKey(smallPaths[i],priority);
                if(!key.isEmpty()) rightGroups[key] << i;
            }
            for(auto it=leftGroups.constBegin();it!=leftGroups.constEnd();++it)
                if(it.value().size()==1 && rightGroups.value(it.key()).size()==1) {
                    const int right=rightGroups.value(it.key()).first();matches[it.value().first()]=right;used.insert(right);
                }
        };
        matchUnique(0);matchUnique(1);matchUnique(2);
        for(int i=0;i<largePaths.size();++i)
            addActualLayerMappingRow(taskItem,target,largePaths[i],matches[i]<0?QString():smallPaths[matches[i]]);
        for(int i=0;i<smallPaths.size();++i) if(!used.contains(i)) addActualLayerMappingRow(taskItem,target,QString(),smallPaths[i]);
    };

    for (const MapTask& task : tasks) {
        const auto aliases=readLayerAliases(m_xmlEdit->text().trimmed(),task.smallScale);
        const auto excludedLayerKeys=readExcludedLayerKeys(m_xmlEdit->text().trimmed(),task.smallScale);
        const auto semanticAliases=readCrossScaleRules(m_xmlEdit->text().trimmed(),task.smallScale).semanticClassAliases;
        QJsonObject mapping = buildActualShpLayerMappingForTask(task, aliases, excludedLayerKeys);

        auto* taskItem = new QTreeWidgetItem(tree);
        taskItem->setText(0, task.pairName);
        taskItem->setData(0, Qt::UserRole, QStringLiteral("Task"));
        taskItem->setData(0, Qt::UserRole + 1, task.pairName);
        taskItem->setData(0, Qt::UserRole + 2, collectLayerCandidates(task.largeShpDir,aliases,excludedLayerKeys));
        taskItem->setData(0, Qt::UserRole + 3, collectLayerCandidates(task.smallShpDir,aliases,excludedLayerKeys));
        tree->setFirstItemColumnSpanned(taskItem, true);

        const QJsonObject layers = mapping.value(QStringLiteral("layers")).toObject();
        for (const QString& target : layers.keys()) {
            const QJsonObject layer = layers.value(target).toObject();
            QStringList largePaths;
            for (const QJsonValue& v : layer.value(QStringLiteral("large")).toArray()) {
                largePaths << v.toString();
            }
            QStringList smallPaths;
            for (const QJsonValue& v : layer.value(QStringLiteral("small")).toArray()) {
                smallPaths << v.toString();
            }

            addActualLayerRows(taskItem, target, largePaths, smallPaths, semanticAliases);
        }
        taskItem->setExpanded(true);
    }

    for (int taskIndex = 0; taskIndex < tree->topLevelItemCount(); ++taskIndex) {
        QTreeWidgetItem* task = tree->topLevelItem(taskIndex);
        SeriesMappingTreeGroups::groupTask(task, {}, 0, 1, 2, 3, Qt::UserRole + 3, Qt::UserRole + 5);
        for (QTreeWidgetItem* row : SeriesMappingTreeGroups::mappingRows(task)) attachActualLayerSelectors(row);
    }
    cellWidgetsReady = true;

    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);

    auto* mappingToolbar = new QHBoxLayout();
    auto* addMappingRowButton = new QPushButton(QStringLiteral("新增对应行"), &dlg);
    addMappingRowButton->setObjectName(QStringLiteral("addCrossScaleMappingRowButton"));
    auto* mappingHint = new QLabel(QStringLiteral("展开下拉框后可输入文字筛选两侧图层；勾选的行才参与检查。"), &dlg);
    mappingToolbar->addWidget(addMappingRowButton);
    mappingToolbar->addWidget(mappingHint);
    mappingToolbar->addStretch();
    layout->addLayout(mappingToolbar);
    connect(addMappingRowButton, &QPushButton::clicked, &dlg, [&]() {
        QTreeWidgetItem* selected = tree->currentItem();
        if (!selected || selected->data(0, Qt::UserRole).toString() != QStringLiteral("MappingRow") || !selected->parent()) {
            QMessageBox::information(&dlg, QStringLiteral("提示"), QStringLiteral("请先选择一行对应关系，再新增对应行。"));
            return;
        }
        QTreeWidgetItem* taskItem = selected->parent();
        const QString target = selected->data(0, Qt::UserRole + 2).toString();
        QTreeWidgetItem* newRow = addActualLayerMappingRow(taskItem, target, QString(), QString());
        if (newRow) {
            newRow->setCheckState(0, Qt::Unchecked);
            tree->setCurrentItem(newRow);
        }
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return false;

    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* taskItem = tree->topLevelItem(i);
        if (!taskItem) continue;
        const QString pairName = taskItem->data(0, Qt::UserRole + 1).toString();
        QJsonObject mapping;
        mapping[QStringLiteral("pair_name")] = pairName;
        for(const MapTask& task:tasks) if(task.pairName==pairName) {mapping[QStringLiteral("source_audits")]=task.sourceAudits;break;}
        QJsonObject layers;
        for (QTreeWidgetItem* row : SeriesMappingTreeGroups::mappingRows(taskItem)) {
            if (row->checkState(0) != Qt::Checked) continue;
            const QString target = row->data(0, Qt::UserRole + 2).toString();
            if (target.isEmpty()) continue;
            const QString largePath = row->data(0, Qt::UserRole + 3).toString();
            const QString smallPath = row->data(0, Qt::UserRole + 5).toString();
            if (largePath.isEmpty() && smallPath.isEmpty()) continue;
            QJsonObject layer = layers.value(target).toObject();
            QJsonArray largeArray = layer.value(QStringLiteral("large")).toArray();
            QJsonArray smallArray = layer.value(QStringLiteral("small")).toArray();
            appendUniqueJsonString(&largeArray, largePath);
            appendUniqueJsonString(&smallArray, smallPath);
            layer[QStringLiteral("large")] = largeArray;
            layer[QStringLiteral("small")] = smallArray;
            layer[QStringLiteral("left")] = largeArray;
            layer[QStringLiteral("right")] = smallArray;
            SeriesLayerMapping::appendRow(layer,
                largePath.isEmpty() ? QStringList() : QStringList{largePath},
                smallPath.isEmpty() ? QStringList() : QStringList{smallPath});
            if (smallArray.isEmpty()) {
                layer[QStringLiteral("status")] = QStringLiteral("large_only");
            } else if (largeArray.isEmpty()) {
                layer[QStringLiteral("status")] = QStringLiteral("missing_large_scale_layer");
            } else {
                layer[QStringLiteral("status")] = QStringLiteral("auto_matched_actual_shp");
            }
            layers[target] = layer;
        }
        mapping[QStringLiteral("layers")] = layers;
        confirmedLayerMappings->insert(pairName, mapping);
    }
    return true;
}

QJsonObject SeriesCrossScaleExpressionTaskDialog::runLayerCorrespondenceCheck(const MapTask& task, const QString& target, const QStringList& smallPaths, const QStringList& largePaths, const CrossScaleRules& rules, const QgsGeometry& largeCoverage, const QJsonObject& confirmedLayer, const QString& checkCrsWkt) const
{
    QJsonObject summary{{QStringLiteral("target_layer"),target},
        {QStringLiteral("large_paths"),stringListToJson(largePaths)},
        {QStringLiteral("small_paths"),stringListToJson(smallPaths)}};
    summary[QStringLiteral("confirmed_layer_mapping")]=confirmedLayer;
    summary[QStringLiteral("support_coverage_threshold")]=rules.matchedThreshold;
    QJsonArray warnings, evidenceRecords;
    int matchedCount=0, suspectCount=0, unmatchedCount=0, skippedCount=0, outOfRangeCount=0, annotationGraphicSkippedCount=0,
        groupMatchedCount=0, unclassifiedCount=0;
    const bool confirmedTargetMapping=!confirmedLayer.isEmpty();
    const auto resolveBusinessClass=[&](const QString& path,const QgsFeature& feature) {
        return CrossScaleSupport::businessClassForConfirmedTarget(path,feature,target,
            rules.semanticClassAliases,confirmedTargetMapping);
    };
    auto finish = [&](const QString& state) {
        summary[QStringLiteral("execution_state")]=state;
        summary[QStringLiteral("status")]=CrossScaleSupport::executionStatus(state);
        summary[QStringLiteral("matched_count")]=matchedCount;
        summary[QStringLiteral("suspect_count")]=suspectCount;
        summary[QStringLiteral("unmatched_count")]=unmatchedCount;
        summary[QStringLiteral("out_of_range_count")]=outOfRangeCount;
        summary[QStringLiteral("group_matched_count")]=groupMatchedCount;
        summary[QStringLiteral("coverage_skipped_count")]=skippedCount;
        summary[QStringLiteral("annotation_graphic_skipped_count")]=annotationGraphicSkippedCount;
        summary[QStringLiteral("checked_feature_count")]=evidenceRecords.size();
        summary[QStringLiteral("support_evidence_records")]=evidenceRecords;
        summary[QStringLiteral("warning_count")]=warnings.size();
        summary[QStringLiteral("warnings")]=warnings;
        return summary;
    };
    auto fail = [&](const QString& state,const QString& message) {
        warnings.append(QJsonObject{{QStringLiteral("pair_name"),task.pairName},
            {QStringLiteral("target_layer"),target},{QStringLiteral("error_type"),QStringLiteral("cross_scale_not_evaluable")},
            {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},{QStringLiteral("description"),message}});
        return finish(state);
    };
    if (largeCoverage.isNull() || largeCoverage.isEmpty() || largeCoverage.type()!=QgsWkbTypes::PolygonGeometry
        || !largeCoverage.isGeosValid() || largeCoverage.area()<=1e-8)
        return fail(QStringLiteral("invalid_scope"),QStringLiteral("共同覆盖范围不是有效面，未执行要素支撑判定。"));
    if (!rules.valid || !std::isfinite(rules.matchedThreshold) || rules.matchedThreshold<=0 || rules.matchedThreshold>1
        || !std::isfinite(rules.geomToleranceM) || rules.geomToleranceM<0
        || !std::isfinite(rules.spaceToleranceMm) || rules.spaceToleranceMm<0)
        return fail(QStringLiteral("invalid_config"),QStringLiteral("覆盖率阈值须在 0 到 1 之间，容差须为非负有限数。"));
    if(rules.toleranceModel==QStringLiteral("scale_pair_v1") && (task.smallScale<=0 || task.largeScale<=0 || task.smallScale<=task.largeScale))
        return fail(QStringLiteral("invalid_config"),QStringLiteral("尺度对容差需要有效且不同的两侧比例尺，且小比例尺分母必须较大。"));
    if (smallPaths.isEmpty()) return finish(QStringLiteral("no_applicable_features"));
    if (largePaths.isEmpty())
        return fail(QStringLiteral("invalid_input"),QStringLiteral("已选的小比例尺图层缺少对应的大比例尺图层。"));

    QgsCoordinateReferenceSystem checkCrs(checkCrsWkt);
    if (!checkCrs.isValid()) {
        QgsVectorLayer layer(smallPaths.first(),QStringLiteral("check_crs"),QStringLiteral("ogr"));
        checkCrs=layer.crs();
    }
    if (!checkCrs.isValid() || checkCrs.isGeographic() || checkCrs.mapUnits()!=QgsUnitTypes::DistanceMeters)
        return fail(QStringLiteral("invalid_input"),QStringLiteral("缺少有效的米制检查坐标系。"));
    struct Ref {
        QString path;
        QgsFeature feature;
        QString identity;
        QString businessClass;
        QStringList sourceFids;
        QgsGeometry sourceGeometry;
        QString logicalModel;
    };
    QVector<Ref> largeFeatures,smallFeatures;
    QString inputError;
    bool featureInputIssues=false;
    QSet<QString> invalidLargeClasses,invalidSmallClasses;
    auto recordInvalidFeature=[&](const QString& path,const QgsFeature& feature,bool isSmall,const QString& reason) {
        featureInputIssues=true;
        const QString businessClass=resolveBusinessClass(path,feature);
        (isSmall?invalidSmallClasses:invalidLargeClasses).insert(businessClass);
        QJsonObject record{{QStringLiteral("pair_name"),task.pairName},{QStringLiteral("target_layer"),target},
            {QStringLiteral("error_type"),QStringLiteral("cross_scale_feature_not_evaluable")},
            {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},
            {QStringLiteral("description"),QStringLiteral("单个要素的几何无效、裁剪失败或坐标转换失败，已跳过该要素并继续检查同层其他要素：%1 / FID %2。原因：%3")
                .arg(path).arg(feature.id()).arg(reason)}};
        if (isSmall) {
            record[QStringLiteral("small_shp")]=path;
            record[QStringLiteral("small_feature_id")]=static_cast<double>(feature.id());
        } else {
            record[QStringLiteral("best_large_shp")]=path;
            record[QStringLiteral("best_large_feature_id")]=static_cast<double>(feature.id());
        }
        warnings.append(record);
        evidenceRecords.append(record);
    };
    auto load = [&](const QStringList& paths,QVector<Ref>& refs,bool isSmall) {
        QSet<QString> seen;
        int count=0;
        for (const QString& path:paths) {
            if (seen.contains(SeriesLayerMapping::pathKey(path))) continue;
            seen.insert(SeriesLayerMapping::pathKey(path));
            QgsVectorLayer layer(path,QFileInfo(path).completeBaseName(),QStringLiteral("ogr"));
            if (!layer.isValid() || !layer.crs().isValid()) { inputError=QStringLiteral("已选图层不可读或缺少坐标系：%1").arg(path); return; }
            QgsFeature feature;
            auto it=layer.getFeatures();
            while (it.nextFeature(feature)) {
                ++count;
                if (CrossScaleSupport::isAnnotationPath(path)
                    && CrossScaleSupport::isNonTextAnnotationGraphic(feature)) {
                    ++annotationGraphicSkippedCount;
                    if (isSmall) ++skippedCount;
                    continue;
                }
                QgsGeometry geometry=feature.geometry();
                if (geometry.isNull() || geometry.isEmpty()) { if(isSmall) ++skippedCount; continue; }
                if (!geometry.isGeosValid()) {
                    recordInvalidFeature(path,feature,isSmall,QStringLiteral("几何自相交或无效"));
                    continue;
                }
                if (!CrossScaleSupport::project(geometry,layer.crs(),checkCrs)) {
                    recordInvalidFeature(path,feature,isSmall,QStringLiteral("坐标转换失败"));
                    continue;
                }
                feature.setGeometry(geometry);
                const QString businessClass=resolveBusinessClass(path,feature);
                const bool pointSymbol=rules.pointSymbolClasses.contains(CrossScaleSupport::key(businessClass));
                Ref ref;
                ref.path=path;
                ref.feature=feature;
                ref.identity=CrossScaleSupport::identity(feature,rules.stableIdentityFields);
                ref.businessClass=businessClass;
                ref.sourceFids << QString::number(feature.id());
                ref.sourceGeometry=geometry;
                ref.logicalModel=pointSymbol?QStringLiteral("point_anchor"):QStringLiteral("source_geometry");
                refs.push_back(ref);
            }
        }
        summary[isSmall?QStringLiteral("small_feature_count"):QStringLiteral("large_feature_count")]=count;
    };
    load(largePaths,largeFeatures,false);
    if (inputError.isEmpty()) load(smallPaths,smallFeatures,true);
    if (!inputError.isEmpty()) return fail(QStringLiteral("invalid_input"),inputError);

    const auto finalizeLogicalEntities=[&](QVector<Ref>& rawRefs,bool isSmall) {
        QVector<Ref> logicalRefs;
        QMap<QString,QVector<int>> symbolGroups;
        for(int i=0;i<rawRefs.size();++i) {
            const Ref& ref=rawRefs.at(i);
            if (!rules.pointSymbolClasses.contains(CrossScaleSupport::key(ref.businessClass))) continue;
            const QString instance=CrossScaleSupport::symbolInstanceKey(ref.feature);
            if (instance.isEmpty()) continue;
            const QString groupKey=SeriesLayerMapping::pathKey(ref.path)+QChar('|')
                +CrossScaleSupport::key(ref.businessClass)+QChar('|')+instance;
            symbolGroups[groupKey] << i;
        }
        QVector<bool> consumed(rawRefs.size(),false);
        for(int i=0;i<rawRefs.size();++i) {
            if(consumed.at(i)) continue;
            const Ref& first=rawRefs.at(i);
            QVector<int> members{i};
            bool hasSymbolInstanceKey=false;
            if(rules.pointSymbolClasses.contains(CrossScaleSupport::key(first.businessClass))) {
                const QString instance=CrossScaleSupport::symbolInstanceKey(first.feature);
                if(!instance.isEmpty()) {
                    hasSymbolInstanceKey=true;
                    const QString groupKey=SeriesLayerMapping::pathKey(first.path)+QChar('|')
                        +CrossScaleSupport::key(first.businessClass)+QChar('|')+instance;
                    members=symbolGroups.value(groupKey,QVector<int>{i});
                }
            }
            for(int member:members) consumed[member]=true;
            bool visible=false;
            QgsRectangle extent;
            bool haveExtent=false;
            QStringList sourceFids;
            for(int member:members) {
                const Ref& part=rawRefs.at(member);
                const QgsRectangle partExtent=part.sourceGeometry.boundingBox();
                if(!haveExtent) { extent=partExtent; haveExtent=true; }
                else extent.combineExtentWith(partExtent);
                sourceFids << part.sourceFids;
                const QgsGeometry clipped=CrossScaleSupport::clipped(part.sourceGeometry,largeCoverage);
                if(!clipped.lastError().isEmpty()) {
                    recordInvalidFeature(part.path,part.feature,isSmall,clipped.lastError());
                    continue;
                }
                if(clipped.isNull() || clipped.isEmpty()) continue;
                visible=true;
            }
            if(!visible || !haveExtent) {
                if(isSmall) ++skippedCount;
                continue;
            }
            Ref logical=first;
            logical.sourceFids=sourceFids;
            if(rules.pointSymbolClasses.contains(CrossScaleSupport::key(first.businessClass))) {
                const QgsPointXY anchor=extent.center();
                logical.feature.setGeometry(QgsGeometry::fromPointXY(anchor));
                logical.sourceGeometry=logical.feature.geometry();
                logical.logicalModel=hasSymbolInstanceKey?QStringLiteral("symbol_instance_anchor")
                    :QStringLiteral("point_anchor_unmerged");
            } else {
                const QgsGeometry clipped=CrossScaleSupport::clipped(first.sourceGeometry,largeCoverage);
                if(clipped.isNull() || clipped.isEmpty() || !clipped.lastError().isEmpty()) {
                    if(isSmall) ++skippedCount;
                    continue;
                }
                logical.feature.setGeometry(clipped);
                logical.sourceGeometry=clipped;
            }
            logicalRefs.push_back(logical);
        }
        rawRefs.swap(logicalRefs);
        summary[isSmall?QStringLiteral("small_logical_entity_count"):QStringLiteral("large_logical_entity_count")]=rawRefs.size();
    };
    finalizeLogicalEntities(largeFeatures,false);
    finalizeLogicalEntities(smallFeatures,true);
    QMap<QString,int> smallIdentityCounts;
    for (const Ref& ref:smallFeatures) if (!ref.identity.isEmpty()) ++smallIdentityCounts[ref.identity];
    QMap<QString,int> largeIdentityCounts;
    for (const Ref& ref:largeFeatures) if (!ref.identity.isEmpty()) ++largeIdentityCounts[ref.identity];
    QMap<QString,QVector<int>> largeIdentityIndexes,largeNameIndexes;
    QVector<QStringList> largeFeatureNameKeys(largeFeatures.size());
    for (int i=0;i<largeFeatures.size();++i) {
        const Ref& ref=largeFeatures.at(i);
        if (CrossScaleSupport::isAnnotationPath(ref.path)) continue;
        if (!ref.identity.isEmpty()) largeIdentityIndexes[ref.identity] << i;
        if (ref.businessClass.isEmpty()) continue;
        largeFeatureNameKeys[i]=CrossScaleSupport::featureNameKeys(ref.feature);
        for (const QString& nameKey:largeFeatureNameKeys.at(i)) {
            const QString indexKey=CrossScaleSupport::key(ref.businessClass)+QChar(0x1f)+nameKey;
            largeNameIndexes[indexKey] << i;
        }
    }
    QMap<QString,int> smallAnnotationTextCounts;
    for (const Ref& ref:smallFeatures) {
        if (!CrossScaleSupport::isAnnotationPath(ref.path)) continue;
        const QString textKey=CrossScaleSupport::normalizeAnnotationText(geoPdfAnnotationText(ref.feature));
        if (!textKey.isEmpty()) ++smallAnnotationTextCounts[textKey];
    }
    QgsSpatialIndex index;
    for(int i=0;i<largeFeatures.size();++i) {
        QgsFeature indexed; indexed.setId(i); indexed.setGeometry(largeFeatures[i].feature.geometry()); index.addFeature(indexed);
    }
    QgsSpatialIndex smallIndex;
    for(int i=0;i<smallFeatures.size();++i) {
        QgsFeature indexed; indexed.setId(i); indexed.setGeometry(smallFeatures[i].feature.geometry()); smallIndex.addFeature(indexed);
    }
    const auto toleranceForClass=[&](const QString& businessClass) {
        return CrossScaleTolerance::resolve(rules.toleranceModel,rules.toleranceProfiles,businessClass,task.smallScale,task.largeScale,
            rules.spaceToleranceMm,rules.geomToleranceM,rules.candidateSearchToleranceMm,rules.maxUncoveredSegmentLengthMm,rules.maxUncoveredSegmentLength);
    };
    const auto defaultTolerance=toleranceForClass(QString());
    auto finiteBudget=[](const CrossScaleTolerance::Budget& budget) {
        return std::isfinite(budget.position) && std::isfinite(budget.support) && std::isfinite(budget.search)
            && std::isfinite(budget.expanded) && std::isfinite(budget.gap);
    };
    if(!finiteBudget(defaultTolerance)) return fail(QStringLiteral("invalid_config"),QStringLiteral("容差换算溢出，不能执行几何计算。"));
    for(const QString& businessClass:rules.toleranceProfiles.keys())
        if(!finiteBudget(toleranceForClass(businessClass))) return fail(QStringLiteral("invalid_config"),QStringLiteral("类别容差换算溢出，不能执行几何计算。"));
    const double annotationTolerance=qMax(0.001,rules.annotationAnchorToleranceMm*task.smallScale/1000.0);
    summary[QStringLiteral("candidate_search_radius")]=defaultTolerance.search;
    summary[QStringLiteral("expanded_candidate_search_radius")]=defaultTolerance.expanded;
    summary[QStringLiteral("position_acceptance_tolerance")]=defaultTolerance.position;
    summary[QStringLiteral("support_tolerance")]=defaultTolerance.support;
    summary[QStringLiteral("tolerance_model")]=rules.toleranceModel;
    summary[QStringLiteral("default_tolerance_budget")]=defaultTolerance.evidence;
    summary[QStringLiteral("tolerance_values_scope")]=QStringLiteral("default_only_actual_values_recorded_per_feature");
    for (const Ref& smallRef:smallFeatures) {
        const QgsGeometry smallGeom=smallRef.feature.geometry();
        const QString semanticClass=CrossScaleSupport::key(smallRef.businessClass);
        const auto tolerance=toleranceForClass(semanticClass);
        const double positionTolerance=tolerance.position,supportTolerance=tolerance.support,
            searchRadius=tolerance.search,expandedSearchRadius=tolerance.expanded,maxGap=tolerance.gap;
        const bool lineAggregationAllowed=smallGeom.type()==QgsWkbTypes::LineGeometry
            && rules.lineAggregationClasses.contains(semanticClass);
        const bool polygonAggregationAllowed=smallGeom.type()==QgsWkbTypes::PolygonGeometry
            && (rules.polygonAggregationAllowed || rules.polygonAggregationClasses.contains(semanticClass));
        const bool annotation=CrossScaleSupport::isAnnotationPath(smallRef.path);
        if (annotation) {
            QVector<CrossScaleSupport::AnnotationCandidate> annotationCandidates;
            for (const Ref& largeRef:largeFeatures) {
                if (!CrossScaleSupport::isAnnotationPath(largeRef.path)) continue;
                if (!confirmedLayer.isEmpty()
                    && !SeriesLayerMapping::allowsPair(confirmedLayer,largeRef.path,smallRef.path)) continue;
                annotationCandidates.push_back({geoPdfAnnotationText(largeRef.feature),largeRef.feature.geometry(),
                    largeRef.path,largeRef.sourceFids});
            }
            const QString text=geoPdfAnnotationText(smallRef.feature);
            const bool smallTextUnique=smallAnnotationTextCounts.value(CrossScaleSupport::normalizeAnnotationText(text))==1;
            auto relation=CrossScaleSupport::resolveAnnotationCorrespondence(text,smallGeom,
                annotationCandidates,annotationTolerance,rules.uniqueScoreMargin,smallTextUnique);
            // A repeated name needs a spatially unique one-to-one candidate; one unique name can survive label displacement.
            if (!smallTextUnique && relation.state==CrossScaleSupport::AnnotationRelationState::Matched)
                relation.state=CrossScaleSupport::AnnotationRelationState::Ambiguous;
            int textVariantCandidateCount=0;
            int nearestTextVariantIndex=-1;
            double nearestTextVariantDistance=-1.0;
            const QString expectedTextCore=CrossScaleSupport::annotationNameCore(text);
            const QgsGeometry smallTextAnchor=smallGeom.centroid();
            if (!expectedTextCore.isEmpty() && !smallTextAnchor.isNull() && !smallTextAnchor.isEmpty()) {
                for (int i=0;i<annotationCandidates.size();++i) {
                    const QString candidateText=CrossScaleSupport::normalizeAnnotationText(annotationCandidates.at(i).text);
                    if (candidateText==CrossScaleSupport::normalizeAnnotationText(text)
                        || CrossScaleSupport::annotationNameCore(candidateText)!=expectedTextCore) continue;
                    ++textVariantCandidateCount;
                    const QgsGeometry candidateAnchor=annotationCandidates.at(i).geometry.centroid();
                    if (candidateAnchor.isNull() || candidateAnchor.isEmpty()) continue;
                    const double distance=smallTextAnchor.distance(candidateAnchor);
                    if (nearestTextVariantDistance<0 || distance<nearestTextVariantDistance) {
                        nearestTextVariantDistance=distance;
                        nearestTextVariantIndex=i;
                    }
                }
            }
            const CrossScaleSupport::AnnotationCandidate bestAnnotation=relation.bestCandidateIndex>=0
                && relation.bestCandidateIndex<annotationCandidates.size()
                ? annotationCandidates.at(relation.bestCandidateIndex):CrossScaleSupport::AnnotationCandidate();
            const CrossScaleSupport::AnnotationCandidate nearestTextAnnotation=relation.nearestTextCandidateIndex>=0
                && relation.nearestTextCandidateIndex<annotationCandidates.size()
                ? annotationCandidates.at(relation.nearestTextCandidateIndex):CrossScaleSupport::AnnotationCandidate();
            QJsonObject evidence{{QStringLiteral("pair_name"),task.pairName},
                {QStringLiteral("target_layer"),target},{QStringLiteral("small_shp"),smallRef.path},
                {QStringLiteral("small_feature_id"),static_cast<double>(smallRef.feature.id())},
                {QStringLiteral("small_source_fids"),stringListToJson(smallRef.sourceFids)},
                {QStringLiteral("small_logical_model"),smallRef.logicalModel},
                {QStringLiteral("annotation_best_large_shp"),bestAnnotation.path},
                {QStringLiteral("annotation_best_large_source_fids"),stringListToJson(bestAnnotation.sourceFids)},
                {QStringLiteral("parsed_text_identity"),CrossScaleSupport::normalizeAnnotationText(text)},
                {QStringLiteral("annotation_candidate_count"),relation.candidateCount},
                {QStringLiteral("annotation_spatial_candidate_count"),relation.spatialCandidateCount},
                {QStringLiteral("annotation_best_distance"),relation.bestDistance},
                {QStringLiteral("annotation_runner_up_distance"),relation.runnerUpDistance},
                {QStringLiteral("annotation_best_overlap_ratio"),relation.bestOverlapRatio},
                {QStringLiteral("annotation_best_relation_score"),relation.bestRelationScore},
                {QStringLiteral("annotation_runner_up_relation_score"),relation.runnerUpRelationScore},
                {QStringLiteral("annotation_tolerance"),annotationTolerance},
                {QStringLiteral("annotation_nearest_same_text_distance"),relation.nearestTextDistance},
                {QStringLiteral("annotation_nearest_same_text_shp"),nearestTextAnnotation.path},
                {QStringLiteral("annotation_nearest_same_text_source_fids"),stringListToJson(nearestTextAnnotation.sourceFids)},
                {QStringLiteral("annotation_match_mode"),relation.state!=CrossScaleSupport::AnnotationRelationState::Matched
                    ? QStringLiteral("unmatched"):(relation.matchedByUniqueText
                        ? QStringLiteral("unique_exact_text") : QStringLiteral("spatial_disambiguation"))},
                {QStringLiteral("annotation_unique_text_match"),relation.matchedByUniqueText},
                {QStringLiteral("annotation_small_text_unique"),smallTextUnique},
                {QStringLiteral("annotation_text_variant_candidate_count"),textVariantCandidateCount},
                {QStringLiteral("annotation_nearest_text_variant_distance"),nearestTextVariantDistance},
                {QStringLiteral("annotation_nearest_text_variant_shp"),nearestTextVariantIndex>=0
                    ? annotationCandidates.at(nearestTextVariantIndex).path:QString()},
                {QStringLiteral("annotation_nearest_text_variant_text"),nearestTextVariantIndex>=0
                    ? annotationCandidates.at(nearestTextVariantIndex).text:QString()},
                {QStringLiteral("annotation_nearest_text_variant_source_fids"),nearestTextVariantIndex>=0
                    ? stringListToJson(annotationCandidates.at(nearestTextVariantIndex).sourceFids):QJsonArray()},
                {QStringLiteral("checked_geometry_wkt"),smallGeom.asWkt(12)},
                {QStringLiteral("checked_geometry_crs"),checkCrs.toWkt()},
                {QStringLiteral("score_model"),QStringLiteral("exact_text_identity_with_spatial_tiebreak")},
                {QStringLiteral("style_comparison_applicable"),false}};
            evidenceRecords.append(evidence);
            if (relation.state==CrossScaleSupport::AnnotationRelationState::Matched) {
                ++matchedCount;
                continue;
            }
            QJsonObject warning=evidence;
            warning[QStringLiteral("severity")]=rules.outputSeverity;
            warning[QStringLiteral("best_large_is_error")]=false;
            if (relation.state==CrossScaleSupport::AnnotationRelationState::MissingText
                || relation.state==CrossScaleSupport::AnnotationRelationState::InvalidGeometry) {
                ++unclassifiedCount;
                warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_annotation_correspondence_not_evaluable");
                warning[QStringLiteral("decision_status")]=QStringLiteral("not_evaluable");
                warning[QStringLiteral("description")]=relation.state==CrossScaleSupport::AnnotationRelationState::MissingText
                    ? QStringLiteral("\u6807\u6ce8\u8981\u7d20\u7f3a\u5c11\u53ef\u7528\u6587\u672c\u5b57\u6bb5\uff0c\u65e0\u6cd5\u5efa\u7acb\u8de8\u6bd4\u4f8b\u5c3a\u6587\u672c\u5bf9\u5e94\u3002")
                    : QStringLiteral("\u6807\u6ce8\u6587\u672c\u6216\u5b9a\u4f4d\u51e0\u4f55\u65e0\u6548\uff0c\u65e0\u6cd5\u8bc4\u4f30\u6587\u672c\u4e0e\u4f4d\u7f6e\u7684\u5bf9\u5e94\u3002");
            } else {
                ++suspectCount;
                warning[QStringLiteral("decision_status")]=QStringLiteral("needs_review");
                warning[QStringLiteral("error_type")]=relation.state==CrossScaleSupport::AnnotationRelationState::Ambiguous
                    ? QStringLiteral("cross_scale_candidate_ambiguous")
                    : relation.state==CrossScaleSupport::AnnotationRelationState::NoTextMatch
                        ? QStringLiteral("cross_scale_annotation_text_no_match")
                        : QStringLiteral("cross_scale_annotation_spatial_no_match");
                warning[QStringLiteral("description")]=relation.state==CrossScaleSupport::AnnotationRelationState::Ambiguous
                    ? QStringLiteral("存在多个同文标注候选，按标注框重叠和锚点位置仍不能唯一确认对应。")
                    : relation.state==CrossScaleSupport::AnnotationRelationState::NoTextMatch
                        ? QStringLiteral("大比例尺对应标注中未找到规范化文本相同的对象；需复核名称变化、拆字或实际遗漏。")
                        : QStringLiteral("找到同文标注，但标注框未相交且锚点距离超出独立标注容差；需复核标注关系或排版移位。" );
            }
            if (relation.state==CrossScaleSupport::AnnotationRelationState::NoTextMatch
                && textVariantCandidateCount>0) {
                warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_annotation_text_variant");
                warning[QStringLiteral("description")]=QStringLiteral("\u672a\u627e\u5230\u5168\u540c\u6587\u672c\u6ce8\u8bb0\uff0c\u4f46\u53d1\u73b0\u53bb\u9664\u4e61\u3001\u9547\u3001\u6751\u3001\u53bf\u3001\u533a\u3001\u5e02\u7b49\u540e\u7f00\u540e\u540c\u540d\u7684\u6807\u6ce8\u5019\u9009\uff1b\u5df2\u8bb0\u5f55\u4f9b\u590d\u6838\uff0c\u4e0d\u81ea\u52a8\u5408\u5e76\u3002");
            }
            warnings.append(warning);
            continue;
        }
        if (annotation || smallRef.businessClass.isEmpty()) {
            ++unclassifiedCount;
            QJsonObject record{{QStringLiteral("pair_name"),task.pairName},{QStringLiteral("target_layer"),target},
                {QStringLiteral("small_shp"),smallRef.path},{QStringLiteral("small_feature_id"),static_cast<double>(smallRef.feature.id())},
                {QStringLiteral("error_type"),annotation?QStringLiteral("cross_scale_annotation_correspondence_not_evaluable"):
                    QStringLiteral("cross_scale_business_class_not_evaluable")},
                {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},
                {QStringLiteral("checked_geometry_wkt"),smallGeom.asWkt(12)},
                {QStringLiteral("description"),annotation
                    ? QStringLiteral("跨比例尺标注允许移位且文本可能拆字；缺少完整文本与所注地物的对应关系，不能用字框面积覆盖判断地物支撑。")
                    : QStringLiteral("业务类别无法识别，未执行同类要素支撑判定；不能将类别缺失解释为空间支撑缺失。")}};
            if(annotation) record[QStringLiteral("parsed_text_identity")]=smallRef.identity;
            warnings.append(record); evidenceRecords.append(record); continue;
        }
        QgsRectangle search=smallGeom.boundingBox(); search.grow(searchRadius);
        QMap<QString,QVector<CrossScaleCandidate>> groups;
        QVector<CrossScaleCandidate> unidentifiedCandidates;
        int rejectedCount=0, candidateCount=0, unclassifiedLargeCandidates=0, outsidePositionCount=0;
        int mappedSpatialHits=0, mappingRejectedCount=0, geometryRejectedCount=0,
            classRejectedCount=0, identityRejectedCount=0;
        int expandedOutsidePositionCount=0, outsideStableIdentityMatchCount=0;
        int outsideSemanticCandidateCount=0, outsideNameMatchCount=0;
        CrossScaleCandidate nearestOutsidePosition;
        const QList<QgsFeatureId> spatialIds=index.intersects(search);
        QList<QgsFeatureId> expandedIds;
        if(expandedSearchRadius>searchRadius) {
            QgsRectangle expandedSearch=smallGeom.boundingBox(); expandedSearch.grow(expandedSearchRadius);
            expandedIds=index.intersects(expandedSearch);
        }
        QVector<QgsFeatureId> ids;
        QSet<QgsFeatureId> seenCandidateIds;
        QSet<int> semanticExpandedCandidateIds;
        for (QgsFeatureId id:spatialIds) {
            if (seenCandidateIds.contains(id)) continue;
            seenCandidateIds.insert(id);
            ids << id;
        }
        int expandedSpatialAdded=0;
        for(QgsFeatureId id:expandedIds) {
            if(seenCandidateIds.contains(id)) continue;
            seenCandidateIds.insert(id); ids << id; ++expandedSpatialAdded;
        }
        if (!smallRef.identity.isEmpty()) {
            for (int indexId:largeIdentityIndexes.value(smallRef.identity)) {
                if (seenCandidateIds.contains(indexId)) continue;
                seenCandidateIds.insert(indexId);
                semanticExpandedCandidateIds.insert(indexId);
                ids << indexId;
            }
        }
        // Stable IDs and exact business names recall semantic candidates omitted by the spatial window.
        const QStringList smallNameKeys=CrossScaleSupport::featureNameKeys(smallRef.feature);
        for (const QString& nameKey:smallNameKeys) {
            const QString nameIndexKey=semanticClass+QChar(0x1f)+nameKey;
            for (int indexId:largeNameIndexes.value(nameIndexKey)) {
                if (seenCandidateIds.contains(indexId)) continue;
                seenCandidateIds.insert(indexId);
                semanticExpandedCandidateIds.insert(indexId);
                ids << indexId;
            }
        }
        for (QgsFeatureId id:ids) {
            const Ref& largeRef=largeFeatures[static_cast<int>(id)];
            if (!confirmedLayer.isEmpty() && !SeriesLayerMapping::allowsPair(confirmedLayer,largeRef.path,smallRef.path)) {
                ++mappingRejectedCount; continue;
            }
            const QgsGeometry geometry=largeRef.feature.geometry();
            const double candidateDistance=smallGeom.distance(geometry);
            if(candidateDistance>=0 && candidateDistance<=searchRadius) ++mappedSpatialHits;
            const QString transition=QStringLiteral("%1->%2")
                .arg(smallGeom.type()==QgsWkbTypes::PointGeometry?QStringLiteral("point"):
                     smallGeom.type()==QgsWkbTypes::LineGeometry?QStringLiteral("line"):QStringLiteral("polygon"))
                .arg(geometry.type()==QgsWkbTypes::PointGeometry?QStringLiteral("point"):
                     geometry.type()==QgsWkbTypes::LineGeometry?QStringLiteral("line"):QStringLiteral("polygon"));
            const bool sameGeometryType=geometry.type()==smallGeom.type();
            if (!sameGeometryType && !rules.geometryTransitions.contains(transition)) {
                ++rejectedCount; ++geometryRejectedCount; continue;
            }
            if ((largeRef.businessClass.isEmpty() || CrossScaleSupport::isAnnotationPath(largeRef.path)) && candidateDistance<=searchRadius) {
                ++unclassifiedLargeCandidates; ++rejectedCount; ++classRejectedCount; continue;
            }
            if (smallRef.businessClass.isEmpty() || largeRef.businessClass.isEmpty() || smallRef.businessClass!=largeRef.businessClass) {
                ++rejectedCount; ++classRejectedCount; continue;
            }
            if (CrossScaleSupport::identitiesConflict(smallRef.identity,largeRef.identity)) {
                ++rejectedCount; ++identityRejectedCount; continue;
            }
            if (candidateDistance<0 || !std::isfinite(candidateDistance)) { ++rejectedCount; ++geometryRejectedCount; continue; }
            if (candidateDistance>expandedSearchRadius && !semanticExpandedCandidateIds.contains(static_cast<int>(id))) continue;
            bool exactNameMatch=false;
            for (const QString& smallNameKey:smallNameKeys)
                if (largeFeatureNameKeys.at(static_cast<int>(id)).contains(smallNameKey)) {
                    exactNameMatch=true;
                    break;
                }
            const bool exactIdentityMatch=!smallRef.identity.isEmpty() && smallRef.identity==largeRef.identity;
            CrossScaleCandidate candidate=buildSingleCandidate(smallRef.feature,smallGeom,largeRef.path,largeRef.feature,
                positionTolerance,supportTolerance,supportTolerance,false,rules.spatialWeight,rules.geometryWeight,
                rules.attributeWeight,rules.directionAngleMax);
            candidate.sourceFids=largeRef.sourceFids;
            candidate.logicalModel=largeRef.logicalModel;
            candidate.evidence[QStringLiteral("candidate_match_basis")]=exactIdentityMatch
                ? QStringLiteral("stable_identity"):(exactNameMatch?QStringLiteral("normalized_business_name"):
                    QStringLiteral("spatial_geometry"));
            if(candidateDistance>positionTolerance) {
                ++outsidePositionCount;
                if (semanticExpandedCandidateIds.contains(static_cast<int>(id))) ++expandedOutsidePositionCount;
                if (exactIdentityMatch || exactNameMatch) ++outsideSemanticCandidateCount;
                if (exactNameMatch) ++outsideNameMatchCount;
                if (exactIdentityMatch) ++outsideStableIdentityMatchCount;
                if(nearestOutsidePosition.path.isEmpty() || candidate.distance<nearestOutsidePosition.distance)
                    nearestOutsidePosition=candidate;
                continue;
            }
            if (!sameGeometryType) {
                candidate.geometryScore=1.0;
                candidate.evidence[QStringLiteral("geometry_mode")]=QStringLiteral("configured_geometry_transition");
                candidate.evidence[QStringLiteral("geometry_transition")]=transition;
                candidate.totalScore=weightedCrossScaleScore(candidate.spatialScore,candidate.geometryScore,candidate.attributeScore,
                    rules.spatialWeight,rules.geometryWeight,candidate.hasComparableAttribute?rules.attributeWeight:0.0);
            }
            if (largeRef.identity.isEmpty() || (smallRef.identity.isEmpty() && (lineAggregationAllowed || polygonAggregationAllowed)))
                unidentifiedCandidates << candidate;
            else groups[largeRef.identity] << candidate;
            ++candidateCount;
        }
        const QJsonObject candidateStages{{QStringLiteral("spatial_index_bbox_hits"),spatialIds.size()},
            {QStringLiteral("expanded_spatial_bbox_added"),expandedSpatialAdded},
            {QStringLiteral("expanded_spatial_search_executed"),expandedSearchRadius>searchRadius},
            {QStringLiteral("semantic_identity_or_name_expanded_candidates"),semanticExpandedCandidateIds.size()},
            {QStringLiteral("confirmed_mapping_rejected"),mappingRejectedCount},
            {QStringLiteral("confirmed_sources_within_search_radius"),mappedSpatialHits},
            {QStringLiteral("geometry_type_rejected"),geometryRejectedCount},
            {QStringLiteral("business_class_rejected"),classRejectedCount},
            {QStringLiteral("stable_identity_rejected"),identityRejectedCount},
            {QStringLiteral("position_tolerance_rejected"),outsidePositionCount},
            {QStringLiteral("semantic_expanded_outside_position_tolerance"),expandedOutsidePositionCount},
            {QStringLiteral("semantic_outside_position_candidates"),outsideSemanticCandidateCount},
            {QStringLiteral("same_name_outside_position_candidates"),outsideNameMatchCount},
            {QStringLiteral("eligible_support_candidates"),candidateCount}};

        if(candidateCount==0 && outsidePositionCount>0
            && (unclassifiedLargeCandidates==0 || outsideSemanticCandidateCount>0)) {
            ++outOfRangeCount;
            ++suspectCount;
            const bool uniqueStableIdentityOutside=outsideStableIdentityMatchCount==1
                && !smallRef.identity.isEmpty() && smallIdentityCounts.value(smallRef.identity)==1
                && largeIdentityCounts.value(smallRef.identity)==1;
            const bool ambiguousExpandedOutside=!uniqueStableIdentityOutside
                && (outsideNameMatchCount>1 || outsideStableIdentityMatchCount>1 || unclassifiedLargeCandidates>0);
            QJsonObject warning{{QStringLiteral("pair_name"),task.pairName},{QStringLiteral("target_layer"),target},
                {QStringLiteral("small_shp"),smallRef.path},
                {QStringLiteral("small_feature_id"),static_cast<double>(smallRef.feature.id())},
                {QStringLiteral("small_source_fids"),stringListToJson(smallRef.sourceFids)},
                {QStringLiteral("small_logical_model"),smallRef.logicalModel},
                {QStringLiteral("best_large_shp"),nearestOutsidePosition.path},
                {QStringLiteral("best_large_feature_id"),static_cast<double>(nearestOutsidePosition.feature.id())},
                {QStringLiteral("best_large_source_fids"),stringListToJson(nearestOutsidePosition.sourceFids)},
                {QStringLiteral("best_distance"),nearestOutsidePosition.distance},
                {QStringLiteral("tolerance"),positionTolerance},{QStringLiteral("support_tolerance"),supportTolerance},
                {QStringLiteral("candidate_search_radius"),searchRadius},
                {QStringLiteral("expanded_candidate_search_radius"),expandedSearchRadius},
                {QStringLiteral("position_acceptance_tolerance"),positionTolerance},
                {QStringLiteral("tolerance_budget"),tolerance.evidence},
                {QStringLiteral("candidate_outside_position_tolerance_count"),outsidePositionCount},
                {QStringLiteral("semantic_expanded_outside_position_tolerance"),expandedOutsidePositionCount},
                {QStringLiteral("candidate_match_basis"),nearestOutsidePosition.evidence.value(QStringLiteral("candidate_match_basis"))},
                {QStringLiteral("candidate_stage_counts"),candidateStages},
                {QStringLiteral("checked_geometry_wkt"),smallGeom.asWkt(12)},
                {QStringLiteral("checked_geometry_crs"),checkCrs.toWkt()},
                {QStringLiteral("error_type"),QStringLiteral("cross_scale_candidate_outside_position_tolerance")},
                {QStringLiteral("decision_status"),QStringLiteral("needs_review")},
                {QStringLiteral("best_large_is_error"),false},
                {QStringLiteral("description"),QStringLiteral("检索范围内存在同类候选，但距离超过位置接受容差；需复核尺度概括或空间偏移。")}};
            if (ambiguousExpandedOutside) {
                warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_candidate_ambiguous");
                warning[QStringLiteral("description")]=QStringLiteral("发现多个同标识或同规范名称的远距离候选，无法仅凭语义唯一确定对应关系；需复核。");
            }
            evidenceRecords.append(warning);
            warnings.append(warning);
            continue;
        }

        if (!unidentifiedCandidates.isEmpty()) {
            QVector<int> parent(unidentifiedCandidates.size());
            for (int i=0;i<parent.size();++i) parent[i]=i;
            auto rootOf=[&](int value) {
                int root=value;
                while(parent[root]!=root) root=parent[root];
                while(parent[value]!=value) { const int next=parent[value]; parent[value]=root; value=next; }
                return root;
            };
            if (lineAggregationAllowed) {
                const QVector<QPair<int,int>> joins=unambiguousLineEndpointJoins(unidentifiedCandidates,
                    rules.lineJoinToleranceM,rules.lineJoinAngleMax);
                for(const auto& join:joins) {
                    const int rootA=rootOf(join.first),rootB=rootOf(join.second);
                    if(rootA!=rootB) parent[rootB]=rootA;
                }
            }
            if (polygonAggregationAllowed) {
                for (int i=0;i<unidentifiedCandidates.size();++i) {
                    const QgsGeometry first=unidentifiedCandidates[i].feature.geometry();
                    for (int j=i+1;j<unidentifiedCandidates.size();++j) {
                        const QgsGeometry second=unidentifiedCandidates[j].feature.geometry();
                        if (first.type()!=second.type()) continue;
                        if (polygonAggregationAllowed && first.type()!=QgsWkbTypes::PolygonGeometry) continue;
                        if (first.distance(second)>supportTolerance) continue;
                        const int rootA=rootOf(i),rootB=rootOf(j);
                        if (rootA!=rootB) parent[rootB]=rootA;
                    }
                }
            }
            QMap<int,QVector<CrossScaleCandidate>> components;
            for (int i=0;i<unidentifiedCandidates.size();++i)
                components[rootOf(i)] << unidentifiedCandidates[i];
            for (auto it=components.constBegin();it!=components.constEnd();++it) {
                const QVector<CrossScaleCandidate>& component=it.value();
                const bool mayCombine=component.size()>1 && (lineAggregationAllowed || polygonAggregationAllowed);
                if (mayCombine) groups[QStringLiteral("aggregate:%1:%2").arg(semanticClass).arg(it.key())]=component;
                else for (const CrossScaleCandidate& candidate:component)
                    groups[QStringLiteral("unidentified:%1|%2").arg(SeriesLayerMapping::pathKey(candidate.path)).arg(candidate.feature.id())]
                        << candidate;
            }
        }
        struct Relation {
            CrossScaleCandidate candidate;
            QString key;
            QgsGeometry uncovered;
            double coverage=0;
            double gap=0;
            double score=0;
            int count=0;
            QStringList sourceFids;
            QJsonArray sources;
            double directionCoverage=-1;
        };
        QVector<Relation> relations;
        bool relationComputationFailed=false;
        for (auto groupIt=groups.constBegin();groupIt!=groups.constEnd();++groupIt) {
            const auto& candidates=groupIt.value();
            CrossScaleCandidate candidate=candidates.first();
            for (const CrossScaleCandidate& member:candidates)
                if (member.distance<candidate.distance) candidate=member;
            QVector<QgsGeometry> masks;
            QStringList sourceFids;
            QJsonArray supportSources;
            QVector<QgsGeometry> directionTargets;
            bool allLineTargets=true;
            for(const auto& member:candidates) {
                const QgsGeometry sourceGeometry=member.feature.geometry();
                if(sourceGeometry.type()==QgsWkbTypes::LineGeometry) directionTargets << sourceGeometry;
                else allLineTargets=false;
                if (sourceGeometry.type()==QgsWkbTypes::PolygonGeometry) masks << (rules.toleranceModel==QStringLiteral("scale_pair_v1")
                    ? sourceGeometry.buffer(supportTolerance,8):sourceGeometry);
                else {
                    const double maskTolerance=smallGeom.type()==QgsWkbTypes::PointGeometry
                        && sourceGeometry.type()==QgsWkbTypes::PointGeometry ? positionTolerance:supportTolerance;
                    masks << sourceGeometry.buffer(maskTolerance,8);
                }
                sourceFids << member.sourceFids;
                supportSources.append(QJsonObject{{QStringLiteral("path"),member.path},
                    {QStringLiteral("source_fids"),stringListToJson(member.sourceFids)}});
            }
            const QgsGeometry mask=combineGeometries(masks);
            if(mask.isNull() || !mask.isGeosValid()) {
                relationComputationFailed=true;
                recordInvalidFeature(smallRef.path,smallRef.feature,true,QStringLiteral("候选联合支撑几何构建失败"));
                break;
            }
            const QgsGeometry covered=CrossScaleSupport::clipped(smallGeom,mask);
            const QgsGeometry uncovered=CrossScaleSupport::dimension(smallGeom.difference(mask),smallGeom.type());
            if(!covered.lastError().isEmpty() || !uncovered.lastError().isEmpty()) {
                relationComputationFailed=true;
                recordInvalidFeature(smallRef.path,smallRef.feature,true,QStringLiteral("支撑覆盖几何运算失败"));
                break;
            }
            double coverage=0;
            if (smallGeom.type()==QgsWkbTypes::LineGeometry) coverage=covered.isNull()?0:clampScore(covered.length()/smallGeom.length());
            else if (smallGeom.type()==QgsWkbTypes::PolygonGeometry) coverage=covered.isNull()?0:clampScore(covered.area()/smallGeom.area());
            else coverage=static_cast<double>(CrossScaleSupport::pointCount(covered))/CrossScaleSupport::pointCount(smallGeom);
            const double groupGeometryScore=candidates.size()>1?coverage:candidate.geometryScore;
            const double directionCoverage=rules.toleranceModel==QStringLiteral("scale_pair_v1") && smallGeom.type()==QgsWkbTypes::LineGeometry && allLineTargets
                ? CrossScaleSupport::localDirectionCoverage(smallGeom,combineGeometries(directionTargets),supportTolerance,rules.directionAngleMax):-1;
            const double effectiveCoverage=directionCoverage<0?coverage:
                (std::isfinite(directionCoverage)?qMin(coverage,directionCoverage):0);
            const double relationScore=clampScore(.65*effectiveCoverage+.25*groupGeometryScore+.10*candidate.spatialScore);
            relations.append({candidate,groupIt.key(),uncovered,coverage,
                smallGeom.type()==QgsWkbTypes::LineGeometry?CrossScaleSupport::longestPart(uncovered):0,
                relationScore,candidates.size(),sourceFids,supportSources,directionCoverage});
        }
        if (relationComputationFailed) continue;
        std::sort(relations.begin(),relations.end(),[&](const Relation& a,const Relation& b) {
            const bool aIdentityMatch=!smallRef.identity.isEmpty() && a.key==smallRef.identity;
            const bool bIdentityMatch=!smallRef.identity.isEmpty() && b.key==smallRef.identity;
            if(aIdentityMatch!=bIdentityMatch) return aIdentityMatch;
            if (!qFuzzyCompare(a.score+1.0,b.score+1.0)) return a.score>b.score;
            return a.candidate.distance<b.candidate.distance;
        });
        const Relation bestRelation=relations.isEmpty()?Relation():relations.first();
        const CrossScaleCandidate best=bestRelation.candidate;
        const QString bestIdentity=bestRelation.key;
        const double bestCoverage=relations.isEmpty()?-1.0:bestRelation.coverage;
        const double bestGap=bestRelation.gap;
        const int bestCount=bestRelation.count;
        const QgsGeometry bestUncovered=bestRelation.uncovered;
        const double runnerUpScore=relations.size()>1?relations.at(1).score:-1.0;
        const double runnerUpDistance=relations.size()>1?relations.at(1).candidate.distance:-1.0;
        const bool geometryUnique=relations.size()==1 || (bestRelation.score-runnerUpScore>=rules.uniqueScoreMargin)
            || (runnerUpDistance-best.distance>=qMax(1.0,positionTolerance*.35) && best.distance<=positionTolerance*.5);
        const bool identityMatched=!smallRef.identity.isEmpty() && smallIdentityCounts.value(smallRef.identity)==1
            && largeIdentityCounts.value(smallRef.identity)==1 && bestIdentity==smallRef.identity;
        const QString largeBusiness=bestCount>0?resolveBusinessClass(best.path,best.feature):QString();
        const bool businessMatched=!smallRef.businessClass.isEmpty() && smallRef.businessClass==largeBusiness;
        const bool aggregationAllowed=bestCount<=1 || lineAggregationAllowed || polygonAggregationAllowed;
        const bool pointUnambiguous=smallGeom.type()!=QgsWkbTypes::PointGeometry || bestCount<=1;
        const bool pointEntityReliable=!rules.pointSymbolClasses.contains(semanticClass)
            || (smallRef.logicalModel==QStringLiteral("symbol_instance_anchor")
                && best.logicalModel==QStringLiteral("symbol_instance_anchor"));
        const bool localDirectionApplicable=bestRelation.directionCoverage!=-1;
        const bool directionSupported=!localDirectionApplicable || (std::isfinite(bestRelation.directionCoverage)
            && bestRelation.directionCoverage>=rules.matchedThreshold);
        QJsonArray failedGates;
        if(bestCount<=0) {
            failedGates.append(mappedSpatialHits==0?QStringLiteral("no_spatial_candidate")
                :QStringLiteral("no_eligible_support_candidate"));
        } else {
            if(bestCoverage<rules.matchedThreshold) failedGates.append(QStringLiteral("coverage_below_threshold"));
            if(bestGap>maxGap) failedGates.append(QStringLiteral("significant_uncovered_segment"));
            if(!businessMatched) failedGates.append(QStringLiteral("semantic_class_mismatch"));
            if(!aggregationAllowed) failedGates.append(QStringLiteral("aggregation_not_allowed"));
            if(!pointUnambiguous) failedGates.append(QStringLiteral("multiple_point_support_groups"));
            if(!pointEntityReliable) failedGates.append(QStringLiteral("point_entity_reconstruction_uncertain"));
            if(!identityMatched && !geometryUnique) failedGates.append(QStringLiteral("candidate_not_unique"));
            if(!directionSupported) failedGates.append(std::isfinite(bestRelation.directionCoverage)
                ?QStringLiteral("local_direction_coverage_below_threshold"):QStringLiteral("local_direction_evaluation_failed"));
        }
        QJsonObject evidence{{QStringLiteral("small_shp"),smallRef.path},
            {QStringLiteral("small_feature_id"),static_cast<double>(smallRef.feature.id())},
            {QStringLiteral("small_source_fids"),stringListToJson(smallRef.sourceFids)},
            {QStringLiteral("small_logical_model"),smallRef.logicalModel},
            {QStringLiteral("checked_geometry_wkt"),smallGeom.asWkt(12)},
            {QStringLiteral("checked_geometry_crs"),checkCrs.toWkt()},
            {QStringLiteral("checked_length"),smallGeom.type()==QgsWkbTypes::LineGeometry?smallGeom.length():0},
            {QStringLiteral("coverage_ratio"),qMax(0.0,bestCoverage)},
            {QStringLiteral("longest_uncovered_length"),bestGap},
            {QStringLiteral("max_uncovered_segment_length"),maxGap},
            {QStringLiteral("max_uncovered_segment_threshold_mm"),rules.toleranceModel==QStringLiteral("scale_pair_v1")
                ? CrossScaleTolerance::profile(rules.toleranceProfiles,semanticClass).value(QStringLiteral("gap_mm")).toDouble():rules.maxUncoveredSegmentLengthMm},
            {QStringLiteral("candidate_count"),candidateCount},{QStringLiteral("excluded_candidate_count"),rejectedCount},
            {QStringLiteral("candidate_stage_counts"),candidateStages},
            {QStringLiteral("candidate_outside_position_tolerance_count"),outsidePositionCount},
            {QStringLiteral("nearest_outside_position_distance"),outsidePositionCount>0?QJsonValue(nearestOutsidePosition.distance):QJsonValue()},
            {QStringLiteral("candidate_search_radius"),searchRadius},
            {QStringLiteral("expanded_candidate_search_radius"),expandedSearchRadius},
            {QStringLiteral("tolerance_budget"),tolerance.evidence},
            {QStringLiteral("position_acceptance_tolerance"),positionTolerance},
            {QStringLiteral("support_tolerance"),supportTolerance},
            {QStringLiteral("best_relation_group_candidate_count"),bestCount},
            {QStringLiteral("line_direction_check_applicable"),localDirectionApplicable},
            {QStringLiteral("line_direction_coverage_ratio"),localDirectionApplicable && std::isfinite(bestRelation.directionCoverage)
                ?QJsonValue(bestRelation.directionCoverage):QJsonValue(QJsonValue::Null)},
            {QStringLiteral("line_direction_max_angle_deg"),rules.directionAngleMax},
            {QStringLiteral("line_direction_sampling_step_m"),qMax(1.0,supportTolerance*.5)},
            {QStringLiteral("best_relation_group_key"),bestIdentity},
            {QStringLiteral("best_large_source_fids"),stringListToJson(bestRelation.sourceFids)},
            {QStringLiteral("best_support_sources"),bestRelation.sources},
            {QStringLiteral("best_large_logical_model"),best.logicalModel},
            {QStringLiteral("line_join_tolerance_m"),rules.lineJoinToleranceM},
            {QStringLiteral("line_join_angle_max"),rules.lineJoinAngleMax},
            {QStringLiteral("line_support_grouping_model"),lineAggregationAllowed
                ? QStringLiteral("unique_endpoint_continuity"):QStringLiteral("single_candidate_or_stable_identity")},
            {QStringLiteral("stable_identity_matched"),identityMatched},
            {QStringLiteral("unique_spatial_relation"),geometryUnique},
            {QStringLiteral("best_relation_score"),qMax(0.0,bestRelation.score)},
            {QStringLiteral("best_spatial_score"),best.spatialScore},
            {QStringLiteral("best_geometry_score"),best.geometryScore},
            {QStringLiteral("best_candidate_match_basis"),best.evidence.value(QStringLiteral("candidate_match_basis"))},
            {QStringLiteral("runner_up_relation_score"),runnerUpScore},
            {QStringLiteral("runner_up_distance"),runnerUpDistance},
            {QStringLiteral("unique_score_margin"),rules.uniqueScoreMargin},
            {QStringLiteral("business_class_matched"),businessMatched},
            {QStringLiteral("small_business_class"),smallRef.businessClass},
            {QStringLiteral("best_large_business_class"),largeBusiness},
            {QStringLiteral("candidate_limit_policy"),QStringLiteral("diagnostic_samples_only")},
            {QStringLiteral("configured_candidate_limit"),rules.candidateLimit},
            {QStringLiteral("support_failure_gates"),failedGates},
            {QStringLiteral("diagnostic_score_is_acceptance_gate"),false},
            {QStringLiteral("score_model"),QStringLiteral("semantic_class_geometry_coverage_unique_relation")},
            {QStringLiteral("style_comparison_applicable"),false}};
        evidenceRecords.append(evidence);
        const bool pointFullyCovered=smallGeom.type()!=QgsWkbTypes::PointGeometry || bestCoverage>=1.0;
        const bool supported=bestCount>0 && bestCoverage>=rules.matchedThreshold && bestGap<=maxGap
            && businessMatched && aggregationAllowed && pointUnambiguous && pointEntityReliable && pointFullyCovered
            && directionSupported && (identityMatched || geometryUnique);
        if (supported) {
            ++matchedCount; if(bestCount>1) ++groupMatchedCount; continue;
        }
        QJsonObject warning=evidence;
        warning[QStringLiteral("pair_name")]=task.pairName;
        warning[QStringLiteral("sub_rule")]=QStringLiteral("跨比例尺实际SHP要素对应");
        warning[QStringLiteral("target_layer")]=target;
        warning[QStringLiteral("best_large_shp")]=best.path;
        warning[QStringLiteral("best_large_feature_id")]=bestCount>0 ? QJsonValue(static_cast<double>(best.feature.id())) : QJsonValue();
        warning[QStringLiteral("best_score")]=qMax(0.0,bestCoverage);
        warning[QStringLiteral("best_distance")]=bestCount>0?QJsonValue(best.distance):QJsonValue();
        warning[QStringLiteral("tolerance")]=positionTolerance;
        warning[QStringLiteral("severity")]=rules.outputSeverity;
        warning[QStringLiteral("decision_status")]=QStringLiteral("needs_review");
        warning[QStringLiteral("best_large_is_error")]=false;
        warning[QStringLiteral("uncovered_geometry_wkt")]=bestUncovered.isNull()?smallGeom.asWkt(12):bestUncovered.asWkt(12);
        if (unclassifiedLargeCandidates>0) {
            ++unclassifiedCount;
            warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_business_class_not_evaluable");
            warning[QStringLiteral("decision_status")]=QStringLiteral("not_evaluable");
            warning[QStringLiteral("unclassified_large_candidate_count")]=unclassifiedLargeCandidates;
            warning[QStringLiteral("description")]=QStringLiteral("已确认对应的大比例尺空间候选无法识别业务类别，未据此判定支撑缺失或不足。");
        } else if (candidateCount==0) {
            ++unmatchedCount;
            warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_support_missing");
            warning[QStringLiteral("description")]=QStringLiteral("共同覆盖范围内未找到同类、同标识空间支撑，需复核数据表达或概括关系。");
        } else if ((!identityMatched && !geometryUnique) || !businessMatched || !aggregationAllowed || !pointUnambiguous
            || !pointEntityReliable) {
            ++suspectCount;
            warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_candidate_ambiguous");
            warning[QStringLiteral("description")]=QStringLiteral("存在空间候选，但业务类别未明确、稳定标识不唯一或缺少聚合规则，不能确认支撑关系。");
        } else {
            ++suspectCount;
            warning[QStringLiteral("error_type")]=QStringLiteral("cross_scale_weak_support");
            warning[QStringLiteral("description")]=QStringLiteral("同标识候选的联合覆盖不足或存在过长未覆盖段，需复核具体区段。");
        }
        if (rules.toleranceModel!=QStringLiteral("scale_pair_v1") && candidateCount==0 && unclassifiedLargeCandidates==0 && rules.strictMissingSupportClasses.contains(semanticClass)
            && !smallRef.identity.isEmpty() && smallIdentityCounts.value(smallRef.identity)==1
            && !invalidLargeClasses.contains(semanticClass)) {
            warning[QStringLiteral("decision_status")]=QStringLiteral("confirmed_error");
            warning[QStringLiteral("right_is_error")]=true;
            warning[QStringLiteral("description")]=QStringLiteral("该类别配置为严格保留对象，且在完整读取的大比例尺成果中未找到同标识支撑要素。");
        }
        warnings.append(warning);
    }
    int retentionCheckedCount=0;
    for (const Ref& largeRef:largeFeatures) {
        const QString semanticClass=CrossScaleSupport::key(largeRef.businessClass);
        if (!rules.mandatoryRetentionClasses.contains(semanticClass)) continue;
        const auto tolerance=toleranceForClass(semanticClass);
        const double positionTolerance=tolerance.position,supportTolerance=tolerance.support,searchRadius=tolerance.expanded;
        ++retentionCheckedCount;
        const bool stableIdentityUnique=!largeRef.identity.isEmpty() && largeIdentityCounts.value(largeRef.identity)==1;
        bool represented=false,unclassifiedSmallCandidate=false;
        QgsRectangle search=largeRef.feature.geometry().boundingBox();
        search.grow(searchRadius);
        const QgsGeometry largeGeom=largeRef.feature.geometry();
        for (QgsFeatureId id:smallIndex.intersects(search)) {
            const Ref& smallRef=smallFeatures[static_cast<int>(id)];
            if (!confirmedLayer.isEmpty() && !SeriesLayerMapping::allowsPair(confirmedLayer,largeRef.path,smallRef.path)) continue;
            const QgsGeometry smallGeom=smallRef.feature.geometry();
            if (smallRef.businessClass.isEmpty() || CrossScaleSupport::isAnnotationPath(smallRef.path)) {
                if (largeGeom.distance(smallGeom)<=searchRadius) unclassifiedSmallCandidate=true;
                continue;
            }
            if (smallRef.businessClass!=largeRef.businessClass) continue;
            if (CrossScaleSupport::identitiesConflict(smallRef.identity,largeRef.identity)) continue;
            const QString transition=QStringLiteral("%1->%2")
                .arg(smallGeom.type()==QgsWkbTypes::PointGeometry?QStringLiteral("point"):
                     smallGeom.type()==QgsWkbTypes::LineGeometry?QStringLiteral("line"):QStringLiteral("polygon"))
                .arg(largeGeom.type()==QgsWkbTypes::PointGeometry?QStringLiteral("point"):
                     largeGeom.type()==QgsWkbTypes::LineGeometry?QStringLiteral("line"):QStringLiteral("polygon"));
            if (largeGeom.type()!=smallGeom.type() && !rules.geometryTransitions.contains(transition)) continue;
            if (largeGeom.distance(smallGeom)>positionTolerance) continue;
            if(rules.toleranceModel==QStringLiteral("scale_pair_v1") && largeGeom.type()==QgsWkbTypes::LineGeometry
                && smallGeom.type()==QgsWkbTypes::LineGeometry) {
                const double directionCoverage=CrossScaleSupport::localDirectionCoverage(largeGeom,smallGeom,supportTolerance,rules.directionAngleMax);
                if(!std::isfinite(directionCoverage) || directionCoverage<rules.matchedThreshold) continue;
            }
            const double retentionBuffer=rules.toleranceModel==QStringLiteral("scale_pair_v1") && smallGeom.type()==QgsWkbTypes::PointGeometry
                ?positionTolerance:supportTolerance;
            const QgsGeometry mask=smallGeom.type()==QgsWkbTypes::PolygonGeometry && rules.toleranceModel!=QStringLiteral("scale_pair_v1")
                ? smallGeom : smallGeom.buffer(retentionBuffer,8);
            const QgsGeometry covered=CrossScaleSupport::clipped(largeGeom,mask);
            if (covered.isNull() || !covered.lastError().isEmpty()) { unclassifiedSmallCandidate=true; continue; }
            double coverage=0;
            if (largeGeom.type()==QgsWkbTypes::LineGeometry) coverage=clampScore(covered.length()/qMax(0.0001,largeGeom.length()));
            else if (largeGeom.type()==QgsWkbTypes::PolygonGeometry) coverage=clampScore(covered.area()/qMax(0.0001,largeGeom.area()));
            else coverage=static_cast<double>(CrossScaleSupport::pointCount(covered))/qMax(1,CrossScaleSupport::pointCount(largeGeom));
            if (coverage>=rules.matchedThreshold) { represented=true; break; }
        }
        if (represented) continue;
        QJsonObject warning{{QStringLiteral("pair_name"),task.pairName},{QStringLiteral("target_layer"),target},
            {QStringLiteral("small_shp"),largeRef.path},
            {QStringLiteral("small_feature_id"),static_cast<double>(largeRef.feature.id())},
            {QStringLiteral("best_large_shp"),largeRef.path},
            {QStringLiteral("best_large_feature_id"),static_cast<double>(largeRef.feature.id())},
            {QStringLiteral("checked_geometry_wkt"),largeGeom.asWkt(12)},
            {QStringLiteral("checked_geometry_crs"),checkCrs.toWkt()},
            {QStringLiteral("small_business_class"),semanticClass},
            {QStringLiteral("tolerance_budget"),tolerance.evidence},
            {QStringLiteral("error_type"),QStringLiteral("cross_scale_mandatory_retention_missing")},
            {QStringLiteral("right_is_error"),true},
            {QStringLiteral("description"),QStringLiteral("该大比例尺要素所属类别配置为小比例尺必须保留，但未找到满足几何覆盖条件的对应表达。")}};
        if (rules.toleranceModel!=QStringLiteral("scale_pair_v1") && stableIdentityUnique && !invalidSmallClasses.contains(semanticClass) && !unclassifiedSmallCandidate) {
            warning[QStringLiteral("decision_status")]=QStringLiteral("confirmed_error");
        } else {
            warning[QStringLiteral("decision_status")]=unclassifiedSmallCandidate || invalidSmallClasses.contains(semanticClass)
                ? QStringLiteral("not_evaluable") : QStringLiteral("needs_review");
            warning[QStringLiteral("description")]=QStringLiteral("未找到对应表达，但稳定标识不唯一或小比例尺同类数据不完整，暂不能自动确认遗漏。");
        }
        evidenceRecords.append(warning);
        warnings.append(warning);
    }

    const int evaluatedFeatureCount=matchedCount+suspectCount+unmatchedCount+retentionCheckedCount;
    const bool notEvaluableFeatures=unclassifiedCount>0 || featureInputIssues;
    return finish(evidenceRecords.isEmpty()?QStringLiteral("no_applicable_features"):
        (evaluatedFeatureCount==0?QStringLiteral("not_evaluable"):
            (notEvaluableFeatures?QStringLiteral("partial_not_evaluable"):QStringLiteral("completed"))));
}

QJsonObject SeriesCrossScaleExpressionTaskDialog::runCrossScaleExpressionCheck(const MapTask& task, const QJsonObject& layerMapping) const
{
    QJsonObject result{{QStringLiteral("pair_name"),task.pairName},
        {QStringLiteral("large_scale_map"),task.largeScaleName},{QStringLiteral("small_scale_map"),task.smallScaleName},
        {QStringLiteral("large_scale"),task.largeScale},{QStringLiteral("small_scale"),task.smallScale}};
    result[QStringLiteral("source_audits")]=layerMapping.value(QStringLiteral("source_audits"));
    const QJsonObject layers=SeriesLayerExclusions::filterMapping(layerMapping, true).value(QStringLiteral("layers")).toObject();
    auto stopped = [&](const QString& state,const QString& message) {
        QJsonArray summaries,warnings;
        const QStringList targets=layers.isEmpty()?QStringList{QStringLiteral("图层对应关系")}:layers.keys();
        for (const QString& target:targets) {
            const QJsonObject warning{{QStringLiteral("pair_name"),task.pairName},{QStringLiteral("target_layer"),target},
                {QStringLiteral("error_type"),QStringLiteral("cross_scale_not_evaluable")},
                {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},{QStringLiteral("description"),message}};
            summaries.append(QJsonObject{{QStringLiteral("target_layer"),target},{QStringLiteral("execution_state"),state},
                {QStringLiteral("status"),CrossScaleSupport::executionStatus(state)},{QStringLiteral("warnings"),QJsonArray{warning}}});
            warnings.append(warning);
        }
        result[QStringLiteral("layer_summaries")]=summaries; result[QStringLiteral("warnings")]=warnings;
        result[QStringLiteral("warning_count")]=warnings.size(); result[QStringLiteral("execution_state")]=state;
        result[QStringLiteral("status")]=CrossScaleSupport::executionStatus(state);
        result[QStringLiteral("description")]=message;
        return result;
    };
    const CrossScaleRules rules=readCrossScaleRules(m_xmlEdit->text().trimmed(),task.smallScale);
    if (!rules.valid) return stopped(QStringLiteral("invalid_config"),rules.errorMessage);
    if (layers.isEmpty()) return stopped(QStringLiteral("no_applicable_features"),QStringLiteral("未勾选适用的图层对应关系。"));
    const auto largeScope=CrossScaleSupport::frames(task.largeShpDir);
    if (!largeScope.error.isEmpty()) return stopped(QStringLiteral("invalid_scope"),largeScope.error);
    const auto smallScope=CrossScaleSupport::frames(task.smallShpDir,largeScope.crs);
    if (!smallScope.error.isEmpty()) return stopped(QStringLiteral("invalid_scope"),smallScope.error);
    const QgsGeometry common=CrossScaleSupport::dimension(largeScope.geometry.intersection(smallScope.geometry),QgsWkbTypes::PolygonGeometry);
    if (common.isNull() || common.isEmpty() || !common.isGeosValid())
        return stopped(QStringLiteral("invalid_scope"),QStringLiteral("两侧真实图廓没有有效的共同覆盖面。"));
    result[QStringLiteral("check_crs")]=largeScope.crs.toWkt();
    result[QStringLiteral("checked_scope_wkt")]=common.asWkt(12);
    QJsonArray warnings,layerSummaries;
    QString state=QStringLiteral("no_applicable_features");
    for (const QString& target:layers.keys()) {
        const QJsonObject layer=layers.value(target).toObject();
        QStringList largePaths,smallPaths;
        for (const QJsonValue& v:SeriesLayerMapping::selectedPaths(layer,QStringLiteral("left"))) largePaths << v.toString();
        for (const QJsonValue& v:SeriesLayerMapping::selectedPaths(layer,QStringLiteral("right"))) smallPaths << v.toString();
        bool incomplete=layer.contains(QStringLiteral("rows")) && !layer.value(QStringLiteral("rows")).isArray();
        auto hasPaths=[](const QJsonArray& paths) {
            if(paths.isEmpty()) return false;
            for(const auto& path:paths) if(!path.isString() || path.toString().trimmed().isEmpty()) return false;
            return true;
        };
        for(const auto& rowValue:layer.value(QStringLiteral("rows")).toArray()) {
            const auto row=rowValue.toObject();
            if(!hasPaths(row.value(QStringLiteral("left")).toArray()) || !hasPaths(row.value(QStringLiteral("right")).toArray())) incomplete=true;
        }
        QJsonObject summary;
        if(incomplete) {
            const QJsonObject warning{{QStringLiteral("target_layer"),target},
                {QStringLiteral("error_type"),QStringLiteral("cross_scale_not_evaluable")},
                {QStringLiteral("decision_status"),QStringLiteral("not_evaluable")},
                {QStringLiteral("description"),QStringLiteral("勾选的对应行缺少一侧实际图层，请补齐对应关系或取消勾选。")}};
            summary=QJsonObject{{QStringLiteral("target_layer"),target},{QStringLiteral("execution_state"),QStringLiteral("invalid_input")},
                {QStringLiteral("status"),QStringLiteral("质检失败")},{QStringLiteral("warnings"),QJsonArray{warning}}};
        } else summary=runLayerCorrespondenceCheck(task,target,smallPaths,largePaths,rules,common,layer,largeScope.crs.toWkt());
        layerSummaries.append(summary);
        const QString layerState=summary.value(QStringLiteral("execution_state")).toString();
        state=CrossScaleSupport::mergeExecutionState(state,layerState);
        for (const QJsonValue& warning:summary.value(QStringLiteral("warnings")).toArray()) warnings.append(warning);
    }
    result[QStringLiteral("layer_summaries")]=layerSummaries;
    result[QStringLiteral("warnings")]=warnings;
    result[QStringLiteral("warning_count")]=warnings.size();
    result[QStringLiteral("execution_state")]=state;
    result[QStringLiteral("status")]=CrossScaleSupport::executionStatus(state);
    return result;
}

QVector<QJsonObject> SeriesCrossScaleExpressionTaskDialog::buildOutputRecords(const QVector<QJsonObject>& taskResults, QMap<QString, QSet<QString>>* checkedLayersByMap) const
{
    QVector<QJsonObject> records;
    int errIndex = 1;

    for (const QJsonObject& task : taskResults) {
        const QString pairName = task.value(QStringLiteral("pair_name")).toString();
        const QString largeMap = task.value(QStringLiteral("large_scale_map")).toString();
        const QString smallMap = task.value(QStringLiteral("small_scale_map")).toString();

        const QJsonArray layerSummaries = task.value(QStringLiteral("layer_summaries")).toArray();
        QMap<QString,QJsonObject> summariesByLayer;
        for (const QJsonValue& value : layerSummaries) {
            const QString layerName = value.toObject().value(QStringLiteral("target_layer")).toString();
            if(!layerName.isEmpty()) summariesByLayer[layerName]=value.toObject();
            if (layerName.isEmpty() || !checkedLayersByMap) continue;
            if (!largeMap.isEmpty()) (*checkedLayersByMap)[largeMap].insert(layerName);
            if (!smallMap.isEmpty()) (*checkedLayersByMap)[smallMap].insert(layerName);
        }

        const QJsonArray warnings = task.value(QStringLiteral("warnings")).toArray();
        for (const QJsonValue& value : warnings) {
            QJsonObject record = CrossScaleSupport::normalizeFinding(value.toObject());
            if (record.value(QStringLiteral("checked_geometry_crs")).toString().trimmed().isEmpty())
                record[QStringLiteral("checked_geometry_crs")] = task.value(QStringLiteral("check_crs"));
            const QString layerName = record.value(QStringLiteral("target_layer")).toString(record.value(QStringLiteral("layer")).toString());
            const QString smallShpPath = record.value(QStringLiteral("small_shp")).toString();
            const QString largeShpPath = record.value(QStringLiteral("best_large_shp")).toString();
            const QString smallFid = record.value(QStringLiteral("small_feature_id")).toVariant().toString();
            const QString largeFid = record.value(QStringLiteral("best_large_feature_id")).toVariant().toString();

            record[QStringLiteral("err_id")] = QStringLiteral("cross_scale_%1").arg(errIndex++, 5, 10, QChar('0'));
            record[QStringLiteral("pair_name")] = pairName;
            record[QStringLiteral("layer")] = layerName;
            record[QStringLiteral("standard_layer")] = layerName;
            record[QStringLiteral("left_sheet")] = largeMap;
            record[QStringLiteral("right_sheet")] = smallMap;
            record[QStringLiteral("left_standard_layer")] = layerName;
            record[QStringLiteral("right_standard_layer")] = layerName;
            record[QStringLiteral("left_shp_path")] = largeShpPath;
            record[QStringLiteral("right_shp_path")] = smallShpPath;
            record[QStringLiteral("left_shp")] = largeShpPath.isEmpty() ? QString() : QFileInfo(largeShpPath).fileName();
            record[QStringLiteral("right_shp")] = smallShpPath.isEmpty() ? QString() : QFileInfo(smallShpPath).fileName();
            record[QStringLiteral("left_fid")] = largeFid;
            record[QStringLiteral("right_fid")] = smallFid;
            record[QStringLiteral("check_name")] = QStringLiteral("同系列跨比例尺表达检查");
            records.push_back(CrossScaleFindingPresentation::decorate(record,task,summariesByLayer.value(layerName)));
        }
    }

    return records;
}

bool SeriesCrossScaleExpressionTaskDialog::writeOutputs(const QVector<QJsonObject>& taskResults, QString* errorDetails) const
{
    if(errorDetails) errorDetails->clear();
    bool ok=true;
    QMap<QString,QJsonArray> outputDiagnostics;
    QStringList failureMessages;
    auto outputIssue=[&](const QString& map,const QString& stage,const QString& path,
        const QString& fid,const QString& reason,bool warning=false) {
        outputDiagnostics[map].append(QJsonObject{{QStringLiteral("severity"),warning?QStringLiteral("warning"):QStringLiteral("error")},
            {QStringLiteral("stage"),stage},{QStringLiteral("path"),path},
            {QStringLiteral("fid"),fid},{QStringLiteral("reason"),reason}});
        if(!warning) {
            ok=false;
            if(failureMessages.size()<5)
                failureMessages << QStringLiteral("%1 / %2 / %3 / FID %4：%5").arg(map,stage,path,fid,reason);
        }
    };
    QMap<QString,QSet<QString>> checkedLayersByMap;
    const QVector<QJsonObject> records=buildOutputRecords(taskResults,&checkedLayersByMap);
    const QString checkName=QStringLiteral("同系列跨比例尺表达检查");
    QString jsonIoError;
    auto writeJson=[&](const QString& path,const QJsonObject& object) {
        jsonIoError.clear();
        QFile file(path);
        if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate)) { jsonIoError=file.errorString(); return false; }
        const QByteArray bytes=QJsonDocument(object).toJson(QJsonDocument::Indented);
        if(file.write(bytes)!=bytes.size() || !file.flush()) { jsonIoError=file.errorString(); return false; }
        return true;
    };
    auto execution=[&](const QString& mapName,const QString& layerName) {
        QString state=QStringLiteral("no_applicable_features");
        QJsonArray executions;
        for(const QJsonObject& task:taskResults) {
            if(task.value(QStringLiteral("large_scale_map")).toString()!=mapName
                && task.value(QStringLiteral("small_scale_map")).toString()!=mapName) continue;
            for(const auto& value:task.value(QStringLiteral("layer_summaries")).toArray()) {
                QJsonObject layer=value.toObject();
                if(!layerName.isEmpty() && layer.value(QStringLiteral("target_layer")).toString()!=layerName) continue;
                layer.remove(QStringLiteral("warnings"));
                layer[QStringLiteral("pair_name")]=task.value(QStringLiteral("pair_name"));
                layer[QStringLiteral("small_scale_map")]=task.value(QStringLiteral("small_scale_map"));
                layer[QStringLiteral("large_scale_map")]=task.value(QStringLiteral("large_scale_map"));
                layer[QStringLiteral("small_scale")]=task.value(QStringLiteral("small_scale"));
                layer[QStringLiteral("large_scale")]=task.value(QStringLiteral("large_scale"));
                layer[QStringLiteral("checked_scope_wkt")]=task.value(QStringLiteral("checked_scope_wkt"));
                layer[QStringLiteral("check_crs")]=task.value(QStringLiteral("check_crs"));
                layer[QStringLiteral("source_audits")]=task.value(QStringLiteral("source_audits"));
                executions.append(layer);
                const QString current=layer.value(QStringLiteral("execution_state")).toString(task.value(QStringLiteral("execution_state")).toString());
                state=CrossScaleSupport::mergeExecutionState(state,current);
            }
        }
        return QJsonObject{{QStringLiteral("execution_state"),state},
            {QStringLiteral("execution_records"),executions}};
    };
    auto checkItem=[&](const QVector<QJsonObject>& mapRecords,const QJsonObject& executionInfo,const QString& layerName) {
        QJsonObject errors,reviews;
        int needsReview=0,notEvaluable=0;
        for(QJsonObject record:mapRecords) {
            if(!layerName.isEmpty() && record.value(QStringLiteral("layer")).toString()!=layerName) continue;
            record=CrossScaleSupport::normalizeFinding(record);
            const QString id=record.value(QStringLiteral("err_id")).toString();
            const QString status=record.value(QStringLiteral("decision_status")).toString();
            if(status==QStringLiteral("confirmed_error")) errors[id]=record;
            else {
                reviews[id]=record;
                if(status==QStringLiteral("not_evaluable")) ++notEvaluable; else ++needsReview;
            }
        }
        QJsonObject info=executionInfo;
        info[QStringLiteral("auto_confirmed_count")]=errors.size();
        info[QStringLiteral("final_error_count")]=errors.size();
        info[QStringLiteral("error_count")]=errors.size();
        info[QStringLiteral("needs_review_count")]=needsReview;
        info[QStringLiteral("not_evaluable_count")]=notEvaluable;
        const QString state=info.value(QStringLiteral("execution_state")).toString();
        const QString description=state==QStringLiteral("completed")
            ? QStringLiteral("确认错误 %1 条，待复核 %2 条，不可评估 %3 条。").arg(errors.size()).arg(needsReview).arg(notEvaluable)
            : QStringLiteral("检查未完整执行（%1），确认错误 %2 条，待复核 %3 条，不可评估 %4 条。")
                .arg(state).arg(errors.size()).arg(needsReview).arg(notEvaluable);
        info[QStringLiteral("description")]=description;
        info[QStringLiteral("summary")]=description;
        return QJsonObject{{QStringLiteral("id"),kCrossScaleCheckItemId},{QStringLiteral("name"),checkName},
            {QStringLiteral("status"),CrossScaleSupport::executionStatus(state)},
            {QStringLiteral("err_exist"),!errors.isEmpty() || needsReview>0},{QStringLiteral("info"),info},
            {QStringLiteral("errors"),errors},{QStringLiteral("review_errors"),reviews}};
    };
    struct FeatureOutput { QJsonObject record; QgsGeometry geometry; QgsCoordinateReferenceSystem crs; QString side,sheet,shp,fid,status; };
    struct ResultColumn { QString name,alias; QVariant::Type type; int length,precision,width; bool hidden=false; };
    const QVector<ResultColumn> resultColumns{
        {QStringLiteral("result"),QStringLiteral("复核状态"),QVariant::String,40,0,120},
        {QStringLiteral("issue"),QStringLiteral("发现情况"),QVariant::String,80,0,170},
        {QStringLiteral("sm_map"),QStringLiteral("小比例尺检查地图"),QVariant::String,160,0,160},
        {QStringLiteral("sm_scale"),QStringLiteral("小图比例尺"),QVariant::String,24,0,110},
        {QStringLiteral("sm_layer"),QStringLiteral("小图源图层"),QVariant::String,200,0,170},
        {QStringLiteral("sm_fid"),QStringLiteral("小图源要素FID"),QVariant::String,254,0,100},
        {QStringLiteral("lg_map"),QStringLiteral("大比例尺支撑地图"),QVariant::String,160,0,180},
        {QStringLiteral("lg_scale"),QStringLiteral("大图比例尺"),QVariant::String,24,0,110},
        {QStringLiteral("lg_layer"),QStringLiteral("确认的大图对应图层"),QVariant::String,254,0,190},
        {QStringLiteral("lg_fid"),QStringLiteral("大图候选要素FID"),QVariant::String,254,0,150},
        {QStringLiteral("desc"),QStringLiteral("实际发现与触发条件"),QVariant::String,254,0,440},
        {QStringLiteral("advice"),QStringLiteral("需要核对的内容"),QVariant::String,254,0,360},
        {QStringLiteral("liability"),QStringLiteral("错误归属判断"),QVariant::String,120,0,220},
        {QStringLiteral("mark_side"),QStringLiteral("结果几何标记来源"),QVariant::String,254,0,200},
        {QStringLiteral("cov_pct"),QStringLiteral("支撑覆盖率(%)"),QVariant::Double,12,2,125},
        {QStringLiteral("gap_m"),QStringLiteral("最长连续未支撑段(米)"),QVariant::Double,18,2,160},
        {QStringLiteral("cov_min"),QStringLiteral("最低覆盖要求(%)"),QVariant::Double,12,2,125,true},
        {QStringLiteral("gap_lim"),QStringLiteral("连续缺段限值(米)"),QVariant::Double,18,2,140,true},
        {QStringLiteral("dist_m"),QStringLiteral("候选最近距离(米)"),QVariant::Double,18,2,140,true},
        {QStringLiteral("search_m"),QStringLiteral("空间初选范围(米)"),QVariant::Double,18,2,140,true},
        {QStringLiteral("sm_mm"),QStringLiteral("初选范围折合小图(毫米)"),QVariant::Double,14,3,170,true},
        {QStringLiteral("lg_mm"),QStringLiteral("初选范围折合大图(毫米)"),QVariant::Double,14,3,170,true},
        {QStringLiteral("pos_lim"),QStringLiteral("位置接受/标注消歧容差(米)"),QVariant::Double,18,2,175,true},
        {QStringLiteral("geom_tol"),QStringLiteral("几何支撑缓冲(米)"),QVariant::Double,18,2,140,true},
        {QStringLiteral("search_ext"),QStringLiteral("扩展初选范围(米)"),QVariant::Double,18,2,140,true},
        {QStringLiteral("tol_model"),QStringLiteral("容差计算方式"),QVariant::String,100,0,200,true},
        {QStringLiteral("tol_src"),QStringLiteral("容差参数来源"),QVariant::String,160,0,230,true},
        {QStringLiteral("tol_class"),QStringLiteral("容差使用的业务类别"),QVariant::String,80,0,200,true},
        {QStringLiteral("dir_cov"),QStringLiteral("局部方向一致覆盖率(%)"),QVariant::Double,12,2,165,true},
        {QStringLiteral("dir_deg"),QStringLiteral("局部方向允许夹角(度)"),QVariant::Double,12,2,165,true},
        {QStringLiteral("err_id"),QStringLiteral("详细证据记录编号"),QVariant::String,40,0,190},
        {QStringLiteral("status"),QStringLiteral("内部状态代码"),QVariant::String,24,0,130,true},
        {QStringLiteral("err_type"),QStringLiteral("内部情况代码"),QVariant::String,100,0,200,true}};
    QgsFields fields;
    QJsonObject fieldGuide;
    for(const ResultColumn& column:resultColumns) {
        fields.append(QgsField(column.name,column.type,QString(),column.length,column.precision));
        fieldGuide[column.name]=column.alias;
    }
    for(auto mapIt=checkedLayersByMap.constBegin();mapIt!=checkedLayersByMap.constEnd();++mapIt) {
        const QString mapName=mapIt.key();
        const QDir root(QDir(outputRootDir()).filePath(mapName));
        const QString resultDir=root.filePath(QStringLiteral("质检结果"));
        const QString jsonDir=root.filePath(QStringLiteral("质检Json文件"));
        const QString logDir=root.filePath(QStringLiteral("质检日志"));
        if(!QDir().mkpath(resultDir) || !QDir().mkpath(jsonDir) || !QDir().mkpath(logDir)) {
            outputIssue(mapName,QStringLiteral("create_output_directory"),root.absolutePath(),QString(),QStringLiteral("无法创建结果、JSON 或日志目录"));
            continue;
        }
        const QString base=QDir(resultDir).filePath(checkName);
        const QStringList types{QStringLiteral("points"),QStringLiteral("lines"),QStringLiteral("polygons"),QStringLiteral("links")};
        for(const QString& type:types)
            for(const QString& ext:QStringList{QStringLiteral("shp"),QStringLiteral("shx"),QStringLiteral("dbf"),QStringLiteral("prj"),
                QStringLiteral("cpg"),QStringLiteral("qpj"),QStringLiteral("sbn"),QStringLiteral("sbx"),QStringLiteral("qml")}) {
                const QString path=base+QStringLiteral("_error_")+type+QStringLiteral(".")+ext;
                QFile oldFile(path);
                if(oldFile.exists() && !oldFile.remove())
                    outputIssue(mapName,QStringLiteral("remove_previous_output"),path,QString(),oldFile.errorString());
            }
        QVector<QJsonObject> mapRecords;
        QVector<FeatureOutput> outputs[3];
        for(const QJsonObject& record:records) {
            if(record.value(QStringLiteral("left_sheet")).toString()!=mapName && record.value(QStringLiteral("right_sheet")).toString()!=mapName) continue;
            mapRecords << record;
            const QString decision=CrossScaleSupport::normalizeFinding(record).value(QStringLiteral("decision_status")).toString();
            const bool confirmed=decision==QStringLiteral("confirmed_error");
            const bool review=decision==QStringLiteral("needs_review");
            if(!confirmed && !review) continue;
            for(const QString& side:QStringList{QStringLiteral("left"),QStringLiteral("right")}) {
                if(review && side!=QStringLiteral("right")) continue;
                if(record.value(side+QStringLiteral("_sheet")).toString()!=mapName) continue;
                if(confirmed && side==QStringLiteral("left") && !record.value(QStringLiteral("left_is_error")).toBool()
                    && !record.value(QStringLiteral("best_large_is_error")).toBool()) continue;
                if(confirmed && side==QStringLiteral("right") && record.contains(QStringLiteral("right_is_error")) && !record.value(QStringLiteral("right_is_error")).toBool()) continue;
                const QString path=record.value(side+QStringLiteral("_shp_path")).toString();
                bool fidOk=false;
                const qlonglong fid=record.value(side+QStringLiteral("_fid")).toString().toLongLong(&fidOk);
                if(path.isEmpty() || !fidOk) {
                    outputIssue(mapName,QStringLiteral("source_reference"),path,record.value(side+QStringLiteral("_fid")).toString(),QStringLiteral("缺少源路径或无法解析源 FID"));
                    continue;
                }
                QgsVectorLayer layer(path,QFileInfo(path).completeBaseName(),QStringLiteral("ogr"));
                if(!layer.isValid() || !layer.crs().isValid()) {
                    outputIssue(mapName,QStringLiteral("open_source_layer"),path,QString::number(fid),
                        layer.isValid()?QStringLiteral("源图层缺少有效坐标系"):QStringLiteral("无法打开源图层"));
                    continue;
                }
                QgsFeature feature;
                if(!layer.getFeatures(QgsFeatureRequest().setFilterFid(fid)).nextFeature(feature)) {
                    outputIssue(mapName,QStringLiteral("read_source_feature"),path,QString::number(fid),QStringLiteral("源图层中不存在此 FID"));
                    continue;
                }
                QgsGeometry geometry=feature.geometry();
                QgsCoordinateReferenceSystem crs=layer.crs();
                const QString checkedWkt=record.value(side==QStringLiteral("right")?QStringLiteral("checked_geometry_wkt"):QStringLiteral("left_checked_geometry_wkt")).toString();
                QStringList savedWkts;
                if(review && !record.value(QStringLiteral("uncovered_geometry_wkt")).toString().isEmpty())
                    savedWkts << record.value(QStringLiteral("uncovered_geometry_wkt")).toString();
                if(!checkedWkt.isEmpty()) savedWkts << checkedWkt;
                const QgsCoordinateReferenceSystem checkedCrs(record.value(QStringLiteral("checked_geometry_crs")).toString());
                bool usedSavedGeometry=false;
                if(checkedCrs.isValid()) for(const QString& wkt:savedWkts) {
                    const QgsGeometry saved=QgsGeometry::fromWkt(wkt);
                    if(saved.isNull() || saved.isEmpty()) continue;
                    geometry=saved; crs=checkedCrs; usedSavedGeometry=true; break;
                }
                if(!savedWkts.isEmpty() && !usedSavedGeometry)
                    outputIssue(mapName,QStringLiteral("saved_geometry_fallback"),path,QString::number(fid),
                        QStringLiteral("保存的检查几何或其坐标系无效，改用源要素几何及源坐标系；未给未知坐标的 WKT 指派猜测坐标系"),true);
                if(geometry.isNull() || geometry.isEmpty() || !crs.isValid()) {
                    outputIssue(mapName,QStringLiteral("output_geometry"),path,QString::number(fid),QStringLiteral("保存几何与源要素均无法提供有效几何和坐标系"));
                    continue;
                }
                const int type=static_cast<int>(geometry.type());
                if(type<0 || type>2) {
                    outputIssue(mapName,QStringLiteral("output_geometry_type"),path,QString::number(fid),QStringLiteral("输出不是点、线或面几何"));
                    continue;
                }
                outputs[type].push_back({record,geometry,crs,
                    side==QStringLiteral("left")?QStringLiteral("大比例尺"):QStringLiteral("小比例尺"),
                    mapName,QFileInfo(path).fileName(),QString::number(fid),decision});
            }
        }
        QJsonObject item=checkItem(mapRecords,execution(mapName,QString()),QString());
        QJsonObject info=item.value(QStringLiteral("info")).toObject();
        const QgsWkbTypes::Type wkbTypes[]{QgsWkbTypes::MultiPoint,QgsWkbTypes::MultiLineString,QgsWkbTypes::MultiPolygon};
        const QStringList countKeys{QStringLiteral("error_point_count"),QStringLiteral("error_line_count"),QStringLiteral("error_polygon_count")};
        const QStringList pathKeys{QStringLiteral("error_point_shp"),QStringLiteral("error_line_shp"),QStringLiteral("error_polygon_shp")};
        QString resultShp;
        int spatialReviewCount=0,spatialConfirmedCount=0;
        for(int type=0;type<3;++type) {
            const QString path=base+QStringLiteral("_error_")+types[type]+QStringLiteral(".shp");
            info[countKeys[type]]=0;
            info[pathKeys[type]]=QString();
            if(outputs[type].isEmpty()) continue;
            const QgsCoordinateReferenceSystem crs=outputs[type].first().crs;
            auto writer=std::make_unique<QgsVectorFileWriter>(path,QStringLiteral("UTF-8"),fields,wkbTypes[type],crs,QStringLiteral("ESRI Shapefile"));
            if(writer->hasError()!=QgsVectorFileWriter::NoError) {
                outputIssue(mapName,QStringLiteral("create_shapefile"),path,QString(),writer->errorMessage());
                continue;
            }
            int writtenCount=0;
            for(FeatureOutput& output:outputs[type]) {
                if(!CrossScaleSupport::project(output.geometry,output.crs,crs) || !output.geometry.convertToMultiType()) {
                    outputIssue(mapName,QStringLiteral("transform_output_geometry"),output.shp,output.fid,QStringLiteral("坐标转换或多部件几何转换失败"));
                    continue;
                }
                QgsFeature feature(fields); feature.setGeometry(output.geometry);
                const auto& record=output.record;
                QString smallFids=CrossScaleFindingPresentation::listText(record.value(QStringLiteral("small_source_fids")));
                if(smallFids.isEmpty()) smallFids=record.value(QStringLiteral("right_fid")).toString();
                QString referenceLayers=record.value(QStringLiteral("reference_candidate_layer")).toString();
                if(referenceLayers.isEmpty()) referenceLayers=CrossScaleFindingPresentation::listText(record.value(QStringLiteral("reference_confirmed_layers")));
                QString referenceFids=record.value(QStringLiteral("reference_candidate_fids")).toString();
                if(referenceFids.isEmpty()) referenceFids=QStringLiteral("未确定对应候选");
                QVariantMap values{{QStringLiteral("result"),record.value(QStringLiteral("decision_label")).toString()},
                    {QStringLiteral("issue"),record.value(QStringLiteral("finding_name")).toString()},
                    {QStringLiteral("sm_map"),record.value(QStringLiteral("right_sheet")).toString()},
                    {QStringLiteral("sm_scale"),record.value(QStringLiteral("small_scale_text")).toString()},
                    {QStringLiteral("sm_layer"),record.value(QStringLiteral("right_shp")).toString()},
                    {QStringLiteral("sm_fid"),smallFids},
                    {QStringLiteral("lg_map"),record.value(QStringLiteral("left_sheet")).toString()},
                    {QStringLiteral("lg_scale"),record.value(QStringLiteral("large_scale_text")).toString()},
                    {QStringLiteral("lg_layer"),referenceLayers},{QStringLiteral("lg_fid"),referenceFids},
                    {QStringLiteral("desc"),record.value(QStringLiteral("description")).toString()},
                    {QStringLiteral("advice"),record.value(QStringLiteral("review_advice")).toString()},
                    {QStringLiteral("liability"),record.value(QStringLiteral("responsibility_label")).toString()},
                    {QStringLiteral("mark_side"),QStringLiteral("%1 / %2 / FID %3").arg(output.sheet,output.shp,output.fid)},
                    {QStringLiteral("err_id"),record.value(QStringLiteral("err_id")).toString()},
                    {QStringLiteral("tol_model"),record.value(QStringLiteral("tolerance_model_label")).toString()},
                    {QStringLiteral("tol_src"),record.value(QStringLiteral("tolerance_source_label")).toString()},
                    {QStringLiteral("tol_class"),record.value(QStringLiteral("tolerance_business_class")).toString()},
                    {QStringLiteral("status"),output.status},{QStringLiteral("err_type"),record.value(QStringLiteral("error_type")).toString()}};
                const QMap<QString,QString> metrics{{QStringLiteral("cov_pct"),QStringLiteral("support_coverage_percent")},
                    {QStringLiteral("cov_min"),QStringLiteral("display_coverage_min_percent")},
                    {QStringLiteral("gap_m"),QStringLiteral("display_gap_m")},{QStringLiteral("gap_lim"),QStringLiteral("display_gap_limit_m")},
                    {QStringLiteral("dist_m"),QStringLiteral("display_candidate_distance_m")},{QStringLiteral("search_m"),QStringLiteral("display_search_radius_m")},
                    {QStringLiteral("sm_mm"),QStringLiteral("search_on_small_mm")},{QStringLiteral("lg_mm"),QStringLiteral("search_on_large_mm")},
                    {QStringLiteral("pos_lim"),QStringLiteral("display_position_tolerance_m")},{QStringLiteral("geom_tol"),QStringLiteral("display_geometry_tolerance_m")},
                    {QStringLiteral("search_ext"),QStringLiteral("display_expanded_search_m")},
                    {QStringLiteral("dir_cov"),QStringLiteral("display_direction_coverage_percent")},{QStringLiteral("dir_deg"),QStringLiteral("display_direction_max_deg")}};
                for(auto metric=metrics.constBegin();metric!=metrics.constEnd();++metric) {
                    const auto value=CrossScaleFindingPresentation::number(record,metric.value());
                    values[metric.key()]=value.isDouble()?QVariant(value.toDouble()):QVariant(QVariant::Double);
                }
                for(const ResultColumn& column:resultColumns) {
                    QVariant value=values.value(column.name);
                    if(column.type==QVariant::String) value=CrossScaleFindingPresentation::shapefileText(value.toString(),column.length);
                    feature.setAttribute(column.name,value);
                }
                if(!writer->addFeature(feature)) {
                    outputIssue(mapName,QStringLiteral("write_shapefile_feature"),path,output.fid,writer->lastError());
                    continue;
                }
                ++writtenCount;
                if(output.status==QStringLiteral("needs_review")) ++spatialReviewCount;
                else if(output.status==QStringLiteral("confirmed_error")) ++spatialConfirmedCount;
            }
            writer.reset(); // Close DBF/SHP before reopening the layer to save attribute-table aliases.
            if(writtenCount>0) {
                QgsVectorLayer readableLayer(path,checkName,QStringLiteral("ogr"));
                if(readableLayer.isValid()) {
                    for(const ResultColumn& column:resultColumns) {
                        const int fieldIndex=readableLayer.fields().indexFromName(column.name);
                        if(fieldIndex>=0) readableLayer.setFieldAlias(fieldIndex,column.alias);
                    }
                    auto config=readableLayer.attributeTableConfig();
                    config.update(readableLayer.fields());
                    auto columns=config.columns();
                    for(auto& column:columns) for(const ResultColumn& definition:resultColumns)
                        if(column.name==definition.name) { column.hidden=definition.hidden; column.width=definition.width; break; }
                    config.setColumns(columns);
                    readableLayer.setAttributeTableConfig(config);
                    bool saved=false;
                    const QString stylePath=QFileInfo(path).absolutePath()+QDir::separator()+QFileInfo(path).completeBaseName()+QStringLiteral(".qml");
                    const QString styleError=readableLayer.saveNamedStyle(stylePath,saved,QgsMapLayer::Fields|QgsMapLayer::AttributeTable);
                    if(!saved) outputIssue(mapName,QStringLiteral("write_attribute_style"),stylePath,QString(),styleError,true);
                } else outputIssue(mapName,QStringLiteral("read_attribute_style_layer"),path,QString(),QStringLiteral("无法重开结果图层保存中文字段别名"),true);
            }
            info[countKeys[type]]=writtenCount;
            if(writtenCount>0) {
                info[pathKeys[type]]=path;
                if(resultShp.isEmpty()) resultShp=path;
            }
        }
        info[QStringLiteral("needs_review_output_count")]=spatialReviewCount;
        info[QStringLiteral("result_field_guide")]=fieldGuide;
        info[QStringLiteral("result_schema_version")]=3;
        info[QStringLiteral("result_geometry_role")]=QStringLiteral("标记来源用于定位检查对象；待复核记录不据此确定错误归属。空指标表示未计算或不适用。");
        info[QStringLiteral("confirmed_error_output_count")]=spatialConfirmedCount;
        int outputFailureCount=0,outputWarningCount=0;
        for(const auto& value:outputDiagnostics.value(mapName)) {
            if(value.toObject().value(QStringLiteral("severity")).toString()==QStringLiteral("error")) ++outputFailureCount;
            else ++outputWarningCount;
        }
        info[QStringLiteral("output_failure_count")]=outputFailureCount;
        info[QStringLiteral("output_warning_count")]=outputWarningCount;
        info[QStringLiteral("output_diagnostics")]=outputDiagnostics.value(mapName);
        const QString logPath=QDir(logDir).filePath(checkName+QStringLiteral(".log"));
        info[QStringLiteral("error_link_count")]=0; info[QStringLiteral("error_link_shp")]=QString();
        info[QStringLiteral("result_shp")]=resultShp; info[QStringLiteral("log_path")]=logPath;
        item[QStringLiteral("info")]=info;
        const QString itemJsonPath=QDir(jsonDir).filePath(checkName+QStringLiteral(".json"));
        if(!writeJson(itemJsonPath,QJsonObject{{QStringLiteral("check_item1"),item}}))
            outputIssue(mapName,QStringLiteral("write_result_json"),itemJsonPath,QString(),jsonIoError);

        const QFileInfoList totals=root.entryInfoList({QStringLiteral("*_result_*.json")},QDir::Files,QDir::Time);
        const QString totalPath=totals.isEmpty()?root.filePath(QStringLiteral("%1_result_%2.json").arg(mapName,QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmss")))):totals.first().absoluteFilePath();
        QJsonObject total;
        QFile totalFile(totalPath);
        bool canWriteTotal=true;
        if(totalFile.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument document=QJsonDocument::fromJson(totalFile.readAll(),&parseError);
            if(parseError.error!=QJsonParseError::NoError || !document.isObject()) {
                outputIssue(mapName,QStringLiteral("read_total_json"),totalPath,QString(),
                    parseError.error!=QJsonParseError::NoError?parseError.errorString():QStringLiteral("汇总 JSON 顶层不是对象，已保留原文件"));
                canWriteTotal=false;
            } else total=document.object();
        } else if(QFile::exists(totalPath)) {
            outputIssue(mapName,QStringLiteral("read_total_json"),totalPath,QString(),totalFile.errorString());
            canWriteTotal=false;
        }
        totalFile.close();
        if(canWriteTotal) {
        QString geopdfKey;
        for(auto it=total.constBegin();it!=total.constEnd();++it)
            if(it.key().startsWith(QStringLiteral("Geopdf")) && it.value().toObject().value(QStringLiteral("name")).toString()==mapName) { geopdfKey=it.key(); break; }
        if(geopdfKey.isEmpty()) {
            geopdfKey=QStringLiteral("Geopdf");int index=2;
            while(total.contains(geopdfKey)) geopdfKey=QStringLiteral("Geopdf%1").arg(index++);
        }
        QJsonObject geopdf=total.value(geopdfKey).toObject();geopdf[QStringLiteral("name")]=mapName;
        QJsonObject tasks=geopdf.value(QStringLiteral("tasks")).toObject();
        for(const QString& layer:mapIt.value()) {
            QString taskKey;
            for(auto it=tasks.constBegin();it!=tasks.constEnd();++it)
                if(it.key().startsWith(QStringLiteral("task")) && it.value().toObject().value(QStringLiteral("layer_name")).toString()==layer) { taskKey=it.key();break; }
            if(taskKey.isEmpty()) { int index=1;do{taskKey=QStringLiteral("task%1").arg(index++);}while(tasks.contains(taskKey)); }
            QJsonObject task=tasks.value(taskKey).toObject();task[QStringLiteral("layer_name")]=layer;
            QString itemKey;
            for(auto it=task.constBegin();it!=task.constEnd();++it)
                if(it.key().startsWith(QStringLiteral("check_item")) && it.value().toObject().value(QStringLiteral("id")).toString()==kCrossScaleCheckItemId) { itemKey=it.key();break; }
            if(itemKey.isEmpty()) { int index=1;do{itemKey=QStringLiteral("check_item%1").arg(index++);}while(task.contains(itemKey)); }
            task[itemKey]=checkItem(mapRecords,execution(mapName,layer),layer);tasks[taskKey]=task;
        }
        int completed=0,errors=0,taskCount=0;
        for(auto it=tasks.constBegin();it!=tasks.constEnd();++it) {
            if(!it.key().startsWith(QStringLiteral("task")) || !it.value().isObject()) continue;
            ++taskCount;bool allCompleted=true,hasCheck=false,hasError=false;
            const auto task=it.value().toObject();
            for(auto ct=task.constBegin();ct!=task.constEnd();++ct) {
                if(!ct.key().startsWith(QStringLiteral("check_item")) || !ct.value().isObject()) continue;
                hasCheck=true;const auto check=ct.value().toObject();
                allCompleted=allCompleted && check.value(QStringLiteral("status")).toString()==QStringLiteral("质检成功");
                hasError=hasError || check.value(QStringLiteral("err_exist")).toBool();
            }
            if(hasCheck && allCompleted) ++completed;if(hasError) ++errors;
        }
        QJsonObject totalInfo=geopdf.value(QStringLiteral("info")).toObject();
        totalInfo[QStringLiteral("tasks_num")]=taskCount;totalInfo[QStringLiteral("completed_num")]=completed;
        totalInfo[QStringLiteral("err_num")]=errors;totalInfo[QStringLiteral("uncompleted_num")]=taskCount-completed;
        geopdf[QStringLiteral("info")]=totalInfo;geopdf[QStringLiteral("tasks")]=tasks;total[geopdfKey]=geopdf;
        totalFile.close();
        if(!writeJson(totalPath,total))
            outputIssue(mapName,QStringLiteral("write_total_json"),totalPath,QString(),jsonIoError);
        }
        outputFailureCount=0; outputWarningCount=0;
        for(const auto& value:outputDiagnostics.value(mapName)) {
            if(value.toObject().value(QStringLiteral("severity")).toString()==QStringLiteral("error")) ++outputFailureCount;
            else ++outputWarningCount;
        }
        QFile log(logPath);
        if(log.open(QIODevice::WriteOnly|QIODevice::Truncate)) {
            QTextStream out(&log);out.setCodec("UTF-8");
            for(const QString& line:m_runtimeLogLines) out << line << "\n";
            out << info.value(QStringLiteral("description")).toString() << "\n";
            out << QStringLiteral("实际写出待复核 %1 条，确认错误 %2 条；导出错误 %3 项，告警 %4 项。\n")
                .arg(spatialReviewCount).arg(spatialConfirmedCount).arg(outputFailureCount).arg(outputWarningCount);
            for(const auto& value:outputDiagnostics.value(mapName)) {
                const auto issue=value.toObject();
                out << "[" << issue.value(QStringLiteral("severity")).toString() << "]["
                    << issue.value(QStringLiteral("stage")).toString() << "] " << mapName
                    << " | " << issue.value(QStringLiteral("path")).toString()
                    << " | FID " << issue.value(QStringLiteral("fid")).toString()
                    << " | " << issue.value(QStringLiteral("reason")).toString() << "\n";
            }
            out.flush();
            if(out.status()!=QTextStream::Ok)
                outputIssue(mapName,QStringLiteral("write_output_log"),logPath,QString(),log.errorString());
        } else outputIssue(mapName,QStringLiteral("open_output_log"),logPath,QString(),log.errorString());
    }
    if(errorDetails && !ok) *errorDetails=failureMessages.mid(0,3).join(QChar('\n'));
    return ok;
}

void SeriesCrossScaleExpressionTaskDialog::startCheck()
{
    if (!m_xmlEdit || m_xmlEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择 XML 质检规则文件。"));
        return;
    }
    if (!m_configTree || m_configTree->topLevelItemCount() == 0) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先添加地图配置。"));
        return;
    }

    checkTableStatus();
    QVector<MapTask> tasks = buildConfiguredTasks();
    if (tasks.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请至少配置 1 组不同比例尺的主检查地图和关联地图。"));
        return;
    }

    m_runtimeLogLines.clear();
    if (m_progressBar) {
        m_progressBar->setValue(0);
        m_progressBar->setFormat(QStringLiteral("准备一期预处理成果..."));
    }
    if (!prepareTaskShpDirs(&tasks)) return;

    QMap<QString, QJsonObject> mappings;
    if (!confirmLayerMappings(tasks, &mappings)) {
        appendRuntimeLog(QStringLiteral("用户取消图层对应确认，已停止检查。"));
        return;
    }

    QVector<QJsonObject> results;
    int index = 0;
    for (const MapTask& task : tasks) {
        if (m_progressBar) {
            m_progressBar->setFormat(QStringLiteral("执行跨比例尺表达检查 (%1/%2)").arg(index + 1).arg(tasks.size()));
            m_progressBar->setValue(50 + index * 50 / qMax(1, tasks.size()));
        }
        QJsonObject result = runCrossScaleExpressionCheck(task, mappings.value(task.pairName));
        results.push_back(result);
        ++index;
    }

    QString outputError;
    if (!writeOutputs(results,&outputError)) {
        appendRuntimeLog(QStringLiteral("结果未完整写出：%1").arg(outputError));
        QMessageBox::warning(this, QStringLiteral("输出失败"),
            QStringLiteral("检查已执行，但部分结果写入失败。\n%1\n完整详情见各地图的质检日志（含失败阶段、源图层、FID 和原因）。").arg(outputError));
        return;
    }
    if (m_progressBar) {
        m_progressBar->setValue(100);
        m_progressBar->setFormat(QStringLiteral("检查完成"));
    }
    int completed=0,partial=0,failed=0,noApplicable=0,notEvaluable=0,notExecuted=0;
    for (const QJsonObject& result:results) {
        const QString state=result.value(QStringLiteral("execution_state")).toString();
        if(state.startsWith(QStringLiteral("invalid"))) ++failed;
        else if(state==QStringLiteral("partial_not_evaluable")) ++partial;
        else if(state==QStringLiteral("completed")) ++completed;
        else if(state==QStringLiteral("no_applicable_features")) ++noApplicable;
        else if(state==QStringLiteral("not_evaluable")) ++notEvaluable;
        else ++notExecuted;
    }
    QMessageBox::information(this, QStringLiteral("同系列跨比例尺表达检查完成"),
        QStringLiteral("已处理 %1 组：完成 %2 组，部分完成 %3 组，失败 %4 组，无适用对象 %5 组，无法判定 %6 组，未执行 %7 组。待复核结果已单独保存。")
            .arg(results.size()).arg(completed).arg(partial).arg(failed).arg(noApplicable).arg(notEvaluable).arg(notExecuted));
}


