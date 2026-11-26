#define _HAS_STD_BYTE 0
#include "se_global_blank_check.h"
#include "../ui_class/se_error_define.h"
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogrsf_frmts.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
struct Failure : std::runtime_error {
    int code;
    Failure(int c, const QString& text) : std::runtime_error(text.toUtf8().constData()), code(c) {}
};
void require(bool value, int code, const QString& text) { if (!value) throw Failure(code, text); }
QString qpath(const char* s) { return QString::fromUtf8(s ? s : "").trimmed(); }
struct GeometryDeleter { void operator()(OGRGeometry* p) const { OGRGeometryFactory::destroyGeometry(p); } };
struct DatasetDeleter { void operator()(GDALDataset* p) const { if (p) GDALClose(p); } };
struct FeatureDeleter { void operator()(OGRFeature* p) const { OGRFeature::DestroyFeature(p); } };
struct TransformDeleter { void operator()(OGRCoordinateTransformation* p) const { if (p) OCTDestroyCoordinateTransformation(p); } };
using Geometry = std::unique_ptr<OGRGeometry, GeometryDeleter>;
using Dataset = std::unique_ptr<GDALDataset, DatasetDeleter>;
using Feature = std::unique_ptr<OGRFeature, FeatureDeleter>;
using Transform = std::unique_ptr<OGRCoordinateTransformation, TransformDeleter>;

double area(const OGRGeometry* g) {
    if (!g || g->IsEmpty()) return 0;
    const auto kind = wkbFlatten(g->getGeometryType());
    if (kind == wkbPolygon || kind == wkbMultiPolygon) return OGR_G_Area(OGRGeometry::ToHandle(const_cast<OGRGeometry*>(g)));
    double result = 0;
    if (kind == wkbGeometryCollection) {
        const auto* collection = g->toGeometryCollection();
        for (int i=0; i<collection->getNumGeometries(); ++i) result += area(collection->getGeometryRef(i));
    }
    return result;
}
Geometry rectangle(double x0, double y0, double x1, double y1) {
    OGRLinearRing ring;
    ring.addPoint(x0,y0); ring.addPoint(x1,y0); ring.addPoint(x1,y1); ring.addPoint(x0,y1); ring.addPoint(x0,y0);
    auto* polygon = new OGRPolygon(); polygon->addRing(&ring);
    return Geometry(polygon);
}
void collectPolygons(const OGRGeometry* source, OGRMultiPolygon& out) {
    if (!source || source->IsEmpty()) return;
    const auto type = wkbFlatten(source->getGeometryType());
    if (type == wkbPolygon) { out.addGeometry(source); return; }
    if (type == wkbMultiPolygon || type == wkbGeometryCollection) {
        const auto* c = source->toGeometryCollection();
        for (int i=0; i<c->getNumGeometries(); ++i) collectPolygons(c->getGeometryRef(i),out);
    }
}
Dataset openVector(const QString& path) {
    Dataset ds(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(),GDAL_OF_VECTOR|GDAL_OF_READONLY,nullptr,nullptr,nullptr)));
    require(ds != nullptr, SE_ERROR_GDAL_OPEN_FAILED, "无法读取图层："+path);
    require(ds->GetLayer(0) != nullptr, SE_ERROR_LAYER_NOT_FOUND, "文件内没有图层："+path);
    return ds;
}
void ensureParent(const QString& path) {
    require(!path.isEmpty(),SE_ERROR_INVALID_PARAMETER,"输出路径不能为空");
    require(QDir().mkpath(QFileInfo(path).absolutePath()),SE_ERROR_WRITE_FILE_FAILED,"无法创建输出目录："+path);
}
void writeJson(const QString& path, const QJsonObject& root) {
    ensureParent(path);
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly),SE_ERROR_WRITE_FILE_FAILED,"无法写入JSON："+path);
    const QByteArray bytes=QJsonDocument(root).toJson(QJsonDocument::Indented);
    require(file.write(bytes)==bytes.size() && file.commit(),SE_ERROR_WRITE_FILE_FAILED,"保存JSON失败："+path);
}

double blankAreaFactor(QString unit){
    unit=unit.trimmed().toLower();unit.replace("²","2");unit.replace("^","");unit.remove(' ');
    const QMap<QString,double> factors{{"",1},{"平方毫米",.01},{"mm2",.01},{"平方厘米",1},{"cm2",1},{"平方分米",100},{"dm2",100},
        {"平方米",10000},{"m2",10000},{"平方千米",1e10},{"平方公里",1e10},{"km2",1e10},{"公顷",1e8},{"ha",1e8},{"亩",1e8/15.0}};
    require(factors.contains(unit),SE_ERROR_XML_PARSE_FAILED,"不支持的最小空白面积单位："+unit);return factors.value(unit);
}
struct Rule {
    QMap<QString,QString> values;
    double gridCm2=0, minCm2=0, pointMm=0.5, lineMm=0.2, marginMm=0, connectionMm=0;
    int depth=0;
    std::uint64_t maxPixels=16000000;
    bool saveCells=false, prepared=false, automaticGrid=true, automaticDepth=true;
};
Rule readRules(const QString& path, int scale) {
    QFile file(path); QDomDocument doc;
    require(file.open(QIODevice::ReadOnly),SE_ERROR_FILE_NOT_FOUND,"XML不存在或无法读取："+path);
    require(doc.setContent(&file),SE_ERROR_XML_PARSE_FAILED,"XML格式错误："+path);
    require(doc.documentElement().tagName()=="check_list",SE_ERROR_XML_PARSE_FAILED,"XML根节点必须为check_list");
    QDomElement exact, generic;
    const auto scales=doc.documentElement().elementsByTagName("scale");
    for (int i=0; i<scales.size(); ++i) {
        const auto s=scales.at(i).toElement();
        const QString text=s.firstChildElement("value").text().trimmed();
        bool ok=false; const int value=text.toInt(&ok);
        const bool isExact=ok && value==scale;
        const bool isGeneric=text.isEmpty() || (ok && value==0);
        if (!isExact && !isGeneric) continue;
        const auto items=s.elementsByTagName("check_item");
        for (int j=0;j<items.size();++j) {
            const auto item=items.at(j).toElement();
            if (item.firstChildElement("name").text().trimmed()!=QString::fromUtf8("全局空白检查")) continue;
            QDomElement& slot=isExact?exact:generic;
            require(slot.isNull(),SE_ERROR_XML_PARSE_FAILED,"同一比例尺存在重复的全局空白检查配置");
            slot=item;
        }
    }
    const auto item=exact.isNull()?generic:exact;
    require(!item.isNull(),SE_ERROR_XML_PARSE_FAILED,"未找到当前比例尺或通用的全局空白检查");
    Rule rule;
    const auto nodes=item.elementsByTagName("rule");
    for (int i=0;i<nodes.size();++i) {
        const auto node=nodes.at(i).toElement();
        QString name=node.firstChildElement("name").text().trimmed();
        QString normalized=name;
        normalized.replace(QChar(0xff08),'('); normalized.replace(QChar(0xff09),')');
        if (normalized.startsWith("候选要素空间关系(") && normalized.endsWith(')')) name="候选要素空间关系";
        // 解释面积、候选空间关系和可选连接宽度，其他信息由集成接口传入。
        if (name!="最小空白面积" && name!="候选要素空间关系" && name!="最小连接宽度" && name!="格网边长") continue;
        require(!name.isEmpty() && !rule.values.contains(name),SE_ERROR_XML_PARSE_FAILED,"规则名称为空或重复："+name);
        rule.values[name]=node.firstChildElement("value").text().trimmed();
        const QString unit=node.firstChildElement("unit").text().trimmed();
        if(name=="格网边长"){
            bool ok=false;const double value=rule.values[name].toDouble(&ok);
            const QMap<QString,double> factors{{"",1},{"毫米",1},{"mm",1},{"厘米",10},{"cm",10},{"米",1000},{"m",1000}};
            require(ok&&factors.contains(unit.toLower()),SE_ERROR_XML_PARSE_FAILED,"格网边长或单位不合法，支持毫米、厘米、米及mm/cm/m");
            const double mm=value*factors.value(unit.toLower());
            require(std::isfinite(mm)&&mm>0,SE_ERROR_INVALID_PARAMETER,"格网边长必须为有限正数");
            rule.values[name]=QString::number(mm,'g',17);
        }
        const bool areaParam=name=="初始格网面积" || name=="最小空白面积";
        const bool lengthParam=name=="最小连接宽度";
        if(areaParam){bool ok=false;double value=rule.values[name].toDouble(&ok);double converted=value*blankAreaFactor(unit);
            require(ok&&std::isfinite(converted),SE_ERROR_INVALID_PARAMETER,"最小空白面积换算结果不是有限数值");rule.values[name]=QString::number(converted,'g',17);}
        if(lengthParam&&!unit.isEmpty())require(unit=="毫米",SE_ERROR_XML_PARSE_FAILED,"单位不符："+name);
    }
    auto number=[&](const QString& name, double fallback, bool mandatory) {
        require(!mandatory || rule.values.contains(name),SE_ERROR_XML_PARSE_FAILED,"缺少参数："+name);
        if (!rule.values.contains(name)) return fallback;
        bool ok=false; const double value=rule.values[name].toDouble(&ok);
        require(ok && std::isfinite(value),SE_ERROR_XML_PARSE_FAILED,"参数不是有限数值："+name);
        return value;
    };
    rule.minCm2=number("最小空白面积",0,true);
    rule.connectionMm=number("最小连接宽度",0,false);
    require(rule.connectionMm>=0,SE_ERROR_INVALID_PARAMETER,"最小连接宽度不能小于0");
    rule.automaticGrid=!rule.values.contains("格网边长");
    rule.automaticDepth=true;
    require(rule.minCm2>=0 && (!rule.automaticGrid || rule.minCm2>0),SE_ERROR_INVALID_PARAMETER,"自动配置时最小空白面积必须大于0");
    // XML只暴露最终格网边长。内部按4×4小格组成的大格批量判断空白，最多细分两次。
    const double gridMm=number("格网边长",5.0,false);
    const double initialCm=gridMm*4.0/10.0;
    rule.gridCm2=initialCm*initialCm;
    require(std::isfinite(rule.gridCm2),SE_ERROR_INVALID_PARAMETER,"格网边长换算溢出");
    require(rule.gridCm2>0,SE_ERROR_INVALID_PARAMETER,"初始格网面积必须大于0");
    // 细分仅为内部批处理优化，最终格网精度由XML格网边长决定。
    const int automaticDepth=2;
    const double depth=automaticDepth;
    require(rule.gridCm2>0 && rule.minCm2>=0 && depth>=0 && depth<=12 && depth==std::floor(depth),SE_ERROR_INVALID_PARAMETER,"面积参数无效或细分层数不在0—12的整数范围");
    rule.depth=static_cast<int>(depth);
    const double pixels=number("最大栅格数",16000000,false);
    require(pixels>=1 && pixels<=64000000 && pixels==std::floor(pixels),SE_ERROR_INVALID_PARAMETER,"最大栅格数必须为1—64000000之间的整数");
    rule.maxPixels=static_cast<std::uint64_t>(pixels);
    auto boolean=[&](const QString& name,bool fallback) {
        if (!rule.values.contains(name)) return fallback;
        const QString value=rule.values[name].toLower();
        require(value=="true" || value=="false",SE_ERROR_XML_PARSE_FAILED,name+"仅支持true/false");
        return value=="true";
    };
    rule.saveCells=boolean("输出空白格网",false);
    const QString mode=rule.values.value("输入模式","几何缓冲");
    require(mode=="占用面" || mode=="几何缓冲",SE_ERROR_XML_PARSE_FAILED,"输入模式仅支持占用面或几何缓冲");
    rule.prepared=mode=="占用面";
    rule.pointMm=number("点符号半径",0.5,false);
    rule.lineMm=number("线宽",0.2,false);
    rule.marginMm=number("附加避让距离",0,false);
    require(rule.pointMm>0 && rule.lineMm>0 && rule.marginMm>=0,SE_ERROR_INVALID_PARAMETER,"点符号半径、线宽必须大于0，附加避让距离不能小于0");
    return rule;
}
QString resolvePath(const QString& path) {
    const QString p=path.trimmed();
    if (p.isEmpty()) return p;
    return QDir::cleanPath(QFileInfo(p).absoluteFilePath());
}

