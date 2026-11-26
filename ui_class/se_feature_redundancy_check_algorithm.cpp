#include "se_feature_redundancy_internal.h"
#include "redundancy_output_paths.h"
#include <QDateTime>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextCodec>
#include <cpl_conv.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace redundancy {
namespace {
void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
QString canonical(const QString& path) { QFileInfo f(path); return QDir::cleanPath(f.exists()?f.canonicalFilePath():f.absoluteFilePath()).toLower(); }
QString family(const QString& path) {
    QString base=QFileInfo(path).completeBaseName();
    base.remove(QRegularExpression(QStringLiteral("_(Point|LineString|Polygon)$"),QRegularExpression::CaseInsensitiveOption));
    return canonical(QDir(QFileInfo(path).absolutePath()).filePath(base));
}
QStringList resolve(const QString& folder,const QString& name) {
    QStringList out;
    for(const auto& suffix:{QString(),QStringLiteral("_Point"),QStringLiteral("_LineString"),QStringLiteral("_Polygon")}) {
        QString path=QDir(folder).filePath(name+suffix+QStringLiteral(".shp")); if(QFileInfo::exists(path))out<<path;
    }
    return out;
}
QStringList csvCells(const QString& row) {
    QStringList out; QString cell; bool quoted=false;
    for(int i=0;i<row.size();++i) { QChar c=row[i]; if(c=='"') {if(quoted&&i+1<row.size()&&row[i+1]=='"'){cell+=c;++i;}else quoted=!quoted;}
        else if(c==','&&!quoted){out<<cell.trimmed();cell.clear();}else cell+=c; }
    if(quoted)fail(QStringLiteral("图层配置含未闭合引号")); out<<cell.trimmed(); return out;
}
QMap<QString,QStringList> mapping(const QString& path) {
    QMap<QString,QStringList> result; if(path.isEmpty())return result;
    QFile file(path);if(!file.open(QIODevice::ReadOnly))fail(QStringLiteral("无法读取图层配置：%1").arg(path));
    QByteArray bytes=file.readAll(); QTextCodec::ConverterState state;
    QString value=QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(),bytes.size(),&state);
    if(state.invalidChars)value=QTextCodec::codecForName("GB18030")->toUnicode(bytes);
    value.remove(QChar(0xfeff));
    for(const auto& row:value.split(QRegularExpression(QStringLiteral("[\r\n]+")),QString::SkipEmptyParts)) {
        auto cells=csvCells(row); if(cells.size()<3||cells[0]==QStringLiteral("标准图层")||cells[0].startsWith('#'))continue;
        if(result.contains(cells[0])&&result[cells[0]]!=split(cells[2]))fail(QStringLiteral("图层配置存在冲突：%1").arg(cells[0]));
        result[cells[0]]=split(cells[2]);
    }
    return result;
}
Config config(const AlgorithmFilePaths& paths,int scale) {
    QStringList files=paths.xmlPaths; const QString expected=paths.standardLayer;
    std::stable_sort(files.begin(),files.end(),[&](const QString& a,const QString& b){return (QFileInfo(a).completeBaseName()==expected)>(QFileInfo(b).completeBaseName()==expected);});
    for(const auto& path:files) {
        // A layer-specific file must never borrow another subcheck's generic rule.
        if(!expected.isEmpty()&&QFileInfo(path).completeBaseName()!=expected&&files.size()>1)continue;
        QFile f(path);if(!f.open(QIODevice::ReadOnly))fail(QStringLiteral("无法读取XML：%1").arg(path));
        QDomDocument doc;QString message;int line=0,col=0;
        if(!doc.setContent(&f,&message,&line,&col))fail(QStringLiteral("XML解析失败：%1:%2 %3").arg(path).arg(line).arg(message));
        if(doc.documentElement().tagName()!=QStringLiteral("check_list"))fail(QStringLiteral("XML根节点须为check_list"));
        int best=-1;QDomElement selected;
        for(auto block=doc.documentElement().firstChildElement("scale");!block.isNull();block=block.nextSiblingElement("scale")) {
            QString raw=block.firstChildElement("value").text().trimmed();bool ok=false;int denominator=raw.toInt(&ok);
            int rank=raw.isEmpty()?0:(ok&&denominator==scale?1:-1);if(rank<0)continue;
            for(auto item=block.firstChildElement("check_item");!item.isNull();item=item.nextSiblingElement("check_item")) {
                QString name=item.firstChildElement("name").text().trimmed(),id=item.firstChildElement("id").text().trimmed();
                if(name!=QStringLiteral("要素冗余检查"))continue;
                if(id!=QStringLiteral("119"))fail(QStringLiteral("要素冗余检查名称与ID=119不一致"));
                if(rank==best)fail(QStringLiteral("同一比例尺存在重复的要素冗余检查规则"));
                if(rank>best){best=rank;selected=item;}
            }
        }
        if(selected.isNull())continue;
        Config cfg;cfg.xml=path;cfg.severity=selected.firstChildElement("severity").text();
        for(auto r=selected.firstChildElement("rule");!r.isNull();r=r.nextSiblingElement("rule")) {
            QString key=r.firstChildElement("name").text().trimmed();if(key.isEmpty()||cfg.values.contains(key))fail(QStringLiteral("XML存在空名或重复规则：%1").arg(key));
            cfg.values[key]=r.firstChildElement("value").text().trimmed();cfg.units[key]=r.firstChildElement("unit").text().trimmed();
        }
        if(cfg.text(QStringLiteral("检查类型")).isEmpty()||cfg.text(QStringLiteral("检查图层")).isEmpty())fail(QStringLiteral("XML缺少检查类型或检查图层"));
        for(auto it=cfg.units.begin();it!=cfg.units.end();++it) {
            if(it.value().isEmpty())continue;
            const bool physical=it.key()==QStringLiteral("连接表示最大间隔");
            const bool lengthKey=it.key().contains(QStringLiteral("长度"))||it.key().contains(QStringLiteral("间距"))||it.key().contains(QStringLiteral("容差"))||it.key().contains(QStringLiteral("半径"));
            if(physical&&it.value()!=QStringLiteral("米"))fail(QStringLiteral("连接表示最大间隔单位必须为米"));
            if(lengthKey&&!physical&&it.value()!=QStringLiteral("毫米"))fail(QStringLiteral("图上长度参数单位必须为毫米：%1").arg(it.key()));
        }
        if(cfg.text(QStringLiteral("检查类型"))==QStringLiteral("隧道")&&cfg.values.contains(QStringLiteral("长度计算依据"))&&cfg.text(QStringLiteral("长度计算依据"))!=QStringLiteral("解析成果SHP隧道实际几何长度按任务比例尺换算"))fail(QStringLiteral("不支持的XML隧道长度计算依据"));
        if(cfg.text(QStringLiteral("检查类型"))==QStringLiteral("河流")){
            const QString partition=expected.contains(QStringLiteral("干涸"))?QStringLiteral("全部干涸河"):QStringLiteral("按等级包含干涸河");
            if(cfg.values.contains(QStringLiteral("子类分流"))&&cfg.text(QStringLiteral("子类分流"))!=partition)
                fail(QStringLiteral("河流规则子类分流与当前算法不一致，请重新加载包含干涸河的新版六级/等外XML"));
            const QMap<QString,QString> supported={
                {QStringLiteral("长度计算依据"),QStringLiteral("解析成果SHP河流在检查范围内的实际几何长度")},
                {QStringLiteral("间距检查方式"),QStringLiteral("相邻独立河流的图上空间间距")},
                {QStringLiteral("间距冲突处理"),QStringLiteral("自动报告选取过密冲突")},
                {QStringLiteral("主区判定来源"),QStringLiteral("成果普色面")},
                {QStringLiteral("主区构建方式"),QStringLiteral("全部主区普色面合并")},
                {QStringLiteral("外区判定方式"),QStringLiteral("外区普色合并面减主区普色合并面")},
                {QStringLiteral("附加干涸河范围"),QStringLiteral("非六级等外的干涸河由等外任务执行外区禁表")},
                {QStringLiteral("注记允许的最末等级"),QStringLiteral("六级")}};
            for(auto it=supported.begin();it!=supported.end();++it)if(cfg.values.contains(it.key())&&cfg.text(it.key())!=it.value())fail(QStringLiteral("尚不支持该规则模式：%1=%2").arg(it.key(),cfg.text(it.key())));
            if(cfg.flag(QStringLiteral("不足必注长度是否禁止六级名称"),false))fail(QStringLiteral("当前规则不禁止较短六级河流名称；不支持开启该反向限制"));
        }
        return cfg;
    }
    fail(QStringLiteral("未找到当前图层及比例尺的要素冗余检查119规则"));return {};
}
std::vector<Feature> read(Context& ctx,const QStringList& files,const QString& filter,bool required) {
    if(files.isEmpty()){if(required)fail(QStringLiteral("缺少所需图层文件"));return {};}
    std::vector<Feature> result;QSet<QString> visited;
    for(const auto& path:files) {
        if(visited.contains(canonical(path)))continue;visited.insert(canonical(path));
        std::unique_ptr<GDALDataset,decltype(&GDALClose)> ds(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(),GDAL_OF_VECTOR|GDAL_OF_READONLY,nullptr,nullptr,nullptr)),GDALClose);
        if(!ds||ds->GetLayerCount()!=1)fail(QStringLiteral("无法打开单图层SHP：%1").arg(path));
        OGRLayer* layer=ds->GetLayer(0);auto* raw=layer->GetSpatialRef();if(!raw)fail(QStringLiteral("图层缺少坐标系：%1").arg(path));
        std::shared_ptr<OGRSpatialReference> local(raw->Clone(),[](OGRSpatialReference* p){p->Release();});local->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if(!ctx.srs) {
            if(!local->IsProjected()&&!local->IsLocal())fail(QStringLiteral("检查图层需要带长度单位的投影坐标系"));
            ctx.srs.reset(local->Clone(),[](OGRSpatialReference* p){p->Release();});ctx.metresPerUnit=ctx.srs->GetLinearUnits();
            if(!std::isfinite(ctx.metresPerUnit)||ctx.metresPerUnit<=0)fail(QStringLiteral("投影长度单位无效"));
        }
        std::unique_ptr<OGRCoordinateTransformation,decltype(&OCTDestroyCoordinateTransformation)> transform(nullptr,OCTDestroyCoordinateTransformation);
        if(!local->IsSame(ctx.srs.get())){transform.reset(OGRCreateCoordinateTransformation(local.get(),ctx.srs.get()));if(!transform)fail(QStringLiteral("无法转换坐标系：%1").arg(path));}
        if(!filter.isEmpty()&&layer->SetAttributeFilter(filter.toUtf8().constData())!=OGRERR_NONE)fail(QStringLiteral("GDB筛选条件无效：%1").arg(filter));
        layer->ResetReading();CPLErrorReset();
        while(auto* rawFeature=layer->GetNextFeature()) {
            std::unique_ptr<OGRFeature,decltype(&OGRFeature::DestroyFeature)> source(rawFeature,OGRFeature::DestroyFeature);
            Feature feature;feature.path=path;feature.key=canonical(path)+QStringLiteral("#%1").arg(source->GetFID());feature.fids.append(QString::number(source->GetFID()));
            for(int i=0;i<source->GetFieldCount();++i)if(source->IsFieldSetAndNotNull(i))feature.attrs[QString::fromUtf8(source->GetFieldDefnRef(i)->GetNameRef()).toUpper()]=QString::fromUtf8(source->GetFieldAsString(i)).trimmed();
            if(source->GetGeometryRef()) {
                feature.geom=copy(source->GetGeometryRef());feature.geom->flattenTo2D();
                if(transform&&feature.geom->transform(transform.get())!=OGRERR_NONE)fail(QStringLiteral("要素坐标转换失败：%1").arg(feature.key));
                feature.geom->assignSpatialReference(ctx.srs.get());OGREnvelope bounds;feature.geom->getEnvelope(&bounds);
                if(feature.geom->IsEmpty()||!std::isfinite(bounds.MinX)||!std::isfinite(bounds.MaxX)||!std::isfinite(bounds.MinY)||!std::isfinite(bounds.MaxY))feature.problem=QStringLiteral("要素几何为空或坐标非有限值");
            }else feature.problem=QStringLiteral("要素缺少几何");
            QJsonObject rawIdentity;rawIdentity["source_path"]=path;rawIdentity["source_key"]=feature.key;rawIdentity["source_fids"]=feature.fids;
            feature.originals.append(rawIdentity);ctx.originalGeometry[feature.key]=copy(feature.geom.get());
            result.push_back(std::move(feature));
        }
        if(CPLGetLastErrorType()>=CE_Failure)fail(QStringLiteral("要素读取失败：%1").arg(path));
    }
    return result;
}
bool valid(const OGRGeometry* g){return g&&!g->IsEmpty()&&g->IsValid();}
Geo representative(const Geo& g) {
    if(!g||g->IsEmpty())return {};
    auto* point=new OGRPoint();if(g->Centroid(point)!=OGRERR_NONE||point->IsEmpty()){OGREnvelope e;g->getEnvelope(&e);point->setX((e.MinX+e.MaxX)/2);point->setY((e.MinY+e.MaxY)/2);}return geometry(point);
}
void lineParts(const OGRGeometry* geom,std::vector<const OGRLineString*>& lines) {
    if(!geom||geom->IsEmpty())return;
    const auto t=wkbFlatten(geom->getGeometryType());
    if(t==wkbLineString)lines.push_back(static_cast<const OGRLineString*>(geom));
    else if(OGR_GT_IsSubClassOf(t,wkbGeometryCollection)){const auto* c=static_cast<const OGRGeometryCollection*>(geom);for(int i=0;i<c->getNumGeometries();++i)lineParts(c->getGeometryRef(i),lines);}
}
double measureOnLine(const OGRLineString& line,const OGRPoint& p) {
    double measure=0,bestMeasure=0,bestDistance=std::numeric_limits<double>::infinity();
    for(int i=1;i<line.getNumPoints();++i){double ax=line.getX(i-1),ay=line.getY(i-1),dx=line.getX(i)-ax,dy=line.getY(i)-ay;double size=std::hypot(dx,dy);if(size==0)continue;
        double fraction=std::max(0.0,std::min(1.0,((p.getX()-ax)*dx+(p.getY()-ay)*dy)/(size*size)));double distance=std::hypot(p.getX()-ax-fraction*dx,p.getY()-ay-fraction*dy);
        if(distance<bestDistance){bestDistance=distance;bestMeasure=measure+fraction*size;}measure+=size;}
    return bestMeasure;
}
bool completeCrossing(const Feature& road,const Feature& bridge,const Geo& water,const Geo& boundary,double radius,double epsilon) {
    std::vector<const OGRLineString*> roads;lineParts(road.geom.get(),roads);
    for(const auto* route:roads){auto cut=geometry(route->Intersection(water.get()));auto inside=cut?geometry(cut->Difference(boundary.get())):Geo();std::vector<const OGRLineString*> sections;lineParts(inside.get(),sections);
        for(const auto* section:sections){if(section->get_Length()<=epsilon||section->Distance(bridge.geom.get())>radius)continue;
            OGRPoint a,b;section->StartPoint(&a);section->EndPoint(&b);if(boundary->Distance(&a)>epsilon||boundary->Distance(&b)>epsilon)continue;
            double first=measureOnLine(*route,a),last=measureOnLine(*route,b);if(first>last)std::swap(first,last);const double total=route->get_Length();if(first<=epsilon||total-last<=epsilon)continue;
            double step=std::min({epsilon*10,(last-first)/4,first/2,(total-last)/2});OGRPoint before,after;route->Value(first-step,&before);route->Value(last+step,&after);
            if(!water->Intersects(&before)&&!water->Intersects(&after))return true;
        }
    }
    return false;
}
void writeBytes(const QString& path,const QByteArray& data) {
    QSaveFile f(path);if(!f.open(QIODevice::WriteOnly)||f.write(data)!=data.size()||!f.commit())fail(QStringLiteral("无法写入结果：%1").arg(path));
}
void writeLocations(Context& ctx,const QString& path) {
    // The reserved per-check prefix also covers legacy point outputs and archived copies.
    QString label=ctx.paths.standardLayer.isEmpty()?ctx.cfg.text(QStringLiteral("检查图层")):ctx.paths.standardLayer;
    const QString outputDir=QFileInfo(QFileInfo(path).absolutePath()).absolutePath();
    for(const auto& previous:RedundancyOutput::previousFiles(outputDir,label))if(!QFile::remove(previous))fail(QStringLiteral("无法清理旧错误文件：%1").arg(previous));
    QMap<QString,std::vector<std::pair<Geo,QJsonObject>>> groups;
    QMap<QString,OGRwkbGeometryType> types;
    for(const auto& location:ctx.locations){if(!location.first||location.first->IsEmpty())continue;auto type=wkbFlatten(location.first->getGeometryType());
        if(type!=wkbPoint&&type!=wkbMultiPoint&&type!=wkbLineString&&type!=wkbMultiLineString&&type!=wkbPolygon&&type!=wkbMultiPolygon)fail(QStringLiteral("错误原几何类型不受SHP支持"));
        QString token=QString::fromLatin1(OGRGeometryTypeToName(type));token.remove(' ');if(type==wkbLineString)token="LineString";if(type==wkbMultiLineString)token="MultiLineString";if(type==wkbMultiPoint)token="MultiPoint";if(type==wkbMultiPolygon)token="MultiPolygon";
        QString key=location.second["finding_type"].toString()+"_"+token;groups[key].push_back(location);types[key]=type;}
    QJsonArray paths,layers;int outputCount=0;
    auto* shapeDriver=GetGDALDriverManager()->GetDriverByName("ESRI Shapefile");if(!shapeDriver)fail(QStringLiteral("缺少SHP驱动"));
    for(auto group=groups.begin();group!=groups.end();++group){QString target=path;target.chop(4);target+="_"+group.key()+".shp";
        std::unique_ptr<GDALDataset,decltype(&GDALClose)> dataset(shapeDriver->Create(target.toUtf8().constData(),0,0,0,GDT_Unknown,nullptr),GDALClose);if(!dataset)fail(QStringLiteral("无法创建错误图层"));
        char* opts[]={const_cast<char*>("ENCODING=UTF-8"),nullptr};auto* output=dataset->CreateLayer("errors",ctx.srs.get(),types[group.key()],opts);if(!output)fail(QStringLiteral("无法创建原几何错误图层"));
        for(const auto* name:{"error_id","rule","message","status","src_layer","src_fids","xml_rule","compare","unit","decision","err_kind","geo_src","src_fid","group_id","src_path","ref_id","ref_layer"}){OGRFieldDefn field(name,OFTString);field.SetWidth(254);if(output->CreateField(&field)!=OGRERR_NONE)fail(QStringLiteral("无法创建错误字段"));}
        for(const auto* name:{"actual","limit","delta"}){OGRFieldDefn field(name,OFTReal);field.SetWidth(32);field.SetPrecision(15);if(output->CreateField(&field)!=OGRERR_NONE)fail(QStringLiteral("无法创建数值字段"));}
        for(const auto& location:group.value()){auto value=location.second;value["message"]=value.value("description");value["err_kind"]=value.value("finding_type");value["geo_src"]=value.value("location_source");value["src_path"]=value.value("source_path");value["ref_id"]=value.value("reference_id");value["ref_layer"]=value.value("reference_layer");
            std::unique_ptr<OGRFeature,decltype(&OGRFeature::DestroyFeature)> row(OGRFeature::CreateFeature(output->GetLayerDefn()),OGRFeature::DestroyFeature);
            for(int i=0;i<row->GetFieldCount();++i){QString name=QString::fromLatin1(row->GetFieldDefnRef(i)->GetNameRef());auto v=value[name];if(v.isDouble())row->SetField(i,v.toDouble());else if(v.isString()){QString text=v.toString();while(text.toUtf8().size()>254)text.chop(1);row->SetField(i,text.toUtf8().constData());}}
            row->SetGeometry(location.first.get());if(output->CreateFeature(row.get())!=OGRERR_NONE)fail(QStringLiteral("写入原几何错误失败"));++outputCount;}
        paths.append(target);QJsonObject item;item["path"]=target;item["geometry_type"]=QString::fromLatin1(OGRGeometryTypeToName(types[group.key()]));item["finding_type"]=group.value().front().second["finding_type"];item["feature_count"]=int(group.value().size());layers.append(item);
    }
    ctx.info["output_feature_count"]=outputCount;ctx.info["result_shps"]=paths;ctx.info["result_layers"]=layers;if(!paths.isEmpty())ctx.info["result_shp"]=paths.first();
    return;
}
}

