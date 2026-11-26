#define _HAS_STD_BYTE 0
#include "se_feature_conflict_check.h"
#include <QJsonParseError>
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
#include <QTextStream>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <algorithm>
#include <cmath>
#include <climits>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
struct Failure:std::runtime_error {
    int code;Failure(int value,const QString& s):std::runtime_error(s.toUtf8().constData()),code(value){}
};
void need(bool condition,int code,const QString& text){if(!condition)throw Failure(code,text);}
QString str(const char* s){return QString::fromUtf8(s?s:"").trimmed();}
QString absolute(const QString& p){return p.isEmpty()?QString():QDir::cleanPath(QFileInfo(p).absoluteFilePath());}
QString keyPath(const QString& p){return absolute(p).toCaseFolded();}
QStringList list(const QString& s){QStringList out;for(const auto& p:s.split(';',Qt::SkipEmptyParts))if(!p.trimmed().isEmpty())out<<absolute(p.trimmed());return out;}
struct GD{void operator()(OGRGeometry* g)const{OGRGeometryFactory::destroyGeometry(g);}};
struct DD{void operator()(GDALDataset* d)const{if(d)GDALClose(d);}};
struct FD{void operator()(OGRFeature* f)const{if(f)OGRFeature::DestroyFeature(f);}};
struct TD{void operator()(OGRCoordinateTransformation* t)const{if(t)OCTDestroyCoordinateTransformation(t);}};
using Geom=std::unique_ptr<OGRGeometry,GD>;
using DS=std::unique_ptr<GDALDataset,DD>;
using Feat=std::unique_ptr<OGRFeature,FD>;
using CT=std::unique_ptr<OGRCoordinateTransformation,TD>;
void parent(const QString& p){need(QDir().mkpath(QFileInfo(p).absolutePath()),SE_ERROR_WRITE_FILE_FAILED,"无法创建输出目录："+p);}
void writeJson(const QString& p,const QJsonObject& value){
    parent(p);QSaveFile f(p);need(f.open(QIODevice::WriteOnly),SE_ERROR_WRITE_FILE_FAILED,"无法写入JSON："+p);
    const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Indented);
    need(f.write(bytes)==bytes.size()&&f.commit(),SE_ERROR_WRITE_FILE_FAILED,"保存JSON失败："+p);
}
DS open(const QString& p){
    DS d(static_cast<GDALDataset*>(GDALOpenEx(p.toUtf8().constData(),GDAL_OF_VECTOR|GDAL_OF_READONLY,nullptr,nullptr,nullptr)));
    need(d&&d->GetLayerCount()>0,SE_ERROR_GDAL_OPEN_FAILED,"无法读取矢量数据："+p);return d;
}
OGRSpatialReference layerSrs(OGRLayer* l){
    need(l->GetSpatialRef()!=nullptr,SE_ERROR_UNSUPPORTED_SRS,"图层缺少坐标系："+QString::fromUtf8(l->GetName()));
    OGRSpatialReference s(*l->GetSpatialRef());s.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);return s;
}
CT transformation(OGRLayer* l,OGRSpatialReference& target){
    auto s=layerSrs(l);if(s.IsSame(&target))return CT();
    CT t(OGRCreateCoordinateTransformation(&s,&target));need(t!=nullptr,SE_ERROR_UNSUPPORTED_SRS,"无法转换输入图层坐标系");return t;
}
Geom geometry(OGRFeature* f,CT& t,const QString& context,int* repaired=nullptr){
    if(!f->GetGeometryRef()||f->GetGeometryRef()->IsEmpty())return Geom();
    Geom g(f->GetGeometryRef()->clone());
    if(t)need(g->transform(t.get())==OGRERR_NONE,SE_ERROR_UNSUPPORTED_SRS,"坐标转换失败："+context);
    OGREnvelope e;g->getEnvelope(&e);
    need(std::isfinite(e.MinX)&&std::isfinite(e.MaxX)&&std::isfinite(e.MinY)&&std::isfinite(e.MaxY),SE_ERROR_INVALID_GEOMETRY,"非有限坐标："+context);
    if(!g->IsValid()&&repaired){Geom fixed(g->MakeValid());need(fixed&&!fixed->IsEmpty()&&fixed->IsValid(),SE_ERROR_INVALID_GEOMETRY,"范围几何修复失败："+context);g=std::move(fixed);++*repaired;}
    need(g->IsValid(),SE_ERROR_INVALID_GEOMETRY,"无效几何："+context);return g;
}
void polygons(const OGRGeometry* g,OGRMultiPolygon& out){
    const auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbPolygon){out.addGeometry(g);return;}
    if(type==wkbMultiPolygon||type==wkbGeometryCollection){const auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)polygons(c->getGeometryRef(i),out);}
}
Geom rect(double x0,double y0,double x1,double y1){
    OGRLinearRing r;r.addPoint(x0,y0);r.addPoint(x1,y0);r.addPoint(x1,y1);r.addPoint(x0,y1);r.addPoint(x0,y0);
    auto* p=new OGRPolygon();p->addRing(&r);return Geom(p);
}
Geom loadArea(const QString& path,OGRSpatialReference& target,bool allowLine,int* repaired){
    DS d=open(path);OGRMultiPolygon all;
    for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);auto t=transformation(l,target);
        while(Feat f{l->GetNextFeature()}){
            auto g=geometry(f.get(),t,path,repaired);if(!g)continue;
            if(allowLine&&wkbFlatten(g->getGeometryType())==wkbLineString){
                const auto* line=g->toLineString();need(line->getNumPoints()>=4&&line->get_IsClosed(),SE_ERROR_INVALID_GEOMETRY,"内图廓线必须闭合");
                OGRLinearRing ring;for(int i=0;i<line->getNumPoints();++i)ring.addPoint(line->getX(i),line->getY(i));
                auto* p=new OGRPolygon();p->addRing(&ring);g.reset(p);
            }
            need(g->getDimension()==2,SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"范围图层需要面（内图廓也支持闭合线）："+path);
            polygons(g.get(),all);
        }
    }
    need(!all.IsEmpty(),SE_ERROR_NOT_ENOUGH_DATA,"范围图层没有有效面："+path);
    Geom result(all.UnionCascaded());need(result&&!result->IsEmpty()&&result->IsValid(),SE_ERROR_INVALID_GEOMETRY,"范围面合并失败："+path);return result;
}
struct AttributeCondition {QString field,op,value;int number=1;};
bool passesSingle(OGRFeature* f,int index,const AttributeCondition& r){
    const bool null=!f->IsFieldSetAndNotNull(index);const QString value=null?QString():QString::fromUtf8(f->GetFieldAsString(index));
    const bool empty=null||value.trimmed().isEmpty();
    if(r.value=="为空")return empty;if(r.value=="不为空")return !empty;
    if(null)return false;
    if(r.op=="包含")return value.contains(r.value);
    const auto type=f->GetDefnRef()->GetFieldDefn(index)->GetType();bool ok=false;
    if(type==OFTInteger||type==OFTInteger64){const auto expected=r.value.toLongLong(&ok);need(ok,SE_ERROR_XML_PARSE_FAILED,"整数字段筛选值不是有效整数："+r.field);return f->GetFieldAsInteger64(index)==expected;}
    if(type==OFTReal){const double expected=r.value.toDouble(&ok);need(ok&&std::isfinite(expected),SE_ERROR_XML_PARSE_FAILED,"数值字段筛选值不是有限数："+r.field);return f->GetFieldAsDouble(index)==expected;}
    return value==r.value;
}
bool passes(OGRFeature* f,int index,const AttributeCondition& r){
    QString text=r.value;text.replace(QChar(0xff1b),QChar(';'));
    const auto values=text.split(';',Qt::SkipEmptyParts);bool matched=false,hasValue=false;
    for(const auto& value:values){AttributeCondition single=r;single.value=value.trimmed();if(single.value.isEmpty())continue;hasValue=true;
        if(single.value=="为空"||single.value=="不为空")need(r.op.isEmpty()||r.op=="等于",SE_ERROR_XML_PARSE_FAILED,"空值筛选方式只能留空或填等于");
        // Evaluate every alternative so malformed numeric values cannot be hidden by an earlier match.
        const bool hit=passesSingle(f,index,single);matched=matched||hit;
    }
    need(hasValue,SE_ERROR_XML_PARSE_FAILED,"属性筛选值列表不能为空："+r.field);return matched;
}
struct Output {
    DS ds;OGRLayer* layer=nullptr;
    Output(const QString& path,OGRSpatialReference& srs,OGRwkbGeometryType geometryType,bool enabled=true){
        parent(path);const auto ext=QFileInfo(path).suffix().toLower();const char* name=ext=="shp"?"ESRI Shapefile":ext=="geojson"?"GeoJSON":ext=="gpkg"?"GPKG":nullptr;
        need(name,SE_ERROR_INVALID_PARAMETER,"输出图层格式必须为SHP、GeoJSON或GPKG");auto* driver=GetGDALDriverManager()->GetDriverByName(name);need(driver,SE_ERROR_GDAL_DRIVER_NOT_FOUND,"缺少输出驱动");
        if(QFileInfo::exists(path))need(driver->Delete(path.toUtf8().constData())==CE_None,SE_ERROR_WRITE_FILE_FAILED,"无法覆盖输出："+path);
        if(!enabled)return;
        ds.reset(driver->Create(path.toUtf8().constData(),0,0,0,GDT_Unknown,nullptr));need(ds!=nullptr,SE_ERROR_GDAL_CREATE_FAILED,"创建输出失败："+path);
        char** options=nullptr;if(ext=="shp")options=CSLSetNameValue(options,"ENCODING","UTF-8");
        layer=ds->CreateLayer(QFileInfo(path).completeBaseName().toUtf8().constData(),&srs,geometryType,options);CSLDestroy(options);
        need(layer,SE_ERROR_LAYER_CREATE_FAILED,"创建输出图层失败");
    }
    void field(const char* name,OGRFieldType type){if(!layer||layer->GetLayerDefn()->GetFieldIndex(name)>=0)return;OGRFieldDefn d(name,type);if(type==OFTString)d.SetWidth(254);need(layer->CreateField(&d)==OGRERR_NONE,SE_ERROR_FIELD_CREATE_FAILED,"创建字段失败");}
    void write(OGRFeature* f){need(layer->CreateFeature(f)==OGRERR_NONE,SE_ERROR_FEATURE_CREATE_FAILED,"写入输出要素失败");}
};
struct ConflictRule {int number=0;QString layer,mode;double distanceMm=0;std::vector<AttributeCondition> conditions;};
struct Config {std::vector<AttributeCondition> targetConditions;std::vector<ConflictRule> references;};
std::vector<AttributeCondition> conditions(const QMap<QString,QString>& values,const QString& prefix){
    QSet<int> numbers{1};const QRegularExpression re("^"+QRegularExpression::escape(prefix)+"属性筛选(?:字段|方式|值)([0-9]+)$");
    for(auto it=values.constBegin();it!=values.constEnd();++it){auto match=re.match(it.key());if(match.hasMatch()){
        bool ok=false;int n=match.captured(1).toInt(&ok);need(ok&&n>=2&&n<=1000&&QString::number(n)==match.captured(1),SE_ERROR_XML_PARSE_FAILED,"属性条件编号不合法："+it.key());numbers.insert(n);}}
    auto ordered=numbers.values();std::sort(ordered.begin(),ordered.end());std::vector<AttributeCondition> out;
    for(int n:ordered){QString suffix=n==1?QString():QString::number(n);AttributeCondition c;c.number=n;
        c.field=values.value(prefix+"属性筛选字段"+suffix);c.op=values.value(prefix+"属性筛选方式"+suffix);c.value=values.value(prefix+"属性筛选值"+suffix);
        if(c.field.isEmpty()&&c.op.isEmpty()&&c.value.isEmpty())continue;
        need(!c.field.isEmpty()&&!c.value.isEmpty(),SE_ERROR_XML_PARSE_FAILED,prefix+"属性筛选字段和值必须同时填写");
        auto text=c.value;text.replace(QChar(0xff1b),';');QStringList alternatives;for(const auto& value:text.split(';',Qt::SkipEmptyParts))if(!value.trimmed().isEmpty())alternatives<<value.trimmed();
        need(!alternatives.isEmpty(),SE_ERROR_XML_PARSE_FAILED,prefix+"属性筛选值列表为空");
        bool special=false;for(const auto& value:alternatives)if(value=="为空"||value=="不为空")special=true;
        if(special)need(alternatives.size()==1&&(c.op.isEmpty()||c.op=="等于"),SE_ERROR_XML_PARSE_FAILED,"为空/不为空须单独使用，方式留空或等于");
        else need(c.op=="等于"||c.op=="包含"||c.op=="不包含",SE_ERROR_XML_PARSE_FAILED,"筛选方式支持等于、包含、不包含");out.push_back(c);
    }return out;
}
Config conflictConfig(const QString& path,int scale){
    QFile file(path);QDomDocument doc;need(file.open(QIODevice::ReadOnly)&&doc.setContent(&file),SE_ERROR_XML_PARSE_FAILED,"无法读取规则XML："+path);
    need(doc.documentElement().tagName()=="check_list",SE_ERROR_XML_PARSE_FAILED,"XML根节点须为check_list");
    std::vector<QDomElement> exact,generic;auto scales=doc.elementsByTagName("scale");
    for(int i=0;i<scales.size();++i){auto e=scales.at(i).toElement();const auto text=e.firstChildElement("value").text().trimmed();bool ok=false;int n=text.toInt(&ok);
        bool specific=ok&&n==scale,common=text.isEmpty()||(ok&&n==0);if(!specific&&!common)continue;
        auto items=e.elementsByTagName("check_item");for(int j=0;j<items.size();++j){auto item=items.at(j).toElement();if(item.firstChildElement("id").text().trimmed()=="123"||item.firstChildElement("name").text().trimmed()=="要素表示冲突检查")(specific?exact:generic).push_back(item);}}
    const auto selected=exact.empty()?generic:exact;need(selected.size()==1,SE_ERROR_XML_PARSE_FAILED,"需要唯一匹配当前比例尺的123要素表示冲突检查配置");
    QMap<QString,QString> values,units;auto rules=selected.front().elementsByTagName("rule");QSet<int> refs;
    for(int i=0;i<rules.size();++i){auto r=rules.at(i).toElement();auto name=r.firstChildElement("name").text().trimmed();need(!values.contains(name),SE_ERROR_XML_PARSE_FAILED,"重复规则："+name);
        values[name]=r.firstChildElement("value").text().trimmed();units[name]=r.firstChildElement("unit").text().trimmed().toLower();
        auto match=QRegularExpression("^关联图层([0-9]+)").match(name);if(match.hasMatch()){bool ok=false;int n=match.captured(1).toInt(&ok);need(ok&&n>=1&&n<=1000&&QString::number(n)==match.captured(1),SE_ERROR_XML_PARSE_FAILED,"关联编号须为1至1000");refs.insert(n);}}
    need(!refs.isEmpty(),SE_ERROR_XML_PARSE_FAILED,"未配置关联图层");Config config;config.targetConditions=conditions(values,"");auto ordered=refs.values();std::sort(ordered.begin(),ordered.end());
    const QMap<QString,double> factors{{"",1},{"毫米",1},{"mm",1},{"厘米",10},{"cm",10},{"米",1000},{"m",1000}};
    // Keep an owning QString here: with QT_USE_QSTRINGBUILDER, `auto` would retain
    // a reference to the temporary returned by QString::number().
    for(int n:ordered){
        const QString prefix =
            QStringLiteral("关联图层") + QString::number(n); ConflictRule r;r.number=n;r.layer=values.value(prefix);r.mode=values.value(prefix+"判定方式");r.conditions=conditions(values,prefix);
        need(!r.layer.isEmpty(),SE_ERROR_XML_PARSE_FAILED,prefix+"名称未填写");need(r.mode=="不能压盖"||r.mode=="最小间距",SE_ERROR_XML_PARSE_FAILED,prefix+"判定方式应为不能压盖或最小间距");
        if(r.mode=="最小间距"){bool ok=false;double distance=values.value(prefix+"最小间距").toDouble(&ok);const auto unit=units.value(prefix+"最小间距");need(ok&&factors.contains(unit),SE_ERROR_XML_PARSE_FAILED,prefix+"最小间距或单位不合法");r.distanceMm=distance*factors.value(unit);need(std::isfinite(r.distanceMm)&&r.distanceMm>0,SE_ERROR_INVALID_PARAMETER,prefix+"最小间距必须大于0");}config.references.push_back(r);
    }return config;
}
bool conflictPasses(OGRFeature* f,int field,const AttributeCondition& c){
    if(c.op!="不包含")return passes(f,field,c);
    if(!f->IsFieldSetAndNotNull(field))return true;
    AttributeCondition positive=c;positive.op="包含";return !passes(f,field,positive);
}
struct ConflictRecord {QString identity,path,layer,symbol,name;GIntBig fid;Geom geometry;};
struct ConflictObject {QString key,symbol;std::vector<int> records;QSet<QString> identities;OGREnvelope box;Geom occupied;};
struct ParsedData {std::vector<ConflictRecord> records;std::vector<ConflictObject> objects;};
using Log=std::function<void(const QString&)>;
Geom occupiedGeometry(const std::vector<int>& rows,const std::vector<ConflictRecord>& records){
    OGRGeometryCollection original;OGRMultiLineString lines;
    std::function<void(const OGRGeometry*)> collect=[&](const OGRGeometry* g){auto type=wkbFlatten(g->getGeometryType());
        if(type==wkbLineString){lines.addGeometry(g);return;}
        if(type==wkbMultiLineString||type==wkbGeometryCollection){const auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)collect(c->getGeometryRef(i));}};
    for(int i:rows){const auto* g=records[i].geometry.get();original.addGeometry(g);collect(g);}
    if(!lines.IsEmpty()){
        // Noding combines split edges and intersections before polygonization; no buffer or forced gap closure.
        Geom noded(lines.Union(&lines));need(noded!=nullptr,SE_ERROR_INVALID_GEOMETRY,"符号线节点化失败");
        OGRMultiLineString polygonizeInput;
        std::function<void(const OGRGeometry*)> flatten=[&](const OGRGeometry* g){
            if(wkbFlatten(g->getGeometryType())==wkbLineString){polygonizeInput.addGeometry(g);return;}
            const auto* collection=g->toGeometryCollection();for(int i=0;i<collection->getNumGeometries();++i)flatten(collection->getGeometryRef(i));
        };flatten(noded.get());Geom polygons(polygonizeInput.Polygonize());
        need(polygons!=nullptr,SE_ERROR_INVALID_GEOMETRY,"符号闭合轮廓构面失败");if(!polygons->IsEmpty())original.addGeometry(polygons.get());
    }
    return Geom(original.clone());
}
std::shared_ptr<ParsedData> loadParsed(const QString& paths,OGRSpatialReference& target,const Log& log){
    auto out=std::make_shared<ParsedData>();auto files=list(paths);need(!files.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少解析图层");QSet<QString> seen;QMap<QString,int> objects;
    for(const auto& path:files){if(seen.contains(keyPath(path)))continue;seen.insert(keyPath(path));log("读取解析图层："+path);auto d=open(path);
        for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);auto t=transformation(l,target);int symbolField=l->GetLayerDefn()->GetFieldIndex("symbol_id");
            while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,path);if(!g)continue;Geom linear(g->getLinearGeometry());need(linear!=nullptr,SE_ERROR_INVALID_GEOMETRY,"符号曲线线性化失败");
                const QString layer=QString::fromUtf8(l->GetName()),identity=keyPath(path)+"|"+layer+"|"+QString::number(f->GetFID());QString symbol;if(symbolField>=0&&f->IsFieldSetAndNotNull(symbolField))symbol=QString::fromUtf8(f->GetFieldAsString(symbolField)).trimmed();
                const QString key=symbol.isEmpty()?identity:"symbol:"+symbol;int index;
                if(objects.contains(key))index=objects.value(key);else{index=static_cast<int>(out->objects.size());ConflictObject o;o.key=key;o.symbol=symbol;out->objects.push_back(std::move(o));objects[key]=index;}
                auto& object=out->objects[index];OGREnvelope box;linear->getEnvelope(&box);if(object.records.empty())object.box=box;else object.box.Merge(box);object.identities.insert(identity);object.records.push_back(static_cast<int>(out->records.size()));
                QString displayName;int nameField=l->GetLayerDefn()->GetFieldIndex("NAME");if(nameField>=0&&f->IsFieldSetAndNotNull(nameField))displayName=QString::fromUtf8(f->GetFieldAsString(nameField));
                out->records.push_back({identity,path,layer,symbol,displayName,f->GetFID(),std::move(linear)});
            }
        }
    }
    for(auto& object:out->objects)object.occupied=occupiedGeometry(object.records,out->records);
    log(QString("完整符号%1个，原始记录%2条").arg(out->objects.size()).arg(out->records.size()));return out;
}
struct SourceInfo {QString path,layer,name;GIntBig fid=0;};
struct SourceRow {QString path,layer,name;GIntBig fid;Geom geometry;bool selected;};
std::vector<int> selectObjects(const ParsedData& parsed,const QString& sources,const QString& sourceLayers,const std::vector<AttributeCondition>& conditions,OGRSpatialReference& target,double tolerance,const QString& role,const Log& log,QMap<int,SourceInfo>* sourceInfo=nullptr){
    std::vector<int> chosen;if(conditions.empty()){for(int i=0;i<static_cast<int>(parsed.objects.size());++i)chosen.push_back(i);return chosen;}
    const auto paths=list(sources);need(!paths.empty(),SE_ERROR_INVALID_PARAMETER,role+"缺少属性筛选所需GDB/源数据图层");
    QStringList wanted;for(const auto& name:str(sourceLayers.toUtf8().constData()).split(';',Qt::SkipEmptyParts))wanted<<name.trimmed();
    std::vector<SourceRow> rows;QSet<QString> seen,matched;
    for(const auto& path:paths){if(seen.contains(keyPath(path)))continue;seen.insert(keyPath(path));auto d=open(path);need(d->GetLayerCount()==1||!wanted.isEmpty(),SE_ERROR_INVALID_PARAMETER,role+"多图层数据库必须指定源图层名称");
        for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);const auto name=QString::fromUtf8(l->GetName());if(!wanted.isEmpty()&&!wanted.contains(name))continue;matched.insert(name);auto t=transformation(l,target);std::vector<int> fields;
            for(const auto& c:conditions){int field=l->GetLayerDefn()->GetFieldIndex(c.field.toUtf8().constData());need(field>=0,SE_ERROR_CORRUPT_DATA,role+"源图层缺少属性字段："+c.field);fields.push_back(field);}
            while(Feat f{l->GetNextFeature()}){bool selected=true;for(size_t i=0;i<conditions.size();++i){const bool hit=conflictPasses(f.get(),fields[i],conditions[i]);selected=selected&&hit;}auto g=geometry(f.get(),t,path);if(g){QString displayName;int nameField=l->GetLayerDefn()->GetFieldIndex("NAME");if(nameField>=0&&f->IsFieldSetAndNotNull(nameField))displayName=QString::fromUtf8(f->GetFieldAsString(nameField));rows.push_back({path,name,displayName,f->GetFID(),std::move(g),selected});}}
        }
    }
    for(const auto& name:wanted)need(matched.contains(name),SE_ERROR_LAYER_NOT_FOUND,role+"未找到源图层："+name);
    const double tie=std::max(1e-9,tolerance*1e-9);std::map<int,std::vector<std::pair<double,int>>> claims;int unlocated=0,ambiguous=0;
    for(int i=0;i<static_cast<int>(parsed.objects.size());++i){const auto& o=parsed.objects[i];OGRPoint center((o.box.MinX+o.box.MaxX)/2,(o.box.MinY+o.box.MaxY)/2);double nearest=std::numeric_limits<double>::max();int match=-1;bool multiple=false;
        for(int j=0;j<static_cast<int>(rows.size());++j){double distance=center.Distance(rows[j].geometry.get());need(std::isfinite(distance)&&distance>=0,SE_ERROR_INVALID_GEOMETRY,role+"源对象定位距离计算失败");if(distance<nearest-tie){nearest=distance;match=j;multiple=false;}else if(std::abs(distance-nearest)<=tie)multiple=true;}
        if(match<0||nearest>tolerance){++unlocated;log(role+"未定位源数据，跳过："+o.key);continue;}if(multiple){++ambiguous;log(role+"源数据定位不唯一，跳过："+o.key);continue;}claims[match].push_back({nearest,i});
    }
    for(auto& entry:claims){auto& candidates=entry.second;std::sort(candidates.begin(),candidates.end());if(candidates.size()>1&&std::abs(candidates[0].first-candidates[1].first)<=tie){ambiguous+=static_cast<int>(candidates.size());log(role+"多个符号竞争同一源对象且距离相同，跳过");continue;}
        if(rows[entry.first].selected){const int index=candidates.front().second;chosen.push_back(index);if(sourceInfo){const auto& row=rows[entry.first];(*sourceInfo)[index]={row.path,row.layer,row.name,row.fid};}}if(candidates.size()>1)log(role+QString("同一源对象仅保留最近符号，跳过%1个其他符号").arg(candidates.size()-1));}
    std::sort(chosen.begin(),chosen.end());log(role+QString("筛选后%1个，源数据未定位%2个，定位歧义%3个").arg(chosen.size()).arg(unlocated).arg(ambiguous));return chosen;
}
}

