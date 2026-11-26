#define _HAS_STD_BYTE 0
#include "se_feature_density_check.h"
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
struct Rule {QString type,field,op,value,region;std::vector<AttributeCondition> conditions;double area=0;int maximum=0;bool filter=false;};
std::vector<Rule> rules(const QString& path,int denominator){
    QFile f(path);QDomDocument doc;need(f.open(QIODevice::ReadOnly),SE_ERROR_FILE_NOT_FOUND,"无法读取XML："+path);
    need(doc.setContent(&f)&&doc.documentElement().tagName()=="check_list",SE_ERROR_XML_PARSE_FAILED,"XML格式错误");
    std::vector<QDomElement> exact,generic;
    const auto scales=doc.documentElement().elementsByTagName("scale");
    for(int i=0;i<scales.size();++i){const auto s=scales.at(i).toElement();const auto text=s.firstChildElement("value").text().trimmed();bool ok=false;const int scale=text.toInt(&ok);
        const bool match=ok&&scale==denominator,common=text.isEmpty()||(ok&&scale==0);if(!match&&!common)continue;
        const auto items=s.elementsByTagName("check_item");for(int j=0;j<items.size();++j){const auto item=items.at(j).toElement();if(item.firstChildElement("id").text().trimmed()!="121" && item.firstChildElement("name").text().trimmed()!="要素密度检查")continue;
            (match?exact:generic).push_back(item);}}
    const auto items=exact.empty()?generic:exact;need(!items.empty(),SE_ERROR_XML_PARSE_FAILED,"未找到当前比例尺的要素密度检查配置");
    std::vector<Rule> result;for(const auto& item:items){
    QMap<QString,QString> values;double areaFactor=1;const auto nodes=item.elementsByTagName("rule");
    const QSet<QString> supported{"检查对象类型","属性筛选字段","属性筛选方式","属性筛选值","检查范围","格网面积","格网内最大要素数量"};
    const QRegularExpression numbered("^属性筛选(?:字段|方式|值)([0-9]+)$");QSet<int> conditionNumbers{1};
    for(int i=0;i<nodes.size();++i){auto node=nodes.at(i).toElement();const auto name=node.firstChildElement("name").text().trimmed();const auto numberedMatch=numbered.match(name);
        if(!supported.contains(name)&&!numberedMatch.hasMatch())continue;
        if(numberedMatch.hasMatch()){bool ok=false;const auto text=numberedMatch.captured(1);const int number=text.toInt(&ok);
            need(ok&&number>=2&&number<=1000&&text==QString::number(number),SE_ERROR_XML_PARSE_FAILED,"附加筛选编号应为2—1000："+name);conditionNumbers.insert(number);}
        need(!values.contains(name),SE_ERROR_XML_PARSE_FAILED,"重复规则："+name);values[name]=node.firstChildElement("value").text().trimmed();
        if(name=="格网面积"){
            auto unit=node.firstChildElement("unit").text().trimmed().toLower();unit.replace("²","2");unit.replace("^","");unit.remove(' ');
            const QMap<QString,double> factors{{"",1},{"平方毫米",0.01},{"mm2",0.01},{"平方厘米",1},{"cm2",1},
                {"平方分米",100},{"dm2",100},{"平方米",10000},{"m2",10000},{"平方千米",1e10},{"平方公里",1e10},{"km2",1e10},
                {"公顷",1e8},{"ha",1e8},{"亩",1e8/15.0}};
            need(factors.contains(unit),SE_ERROR_XML_PARSE_FAILED,"不支持的格网面积单位："+unit);areaFactor=factors.value(unit);
        }}
    Rule r;r.type=values.value("检查对象类型");need(r.type=="符号"||r.type=="注记",SE_ERROR_XML_PARSE_FAILED,"检查对象类型必须为符号或注记");
    bool ok=false;r.area=values.value("格网面积").toDouble(&ok)*areaFactor;need(ok&&std::isfinite(r.area)&&r.area>0,SE_ERROR_INVALID_PARAMETER,"格网面积及单位换算结果必须为有限正数");
    r.maximum=values.value("格网内最大要素数量").toInt(&ok);need(ok&&r.maximum>=0,SE_ERROR_INVALID_PARAMETER,"格网内最大要素数量必须为非负整数");
    r.region=values.value("检查范围");if(r.region.isEmpty())r.region="全部";
    need(r.region=="主区"||r.region=="外区"||r.region=="全部",SE_ERROR_XML_PARSE_FAILED,"检查范围必须为主区、外区或全部");
    r.field=values.value("属性筛选字段");r.op=values.value("属性筛选方式");r.value=values.value("属性筛选值");
    auto numbers=conditionNumbers.values();std::sort(numbers.begin(),numbers.end());
    for(int number:numbers){const auto suffix=number==1?QString():QString::number(number);AttributeCondition condition;
        condition.number=number;condition.field=values.value("属性筛选字段"+suffix);condition.op=values.value("属性筛选方式"+suffix);condition.value=values.value("属性筛选值"+suffix);
        if(condition.field.isEmpty()&&condition.op.isEmpty()&&condition.value.isEmpty())continue;
        need(!condition.field.isEmpty()&&!condition.value.isEmpty(),SE_ERROR_XML_PARSE_FAILED,QString("第%1组属性筛选字段和值必须填写").arg(number));
        if(condition.value=="为空"||condition.value=="不为空")need(condition.op.isEmpty()||condition.op=="等于",SE_ERROR_XML_PARSE_FAILED,"空值筛选方式只能留空或填等于");
        else need(condition.op=="等于"||condition.op=="包含",SE_ERROR_XML_PARSE_FAILED,"普通属性筛选方式必须为等于或包含");
        r.conditions.push_back(condition);
    }
    r.filter=!r.conditions.empty();
    result.push_back(r);}
    return result;
}
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
struct Raw {QString path,layer,text,symbol;GIntBig fid=0;Geom g;};
struct Source {QString path,layer;GIntBig fid=0;bool selected=false;Geom g;};
struct Object {
    QString key,name,symbol;OGREnvelope envelope;bool initialized=false;
    std::vector<int> raw;QSet<GIntBig> sourceIds;QJsonArray sources;
    double x()const{return envelope.MinX+(envelope.MaxX-envelope.MinX)/2;}
    double y()const{return envelope.MinY+(envelope.MaxY-envelope.MinY)/2;}
};
void add(Object& o,int index,const Raw& raw){OGREnvelope e;raw.g->getEnvelope(&e);if(!o.initialized){o.envelope=e;o.initialized=true;}else o.envelope.Merge(e);o.raw.push_back(index);}
struct Association {QString space,group,name,text;QSet<GIntBig> sourceIds;Geom g;};
QSet<GIntBig> ids(const QString& text){QSet<GIntBig> out;QString normalized=text;normalized.replace(';',',');for(const auto& s:normalized.split(',',Qt::SkipEmptyParts)){bool ok=false;const auto id=s.trimmed().toLongLong(&ok);if(ok&&id>=0)out.insert(id);}return out;}
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
}

