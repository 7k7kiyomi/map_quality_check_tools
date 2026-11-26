#include "se_feature_redundancy_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace redundancy {
namespace {
void endpoints(const OGRGeometry* g,std::vector<OGRPoint>& points) {
    if(!g)return;
    auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbLineString){auto* line=static_cast<const OGRLineString*>(g);if(line->getNumPoints()>1){OGRPoint a,b;line->StartPoint(&a);line->EndPoint(&b);points.push_back(a);points.push_back(b);}}
    else if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)){auto* group=static_cast<const OGRGeometryCollection*>(g);for(int i=0;i<group->getNumGeometries();++i)endpoints(group->getGeometryRef(i),points);}
}
bool validBusinessValue(const QString& value) {
    return reliableResultId(value);
}
QString businessId(const Feature& f,const QString& configured) {
    QStringList fields;
    if(!configured.trimmed().isEmpty())fields.push_back(configured.trimmed());
    for(const auto& field:{"WRID","ELEMID","ID"})if(!fields.contains(QString::fromLatin1(field)))fields.push_back(QString::fromLatin1(field));
    for(const auto& field:fields){const QString value=f.text(field).trimmed();if(validBusinessValue(value))return field.toUpper()+":"+value;}
    return {};
}
bool continuousSkeleton(const OGRGeometry* geometry,double tolerance) {
    if(!geometry||geometry->IsEmpty()||!geometry->IsSimple())return false;
    std::vector<const OGRLineString*> lines;
    std::function<void(const OGRGeometry*)> collect=[&](const OGRGeometry* g){auto type=wkbFlatten(g->getGeometryType());if(type==wkbLineString)lines.push_back(static_cast<const OGRLineString*>(g));else if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)){auto* group=static_cast<const OGRGeometryCollection*>(g);for(int i=0;i<group->getNumGeometries();++i)collect(group->getGeometryRef(i));}};
    collect(geometry);if(lines.empty())return false;
    std::vector<int> parent(lines.size());std::iota(parent.begin(),parent.end(),0);std::function<int(int)> root=[&](int i){return parent[i]==i?i:parent[i]=root(parent[i]);};
    std::vector<std::vector<OGRPoint>> ends(lines.size());for(size_t i=0;i<lines.size();++i)endpoints(lines[i],ends[i]);
    for(size_t i=0;i<lines.size();++i)for(size_t j=i+1;j<lines.size();++j)for(const auto& a:ends[i])for(const auto& b:ends[j])if(a.Distance(&b)<=tolerance)parent[root(static_cast<int>(j))]=root(static_cast<int>(i));
    for(size_t i=1;i<lines.size();++i)if(root(static_cast<int>(i))!=root(0))return false;
    for(const auto& points:ends)for(const auto& p:points){int degree=0;for(const auto& others:ends)for(const auto& q:others)if(p.Distance(&q)<=tolerance)++degree;if(degree>2)return false;}
    return true;
}
// Stable identifiers do not override topology: only an unbranched connected skeleton is merged.
std::vector<Feature> riverSources(Context& ctx,std::vector<Feature> input,const QString& levelField,const QString& typeField,QMap<QString,QString>& incomplete) {
    const double tolerance=ctx.fromMm(0.00001);QMap<QString,std::vector<Feature>> buckets;
    const QString idField=ctx.cfg.text(QStringLiteral("河流业务标识字段"),QStringLiteral("WRID"));
    // An anonymous endpoint adjoining another compatible row may be a split business river.
    // Keep its known geometry/category usable, but do not infer complete length from that row.
    for(size_t i=0;i<input.size();++i)if(businessId(input[i],idField).isEmpty()&&input[i].geom){
        std::vector<OGRPoint> ends;endpoints(input[i].geom.get(),ends);
        for(size_t j=0;j<input.size();++j)if(i!=j&&input[j].geom&&input[i].text(levelField)==input[j].text(levelField)&&input[i].text(typeField)==input[j].text(typeField)){
            bool adjoining=false;for(const auto& p:ends)if(input[j].geom->Distance(&p)<=tolerance)adjoining=true;
            if(adjoining){const QString why=QStringLiteral("相接骨架含缺少稳定业务标识的记录，可能为同一河流碎段，完整长度不能确认");incomplete[input[i].key]=why;incomplete[input[j].key]=why;}
        }
    }
    // A catalog WRID may be reused by distinct, qualified rivers (for example a
    // second-grade main river and a third-grade tributary). Distinct reliable ELEMIDs
    // permit those qualification groups to stay separate; one conflicting ELEMID does not.
    const QString nameField=ctx.cfg.text(QStringLiteral("名称字段"),"NAME");
    auto qualification=[&](const Feature& f){return QString("%1|%2|%3").arg(f.text(levelField),f.text(typeField),f.text(nameField));};
    QMap<QString,QSet<QString>> qualifications;QMap<QString,QMap<QString,QSet<QString>>> elemQualifications;
    for(const auto& f:input){const QString id=businessId(f,idField);if(id.isEmpty())continue;const QString q=qualification(f),elem=f.text("ELEMID");qualifications[id].insert(q);
        if(reliableResultId(elem))elemQualifications[id][elem].insert(q);}
    for(auto& f:input){QString id=businessId(f,idField);if(!id.isEmpty()&&qualifications[id].size()>1){const QString elem=f.text("ELEMID");
            if(!reliableResultId(elem)){incomplete[f.key]=QStringLiteral("复用业务标识下缺少可靠ELEMID，无法确认该参考对象独立身份");id+=QString("|UNRESOLVED:")+f.key;}
            else if(elemQualifications[id][elem].size()>1)id+=QString("|CONFLICT_ELEMID:")+elem;
            else id+=QString("|QUAL:")+qualification(f);}
        buckets[id.isEmpty()?f.key:id].push_back(std::move(f));}
    std::vector<Feature> result;
    for(auto bucket=buckets.begin();bucket!=buckets.end();++bucket) {
        auto& rows=bucket.value();std::vector<int> parent(rows.size());std::iota(parent.begin(),parent.end(),0);
        QSet<QString> names;for(const auto& row:rows){auto name=row.text(ctx.cfg.text(QStringLiteral("名称字段"),"NAME"));if(!name.isEmpty())names.insert(name);}
        if(names.size()>1){for(auto& row:rows){incomplete[row.key]=QStringLiteral("同一业务标识对应不同河流名称，不能可靠归并");result.push_back(std::move(row));}continue;}
        std::function<int(int)> root=[&](int i){return parent[i]==i?i:parent[i]=root(parent[i]);};
        std::vector<std::vector<OGRPoint>> ends(rows.size());for(size_t i=0;i<rows.size();++i)endpoints(rows[i].geom.get(),ends[i]);
        bool branched=false;
        for(size_t i=0;i<rows.size();++i)for(size_t j=i+1;j<rows.size();++j){
            if(rows[i].text(levelField)!=rows[j].text(levelField)||rows[i].text(typeField)!=rows[j].text(typeField))continue;
            bool joined=false;for(const auto& a:ends[i])for(const auto& b:ends[j])if(a.Distance(&b)<=tolerance)joined=true;
            if(joined)parent[root(static_cast<int>(j))]=root(static_cast<int>(i));
        }
        for(size_t i=0;i<ends.size();++i)for(const auto& p:ends[i]){int degree=0;for(const auto& points:ends)for(const auto& q:points)if(p.Distance(&q)<=tolerance)++degree;if(degree>2)branched=true;}
        if(branched&&rows.size()>1){for(auto& row:rows){incomplete[row.key]=QStringLiteral("同一业务标识存在分支，完整河流归并不确定");result.push_back(std::move(row));}continue;}
        QMap<int,std::vector<int>> groups;for(size_t i=0;i<rows.size();++i)groups[root(static_cast<int>(i))].push_back(static_cast<int>(i));
        if(groups.size()>1&&rows.size()>1){for(auto& row:rows){incomplete[row.key]=QStringLiteral("同一业务标识的骨架不连续，完整河流无法可靠恢复");result.push_back(std::move(row));}continue;}
        for(auto group=groups.begin();group!=groups.end();++group){Feature f=rows[group.value().front()];std::vector<Geo> geoms;f.fids=QJsonArray();
            for(int i:group.value()){
                for(const auto& fid:rows[i].fids)f.fids.append(fid);
                const QString elemId=rows[i].text(QStringLiteral("ELEMID")).trimmed();
                if(validBusinessValue(elemId))f.memberElemIds.insert(elemId);
                f.memberElemIds.unite(rows[i].memberElemIds);
                if(!rows[i].problem.isEmpty())f.problem=rows[i].problem;
                if(incomplete.contains(rows[i].key))incomplete[f.key]=incomplete[rows[i].key];
                geoms.push_back(rows[i].geom);
            }
            f.geom=unite(geoms);if(!continuousSkeleton(f.geom.get(),tolerance))incomplete[f.key]=QStringLiteral("完整河流骨架存在不连续或分支，不能用各分量长度之和判断选取资格");result.push_back(std::move(f));}
    }
    return result;
}
bool confluence(const Geo& a,const Geo& b,double epsilon) {
    if(!a||!b||a->Distance(b.get())>epsilon)return false;
    auto shared=geometry(a->Intersection(b.get()));if(!shared||shared->IsEmpty()||length(shared.get())>epsilon)return false;
    std::vector<OGRPoint> ends;endpoints(a.get(),ends);endpoints(b.get(),ends);
    for(const auto& p:ends)if(shared->Distance(&p)<=epsilon)return true;
    return false;
}
int gradePartition(const QString& level);
bool reviewOwnedByTask(int owner,const QString& level,const QString& type,const QString& dry) {
    if(owner<0)return true;
    if(owner==0)return type==dry;
    const int partition=gradePartition(level);
    if(partition>=0)return owner==partition;
    // Unknown-grade records use one deterministic task as their review home.
    return owner==2;
}
QString riverPairKey(const QString& prefix,const Feature& a,const Feature& b) {
    QStringList keys{a.key,b.key};keys.sort();
    return prefix+keys.join(QStringLiteral("|"));
}
Geo safeArea(Context& ctx,const QString& name,const QString& rule) {
    try {auto value=ctx.area(name,false);if(!value)ctx.review(rule,QStringLiteral("缺少范围面：%1；该范围子规则无法判定").arg(name));return value;}
    catch(const std::exception& e){ctx.review(rule,QString::fromUtf8(e.what()));return {};}
}
QString labelText(const Feature& feature,const Config& cfg) {
    QString field=cfg.text(QStringLiteral("河流注记文字字段"),cfg.text(QStringLiteral("注记文本字段")));
    if(!field.isEmpty())return feature.text(field);
    // These are explicit readable text attributes. Drawn glyph outlines are never interpreted as words.
    for(const auto& name:{"text_conte","TEXT","TEXTSTRING","CONTENT"}){auto value=feature.text(name);if(!value.isEmpty())return value;}
    return {};
}
QString compact(QString value){return value.remove(QRegularExpression(QStringLiteral("\\s+")));}
struct River {Feature shown,source;Geo effective;QString level,type;int partition=-1;double mm=0;bool lengthKnown=true;};
int gradePartition(const QString& level) {
    const QStringList grades{QStringLiteral("一级"),QStringLiteral("二级"),QStringLiteral("三级"),QStringLiteral("四级"),QStringLiteral("五级")};
    int index=grades.indexOf(level);return index>=0?index+3:level==QStringLiteral("六级")?1:level==QStringLiteral("等外")?2:-1;
}
}