Geo geometry(OGRGeometry* raw){return Geo(raw,OGRGeometryFactory::destroyGeometry);}
Geo copy(const OGRGeometry* raw){return raw?geometry(raw->clone()):Geo();}
QStringList split(const QString& value){QStringList result;for(auto s:value.split(QRegularExpression(QStringLiteral("[;；,，、|]+")),QString::SkipEmptyParts)){s=s.trimmed();if(!s.isEmpty()&&!result.contains(s))result<<s;}return result;}
QString Feature::text(const QString& key)const{return attrs.value(key.toUpper()).trimmed();}
QString Config::text(const QString& key,const QString& fallback)const{return values.value(key,fallback);}
double Config::number(const QString& key,double fallback)const{if(!values.contains(key))return fallback;bool ok=false;double n=values[key].toDouble(&ok);if(!ok||!std::isfinite(n)||n<0)fail(QStringLiteral("数值参数无效：%1").arg(key));return n;}
bool Config::flag(const QString& key,bool fallback)const{if(!values.contains(key))return fallback;QString v=values[key].toLower();if(v=="true"||v=="1")return true;if(v=="false"||v=="0")return false;fail(QStringLiteral("布尔参数无效：%1").arg(key));return false;}
QStringList Config::list(const QString& key,const QString& fallback)const{return split(text(key,fallback));}
double Context::metres(double n)const{return n*metresPerUnit;}
double Context::units(double n)const{return n/metresPerUnit;}
double Context::mapMm(double n)const{return metres(n)*1000.0/scale;}
double Context::fromMm(double n)const{double result=units(n*scale/1000.0);if(!std::isfinite(result))fail(QStringLiteral("图上长度参数溢出"));return result;}
double length(const OGRGeometry* g){
    if(!g||g->IsEmpty())return 0;
    const auto type=wkbFlatten(g->getGeometryType());
    if(OGR_GT_IsSubClassOf(type,wkbCurve))return OGR_G_Length(OGRGeometry::ToHandle(const_cast<OGRGeometry*>(g)));
    if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)){double sum=0;const auto* c=static_cast<const OGRGeometryCollection*>(g);for(int i=0;i<c->getNumGeometries();++i)sum+=length(c->getGeometryRef(i));return sum;}
    return 0;
}
Geo polygon(const OGRGeometry* raw,bool repair) {
    if(!raw||raw->IsEmpty())return {};
    auto type=wkbFlatten(raw->getGeometryType());Geo result;
    if(type==wkbPolygon||type==wkbMultiPolygon)result=copy(raw);
    else if(type==wkbLineString) {
        const auto* l=static_cast<const OGRLineString*>(raw);if(l->getNumPoints()<4||!l->get_IsClosed())return {};
        OGRLinearRing ring;for(int i=0;i<l->getNumPoints();++i)ring.addPoint(l->getX(i),l->getY(i));auto* p=new OGRPolygon();p->addRing(&ring);result=geometry(p);
    }else return {};
    CPLPushErrorHandler(CPLQuietErrorHandler);bool good=result->IsValid();CPLPopErrorHandler();
    if(!good&&repair)result=geometry(result->MakeValid());
    if(!valid(result.get()))return {};
    type=wkbFlatten(result->getGeometryType());if(type!=wkbPolygon&&type!=wkbMultiPolygon)return {};
    return result;
}
Geo unite(const std::vector<Geo>& parts){Geo result;for(const auto& p:parts){if(!p||p->IsEmpty())continue;if(!result)result=copy(p.get());else{result=geometry(result->Union(p.get()));if(!result)fail(QStringLiteral("几何合并失败"));}}return result;}
std::vector<Feature> Context::actual(const QString& name,bool required){auto files=resolve(paths.shpPath,name);if(files.isEmpty()){auto rows=mapping(paths.csvPath);for(const auto& n:rows.value(name))files<<resolve(paths.shpPath,n);}if(files.isEmpty()&&required)fail(QStringLiteral("缺少成果图层：%1").arg(name));return read(*this,files,QString(),required);}
std::vector<Feature> Context::gdb(const QString& name,const QString& filter,bool required){auto files=resolve(paths.gdbShpPath,name);if(files.isEmpty()&&required)fail(QStringLiteral("缺少GDB图层：%1").arg(name));return read(*this,files,filter,required);}
Geo Context::area(const QString& name,bool required,bool allowPartial) {
    auto parts=actual(name,required);if(parts.empty())return {};
    std::vector<Geo> faces,nativeFaces;int repaired=0;
    // Prefer explicit polygons to matching symbol outlines to preserve hole semantics.
    QSet<QString> polygonIds;for(const auto& part:parts)if(part.geom&&(wkbFlatten(part.geom->getGeometryType())==wkbPolygon||wkbFlatten(part.geom->getGeometryType())==wkbMultiPolygon)){if(!part.text("symbol_id").isEmpty())polygonIds.insert(part.text("symbol_id"));auto p=polygon(part.geom.get(),true);if(p)nativeFaces.push_back(p);}
    std::vector<Geo> nativeBoundaries;for(const auto& p:nativeFaces)nativeBoundaries.push_back(geometry(p->Boundary()));
    for(const auto& part:parts) {
        if(!part.problem.isEmpty()){if(allowPartial){uncertainAreas.push_back(part);review(QStringLiteral("水体面几何异常"),part.problem,&part);continue;}fail(QStringLiteral("范围/水体几何异常：%1 %2").arg(name,part.key));}
        auto type=wkbFlatten(part.geom->getGeometryType());const bool face=type==wkbPolygon||type==wkbMultiPolygon;
        if(!face&&polygonIds.contains(part.text("symbol_id")))continue;
        if(!face){bool duplicate=false;for(const auto& boundary:nativeBoundaries)if(boundary){auto remaining=geometry(part.geom->Difference(boundary.get()));if(remaining&&remaining->IsEmpty()){duplicate=true;break;}}if(duplicate){info["removed_boundary_stroke_count"]=info["removed_boundary_stroke_count"].toInt()+1;continue;}}
        if(!face&&name!=QStringLiteral("内图廓")&&part.text("filled").toLower()!=QStringLiteral("true")&&part.text("filled")!="1")continue;
        auto p=polygon(part.geom.get(),true);if(!p){if(allowPartial){uncertainAreas.push_back(part);review(QStringLiteral("水体面几何异常"),QStringLiteral("无法可靠恢复该水面；保留其他有效水面继续检查，附近负向结论列为复核"),&part);continue;}fail(QStringLiteral("无法可靠恢复范围/水体面：%1 %2").arg(name,part.key));}
        CPLPushErrorHandler(CPLQuietErrorHandler);bool wasValid=part.geom->IsValid();CPLPopErrorHandler();if(!wasValid){++repaired;notice(QStringLiteral("几何修复"),QStringLiteral("在内存副本中修复面，源SHP未修改"),&part);}faces.push_back(p);
    }
    if(faces.empty()){if(allowPartial)return {};fail(QStringLiteral("图层没有可用面：%1").arg(name));}
    auto result=unite(faces);if(!valid(result.get()))fail(QStringLiteral("范围/水体合并失败：%1").arg(name));
    info["repaired_geometry_count"]=info["repaired_geometry_count"].toInt()+repaired;return result;
}
std::vector<Feature> symbols(std::vector<Feature> parts,const QString& field) {
    QMap<QString,std::vector<Feature>> grouped;
    for(auto& part:parts){QString id=part.text(field);QString key=id.isEmpty()?part.key:family(part.path)+"#SYMBOL:"+id;grouped[key].push_back(std::move(part));}
    std::vector<Feature> out;
    for(auto it=grouped.begin();it!=grouped.end();++it){Feature f=it.value().front();f.key=it.key();f.fids=QJsonArray();f.originals=QJsonArray();std::vector<Geo> geoms;
        for(const auto& p:it.value()){for(const auto& original:p.originals)f.originals.append(original);for(const auto& fid:p.fids)f.fids.append(fid);if(!p.problem.isEmpty())f.problem=p.problem;if(p.geom)geoms.push_back(p.geom);if(p.text("symbol_nam")!=f.text("symbol_nam"))f.problem=QStringLiteral("同一符号的名称属性冲突");}
        f.geom=unite(geoms);out.push_back(std::move(f));}
    return out;
}
bool resultAttributePresent(const QString& value){const QString v=value.trimmed();if(v.isEmpty()||v=="-"||v.compare("NULL",Qt::CaseInsensitive)==0)return false;bool ok=false;const double number=v.toDouble(&ok);return !(ok&&number==-999999);}
bool reliableResultId(const QString& value){if(!resultAttributePresent(value))return false;bool ok=false;const double number=value.trimmed().toDouble(&ok);return !(ok&&number==0);}
std::vector<Feature> resultSymbols(Context& ctx,std::vector<Feature> parts,const QString& field,const QStringList& attributes,const QString& diagnosticRule){
    QMap<QString,std::vector<Feature>> members;for(const auto& part:parts){const QString id=part.text(field);members[id.isEmpty()?part.key:family(part.path)+"#SYMBOL:"+id].push_back(part);}
    auto merged=symbols(std::move(parts),field);
    for(auto& feature:merged){const auto& rows=members[feature.key];QJsonObject values;
        for(const auto& attribute:attributes){const QString key=attribute.toUpper();QSet<QString> candidates;
            for(const auto& row:rows){const QString value=row.text(key);if(resultAttributePresent(value))candidates.insert(value);}
            if(candidates.size()==1)feature.attrs[key]=*candidates.begin();
            else if(candidates.size()>1){feature.attrs.remove(key);feature.attributeConflicts.insert(key);QStringList ordered=candidates.values();ordered.sort();values[key]=QJsonArray::fromStringList(ordered);}
            else feature.attrs.remove(key);
        }
        if(!feature.attributeConflicts.isEmpty()){QStringList conflicts=feature.attributeConflicts.values();conflicts.sort();QJsonObject detail;detail["attribute_conflicts"]=QJsonArray::fromStringList(conflicts);detail["attribute_values"]=values;QJsonArray originals;for(const auto& row:rows)originals.append(identity(row));detail["members"]=originals;
            ctx.review(diagnosticRule,QStringLiteral("同一成果标识的%1属性冲突，依赖该属性的规则无法判定").arg(conflicts.join("/")),&feature,detail);}
    }return merged;
}
QJsonObject identity(const Feature& f){QJsonObject r;r["source_key"]=f.key;r["source_path"]=f.path;r["source_fids"]=f.fids;r["original_members"]=f.originals;QJsonObject attrs;for(auto i=f.attrs.begin();i!=f.attrs.end();++i)attrs[i.key()]=i.value();r["attributes"]=attrs;if(!f.attributeConflicts.isEmpty()){QStringList conflicts=f.attributeConflicts.values();conflicts.sort();r["attribute_conflicts"]=QJsonArray::fromStringList(conflicts);}return r;}
bool comparison(double actual,const QString& op,double limit){
    if(!std::isfinite(actual)||!std::isfinite(limit))fail(QStringLiteral("数值比较含非有限值"));
    if(op==">")return actual>limit;if(op==">=")return actual>=limit;if(op=="<")return actual<limit;if(op=="<=")return actual<=limit;
    fail(QStringLiteral("不支持的XML比较符：%1").arg(op));return false;
}
QJsonObject numericDetail(double actual,const QString& op,double limit,const QString& unit,const QString& rule,const QString& decision){
    comparison(actual,op,limit);QJsonObject d;d["actual"]=actual;d["compare"]=op;d["limit"]=limit;d["delta"]=actual-limit;d["unit"]=unit;d["xml_rule"]=rule;d["decision"]=decision;return d;
}
QJsonObject forbiddenDetail(const QString& rule){QJsonObject d;d["xml_rule"]=rule;d["decision"]=QStringLiteral("不允许表示");return d;}
QString numericMessage(const QString& subject,const QJsonObject& d){
    double actual=d["actual"].toDouble(),limit=d["limit"].toDouble(),delta=actual-limit;
    auto number=[&](double value){int precision=3;while(precision<12&&value!=0&&QString::number(value,'f',precision).toDouble()==0)++precision;
        while(precision<12&&actual!=limit&&QString::number(actual,'f',precision)==QString::number(limit,'f',precision))++precision;
        QString s=QString::number(value,'f',precision);while(s.contains('.')&&s.endsWith('0'))s.chop(1);if(s.endsWith('.'))s.chop(1);return s;};
    QString change=delta==0?QStringLiteral("等于阈值"):(delta<0?QStringLiteral("不足"):QStringLiteral("超出"))+number(std::abs(delta))+" "+d["unit"].toString();
    if(delta==0&&d["compare"].toString()==">")change+=QStringLiteral("，XML要求严格大于");
    if(delta==0&&d["compare"].toString()=="<")change+=QStringLiteral("，XML要求严格小于");
    return QStringLiteral("%1%2 %3；XML要求%4%5 %3；%6，%7。").arg(subject,number(actual),d["unit"].toString(),d["compare"].toString(),number(limit),change,d["decision"].toString());
}
void Context::error(const QString& rule,const QString& description,const Feature& feature,QJsonObject detail,Geo location) {
    QString key=detail.value("dedup_key").toString();if(key.isEmpty())key=rule+":"+feature.key;if(emitted.contains(key))return;emitted.insert(key);
    QJsonObject value=identity(feature);for(auto it=detail.begin();it!=detail.end();++it)value[it.key()]=it.value();value["rule"]=rule;value["error_type"]=rule;value["description"]=description;
    value["src_layer"]=QFileInfo(feature.path).completeBaseName();QStringList ids;for(const auto& fid:feature.fids)ids<<fid.toString();QString displayed=ids.join(",");
    if(displayed.toUtf8().size()>254){const QString suffix=QStringLiteral("；…共%1个，完整编号见JSON").arg(ids.size());displayed.clear();for(const auto& id:ids){const QString candidate=displayed.isEmpty()?id:displayed+","+id;if((candidate+suffix).toUtf8().size()>254)break;displayed=candidate;}displayed+=suffix;}
    value["src_fids"]=displayed;
    if(!value.contains("xml_rule"))value["xml_rule"]=rule;if(!value.contains("decision"))value["decision"]=QStringLiteral("不允许表示");
    for(const auto* field:{"actual","limit","delta"})if(!value.contains(field))value[field]=QJsonValue(QJsonValue::Null);
    for(const auto* field:{"compare","unit"})if(!value.contains(field))value[field]=QString();
    const QString id=QStringLiteral("error%1").arg(errors.size()+1);value["error_id"]=id;value["status"]=QStringLiteral("错误");
    Geo geom=location?location:feature.geom;if(geom){char* wkt=nullptr;if(geom->exportToWkt(&wkt)==OGRERR_NONE){value["geometry_wkt"]=QString::fromUtf8(wkt);if(location)value["violation_geometry_wkt"]=QString::fromUtf8(wkt);CPLFree(wkt);}}
    if(!value.contains("finding_type"))value["finding_type"]="redundancy";
    if(!value.contains("location_source"))value["location_source"]="actual";
    value["group_id"]=id;
    errors[id]=value;
    if(value["location_source"].toString()!="actual"){if(geom)locations.push_back({geom,value});return;}
    QJsonArray members=detail["conflict_members"].toArray();if(members.isEmpty())members=detail["members"].toArray();
    if(members.isEmpty())members.append(identity(feature));
    QSet<QString> written;
    for(const auto& member:members){auto m=member.toObject();auto originals=m["original_members"].toArray();if(originals.isEmpty())originals.append(m);
        for(const auto& original:originals){auto raw=original.toObject();QString path=raw["source_path"].toString();for(const auto& fid:raw["source_fids"].toArray()){
            QString rawKey=canonical(path)+"#"+fid.toString();if(written.contains(rawKey))continue;written.insert(rawKey);
            auto rawGeom=originalGeometry.value(rawKey);if(!rawGeom)continue;auto row=value;row["source_path"]=path;row["src_layer"]=QFileInfo(path).completeBaseName();row["src_fid"]=fid.toString();row["source_fids"]=QJsonArray{fid};locations.push_back({rawGeom,row});}}
    }
}
void Context::absent(const QString& kind,const QString& rule,const QString& description,const Feature& reference,QJsonObject detail,Geo expected){
    detail["finding_type"]=kind;detail["location_source"]=expected?"expected":"reference";detail["reference"]=identity(reference);
    detail["reference_id"]=reference.key;detail["reference_layer"]=QFileInfo(reference.path).completeBaseName();
    Feature missing=reference;missing.path.clear();missing.fids=QJsonArray();missing.originals=QJsonArray();detail["decision"]=QStringLiteral("必须表示但缺失");
    error(rule,description,missing,detail,expected?expected:reference.geom);
}
void Context::review(const QString& rule,const QString& description,const Feature* feature,QJsonObject detail){
    const QString dedupKey=detail.value("dedup_key").toString();
    if(!dedupKey.isEmpty()&&reviewed.contains(dedupKey))return;
    if(!dedupKey.isEmpty())reviewed.insert(dedupKey);
    auto v=feature?identity(*feature):QJsonObject();
    for(auto it=detail.begin();it!=detail.end();++it)v[it.key()]=it.value();
    const QString id=QStringLiteral("review%1").arg(reviews.size()+1);
    v["review_id"]=id;v["status"]=QStringLiteral("待复核");v["rule"]=rule;v["description"]=description;
    reviews.append(v);
}
void Context::notice(const QString& rule,const QString& description,const Feature* feature){auto v=feature?identity(*feature):QJsonObject();v["rule"]=rule;v["description"]=description;notices.append(v);}
int match(const Feature& rendered,const std::vector<Feature>& sources,double radius,bool coverage) {
    if(!rendered.geom||!rendered.problem.isEmpty())return -1;int winner=-1;double best=std::numeric_limits<double>::infinity();bool ambiguous=false;
    QString identityField;
    for(const auto& field:{QString("ELEMID"),QString("WRID"),QString("ID")})if(reliableResultId(rendered.text(field))){bool common=false;for(const auto& source:sources)if(reliableResultId(source.text(field))||(field=="ELEMID"&&!source.memberElemIds.isEmpty()))common=true;if(common){identityField=field;break;}}
    bool exactIdentity=false;if(!identityField.isEmpty())for(const auto& source:sources)if(source.text(identityField)==rendered.text(identityField)||(identityField=="ELEMID"&&source.memberElemIds.contains(rendered.text(identityField))))exactIdentity=true;
    OGREnvelope e;rendered.geom->getEnvelope(&e);
    for(size_t i=0;i<sources.size();++i){const auto& f=sources[i];if(!f.geom||!f.problem.isEmpty())continue;
        if(!identityField.isEmpty()){const bool equal=f.text(identityField)==rendered.text(identityField)||(identityField=="ELEMID"&&f.memberElemIds.contains(rendered.text(identityField)));if((exactIdentity&&!equal)||(!equal&&reliableResultId(f.text(identityField))))continue;}
        OGREnvelope b;f.geom->getEnvelope(&b);
        if(e.MinX>b.MaxX+radius||e.MaxX<b.MinX-radius||e.MinY>b.MaxY+radius||e.MaxY<b.MinY-radius)continue;
        double distance=rendered.geom->Distance(f.geom.get());if(distance<0||!std::isfinite(distance)||distance>radius)continue;
        const QString aId=rendered.text("ELEMID"),bId=f.text("ELEMID");
        if(reliableResultId(aId)&&reliableResultId(bId)&&aId!=bId&&!f.memberElemIds.contains(aId))continue;
        if(coverage){const double bufferedRadius=std::max(radius,1e-8);if(!f.matchBuffer||f.matchRadius!=bufferedRadius){f.matchBuffer=geometry(f.geom->Buffer(bufferedRadius));f.matchRadius=bufferedRadius;}if(!f.matchBuffer)continue;auto outside=geometry(rendered.geom->Difference(f.matchBuffer.get()));if(!outside||length(outside.get())>std::max(1e-7,length(rendered.geom.get())*0.05))continue;}
        if(distance+1e-7<best){best=distance;winner=static_cast<int>(i);ambiguous=false;}else if(std::abs(distance-best)<=1e-7)ambiguous=true;
    }
    return ambiguous?-2:winner;
}