QStringList names(const QString& value) {
    QString text=value; text.replace(QChar(0xff1b),';');
    QStringList result;
    for (const QString& part:text.split(';',Qt::SkipEmptyParts)) {
        const QString p=part.trimmed(); if (!p.isEmpty()) result<<p;
    }
    return result;
}
QString normalizedPath(const QString& path) { return QFileInfo(path).absoluteFilePath().toCaseFolded(); }
bool matchesLayer(const QString& name,const QStringList& patterns,bool includeGeometrySuffix=false) {
    QStringList choices{name,QFileInfo(name).completeBaseName()};
    if (includeGeometrySuffix) {
        const QString stem=QFileInfo(name).completeBaseName();
        for (const QString& suffix:QStringList{"_Point","_LineString","_Polygon"})
            if (stem.endsWith(suffix,Qt::CaseInsensitive)) choices<<stem.left(stem.size()-suffix.size());
    }
    for (const QString& pattern:patterns) {
        const QString filePattern=QFileInfo(pattern).fileName();
        if (pattern.contains('/') || pattern.contains('\\')) {
            for (const QString& choice:choices)
                if (choice.compare(filePattern,Qt::CaseInsensitive)==0 ||
                    choice.compare(QFileInfo(filePattern).completeBaseName(),Qt::CaseInsensitive)==0) return true;
            continue;
        }
        const QRegularExpression expression(QRegularExpression::wildcardToRegularExpression(pattern),QRegularExpression::CaseInsensitiveOption);
        for (const QString& choice:choices) if (expression.match(choice).hasMatch()) return true;
    }
    return false;
}
// 扫描直属文件；.gdb作为数据库整体读取，不递归进入其内部文件。
QStringList expandInputs(const QString& value,bool sourceDatabase) {
    QStringList result;
    for (const QString& entry:names(value)) {
        const QString path=resolvePath(entry);
        const QFileInfo info(path);
        require(info.exists(),SE_ERROR_FILE_NOT_FOUND,"数据路径不存在："+path);
        if (info.isDir() && !(sourceDatabase && info.suffix().compare("gdb",Qt::CaseInsensitive)==0)) {
            const auto files=QDir(path).entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name);
            for (const QFileInfo& f:files) {
                const QString ext=f.suffix().toLower();
                if ((f.isFile() && (ext=="shp" || ext=="geojson" || ext=="gpkg")) ||
                    (sourceDatabase && f.isDir() && ext=="gdb")) result<<f.absoluteFilePath();
            }
        } else result<<path;
    }
    QSet<QString> seen; QStringList unique;
    for (const QString& p:result) if (!seen.contains(normalizedPath(p))) { seen.insert(normalizedPath(p)); unique<<p; }
    return unique;
}

struct BlankRegion { int id; Geometry geometry; OGREnvelope envelope; };
struct CandidateResult {
    QMap<int,QJsonArray> byRegion;
    QJsonArray layers;
    int features=0,associations=0;
};
bool spatialHit(const OGRGeometry* g,const OGRGeometry* region,const QString& mode) {
    if (!g->Intersects(region)) return false;
    if (mode=="完全位于") {
        Geometry rest(g->Difference(region));
        require(rest!=nullptr,SE_ERROR_INVALID_GEOMETRY,"候选要素包含关系计算失败");
        return rest->IsEmpty();
    }
    Geometry intersection(g->Intersection(region));
    require(intersection!=nullptr,SE_ERROR_INVALID_GEOMETRY,"候选要素相交关系计算失败");
    if (g->getDimension()==0) return !intersection->IsEmpty();
    if (g->getDimension()==2) return area(intersection.get())>0;
    return !intersection->IsEmpty() && intersection->getDimension()>=1;
}
void addField(OGRLayer* layer,const char* name,OGRFieldType type) {
    OGRFieldDefn field(name,type);
    require(layer->CreateField(&field)==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"创建候选关联字段失败");
}

