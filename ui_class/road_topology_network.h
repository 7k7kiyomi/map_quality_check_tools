#pragma once
#include "ReadShp.h"
#include <qgsspatialindex.h>
#include <qgscoordinatetransform.h>
#include <qgsproject.h>
#include <qgspolygon.h>
#include <QDir>
#include <QFileInfo>
#include <QTextEdit>
#include <memory>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <array>
#include <map>
#include <qgsmultilinestring.h>

// Result-map topology only. Database linework never acts as an edge.
namespace RoadTopology {
enum Family { None, Road, Railway, Wall };
inline QString baseName(QString name) {
    name=QFileInfo(name).completeBaseName();
    for(const QString& suffix:{QStringLiteral("_MultiLineString"),QStringLiteral("_LineString"),
        QStringLiteral("Line_String"),QStringLiteral("_Polygon"),QStringLiteral("_Point")})
        if(name.endsWith(suffix,Qt::CaseInsensitive)){name.chop(suffix.size());break;}
    return name;
}
inline Family family(const QString& name) {
    const QString n=baseName(name);
    if(n.contains(QStringLiteral("标注"))||n.contains(QStringLiteral("隧道口")))return None;
    if(n.contains(QStringLiteral("铁路")))return Railway;
    if(n==QStringLiteral("长城"))return Wall;
    const QStringList names{QStringLiteral("高速公路"),QStringLiteral("在建高速公路"),
        QStringLiteral("国道"),QStringLiteral("省道"),QStringLiteral("县道"),QStringLiteral("乡道"),
        QStringLiteral("村道"),QStringLiteral("快速路"),QStringLiteral("连接道"),
        QStringLiteral("专用道"),QStringLiteral("专用公路"),QStringLiteral("专用道路"),
        QStringLiteral("公路隧道"),QStringLiteral("隧道")};
    // Street lines are outside the current road topology scope.
    return names.contains(n)?Road:None;
}
inline QString identity(QgsVectorLayer* layer) {
    const QString path=QFileInfo(layer->source()).canonicalFilePath();
    return path.isEmpty()?layer->id():QDir::fromNativeSeparators(path).toCaseFolded();
}
struct Part {
    QString source;
    QgsFeature feature;
    int number=0;
    QgsGeometry geometry,original;
    QgsPointXY ends[2];
    bool closed=false;
    bool backbone=false;
};
class Components {
    std::vector<int> parents;
public:
    explicit Components(int count):parents(count){std::iota(parents.begin(),parents.end(),0);}
    int find(int id){return parents[id]==id?id:parents[id]=find(parents[id]);}
    void join(int a,int b){a=find(a);b=find(b);if(a!=b)parents[b]=a;}
};
inline double buffer(ReadShpDialog& dlg,const QString& key,double defaultMm) {
    const QString item=QStringLiteral("线要素拓扑检查");
    const double raw=dlg.m_config->getParameterDouble(item,key,defaultMm);
    const QString unit=dlg.m_config->getParameterString(item,key+QStringLiteral("_unit"),QStringLiteral("毫米"));
    if(dlg.m_mapScale<=0||!std::isfinite(raw)||raw<=0)
        throw std::runtime_error("Road topology requires a positive map scale and buffers");
    const double value=dlg.convertUnitValue(raw,unit,false);
    if(!std::isfinite(value)||value<=0)throw std::runtime_error("Invalid road topology buffer");
    return value;
}
inline std::vector<QualityCheckResult> check(ReadShpDialog& dlg,QgsVectorLayer* parsed,const QString& sourceFile) {
    std::vector<QualityCheckResult> results;
    dlg.executedLineTopology=false;
    if(!parsed||!parsed->isValid()||parsed->geometryType()!=QgsWkbTypes::LineGeometry)
        throw std::runtime_error("Road topology requires a valid result-map line layer");
    if(!dlg.m_config||!dlg.m_config->isCheckItemEnabled(QStringLiteral("线要素拓扑检查")))
        throw std::runtime_error("The selected XML does not contain line topology rule 112");
    const bool railwayTunnel=baseName(sourceFile)==QStringLiteral("铁路隧道");
    const double roadBuffer=railwayTunnel
        ? dlg.getConvertedParameter(QStringLiteral("线要素拓扑检查"), QStringLiteral("dangling_tolerance"))
        : buffer(dlg,QStringLiteral("road_connection_buffer"),0.2);
    if(!std::isfinite(roadBuffer)||roadBuffer<=0)
        throw std::runtime_error("Invalid line topology connection tolerance");
    const double residentBuffer=buffer(dlg,QStringLiteral("road_settlement_buffer"),1.0);
    QgsCoordinateReferenceSystem analysisCrs=parsed->crs();
    if(!analysisCrs.isValid())throw std::runtime_error("Road topology requires a valid coordinate system");
    if(analysisCrs.mapUnits()!=QgsUnitTypes::DistanceMeters) {
        QgsGeometry centre=QgsGeometry::fromPointXY(parsed->extent().center());
        centre.transform(QgsCoordinateTransform(analysisCrs,QgsCoordinateReferenceSystem("EPSG:4326"),QgsProject::instance()));
        const QgsPointXY point=centre.asPoint();
        const int zone=std::max(1,std::min(60,static_cast<int>(std::floor((point.x()+180)/6))+1));
        analysisCrs=QgsCoordinateReferenceSystem(QStringLiteral("EPSG:%1").arg((point.y()>=0?32600:32700)+zone));
    }
    const QString target=identity(parsed);
    std::vector<Part> parts;
    std::vector<std::unique_ptr<QgsVectorLayer>> owned;
    QSet<QString> loaded;
    auto transform=[&](QgsGeometry geometry,QgsVectorLayer* layer) {
        if(!layer->crs().isValid())throw std::runtime_error("A topology context layer has no coordinate system");
        if(layer->crs()!=analysisCrs)
            geometry.transform(QgsCoordinateTransform(layer->crs(),analysisCrs,QgsProject::instance()));
        return geometry;
    };
    int parsedValid=0;
    dlg.collectInvalidGeometry(parsed,sourceFile,CheckType::LINE_TOPOLOGY_ERROR,results);
    auto addRoad=[&](QgsVectorLayer* layer) {
        const QString source=identity(layer);
        if(loaded.contains(source)||layer->geometryType()!=QgsWkbTypes::LineGeometry)return;
        loaded.insert(source);
        QgsFeature f;auto it=layer->getFeatures();
        while(it.nextFeature(f)) {
            if(!f.hasGeometry()||f.geometry().isEmpty()||!f.geometry().isGeosValid())continue;
            if(source==target)++parsedValid;
            const QgsGeometry geometry=f.geometry();int number=0;
            for(const QgsLineString* line:dlg.extractLineStrings(geometry)) {
                ++number;if(!line||line->numPoints()<2)continue;
                Part part;part.source=source;part.feature=f;part.number=number;
                const QString category=baseName(layer->name());
                part.backbone=family(category)==Road && category!=QStringLiteral("村道") &&
                    category!=QStringLiteral("连接道") && category!=QStringLiteral("公路隧道") && category!=QStringLiteral("隧道");
                part.original=QgsGeometry(line->clone());part.geometry=transform(part.original,layer);
                const auto metricLine=qgsgeometry_cast<const QgsLineString*>(part.geometry.constGet());
                if(!metricLine||metricLine->numPoints()<2)continue;
                part.ends[0]=QgsPointXY(metricLine->startPoint());part.ends[1]=QgsPointXY(metricLine->endPoint());
                part.closed=metricLine->isClosed();parts.push_back(std::move(part));
            }
        }
    };
    addRoad(parsed);
    const QString dir=QFileInfo(parsed->source()).absolutePath();
    const auto files=QDir(dir).entryInfoList(QStringList{QStringLiteral("*.shp")},QDir::Files,QDir::Name);
    for(const auto& file:files) {
        if(family(file.fileName())!=(railwayTunnel?Railway:Road))continue;
        auto layer=std::make_unique<QgsVectorLayer>(file.absoluteFilePath(),file.completeBaseName(),"ogr");
        if(!layer->isValid())throw std::runtime_error(QStringLiteral("Cannot load road context: %1").arg(file.absoluteFilePath()).toStdString());
        addRoad(layer.get());owned.push_back(std::move(layer));
    }
    std::vector<QgsGeometry> residents;
    std::vector<QgsGeometry> frames;
    std::vector<QgsGeometry> frameAreas;
    std::vector<QgsGeometry> blocks;
    std::vector<QgsGeometry> portals;
    int invalidPortals=0;
    const QStringList residentNames{QStringLiteral("自然村"),QStringLiteral("行政村"),
        QStringLiteral("乡镇级驻地"),QStringLiteral("县级驻地"),QStringLiteral("市级驻地"),
        QStringLiteral("省外乡镇驻地"),QStringLiteral("其他居民地"),QStringLiteral("RESA")};
    for(const auto& file:files) {
        if(railwayTunnel||!residentNames.contains(baseName(file.fileName())))continue;
        QgsVectorLayer layer(file.absoluteFilePath(),file.completeBaseName(),"ogr");
        if(!layer.isValid())throw std::runtime_error(QStringLiteral("Cannot load settlement context: %1").arg(file.absoluteFilePath()).toStdString());
        auto it=layer.getFeatures();QgsFeature f;
        while(it.nextFeature(f)) {
            if(!f.hasGeometry()||f.geometry().isEmpty()||!f.geometry().isGeosValid())continue;
            const QgsGeometry geometry=f.geometry();
            if(geometry.type()!=QgsWkbTypes::LineGeometry)residents.push_back(transform(geometry,&layer));
            else for(const QgsLineString* line:dlg.extractLineStrings(geometry)) {
                if(!line||line->numPoints()<2)continue;
                if(line->isClosed()&&line->numPoints()>=4) {
                    auto polygon=new QgsPolygon;polygon->setExteriorRing(line->clone());
                    residents.push_back(transform(QgsGeometry(polygon),&layer));
                }else residents.push_back(transform(QgsGeometry(line->clone()),&layer));
            }
        }
    }
    for(const auto& file:files) {
        if(railwayTunnel||baseName(file.fileName())!=QStringLiteral("街区面"))continue;
        QgsVectorLayer layer(file.absoluteFilePath(),file.completeBaseName(),"ogr");
        if(!layer.isValid())throw std::runtime_error(QStringLiteral("Cannot load street block: %1").arg(file.absoluteFilePath()).toStdString());
        if(layer.geometryType()!=QgsWkbTypes::LineGeometry&&layer.geometryType()!=QgsWkbTypes::PolygonGeometry)continue;
        auto it=layer.getFeatures();QgsFeature f;
        while(it.nextFeature(f)) {
            if(!f.hasGeometry()||f.geometry().isEmpty()||!f.geometry().isGeosValid())continue;
            blocks.push_back(transform(f.geometry(),&layer));
        }
    }
    for(const auto& file:files) {
        if(baseName(file.fileName())!=QStringLiteral("内图廓"))continue;
        QgsVectorLayer layer(file.absoluteFilePath(),file.completeBaseName(),"ogr");
        if(!layer.isValid())throw std::runtime_error(QStringLiteral("Cannot load map frame: %1").arg(file.absoluteFilePath()).toStdString());
        auto it=layer.getFeatures();QgsFeature f;
        while(it.nextFeature(f)) {
            if(!f.hasGeometry()||f.geometry().isEmpty()||!f.geometry().isGeosValid())continue;
            QgsGeometry geometry=f.geometry();
            if(geometry.type()==QgsWkbTypes::PolygonGeometry) {
                frameAreas.push_back(transform(geometry,&layer));
                geometry=QgsGeometry(geometry.constGet()->boundary());
            }else if(geometry.type()==QgsWkbTypes::LineGeometry) {
                for(const QgsLineString* line:dlg.extractLineStrings(geometry)) {
                    if(!line||line->numPoints()<4||!line->isClosed())continue;
                    auto polygon=new QgsPolygon;polygon->setExteriorRing(line->clone());
                    QgsGeometry area(polygon);
                    if(!area.isEmpty()&&area.isGeosValid()&&area.area()>0)frameAreas.push_back(transform(area,&layer));
                }
            }
            if(geometry.type()==QgsWkbTypes::LineGeometry&&!geometry.isEmpty())frames.push_back(transform(geometry,&layer));
        }
    }
    for(const auto& file:files) {
        if(baseName(file.fileName())!=(railwayTunnel?QStringLiteral("铁路隧道口"):QStringLiteral("公路隧道口")))continue;
        QgsVectorLayer layer(file.absoluteFilePath(),file.completeBaseName(),"ogr");
        if(!layer.isValid())throw std::runtime_error(QStringLiteral("Cannot load road tunnel portals: %1").arg(file.absoluteFilePath()).toStdString());
        auto it=layer.getFeatures();QgsFeature f;
        while(it.nextFeature(f)) {
            auto skip=[&]() {
                ++invalidPortals;
                dlg.logTextEdit->append(QStringLiteral("[ROAD_TOPOLOGY] 公路隧道口无法形成有效区域，已跳过：%1，要素ID=%2")
                    .arg(file.fileName()).arg(f.id()));
            };
            if(!f.hasGeometry()||f.geometry().isEmpty()){skip();continue;}
            const QgsGeometry geometry=f.geometry();
            if(geometry.type()==QgsWkbTypes::PolygonGeometry) {
                if(!geometry.isGeosValid()){skip();continue;}
                portals.push_back(transform(geometry,&layer));
            }else if(geometry.type()==QgsWkbTypes::LineGeometry) {
                const auto lines=dlg.extractLineStrings(geometry);
                if(lines.empty()){skip();continue;}
                for(const QgsLineString* line:lines) {
                    if(!line||line->numPoints()<4||!line->isClosed()){skip();continue;}
                    auto polygon=new QgsPolygon;polygon->setExteriorRing(line->clone());
                    QgsGeometry area(polygon);
                    if(area.isEmpty()||!area.isGeosValid()||area.area()<=0){skip();continue;}
                    portals.push_back(transform(area,&layer));
                }
            }else skip();
        }
    }
    QgsSpatialIndex roadIndex,residentIndex,frameIndex,blockIndex,portalIndex;
    for(int i=0;i<static_cast<int>(parts.size());++i){QgsFeature f;f.setId(i);f.setGeometry(parts[i].geometry);roadIndex.addFeature(f);}
    for(int i=0;i<static_cast<int>(residents.size());++i){QgsFeature f;f.setId(i);f.setGeometry(residents[i]);residentIndex.addFeature(f);}
    for(int i=0;i<static_cast<int>(frames.size());++i){QgsFeature f;f.setId(i);f.setGeometry(frames[i]);frameIndex.addFeature(f);}
    for(int i=0;i<static_cast<int>(blocks.size());++i){QgsFeature f;f.setId(i);f.setGeometry(blocks[i]);blockIndex.addFeature(f);}
    for(int i=0;i<static_cast<int>(portals.size());++i){QgsFeature f;f.setId(i);f.setGeometry(portals[i]);portalIndex.addFeature(f);}
    auto nearResident=[&](const QgsPointXY& point) {
        const QgsGeometry p=QgsGeometry::fromPointXY(point);
        QgsRectangle bounds(point,point);bounds.grow(residentBuffer);
        for(const auto id:residentIndex.intersects(bounds)) {
            const double distance=p.distance(residents.at(static_cast<size_t>(id)));
            if(distance>=0&&distance<=residentBuffer+1e-8)return true;
        }return false;
    };
    auto nearFrame=[&](const QgsPointXY& point) {
        const QgsGeometry p=QgsGeometry::fromPointXY(point);
        QgsRectangle bounds(point,point);bounds.grow(roadBuffer);
        for(const auto id:frameIndex.intersects(bounds)) {
            const double distance=p.distance(frames.at(static_cast<size_t>(id)));
            if(distance>=0&&distance<=roadBuffer+1e-8)return true;
        }return false;
    };
    auto outsideAcrossFrame=[&](const Part& part,int end) {
        const QgsGeometry p=QgsGeometry::fromPointXY(part.ends[end]);
        for(const QgsGeometry& area:frameAreas) {
            if(area.intersects(p)||!area.intersects(part.geometry))continue;
            const QgsGeometry within=part.geometry.intersection(area);
            if(within.isEmpty())continue;
            const QgsGeometry boundary(area.constGet()->boundary());
            const QgsGeometry interior=within.difference(boundary);
            if(!interior.isEmpty()&&interior.length()>1e-8)return true;
        }
        return false;
    };
    auto nearBlock=[&](const QgsPointXY& point) {
        const QgsGeometry p=QgsGeometry::fromPointXY(point);
        QgsRectangle bounds(point,point);bounds.grow(roadBuffer);
        for(const auto id:blockIndex.intersects(bounds)) {
            const double distance=p.distance(blocks.at(static_cast<size_t>(id)));
            if(distance>=0&&distance<=roadBuffer+1e-8)return true;
        }return false;
    };
    auto nearPortal=[&](const QgsPointXY& point) {
        const QgsGeometry p=QgsGeometry::fromPointXY(point);
        QgsRectangle bounds(point,point);bounds.grow(roadBuffer);
        for(const auto id:portalIndex.intersects(bounds)) {
            const double distance=p.distance(portals.at(static_cast<size_t>(id)));
            if(distance>=0&&distance<=roadBuffer+1e-8)return true;
        }return false;
    };
    const int count=static_cast<int>(parts.size());Components components(count);
    for(int i=0;i<count;++i) {
        QgsRectangle bounds=parts[i].geometry.boundingBox();bounds.grow(roadBuffer);
        for(const auto id:roadIndex.intersects(bounds)) {
            const int j=static_cast<int>(id);if(j<=i)continue;
            const double distance=parts[i].geometry.distance(parts[j].geometry);
            if(distance>=0&&distance<=roadBuffer+1e-8)components.join(i,j);
        }
    }
    std::vector<bool> backbone(count,false),populated(count,false);
    std::vector<double> lengths(count,0.0);
    std::vector<QString> componentKeys(count);
    std::vector<int> sizes(count,0);std::vector<std::array<bool,2>> nearResidents(count),nearFrames(count),nearBlocks(count),nearPortals(count);
    for(int i=0;i<count;++i) {
        const int root=components.find(i);++sizes[root];backbone[root]=backbone[root]||parts[i].backbone;
        lengths[root]+=parts[i].geometry.length();
        const QString key=parts[i].source+QStringLiteral("#%1:%2").arg(parts[i].feature.id()).arg(parts[i].number);
        if(componentKeys[root].isEmpty()||key<componentKeys[root])componentKeys[root]=key;
        for(int end=0;end<2;++end){
            nearResidents[i][end]=nearResident(parts[i].ends[end]);
            nearFrames[i][end]=nearFrame(parts[i].ends[end]);
            if(!nearFrames[i][end]&&outsideAcrossFrame(parts[i],end)) {
                nearFrames[i][end]=true;
                if(parts[i].source==target)dlg.logTextEdit->append(QStringLiteral("[ROAD_TOPOLOGY] 道路穿过内图廓，图外端点按已连接处理：要素ID=%1，部件=%2，%3")
                    .arg(parts[i].feature.id()).arg(parts[i].number).arg(end==0?QStringLiteral("起点"):QStringLiteral("终点")));
            }
            nearBlocks[i][end]=nearBlock(parts[i].ends[end]);nearPortals[i][end]=nearPortal(parts[i].ends[end]);
            populated[root]=populated[root]||nearResidents[i][end]||nearFrames[i][end]||nearBlocks[i][end]||nearPortals[i][end];
        }
    }
    // Choose a result-map main network, independently of which source is checked.
    // A village/connector/tunnel island must not promote itself merely by
    // crossing a source-file boundary. A lone backbone stroke is not a network.
    int mainRoot=-1;
    for(int i=0;i<count;++i)if(sizes[i]>=2&&backbone[i]&&
        (mainRoot<0||lengths[i]>lengths[mainRoot]+1e-8||
         (std::abs(lengths[i]-lengths[mainRoot])<=1e-8&&componentKeys[i]<componentKeys[mainRoot])))mainRoot=i;
    int danglingCount=0,isolatedCount=0;
    std::map<QgsFeatureId,size_t> featureResults;
    for(int i=0;i<count;++i) {
        const Part& part=parts[i];if(part.source!=target)continue;
        // Open roads are classified by their own endpoints. A detached network
        // must not override an endpoint which joins another road in that network.
        const int root=components.find(i);const bool componentIsolated=part.closed&&root!=mainRoot&&!populated[root];
        QStringList freeEnds;
        std::vector<QgsGeometry> freePoints;
        if(!part.closed)for(int end=0;end<2;++end) {
            if(nearResidents[i][end]||nearFrames[i][end]||nearBlocks[i][end]||nearPortals[i][end])continue;
            bool connected=false;const QgsGeometry p=QgsGeometry::fromPointXY(part.ends[end]);
            QgsRectangle bounds(part.ends[end],part.ends[end]);bounds.grow(roadBuffer);
            for(const auto id:roadIndex.intersects(bounds)) {
                if(id==i)continue;
                const double distance=p.distance(parts.at(static_cast<size_t>(id)).geometry);
                if(distance>=0&&distance<=roadBuffer+1e-8){connected=true;break;}
            }
            if(!connected) {
                freeEnds.push_back(end==0?QStringLiteral("线的第一个点"):QStringLiteral("线的最后一个点"));
                freePoints.push_back(p);
            }
        }
        const bool endpointsIsolated=freeEnds.size()==2;
        const bool isolated=componentIsolated||endpointsIsolated;
        if(!isolated&&freeEnds.isEmpty())continue;
        double nearest=std::numeric_limits<double>::infinity();
        std::vector<double> endpointDistances(freePoints.size(),std::numeric_limits<double>::infinity());
        for(int j=0;j<count;++j)if(j!=i&&(!componentIsolated||endpointsIsolated||components.find(j)!=root)){
            if(componentIsolated&&!endpointsIsolated) {
                const double distance=part.geometry.distance(parts[j].geometry);
                if(distance>=0)nearest=std::min(nearest,distance);
            }else for(size_t k=0;k<freePoints.size();++k) {
                const double distance=freePoints[k].distance(parts[j].geometry);
                if(distance>=0){nearest=std::min(nearest,distance);endpointDistances[k]=std::min(endpointDistances[k],distance);}
            }
        }
        QualityCheckResult result{};result.passed=false;result.checkType=CheckType::LINE_TOPOLOGY_ERROR;
        result.errorType=isolated?QStringLiteral("道路不合理独立"):QStringLiteral("道路悬挂");
        result.feature=part.feature;result.featureId=part.feature.id();result.sourceFile=sourceFile;
        result.geometry=part.feature.geometry();result.errorGeometry=part.original;result.pointIndex=-1;
        result.calculatedValue=std::isfinite(nearest)?nearest:-1.0;result.standardValue=roadBuffer;
        const QString segment=part.feature.geometry().isMultipart()
            ? QStringLiteral("同一条记录中的第%1段：").arg(part.number) : QString();
        if(isolated) {
            ++isolatedCount;
            if(endpointsIsolated) {
                result.description=segment+QStringLiteral("道路两端均没有合理连接：各端%1米内没有其他道路、街区面、内图廓或公路隧道口，%2米内没有居民点。")
                    .arg(roadBuffer,0,'f',2).arg(residentBuffer,0,'f',2);
                for(size_t k=0;k<freeEnds.size();++k) {
                    result.description+=std::isfinite(endpointDistances[k])
                        ? QStringLiteral(" %1距最近其他道路%2米。").arg(freeEnds[static_cast<int>(k)]).arg(endpointDistances[k],0,'f',2)
                        : QStringLiteral(" %1未找到其他道路。").arg(freeEnds[static_cast<int>(k)]);
                }
            }else {
            const QString network=mainRoot<0 ? QStringLiteral("没有找到可作为主要道路网的道路")
                                               : QStringLiteral("没有接上主要道路网");
            result.description=segment+QStringLiteral("这段道路所在的相连道路%1，端点附近没有街区面、内图廓或公路隧道口，且这些道路的端点%2米内均没有居民点。")
                .arg(network).arg(residentBuffer,0,'f',2);
            result.description+=std::isfinite(nearest)
                ? QStringLiteral(" 这段道路距离这组道路以外最近的其他道路%1米。").arg(nearest,0,'f',2)
                : QStringLiteral(" 没有找到这组道路以外的其他道路。");
            }
        }else {
            ++danglingCount;
            const QString ends=freeEnds.size()==1 ? QStringLiteral("一端") : QStringLiteral("两端");
            result.description=segment+QStringLiteral("道路有%1没有接上其他道路（%2，按数据存储顺序）：%3米内没有其他道路、街区面、内图廓或公路隧道口，%4米内也没有居民点。")
                .arg(ends).arg(freeEnds.join(QStringLiteral("、"))).arg(roadBuffer,0,'f',2).arg(residentBuffer,0,'f',2);
            result.description+=std::isfinite(nearest)
                ? QStringLiteral(" 未接上的端点距离最近的其他道路%1米。").arg(nearest,0,'f',2)
                : QStringLiteral(" 没有找到其他道路。");
        }
        if(railwayTunnel) {
            result.errorType=isolated?QStringLiteral("铁路隧道不合理独立"):QStringLiteral("铁路隧道悬挂");
            result.description=isolated
                ? QStringLiteral("铁路隧道两端均未在%1米范围内连接铁路或铁路隧道口。").arg(roadBuffer,0,'f',2)
                : QStringLiteral("铁路隧道%1未在%2米范围内连接铁路或铁路隧道口。")
                    .arg(freeEnds.join(QStringLiteral("、"))).arg(roadBuffer,0,'f',2);
            if(std::isfinite(nearest))
                result.description+=QStringLiteral(" 未连接端距最近铁路%1米。").arg(nearest,0,'f',2);
        }
        const auto previous=featureResults.find(part.feature.id());
        if(previous==featureResults.end()) {
            featureResults[part.feature.id()]=results.size();
            results.push_back(std::move(result));
        }else {
            QualityCheckResult& merged=results[previous->second];
            auto lines=new QgsMultiLineString;
            for(const QgsLineString* line:dlg.extractLineStrings(merged.errorGeometry))lines->addGeometry(line->clone());
            lines->addGeometry(part.original.constGet()->clone());
            merged.errorGeometry=QgsGeometry(lines);
            if(!merged.errorType.contains(result.errorType))merged.errorType+=QStringLiteral("；")+result.errorType;
            merged.description+=QLatin1Char('\n')+result.description;
            if(result.calculatedValue>=0&&(merged.calculatedValue<0||result.calculatedValue<merged.calculatedValue))
                merged.calculatedValue=result.calculatedValue;
        }
    }
    dlg.executedLineTopology=true;
    dlg.logTextEdit->append(QStringLiteral("[ROAD_TOPOLOGY] 输入线要素=%1 有效参与=%2 无效跳过=%3 成果道路部件=%4 居民地几何=%5 道路缓冲=%6米 居民点缓冲=%7米 悬挂部件=%8 独立部件=%9；内图廓几何=%10 街区面线面几何=%11 公路隧道口区域=%12 无效隧道口跳过=%13；库体不参与成果构网")
        .arg(parsed->featureCount()).arg(parsedValid).arg(parsed->featureCount()-parsedValid).arg(count)
        .arg(residents.size()).arg(roadBuffer).arg(residentBuffer).arg(danglingCount).arg(isolatedCount).arg(frames.size()).arg(blocks.size()).arg(portals.size()).arg(invalidPortals));
    return results;
}
} // namespace RoadTopology