void checkBridges(Context& ctx) {
    ctx.targets=symbols(std::move(ctx.targets),ctx.cfg.text(QStringLiteral("桥梁成果分组字段"),QStringLiteral("symbol_id")));
    ctx.info["total_count"]=static_cast<int>(ctx.targets.size());
    Geo water;
    try {water=ctx.area(ctx.cfg.text(QStringLiteral("水体面成果图层"),QStringLiteral("水系面")),false,true);
        if(!water){std::vector<Geo> fallback;for(const auto& name:ctx.cfg.list(QStringLiteral("水体面参考GDB图层"),QStringLiteral("HYDA;HYDA_HPSK")))for(const auto& f:ctx.gdb(name,QString(),false)){auto p=polygon(f.geom.get(),true);if(!p)fail(QStringLiteral("GDB水体面无法恢复"));fallback.push_back(p);}water=unite(fallback);}
    }catch(const std::exception& e){ctx.review(QStringLiteral("桥梁输入异常"),QString::fromUtf8(e.what()));return;}
    auto roads=ctx.gdb(ctx.cfg.text(QStringLiteral("公路载体GDB图层"),"LRDL"),QString(),false);
    auto rails=ctx.gdb(ctx.cfg.text(QStringLiteral("铁路载体GDB图层"),"LRRL"),QString(),false);
    bool usesRoads=false,usesRails=false;
    for(const auto& target:ctx.targets){QString symbol=target.text(ctx.cfg.text(QStringLiteral("桥梁类型字段"),"symbol_nam"));usesRoads|=symbol==ctx.cfg.text(QStringLiteral("公路桥符号值"),QStringLiteral("公路桥"));usesRails|=symbol==ctx.cfg.text(QStringLiteral("铁路桥符号值"),QStringLiteral("铁路桥"));}
    auto checkCarriers=[&](const std::vector<Feature>& features){for(const auto& source:features)if(!source.problem.isEmpty()||!source.geom||source.geom->IsEmpty())ctx.review(QStringLiteral("桥梁载体输入异常"),QStringLiteral("所需载体包含损坏或空几何记录，源数据完整性未确认"),&source);};
    if(usesRoads)checkCarriers(roads);if(usesRails)checkCarriers(rails);
    const double radius=ctx.fromMm(ctx.cfg.number(QStringLiteral("候选匹配搜索半径"),2));
    const bool exclusive=ctx.cfg.flag(QStringLiteral("是否仅允许跨面水系桥梁"),false);
    for(const auto& target:ctx.targets) {
        if(!target.problem.isEmpty()||!target.geom){ctx.review(QStringLiteral("桥梁几何异常"),target.problem,&target);continue;}
        const QString symbol=target.text(ctx.cfg.text(QStringLiteral("桥梁类型字段"),"symbol_nam"));
        const auto* carriers=&roads;
        if(symbol==ctx.cfg.text(QStringLiteral("铁路桥符号值"),QStringLiteral("铁路桥")))carriers=&rails;
        else if(symbol!=ctx.cfg.text(QStringLiteral("公路桥符号值"),QStringLiteral("公路桥"))){ctx.review(QStringLiteral("桥梁类型未知"),QStringLiteral("桥梁符号没有明确公路/铁路类别"),&target);continue;}
        if(!water||carriers->empty()){ctx.review(QStringLiteral("桥梁输入缺失"),QStringLiteral("缺少完整道路载体或水体面，无法判断跨水"),&target);continue;}
        Feature carrierQuery=target;for(const auto& field:{"ELEMID","WRID","ID"})carrierQuery.attrs.remove(field);
        int index=match(carrierQuery,*carriers,radius,false);if(index<0){ctx.review(QStringLiteral("桥梁载体匹配"),QStringLiteral("无法唯一确认桥梁所属道路"),&target);continue;}
        const auto& road=(*carriers)[index];auto interior=geometry(road.geom->Intersection(water.get()));auto boundary=geometry(water->Boundary());
        if(!interior||!boundary){ctx.review(QStringLiteral("桥梁几何运算"),QStringLiteral("道路与水面求交失败"),&target);continue;}
        auto inside=geometry(interior->Difference(boundary.get()));auto outside=geometry(road.geom->Difference(water.get()));
        auto local=geometry(target.geom->Buffer(radius));auto nearbyInside=inside&&local?geometry(inside->Intersection(local.get())):Geo();
        const bool crossed=inside&&outside&&nearbyInside&&length(nearbyInside.get())>1e-8&&completeCrossing(road,target,water,boundary,radius,std::max(1e-8,ctx.fromMm(0.000001)));
        if(crossed){ctx.info["crossing_bridge_count"]=ctx.info["crossing_bridge_count"].toInt()+1;continue;}
        // A road clipped while inside a waterbody is incomplete evidence, not a forbidden bridge.
        if(nearbyInside&&length(nearbyInside.get())>1e-8){ctx.review(QStringLiteral("桥梁穿越证据不足"),QStringLiteral("道路进入水面但缺少完整进出证据，可能被裁切"),&target);continue;}
        bool uncertain=false;OGREnvelope box;target.geom->getEnvelope(&box);
        for(const auto& damaged:ctx.uncertainAreas){if(!damaged.geom||!damaged.problem.isEmpty()){uncertain=true;break;}OGREnvelope b;damaged.geom->getEnvelope(&b);if(box.MinX<=b.MaxX+radius&&box.MaxX>=b.MinX-radius&&box.MinY<=b.MaxY+radius&&box.MaxY>=b.MinY-radius){uncertain=true;break;}}
        if(uncertain){ctx.review(QStringLiteral("桥梁附近水体不完整"),QStringLiteral("桥位附近存在无法可靠恢复的水面，不能作非跨水结论"),&target);continue;}
        if(exclusive)ctx.error(QStringLiteral("非跨水桥梁冗余"),QStringLiteral("该桥位未跨越水面内部；XML仅允许跨面水系桥梁，不允许表示。"),target,forbiddenDetail(QStringLiteral("是否仅允许跨面水系桥梁")));
        else ctx.review(QStringLiteral("桥梁跨水条件未满足"),QStringLiteral("未确认跨水，当前规则允许跨线桥/旱桥等其他用途，需核对"),&target);
    }
}
}