int FeatureDensityCheck(const char* boundaryPath,const char* parsedPaths,const char* sourceDataPaths,const char* sourceLayerNames,
    const char* mainAreaPath,const char* annotationAssociationPath,const char* schemaXmlPath,int scale,const char* severity,
    const char* outputGridPath,const char* outputFeaturePath,const char* outputJsonPath,const char* outputLogPath,double sourceMatchToleranceMm){
    const QString xml=absolute(str(schemaXmlPath)),grid=absolute(str(outputGridPath)),points=absolute(str(outputFeaturePath)),json=absolute(str(outputJsonPath)),log=absolute(str(outputLogPath));
    QFile logFile;bool safeOutput=false;QJsonArray warnings;
    auto message=[&](const QString& s){if(logFile.isOpen()){QTextStream stream(&logFile);stream.setCodec("UTF-8");stream<<QDateTime::currentDateTime().toString(Qt::ISODate)<<" "<<s<<"\n";stream.flush();}};
    auto warn=[&](const QString& s){warnings.append(s);message("警告："+s);};
    try {
        const auto boundary=absolute(str(boundaryPath)),main=absolute(str(mainAreaPath)),association=absolute(str(annotationAssociationPath));
        const auto parsed=list(str(parsedPaths)),sourcePaths=list(str(sourceDataPaths));
        QStringList inputPaths=parsed;inputPaths<<sourcePaths<<boundary<<xml;if(!main.isEmpty())inputPaths<<main;if(!association.isEmpty())inputPaths<<association;
        bool safeJson=!json.isEmpty();
        for(const auto& input:inputPaths){if(input.isEmpty())continue;const QFileInfo a(json),b(input);
            if(keyPath(json)==keyPath(input)||keyPath(json).startsWith(keyPath(input)+"/")||
               (a.absolutePath().compare(b.absolutePath(),Qt::CaseInsensitive)==0&&a.completeBaseName().compare(b.completeBaseName(),Qt::CaseInsensitive)==0))safeJson=false;
        }
        for(const auto& output:QStringList{grid,points,log})if(!output.isEmpty()&&keyPath(json)==keyPath(output))safeJson=false;
        safeOutput=safeJson;
        QSet<QString> used;
        for(const auto& p:QStringList{grid,points,json,log}){
            need(!p.isEmpty()&&!used.contains(keyPath(p)),SE_ERROR_INVALID_PARAMETER,"输出路径为空或重复");used.insert(keyPath(p));
            for(const auto& input:inputPaths){
                const QFileInfo a(p),b(input);
                const bool sidecar=a.absolutePath().compare(b.absolutePath(),Qt::CaseInsensitive)==0&&a.completeBaseName().compare(b.completeBaseName(),Qt::CaseInsensitive)==0;
                need(keyPath(p)!=keyPath(input)&&!sidecar&&!keyPath(p).startsWith(keyPath(input)+"/"),SE_ERROR_INVALID_PARAMETER,"输出不能覆盖输入或写入输入数据库："+p);
            }
        }
        safeOutput=true;parent(log);logFile.setFileName(log);need(logFile.open(QIODevice::WriteOnly|QIODevice::Truncate),SE_ERROR_WRITE_FILE_FAILED,"无法写入日志");
        need(scale>0,SE_ERROR_INVALID_PARAMETER,"比例尺分母必须大于0");
        need(std::isfinite(sourceMatchToleranceMm)&&sourceMatchToleranceMm>=0,SE_ERROR_INVALID_PARAMETER,"源要素定位容差必须为非负有限数");
        QStringList missing;
        if(boundary.isEmpty())missing<<"内图廓图层";
        if(parsed.isEmpty())missing<<"待检解析图层";
        if(xml.isEmpty())missing<<"检查规则XML";
        need(missing.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少必要输入："+missing.join("、"));
        const auto ruleList=rules(xml,scale);GDALAllRegister();
        bool needsSource=false,needsMain=false,needsAssociation=false;
        for(const auto& rule:ruleList){needsSource|=rule.filter;needsMain|=rule.region!="全部";needsAssociation|=rule.type=="注记";}
        if(needsSource&&sourcePaths.isEmpty())missing<<"GDB/源数据图层（属性筛选需要）";
        if(needsMain&&main.isEmpty())missing<<"行政区划普色面图层（主区/外区检查需要）";
        if(needsAssociation&&association.isEmpty())missing<<"200检查项完整匹配中间图层（注记检查需要）";
        need(missing.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少必要图层："+missing.join("、"));
        auto validateLayer=[&](const QString& path,const QString& role){
            need(QFileInfo::exists(path),SE_ERROR_FILE_NOT_FOUND,role+"不存在："+path);
            try{auto dataset=open(path);}catch(const std::exception&){throw Failure(SE_ERROR_GDAL_OPEN_FAILED,role+"无法读取："+path);}
        };
        validateLayer(boundary,"内图廓图层");
        for(const auto& path:parsed)validateLayer(path,"待检解析图层");
        if(needsSource)for(const auto& path:sourcePaths)validateLayer(path,"GDB/源数据图层");
        if(needsMain)validateLayer(main,"行政区划普色面图层");
        if(needsAssociation)validateLayer(association,"200检查项完整匹配中间图层");

        std::unique_ptr<Output> gridsOutput,featuresOutput;QJsonObject errors;int totalOver=0,totalCounted=0,gridId=0,ruleNumber=0;QStringList summaries;
        for(const Rule& rule:ruleList){++ruleNumber;warnings=QJsonArray();message(QString("开始第%1条规则 [%2]").arg(ruleNumber).arg(rule.region));
        need(!rule.filter||!sourcePaths.isEmpty(),SE_ERROR_INVALID_PARAMETER,"属性筛选需要传入对应源数据图层");
        need(rule.region=="全部"||!main.isEmpty(),SE_ERROR_INVALID_PARAMETER,"检查主区或外区必须传入行政区划普色面");
        need(rule.type!="注记"||!association.isEmpty(),SE_ERROR_INVALID_PARAMETER,"注记检查必须传入200完整匹配中间图层");
        DS frameData=open(boundary);auto srs=layerSrs(frameData->GetLayer(0));
        need(srs.IsProjected(),SE_ERROR_GEOGRAPHIC_CRS,"内图廓必须使用投影坐标系");const double meters=srs.GetLinearUnits();
        need(std::isfinite(meters)&&meters>0,SE_ERROR_UNSUPPORTED_SRS,"无效坐标单位");
        int repairedFrame=0,repairedMain=0;
        auto frame=loadArea(boundary,srs,true,&repairedFrame);Geom mainGeometry;if(rule.region!="全部")mainGeometry=loadArea(main,srs,false,&repairedMain);
        if(repairedFrame||repairedMain)warn(QString("内存修复范围几何：内图廓%1个、行政区划普色面%2个；不修改源文件").arg(repairedFrame).arg(repairedMain));
        const double side=std::sqrt(rule.area)*scale/100.0/meters,mm=scale/1000.0/meters;
        OGREnvelope extent;frame->getEnvelope(&extent);
        const double nx=std::ceil((extent.MaxX-extent.MinX)/side),ny=std::ceil((extent.MaxY-extent.MinY)/side);
        need(std::isfinite(side)&&side>0&&nx>0&&ny>0&&nx<=INT_MAX&&ny<=INT_MAX,SE_ERROR_NOT_ENOUGH_MEMORY,"格网边长或行列数量无效");
        const int columns=static_cast<int>(nx),rows=static_cast<int>(ny);
        message(QString("121：%1；格网面积%2平方厘米；边长%3坐标单位；最多%4个；范围%5").arg(rule.type).arg(rule.area).arg(side).arg(rule.maximum).arg(rule.region));
        std::vector<Raw> raw;QSet<QString> seenParsed;int independentSymbols=0;
        for(const auto& path:parsed){need(QFileInfo(path).isFile(),SE_ERROR_INVALID_PARAMETER,"待检解析图层必须为具体文件："+path);if(seenParsed.contains(keyPath(path)))continue;seenParsed.insert(keyPath(path));
            DS d=open(path);for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);auto t=transformation(l,srs);const int symbol=l->GetLayerDefn()->GetFieldIndex("symbol_id"),text=l->GetLayerDefn()->GetFieldIndex("text_conte");
                need(rule.type!="注记"||text>=0,SE_ERROR_CORRUPT_DATA,"待检注记缺少text_conte："+path);
                while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,path);if(!g)continue;
                    Raw r;r.path=path;r.layer=QString::fromUtf8(l->GetName());r.fid=f->GetFID();r.g=std::move(g);
                    if(symbol>=0&&f->IsFieldSetAndNotNull(symbol))r.symbol=QString::fromUtf8(f->GetFieldAsString(symbol)).trimmed();
                    if(text>=0&&f->IsFieldSetAndNotNull(text))r.text=QString::fromUtf8(f->GetFieldAsString(text));
                    // 无编号表示该记录已是完整符号，包括解析成线/面的点符号。
                    if(rule.type=="符号"&&r.symbol.isEmpty())++independentSymbols;
                    raw.push_back(std::move(r));}
            }
        }
        if(independentSymbols)message(QString("%1条符号记录没有有效symbol_id，按独立完整对象计数").arg(independentSymbols));
        std::vector<Object> objects;QMap<QString,int> objectByKey;int unresolved=0;
        auto group=[&](const QString& key)->Object&{auto it=objectByKey.constFind(key);if(it!=objectByKey.constEnd())return objects[it.value()];Object o;o.key=key;const int index=static_cast<int>(objects.size());objects.push_back(std::move(o));objectByKey[key]=index;return objects.back();};
        if(rule.type=="符号"){
            for(int i=0;i<static_cast<int>(raw.size());++i){auto& r=raw[i];const QString key=r.symbol.isEmpty()?QString(r.path+"|"+r.layer+"|"+QString::number(r.fid)):QString("symbol:"+r.symbol);auto& o=group(key);o.symbol=r.symbol;add(o,i,r);}
        } else {
            std::vector<Association> links;QMap<QString,std::vector<int>> byText;DS data=open(association);
            for(int k=0;k<data->GetLayerCount();++k){auto* l=data->GetLayer(k);auto t=transformation(l,srs);auto* def=l->GetLayerDefn();
                const int text=def->GetFieldIndex("text_conte"),name=def->GetFieldIndex("assoc_name"),id=def->GetFieldIndex("group_id"),fids=def->GetFieldIndex("gdb_fids"),fid=def->GetFieldIndex("gdb_fid");
                need(text>=0&&name>=0&&id>=0,SE_ERROR_CORRUPT_DATA,"200中间图层需要text_conte、assoc_name、group_id字段");
                while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,association);if(!g)continue;Association a;a.space=QString::fromUtf8(l->GetName());
                    a.text=QString::fromUtf8(f->GetFieldAsString(text));a.name=QString::fromUtf8(f->GetFieldAsString(name));
                    if(f->IsFieldSetAndNotNull(name)&&!a.name.trimmed().isEmpty()&&f->IsFieldSetAndNotNull(id)&&f->GetFieldAsInteger64(id)>=0)
                        a.group=QString::number(a.name.size())+":"+a.name+"|"+QString::number(f->GetFieldAsInteger64(id));
                    if(fids>=0)a.sourceIds=ids(QString::fromUtf8(f->GetFieldAsString(fids)));
                    if(fid>=0&&f->IsFieldSetAndNotNull(fid)&&f->GetFieldAsInteger64(fid)>=0)a.sourceIds.insert(f->GetFieldAsInteger64(fid));
                    a.g=std::move(g);byText[a.text].push_back(static_cast<int>(links.size()));links.push_back(std::move(a));}
            }
            for(int i=0;i<static_cast<int>(raw.size());++i){const auto& r=raw[i];QSet<QString> hits;std::vector<int> matching;
                for(int index:byText.value(r.text)){const auto& a=links[index];if(a.group.isEmpty())continue;
                    if(r.g->Equals(a.g.get())){hits.insert(a.space+"|"+a.group);matching.push_back(index);}}
                if(hits.size()!=1){++unresolved;warn(QString("注记未能唯一关联有效assoc_name及group_id组合：%1 FID=%2 text=%3；跳过计数").arg(r.path).arg(r.fid).arg(r.text));continue;}
                auto& o=group("annotation:"+*hits.constBegin());
                bool duplicate=false;for(int old:o.raw)if(raw[old].text==r.text&&raw[old].g->Equals(r.g.get())){duplicate=true;break;}
                if(!duplicate)add(o,i,r);
                for(int index:matching){o.sourceIds.unite(links[index].sourceIds);if(o.name.isEmpty())o.name=links[index].name;}
            }
        }
        std::vector<Source> sources;QMap<GIntBig,std::vector<int>> byFid;QSet<QString> sourceLayers;
        if(rule.filter){QStringList wanted;for(const auto& n:str(sourceLayerNames).split(';',Qt::SkipEmptyParts))wanted<<n.trimmed();QSet<QString> matched;QSet<QString> seen;
            for(const auto& path:sourcePaths){if(seen.contains(keyPath(path)))continue;seen.insert(keyPath(path));DS d=open(path);
                need(d->GetLayerCount()==1||!wanted.isEmpty(),SE_ERROR_INVALID_PARAMETER,"多图层源数据库需要接口指定sourceLayerNames");
                for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);const auto name=QString::fromUtf8(l->GetName());if(!wanted.isEmpty()&&!wanted.contains(name))continue;matched.insert(name);sourceLayers.insert(path+"|"+name);
                    std::vector<int> fields;for(const auto& condition:rule.conditions){const int field=l->GetLayerDefn()->GetFieldIndex(condition.field.toUtf8().constData());need(field>=0,SE_ERROR_CORRUPT_DATA,"源图层不存在筛选字段："+condition.field+" 图层="+name);fields.push_back(field);}auto t=transformation(l,srs);
                    while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,path);if(!g)continue;Source s;s.path=path;s.layer=name;s.fid=f->GetFID();s.selected=true;
                        for(size_t ci=0;ci<rule.conditions.size();++ci)if(!passes(f.get(),fields[ci],rule.conditions[ci]))s.selected=false;
                        s.g=std::move(g);byFid[s.fid].push_back(static_cast<int>(sources.size()));sources.push_back(std::move(s));}
                }
            }
            for(const auto& name:wanted)need(matched.contains(name),SE_ERROR_LAYER_NOT_FOUND,"源数据未找到指定图层："+name);
        }
        std::vector<int> sourceFor(objects.size(),-1);std::vector<bool> keep(objects.size(),!rule.filter);int unlocated=0,ambiguous=0;
        if(rule.filter){
            const double tolerance=sourceMatchToleranceMm*mm,tie=std::max(1e-9,mm*1e-8);
            std::vector<double> bestDistance(objects.size(),std::numeric_limits<double>::infinity());
            std::map<int,std::vector<int>> claim;
            for(int i=0;i<static_cast<int>(objects.size());++i){auto& o=objects[i];
                // 200原始GDB FID仅在调用方传入单一对应源图层时使用。
                if(rule.type=="注记"&&sourceLayers.size()==1&&!o.sourceIds.isEmpty()){
                    bool any=false;for(auto id:o.sourceIds){const auto indexes=byFid.value(id);if(indexes.size()!=1)continue;const int s=indexes[0];any=true;
                        if(sources[s].selected){keep[i]=true;o.sources.append(QJsonObject{{"source_path",sources[s].path},{"source_layer",sources[s].layer},{"source_fid",QString::number(sources[s].fid)},{"method","200_gdb_fid"}});}}
                    if(any)continue;
                }
                OGRPoint p(o.x(),o.y());int best=-1;bool tied=false;
                for(int s=0;s<static_cast<int>(sources.size());++s){OGREnvelope e;sources[s].g->getEnvelope(&e);if(p.getX()<e.MinX-tolerance||p.getX()>e.MaxX+tolerance||p.getY()<e.MinY-tolerance||p.getY()>e.MaxY+tolerance)continue;
                    const double distance=p.Distance(sources[s].g.get());if(!std::isfinite(distance)||distance<0||distance>tolerance)continue;
                    if(distance+tie<bestDistance[i]){best=s;bestDistance[i]=distance;tied=false;}else if(std::abs(distance-bestDistance[i])<=tie)tied=true;
                }
                if(best<0){++unlocated;continue;}if(tied){++ambiguous;continue;}sourceFor[i]=best;claim[best].push_back(i);
            }
            // 符号定位执行一对一分配。先在全部源对象中定位，再判断属性，避免未上图的合格源对象抢占其他符号。
            for(const auto& entry:claim){int winner=-1;double distance=std::numeric_limits<double>::infinity();bool tied=false;
                for(int i:entry.second){if(bestDistance[i]+tie<distance){winner=i;distance=bestDistance[i];tied=false;}else if(std::abs(bestDistance[i]-distance)<=tie)tied=true;}
                if(tied){ambiguous+=static_cast<int>(entry.second.size());continue;}
                for(int i:entry.second)if(i!=winner)++unlocated;
                const auto& s=sources[entry.first];keep[winner]=s.selected;
                if(s.selected)objects[winner].sources.append(QJsonObject{{"source_path",s.path},{"source_layer",s.layer},{"source_fid",QString::number(s.fid)},{"method","bounded_spatial"},{"distance_mm",distance/mm}});
            }
            if(unlocated)message(QString("%1个解析完整对象没有唯一源定位，跳过").arg(unlocated));if(ambiguous)warn(QString("%1个对象源定位存在等距离歧义，跳过").arg(ambiguous));
        }
        using CellKey=std::pair<int,int>;std::map<CellKey,std::vector<int>> cells;int counted=0,outsideFrame=0,outsideRegion=0,filtered=0;
        for(int i=0;i<static_cast<int>(objects.size());++i){if(!keep[i]){++filtered;continue;}const auto& o=objects[i];OGRPoint p(o.x(),o.y());
            if(!frame->Intersects(&p)){++outsideFrame;continue;}if(mainGeometry){const bool inside=mainGeometry->Intersects(&p);if((rule.region=="主区"&&!inside)||(rule.region=="外区"&&inside)){++outsideRegion;continue;}}
            auto cellIndex=[](double value,int size){const double nearest=std::round(value);if(std::abs(value-nearest)<1e-10)value=nearest;return std::max(0,std::min(size-1,static_cast<int>(std::floor(value))));};
            const int column=cellIndex((o.x()-extent.MinX)/side,columns),row=cellIndex((extent.MaxY-o.y())/side,rows);
            cells[{row,column}].push_back(i);++counted;
        }
        OGRwkbGeometryType outputType=raw.empty()?wkbUnknown:raw.front().g->getGeometryType();
        bool hasErrors=false;for(const auto& cell:cells)if(static_cast<int>(cell.second.size())>rule.maximum){hasErrors=true;break;}
        if(!gridsOutput||(hasErrors&&!gridsOutput->layer)){gridsOutput.reset(new Output(grid,srs,wkbMultiPolygon,hasErrors));featuresOutput.reset(new Output(points,srs,outputType,hasErrors));}
        auto& gridOut=*gridsOutput;auto& pointOut=*featuresOutput;
        for(auto* output:{&gridOut,&pointOut}){output->field("CHECK_AREA",OFTString);output->field("RULE_NO",OFTInteger);output->field("GRID_ID",OFTInteger);output->field("ROW_NO",OFTInteger);output->field("COL_NO",OFTInteger);}
        gridOut.field("ACT_COUNT",OFTInteger);gridOut.field("MAX_COUNT",OFTInteger);gridOut.field("EXCESS",OFTInteger);gridOut.field("AREA_CM2",OFTReal);gridOut.field("CLIP_CM2",OFTReal);
        pointOut.field("SRC_FID",OFTInteger64);pointOut.field("SRC_LAYER",OFTString);pointOut.field("OBJECT_ID",OFTInteger);pointOut.field("OBJ_TYPE",OFTString);pointOut.field("SYMBOL_ID",OFTString);pointOut.field("ANNO_NAME",OFTString);pointOut.field("PART_COUNT",OFTInteger);
        QJsonArray details;int over=0,outputCount=0;
        for(const auto& cell:cells){++gridId;const int count=static_cast<int>(cell.second.size());if(count<=rule.maximum)continue;++over;
            const int row=cell.first.first,column=cell.first.second;const double left=extent.MinX+column*side,top=extent.MaxY-row*side;
            auto box=rect(left,top-side,left+side,top);Geom clipped(box->Intersection(frame.get()));need(clipped!=nullptr,SE_ERROR_INVALID_GEOMETRY,"超限格网裁切失败");OGRMultiPolygon parts;polygons(clipped.get(),parts);
            Feat f(OGRFeature::CreateFeature(gridOut.layer->GetLayerDefn()));f->SetField("CHECK_AREA",rule.region.toUtf8().constData());f->SetField("RULE_NO",ruleNumber);f->SetField("GRID_ID",gridId);f->SetField("ROW_NO",row+1);f->SetField("COL_NO",column+1);f->SetField("ACT_COUNT",count);f->SetField("MAX_COUNT",rule.maximum);f->SetField("EXCESS",count-rule.maximum);f->SetField("AREA_CM2",rule.area);
            f->SetField("CLIP_CM2",parts.get_Area()*meters*meters*10000.0/(static_cast<double>(scale)*scale));f->SetGeometry(&parts);gridOut.write(f.get());QJsonArray members;
            for(int index:cell.second){const auto& o=objects[index];OGRPoint p(o.x(),o.y());Feat center(OGRFeature::CreateFeature(pointOut.layer->GetLayerDefn()));center->SetGeometry(&p);center->SetField("CHECK_AREA",rule.region.toUtf8().constData());center->SetField("RULE_NO",ruleNumber);center->SetField("GRID_ID",gridId);center->SetField("ROW_NO",row+1);center->SetField("COL_NO",column+1);center->SetField("OBJECT_ID",index+1);center->SetField("OBJ_TYPE",rule.type.toUtf8().constData());center->SetField("SYMBOL_ID",o.symbol.toUtf8().constData());center->SetField("ANNO_NAME",o.name.toUtf8().constData());center->SetField("PART_COUNT",static_cast<int>(o.raw.size()));for(int ri:o.raw){center->SetGeometry(raw[ri].g.get());center->SetField("SRC_FID",raw[ri].fid);center->SetField("SRC_LAYER",raw[ri].layer.toUtf8().constData());pointOut.write(center.get());++outputCount;}
                QJsonArray records;for(int ri:o.raw)records.append(QJsonObject{{"path",raw[ri].path},{"layer",raw[ri].layer},{"fid",QString::number(raw[ri].fid)},{"text",raw[ri].text}});
                members.append(QJsonObject{{"object_id",index+1},{"group_key",o.key},{"symbol_id",o.symbol},{"annotation_name",o.name},{"center_x",o.x()},{"center_y",o.y()},{"parsed_records",records},{"sources",o.sources}});
            }
            QJsonObject detail{{"grid_id",gridId},{"row",row+1},{"column",column+1},{"actual_count",count},{"max_count",rule.maximum},{"excess_count",count-rule.maximum},{"objects",members},{"description",QString("格网%1：%2个，允许%3个，超出%4个").arg(gridId).arg(count).arg(rule.maximum).arg(count-rule.maximum)}};
            details.append(detail);QStringList refs;for(int index:cell.second)for(int ri:objects[index].raw)refs<<QString("%1/FID=%2").arg(raw[ri].layer).arg(raw[ri].fid);errors["error"+QString::number(totalOver+over)]=QString(QString("[%1] 规则%2：").arg(rule.region).arg(ruleNumber)+detail["description"].toString()+"；要素："+refs.join("、"));
        }

        int selectedSource=0;for(const auto& s:sources)if(s.selected)++selectedSource;
        QJsonArray filters;for(const auto& condition:rule.conditions)filters.append(QJsonObject{{"number",condition.number},{"field",condition.field},{"operator",condition.op},{"value",condition.value}});
        QJsonObject info{{"description",QString("检查%1个完整对象，%2个格网超限").arg(counted).arg(over)},
            {"object_type",rule.type},{"check_region",rule.region},{"grid_area_cm2",rule.area},{"grid_side_map_units",side},{"max_count",rule.maximum},{"scale",scale},
            {"grid_origin_x",extent.MinX},{"grid_origin_y",extent.MaxY},{"grid_rows",rows},{"grid_columns",columns},{"populated_grids",static_cast<int>(cells.size())},
            {"raw_record_count",static_cast<int>(raw.size())},{"complete_object_count",static_cast<int>(objects.size())},{"counted_objects",counted},
            {"independent_symbols_without_id",independentSymbols},
            {"repaired_frame_geometries",repairedFrame},{"repaired_main_area_geometries",repairedMain},
            {"source_selected_count",selectedSource},{"source_total_count",static_cast<int>(sources.size())},{"skipped_by_filter_or_source",filtered},{"skipped_outside_frame",outsideFrame},{"skipped_outside_region",outsideRegion},
            {"unresolved_annotation_records",unresolved},{"source_unlocated_objects",unlocated},{"source_ambiguous_objects",ambiguous},{"incomplete_association",unresolved>0||ambiguous>0},
            {"attribute_filter_enabled",rule.filter},{"filter_field",rule.field},{"filter_operator",rule.op},{"filter_value",rule.value},{"source_match_tolerance_mm",sourceMatchToleranceMm},
            {"attribute_filters",filters},{"attribute_filter_logic","AND"},
            {"over_limit_grids",over},{"output_center_count",outputCount},{"grid_layer",grid},{"feature_layer",points},{"warnings",warnings},
            {"center_method","完整对象外包矩形中心"},{"grid_edge_policy","内边界归右侧/下侧格；外边界归末格；图廓边缘格不按面积缩放数量上限"}};
        message(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact)));
        totalOver+=over;totalCounted+=counted;summaries<<QString("[%1] 规则%2：%3").arg(rule.region).arg(ruleNumber).arg(info["description"].toString());
        message(info["description"].toString());
        }
        gridsOutput.reset();featuresOutput.reset();
        writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","121"},{"name","要素密度检查"},{"status","质检成功"},{"err_exist",totalOver>0},{"info",summaries.join("；")},{"errors",errors}}}});
        return SE_ERROR_NONE;
    } catch(const std::exception& e){const auto* f=dynamic_cast<const Failure*>(&e);const int code=f?f->code:SE_ERROR_FAILURE;const auto text=QString::fromUtf8(e.what());message("失败："+text);
        if(safeOutput)try{writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","121"},{"name","要素密度检查"},{"status","质检失败"},{"err_exist",true},{"info",text},{"errors",QJsonObject{{"execution_error",text}}}}}});}catch(...){}
        return code;
    }
}
