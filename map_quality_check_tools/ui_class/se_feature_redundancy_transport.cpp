#include "se_feature_redundancy_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <functional>
#include <queue>

namespace redundancy {
namespace {
QString normalizedCode(const QString& value) {
    const QString text=value.trimmed(); bool ok=false; double n=text.toDouble(&ok);
    if(ok&&std::isfinite(n)&&std::floor(n)==n) return QString::number(n,'f',0);
    return text;
}
bool containsCode(const QStringList& codes,const QString& value) {
    for(const auto& code:codes) if(normalizedCode(code)==value) return true;
    return false;
}
void lines(const OGRGeometry* g,std::vector<const OGRLineString*>& result) {
    if(!g) return;
    auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbLineString) result.push_back(g->toLineString());
    else if(type==wkbMultiLineString||type==wkbGeometryCollection) {
        const auto* collection=g->toGeometryCollection();
        for(int i=0;i<collection->getNumGeometries();++i) lines(collection->getGeometryRef(i),result);
    }
}
double distance(const OGRPoint& a,const OGRPoint& b) { return std::hypot(a.getX()-b.getX(),a.getY()-b.getY()); }
bool endpointConnection(const Geo& a,const Geo& b,double tolerance) {
    if(!a||!b)return false;std::vector<const OGRLineString*> first,second;lines(a.get(),first);lines(b.get(),second);
    for(const auto* part:first)if(part->getNumPoints()>1){OGRPoint start,end;part->StartPoint(&start);part->EndPoint(&end);if(b->Distance(&start)<=tolerance||b->Distance(&end)<=tolerance)return true;}
    for(const auto* part:second)if(part->getNumPoints()>1){OGRPoint start,end;part->StartPoint(&start);part->EndPoint(&end);if(a->Distance(&start)<=tolerance||a->Distance(&end)<=tolerance)return true;}
    return false;
}
// Only exact connected chains are reconstructed. A gap, branch or loop needs review.
Geo chain(const Geo& g,double tolerance) {
    std::vector<const OGRLineString*> parts; lines(g.get(),parts);
    if(parts.empty()||length(g.get())<=0) return Geo();
    for(const auto* part:parts) if(part->getNumPoints()<2) return Geo();
    std::vector<OGRPoint> nodes;
    std::vector<std::pair<int,int>> edges;
    std::vector<std::vector<int>> incident;
    auto node=[&](const OGRPoint& p) {
        for(size_t i=0;i<nodes.size();++i) if(distance(nodes[i],p)<=tolerance) return int(i);
        nodes.push_back(p); incident.emplace_back(); return int(nodes.size()-1);
    };
    for(size_t i=0;i<parts.size();++i) {
        OGRPoint a,b; parts[i]->StartPoint(&a); parts[i]->EndPoint(&b);
        int ai=node(a),bi=node(b); if(ai==bi) return Geo();
        edges.emplace_back(ai,bi); incident[ai].push_back(int(i)); incident[bi].push_back(int(i));
    }
    int start=-1,endCount=0;
    for(size_t i=0;i<incident.size();++i) {
        if(incident[i].size()>2) return Geo();
        if(incident[i].size()==1) { start=int(i); ++endCount; }
    }
    if(endCount!=2) return Geo();
    auto* joined=new OGRLineString;
    std::vector<bool> used(parts.size(),false); int current=start;
    for(size_t count=0;count<parts.size();++count) {
        int next=-1; for(int edge:incident[current]) if(!used[edge]) { next=edge; break; }
        if(next<0) { delete joined; return Geo(); }
        const auto* part=parts[next]; bool forward=edges[next].first==current;
        for(int i=0;i<part->getNumPoints();++i) {
            if(count&&i==0) continue;
            OGRPoint p; part->getPoint(forward?i:part->getNumPoints()-1-i,&p); joined->addPoint(&p);
        }
        used[next]=true; current=forward?edges[next].second:edges[next].first;
    }
    return geometry(joined);
}
double coverage(const Geo& subject,const Geo& covering,double radius) {
    if(!subject||!covering||length(subject.get())<=0) return 0;
    Geo buffer=geometry(covering->Buffer(radius)); if(!buffer) return 0;
    Geo intersection=geometry(subject->Intersection(buffer.get()));
    return intersection?length(intersection.get())/length(subject.get()):0;
}
bool sourceInputsComplete(Context& ctx,const std::vector<Feature>& source,const QString& rule) {
    bool complete=true;
    for(const auto& f:source) if(!f.problem.isEmpty()||!f.geom||f.geom->IsEmpty()) {
        complete=false;
        ctx.review(rule,QStringLiteral("参考源存在几何缺失或异常，不能给出完整输入判定：%1")
            .arg(f.problem.isEmpty()?QStringLiteral("几何为空"):f.problem),&f);
    }
    return complete;
}
void stations(Context& ctx) {
    const QString gradeField=ctx.cfg.text(QStringLiteral("车站等级字段"),"CZFJ"),townField=ctx.cfg.text(QStringLiteral("城镇类别字段"),"TYPE");
    const auto ordinary=ctx.cfg.list(QStringLiteral("普通车站等级值"),"2;3"),highspeed=ctx.cfg.list(QStringLiteral("高铁车站等级值"),"1");
    const auto allowed=ctx.cfg.list(QStringLiteral("允许表示城镇类别"),QStringLiteral("县级城市")),forbidden=ctx.cfg.list(QStringLiteral("禁止表示城镇类别"),QStringLiteral("县级以下城镇"));
    const double radius=ctx.fromMm(ctx.cfg.number(QStringLiteral("候选匹配搜索半径"),2));
    std::vector<Feature> source;bool complete=true;
    try {source=ctx.gdb(ctx.cfg.text(QStringLiteral("参考GDB图层"),"LRRP"),ctx.cfg.text(QStringLiteral("属性候选筛选条件"),ctx.cfg.text(QStringLiteral("GDB筛选条件"),"CLASID = '410301'")),false);}
    catch(const std::exception& e){complete=false;ctx.review(QStringLiteral("车站参考源完整性"),QString::fromUtf8(e.what()));}
    if(source.empty()){complete=false;ctx.review(QStringLiteral("车站必表清单"),QStringLiteral("缺少完整LRRP必表清单，缺失核对未完成"));}
    complete=sourceInputsComplete(ctx,source,QStringLiteral("车站参考源完整性"))&&complete;
    std::vector<bool> represented(source.size(),false),uncertain(source.size(),false);
    for(const auto& rendered:ctx.targets){
        Feature attributes=rendered;int index=match(rendered,source,radius);
        bool stableAssociation=false;for(size_t i=0;i<source.size();++i)for(const auto& field:{"ELEMID","ID"})if(reliableResultId(rendered.text(field))&&rendered.text(field)==source[i].text(field)){represented[i]=true;stableAssociation=true;}
        if(stableAssociation&&index<0)ctx.review(QStringLiteral("车站身份几何冲突"),QStringLiteral("稳定业务标识已关联参考对象，但成果几何无法可靠对应；不转配其他对象。"),&rendered);
        // Stable matching blocks a duplicate missing finding even when the class conflicts.
        if(index>=0)represented[index]=true;
        if(index==-2){for(size_t i=0;i<source.size();++i)if(source[i].geom&&rendered.geom&&source[i].geom->Distance(rendered.geom.get())<=radius)uncertain[i]=true;ctx.review(QStringLiteral("车站匹配"),QStringLiteral("成果与参考存在歧义候选"),&rendered);}
        if(!rendered.problem.isEmpty()||!rendered.geom||!rendered.geom->IsValid()){complete=false;ctx.review(QStringLiteral("车站成果几何"),QStringLiteral("成果输入不完整或几何无效，不能确认未表示"),&rendered);continue;}
        for(const auto& field:{gradeField,townField})if(!attributes.attributeConflicts.contains(field.toUpper())&&!resultAttributePresent(attributes.text(field))&&index>=0)attributes.attrs[field.toUpper()]=source[index].text(field);
        const QString grade=normalizedCode(attributes.text(gradeField)),town=normalizedCode(attributes.text(townField));
        if(containsCode(ordinary,grade)&&containsCode(forbidden,town)){
            auto detail=forbiddenDetail(QStringLiteral("禁止表示城镇类别"));detail["CZFJ"]=grade;detail["TYPE"]=attributes.text(townField);
            if(index>=0)detail["reference"]=identity(source[index]);
            ctx.error(QStringLiteral("普通车站城镇级别"),QStringLiteral("普通车站CZFJ=%1，TYPE=%2；XML禁止表示，成果已表示。").arg(grade,attributes.text(townField)),rendered,detail);
        }else if(!containsCode(highspeed,grade)&&!(containsCode(ordinary,grade)&&containsCode(allowed,town)))ctx.review(QStringLiteral("车站类别"),QStringLiteral("车站等级或必要城镇类别未知，表示资格无法确认"),&rendered);
        if(index>=0)for(const auto& field:{gradeField,townField})if(resultAttributePresent(rendered.text(field))&&resultAttributePresent(source[index].text(field))&&normalizedCode(rendered.text(field))!=normalizedCode(source[index].text(field)))ctx.review(QStringLiteral("车站属性冲突"),QStringLiteral("成果与参考属性冲突，保留成果有效属性"),&rendered);
        QString symbol=rendered.text(ctx.cfg.text(QStringLiteral("成果符号核对字段"),"symbol_nam"));
        bool symbolHigh=ctx.cfg.list(QStringLiteral("高铁车站符号值"),QStringLiteral("高铁火车站")).contains(symbol),symbolOrdinary=ctx.cfg.list(QStringLiteral("普通车站符号值"),QStringLiteral("普通火车站")).contains(symbol);
        if((containsCode(highspeed,grade)&&symbolOrdinary)||(containsCode(ordinary,grade)&&symbolHigh))ctx.review(QStringLiteral("车站符号类别"),QStringLiteral("成果符号名称与车站等级冲突"),&rendered);
    }
    for(size_t i=0;i<source.size();++i)if(!represented[i]&&!containsCode(highspeed,normalizedCode(source[i].text(gradeField)))&&!containsCode(ordinary,normalizedCode(source[i].text(gradeField))))ctx.review(QStringLiteral("车站清单等级未知"),QStringLiteral("LRRP清单存在未知或缺失CZFJ，相关必表分类核对未完成。"),&source[i]);
    if(!complete)return;
    bool pending=false;for(size_t i=0;i<source.size();++i)if(!represented[i]&&containsCode(highspeed,normalizedCode(source[i].text(gradeField))))pending=true;
    if(!pending)return;
    Geo frame;try{frame=ctx.area(ctx.cfg.text(QStringLiteral("检查范围图层"),QStringLiteral("内图廓")),false);}catch(const std::exception& e){ctx.review(QStringLiteral("车站完整性范围"),QString::fromUtf8(e.what()));return;}
    if(!frame){ctx.review(QStringLiteral("车站完整性范围"),QStringLiteral("缺少可靠内图廓，必表缺失核对未完成"));return;}
    for(size_t i=0;i<source.size();++i)if(!represented[i]&&!uncertain[i]&&containsCode(highspeed,normalizedCode(source[i].text(gradeField)))&&source[i].geom&&frame->Intersects(source[i].geom.get()))
        ctx.absent("missing",QStringLiteral("车站应表未表"),QStringLiteral("CZFJ=1高铁车站位于内图廓，完整清单未找到对应成果车站。"),source[i]);
}
std::pair<double,double> interval(const Geo& skeleton,const OGRLineString* route) {
    double low=std::numeric_limits<double>::max(),high=-1;
    std::vector<const OGRLineString*> parts; lines(skeleton.get(),parts);
    for(const auto* line:parts) for(int i=0;i<line->getNumPoints();++i) {
        OGRPoint point; line->getPoint(i,&point); double d=route->Project(&point);
        if(d<0) return {-1,-1}; low=std::min(low,d); high=std::max(high,d);
    }
    return high<0?std::make_pair(-1.,-1.):std::make_pair(low,high);
}
// A unique route uses only graph bridges. Dead-end branches do not make that route ambiguous;
// cycles on the route do. Anchor points are projected onto actual road edges before splitting.
Geo networkPath(const Geo& network,const OGRPoint& first,const OGRPoint& second,double tolerance,double radius) {
    std::vector<const OGRLineString*> parts;lines(network.get(),parts);if(parts.empty())return {};
    std::vector<OGRPoint> junctions;for(const auto* part:parts)if(part->getNumPoints()>1){OGRPoint a,b;part->StartPoint(&a);part->EndPoint(&b);junctions.push_back(a);junctions.push_back(b);}
    OGRPoint anchors[2]={first,second};
    int anchorEdge[2]={-1,-1};double measures[2]={0,0};
    for(int a=0;a<2;++a){double nearest=std::numeric_limits<double>::infinity();for(size_t i=0;i<parts.size();++i){double m=parts[i]->Project(&anchors[a]);if(m<0)continue;OGRPoint p;parts[i]->Value(m,&p);double d=distance(p,anchors[a]);if(d<nearest){nearest=d;anchorEdge[a]=int(i);measures[a]=m;}}if(anchorEdge[a]<0||nearest>radius)return {};}
    struct Edge{int a,b;Geo geom;};std::vector<Edge> edges;std::vector<OGRPoint> nodes;std::vector<std::vector<int>> incident;int anchorNode[2]={-1,-1};
    auto node=[&](const OGRPoint& p){for(size_t i=0;i<nodes.size();++i)if(distance(nodes[i],p)<=tolerance)return int(i);nodes.push_back(p);incident.emplace_back();return int(nodes.size()-1);};
    for(size_t i=0;i<parts.size();++i){std::vector<double> cuts{0,parts[i]->get_Length()};for(int a=0;a<2;++a)if(anchorEdge[a]==int(i))cuts.push_back(measures[a]);
        // A source endpoint on a through line is a real T junction. Interior/interior
        // intersections are not inserted as nodes without explicit junction evidence.
        for(const auto& junction:junctions)if(parts[i]->Distance(&junction)<=tolerance){double m=parts[i]->Project(&junction);if(m>=0)cuts.push_back(m);}
        std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end(),[&](double a,double b){return std::abs(a-b)<=tolerance;}),cuts.end());
        for(const double cut:cuts){OGRPoint p;parts[i]->Value(cut,&p);int n=node(p);for(int a=0;a<2;++a)if(anchorEdge[a]==int(i)&&std::abs(cut-measures[a])<=tolerance)anchorNode[a]=n;}
        for(size_t j=1;j<cuts.size();++j){auto segment=geometry(parts[i]->getSubLine(cuts[j-1],cuts[j],FALSE));if(!segment||length(segment.get())<=tolerance)continue;OGRPoint a,b;segment->toLineString()->StartPoint(&a);segment->toLineString()->EndPoint(&b);int ai=node(a),bi=node(b),id=int(edges.size());edges.push_back({ai,bi,segment});incident[ai].push_back(id);incident[bi].push_back(id);}}
    if(anchorNode[0]<0||anchorNode[1]<0||anchorNode[0]==anchorNode[1])return {};
    std::vector<int> seen(nodes.size(),-1),low(nodes.size());std::vector<bool> bridges(edges.size(),false);int timer=0;
    std::function<void(int,int)> visit=[&](int n,int parent){seen[n]=low[n]=timer++;for(int id:incident[n])if(id!=parent){int other=edges[id].a==n?edges[id].b:edges[id].a;if(seen[other]<0){visit(other,id);low[n]=std::min(low[n],low[other]);if(low[other]>seen[n])bridges[id]=true;}else low[n]=std::min(low[n],seen[other]);}};
    visit(anchorNode[0],-1);std::vector<int> previous(nodes.size(),-1),via(nodes.size(),-1);std::queue<int> queue;queue.push(anchorNode[0]);previous[anchorNode[0]]=anchorNode[0];
    while(!queue.empty()){int n=queue.front();queue.pop();for(int id:incident[n])if(bridges[id]){int other=edges[id].a==n?edges[id].b:edges[id].a;if(previous[other]<0){previous[other]=n;via[other]=id;queue.push(other);}}}
    if(previous[anchorNode[1]]<0)return {};std::vector<int> routeNodes;for(int n=anchorNode[1];n!=anchorNode[0];n=previous[n])routeNodes.push_back(n);std::reverse(routeNodes.begin(),routeNodes.end());
    auto* route=new OGRLineString;int current=anchorNode[0];for(int n:routeNodes){const auto& edge=edges[via[n]];const auto* line=edge.geom->toLineString();bool forward=edge.a==current;for(int i=0;i<line->getNumPoints();++i){if(route->getNumPoints()&&i==0)continue;OGRPoint p;line->getPoint(forward?i:line->getNumPoints()-1-i,&p);route->addPoint(&p);}current=n;}
    return geometry(route);
}
Geo uniqueRoute(const Geo& network,const Geo& first,const Geo& second,double tolerance,double radius) {
    // Physical endpoints and full geometry coverage constrain the route. A centroid can lie
    // on an unrelated branch inside a curved tunnel and is never a route anchor.
    auto firstChain=chain(first,tolerance),secondChain=chain(second,tolerance);if(!firstChain||!secondChain)return {};
    OGRPoint firstEnds[2],secondEnds[2];firstChain->toLineString()->StartPoint(&firstEnds[0]);firstChain->toLineString()->EndPoint(&firstEnds[1]);secondChain->toLineString()->StartPoint(&secondEnds[0]);secondChain->toLineString()->EndPoint(&secondEnds[1]);
    Geo accepted,acceptedGap;
    for(const auto& a:firstEnds)for(const auto& b:secondEnds){auto candidate=networkPath(network,a,b,tolerance,radius);if(!candidate)continue;
        if(coverage(first,candidate,radius)<.98||coverage(second,candidate,radius)<.98)continue;
        auto left=interval(first,candidate->toLineString()),right=interval(second,candidate->toLineString());if(left.first>right.first)std::swap(left,right);
        auto gap=right.first-left.second>tolerance?geometry(candidate->toLineString()->getSubLine(left.second,right.first,FALSE)):Geo();
        if(accepted&&acceptedGap&&gap&&!acceptedGap->Equals(gap.get()))return {};
        if(!accepted){accepted=candidate;acceptedGap=gap;}
    }
    return accepted;
}
void tunnels(Context& ctx) {
    const QString order=ctx.cfg.text(QStringLiteral("长度判定顺序"));
    if(!order.isEmpty()&&order!=QStringLiteral("先判断每个完整隧道自身长度，再检查合格隧道之间的连接")&&order!=QStringLiteral("先判断每条完整隧道自身长度，再检查达标隧道之间的连接")){
        throw std::runtime_error(QStringLiteral("不支持的XML长度判定顺序：%1").arg(order).toUtf8().constData());
    }
    const double minimum=ctx.cfg.number(QStringLiteral("最小表示长度"),5);
    const QString op=ctx.cfg.text(QStringLiteral("最小长度比较符"),ctx.cfg.text(QStringLiteral("最小表示长度比较符"),">="));comparison(0,op,minimum);
    const double maximum=ctx.cfg.number(QStringLiteral("连接表示最大间隔"),1000);
    const QString gapOp=ctx.cfg.text(QStringLiteral("连接间隔比较符"),ctx.cfg.text(QStringLiteral("连接表示最大间隔比较符"),"<="));comparison(0,gapOp,maximum);
    const double epsilon=ctx.units(1e-6),radius=ctx.fromMm(ctx.cfg.number(QStringLiteral("候选匹配搜索半径"),2));
    if(!ctx.cfg.text(QStringLiteral("检查范围图层")).isEmpty()){
        try{auto frame=ctx.area(ctx.cfg.text(QStringLiteral("检查范围图层")));std::vector<Feature> clipped;
            for(auto target:ctx.targets){if(target.geom){auto part=geometry(target.geom->Intersection(frame.get()));if(!part){ctx.review(QStringLiteral("隧道范围剪裁"),QStringLiteral("成果与检查范围求交失败"),&target);continue;}if(part->IsEmpty())continue;target.geom=part;}clipped.push_back(std::move(target));}ctx.targets=std::move(clipped);
        }catch(const std::exception& e){ctx.review(QStringLiteral("隧道检查范围"),QString::fromUtf8(e.what()));return;}
    }
    const QString kindField=ctx.cfg.text(QStringLiteral("连接段类型字段"),"TYPE");
    const QString connectorValue=ctx.cfg.text(QStringLiteral("连接段类型值"),QStringLiteral("连接段"));
    std::vector<Feature> eligible,connectors,unresolved;
    for(const auto& target:ctx.targets){
        if(!target.problem.isEmpty()||!target.geom||length(target.geom.get())<=0||!target.geom->IsValid()){
            unresolved.push_back(target);ctx.review(QStringLiteral("隧道成果几何"),QStringLiteral("成果没有有效线几何，长度无法判定"),&target);continue;}
        bool conflictingRole=target.attributeConflicts.contains(kindField.toUpper());for(const auto& field:{"ROLE","OBJ_ROLE","PART_ROLE","OBJECTROLE"})if(target.attributeConflicts.contains(field))conflictingRole=true;
        if(conflictingRole){unresolved.push_back(target);continue;}
        QString role=target.text(kindField);if(!resultAttributePresent(role))for(const auto& field:{"ROLE","OBJ_ROLE","PART_ROLE","OBJECTROLE"})if(resultAttributePresent(target.text(field))){role=target.text(field);break;}
        if(role==connectorValue||role.compare("connector",Qt::CaseInsensitive)==0||role.compare("connection",Qt::CaseInsensitive)==0){connectors.push_back(target);continue;}
        const double mm=ctx.mapMm(length(target.geom.get()));
        if(!comparison(mm,op,minimum)){
            auto detail=numericDetail(mm,op,minimum,"mm",QStringLiteral("最小表示长度"),QStringLiteral("不允许表示"));detail["length_mm"]=mm;detail["minimum_mm"]=minimum;detail["measurement_scope"]=ctx.cfg.text(QStringLiteral("检查范围图层")).isEmpty()?QStringLiteral("完整成果几何"):QStringLiteral("检查范围内成果几何");
            ctx.error(QStringLiteral("隧道最小长度"),numericMessage(ctx.cfg.text(QStringLiteral("检查范围图层")).isEmpty()?QStringLiteral("图上长度"):QStringLiteral("检查范围内图上长度"),detail),target,detail);
        }else {Feature physical=target;bool hasIdentity=reliableResultId(target.text(ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id")))||reliableResultId(target.text("ELEMID"));
            if(!hasIdentity&&!resultAttributePresent(role)){physical.attrs["ROLE_UNCERTAIN"]="1";ctx.review(QStringLiteral("隧道连接分段未完成"),QStringLiteral("缺少可靠成果对象标识或物理段元数据，不能由合并连续线反推隧道边界与连接。"),&target);}eligible.push_back(physical);}
    }
    if(eligible.size()<2&&connectors.empty())return;
    const bool rail=ctx.cfg.text(QStringLiteral("检查图层"),ctx.paths.standardLayer).contains(QStringLiteral("铁路"));
    std::vector<Feature> roads;
    try{QSet<QString> loaded;for(const auto& layer:ctx.cfg.list(QStringLiteral("承载道路成果图层"),rail?QStringLiteral("铁路"):QStringLiteral("公路")))for(auto& row:ctx.actual(layer,false))if(!loaded.contains(row.key)){loaded.insert(row.key);roads.push_back(std::move(row));}}
    catch(const std::exception& e){ctx.review(QStringLiteral("隧道成果承载道路"),QString::fromUtf8(e.what()));return;}
    if(roads.empty()){
        ctx.review(QStringLiteral("隧道连接关系"),QStringLiteral("缺少可辨认的成果承载道路或连接段，连接规则无法判定"));return;}
    struct Along{int tunnel;double low,high;};
    // Connect source road rows by actual endpoints; disconnected routes remain separate.
    std::vector<Feature> joinedRoads;std::vector<Geo> networks;std::vector<bool> consumed(roads.size(),false);
    for(size_t i=0;i<roads.size();++i)if(!consumed[i]){Feature road=roads[i];std::vector<Geo> parts{road.geom};consumed[i]=true;bool changed=true;
        while(changed){changed=false;for(size_t j=0;j<roads.size();++j)if(!consumed[j]&&roads[j].geom){bool touching=false;for(const auto& part:parts)if(part&&part->Distance(roads[j].geom.get())<=epsilon){if(endpointConnection(part,roads[j].geom,epsilon))touching=true;
                else {QStringList keys{roads[i].key,roads[j].key};keys.sort();QJsonObject detail;detail["dedup_key"]=QString("road-crossing:"+keys.join("|"));ctx.review(QStringLiteral("成果道路交叉未确认"),QStringLiteral("道路仅在线内部交叉，缺少路口或高程信息，不能默认为可通行连接。"),&roads[j],detail);}}
            if(touching){consumed[j]=true;parts.push_back(roads[j].geom);changed=true;}}}
        auto* network=new OGRMultiLineString;for(const auto& part:parts){std::vector<const OGRLineString*> raw;lines(part.get(),raw);for(const auto* line:raw)network->addGeometry(line);}networks.push_back(geometry(network));
        road.geom=unite(parts);joinedRoads.push_back(road);}
    roads=std::move(joinedRoads);
    std::vector<Geo> routes;for(const auto& network:networks)routes.push_back(chain(network,epsilon));
    std::vector<std::vector<Along>> groups(roads.size());std::vector<std::vector<int>> branched(roads.size());
    for(size_t i=0;i<eligible.size();++i){Feature query=eligible[i];for(const auto& field:{"ELEMID","WRID","ID"})query.attrs.remove(field);int road=match(query,roads,radius,true);
        if(road<0){ctx.review(QStringLiteral("隧道连接关系"),QStringLiteral("成果隧道无法唯一关联成果路网"),&eligible[i]);continue;}
        if(!routes[road]){branched[road].push_back(int(i));continue;}
        auto range=interval(eligible[i].geom,routes[road]->toLineString());if(range.first>=0)groups[road].push_back({int(i),range.first,range.second});}
    struct Relation{int road,a,b;Geo section;};std::vector<Relation> relations;
    for(size_t road=0;road<groups.size();++road){auto& group=groups[road];std::sort(group.begin(),group.end(),[](const Along& a,const Along& b){return a.low<b.low;});
        for(size_t i=1;i<group.size();++i){const auto& a=group[i-1];const auto& b=group[i];if(b.low-a.high<=epsilon)continue;
            auto section=geometry(routes[road]->toLineString()->getSubLine(a.high,b.low,FALSE));if(section)relations.push_back({int(road),a.tunnel,b.tunnel,section});}
        const auto& candidates=branched[road];
        for(size_t i=0;i<candidates.size();++i)for(size_t j=i+1;j<candidates.size();++j){int ai=candidates[i],bi=candidates[j];auto route=uniqueRoute(networks[road],eligible[ai].geom,eligible[bi].geom,epsilon,radius);
            if(!route){ctx.review(QStringLiteral("隧道路网路径不唯一"),QStringLiteral("实际路网不能确认这对隧道间唯一沿线路径，连接规则未完成。"),&eligible[ai]);continue;}
            auto first=interval(eligible[ai].geom,route->toLineString()),second=interval(eligible[bi].geom,route->toLineString());if(first.first>second.first){std::swap(first,second);std::swap(ai,bi);}if(second.first-first.second<=epsilon)continue;
            bool adjacent=true;for(int other:candidates)if(other!=ai&&other!=bi&&coverage(eligible[other].geom,route,radius)>=.95){auto between=interval(eligible[other].geom,route->toLineString());if(between.first<second.first-epsilon&&between.second>first.second+epsilon)adjacent=false;}
            if(!adjacent)continue;auto section=geometry(route->toLineString()->getSubLine(first.second,second.first,FALSE));if(section)relations.push_back({int(road),ai,bi,section});}
    }
    QSet<QString> resolvedConnectors;
    for(const auto& relation:relations){const int ai=relation.a,bi=relation.b;if(eligible[ai].text("ROLE_UNCERTAIN")=="1"||eligible[bi].text("ROLE_UNCERTAIN")=="1")continue;
        const auto& section=relation.section;const Feature* rendered=nullptr;
        for(const auto& connector:connectors)if(coverage(section,connector.geom,std::max(epsilon,ctx.fromMm(.00001)))>=.98){rendered=&connector;resolvedConnectors.insert(connector.key);break;}
        const double metres=ctx.metres(length(section.get()));
        if(!comparison(metres,gapOp,maximum)&&rendered){auto detail=numericDetail(metres,gapOp,maximum,QStringLiteral("米"),QStringLiteral("连接表示最大间隔"),QStringLiteral("不允许连接表示"));detail["gap_m"]=metres;detail["maximum_m"]=maximum;
            auto actualSection=geometry(rendered->geom->Intersection(section.get()));ctx.error(QStringLiteral("隧道超长连接"),numericMessage(QStringLiteral("连接间隔"),detail),*rendered,detail,actualSection?actualSection:rendered->geom);
        }else if(comparison(metres,gapOp,maximum)&&!rendered){bool blocked=false;for(const auto& candidate:unresolved)if(!candidate.geom||candidate.geom->IsEmpty()||candidate.geom->Distance(section.get())<=radius)blocked=true;
            if(blocked){ctx.review(QStringLiteral("隧道连接候选未完成"),QStringLiteral("连接区间存在未能辨认的成果候选，不能确认连接缺失。"),&eligible[ai]);continue;}
            auto detail=numericDetail(metres,gapOp,maximum,QStringLiteral("米"),QStringLiteral("连接表示最大间隔"),QStringLiteral("必须连接表示"));detail["tunnels"]=QJsonArray{identity(eligible[ai]),identity(eligible[bi])};detail["dedup_key"]=QString(eligible[ai].key+"|"+eligible[bi].key);
            ctx.absent("connection_missing",QStringLiteral("隧道连接应表未表"),QStringLiteral("成果相邻合格隧道沿线间隔<=阈值，确认缺少连接段。"),roads[relation.road],detail,section);}
    }
    for(const auto& connector:connectors)if(!resolvedConnectors.contains(connector.key))ctx.review(QStringLiteral("隧道连接关系"),QStringLiteral("该成果连接段无法确认两端合格隧道"),&connector);
}

}
void checkTransport(Context& ctx) {
    const QString groupField=ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id");
    if(ctx.cfg.text(QStringLiteral("检查类型"))==QStringLiteral("隧道")){
        for(auto& target:ctx.targets){target.attrs.remove("TUN_GROUP");QString id=target.text(groupField).trimmed();
            if(reliableResultId(id))target.attrs["TUN_GROUP"]=id;
        }
        ctx.targets=resultSymbols(ctx,std::move(ctx.targets),"TUN_GROUP",{ctx.cfg.text(QStringLiteral("连接段类型字段"),"TYPE"),"NAME","ROLE","OBJ_ROLE","PART_ROLE","OBJECTROLE"},QStringLiteral("隧道成果属性冲突"));
    }else {for(auto& target:ctx.targets){target.attrs.remove("STATION_GROUP");if(reliableResultId(target.text(groupField)))target.attrs["STATION_GROUP"]=target.text(groupField);}
        ctx.targets=resultSymbols(ctx,std::move(ctx.targets),"STATION_GROUP",{ctx.cfg.text(QStringLiteral("车站等级字段"),"CZFJ"),ctx.cfg.text(QStringLiteral("城镇类别字段"),"TYPE")},QStringLiteral("车站成果属性冲突"));}
    ctx.info["total_count"]=int(ctx.targets.size());
    if(ctx.cfg.text(QStringLiteral("检查类型")).contains(QStringLiteral("火车站"))) stations(ctx);
    else tunnels(ctx);
}
}