void FeatureRedundancyCheckWrapper(const QString& name,const AlgorithmFilePaths& paths,int scale,
                                   const QString& outputDir,CheckItemResult& result)
{
    using namespace redundancy;
    result=CheckItemResult{};result.id=QStringLiteral("119");result.name=name;result.status=QStringLiteral("质检失败");result.err_exist=true;
    Context ctx;ctx.paths=paths;ctx.scale=scale;QString jsonPath,logPath;
    try {
        static std::once_flag once;std::call_once(once,[]{GDALAllRegister();});
        if(scale<=0)fail(QStringLiteral("任务比例尺必须为正整数"));if(outputDir.trimmed().isEmpty())fail(QStringLiteral("输出目录为空"));
        ctx.cfg=config(paths,scale);
        QString label=paths.standardLayer.isEmpty()?ctx.cfg.text(QStringLiteral("检查图层")):paths.standardLayer;label.replace(QRegularExpression(QStringLiteral("[\\/:*?\"<>|]")),"_");
        auto output=[&](const QString& folder,const QString& suffix){QString dir=QDir(outputDir).filePath(folder);if(!QDir().mkpath(dir))fail(QStringLiteral("无法创建输出目录"));return QDir(dir).filePath(QStringLiteral("要素冗余_%1%2").arg(label,suffix));};
        jsonPath=output(QStringLiteral("质检json文件"),".json");logPath=output(QStringLiteral("质检日志文件"),".log");QString shpPath=output(QStringLiteral("质检错误结果"),"_errors.shp");
        auto rows=mapping(paths.csvPath);if(rows.contains(paths.standardLayer)&&!rows[paths.standardLayer].contains(ctx.cfg.text(QStringLiteral("检查图层"))))fail(QStringLiteral("XML检查图层与CSV映射不一致"));
        QStringList targets=split(paths.actualShpSemicolon);if(targets.isEmpty())targets=paths.actualShpList;if(targets.isEmpty())targets=resolve(paths.shpPath,ctx.cfg.text(QStringLiteral("检查图层")));
        ctx.targets=read(ctx,targets,QString(),true);ctx.info["raw_feature_count"]=static_cast<int>(ctx.targets.size());ctx.info["total_count"]=static_cast<int>(ctx.targets.size());
        const QString type=ctx.cfg.text(QStringLiteral("检查类型"));
        if(type==QStringLiteral("桥梁"))checkBridges(ctx);
        else if(type==QStringLiteral("火车站")||type==QStringLiteral("隧道"))checkTransport(ctx);
        else if(type==QStringLiteral("河流"))checkRivers(ctx);
        else if(type==QStringLiteral("村庄"))checkVillages(ctx);
        else fail(QStringLiteral("不支持的冗余检查类型：%1").arg(type));
        ctx.info["error_count"]=ctx.errors.size();ctx.info["review_count"]=ctx.reviews.size();ctx.info["notice_count"]=ctx.notices.size();ctx.info["reviews"]=ctx.reviews;ctx.info["notices"]=ctx.notices;
        QJsonObject counts;for(const auto& entry:ctx.reviews){QString rule=entry.toObject()["rule"].toString();counts[rule]=counts[rule].toInt()+1;}ctx.info["diagnostic_counts"]=counts;ctx.info["diagnostics"]=ctx.reviews;ctx.info["unresolved_count"]=ctx.reviews.size();
        ctx.info["description"]=QStringLiteral("检查 %1 个对象：错误 %2，待复核 %3，提示 %4").arg(ctx.info["total_count"].toInt()).arg(ctx.errors.size()).arg(ctx.reviews.size()).arg(ctx.notices.size());
        ctx.info["xml_path"]=ctx.cfg.xml;ctx.info["severity"]=ctx.cfg.severity;ctx.info["scale"]=scale;ctx.info["json_path"]=jsonPath;ctx.info["log_path"]=logPath;ctx.info["verdict_complete"]=ctx.reviews.isEmpty();
        int redundancyCount=0;for(auto i=ctx.errors.begin();i!=ctx.errors.end();++i)if(i.value().toObject()["finding_type"].toString()=="redundancy")++redundancyCount;
        ctx.info["redundancy_count"]=redundancyCount;ctx.info["missing_count"]=ctx.errors.size()-redundancyCount;
        writeLocations(ctx,shpPath);
        result.status=QStringLiteral("质检成功");result.err_exist=!ctx.errors.isEmpty();result.info=ctx.info;result.errors=ctx.errors;
    }catch(const std::exception& e){result.info=ctx.info;result.info["message"]=QString::fromUtf8(e.what());result.info["reviews"]=ctx.reviews;result.info["diagnostics"]=ctx.reviews;result.info["verdict_complete"]=false;result.status=QStringLiteral("质检失败");result.err_exist=true;result.errors=ctx.errors;}
    if(!jsonPath.isEmpty()){
        result.info["json_path"]=jsonPath;result.info["log_path"]=logPath;
        auto outputFailure=[&](const std::exception& e){result.status=QStringLiteral("质检失败");result.err_exist=true;result.info["verdict_complete"]=false;result.info["output_error"]=QString::fromUtf8(e.what());};
        // QByteArray concatenation also uses a lazy builder under QT_USE_QSTRINGBUILDER.
        auto logData=[&]() -> QByteArray {return QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8()+" "+name.toUtf8()+" ID119 "+result.status.toUtf8()+"\n"+QJsonDocument(result.info).toJson(QJsonDocument::Indented);};
        try{writeBytes(logPath,logData());}catch(const std::exception& e){outputFailure(e);}
        try{QJsonObject item;item["id"]=result.id;item["name"]=result.name;item["status"]=result.status;item["err_exist"]=result.err_exist;item["info"]=result.info;item["errors"]=result.errors;QJsonObject root;root["check_item"]=item;writeBytes(jsonPath,QJsonDocument(root).toJson(QJsonDocument::Indented));}
        catch(const std::exception& e){outputFailure(e);try{writeBytes(logPath,logData());}catch(const std::exception&) {}}
    }
}