int FeatureConflictCheck(const char* xmlPath,const char* targetParsedPaths,const char* targetSourcePaths,const char* targetSourceLayers,const char* associationInputsJson,int scale,const char* outputErrorPath,const char* outputJsonPath,const char* outputLogPath,double sourceMatchToleranceMm){
    const QString xml=absolute(str(xmlPath)),output=absolute(str(outputErrorPath)),json=absolute(str(outputJsonPath)),log=absolute(str(outputLogPath));QFile logfile;bool safe=false;
    auto message=[&](const QString& text){if(logfile.isOpen()){QTextStream stream(&logfile);stream.setCodec("UTF-8");stream<<QDateTime::currentDateTime().toString(Qt::ISODate)<<" "<<text<<"\n";stream.flush();}};
    try{
        QJsonParseError parseError;auto document=QJsonDocument::fromJson(str(associationInputsJson).toUtf8(),&parseError);QJsonObject associations=document.object();
        QStringList inputs=list(str(targetParsedPaths));inputs<<list(str(targetSourcePaths))<<xml;
        for(auto it=associations.constBegin();it!=associations.constEnd();++it){inputs<<list(it.value().toObject().value("parsed_paths").toString())<<list(it.value().toObject().value("source_paths").toString());}
        auto isSafe=[&](const QString& p){if(p.isEmpty())return false;for(const auto& input:inputs)if(!input.isEmpty()){const QFileInfo a(p),b(input);if(keyPath(p)==keyPath(input)||keyPath(p).startsWith(keyPath(input)+"/")||(a.absolutePath().compare(b.absolutePath(),Qt::CaseInsensitive)==0&&a.completeBaseName().compare(b.completeBaseName(),Qt::CaseInsensitive)==0))return false;}return true;};
        safe=isSafe(json)&&keyPath(json)!=keyPath(output)&&keyPath(json)!=keyPath(log);need(safe,SE_ERROR_INVALID_PARAMETER,"JSON路径为空或与输入输出冲突");
        need(isSafe(output)&&isSafe(log)&&keyPath(output)!=keyPath(log),SE_ERROR_INVALID_PARAMETER,"输出路径为空、重复或覆盖输入");
        // 封装层会先写入输入准备阶段的诊断信息。核心算法使用追加方式，
        // 避免在刚进入核心后发生进程级异常时把调用前日志清空。
        parent(log);logfile.setFileName(log);need(logfile.open(QIODevice::WriteOnly|QIODevice::Append),SE_ERROR_WRITE_FILE_FAILED,"无法写入日志");
        need(scale>0&&std::isfinite(sourceMatchToleranceMm)&&sourceMatchToleranceMm>=0,SE_ERROR_INVALID_PARAMETER,"比例尺或源匹配容差不合法");
        need(parseError.error==QJsonParseError::NoError&&document.isObject(),SE_ERROR_INVALID_PARAMETER,"关联图层输入必须为JSON对象");const auto config=conflictConfig(xml,scale);
        need(!list(str(targetParsedPaths)).empty(),SE_ERROR_INVALID_PARAMETER,"缺少待检解析图层");GDALAllRegister();
        auto first=open(list(str(targetParsedPaths)).front());auto srs=layerSrs(first->GetLayer(0));need(srs.IsProjected(),SE_ERROR_GEOGRAPHIC_CRS,"待检图层必须使用投影坐标系");const double meters=srs.GetLinearUnits(),mm=scale/1000.0/meters;
        need(std::isfinite(mm)&&mm>0,SE_ERROR_UNSUPPORTED_SRS,"无效坐标单位");
        std::map<QString,std::shared_ptr<ParsedData>> cache;
        auto parsed=[&](const QString& paths,const QString& role){auto files=list(paths);need(!files.empty(),SE_ERROR_INVALID_PARAMETER,"缺少"+role+"解析图层");QStringList keys;for(const auto& f:files)keys<<keyPath(f);keys.removeDuplicates();std::sort(keys.begin(),keys.end());QString key=keys.join(";");
            if(cache.count(key))return cache[key];try{auto d=loadParsed(paths,srs,message);cache[key]=d;return d;}catch(const Failure& e){throw Failure(e.code,role+"："+QString::fromUtf8(e.what()));}};
        QMap<int,SourceInfo> targetSourceInfo;auto target=parsed(str(targetParsedPaths),"待检");const auto targets=selectObjects(*target,str(targetSourcePaths),str(targetSourceLayers),config.targetConditions,srs,sourceMatchToleranceMm*mm,"待检",message,&targetSourceInfo);
        struct Reference {ConflictRule rule;std::shared_ptr<ParsedData> data;std::vector<int> chosen;QMap<int,SourceInfo> sourceInfo;};std::vector<Reference> references;
        QStringList skipped;
        for(const auto& rule:config.references){QString key=QString::number(rule.number);auto input=associations.value(key).toObject();
            QString reason;const auto files=list(input.value("parsed_paths").toString());
            if(files.isEmpty())reason="图层不存在或未提供解析图层";
            else for(const auto& path:files)if(!QFileInfo::exists(path)){reason="解析图层不存在："+path;break;}
            if(reason.isEmpty()&&!rule.conditions.empty()){
                const auto sourceFiles=list(input.value("source_paths").toString());if(sourceFiles.isEmpty())reason="未提供属性筛选所需源数据图层";
                else for(const auto& path:sourceFiles)if(!QFileInfo::exists(path)){reason="属性筛选所需源数据图层不存在："+path;break;}
            }
            if(!reason.isEmpty()){const QString text=QString("XML提到的关联图层%1[%2]：%3，所以未进行该组检查").arg(key).arg(rule.layer).arg(reason);skipped<<text;message(text);continue;}
            auto data=parsed(input.value("parsed_paths").toString(),"关联图层"+key);
            QMap<int,SourceInfo> sourceInfo;auto chosen=selectObjects(*data,input.value("source_paths").toString(),input.value("source_layers").toString(),rule.conditions,srs,sourceMatchToleranceMm*mm,"关联图层"+key,message,&sourceInfo);references.push_back({rule,data,chosen,sourceInfo});}
        auto describeObject=[&](const ParsedData& data,int index,const QMap<int,SourceInfo>& matched)->QString{
            const auto& object=data.objects[index];
            if(matched.contains(index)&&!matched[index].name.trimmed().isEmpty())return matched[index].name+"；symbol_id="+(object.symbol.isEmpty()?QString("无编号"):object.symbol);
            if(!object.symbol.isEmpty())return QString("symbol_id=")+object.symbol;
            QStringList fids;QString layer;for(int row:object.records){const auto& record=data.records[row];layer=record.layer;fids<<QString::number(record.fid);}
            return layer+" FID="+fids.join(",");
        };
        QJsonObject errors;QMap<int,int> counts;QMap<int,QStringList> descriptions;int errorNo=0;
        for(int i:targets){const auto& a=target->objects[i];int count=0;
            for(const auto& reference:references)for(int j:reference.chosen){const auto& b=reference.data->objects[j];bool self=false;for(const auto& id:a.identities)if(b.identities.contains(id)){self=true;break;}if(self)continue;
                OGREnvelope ae=a.box,be=b.box;double limit=reference.rule.distanceMm*mm;const double dx=std::max({0.0,ae.MinX-be.MaxX,be.MinX-ae.MaxX}),dy=std::max({0.0,ae.MinY-be.MaxY,be.MinY-ae.MaxY});if(reference.rule.mode=="不能压盖"&&(dx>0||dy>0))continue;if(reference.rule.mode=="最小间距"&&std::hypot(dx,dy)>=limit)continue;
                bool conflict=false;double distance=0;if(reference.rule.mode=="不能压盖")conflict=a.occupied->Intersects(b.occupied.get());else{distance=a.occupied->Distance(b.occupied.get());need(std::isfinite(distance)&&distance>=0,SE_ERROR_INVALID_GEOMETRY,"冲突对象距离计算失败");conflict=distance<limit;}
                if(!conflict)continue;++count;QString description=QString("待检对象[%1]与%3[%4]冲突：%5（关联组%2）").arg(describeObject(*target,i,targetSourceInfo)).arg(reference.rule.number).arg(reference.rule.layer).arg(describeObject(*reference.data,j,reference.sourceInfo)).arg(reference.rule.mode);
                if(reference.rule.mode=="最小间距")description+=QString("；图上间距%1毫米，小于阈值%2毫米").arg(distance/mm,0,'f',3).arg(reference.rule.distanceMm,0,'g',12);
                descriptions[i].append(description);errors["error"+QString::number(++errorNo)]=description;message(description);
            }if(count)counts[i]=count;
        }
        OGRwkbGeometryType type=target->records.empty()?wkbUnknown:target->records.front().geometry->getGeometryType();
        for(auto it=counts.constBegin();it!=counts.constEnd();++it)for(int row:target->objects[it.key()].records)need(target->records[row].geometry->getDimension()==(type==wkbUnknown?-1:target->records.front().geometry->getDimension()),SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"错误对象混合几何维度无法写入同一SHP");
        Output out(output,srs,type,!counts.isEmpty());if(out.layer){out.field("OBJECT_ID",OFTInteger);out.field("SRC_FID",OFTInteger64);out.field("SRC_LAYER",OFTString);out.field("SYMBOL_ID",OFTString);out.field("CONFLICTS",OFTInteger);out.field("CONFLICT",OFTString);
            for(auto it=counts.constBegin();it!=counts.constEnd();++it){const auto& object=target->objects[it.key()];for(int row:object.records){const auto& record=target->records[row];Feat f(OGRFeature::CreateFeature(out.layer->GetLayerDefn()));f->SetGeometry(record.geometry.get());f->SetField("OBJECT_ID",it.key()+1);f->SetField("SRC_FID",record.fid);f->SetField("SRC_LAYER",record.layer.toUtf8().constData());f->SetField("SYMBOL_ID",object.symbol.toUtf8().constData());f->SetField("CONFLICTS",it.value());const QByteArray conflictText=descriptions.value(it.key()).join("；").toUtf8();f->SetField("CONFLICT",conflictText.constData());out.write(f.get());}}}out.ds.reset();
        QString info=QString("检查%1个完整待检符号，%2个存在表示冲突，共%3条冲突关系；已检查%4组关联图层，跳过%5组").arg(targets.size()).arg(counts.size()).arg(errorNo).arg(references.size()).arg(skipped.size());
        if(!skipped.isEmpty())info+="；"+skipped.join("；");message(info);
        writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","123"},{"name","要素表示冲突检查"},{"status","质检成功"},{"err_exist",!counts.isEmpty()},{"info",info},{"errors",errors}}}});return 0;
    }catch(const std::exception& e){const auto* failure=dynamic_cast<const Failure*>(&e);int code=failure?failure->code:SE_ERROR_FAILURE;QString text=QString::fromUtf8(e.what());message(text);if(safe)try{writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","123"},{"name","要素表示冲突检查"},{"status","质检失败"},{"err_exist",true},{"info",text},{"errors",QJsonObject{{"execution_error",text}}}}}});}catch(...){}return code;}
}
