#include "ContainmentCheckInterface.h"
#include "ContainmentCheckDialog.h"
#include "XmlConfigReader.h"
#include <QString>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QDateTime>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <algorithm>

#include <QDomDocument>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <cmath>
#include <climits>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace SpringRelationDetail {
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

// Only selects the new 113 sub-item; the original five branches retain their reader.
bool isSpringCheck(const char* xmlPath, int scale) {
    QFile file(str(xmlPath)); QDomDocument doc;
    if (!file.open(QIODevice::ReadOnly) || !doc.setContent(&file)) return false;
    QStringList exact, generic;
    const auto scales = doc.elementsByTagName("scale");
    for (int i=0; i<scales.size(); ++i) {
        const auto e=scales.at(i).toElement();
        const auto value=e.firstChildElement("value").text().trimmed();
        bool ok=false; const int n=value.toInt(&ok);
        const bool match=ok && n==scale, common=value.isEmpty() || (ok && n==0);
        if (!match && !common) continue;
        const auto items=e.elementsByTagName("check_item");
        for (int j=0; j<items.size(); ++j) {
            const auto item=items.at(j).toElement();
            if (item.firstChildElement("id").text().trimmed()!="113" &&
                item.firstChildElement("name").text().trimmed()!="关系检查") continue;
            const auto rules=item.elementsByTagName("rule");
            for (int k=0; k<rules.size(); ++k) {
                const auto r=rules.at(k).toElement();
                if (r.firstChildElement("name").text().trimmed()=="具体项")
                    (match?exact:generic).append(r.firstChildElement("value").text().trimmed());
            }
        }
    }
    return (exact.isEmpty()?generic:exact).contains("泉位置检查");
}

} // namespace SpringRelationDetail

// 辅助函数：生成关系检查错误JSON
static void generateRelationErrorJson(const char *szOutputJsonPath, const QString &errorInfo)
{
    qDebug() << "=== 开始生成关系检查错误JSON ===";
    if (!szOutputJsonPath)
        return;

    QString jsonPath = QString::fromUtf8(szOutputJsonPath);
    qDebug() << "生成的JSON文件路径:" << jsonPath;

    // 2. 确保输出目录存在
    QFileInfo jsonFileInfo(jsonPath);
    QDir jsonDir = jsonFileInfo.dir();
    if (!jsonDir.exists())
    {
        qDebug() << "创建JSON输出目录:" << jsonDir.path();
        if (!jsonDir.mkpath("."))
        {
            qCritical() << "❌ 无法创建JSON输出目录:" << jsonDir.path();
            return;
        }
    }

    // 3. 创建并写入JSON文件内容
    QJsonObject checkItem;
    checkItem["id"] = "113";
    checkItem["name"] = QString::fromUtf8(u8"关系检查");
    checkItem["status"] = "质检失败";
    checkItem["err_exist"] = true;
    checkItem["info"] = errorInfo;

    QJsonObject jsonLog;
    jsonLog["check_item"] = checkItem;

    QJsonDocument doc(jsonLog);

    QFile jsonFile(jsonPath);
    if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        jsonFile.write(doc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        qDebug() << "✓ JSON文件已创建:" << jsonPath;
    }
    else
    {
        qCritical() << "❌ 无法创建JSON文件:" << jsonPath;
    }
}

// 辅助函数：写入日志
static void writeRelationLogFile(const QString &logFilePath, const QString &message)
{
    if (logFilePath.isEmpty())
        return;

    try
    {
        QFile file(logFilePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            QTextStream stream(&file);
            stream.setCodec("UTF-8");
            QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
            stream << timestamp << " - " << message << "\n";
            file.close();
        }
    }
    catch (...)
    {
        qCritical() << "无法写入关系检查日志文件:" << logFilePath;
    }
}