void checkRivers(Context& ctx) {
    const QString levelField=ctx.cfg.text(QStringLiteral("等级字段"),"LEVEL"),typeField=ctx.cfg.text(QStringLiteral("类型字段"),"TYPE");
    const QString nameField=ctx.cfg.text(QStringLiteral("名称字段"),"NAME"),dry=ctx.cfg.text(QStringLiteral("干涸河属性值"),QStringLiteral("干涸河"));
    const auto major=ctx.cfg.list(QStringLiteral("全部表示等级"),QStringLiteral("一级;二级;三级;四级;五级"));
    const auto selected=ctx.cfg.list(QStringLiteral("选取表示等级"),QStringLiteral("六级;等外"));
    const auto knownTypes=ctx.cfg.list(QStringLiteral("河流类型字典"),QStringLiteral("常年河;季节河;时令河;干涸河;一般河流;河流"));
    const auto reliableType=[&](const QString& type){return knownTypes.contains(type);};
    const double radius=ctx.fromMm(ctx.cfg.number(QStringLiteral("候选匹配搜索半径"),2)),minimum=ctx.cfg.number(QStringLiteral("最小选取长度"),15);
    const QString lengthOp=ctx.cfg.text(QStringLiteral("最小选取长度比较符"),">");comparison(0,lengthOp,minimum);
    const double spacing=ctx.cfg.number(QStringLiteral("选取参考最小间距"),5),epsilon=ctx.fromMm(.00001);
    const QString spacingOp=ctx.cfg.text(QStringLiteral("选取参考最小间距比较符"),">=");comparison(0,spacingOp,spacing);
    const double requiredName=ctx.cfg.number(QStringLiteral("六级名称必注长度"),30);
    const QString nameOp=ctx.cfg.text(QStringLiteral("六级名称必注长度比较符"),">=");comparison(0,nameOp,requiredName);
    const QString layer=ctx.paths.standardLayer;
    const int owner=layer.contains(QStringLiteral("干涸"))?0:gradePartition(ctx.cfg.text(QStringLiteral("任务目标资格"),QString(layer).remove(QStringLiteral("河流"))));
    const auto owns=[&](const QString& level,const QString& type){return owner<0||(owner==0?type==dry:gradePartition(level)==owner)||(owner==2&&type==dry&&gradePartition(level)<0);};
    ctx.info["river_partition_mode"]=QStringLiteral("按等级包含干涸河");
    // A rendered symbol or WRID can be reused by distinct river objects. Keep each
    // reliable ELEMID separate, and diagnose symbol-ID collisions instead of merging
    // their geometry/attributes into one identity that can hide a mandatory object.
    const QString symbolField=ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id");
    const QString wridField=ctx.cfg.text(QStringLiteral("成果业务标识字段"),"WRID");
    QMap<QString,QSet<QString>> symbolElemIds;
    for(const auto& target:ctx.targets){const QString symbol=target.text(symbolField),elem=target.text("ELEMID");
        if(reliableResultId(symbol)&&reliableResultId(elem))symbolElemIds[symbol].insert(elem);}
    for(auto& target:ctx.targets){
        target.attrs.remove("RED_GROUP");
        const QString symbol=target.text(symbolField),elem=target.text("ELEMID"),wrid=target.text(wridField);
        if(reliableResultId(symbol)){
            QString group="SYMBOL:"+symbol;
            if(reliableResultId(elem))group+="|ELEMID:"+elem;
            target.attrs["RED_GROUP"]=group;
            if(symbolElemIds.value(symbol).size()>1){QJsonObject detail;detail["symbol_id"]=symbol;detail["elem_id"]=elem;
                detail["member_elem_ids"]=QJsonArray::fromStringList(symbolElemIds.value(symbol).values());
                ctx.review(QStringLiteral("河流符号标识对应多个业务要素"),QStringLiteral("同一符号标识包含多个可靠ELEMID，已分别保留各要素；请核对符号标识是否复用。"),&target,detail);}
        }else if(reliableResultId(elem))target.attrs["RED_GROUP"]="ELEMID:"+elem;
        else if(reliableResultId(wrid))target.attrs["RED_GROUP"]="WRID:"+wrid;
    }
    ctx.targets=resultSymbols(ctx,std::move(ctx.targets),"RED_GROUP",{levelField,typeField,nameField},QStringLiteral("河流成果属性冲突"));
    auto validAttribute=[&](const QString& value){return resultAttributePresent(value);};
    auto missing=[&](const Feature& feature,const QString& field){return !feature.attributeConflicts.contains(field.toUpper())&&!validAttribute(feature.text(field));};
    auto attributeQuery=[&](const Feature& target){Feature query=target;const QString id=query.text("ELEMID");
        if(!reliableResultId(id))query.attrs.remove("ELEMID");return query;};
    Geo frame=safeArea(ctx,ctx.cfg.text(QStringLiteral("检查范围图层"),QStringLiteral("内图廓")),QStringLiteral("河流检查范围"));
    if(!frame){ctx.info["scope_verdict_blocked"]=true;return;}
    std::vector<Feature> annotations;bool annotationComplete=false,annotationReadOkay=true;
    try{annotations=ctx.actual(ctx.cfg.text(QStringLiteral("河流注记图层"),QStringLiteral("水系_标注")),false);}
    catch(const std::exception& e){annotationReadOkay=false;ctx.review(QStringLiteral("河流注记输入异常"),QString::fromUtf8(e.what()));}
    const QString annotationLayer=ctx.cfg.text(QStringLiteral("河流注记图层"),QStringLiteral("水系_标注"));
    for(const auto& suffix:{QString(),QString("_Point"),QString("_LineString"),QString("_Polygon")})if(QFileInfo::exists(QDir(ctx.paths.shpPath).filePath(annotationLayer+suffix+".shp")))annotationComplete=true;
    for(const auto& annotation:annotations)if(!annotation.problem.isEmpty()||!annotation.geom||labelText(annotation,ctx.cfg).isEmpty())annotationComplete=false;
    annotationComplete=annotationComplete&&annotationReadOkay;
    bool needSource=false;for(const auto& target:ctx.targets)if(missing(target,levelField)||missing(target,typeField)||(!annotations.empty()&&missing(target,nameField)))needSource=true;
    std::vector<Feature> sources;
    // Dataset readability is global. Geometry/identity problems belong to individual
    // reference objects and must not suppress other known mandatory inventory entries.
    bool inventoryComplete=true;QMap<QString,QString> inventoryProblems;QSet<QString> inventoryKeys,blockedInventory;
    if(needSource||owner==2||owner<0||owner>=3){try{auto raw=ctx.gdb(ctx.cfg.text(QStringLiteral("参考GDB图层"),"HYDL_HL"),ctx.cfg.text(QStringLiteral("属性候选筛选条件")),false);
            if(raw.empty())inventoryComplete=false;
            if(owner==2||owner<0||owner>=3)for(const auto& f:raw)if(!major.contains(f.text(levelField))&&!selected.contains(f.text(levelField))){bool associated=false;
                for(const auto& actual:ctx.targets)if(match(attributeQuery(actual),std::vector<Feature>{f},radius,true)>=0)associated=true;
                if(!associated)ctx.review(QStringLiteral("河流清单等级未知"),QStringLiteral("HYDL_HL清单存在未知或缺失LEVEL，相关必表分类核对未完成。"),&f);}
            for(const auto& f:raw)if(!f.problem.isEmpty()||!f.geom||!f.geom->IsValid()||length(f.geom.get())<=0){blockedInventory.insert(f.key);
                if(reviewOwnedByTask(owner,f.text(levelField),f.text(typeField),dry))ctx.review(QStringLiteral("河流属性参考异常"),QStringLiteral("该参考对象几何异常，其属性补全及必表缺失核对未完成；其他可靠对象继续检查。"),&f);}
            const QString inventoryFilter=ctx.cfg.text(QStringLiteral("必表清单筛选条件"));
            if(owner<0||owner>=3){auto inventory=inventoryFilter.isEmpty()?raw:ctx.gdb(ctx.cfg.text(QStringLiteral("参考GDB图层"),"HYDL_HL"),inventoryFilter,false);for(const auto& f:inventory)inventoryKeys.insert(f.key);}
            sources=riverSources(ctx,std::move(raw),levelField,typeField,inventoryProblems);
            for(const auto& f:sources)if(inventoryProblems.contains(f.key)){blockedInventory.insert(f.key);
                if(major.contains(f.text(levelField))&&owns(f.text(levelField),f.text(typeField))){QJsonObject detail;detail["blocked_rule"]="missing";detail["reference"]=identity(f);
                    ctx.review(QStringLiteral("河流必表对象未完成"),inventoryProblems.value(f.key)+QStringLiteral("；仅暂缓该对象，其他可靠必表对象继续核对。"),&f,detail);}}
        }catch(const std::exception& e){inventoryComplete=false;ctx.review(QStringLiteral("河流属性参考异常"),QString::fromUtf8(e.what()));}}
    if(!inventoryComplete&&(owner<0||owner>=3))ctx.review(QStringLiteral("河流必表清单"),QStringLiteral("HYDL_HL清单不完整，必表缺失核对未完成"));
    std::vector<River> rivers;
    for(const auto& target:ctx.targets){
        if(!target.problem.isEmpty()||!target.geom||!target.geom->IsValid()||length(target.geom.get())<=0){ctx.review(QStringLiteral("河流成果几何"),QStringLiteral("成果没有有效线几何，长度无法判定"),&target);continue;}
        River river;river.shown=target;river.shown.geom=geometry(target.geom->Intersection(frame.get()));
        if(!river.shown.geom){ctx.review(QStringLiteral("河流长度计算"),QStringLiteral("成果与检查范围求交失败"),&target);continue;}if(length(river.shown.geom.get())<=epsilon)continue;
        Feature attributes=river.shown;
        bool missingLevel=missing(target,levelField);
        bool missingType=missing(target,typeField),missingName=missing(target,nameField);
        if(missingLevel||missingType||missingName){auto candidates=sources;if(!missingLevel&&validAttribute(target.text(levelField)))for(auto& candidate:candidates)if(candidate.text(levelField)!=target.text(levelField))candidate.geom.reset();
            int index=match(attributeQuery(river.shown),candidates,radius,true);
            if(index>=0){river.source=candidates[index];for(const auto& field:{levelField,typeField,nameField}){
                    bool missing=field==levelField?missingLevel:field==typeField?missingType:missingName;
                    if(missing&&validAttribute(candidates[index].text(field)))attributes.attrs[field.toUpper()]=candidates[index].text(field);}}
            else if((missingLevel||missingType)&&reviewOwnedByTask(owner,attributes.text(levelField),attributes.text(typeField),dry))ctx.review(QStringLiteral("河流属性补全"),QStringLiteral("缺失业务属性且参考无法唯一匹配，相关规则无法判定"),&target);
        }
        river.level=attributes.text(levelField);river.type=attributes.text(typeField);river.partition=gradePartition(river.level);
        river.shown.attrs=attributes.attrs;
        river.effective=river.shown.geom;river.mm=ctx.mapMm(length(river.effective.get()));
        const bool active=owns(river.level,river.type);
        if(!target.attributeConflicts.contains(levelField.toUpper())&&!major.contains(river.level)&&!selected.contains(river.level)&&reviewOwnedByTask(owner,river.level,river.type,dry))ctx.review(QStringLiteral("河流等级未知"),QStringLiteral("成果及属性补全未确认有效等级，长度资格无法判定"),&target);
        if(!target.attributeConflicts.contains(typeField.toUpper())&&!reliableType(river.type)&&active&&reviewOwnedByTask(owner,river.level,river.type,dry))ctx.review(QStringLiteral("河流类型未知"),QStringLiteral("缺少有效TYPE，干涸河附加规则无法判定"),&target);
        if(active&&selected.contains(river.level)&&!comparison(river.mm,lengthOp,minimum)){
            auto d=numericDetail(river.mm,lengthOp,minimum,"mm",QStringLiteral("最小选取长度"),QStringLiteral("不允许表示"));d["length_mm"]=river.mm;d["minimum_mm"]=minimum;d["level"]=river.level;d["measurement_scope"]=QStringLiteral("检查范围内成果几何");
            if(!river.source.key.isEmpty())d["reference"]=identity(river.source);
            ctx.error(QStringLiteral("短河流冗余"),numericMessage(QStringLiteral("检查范围内图上长度"),d),river.shown,d);
        }
        rivers.push_back(std::move(river));
    }
    Geo main,outer,boundaryBand;bool needsDryRange=false;
    for(const auto& river:rivers)if(river.type==dry&&owns(river.level,river.type))needsDryRange=true;
    for(const auto& source:sources)if((source.text(typeField)==dry||!reliableType(source.text(typeField)))&&major.contains(source.text(levelField))&&owns(source.text(levelField),source.text(typeField)))needsDryRange=true;
    if(needsDryRange){main=safeArea(ctx,ctx.cfg.text(QStringLiteral("主区范围图层"),QStringLiteral("行政区划面普色")),QStringLiteral("主区外干涸河范围"));
        auto background=safeArea(ctx,ctx.cfg.text(QStringLiteral("外区范围图层"),QStringLiteral("外区普色")),QStringLiteral("主区外干涸河范围"));
        if(main&&background){outer=geometry(background->Difference(main.get()));auto boundary=geometry(main->Boundary());if(boundary)boundaryBand=geometry(boundary->Buffer(ctx.fromMm(ctx.cfg.number(QStringLiteral("主区边界容差"),.2))));if(frame&&outer)outer=geometry(outer->Intersection(frame.get()));}}
    // Match the complete mandatory inventory against clipped actual objects; reference length is never selection evidence.
    if(inventoryComplete&&(owner<0||owner>=3)){
        QMap<QString,int> wridCandidates;for(const auto& reference:sources)if(reliableResultId(reference.text("WRID")))++wridCandidates[reference.text("WRID")];
        bool actualComplete=true;for(const auto& target:ctx.targets)if(!target.problem.isEmpty()||!target.geom||!target.geom->IsValid()||length(target.geom.get())<=0)actualComplete=false;
        for(const auto& source:sources)if(!blockedInventory.contains(source.key)&&inventoryKeys.contains(source.key)&&major.contains(source.text(levelField))&&owns(source.text(levelField),source.text(typeField))&&source.geom&&frame->Intersects(source.geom.get())){
            Geo expected=geometry(source.geom->Intersection(frame.get()));if(!expected||length(expected.get())<=epsilon)continue;
            if(!reliableType(source.text(typeField))){auto outsideMain=main?geometry(expected->Difference(main.get())):Geo();if(!outsideMain||length(outsideMain.get())>epsilon){ctx.review(QStringLiteral("河流必表资格未知"),QStringLiteral("TYPE未知且对象可能处于外区，无法排除干涸河禁表例外。"),&source);continue;}}
            if(source.text(typeField)==dry){if(!main||!outer)continue;expected=geometry(expected->Difference(outer.get()));if(!expected||length(expected.get())<=epsilon)continue;}
            bool represented=false,ambiguous=false;
            for(const auto& actual:rivers){bool associated=false;const QString actualElem=actual.shown.text("ELEMID"),sourceElem=source.text("ELEMID");
                const bool comparableElem=reliableResultId(actualElem)&&(reliableResultId(sourceElem)||!source.memberElemIds.isEmpty());
                if(comparableElem)associated=actualElem==sourceElem||source.memberElemIds.contains(actualElem);
                const bool sharedWrid=reliableResultId(source.text("WRID"))&&source.text("WRID")==actual.shown.text("WRID");
                if(!comparableElem&&sharedWrid&&wridCandidates.value(source.text("WRID"))==1)associated=true;
                int index=match(attributeQuery(actual.shown),sources,radius,true);if(index>=0&&sources[index].key==source.key)associated=true;represented=represented||associated;
                if(associated)for(const auto& field:{levelField,typeField,nameField})if(resultAttributePresent(actual.shown.text(field))&&resultAttributePresent(source.text(field))&&actual.shown.text(field)!=source.text(field)){
                    QJsonObject conflict;conflict["dedup_key"]=QString(actual.shown.key+"|"+source.key+"|"+field);conflict["field"]=field;conflict["actual_value"]=actual.shown.text(field);conflict["reference_value"]=source.text(field);
                    ctx.review(QStringLiteral("河流成果参考属性冲突"),QStringLiteral("关联对象的成果与参考属性冲突，保留成果有效值并标记相关清单判定未完成。"),&actual.shown,conflict);}
                if(index<0&&associated)ctx.review(QStringLiteral("河流身份几何冲突"),QStringLiteral("稳定业务标识已有成果表示，但几何未可靠对应；不转配其他清单对象。"),&actual.shown);
                if(!comparableElem&&sharedWrid&&wridCandidates.value(source.text("WRID"))>1&&index<0)ambiguous=true;
                if(index==-2&&(!comparableElem||actualElem==sourceElem||source.memberElemIds.contains(actualElem))&&actual.shown.geom->Distance(expected.get())<=radius)ambiguous=true;}
            if(ambiguous)ctx.review(QStringLiteral("河流必表匹配歧义"),QStringLiteral("存在成果候选，不能确认必表缺失"),&source);
            else if(!represented&&actualComplete){auto detail=forbiddenDetail(QStringLiteral("必表清单筛选条件"));detail["level"]=source.text(levelField);ctx.absent("missing",QStringLiteral("必表河流缺失"),QStringLiteral("一至五级河流位于本图应表范围，完整清单未找到可靠成果表示。"),source,detail);}
        }
    }
    int count=0,dryCount=0,supplementary=0;
    for(const auto& river:rivers)if(owns(river.level,river.type)){++count;if(river.type==dry){++dryCount;if(owner==2&&river.partition<0)++supplementary;}
        if(river.type==dry&&outer&&!ctx.cfg.flag(QStringLiteral("主区外干涸河是否允许表示"),false)){
            auto outside=geometry(river.shown.geom->Intersection(outer.get()));
            if(outside&&boundaryBand){auto uncertain=geometry(outside->Intersection(boundaryBand.get()));if(uncertain&&length(uncertain.get())>epsilon)ctx.review(QStringLiteral("干涸河主区边界复核"),QStringLiteral("主区边界容差带内区段需复核"),&river.shown);outside=geometry(outside->Difference(boundaryBand.get()));}
            if(outside&&length(outside.get())>epsilon){auto d=forbiddenDetail(QStringLiteral("主区外干涸河是否允许表示"));d["outside_length_mm"]=ctx.mapMm(length(outside.get()));ctx.error(QStringLiteral("主区外干涸河冗余"),QStringLiteral("外区干涸河：XML规定不允许表示，成果已表示。"),river.shown,d,outside);}}}
    ctx.info["total_count"]=count;ctx.info["dry_river_count"]=dryCount;ctx.info["supplementary_dry_river_count"]=supplementary;
    for(size_t i=0;i<rivers.size();++i)for(size_t j=i+1;j<rivers.size();++j){const auto& a=rivers[i];const auto& b=rivers[j];
        if(!selected.contains(a.level)||!selected.contains(b.level)||!comparison(a.mm,lengthOp,minimum)||!comparison(b.mm,lengthOp,minimum))continue;
        if(owner==0){if(a.type!=dry&&b.type!=dry)continue;}else if(owner>0&&owner!=std::min(a.partition,b.partition))continue;
        const double distance=a.shown.geom->Distance(b.shown.geom.get());if(distance<0){ctx.review(QStringLiteral("河流间距计算"),QStringLiteral("成果间距计算失败"),&a.shown);continue;}
        if(confluence(a.shown.geom,b.shown.geom,epsilon))continue;
        if(ctx.mapMm(distance)>ctx.cfg.number(QStringLiteral("选取参考最大间距"),10)){
            auto detail=numericDetail(ctx.mapMm(distance),"<=",ctx.cfg.number(QStringLiteral("选取参考最大间距"),10),"mm",QStringLiteral("选取参考最大间距"),QStringLiteral("选取覆盖待核验"));detail["dedup_key"]=riverPairKey("coverage:",a.shown,b.shown);
            ctx.review(QStringLiteral("河流选取覆盖"),QStringLiteral("成果河流间距大于参考上限，缺少完整应选依据，不能确认缺失或冗余。"),&a.shown,detail);continue;}
        if(comparison(ctx.mapMm(distance),spacingOp,spacing))continue;
        auto d=numericDetail(ctx.mapMm(distance),spacingOp,spacing,"mm",QStringLiteral("选取参考最小间距"),QStringLiteral("选取过密"));d["spacing_mm"]=ctx.mapMm(distance);d["threshold_mm"]=spacing;d["conflict_pair"]=true;
        d["dedup_key"]=riverPairKey(QStringLiteral("河流选取过密:"),a.shown,b.shown);d["conflict_members"]=QJsonArray{identity(a.shown),identity(b.shown)};
        Feature pair=a.shown;for(const auto& fid:b.shown.fids)pair.fids.append(fid);
        ctx.error(QStringLiteral("河流选取过密"),numericMessage(QStringLiteral("两条独立河流间距"),d),pair,d,unite({a.shown.geom,b.shown.geom}));
    }
    std::vector<Feature> shown;for(const auto& river:rivers)shown.push_back(river.shown);QSet<QString> named;
    for(const auto& annotation:annotations){if(frame&&annotation.geom&&!frame->Intersects(annotation.geom.get()))continue;
        Feature labelQuery=annotation;for(const auto& field:{"ELEMID","WRID","ID"})labelQuery.attrs.remove(field);
        int index=match(labelQuery,shown,radius,false);if(index<0){annotationComplete=false;if(reviewOwnedByTask(owner,QString(),QString(),dry))ctx.review(QStringLiteral("河流注记匹配"),QStringLiteral("注记无法唯一关联成果河流"),&annotation);continue;}
        const auto& river=rivers[index];if(!owns(river.level,river.type)||!reviewOwnedByTask(owner,river.level,river.type,dry))continue;
        if(river.shown.attributeConflicts.contains(nameField.toUpper())){annotationComplete=false;continue;}
        if(!major.contains(river.level)&&!selected.contains(river.level)){ctx.review(QStringLiteral("河流注记等级未知"),QStringLiteral("关联成果河流的等级未确认，注记资格无法判定"),&annotation);continue;}
        const QString text=labelText(annotation,ctx.cfg);if(text.isEmpty()){ctx.review(QStringLiteral("河流注记文字不可读"),QStringLiteral("未找到可读文字属性，注记资格无法判定"),&annotation);continue;}
        QString expected=river.shown.text(nameField);
        if(!validAttribute(expected)){int sourceIndex=match(attributeQuery(river.shown),sources,radius,true);
            if(sourceIndex>=0&&validAttribute(sources[sourceIndex].text(nameField)))expected=sources[sourceIndex].text(nameField);
            else {annotationComplete=false;ctx.review(QStringLiteral("河流名称属性补全"),QStringLiteral("注记资格需要河流名称，但成果缺失且参考无法唯一补全"),&annotation);continue;}}
        if(compact(expected)!=compact(text)){annotationComplete=false;ctx.review(QStringLiteral("河流注记名称匹配"),QStringLiteral("可读注记与关联成果河流名称未能一致确认"),&annotation);continue;}
        named.insert(river.shown.key);if(river.level==QStringLiteral("等外")){auto d=forbiddenDetail(QStringLiteral("注记允许的最末等级"));d["annotation_text"]=text;d["river"]=identity(river.shown);ctx.error(QStringLiteral("等外河流名称注记冗余"),QStringLiteral("等外河流名称：XML最末允许六级，成果已表示，不允许表示。"),annotation,d);}}
    for(const auto& river:rivers)if(owns(river.level,river.type)&&river.level==QStringLiteral("六级")&&comparison(river.mm,nameOp,requiredName)&&!named.contains(river.shown.key)){
        if(!annotationComplete){ctx.review(QStringLiteral("河流注记输入缺失"),QStringLiteral("注记层缺失、损坏或无可读文字，名称缺失核对未完成"),&river.shown);continue;}
        auto detail=numericDetail(river.mm,nameOp,requiredName,"mm",QStringLiteral("六级名称必注长度"),QStringLiteral("必须注名"));detail["measurement_scope"]=QStringLiteral("检查范围内成果几何");ctx.absent("annotation_missing",QStringLiteral("六级河流必需名称缺失"),QStringLiteral("成果在检查范围内达到名称必注长度，完整可读注记层确认未表示名称。"),river.shown,detail,river.shown.geom);}
}
}