CandidateResult extractCandidates(const QStringList& paths,const QStringList& requested,const QString& mode,
                                  const std::vector<BlankRegion>& blanks,OGRSpatialReference& target,
                                  const QString& output,const std::function<void(const QString&)>& message) {
    CandidateResult result;
    require(!requested.isEmpty(),SE_ERROR_INVALID_PARAMETER,"配置源数据路径后，必须指定空白区提取图层（或*）");
    ensureParent(output);
    auto* driver=GetGDALDriverManager()->GetDriverByName("GPKG");
    require(driver,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少GPKG驱动，无法保留候选要素属性");
    if (QFileInfo::exists(output)) require(driver->Delete(output.toUtf8().constData())==CE_None,SE_ERROR_WRITE_FILE_FAILED,"候选输出被占用："+output);
    Dataset dataset(driver->Create(output.toUtf8().constData(),0,0,0,GDT_Unknown,nullptr));
    require(dataset!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"创建候选输出失败："+output);
    OGRLayer* relation=dataset->CreateLayer("blank_relations",nullptr,wkbNone,nullptr);
    require(relation,SE_ERROR_LAYER_CREATE_FAILED,"创建候选关联表失败");
    addField(relation,"region_id",OFTInteger); addField(relation,"output_layer",OFTString);
    addField(relation,"output_fid",OFTInteger64); addField(relation,"source_path",OFTString);
    addField(relation,"source_layer",OFTString); addField(relation,"source_fid",OFTString);
    addField(relation,"spatial_rule",OFTString);
    // Group every source layer and relation in one transaction to avoid per-feature disk commits.
    require(dataset->StartTransaction()==OGRERR_NONE,SE_ERROR_WRITE_FILE_FAILED,"启动候选输出事务失败");
    QSet<int> matchedPatterns;
    int layerCount=0;
    for (const QString& path:paths) {
        Dataset source=openVector(path);
        for (int layerIndex=0;layerIndex<source->GetLayerCount();++layerIndex) {
            OGRLayer* layer=source->GetLayer(layerIndex); if (!layer) continue;
            const QString sourceName=QString::fromUtf8(layer->GetName());
            bool selected=false;
            for (int p=0;p<requested.size();++p) if (matchesLayer(sourceName,QStringList{requested[p]})) { selected=true; matchedPatterns.insert(p); }
            if (!selected) continue;
            require(layer->GetSpatialRef(),SE_ERROR_UNSUPPORTED_SRS,"候选源图层缺少坐标系："+sourceName);
            OGRSpatialReference sourceSrs(*layer->GetSpatialRef()); sourceSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            Transform transform;
            if (!sourceSrs.IsSame(&target)) {
                transform.reset(OGRCreateCoordinateTransformation(&sourceSrs,&target));
                require(transform!=nullptr,SE_ERROR_UNSUPPORTED_SRS,"无法转换候选图层坐标系："+sourceName);
            }
            const QString outputName=QString("source_%1").arg(++layerCount,3,10,QChar('0'));
            // Unknown permits original point/line/polygon types and preserves whole source geometry.
            char** options=nullptr; options=CSLSetNameValue(options,"FID","_gb_internal_fid");
            while (layer->GetLayerDefn()->GetFieldIndex(CSLFetchNameValue(options,"FID"))>=0)
                options=CSLSetNameValue(options,"FID",(QString::fromUtf8(CSLFetchNameValue(options,"FID"))+"_").toUtf8().constData());
            options=CSLSetNameValue(options,"GEOMETRY_NAME","_gb_geometry");
            while (layer->GetLayerDefn()->GetFieldIndex(CSLFetchNameValue(options,"GEOMETRY_NAME"))>=0)
                options=CSLSetNameValue(options,"GEOMETRY_NAME",(QString::fromUtf8(CSLFetchNameValue(options,"GEOMETRY_NAME"))+"_").toUtf8().constData());
            OGRLayer* dest=dataset->CreateLayer(outputName.toUtf8().constData(),&target,wkbUnknown,options); CSLDestroy(options);
            require(dest,SE_ERROR_LAYER_CREATE_FAILED,"创建候选要素层失败："+sourceName);
            for (int field=0;field<layer->GetLayerDefn()->GetFieldCount();++field)
                require(dest->CreateField(layer->GetLayerDefn()->GetFieldDefn(field))==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"复制源字段失败："+sourceName);
            QString sourceFidField="_gb_source_fid", regionsField="_gb_region_ids";
            while (dest->GetLayerDefn()->GetFieldIndex(sourceFidField.toUtf8().constData())>=0) sourceFidField+="_";
            while (dest->GetLayerDefn()->GetFieldIndex(regionsField.toUtf8().constData())>=0) regionsField+="_";
            addField(dest,sourceFidField.toUtf8().constData(),OFTString); addField(dest,regionsField.toUtf8().constData(),OFTString);
            int layerFeatures=0;
            layer->ResetReading();
            while (Feature f{layer->GetNextFeature()}) {
                if (!f->GetGeometryRef() || f->GetGeometryRef()->IsEmpty()) continue;
                Geometry geometry(f->GetGeometryRef()->clone());
                if (transform) require(geometry->transform(transform.get())==OGRERR_NONE,SE_ERROR_UNSUPPORTED_SRS,"候选要素坐标转换失败："+sourceName);
                require(geometry->IsValid(),SE_ERROR_INVALID_GEOMETRY,"候选源图层有无效几何："+sourceName+" FID="+QString::number(f->GetFID()));
                OGREnvelope envelope; geometry->getEnvelope(&envelope);
                std::vector<int> hits; QStringList ids;
                for (const auto& blank:blanks) {
                    const auto& e=blank.envelope;
                    if (envelope.MaxX<e.MinX || envelope.MinX>e.MaxX || envelope.MaxY<e.MinY || envelope.MinY>e.MaxY) continue;
                    if (spatialHit(geometry.get(),blank.geometry.get(),mode)) { hits.push_back(blank.id); ids<<QString::number(blank.id); }
                }
                if (hits.empty()) continue;
                Feature item(OGRFeature::CreateFeature(dest->GetLayerDefn()));
                require(item->SetFrom(f.get(),TRUE)==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"复制源要素属性失败");
                item->SetFID(OGRNullFID);
                require(item->SetGeometry(geometry.get())==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"复制源要素几何失败");
                const QString fid=QString::number(f->GetFID());
                item->SetField(sourceFidField.toUtf8().constData(),fid.toUtf8().constData());
                item->SetField(regionsField.toUtf8().constData(),ids.join(';').toUtf8().constData());
                require(dest->CreateFeature(item.get())==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"写入候选要素失败");
                ++result.features; ++layerFeatures;
                for (int id:hits) {
                    Feature link(OGRFeature::CreateFeature(relation->GetLayerDefn()));
                    link->SetField("region_id",id); link->SetField("output_layer",outputName.toUtf8().constData());
                    link->SetField("output_fid",item->GetFID()); link->SetField("source_path",path.toUtf8().constData());
                    link->SetField("source_layer",sourceName.toUtf8().constData()); link->SetField("source_fid",fid.toUtf8().constData());
                    link->SetField("spatial_rule",mode.toUtf8().constData());
                    require(relation->CreateFeature(link.get())==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"写入空白区关联表失败");
                    result.byRegion[id].append(QJsonObject{{"source_path",path},{"source_layer",sourceName},{"source_fid",fid},
                        {"output_layer",outputName},{"output_fid",QString::number(item->GetFID())}});
                    ++result.associations;
                }
            }
            result.layers.append(QJsonObject{{"source_path",path},{"source_layer",sourceName},{"output_layer",outputName},
                {"feature_count",layerFeatures},{"source_geometry_type",static_cast<int>(layer->GetGeomType())},
                {"source_fid_field",sourceFidField},{"region_ids_field",regionsField}});
            message(QString("源图层 %1：提取 %2 个空间候选").arg(sourceName).arg(layerFeatures));
        }
    }
    for (int p=0;p<requested.size();++p) require(matchedPatterns.contains(p),SE_ERROR_LAYER_NOT_FOUND,"空白区提取图层规则未匹配到源图层："+requested[p]);
    require(dataset->CommitTransaction()==OGRERR_NONE,SE_ERROR_WRITE_FILE_FAILED,"提交候选输出失败");
    return result;
}

// 每个源图层单独导出SHP；GPKG仍保留完整字段名与多区域关联表。
void exportCandidateShapes(CandidateResult& result,const QString& gpkg,const QString& folder,OGRSpatialReference& srs,const QStringList& protectedPaths) {
    require(QDir().mkpath(folder),SE_ERROR_WRITE_FILE_FAILED,"无法创建分图层输出目录："+folder);
    auto* driver=GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");
    require(driver,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少Shapefile输出驱动");
    const QString manifestPath=QDir(folder).filePath("候选要素输出清单.json");
    QFile previous(manifestPath);
    if (previous.open(QIODevice::ReadOnly)) {
        const auto files=QJsonDocument::fromJson(previous.readAll()).object()["files"].toArray();
        previous.close();
        for (const auto& entry:files) {
            const QString path=entry.toString(); const QFileInfo file(path);
            // Only delete prior files from this exact output directory, never recursively.
            require(normalizedPath(file.absolutePath())==normalizedPath(folder) && file.suffix().compare("shp",Qt::CaseInsensitive)==0,
                    SE_ERROR_INVALID_PARAMETER,"候选输出清单包含目录外路径");
            for(const auto& protectedPath:protectedPaths)require(normalizedPath(path)!=normalizedPath(protectedPath),SE_ERROR_INVALID_PARAMETER,"候选清单不能删除输入或主结果："+path);
            require(file.fileName().startsWith("全局空白检查_"),SE_ERROR_INVALID_PARAMETER,"候选清单包含非候选命名文件");
            if (file.exists()) require(driver->Delete(path.toUtf8().constData())==CE_None,SE_ERROR_WRITE_FILE_FAILED,"无法更新旧候选图层："+path);
        }
    }
    Dataset source=openVector(gpkg); QSet<QString> used; QJsonArray files;
    for(const auto& path:protectedPaths){const QFileInfo f(path);if(normalizedPath(f.absolutePath())==normalizedPath(folder)&&f.suffix().compare("shp",Qt::CaseInsensitive)==0)used.insert(f.completeBaseName().toCaseFolded());}
    for (int i=0;i<result.layers.size();++i) {
        QJsonObject info=result.layers[i].toObject();
        OGRLayer* layer=source->GetLayerByName(info["output_layer"].toString().toUtf8().constData());
        require(layer,SE_ERROR_LAYER_NOT_FOUND,"候选中间图层不存在");
        QSet<int> kinds; bool multiPoint=false;
        while (Feature f{layer->GetNextFeature()}) {
            const auto* g=f->GetGeometryRef(); if (!g || g->IsEmpty()) continue;
            const int dimension=g->getDimension();
            require(dimension>=0 && dimension<=2,SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"源图层包含不能导出为SHP的混合几何");
            kinds.insert(dimension);
            if (wkbFlatten(g->getGeometryType())==wkbMultiPoint) multiPoint=true;
        }
        if (kinds.isEmpty()) {
            Geometry empty(OGRGeometryFactory::createGeometry(static_cast<OGRwkbGeometryType>(info["source_geometry_type"].toInt())));
            const int dimension=empty?empty->getDimension():-1;
            if (dimension>=0 && dimension<=2) kinds.insert(dimension);
        }
        QString base=info["source_layer"].toString();
        base.replace(QRegularExpression("[<>:\"/\\\\|?*]"),"_");
        while (base.endsWith('.') || base.endsWith(' ')) base.chop(1);
        if (base.isEmpty()) base="source";
        QJsonArray shapes;
        QList<int> sorted=kinds.values(); std::sort(sorted.begin(),sorted.end());
        for (int kind:sorted) {
            QString name="全局空白检查_error_"+base+(kinds.size()>1?(kind==0?"_点":kind==1?"_线":"_面"):"");
            QString unique=name;
            for (int suffix=2;used.contains(unique.toCaseFolded());++suffix) unique=name+"_"+QString::number(suffix);
            used.insert(unique.toCaseFolded());
            const QString path=QDir(folder).filePath(unique+".shp");
            if (QFileInfo::exists(path)) require(driver->Delete(path.toUtf8().constData())==CE_None,SE_ERROR_WRITE_FILE_FAILED,"无法覆盖候选图层："+path);
            Dataset output(driver->Create(path.toUtf8().constData(),0,0,0,GDT_Unknown,nullptr));
            require(output!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"无法创建候选SHP："+path);
            const auto type=kind==0?(multiPoint?wkbMultiPoint:wkbPoint):(kind==1?wkbMultiLineString:wkbMultiPolygon);
            char** options=nullptr;options=CSLSetNameValue(options,"ENCODING","UTF-8");
            OGRLayer* dest=output->CreateLayer(unique.toUtf8().constData(),&srs,type,options);CSLDestroy(options);
            require(dest,SE_ERROR_LAYER_CREATE_FAILED,"创建候选SHP图层失败");
            const int count=layer->GetLayerDefn()->GetFieldCount();
            std::vector<int> fieldMap(static_cast<size_t>(count));
            for (int field=0;field<count;++field) {
                fieldMap[field]=field;
                require(dest->CreateField(layer->GetLayerDefn()->GetFieldDefn(field))==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"复制候选SHP字段失败");
            }
            layer->ResetReading();
            while (Feature f{layer->GetNextFeature()}) {
                const auto* g=f->GetGeometryRef(); if (!g || g->IsEmpty() || g->getDimension()!=kind) continue;
                Feature item(OGRFeature::CreateFeature(dest->GetLayerDefn()));
                require(item->SetFrom(f.get(),fieldMap.data(),TRUE)==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"复制候选SHP属性失败");
                item->SetFID(OGRNullFID);
                Geometry geometry(OGRGeometryFactory::forceTo(g->clone(),type));
                require(geometry && item->SetGeometry(geometry.get())==OGRERR_NONE && dest->CreateFeature(item.get())==OGRERR_NONE,
                        SE_ERROR_FEATURE_CREATE_FAILED,"写入候选SHP要素失败");
            }
            shapes.append(path);files.append(path);
        }
        info["shapefiles"]=shapes;result.layers[i]=info;
    }
    writeJson(manifestPath,QJsonObject{{"files",files}});
}

void updateCandidateSummary(const QString& path,const QMap<int,QString>& summaries,const std::function<void(const QString&)>& message){
    Dataset dataset(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(),GDAL_OF_VECTOR|GDAL_OF_UPDATE,nullptr,nullptr,nullptr)));
    require(dataset!=nullptr,SE_ERROR_GDAL_OPEN_FAILED,"无法更新空白面候选统计："+path);auto* layer=dataset->GetLayer(0);
    require(layer!=nullptr,SE_ERROR_CORRUPT_DATA,"空白面图层不存在");
    if(layer->GetLayerDefn()->GetFieldIndex("CAND_INFO")<0){OGRFieldDefn field("CAND_INFO",OFTString);field.SetWidth(254);require(layer->CreateField(&field)==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"创建空白面候选统计字段失败");}
    const bool shp=QFileInfo(path).suffix().compare("shp",Qt::CaseInsensitive)==0;
    while(Feature f{layer->GetNextFeature()}){const int id=f->GetFieldAsInteger("REGION_ID");require(summaries.contains(id),SE_ERROR_CORRUPT_DATA,"空白面区域编号与候选统计不一致");
        QString text=summaries.value(id);if(shp&&text.toUtf8().size()>254){const QString suffix="…详见JSON";while(!text.isEmpty()&&(text+suffix).toUtf8().size()>254)text.chop(1);text+=suffix;message(QString("区域%1候选统计超出SHP字段长度，完整内容保留于JSON").arg(id));}
        f->SetField("CAND_INFO",text.toUtf8().constData());require(layer->SetFeature(f.get())==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"写入空白面候选统计失败");
    }
}
struct VectorOutput {
    Dataset ds; OGRLayer* layer=nullptr;
    VectorOutput(const QString& path,OGRSpatialReference& srs,bool cells) {
        ensureParent(path);
        const QString ext=QFileInfo(path).suffix().toLower();
        const char* driver=ext=="shp"?"ESRI Shapefile":ext=="geojson"?"GeoJSON":ext=="gpkg"?"GPKG":nullptr;
        require(driver,SE_ERROR_INVALID_PARAMETER,"输出图层必须为.shp、.geojson或.gpkg");
        auto* d=GetGDALDriverManager()->GetDriverByName(driver);
        require(d,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少输出驱动");
        if (QFileInfo::exists(path)) require(d->Delete(path.toUtf8().constData())==CE_None,SE_ERROR_WRITE_FILE_FAILED,"无法覆盖输出图层（可能被占用）："+path);
        ds.reset(d->Create(path.toUtf8().constData(),0,0,0,GDT_Unknown,nullptr));
        require(ds!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"无法创建图层："+path);
        char** opts=nullptr;
        if (ext=="shp") opts=CSLSetNameValue(opts,"ENCODING","UTF-8");
        layer=ds->CreateLayer(QFileInfo(path).completeBaseName().toUtf8().constData(),&srs,wkbMultiPolygon,opts);
        CSLDestroy(opts);
        require(layer,SE_ERROR_LAYER_CREATE_FAILED,"创建输出数据层失败");
        for (const auto& field:std::vector<std::pair<const char*,OGRFieldType>>{{"REGION_ID",OFTInteger},{"AREA_CM2",OFTReal},{"AREA_M2",OFTReal},{"DEPTH",OFTInteger}}) {
            OGRFieldDefn definition(field.first,field.second);
            require(layer->CreateField(&definition)==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"创建输出字段失败");
        }
        OGRFieldDefn candidateInfo("CAND_INFO",OFTString);candidateInfo.SetWidth(254);
        require(layer->CreateField(&candidateInfo)==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"创建候选统计字段失败");
        Q_UNUSED(cells);
    }
    void add(const OGRGeometry* geometry,int id,double cm2,double m2,int depth) {
        OGRMultiPolygon polygons; collectPolygons(geometry,polygons);
        if (polygons.IsEmpty()) return;
        Feature item(OGRFeature::CreateFeature(layer->GetLayerDefn()));
        item->SetField("REGION_ID",id); item->SetField("AREA_CM2",cm2); item->SetField("AREA_M2",m2); item->SetField("DEPTH",depth);
        item->SetField("CAND_INFO","");
        require(item->SetGeometry(&polygons)==OGRERR_NONE && layer->CreateFeature(item.get())==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"写入输出要素失败");
    }
};
struct Occupancy { Geometry geometry; OGREnvelope envelope; };
// Width control partitions the existing empty mask without removing any empty cells.
// A 2x finer working grid approximates erosion, then distinct core labels grow back
// within the original mask. Components with no surviving core remain independent.
struct BlankPartition {
    std::vector<int> labels;
    int width=0,height=0,cores=0,preservedSmall=0;
    double pixel=0;
};
BlankPartition partitionBlankMask(const std::vector<unsigned char>& mask,int width,int height,
                                 double pixel,double minimumWidth,std::uint64_t limit,
                                 const OGRGeometry* frame,double left,double top) {
    BlankPartition result;
    const int factor=minimumWidth>0?2:1;
    require(static_cast<std::uint64_t>(width)*height*factor*factor<=limit,
            SE_ERROR_NOT_ENOUGH_MEMORY,"连接宽度分区工作栅格超过内存限制");
    result.width=width*factor;result.height=height*factor;result.pixel=pixel/factor;
    const int w=result.width,h=result.height,n=w*h;
    std::vector<unsigned char> empty(static_cast<size_t>(n),0);
    result.labels.assign(static_cast<size_t>(n),0);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x)
        empty[static_cast<size_t>(y)*w+x]=mask[static_cast<size_t>(y/factor)*width+x/factor];
    if (minimumWidth<=0) {
        for (int i=0;i<n;++i) result.labels[i]=empty[i]?1:0;
        return result;
    }
    auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");
    require(mem,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少连接宽度分区栅格驱动");
    const int pw=w+2,ph=h+2;
    std::vector<unsigned char> padded(static_cast<size_t>(pw)*ph,0);
    for (int y=0;y<h;++y) std::copy(empty.begin()+static_cast<size_t>(y)*w,
        empty.begin()+static_cast<size_t>(y+1)*w,padded.begin()+static_cast<size_t>(y+1)*pw+1);
    Dataset source(mem->Create("",pw,ph,1,GDT_Byte,nullptr));
    Dataset distance(mem->Create("",pw,ph,1,GDT_Float32,nullptr));
    require(source && distance,SE_ERROR_GDAL_CREATE_FAILED,"无法创建连接宽度距离栅格");
    Dataset frameMask(mem->Create("",pw,ph,1,GDT_Byte,nullptr));
    require(frameMask!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"无法创建内图廓距离掩膜");
    double transform[]={left-result.pixel,result.pixel,0,top+result.pixel,0,-result.pixel};
    frameMask->SetGeoTransform(transform);
    int frameBand=1;double burn=1;
    OGRGeometryH frameHandle=OGRGeometry::ToHandle(const_cast<OGRGeometry*>(frame));
    require(GDALRasterizeGeometries(frameMask.get(),1,&frameBand,1,&frameHandle,nullptr,nullptr,&burn,nullptr,nullptr,nullptr)==CE_None,
            SE_ERROR_FAILURE,"计算内图廓距离掩膜失败");
    std::vector<unsigned char> inside(static_cast<size_t>(pw)*ph,0);
    require(frameMask->GetRasterBand(1)->RasterIO(GF_Read,0,0,pw,ph,inside.data(),pw,ph,GDT_Byte,0,0,nullptr)==CE_None,
            SE_ERROR_FAILURE,"读取内图廓掩膜失败");
    for (size_t i=0;i<padded.size();++i) if (!inside[i]) padded[i]=0;
    require(source->GetRasterBand(1)->RasterIO(GF_Write,0,0,pw,ph,padded.data(),pw,ph,GDT_Byte,0,0,nullptr)==CE_None,
            SE_ERROR_WRITE_FILE_FAILED,"写入连接宽度掩膜失败");
    char** options=nullptr;options=CSLSetNameValue(options,"VALUES","0");
    options=CSLSetNameValue(options,"DISTUNITS","PIXEL");
    const CPLErr status=GDALComputeProximity(source->GetRasterBand(1),distance->GetRasterBand(1),options,nullptr,nullptr);
    CSLDestroy(options);require(status==CE_None,SE_ERROR_FAILURE,"计算空白连接宽度距离失败");
    std::vector<float> distances(static_cast<size_t>(pw)*ph);
    require(distance->GetRasterBand(1)->RasterIO(GF_Read,0,0,pw,ph,distances.data(),pw,ph,GDT_Float32,0,0,nullptr)==CE_None,
            SE_ERROR_FAILURE,"读取连接宽度距离失败");
    std::vector<unsigned char> core(static_cast<size_t>(n),0),seen(static_cast<size_t>(n),0);
    // Center-to-background distance minus half a working cell estimates boundary clearance.
    const double cutoff=minimumWidth/(2*result.pixel)+0.5;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const int index=y*w+x;
        core[index]=empty[index] && distances[static_cast<size_t>(y+1)*pw+x+1]>cutoff+1e-7;
    }
    auto neighbors=[&](int index,const std::function<void(int)>& action) {
        const int x=index%w,y=index/w;
        if (x>0) action(index-1);if (x+1<w) action(index+1);
        if (y>0) action(index-w);if (y+1<h) action(index+w);
    };
    std::deque<int> queue;int next=0;
    for (int start=0;start<n;++start) if (core[start] && !result.labels[start]) {
        ++next;result.labels[start]=next;queue.push_back(start);
        while (!queue.empty()) {
            const int current=queue.front();queue.pop_front();
            neighbors(current,[&](int j) { if (core[j] && !result.labels[j]) {result.labels[j]=next;queue.push_back(j);} });
        }
    }
    result.cores=next;
    for (int start=0;start<n;++start) if (empty[start] && !seen[start]) {
        std::vector<int> component;bool hasCore=false;
        seen[start]=1;queue.push_back(start);
        while (!queue.empty()) {
            const int current=queue.front();queue.pop_front();component.push_back(current);
            hasCore=hasCore || result.labels[current]>0;
            neighbors(current,[&](int j) { if (empty[j] && !seen[j]) {seen[j]=1;queue.push_back(j);} });
        }
        if (!hasCore) {
            ++next;++result.preservedSmall;
            for (int index:component) result.labels[index]=next;
        }
    }
    for (int i=0;i<n;++i) if (result.labels[i]) queue.push_back(i);
    while (!queue.empty()) {
        const int current=queue.front();queue.pop_front();
        neighbors(current,[&](int j) { if (empty[j] && !result.labels[j]) {
            result.labels[j]=result.labels[current];queue.push_back(j);
        } });
    }
    for (int i=0;i<n;++i) require((result.labels[i]>0)==(empty[i]>0),SE_ERROR_FAILURE,"连接分区改变了空白范围");
    return result;
}