static QStringList splitMultiPaths(const char *multiPaths)
{
    QStringList pathList;
    if (!multiPaths)
        return pathList;

    QString pathsStr = QString::fromUtf8(multiPaths);
    return pathsStr.split(';', Qt::SkipEmptyParts);
}

// 检查必要的SHP文件是否存在
static bool checkRequiredShpFiles(const QStringList &paths, const QString &fileType,
                                  const QString &logFilePath, const char *szOutputJsonPath,
                                  char *szActualJsonFilePath, const char *szXmlConfigPath)
{
    for (const QString &path : paths)
    {
        if (path.isEmpty())
        {
            QString errorMsg = QString(u8"%1路径为空").arg(fileType);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
        if (!QFileInfo::exists(path))
        {
            QString errorMsg = QString(u8"%1文件不存在: %2").arg(fileType).arg(path);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
        if (!path.toLower().endsWith(".shp"))
        {
            QString errorMsg = QString(u8"%1文件格式错误(非SHP文件): %2").arg(fileType).arg(path);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return false;
        }
    }
    return true;
}

SE_Error RelationshipCheck(
    const char *szXmlConfigPath,
    const char *szBasePolygonPath,
    const char *szBasePointPath,
    const char *szParsedPolygonPath,
    const char *szParsedPointPath,
    const char *szRiverPolygonPath,
    const char *szDitchFlowPaths,
    const char *szDitchPaths,
    const char *szBridgePaths,
    const char *szRoadPaths,
    const char *szTunnelEntrancePaths,
    const char *szTunnelPaths,
    const char *szRoadPathsForTunnel,
    const char *szOutputLogPath,   // 运行日志完整路径
    const char *szOutputErrorPath, // 错误要素基础路径
    const char *szOutputJsonPath,
    int scale)
{
    // 113泉位置检查直接在原接口内执行，不再调用独立算法入口。
    // 保持原接口签名：szDitchFlowPaths传泉解析文件，szDitchPaths传源河流线文件。
    if (SpringRelationDetail::isSpringCheck(szXmlConfigPath, scale)) {
        using namespace SpringRelationDetail;
        const char* xmlPath=szXmlConfigPath;
        const char* springPaths=szDitchFlowPaths;
        const char* riverLinePaths=szDitchPaths;
        const char* errorPath=szOutputErrorPath;
        const char* jsonPath=szOutputJsonPath;
        const char* logPath=szOutputLogPath;
    
        const QString xml=absolute(str(xmlPath)),output=absolute(str(errorPath)),json=absolute(str(jsonPath)),log=absolute(str(logPath));
        const auto springs=list(str(springPaths)),rivers=list(str(riverLinePaths));bool safe=false;QFile logfile;
        auto message=[&](const QString& text){if(logfile.isOpen()){QTextStream stream(&logfile);stream.setCodec("UTF-8");stream<<QDateTime::currentDateTime().toString(Qt::ISODate)<<" "<<text<<"\n";stream.flush();}};
        try{
            QStringList inputs=springs;inputs<<rivers<<xml;
            need(!json.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少JSON输出路径");
            for(const auto& path:inputs)if(!path.isEmpty()){
                const QFileInfo a(json),b(path);need(keyPath(json)!=keyPath(path)&&!keyPath(json).startsWith(keyPath(path)+"/")&&!(a.absolutePath().compare(b.absolutePath(),Qt::CaseInsensitive)==0&&a.completeBaseName().compare(b.completeBaseName(),Qt::CaseInsensitive)==0),SE_ERROR_INVALID_PARAMETER,"JSON不能覆盖输入数据");}
            need(keyPath(json)!=keyPath(output)&&keyPath(json)!=keyPath(log),SE_ERROR_INVALID_PARAMETER,"输出路径重复");safe=true;
            QSet<QString> used;
            for(const auto& p:QStringList{output,json,log}){
                need(!p.isEmpty()&&!used.contains(keyPath(p)),SE_ERROR_INVALID_PARAMETER,"输出路径为空或重复");used.insert(keyPath(p));
                for(const auto& input:inputs)if(!input.isEmpty()){
                    const QFileInfo a(p),b(input);need(keyPath(p)!=keyPath(input)&&!keyPath(p).startsWith(keyPath(input)+"/")&&!(a.absolutePath().compare(b.absolutePath(),Qt::CaseInsensitive)==0&&a.completeBaseName().compare(b.completeBaseName(),Qt::CaseInsensitive)==0),SE_ERROR_INVALID_PARAMETER,"输出不能覆盖输入数据："+p);}
            }
            parent(log);logfile.setFileName(log);need(logfile.open(QIODevice::WriteOnly|QIODevice::Truncate),SE_ERROR_WRITE_FILE_FAILED,"无法写入日志");
            need(scale>0,SE_ERROR_INVALID_PARAMETER,"比例尺分母必须大于0");
            need(!xml.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少XML规则");
            need(!springs.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少泉图层");need(!rivers.isEmpty(),SE_ERROR_INVALID_PARAMETER,"缺少河流线图层");
            QFile file(xml);QDomDocument doc;need(file.open(QIODevice::ReadOnly)&&doc.setContent(&file),SE_ERROR_XML_PARSE_FAILED,"无法读取XML："+xml);
            std::vector<QDomElement> exact,generic;
            auto scales=doc.elementsByTagName("scale");
            for(int i=0;i<scales.size();++i){auto e=scales.at(i).toElement();auto text=e.firstChildElement("value").text().trimmed();bool ok=false;int n=text.toInt(&ok);bool match=ok&&n==scale,common=text.isEmpty()||(ok&&n==0);if(!match&&!common)continue;
                auto items=e.elementsByTagName("check_item");for(int j=0;j<items.size();++j){auto item=items.at(j).toElement();if(item.firstChildElement("id").text().trimmed()!="113"&&item.firstChildElement("name").text().trimmed()!="关系检查")continue;
                    auto rules=item.elementsByTagName("rule");for(int k=0;k<rules.size();++k){auto r=rules.at(k).toElement();if(r.firstChildElement("name").text().trimmed()=="具体项"&&r.firstChildElement("value").text().trimmed()=="泉位置检查"){(match?exact:generic).push_back(item);break;}}}}
            const auto selected=exact.empty()?generic:exact;need(selected.size()==1,SE_ERROR_XML_PARSE_FAILED,"需要唯一匹配当前比例尺的泉位置检查配置");
            double mapDistance=-1;QString unit;auto nodes=selected.front().elementsByTagName("rule");int found=0;
            for(int i=0;i<nodes.size();++i){auto r=nodes.at(i).toElement();if(r.firstChildElement("name").text().trimmed()=="泉距河流端点距离阈值"||r.firstChildElement("name").text().trimmed()=="河源缓冲区半径"){
                bool ok=false;mapDistance=r.firstChildElement("value").text().toDouble(&ok);need(ok,SE_ERROR_XML_PARSE_FAILED,"泉距河流端点距离阈值不是数字");unit=r.firstChildElement("unit").text().trimmed();++found;}}
            need(found==1&&std::isfinite(mapDistance)&&mapDistance>=0,SE_ERROR_INVALID_PARAMETER,"必须填写唯一的非负泉距河流端点距离阈值");
            const QMap<QString,double> factors{{"",1},{"毫米",1},{"mm",1},{"厘米",10},{"cm",10},{"米",1000},{"m",1000}};
            need(factors.contains(unit),SE_ERROR_INVALID_PARAMETER,"不支持的图上距离单位："+unit);GDALAllRegister();
            for(const auto& path:inputs)need(QFileInfo::exists(path),SE_ERROR_FILE_NOT_FOUND,"输入文件不存在："+path);
            DS first=open(springs.front());auto target=layerSrs(first->GetLayer(0));need(target.IsProjected(),SE_ERROR_GEOGRAPHIC_CRS,"泉图层必须是投影坐标系");
            const double meters=target.GetLinearUnits(),threshold=mapDistance*factors.value(unit)*scale/1000.0/meters;
            need(std::isfinite(threshold)&&meters>0,SE_ERROR_INVALID_PARAMETER,"距离换算失败");
            std::vector<OGRPoint> endpoints;
            std::function<void(const OGRGeometry*)> ends=[&](const OGRGeometry* g){
                const auto type=wkbFlatten(g->getGeometryType());
                if(type==wkbLineString){const auto* line=g->toLineString();if(line->getNumPoints()>=2){endpoints.emplace_back(line->getX(0),line->getY(0));int last=line->getNumPoints()-1;endpoints.emplace_back(line->getX(last),line->getY(last));}return;}
                if(type==wkbMultiLineString){const auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)ends(c->getGeometryRef(i));return;}
                throw Failure(SE_ERROR_UNSUPPORTED_GEOMETRY_TYPE,"河流必须为线或多部件线");};
            QSet<QString> seenRivers;
            for(const auto& path:rivers){if(seenRivers.contains(keyPath(path)))continue;seenRivers.insert(keyPath(path));message("读取河流："+path);auto d=open(path);for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);auto t=transformation(l,target);while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,path);if(g)ends(g.get());}}}
            need(!endpoints.empty(),SE_ERROR_NOT_ENOUGH_DATA,"河流线图层没有可用端点，未完成检查");
            struct Record{Geom g;GIntBig fid;QString layer,symbol;};std::vector<Record> records;QMap<QString,std::vector<int>> groups;QSet<QString> seen;
            for(const auto& path:springs){if(seen.contains(keyPath(path)))continue;seen.insert(keyPath(path));auto d=open(path);for(int k=0;k<d->GetLayerCount();++k){auto* l=d->GetLayer(k);auto t=transformation(l,target);int field=l->GetLayerDefn()->GetFieldIndex("symbol_id");
                while(Feat f{l->GetNextFeature()}){auto g=geometry(f.get(),t,path);if(!g)continue;QString symbol;if(field>=0&&f->IsFieldSetAndNotNull(field))symbol=QString::fromUtf8(f->GetFieldAsString(field)).trimmed();
                    const QString key=symbol.isEmpty()?QString(path+"|"+QString::number(k)+"|"+QString::number(f->GetFID())):QString("symbol:"+symbol);
                    groups[key].push_back(static_cast<int>(records.size()));records.push_back({std::move(g),f->GetFID(),QString::fromUtf8(l->GetName()),symbol});}}}
            need(!records.empty(),SE_ERROR_NOT_ENOUGH_DATA,"泉图层没有可用要素，未完成检查");
            struct Error{QString key;std::vector<int> rows;double distance;};std::vector<Error> failures;QJsonObject errors;
            for(auto it=groups.constBegin();it!=groups.constEnd();++it){OGREnvelope box;bool initialized=false;for(int i:it.value()){OGREnvelope b;records[i].g->getEnvelope(&b);if(initialized)box.Merge(b);else{box=b;initialized=true;}}
                OGRPoint center((box.MinX+box.MaxX)/2,(box.MinY+box.MaxY)/2);double nearest=std::numeric_limits<double>::max();for(const auto& end:endpoints)nearest=std::min(nearest,std::hypot(center.getX()-end.getX(),center.getY()-end.getY()));
                if(nearest>threshold+1e-9){failures.push_back({it.key(),it.value(),nearest});const QString text=QString("泉[%1]距最近河流端点的图上距离为%2毫米，超过图上阈值%3毫米（实地距离%4米，实地阈值%5米）").arg(it.key()).arg(nearest*meters*1000.0/scale,0,'f',3).arg(mapDistance*factors.value(unit),0,'g',12).arg(nearest*meters,0,'f',3).arg(threshold*meters,0,'f',3);errors["error"+QString::number(failures.size())]=text;message(text);}}
            Output out(output,target,records.front().g->getGeometryType(),!failures.empty());
            if(out.layer){out.field("OBJECT_ID",OFTInteger);out.field("SRC_FID",OFTInteger64);out.field("SRC_LAYER",OFTString);out.field("SYMBOL_ID",OFTString);out.field("DIST_M",OFTReal);out.field("LIMIT_M",OFTReal);
                int id=0;for(const auto& error:failures){++id;for(int index:error.rows){const auto& r=records[index];Feat f(OGRFeature::CreateFeature(out.layer->GetLayerDefn()));f->SetGeometry(r.g.get());f->SetField("OBJECT_ID",id);f->SetField("SRC_FID",r.fid);f->SetField("SRC_LAYER",r.layer.toUtf8().constData());f->SetField("SYMBOL_ID",r.symbol.toUtf8().constData());f->SetField("DIST_M",error.distance*meters);f->SetField("LIMIT_M",threshold*meters);out.write(f.get());}}}out.ds.reset();
            const QString info=QString("泉位置检查：按symbol_id分组检查%1个完整泉符号，%2个泉距最近河流端点的距离超过图上阈值%3毫米（对应实地%4米）").arg(groups.size()).arg(failures.size()).arg(mapDistance*factors.value(unit),0,'g',12).arg(threshold*meters,0,'f',3);message(info);
            writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","113"},{"name","关系检查"},{"status","质检成功"},{"err_exist",!failures.empty()},{"info",info},{"errors",errors}}}});return SE_ERROR_NONE;
        }catch(const std::exception& e){const auto* f=dynamic_cast<const Failure*>(&e);const int code=f?f->code:SE_ERROR_FAILURE;const QString text=QString::fromUtf8(e.what());message(text);if(safe)try{writeJson(json,QJsonObject{{"check_item",QJsonObject{{"id","113"},{"name","关系检查"},{"status","质检失败"},{"err_exist",true},{"info",text},{"errors",QJsonObject{{"execution_error",text}}}}}});}catch(...){}return static_cast<SE_Error>(code);}
    }

    // === 接口层错误检查 ===

    // 1. 检查空指针
    if (!szXmlConfigPath || !szOutputLogPath || !szOutputErrorPath || !szOutputJsonPath)
    {
        generateRelationErrorJson(szOutputJsonPath, "输入参数包含空指针");
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 2. 检查XML文件是否存在
    QString xmlPathStr = QString::fromUtf8(szXmlConfigPath);
    if (!QFileInfo::exists(xmlPathStr))
    {
        QString errorMsg = QString(u8"XML配置文件不存在: %1").arg(szXmlConfigPath);
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // 3. 检查比例尺有效性
    if (scale <= 0)
    {
        QString errorMsg = QString(u8"无效的比例尺: %1").arg(scale);
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_INVALID_PARAMETER;
    }

    // 4. 检查输出日志目录
    QString logPathStr = QString::fromUtf8(szOutputLogPath);
    QFileInfo logPathInfo(logPathStr);
    QDir logDir = logPathInfo.dir();
    if (!logDir.exists() && !logDir.mkpath("."))
    {
        QString errorMsg = QString(u8"无法创建日志目录: %1").arg(logDir.path());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_WRITE_FILE_FAILED;
    }

    // 5. 检查错误要素输出目录
    QString errorPathStr = QString::fromUtf8(szOutputErrorPath);
    QFileInfo errorFileInfo(errorPathStr);
    QDir errorDir = errorFileInfo.dir();
    if (!errorDir.exists() && !errorDir.mkpath("."))
    {
        QString errorMsg = QString(u8"无法创建错误要素输出目录: %1").arg(errorDir.path());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FILE_NOT_FOUND;
    }

    // === 使用传入的完整路径 ===
    QString logFilePath = QString::fromUtf8(szOutputLogPath);
    QString errorFilePath = QString::fromUtf8(szOutputErrorPath);
    QString jsonFilePath = QString::fromUtf8(szOutputJsonPath);

    // 写入开始日志
    writeRelationLogFile(logFilePath, "开始执行关系检查接口...");
    writeRelationLogFile(logFilePath, QString("XML路径: %1").arg(szXmlConfigPath));
    writeRelationLogFile(logFilePath, QString("输出日志: %1").arg(logFilePath));
    writeRelationLogFile(logFilePath, QString("错误要素路径: %1").arg(errorFilePath));
    writeRelationLogFile(logFilePath, QString("JSON报告路径: %1").arg(jsonFilePath));
    writeRelationLogFile(logFilePath, QString("比例尺: 1:%1").arg(scale));

    try
    {
        // 尝试加载XML配置
        writeRelationLogFile(logFilePath, "正在加载XML配置...");
        XmlConfigReader configReader;
        if (!configReader.loadConfig(xmlPathStr))
        {
            QString errorMsg = QString(u8"无法加载XML配置: %1").arg(xmlPathStr);
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_FILE_NOT_FOUND;
        }
        writeRelationLogFile(logFilePath, "✓ XML配置加载成功");

        // 检查比例尺
        writeRelationLogFile(logFilePath, QString("检查比例尺: 1:%1").arg(scale));
        QList<int> availableScales = configReader.getAvailableScales();

        if (!configReader.hasScale(scale))
        {
            QStringList scaleStrings;
            for (int s : availableScales)
                scaleStrings << QString::number(s);
            QString errorMsg = QString(u8"XML配置中不存在比例尺1:%1，可用比例尺: %2").arg(scale).arg(scaleStrings.join(", "));
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, "✓ 比例尺检查通过");

        // 检查检查项
        QString relationCheckItemName = QString::fromUtf8(u8"关系检查");
        if (!configReader.hasCheckItem(scale, relationCheckItemName))
        {
            QList<QString> itemNames = configReader.getCheckItemNames(scale);
            QString errorMsg = QString(u8"比例尺1:%1下不存在检查项'%2'，可用检查项: %3")
                                   .arg(scale)
                                   .arg(relationCheckItemName)
                                   .arg(itemNames.join(", "));
            writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
            generateRelationErrorJson(szOutputJsonPath, errorMsg);
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, "✓ 检查项存在");

        // 获取检查类型
        QString checkType = configReader.getRuleStringValue(scale, relationCheckItemName, QString::fromUtf8(u8"具体项"), "");
        if (checkType.isEmpty())
        {
            writeRelationLogFile(logFilePath, "❌ 错误: 检查类型为空");
            generateRelationErrorJson(szOutputJsonPath, "检查类型为空");
            return SE_ERROR_INVALID_PARAMETER;
        }
        writeRelationLogFile(logFilePath, QString("检查类型: %1").arg(checkType));

        // === 根据检查类型验证必要的SHP文件 ===
        writeRelationLogFile(logFilePath, "开始验证必要的SHP文件...");

        if (checkType == "行政面包含检查")
        {
            // 行政面包含检查需要的文件
            QStringList requiredFiles;
            if (szBasePolygonPath)
                requiredFiles << QString::fromUtf8(szBasePolygonPath);
            if (szBasePointPath)
                requiredFiles << QString::fromUtf8(szBasePointPath);
            if (szParsedPolygonPath)
                requiredFiles << QString::fromUtf8(szParsedPolygonPath);
            if (szParsedPointPath)
                requiredFiles << QString::fromUtf8(szParsedPointPath);

            if (!checkRequiredShpFiles(requiredFiles, u8"行政面包含检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "桥梁位置检查")
        {
            // 桥梁位置检查需要的文件
            QStringList requiredFiles;
            if (szBridgePaths)
                requiredFiles.append(splitMultiPaths(szBridgePaths));
            if (szRiverPolygonPath)
                requiredFiles << QString::fromUtf8(szRiverPolygonPath);
            if (szRoadPaths)
                requiredFiles.append(splitMultiPaths(szRoadPaths));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"桥梁位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"桥梁位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "隧道口位置检查")
        {
            // 隧道口位置检查需要的文件
            QStringList requiredFiles;
            if (szTunnelEntrancePaths)
                requiredFiles.append(splitMultiPaths(szTunnelEntrancePaths));
            if (szTunnelPaths)
                requiredFiles.append(splitMultiPaths(szTunnelPaths));
            if (szRoadPathsForTunnel)
                requiredFiles.append(splitMultiPaths(szRoadPathsForTunnel));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"隧道口位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"隧道口位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "渠流向位置检查")
        {
            // 渠流向位置检查需要的文件
            QStringList requiredFiles;
            if (szDitchFlowPaths)
                requiredFiles.append(splitMultiPaths(szDitchFlowPaths));
            if (szDitchPaths)
                requiredFiles.append(splitMultiPaths(szDitchPaths));

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"渠流向位置检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"渠流向位置检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }
        else if (checkType == "河流包含检查")
        {
            // 河流包含检查需要的文件
            QStringList requiredFiles;
            if (szParsedPointPath)
                requiredFiles << QString::fromUtf8(szParsedPointPath);
            if (szRiverPolygonPath)
                requiredFiles << QString::fromUtf8(szRiverPolygonPath);

            if (requiredFiles.isEmpty())
            {
                QString errorMsg = u8"河流包含检查需要的SHP文件路径都为空";
                writeRelationLogFile(logFilePath, "❌ 错误: " + errorMsg);
                generateRelationErrorJson(szOutputJsonPath, errorMsg);
                return SE_ERROR_INVALID_PARAMETER;
            }

            if (!checkRequiredShpFiles(requiredFiles, u8"河流包含检查必要文件",
                                       logFilePath, szOutputJsonPath, nullptr, szXmlConfigPath))
            {
                return SE_ERROR_FILE_NOT_FOUND;
            }
        }

        writeRelationLogFile(logFilePath, "✓ 所有必要的SHP文件验证通过");

        // 创建并配置对话框
        ContainmentCheckDialog dialog(nullptr, nullptr);

        // 设置基本参数
        dialog.setScale(scale);
        dialog.setXmlConfigPath(xmlPathStr);

        // 设置输出路径（完整路径）
        dialog.setOutputLogPath(logFilePath);
        dialog.setOutputErrorPath(errorFilePath);
        dialog.setOutputJsonPath(jsonFilePath);

        writeRelationLogFile(logFilePath, QString("开始执行检查: %1").arg(checkType));

        // 设置所有数据路径
        if (szBasePolygonPath)
        {
            dialog.setBasePolygonPath(QString::fromUtf8(szBasePolygonPath));
            writeRelationLogFile(logFilePath, QString("设置库体面路径: %1").arg(szBasePolygonPath));
        }
        if (szBasePointPath)
        {
            dialog.setBasePointPath(QString::fromUtf8(szBasePointPath));
            writeRelationLogFile(logFilePath, QString("设置库体点路径: %1").arg(szBasePointPath));
        }
        if (szParsedPolygonPath)
        {
            dialog.setParsedPolygonPath(QString::fromUtf8(szParsedPolygonPath));
            writeRelationLogFile(logFilePath, QString("设置解析面路径: %1").arg(szParsedPolygonPath));
        }
        if (szParsedPointPath)
        {
            dialog.setParsedPointPath(QString::fromUtf8(szParsedPointPath));
            writeRelationLogFile(logFilePath, QString("设置解析点路径: %1").arg(szParsedPointPath));
        }
        if (szRiverPolygonPath)
        {
            dialog.setRiverPolygonPath(QString::fromUtf8(szRiverPolygonPath));
            writeRelationLogFile(logFilePath, QString("设置河流面路径: %1").arg(szRiverPolygonPath));
        }

        // 设置多路径数据
        if (szDitchFlowPaths)
        {
            dialog.setDitchFlowPaths(splitMultiPaths(szDitchFlowPaths));
            writeRelationLogFile(logFilePath, QString("设置渠流向路径: %1").arg(szDitchFlowPaths));
        }
        if (szDitchPaths)
        {
            dialog.setDitchPaths(splitMultiPaths(szDitchPaths));
            writeRelationLogFile(logFilePath, QString("设置沟渠路径: %1").arg(szDitchPaths));
        }
        if (szBridgePaths)
        {
            dialog.setBridgePaths(splitMultiPaths(szBridgePaths));
            writeRelationLogFile(logFilePath, QString("设置桥梁路径: %1").arg(szBridgePaths));
        }
        if (szRoadPaths)
        {
            dialog.setRoadPaths(splitMultiPaths(szRoadPaths));
            writeRelationLogFile(logFilePath, QString("设置道路路径: %1").arg(szRoadPaths));
        }
        if (szTunnelEntrancePaths)
        {
            dialog.setTunnelEntrancePaths(splitMultiPaths(szTunnelEntrancePaths));
            writeRelationLogFile(logFilePath, QString("设置隧道口路径: %1").arg(szTunnelEntrancePaths));
        }
        if (szTunnelPaths)
        {
            dialog.setTunnelPaths(splitMultiPaths(szTunnelPaths));
            writeRelationLogFile(logFilePath, QString("设置隧道路径: %1").arg(szTunnelPaths));
        }
        if (szRoadPathsForTunnel)
        {
            dialog.setRoadPathsForTunnel(splitMultiPaths(szRoadPathsForTunnel));
            writeRelationLogFile(logFilePath, QString("设置隧道相关道路路径: %1").arg(szRoadPathsForTunnel));
        }

        writeRelationLogFile(logFilePath, QString("根据XML配置调用检查方法: %1").arg(checkType));

        // 执行具体的检查方法
        if (checkType == "行政面包含检查")
        {
            writeRelationLogFile(logFilePath, "调用行政面包含检查...");
            dialog.runAdministrativeContainmentCheck();
        }
        else if (checkType == "桥梁位置检查")
        {
            writeRelationLogFile(logFilePath, "调用桥梁位置检查...");
            dialog.runBridgePositionCheck();
        }
        else if (checkType == "隧道口位置检查")
        {
            writeRelationLogFile(logFilePath, "调用隧道口位置检查...");
            dialog.runTunnelEntrancePositionCheck();
        }
        else if (checkType == "渠流向位置检查")
        {
            writeRelationLogFile(logFilePath, "调用渠流向位置检查...");
            dialog.runDitchFlowPositionCheck();
        }
        else if (checkType == "河流包含检查")
        {
            writeRelationLogFile(logFilePath, "调用河流包含检查...");
            dialog.runRiverContainmentCheck();
        }
        else
        {
            writeRelationLogFile(logFilePath, QString("❌ 错误: 不支持的检查类型: %1").arg(checkType));
            generateRelationErrorJson(szOutputJsonPath, "不支持的检查类型");
            return SE_ERROR_INVALID_PARAMETER;
        }

        writeRelationLogFile(logFilePath, "✓ 检查执行完成");
        return SE_ERROR_NONE;
    }
    catch (const std::exception &e)
    {
        writeRelationLogFile(logFilePath, QString("接口执行异常: %1").arg(e.what()));
        QString errorMsg = QString("程序异常: %1").arg(e.what());
        generateRelationErrorJson(szOutputJsonPath, errorMsg);
        return SE_ERROR_FAILURE;
    }
    catch (...)
    {
        writeRelationLogFile(logFilePath, "接口执行未知异常");
        generateRelationErrorJson(szOutputJsonPath, "未知异常");
        return SE_ERROR_FAILURE;
    }
}
