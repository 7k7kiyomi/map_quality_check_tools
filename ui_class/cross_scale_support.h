#pragma once

// Geometry and decision contracts shared by the independent 502 execution/export path.
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <qgscoordinatetransform.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace CrossScaleSupport {
struct Scope {
    QgsGeometry geometry;
    QgsCoordinateReferenceSystem crs;
    QString error;
};

inline QString key(QString value)
{
    value = value.normalized(QString::NormalizationForm_KC).trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[\\s_\\-]")));
    return value;
}

inline bool project(QgsGeometry& geometry, const QgsCoordinateReferenceSystem& source,
                    const QgsCoordinateReferenceSystem& target)
{
    if (!source.isValid() || !target.isValid()) return false;
    if (source == target) return true;
    try {
        return geometry.transform(QgsCoordinateTransform(source, target, QgsProject::instance()))
            == Qgis::GeometryOperationResult::Success;
    } catch (const QgsCsException&) { return false; }
}

inline QgsGeometry dimension(QgsGeometry geometry, QgsWkbTypes::GeometryType type)
{
    if (!geometry.lastError().isEmpty()) return geometry;
    geometry.convertGeometryCollectionToSubclass(type);
    if (geometry.isNull() || geometry.isEmpty() || geometry.type() != type) return QgsGeometry();
    if (type == QgsWkbTypes::LineGeometry && geometry.length() <= 1e-8) return QgsGeometry();
    if (type == QgsWkbTypes::PolygonGeometry && geometry.area() <= 1e-8) return QgsGeometry();
    return geometry;
}

inline QgsGeometry clipped(const QgsGeometry& geometry, const QgsGeometry& scope)
{
    return dimension(geometry.intersection(scope), geometry.type());
}

inline int pointCount(const QgsGeometry& geometry)
{
    if(geometry.isNull() || geometry.isEmpty()) return 0;
    return geometry.isMultipart()?geometry.asMultiPoint().size():1;
}

inline QgsGeometry framePolygon(const QgsGeometry& geometry)
{
    if (geometry.type() == QgsWkbTypes::PolygonGeometry) return geometry;
    if (geometry.type() != QgsWkbTypes::LineGeometry) return QgsGeometry();
    const QgsMultiPolylineXY lines = geometry.isMultipart()
        ? geometry.asMultiPolyline() : QgsMultiPolylineXY{geometry.asPolyline()};
    QgsGeometry result;
    for (const QgsPolylineXY& line : lines) {
        if (line.size() < 4 || line.first() != line.last()) return QgsGeometry();
        const QgsGeometry polygon = QgsGeometry::fromPolygonXY(QgsPolygonXY{line});
        if (!polygon.isGeosValid() || polygon.area() <= 1e-8) return QgsGeometry();
        result = result.isNull() ? polygon : result.combine(polygon);
    }
    return result;
}

inline Scope frames(const QString& directories, QgsCoordinateReferenceSystem target = QgsCoordinateReferenceSystem())
{
    Scope result;
    const QStringList dirs = directories.split(QStringLiteral(";"), Qt::SkipEmptyParts);
    const QStringList names{QStringLiteral("内图廓"),QStringLiteral("图廓"),QStringLiteral("图框"),QStringLiteral("图幅范围")};
    for (const QString& directory : dirs) {
        QDir dir(directory.trimmed());
        QStringList selected;
        for (const QString& preferred : names) {
            for (const QString& name : dir.entryList({QStringLiteral("*.shp"),QStringLiteral("*.SHP")}, QDir::Files))
                if (key(QFileInfo(name).completeBaseName()) == key(preferred)) selected << dir.filePath(name);
            if (!selected.isEmpty()) break;
        }
        if (selected.isEmpty()) { result.error = QStringLiteral("缺少真实图廓：%1").arg(directory); return result; }
        QgsGeometry oneSheet;
        for (const QString& path : selected) {
            QgsVectorLayer layer(path, QFileInfo(path).completeBaseName(), QStringLiteral("ogr"));
            if (!layer.isValid() || !layer.crs().isValid()) {
                result.error = QStringLiteral("图廓无法读取或缺少坐标系：%1").arg(path); return result;
            }
            QgsFeature feature;
            auto it = layer.getFeatures();
            while (it.nextFeature(feature)) {
                QgsGeometry polygon = framePolygon(feature.geometry());
                if (polygon.isNull() || polygon.isEmpty() || !polygon.isGeosValid()) {
                    result.error = QStringLiteral("图廓不是有效闭合面：%1").arg(path); return result;
                }
                if (!target.isValid()) {
                    target = layer.crs();
                    if (target.mapUnits() != QgsUnitTypes::DistanceMeters || target.isGeographic()) {
                        QgsGeometry center = polygon.centroid();
                        if (!project(center, layer.crs(), QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")))) {
                            result.error = QStringLiteral("图廓中心坐标转换失败：%1").arg(path); return result;
                        }
                        const QgsPointXY point = center.asPoint();
                        target = QgsCoordinateReferenceSystem::fromProj(QStringLiteral(
                            "+proj=aeqd +lat_0=%1 +lon_0=%2 +datum=WGS84 +units=m +no_defs")
                            .arg(point.y(),0,'f',10).arg(point.x(),0,'f',10));
                    }
                }
                if (!project(polygon, layer.crs(), target)) {
                    result.error = QStringLiteral("图廓坐标转换失败：%1").arg(path); return result;
                }
                oneSheet = oneSheet.isNull() ? polygon : oneSheet.combine(polygon);
            }
        }
        if (oneSheet.isNull() || oneSheet.isEmpty() || !oneSheet.isGeosValid() || oneSheet.area() <= 1e-8) {
            result.error = QStringLiteral("图廓为空或合并失败：%1").arg(directory); return result;
        }
        result.geometry = result.geometry.isNull() ? oneSheet : result.geometry.combine(oneSheet);
    }
    result.crs = target;
    if (result.geometry.isNull() || result.geometry.isEmpty() || !result.geometry.isGeosValid() || result.geometry.area() <= 1e-8)
        result.error = QStringLiteral("没有有效的真实图廓覆盖面");
    return result;
}

inline QString fieldValue(const QgsFeature& feature, const QStringList& names)
{
    for (const QString& name : names)
        for (int i = 0; i < feature.fields().size(); ++i)
            if (feature.fields().at(i).name().compare(name,Qt::CaseInsensitive) == 0) {
                // Stable identifiers keep meaningful punctuation; layer-name normalization is more permissive.
                const QString value = feature.attribute(i).toString().normalized(QString::NormalizationForm_KC).trimmed().toCaseFolded();
                if (!value.isEmpty() && value != QStringLiteral("null")) return value;
            }
    return QString();
}

inline QString symbolInstanceKey(const QgsFeature& feature)
{
    const QString symbolId=fieldValue(feature,{QStringLiteral("symbol_id")});
    const QString symbolCenter=fieldValue(feature,{QStringLiteral("symbol_cen")});
    if (symbolId.isEmpty() || symbolCenter.isEmpty()) return QString();
    return symbolId+QChar('|')+symbolCenter;
}

inline QString normalizeAnnotationText(QString value)
{
    value = value.normalized(QString::NormalizationForm_KC).trimmed().toCaseFolded();
    value.remove(QRegularExpression(QStringLiteral("[\\s\\p{Z}]+")));
    return value;
}

inline QString annotationNameCore(QString value)
{
    value=normalizeAnnotationText(value);
    static const QStringList suffixes{QStringLiteral("镇"),QStringLiteral("乡"),
        QStringLiteral("村"),QStringLiteral("县"),QStringLiteral("区"),QStringLiteral("市"),
        QStringLiteral("旗"),QStringLiteral("苏木")};
    for (const QString& suffix:suffixes) {
        if (value.endsWith(suffix) && value.size()>suffix.size()+1) {
            value.chop(suffix.size());
            break;
        }
    }
    return value;
}

inline QStringList featureNameKeys(const QgsFeature& feature)
{
    static const QStringList names{QStringLiteral("NAME"),QStringLiteral("name"),
        QStringLiteral("名称"),QStringLiteral("地名"),QStringLiteral("地物名"),
        QStringLiteral("要素名称"),QStringLiteral("道路名称"),QStringLiteral("河流名称"),
        QStringLiteral("居民地名称"),QStringLiteral("text_conte"),
        QStringLiteral("text_content"),QStringLiteral("text")};
    QStringList result;
    for (const QString& name:names) {
        for (int i=0;i<feature.fields().size();++i) {
            if (feature.fields().at(i).name().compare(name,Qt::CaseInsensitive)!=0) continue;
            const QString value=normalizeAnnotationText(feature.attribute(i).toString());
            if (!value.isEmpty() && !result.contains(value)) result << value;
        }
    }
    return result;
}

inline QString annotationText(const QgsFeature& feature)
{
    const QStringList names{QStringLiteral("text_conte"),QStringLiteral("text_content"),
        QStringLiteral("text"),QStringLiteral("name"),QStringLiteral("NAME"),QStringLiteral("\u540d\u79f0")};
    for (const QString& name : names) {
        for (int i = 0; i < feature.fields().size(); ++i) {
            if (feature.fields().at(i).name().compare(name,Qt::CaseInsensitive) != 0) continue;
            const QString value = feature.attribute(i).toString();
            if (!normalizeAnnotationText(value).isEmpty()) return value.trimmed();
        }
    }
    return QString();
}

inline bool isNonTextAnnotationGraphic(const QgsFeature& feature)
{
    if (!annotationText(feature).isEmpty()) return false;
    const QString elementType = normalizeAnnotationText(fieldValue(
        feature,{QStringLiteral("element_ty"),QStringLiteral("element_type")}));
    static const QStringList nonTextGraphicTypes{
        QStringLiteral("path"),QStringLiteral("image"),QStringLiteral("shading"),
        QStringLiteral("form"),QStringLiteral("clippath")};
    return nonTextGraphicTypes.contains(elementType);
}

enum class AnnotationRelationState { Matched, MissingText, NoTextMatch, NoSpatialMatch, Ambiguous, InvalidGeometry };

struct AnnotationCandidate
{
    QString text;
    QgsGeometry geometry;
    QString path;
    QStringList sourceFids;
};

struct AnnotationRelation
{
    AnnotationRelationState state = AnnotationRelationState::NoTextMatch;
    int candidateCount = 0;
    int spatialCandidateCount = 0;
    int bestCandidateIndex = -1;
    int nearestTextCandidateIndex = -1;
    double nearestTextDistance = -1.0;
    double bestDistance = -1.0;
    double runnerUpDistance = -1.0;
    double bestOverlapRatio = 0.0;
    double bestRelationScore = 0.0;
    double runnerUpRelationScore = -1.0;
    bool matchedByUniqueText = false;
};

inline double annotationBoxOverlapRatio(const QgsGeometry& first,const QgsGeometry& second)
{
    if (first.isNull() || first.isEmpty() || second.isNull() || second.isEmpty()) return 0.0;
    const QgsRectangle a=first.boundingBox();
    const QgsRectangle b=second.boundingBox();
    if (a.isNull() || b.isNull() || !a.intersects(b)) return 0.0;
    const double width=qMax(0.0,qMin(a.xMaximum(),b.xMaximum())-qMax(a.xMinimum(),b.xMinimum()));
    const double height=qMax(0.0,qMin(a.yMaximum(),b.yMaximum())-qMax(a.yMinimum(),b.yMinimum()));
    const double overlapArea=width*height;
    const double minArea=qMin(a.width()*a.height(),b.width()*b.height());
    double boxRatio=minArea>1e-8?overlapArea/minArea:0.0;
    if (first.type()==QgsWkbTypes::PolygonGeometry || second.type()==QgsWkbTypes::PolygonGeometry) {
        const QgsGeometry intersection=first.intersection(second);
        const double minGeometryArea=qMin(first.area(),second.area());
        if (!intersection.isNull() && !intersection.isEmpty() && minGeometryArea>1e-8)
            boxRatio=qMax(boxRatio,intersection.area()/minGeometryArea);
    } else if (first.type()==QgsWkbTypes::LineGeometry || second.type()==QgsWkbTypes::LineGeometry) {
        const QgsGeometry intersection=first.intersection(second);
        const double minGeometryLength=qMin(first.length(),second.length());
        if (!intersection.isNull() && !intersection.isEmpty() && minGeometryLength>1e-8)
            boxRatio=qMax(boxRatio,intersection.length()/minGeometryLength);
        if (boxRatio<=0.0 && first.distance(second)<=1e-8) boxRatio=0.25;
    } else if (first.distance(second)<=1e-8) {
        boxRatio=1.0;
    }
    return qBound(0.0,boxRatio,1.0);
}

inline AnnotationRelation resolveAnnotationCorrespondence(const QString& smallText,
    const QgsGeometry& smallGeometry, const QVector<AnnotationCandidate>& candidates,
    double tolerance, double uniqueScoreMargin = 0.08, bool smallTextUnique = true)
{
    AnnotationRelation result;
    const QString expectedText = normalizeAnnotationText(smallText);
    if (expectedText.isEmpty()) {
        result.state = AnnotationRelationState::MissingText;
        return result;
    }
    if (smallGeometry.isNull() || smallGeometry.isEmpty() || !std::isfinite(tolerance) || tolerance < 0) {
        result.state = AnnotationRelationState::InvalidGeometry;
        return result;
    }
    const QgsGeometry smallAnchor = smallGeometry.centroid();
    if (smallAnchor.isNull() || smallAnchor.isEmpty() || !smallAnchor.lastError().isEmpty()) {
        result.state = AnnotationRelationState::InvalidGeometry;
        return result;
    }

    struct NearbyCandidate { int index; double distance; double overlapRatio; double relationScore; };
    QVector<NearbyCandidate> nearby;
    bool matchingCandidateHadInvalidGeometry = false;
    for (int i = 0; i < candidates.size(); ++i) {
        if (normalizeAnnotationText(candidates.at(i).text) != expectedText) continue;
        ++result.candidateCount;
        const QgsGeometry& geometry = candidates.at(i).geometry;
        if (geometry.isNull() || geometry.isEmpty()) {
            matchingCandidateHadInvalidGeometry = true;
            continue;
        }
        const QgsGeometry anchor = geometry.centroid();
        if (anchor.isNull() || anchor.isEmpty() || !anchor.lastError().isEmpty()) {
            matchingCandidateHadInvalidGeometry = true;
            continue;
        }
        const double distance = smallAnchor.distance(anchor);
        if (!std::isfinite(distance) || distance < 0) {
            matchingCandidateHadInvalidGeometry = true;
            continue;
        }
        const double overlapRatio=annotationBoxOverlapRatio(smallGeometry,geometry);
        if (result.nearestTextDistance < 0 || distance < result.nearestTextDistance) {
            result.nearestTextDistance = distance;
            result.nearestTextCandidateIndex = i;
        }
        if (overlapRatio>0.0 || distance <= tolerance) {
            const double proximityScore=qBound(0.0,1.0-distance/qMax(0.001,tolerance),1.0);
            nearby.push_back({i,distance,overlapRatio,0.70*overlapRatio+0.30*proximityScore});
        }
    }
    if (result.candidateCount == 0) {
        result.state = AnnotationRelationState::NoTextMatch;
        return result;
    }
    if (result.candidateCount==1 && smallTextUnique) {
        if (result.nearestTextCandidateIndex<0) {
            result.state=AnnotationRelationState::InvalidGeometry;
            return result;
        }
        result.state=AnnotationRelationState::Matched;
        result.bestCandidateIndex=result.nearestTextCandidateIndex;
        result.bestDistance=result.nearestTextDistance;
        result.spatialCandidateCount=nearby.size();
        result.matchedByUniqueText=true;
        const auto it=std::find_if(nearby.cbegin(),nearby.cend(),[&](const NearbyCandidate& candidate) {
            return candidate.index==result.bestCandidateIndex;
        });
        if (it!=nearby.cend()) {
            result.bestOverlapRatio=it->overlapRatio;
            result.bestRelationScore=it->relationScore;
        }
        return result;
    }
    if (nearby.isEmpty()) {
        result.state = matchingCandidateHadInvalidGeometry
            ? AnnotationRelationState::InvalidGeometry : AnnotationRelationState::Ambiguous;
        return result;
    }
    std::sort(nearby.begin(),nearby.end(),[](const NearbyCandidate& a,const NearbyCandidate& b) {
        if (!qFuzzyCompare(a.relationScore+1.0,b.relationScore+1.0)) return a.relationScore>b.relationScore;
        if (!qFuzzyCompare(a.distance+1.0,b.distance+1.0)) return a.distance<b.distance;
        return a.index<b.index;
    });
    result.spatialCandidateCount = nearby.size();
    result.bestCandidateIndex = nearby.first().index;
    result.bestDistance = nearby.first().distance;
    if (nearby.size() > 1) result.runnerUpDistance = nearby.at(1).distance;
    result.bestOverlapRatio=nearby.first().overlapRatio;
    result.bestRelationScore=nearby.first().relationScore;
    if (nearby.size()>1) result.runnerUpRelationScore=nearby.at(1).relationScore;
    const double margin = qBound(0.0,uniqueScoreMargin,1.0);
    const double scoreMargin=result.bestRelationScore-result.runnerUpRelationScore;
    const bool unique = nearby.size()==1 || scoreMargin>=margin
        || (result.runnerUpDistance-result.bestDistance>=qMax(1.0,tolerance*.35)
            && result.bestDistance<=tolerance*.5 && result.bestOverlapRatio>=nearby.at(1).overlapRatio);
    result.state = unique ? AnnotationRelationState::Matched : AnnotationRelationState::Ambiguous;
    return result;
}

inline QString identity(const QgsFeature& feature, const QStringList& configuredFields = QStringList())
{
    QStringList fields = configuredFields;
    fields << QStringLiteral("CODE") << QStringLiteral("要素编码");
    fields.removeDuplicates();
    for (const QString& field : fields) {
        const QString value = fieldValue(feature, {field});
        if (value.isEmpty()) continue;
        const QString fieldKey = key(field);
        QString role = QStringLiteral("field:") + fieldKey;
        if (fieldKey == QStringLiteral("code") || field == QStringLiteral("要素编码")) role = QStringLiteral("code");
        else if (fieldKey == QStringLiteral("name") || field == QStringLiteral("名称")) role = QStringLiteral("name");
        else if (fieldKey == QStringLiteral("textconte") || fieldKey == QStringLiteral("textcontent") || fieldKey == QStringLiteral("text")) role = QStringLiteral("text");
        return role + QChar(':') + value;
    }
    return QString();
}

inline bool identitiesConflict(const QString& first,const QString& second)
{
    if (first.isEmpty() || second.isEmpty()) return false;
    if (first==second) return false;
    const int firstSeparator=first.indexOf(QChar(':'));
    const int secondSeparator=second.indexOf(QChar(':'));
    if (firstSeparator<0 || secondSeparator<0) return true;
    const QString firstRole=first.left(firstSeparator);
    const QString secondRole=second.left(secondSeparator);
    return firstRole==secondRole;
}

inline QString boundaryClass(const QString& path, const QgsFeature& feature)
{
    QString value = key(QFileInfo(path).completeBaseName());
    value += fieldValue(feature,{QStringLiteral("行政等级"),QStringLiteral("界线等级"),QStringLiteral("boundary_level")});
    if (!value.contains(QStringLiteral("界")) && !value.contains(QStringLiteral("boundary"))) return QString();
    for (const auto& entry : QVector<QPair<QString,QString>>{
        {QStringLiteral("乡镇"),QStringLiteral("township")},{QStringLiteral("县"),QStringLiteral("county")},
        {QStringLiteral("市"),QStringLiteral("city")},{QStringLiteral("省"),QStringLiteral("province")},
        {QStringLiteral("国界"),QStringLiteral("national")}})
        if (value.contains(entry.first)) return entry.second;
    return QString();
}

inline QString businessClass(const QString& path, const QgsFeature& feature,
                             const QMap<QString, QString>& semanticAliases = QMap<QString, QString>())
{
    QString layerName = QFileInfo(path).completeBaseName();
    layerName.remove(QRegularExpression(QStringLiteral("_(Point|LineString|Polygon|MultiPoint|MultiLineString|MultiPolygon)$"),
                                        QRegularExpression::CaseInsensitiveOption));
    const QString normalizedName = key(layerName);
    const QString configuredClass = semanticAliases.value(normalizedName);
    if (!configuredClass.isEmpty()) return key(configuredClass);

    for (const QString& fieldName : {QStringLiteral("业务类别"), QStringLiteral("要素类别"),
                                     QStringLiteral("地物类别"), QStringLiteral("要素类型"),
                                     QStringLiteral("feature_class"), QStringLiteral("feature_type")}) {
        const QString value = fieldValue(feature, {fieldName});
        if (!value.isEmpty()) {
            const QString mapped = semanticAliases.value(key(value));
            if (!mapped.isEmpty()) return key(mapped);
        }
    }

    const QString boundary=boundaryClass(path,feature);
    if(!boundary.isEmpty()) return QStringLiteral("boundary:")+boundary;
    const QString name=key(layerName);
    const QVector<QPair<QString,QStringList>> classes{
        {QStringLiteral("road_facility"),{QStringLiteral("公路隧道口"),QStringLiteral("公路隧道")}},
        {QStringLiteral("road"),{QStringLiteral("高速公路"),QStringLiteral("快速路"),QStringLiteral("国道"),QStringLiteral("省道"),
            QStringLiteral("县道"),QStringLiteral("乡道"),QStringLiteral("村道"),QStringLiteral("专用公路"),QStringLiteral("连接道")}},
        {QStringLiteral("hydrology_area"),{QStringLiteral("河流面"),QStringLiteral("水域面"),QStringLiteral("湖泊"),QStringLiteral("水库"),QStringLiteral("坑塘")}},
        {QStringLiteral("hydrology"),{QStringLiteral("总干渠"),QStringLiteral("干渠"),QStringLiteral("支渠"),QStringLiteral("常年河"),QStringLiteral("干涸河"),
            QStringLiteral("一级河流"),QStringLiteral("二级河流"),QStringLiteral("三级河流"),QStringLiteral("四级河流"),QStringLiteral("五级河流"),
            QStringLiteral("六级河流"),QStringLiteral("时令河"),QStringLiteral("等外河流"),QStringLiteral("沟渠")}},
        {QStringLiteral("seat"),{QStringLiteral("乡镇级驻地"),QStringLiteral("县级驻地"),QStringLiteral("市级驻地"),QStringLiteral("省级驻地")}},
        {QStringLiteral("admin_area"),{QStringLiteral("乡镇级行政区面"),QStringLiteral("县级行政区面")}},
        {QStringLiteral("settlement"),{QStringLiteral("自然村"),QStringLiteral("行政村"),QStringLiteral("村庄"),QStringLiteral("居民地")}},
        {QStringLiteral("railway"),{QStringLiteral("高速铁路"),QStringLiteral("普通铁路"),QStringLiteral("铁路")}},
        {QStringLiteral("water_point"),{QStringLiteral("井"),QStringLiteral("泉"),QStringLiteral("水文站")}},
        {QStringLiteral("boundary"),{QStringLiteral("行政区界线"),QStringLiteral("行政界线")}}
    };
    for(const auto& entry:classes)
        for(const QString& token:entry.second)
            if(name.contains(token)) return entry.first+QStringLiteral(":")+token;
    return QString();
}

inline QString businessClassForConfirmedTarget(const QString& path, const QgsFeature& feature,
    const QString& target, const QMap<QString,QString>& semanticAliases, bool mappingConfirmed)
{
    if (mappingConfirmed) {
        const QString targetClass = businessClass(target,feature,semanticAliases);
        if (!targetClass.isEmpty()) return targetClass;
    }
    return businessClass(path,feature,semanticAliases);
}

inline double longestPart(const QgsGeometry& geometry)
{
    if (geometry.isNull() || geometry.isEmpty()) return 0;
    if (!geometry.isMultipart()) return geometry.length();
    double length = 0;
    for (const QgsGeometry& part : geometry.asGeometryCollection()) length = qMax(length,part.length());
    return length;
}

// Length-weighted local tangents prevent a wide distance buffer from accepting a crossing line.
// Compare unoriented tangents so reversing digitization does not affect correspondence.
inline double localDirectionCoverage(const QgsGeometry& source,const QgsGeometry& target,double tolerance,double maxAngleDeg)
{
    if(source.type()!=QgsWkbTypes::LineGeometry || target.type()!=QgsWkbTypes::LineGeometry)
        return std::numeric_limits<double>::quiet_NaN();
    const auto parts=source.isMultipart()?source.asMultiPolyline():QgsMultiPolylineXY{source.asPolyline()};
    const double step=qMax(1.0,tolerance*.5),cosLimit=std::cos(maxAngleDeg*3.14159265358979323846/180.0);
    double total=0,aligned=0;
    for(const auto& part:parts) for(int i=1;i<part.size();++i) {
        const double dx=part.at(i).x()-part.at(i-1).x(),dy=part.at(i).y()-part.at(i-1).y();
        const double length=std::hypot(dx,dy);
        if(length<=1e-8) continue;
        const double sampleCount=std::ceil(length/step);
        // An unavailable assessment must stay reviewable rather than silently subsampling.
        if(!std::isfinite(sampleCount) || sampleCount>100000) return std::numeric_limits<double>::quiet_NaN();
        const int samples=qMax(1,static_cast<int>(sampleCount));
        total+=length;
        for(int sample=0;sample<samples;++sample) {
            const double fraction=(sample+.5)/samples;
            const QgsPointXY point(part.at(i-1).x()+fraction*dx,part.at(i-1).y()+fraction*dy);
            QgsPointXY closest;int next=-1;
            const double distanceSquared=target.closestSegmentWithContext(point,closest,next);
            if(next<1 || !std::isfinite(distanceSquared) || distanceSquared<0 || distanceSquared>tolerance*tolerance*(1+1e-9)) continue;
            const auto first=target.vertexAt(next-1),second=target.vertexAt(next);
            const double tx=second.x()-first.x(),ty=second.y()-first.y(),targetLength=std::hypot(tx,ty);
            if(targetLength<=1e-8) continue;
            if(std::fabs(dx*tx+dy*ty)/(length*targetLength)+1e-12>=cosLimit) aligned+=length/samples;
        }
    }
    return total>0?qBound(0.0,aligned/total,1.0):0;
}

inline bool isAnnotationPath(const QString& path)
{
    const QString name=QFileInfo(path).completeBaseName().toCaseFolded();
    return name.contains(QStringLiteral("标注")) || name.contains(QStringLiteral("注记"))
        || name.endsWith(QStringLiteral("_label")) || name.endsWith(QStringLiteral("_text"));
}
inline QJsonObject normalizeFinding(QJsonObject record)
{
    const QString type = record.value(QStringLiteral("error_type")).toString();
    const bool notEvaluable = type == QStringLiteral("cross_scale_rule_config_error")
        || type == QStringLiteral("cross_scale_not_evaluable") || type == QStringLiteral("missing_large_scale_layer")
        || type == QStringLiteral("cross_scale_business_class_not_evaluable")
        || type == QStringLiteral("cross_scale_annotation_correspondence_not_evaluable")
        || type == QStringLiteral("cross_scale_feature_not_evaluable");
    const bool review = type == QStringLiteral("cross_scale_out_of_range")
        || type == QStringLiteral("cross_scale_suspect_correspondence") || type == QStringLiteral("cross_scale_unmatched_feature")
        || type == QStringLiteral("cross_scale_weak_support") || type == QStringLiteral("cross_scale_candidate_ambiguous")
        || type == QStringLiteral("cross_scale_support_missing")
        || type == QStringLiteral("cross_scale_candidate_outside_position_tolerance")
        || type == QStringLiteral("cross_scale_annotation_text_no_match")
        || type == QStringLiteral("cross_scale_annotation_spatial_no_match")
        || type == QStringLiteral("cross_scale_annotation_text_variant");
    const QString currentStatus=record.value(QStringLiteral("decision_status")).toString();
    if (notEvaluable) record[QStringLiteral("decision_status")] = QStringLiteral("not_evaluable");
    else if ((review && currentStatus!=QStringLiteral("confirmed_error")) || currentStatus.isEmpty())
        record[QStringLiteral("decision_status")] = QStringLiteral("needs_review");
    if (record.value(QStringLiteral("decision_status")).toString() != QStringLiteral("confirmed_error"))
        record[QStringLiteral("final_state")] = record.value(QStringLiteral("decision_status"));
    return record;
}

inline QString mergeExecutionState(const QString& a,const QString& b)
{
    const bool aInvalid=a.startsWith(QStringLiteral("invalid"));
    const bool bInvalid=b.startsWith(QStringLiteral("invalid"));
    if (aInvalid && bInvalid) return a;
    if ((aInvalid && (b==QStringLiteral("completed") || b==QStringLiteral("not_evaluable")
            || b==QStringLiteral("partial_not_evaluable")))
        || (bInvalid && (a==QStringLiteral("completed") || a==QStringLiteral("not_evaluable")
            || a==QStringLiteral("partial_not_evaluable")))) return QStringLiteral("partial_not_evaluable");
    if (aInvalid) return a;
    if (bInvalid) return b;
    if (a==QStringLiteral("partial_not_evaluable") || b==QStringLiteral("partial_not_evaluable")) return QStringLiteral("partial_not_evaluable");
    if ((a==QStringLiteral("completed") && b==QStringLiteral("not_evaluable"))
        || (b==QStringLiteral("completed") && a==QStringLiteral("not_evaluable"))) return QStringLiteral("partial_not_evaluable");
    if (a==QStringLiteral("not_evaluable") || b==QStringLiteral("not_evaluable")) return QStringLiteral("not_evaluable");
    if (a==QStringLiteral("completed") || b==QStringLiteral("completed")) return QStringLiteral("completed");
    return b;
}
inline QString executionStatus(const QString& state)
{
    if (state.startsWith(QStringLiteral("invalid"))) return QStringLiteral("质检失败");
    if (state==QStringLiteral("partial_not_evaluable")) return QStringLiteral("质检部分完成");
    if (state==QStringLiteral("not_evaluable")) return QStringLiteral("无法判定");
    if (state==QStringLiteral("no_applicable_features")) return QStringLiteral("无适用对象");
    return state == QStringLiteral("completed") ? QStringLiteral("质检成功") : QStringLiteral("未执行");
}
} // namespace CrossScaleSupport