struct Cell { int x,y,size,depth; };
}

int GlobalBlankCheck(const char* boundaryPath,const char* occupiedPaths,const char* excludedLayers,const char* sourceDataPaths,
                     const char* sourceLayerNames,const char* schemaXmlPath,int scale,const char* severity,
                     const char* outputRegionPath,const char* outputJsonPath,const char* outputLogPath) {
    const QString xml=qpath(schemaXmlPath), out=qpath(outputRegionPath), json=qpath(outputJsonPath), log=qpath(outputLogPath);
    QFile logFile;
    bool canWriteFailureJson=false;
    auto message=[&](const QString& text) {
        if (logFile.isOpen()) { QTextStream stream(&logFile); stream.setCodec("UTF-8"); stream<<QDateTime::currentDateTime().toString(Qt::ISODate)<<" "<<text<<"\n"; stream.flush(); }
    };
    try {
        require(scale>0,SE_ERROR_INVALID_PARAMETER,"比例尺必须大于0");
        require(!xml.isEmpty() && !out.isEmpty() && !json.isEmpty() && !log.isEmpty(),SE_ERROR_INVALID_PARAMETER,"XML、结果图层、JSON和日志路径均不能为空");
        require(out!=json && out!=log && json!=log && xml!=out && xml!=json && xml!=log,SE_ERROR_INVALID_PARAMETER,"输入输出路径不能相同");
        const Rule rule=readRules(xml,scale);
        const QString boundary=resolvePath(qpath(boundaryPath));
        const QString paths=qpath(occupiedPaths);
        require(!boundary.isEmpty() && !paths.isEmpty(),SE_ERROR_INVALID_PARAMETER,"集成程序必须通过接口传入内图廓和待检图层文件夹（或图层列表），不从XML读取路径");
        const QFileInfo outputInfo(out);
        const QString cellsPath=outputInfo.dir().filePath(outputInfo.completeBaseName()+"_cells."+outputInfo.suffix());
        const QString allRegionsPath=outputInfo.dir().filePath(outputInfo.completeBaseName()+"_all."+outputInfo.suffix());
        const QString candidatePath=outputInfo.dir().filePath("全局空白检查_候选要素.gpkg");
        const QString candidateFolder=outputInfo.absolutePath();
        const QStringList outputPaths{out,json,log,cellsPath,candidatePath,allRegionsPath,outputInfo.dir().filePath("候选要素输出清单.json")};
        const QStringList allInputFiles=expandInputs(paths,false);
        QSet<QString> explicitInputFiles;
        for (const QString& entry:names(paths)) {
            const QString resolved=resolvePath(entry);
            if (QFileInfo(resolved).isFile()) explicitInputFiles.insert(normalizedPath(resolved));
        }
        const QStringList exclusions=names(qpath(excludedLayers));
        QStringList inputs,skippedInputs;
        for (const QString& path:allInputFiles) {
            bool generated=false;
            for (const QString& target:outputPaths) if (normalizedPath(path)==normalizedPath(target)) generated=true;
            require(!generated || !explicitInputFiles.contains(normalizedPath(path)),SE_ERROR_INVALID_PARAMETER,"输出不能覆盖显式输入图层："+path);
            if (generated || normalizedPath(path)==normalizedPath(boundary) || matchesLayer(QFileInfo(path).fileName(),exclusions,true))
                skippedInputs<<path;
            else inputs<<path;
        }
        require(!inputs.isEmpty(),SE_ERROR_NOT_ENOUGH_DATA,"排除后没有参与空白检查的图层，请检查文件夹和排除规则");
        const QString sourceValue=qpath(sourceDataPaths);
        const bool candidateEnabled=!sourceValue.isEmpty();
        QStringList requested=names(qpath(sourceLayerNames));
        require(candidateEnabled || requested.isEmpty(),SE_ERROR_INVALID_PARAMETER,"指定源图层名称时必须通过接口提供源数据路径");
        if (candidateEnabled && requested.isEmpty()) requested<<"*";
        const QString spatialRule=rule.values.value("候选要素空间关系","相交");
        require(spatialRule=="相交" || spatialRule=="完全位于",SE_ERROR_INVALID_PARAMETER,"候选要素空间关系仅支持相交或完全位于");
        const QStringList sourcePaths=candidateEnabled?expandInputs(sourceValue,true):QStringList();
        require(!candidateEnabled || !sourcePaths.isEmpty(),SE_ERROR_NOT_ENOUGH_DATA,"源数据目录没有可读取的数据");
        QStringList protectedPaths=inputs; protectedPaths<<sourcePaths<<boundary<<xml;
        QSet<QString> uniqueOutputs;
        for (const QString& target:outputPaths) {
            const QString normalized=QFileInfo(target).absoluteFilePath().toCaseFolded();
            require(!uniqueOutputs.contains(normalized),SE_ERROR_INVALID_PARAMETER,"输出路径冲突"); uniqueOutputs.insert(normalized);
            for (const QString& source:protectedPaths) {
                const QString sourcePath=normalizedPath(source);
                require(normalized!=sourcePath && !(QFileInfo(source).isDir() && normalized.startsWith(sourcePath+"/")),SE_ERROR_INVALID_PARAMETER,"输出不能覆盖输入数据或写入源数据库内部："+source);
            }
        }
        canWriteFailureJson=true;
        ensureParent(log); logFile.setFileName(log);
        require(logFile.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text),SE_ERROR_WRITE_FILE_FAILED,"无法写入日志");
        message("开始全局空白检查（122），仅提示空白，不执行自动补绘");
        for (const QString& skipped:skippedInputs) message("不参与占用计算："+skipped);
        message(QString("扫描得到%1个文件，实际参与%2个图层文件").arg(allInputFiles.size()).arg(inputs.size()));
        GDALAllRegister();
        require(OGRGeometryFactory::haveGEOS(),SE_ERROR_UNSUPPORTED_OPERATION,"当前GDAL未启用GEOS");
        Dataset boundaryDs=openVector(boundary); OGRLayer* boundaryLayer=boundaryDs->GetLayer(0);
        require(boundaryLayer->GetSpatialRef(),SE_ERROR_UNSUPPORTED_SRS,"内图廓缺少坐标系");
        OGRSpatialReference srs(*boundaryLayer->GetSpatialRef()); srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        require(srs.IsProjected(),SE_ERROR_GEOGRAPHIC_CRS,"内图廓须为平面投影坐标系，不能直接使用经纬度");
        const double metersPerUnit=srs.GetLinearUnits();
        require(std::isfinite(metersPerUnit) && metersPerUnit>0,SE_ERROR_UNSUPPORTED_SRS,"无法确定投影单位");
        OGRMultiPolygon boundaryParts;
        boundaryLayer->ResetReading();
        while (Feature f{boundaryLayer->GetNextFeature()}) {
            const auto* g=f->GetGeometryRef(); if (!g || g->IsEmpty()) continue;
            const auto type=wkbFlatten(g->getGeometryType());
            if (type==wkbPolygon || type==wkbMultiPolygon) {
                require(g->IsValid(),SE_ERROR_INVALID_GEOMETRY,"内图廓面无效，请先修复"); collectPolygons(g,boundaryParts);
            } else if (type==wkbLineString || type==wkbMultiLineString) {
                OGRGeometryCollection edges; edges.addGeometry(g);
                // Strict polygon construction: do not silently replace a broken frame by its envelope.
                OGRErr error=OGRERR_NONE;
                Geometry polygon(OGRGeometry::FromHandle(OGRBuildPolygonFromEdges(OGRGeometry::ToHandle(&edges),FALSE,FALSE,0,&error)));
                require(error==OGRERR_NONE && polygon && polygon->IsValid() && area(polygon.get())>0,SE_ERROR_INVALID_GEOMETRY,"内图廓线不能构成闭合面；请先转换为面");
                collectPolygons(polygon.get(),boundaryParts);
            } else throw Failure(SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"内图廓只支持面或可闭合的线");
        }
        require(!boundaryParts.IsEmpty(),SE_ERROR_NOT_ENOUGH_DATA,"内图廓为空");
        Geometry frame(boundaryParts.UnionCascaded());
        require(frame && frame->IsValid() && area(frame.get())>0,SE_ERROR_INVALID_GEOMETRY,"内图廓合并失败");
        std::vector<Occupancy> objects;
        int repairedGeometries=0;
        const double mmUnit=static_cast<double>(scale)/1000.0/metersPerUnit;
        for (const QString& path:inputs) {
            Dataset input=openVector(path); OGRLayer* layer=input->GetLayer(0);
            require(layer->GetSpatialRef(),SE_ERROR_UNSUPPORTED_SRS,"待检图层缺少坐标系："+path);
            OGRSpatialReference source(*layer->GetSpatialRef()); source.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            Transform transform;
            if (!source.IsSame(&srs)) {
                transform.reset(OGRCreateCoordinateTransformation(&source,&srs));
                require(transform!=nullptr,SE_ERROR_UNSUPPORTED_SRS,"无法转换图层坐标系："+path);
            }
            std::uint64_t read=0;
            while (Feature f{layer->GetNextFeature()}) {
                if (!f->GetGeometryRef() || f->GetGeometryRef()->IsEmpty()) continue;
                Geometry geom(f->GetGeometryRef()->clone());
                if (transform) require(geom->transform(transform.get())==OGRERR_NONE,SE_ERROR_UNSUPPORTED_SRS,"坐标转换失败："+path);
                const int originalDimension=geom->getDimension();
                if (!geom->IsValid()) {
                    Geometry repaired(geom->MakeValid());
                    require(repaired && !repaired->IsEmpty() && repaired->IsValid(),SE_ERROR_INVALID_GEOMETRY,"图层几何无法在内存中修复："+path+" FID="+QString::number(f->GetFID()));
                    geom=std::move(repaired); ++repairedGeometries;
                    message("内存修复无效几何（不修改源文件）："+path+" FID="+QString::number(f->GetFID()));
                }
                geom->flattenTo2D();
                const auto kind=wkbFlatten(geom->getGeometryType());
                double buffer=rule.marginMm;
                if (kind==wkbPoint || kind==wkbMultiPoint) {
                    require(!rule.prepared,SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"占用面模式不能输入点；请提供符号范围面或改用几何缓冲模式："+path);
                    buffer+=originalDimension==1?rule.lineMm/2.0:rule.pointMm;
                } else if (kind==wkbLineString || kind==wkbMultiLineString) {
                    require(!rule.prepared,SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"占用面模式不能输入线；请提供线宽占用面或改用几何缓冲模式："+path);
                    buffer+=rule.lineMm/2.0;
                } else if (kind==wkbGeometryCollection && !rule.prepared) {
                    buffer+=rule.lineMm/2.0;
                } else require(kind==wkbPolygon || kind==wkbMultiPolygon,SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"不支持混合几何图层："+path);
                if (buffer>0) {
                    geom.reset(geom->Buffer(buffer*mmUnit,8));
                    require(geom && geom->IsValid(),SE_ERROR_BUFFER_FAILED,"构造图面占用范围失败："+path);
                }
                Geometry clipped(geom->Intersection(frame.get()));
                require(clipped!=nullptr,SE_ERROR_INVALID_GEOMETRY,"占用范围裁切失败");
                if (area(clipped.get())>0) {
                    Occupancy item; item.geometry=std::move(clipped); item.geometry->getEnvelope(&item.envelope); objects.push_back(std::move(item));
                }
                ++read;
            }
            message(QString("读取图层 %1：%2 条有效非空几何").arg(path).arg(static_cast<qulonglong>(read)));
        }
        OGREnvelope env; frame->getEnvelope(&env);
        const double initial=std::sqrt(rule.gridCm2)*scale/100.0/metersPerUnit;
        const int unit=1<<rule.depth;
        const double pixel=initial/unit;
        require(std::isfinite(initial) && std::isfinite(pixel) && pixel>0,SE_ERROR_INVALID_PARAMETER,"换算后的格网尺寸无效");
        const double rootsX=std::ceil((env.MaxX-env.MinX)/initial),rootsY=std::ceil((env.MaxY-env.MinY)/initial);
        require(rootsX>0 && rootsY>0 && rootsX<=rule.maxPixels && rootsY<=rule.maxPixels,SE_ERROR_NOT_ENOUGH_MEMORY,"初始格网数量超出限制");
        const std::uint64_t width64=static_cast<std::uint64_t>(rootsX)*unit,height64=static_cast<std::uint64_t>(rootsY)*unit;
        require(width64<=rule.maxPixels && height64<=rule.maxPixels && width64<=rule.maxPixels/height64 && width64<=INT_MAX && height64<=INT_MAX,SE_ERROR_NOT_ENOUGH_MEMORY,"格网数量超过内存限制，请增大格网边长");
        const int width=static_cast<int>(width64),height=static_cast<int>(height64);
        std::vector<unsigned char> mask(static_cast<size_t>(width)*height,0);
        std::vector<Cell> leaves;
        std::uint64_t visited=0,split=0,terminal=0,blankCells=0;
        message(QString("初始边长%1mm；最小边长%2mm；栅格%3×%4；占用对象%5")
            .arg(std::sqrt(rule.gridCm2)*10).arg(pixel/mmUnit).arg(width).arg(height).arg(static_cast<qulonglong>(objects.size())));
        message(QString("内部批处理格面积%1平方厘米（%2）；内部最多细分%3次（%4）；点半径%5mm，线宽%6mm")
            .arg(rule.gridCm2).arg(rule.automaticGrid?"自动":"显式覆盖").arg(rule.depth)
            .arg(rule.automaticDepth?"自动":"显式覆盖").arg(rule.pointMm).arg(rule.lineMm));
        message(QString("空白识别格网边长：图上%1毫米，对应实地%2米").arg(pixel/mmUnit,0,'g',12).arg(pixel*metersPerUnit,0,'g',12));
        std::function<void(int,int,int,int,const std::vector<size_t>&)> visit;
        visit=[&](int x,int y,int size,int depth,const std::vector<size_t>& candidates) {
            ++visited;
            const double left=env.MinX+x*pixel,top=env.MaxY-y*pixel;
            Geometry cell=rectangle(left,top-size*pixel,left+size*pixel,top);
            if (!frame->Intersects(cell.get())) return;
            std::vector<size_t> hits;
            for (size_t id:candidates) {
                const auto& item=objects[id]; const auto& e=item.envelope;
                if (e.MaxX<=left || e.MinX>=left+size*pixel || e.MaxY<=top-size*pixel || e.MinY>=top) continue;
                if (!item.geometry->Intersects(cell.get())) continue;
                Geometry intersection(item.geometry->Intersection(cell.get()));
                require(intersection!=nullptr,SE_ERROR_INVALID_GEOMETRY,"格网与占用范围求交失败");
                if (area(intersection.get())<=0) continue;
                if (item.geometry->Contains(cell.get())) return;
                hits.push_back(id);
            }
            if (hits.empty()) {
                for (int row=y;row<y+size;++row) std::fill(mask.begin()+static_cast<size_t>(row)*width+x,mask.begin()+static_cast<size_t>(row)*width+x+size,1);
                ++blankCells;
                if (rule.saveCells) leaves.push_back(Cell{x,y,size,depth});
                return;
            }
            if (depth==rule.depth) { ++terminal; return; }
            ++split; const int half=size/2;
            visit(x,y,half,depth+1,hits); visit(x+half,y,half,depth+1,hits);
            visit(x,y+half,half,depth+1,hits); visit(x+half,y+half,half,depth+1,hits);
        };
        std::vector<size_t> candidates(objects.size());
        for (size_t i=0;i<candidates.size();++i) candidates[i]=i;
        for (int y=0;y<height;y+=unit) for (int x=0;x<width;x+=unit) visit(x,y,unit,0,candidates);
        const BlankPartition partition=partitionBlankMask(mask,width,height,pixel,rule.connectionMm*mmUnit,rule.maxPixels,
                                                        frame.get(),env.MinX,env.MaxY);
        message(QString("最小连接宽度%1mm；分区工作格边长%2mm；保留无核心小区域%3个；全部空白范围保留")
            .arg(rule.connectionMm).arg(partition.pixel/mmUnit).arg(partition.preservedSmall));
        auto* mem=GetGDALDriverManager()->GetDriverByName("MEM");
        require(mem,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少MEM栅格驱动");
        Dataset raster(mem->Create("",partition.width,partition.height,1,GDT_Int32,nullptr));
        require(raster!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"无法创建合并栅格");
        double gt[]={env.MinX,partition.pixel,0,env.MaxY,0,-partition.pixel}; raster->SetGeoTransform(gt);
        char* wkt=nullptr; srs.exportToWkt(&wkt); raster->SetProjection(wkt); CPLFree(wkt);
        auto* band=raster->GetRasterBand(1); band->SetNoDataValue(0);
        require(band->RasterIO(GF_Write,0,0,partition.width,partition.height,const_cast<int*>(partition.labels.data()),partition.width,partition.height,GDT_Int32,0,0,nullptr)==CE_None,SE_ERROR_WRITE_FILE_FAILED,"写入合并栅格失败");
        // Default GDALPolygonize uses 4-connectivity, not diagonal adjacency.
        auto* memory=GetGDALDriverManager()->GetDriverByName("Memory");
        require(memory,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少Memory矢量驱动");
        Dataset polygons(memory->Create("",0,0,0,GDT_Unknown,nullptr));
        require(polygons!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"无法创建区域合并数据集");
        auto* merged=polygons->CreateLayer("blank",&srs,wkbPolygon,nullptr);
        require(merged,SE_ERROR_LAYER_CREATE_FAILED,"无法创建区域合并图层");
        OGRFieldDefn value("value",OFTInteger); merged->CreateField(&value);
        require(GDALPolygonize(band,band->GetMaskBand(),merged,0,nullptr,nullptr,nullptr)==CE_None,SE_ERROR_FAILURE,"共边区域合并失败");
        VectorOutput regions(out,srs,false);
        QJsonObject errors; QJsonArray details;
        std::vector<BlankRegion> blankRegions;
        const double areaFactor=metersPerUnit*metersPerUnit*10000.0/(static_cast<double>(scale)*scale);
        int mergedCount=0,retained=0; double totalCm2=0,retainedCm2=0;
        while (Feature f{merged->GetNextFeature()}) {
            Geometry clipped(f->GetGeometryRef()->Intersection(frame.get()));
            require(clipped!=nullptr,SE_ERROR_INVALID_GEOMETRY,"合并区域与内图廓裁切失败");
            OGRMultiPolygon parts; collectPolygons(clipped.get(),parts);
            for (int i=0;i<parts.getNumGeometries();++i) {
                const auto* g=parts.getGeometryRef(i); const double a=area(g); if (a<=0) continue;
                ++mergedCount; const double cm2=a*areaFactor; totalCm2+=cm2;
                if (cm2<=rule.minCm2) continue;
                ++retained; retainedCm2+=cm2;
                regions.add(g,retained,cm2,a*metersPerUnit*metersPerUnit,-1);
                if (candidateEnabled) {
                    BlankRegion blank; blank.id=retained; blank.geometry.reset(g->clone());
                    blank.geometry->getEnvelope(&blank.envelope); blankRegions.push_back(std::move(blank));
                }
                OGRPoint point; g->PointOnSurface(&point);
                QJsonObject detail{{"region_id",retained},{"area_cm2",cm2},{"area_m2",a*metersPerUnit*metersPerUnit},
                    {"position_x",point.getX()},{"position_y",point.getY()},
                    {"description",QString("空白区域%1：图上面积%2平方厘米，大于提示阈值%3平方厘米").arg(retained).arg(cm2,0,'f',4).arg(rule.minCm2)},
                    {"is_hint",true}};
                details.append(detail); errors[QString("error%1").arg(retained)]=detail;
            }
        }
        regions.ds.reset(); // Flush output before reporting success.
        if (rule.saveCells) {
            VectorOutput cells(cellsPath,srs,true); int id=0;
            for (const auto& leaf:leaves) {
                const double left=env.MinX+leaf.x*pixel,top=env.MaxY-leaf.y*pixel;
                Geometry c=rectangle(left,top-leaf.size*pixel,left+leaf.size*pixel,top);
                Geometry clipped(c->Intersection(frame.get()));
                require(clipped!=nullptr,SE_ERROR_INVALID_GEOMETRY,"空白格裁切失败");
                const double a=area(clipped.get());
                if (a>0) cells.add(clipped.get(),++id,a*areaFactor,a*metersPerUnit*metersPerUnit,leaf.depth);
            }
        }
        CandidateResult candidatesResult;
        if (candidateEnabled) {
            candidatesResult=extractCandidates(sourcePaths,requested,spatialRule,blankRegions,srs,candidatePath,message);
            exportCandidateShapes(candidatesResult,candidatePath,candidateFolder,srs,protectedPaths+outputPaths);
            for (int i=0;i<details.size();++i) {
                QJsonObject detail=details[i].toObject(); const int id=detail["region_id"].toInt();
                const QJsonArray entries=candidatesResult.byRegion.value(id);
                detail["candidate_count"]=entries.size(); detail["candidates"]=entries;
                detail["candidate_layer_file"]=candidatePath;
                details[i]=detail; errors[QString("error%1").arg(id)]=detail;
            }
        }
        QJsonObject info{{"description",QString("共识别%1个空白区域，输出其中%2个面积大于规则阈值的区域").arg(mergedCount).arg(retained)},
            {"scale",scale},{"initial_grid_area_cm2",rule.gridCm2},{"max_depth",rule.depth},
            {"minimum_cell_side_mm",pixel/mmUnit},{"minimum_blank_area_cm2",rule.minCm2},
            {"minimum_connection_width_mm",rule.connectionMm},{"connection_partition_cell_side_mm",partition.pixel/mmUnit},
            {"connection_partition_method",rule.connectionMm>0?"近似收缩核心、四邻域分区恢复，保留全部空白":"不控制"},
            {"preserved_small_components",partition.preservedSmall},
            {"automatic_grid",rule.automaticGrid},{"automatic_depth",rule.automaticDepth},
            {"target_cell_side_mm",pixel/mmUnit},{"point_radius_mm",rule.pointMm},{"line_width_mm",rule.lineMm},
            {"visited_cells",static_cast<double>(visited)},{"subdivided_cells",static_cast<double>(split)},
            {"discarded_terminal_cells",static_cast<double>(terminal)},{"blank_leaf_cells",static_cast<double>(blankCells)},
            {"merged_regions",mergedCount},{"retained_regions",retained},
            {"total_blank_area_cm2",totalCm2},{"retained_area_cm2",retainedCm2},
            {"region_layer",out},{"area_threshold_comparison",">"},{"empty_cell_layer",rule.saveCells?cellsPath:QString()},
            {"participating_layers",QJsonArray::fromStringList(inputs)},{"excluded_files",QJsonArray::fromStringList(skippedInputs)},
            {"candidate_enabled",candidateEnabled},{"candidate_file",candidateEnabled?candidatePath:QString()},
            {"repaired_input_geometries",repairedGeometries},
            {"candidate_shapefile_directory",candidateEnabled?candidateFolder:QString()},
            {"candidate_feature_count",candidatesResult.features},{"candidate_association_count",candidatesResult.associations},
            {"candidate_layers",candidatesResult.layers},{"candidate_spatial_rule",spatialRule},
            {"candidate_region_scope","面积大于规则阈值的空白区域"},{"candidate_region_layer",out},
            {"input_mode",rule.prepared?QString("占用面"):QString("几何缓冲")},
            {"note","区域为格网精度下的保守近似；窄通道不主动切断；源要素仅按空间关系提取供复核，未判定必须上图、未去重已表示源对象，不执行自动补绘"}};
        // Keep technical statistics and source associations in the log, not the client JSON.
        message("诊断数据："+QString::fromUtf8(QJsonDocument(QJsonObject{{"info",info},{"regions",details},{"severity",qpath(severity)}}).toJson(QJsonDocument::Compact)));
        QJsonObject conciseErrors;QMap<int,QString> candidateSummaries;
        for(const auto& entry:details){auto detail=entry.toObject();const int id=detail["region_id"].toInt();
            QString description=detail["description"].toString();QMap<QString,QSet<QString>> counts;
            for(const auto& value:detail["candidates"].toArray()){auto candidate=value.toObject();counts[candidate["source_layer"].toString()].insert(candidate["source_path"].toString()+"|"+candidate["source_fid"].toString());}
            QStringList candidates;for(auto it=counts.constBegin();it!=counts.constEnd();++it)candidates<<QString("%1：%2个").arg(it.key()).arg(it.value().size());
            const QString candidateText=!candidateEnabled?QString("未输入源数据，未提取候选要素"):candidates.isEmpty()?QString("无候选要素"):candidates.join("、");
            candidateSummaries[id]=candidateText;
            description+="；候选要素："+candidateText;
            conciseErrors["error"+QString::number(id)]=description;
        }
        updateCandidateSummary(out,candidateSummaries,message);
        QString summary=QString("发现%1个图上面积大于%2平方厘米的空白区域").arg(retained).arg(rule.minCm2);
        if(candidateEnabled)summary+=QString("，提取%1个候选要素").arg(candidatesResult.features);
        else summary+="；未输入源数据，仅检查空白区域";
        writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","122"},{"name","全局空白检查"},{"status","质检成功"},{"err_exist",retained>0},{"info",summary},{"errors",conciseErrors}}}});
        message(info["description"].toString());
        message("完成；共边合并后再按面积过滤，角点接触不合并");
        return SE_ERROR_NONE;
    } catch (const std::exception& e) {
        const auto* failure=dynamic_cast<const Failure*>(&e);
        const int code=failure?failure->code:SE_ERROR_FAILURE;
        const QString text=QString::fromUtf8(e.what()); message("失败："+text);
        // Do not overwrite an input/configuration when caller supplied conflicting paths.
        if (canWriteFailureJson) {
            try { writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","122"},{"name","全局空白检查"},{"status","质检失败"},{"err_exist",true},{"info",text},{"errors",QJsonObject{{"execution_error",text}}}}}}); } catch (...) {}
        }
        return code;
    }
}
