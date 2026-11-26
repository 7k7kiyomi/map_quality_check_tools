#include "se_feature_redundancy_internal.h"
#include <QJsonDocument>
#include <QRegularExpression>
#include <gdal_version.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace redundancy {
namespace {
constexpr double pi = 3.14159265358979323846;
void fail(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }
double positive(Context& ctx, const QString& key, double fallback) {
    const double value=ctx.cfg.number(key,fallback);
    if(value<=0)fail(QStringLiteral("村庄参数必须大于零：%1").arg(key));
    return value;
}
int quota(Context& ctx,const QString& key,int fallback) {
    const double value=positive(ctx,key,fallback);
    if(std::floor(value)!=value||value>100000)fail(QStringLiteral("数量上限必须为有效正整数：%1").arg(key));
    return static_cast<int>(value);
}
void mode(Context& ctx,const QString& key,const QString& supported) {
    if(ctx.cfg.text(key,supported)!=supported)fail(QStringLiteral("不支持的村庄规则模式：%1").arg(key));
}
void units(Context& ctx,const QString& key,const QString& expected) {
    const auto value=ctx.cfg.units.value(key);
    if(!value.isEmpty()&&value!=expected)fail(QStringLiteral("村庄参数单位必须为%1：%2").arg(expected,key));
}
double mapArea(Context& ctx,double areaMm2) {
    const double unit=ctx.fromMm(1),result=areaMm2*unit*unit;
    if(!std::isfinite(result)||result<=0)fail(QStringLiteral("图上面积换算无效或溢出"));
    return result;
}
double area(const OGRGeometry* g) {
    if(!g||g->IsEmpty())return 0;
    const auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbPolygon)return g->toPolygon()->get_Area();
    double result=0;
    if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)) {
        const auto* parts=g->toGeometryCollection();
        for(int i=0;i<parts->getNumGeometries();++i)result+=area(parts->getGeometryRef(i));
    }
    return result;
}
Geo point(double x,double y) { return geometry(new OGRPoint(x,y)); }
Geo operation(OGRGeometry* raw,const QString& name) {
    Geo g=geometry(raw);
    if(!g||!g->IsValid())fail(QStringLiteral("村庄分区几何运算失败：%1").arg(name));
    return g;
}
void polygonParts(const OGRGeometry* g,std::vector<Geo>& parts,double minimum) {
    if(!g||g->IsEmpty())return;
    const auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbPolygon) {
        if(area(g)+minimum*1e-10>=minimum)parts.push_back(copy(g));
    } else if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)) {
        const auto* group=g->toGeometryCollection();
        for(int i=0;i<group->getNumGeometries();++i)polygonParts(group->getGeometryRef(i),parts,minimum);
    }
}
Geo merged(const std::vector<Geo>& faces) {
    OGRMultiPolygon group;
    for(const auto& face:faces) {
        if(!face||face->IsEmpty())continue;
        if(wkbFlatten(face->getGeometryType())==wkbPolygon) {
            if(group.addGeometry(face.get())!=OGRERR_NONE)fail(QStringLiteral("无法组织村庄缓冲面"));
        } else {
            std::vector<Geo> parts;polygonParts(face.get(),parts,0);
            for(const auto& part:parts)if(group.addGeometry(part.get())!=OGRERR_NONE)fail(QStringLiteral("无法组织分区多部件"));
        }
    }
    if(group.getNumGeometries()==0)return {};
    return operation(group.UnionCascaded(),QStringLiteral("缓冲面融合"));
}
Geo buffered(Context& ctx,const std::vector<Feature>& villages,double radiusMm,double minAreaMm2) {
    std::vector<Geo> buffers;
    const double radius=ctx.fromMm(radiusMm),minimum=mapArea(ctx,minAreaMm2);
    for(const auto& village:villages)buffers.push_back(operation(village.geom->Buffer(radius,64),QStringLiteral("定位点缓冲")));
    auto joined=merged(buffers);
    std::vector<Geo> kept;polygonParts(joined.get(),kept,minimum);
    return merged(kept);
}
bool partCenters(const OGRGeometry* g,const Feature& feature,std::vector<OGRPoint>& centers) {
    if(!g||g->IsEmpty())return false;
    const auto type=wkbFlatten(g->getGeometryType());
    if(type==wkbPoint) {centers.push_back(*g->toPoint());return true;}
    if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)) {
        const auto* parts=g->toGeometryCollection();
        for(int i=0;i<parts->getNumGeometries();++i)if(!partCenters(parts->getGeometryRef(i),feature,centers))return false;
        return !centers.empty();
    }
    if(type!=wkbLineString&&type!=wkbPolygon)return false;
    OGREnvelope box;g->getEnvelope(&box);
    const double width=box.MaxX-box.MinX,height=box.MaxY-box.MinY;
    if(width<=0||height<=0||width/height<0.5||width/height>2)return false;
    if(type==wkbLineString) {
        const auto* line=g->toLineString();if(line->getNumPoints()<4)return false;
        OGRPoint first,last;line->StartPoint(&first);line->EndPoint(&last);
        const bool declaredClosed=feature.text("closed").compare("true",Qt::CaseInsensitive)==0||feature.text("closed")=="1";
        if(!declaredClosed&&!line->get_IsClosed()&&first.Distance(&last)>std::max(width,height)*0.2)return false;
    }
    centers.emplace_back(box.MinX+width/2,box.MinY+height/2);
    return true;
}
std::vector<Feature> centers(Context& ctx,std::vector<Feature> grouped,const QString& rule) {
    std::vector<Feature> result;
    const double tolerance=ctx.fromMm(positive(ctx,QStringLiteral("符号中心一致性容差"),0.1));
    for(auto& feature:grouped) {
        std::vector<OGRPoint> parts;
        const bool renderedLines=feature.geom&&wkbFlatten(feature.geom->getGeometryType())!=wkbPoint;
        if(!feature.problem.isEmpty()||(renderedLines&&feature.text(ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id")).isEmpty())
                ||!partCenters(feature.geom.get(),feature,parts)) {
            ctx.review(rule,QStringLiteral("村庄符号缺少可靠分组、有效几何或可恢复的环状定位结构"),&feature);continue;
        }
        OGREnvelope box;feature.geom->getEnvelope(&box);
        OGRPoint center(box.MinX+(box.MaxX-box.MinX)/2,box.MinY+(box.MaxY-box.MinY)/2);
        bool agrees=true;for(const auto& part:parts)if(center.Distance(&part)>tolerance)agrees=false;
        if(!agrees){ctx.review(rule,QStringLiteral("同一符号的各部分中心不一致，不能用跨村庄的外包框中点代替定位点"),&feature);continue;}
        feature.geom=point(center.getX(),center.getY());feature.geom->assignSpatialReference(ctx.srs.get());
        result.push_back(std::move(feature));
    }
    return result;
}
Geo safeArea(Context& ctx,const QString& name) {
    try {auto g=ctx.area(name,false);if(!g)ctx.review(QStringLiteral("村庄范围"),QStringLiteral("缺少或为空的范围面：%1").arg(name));return g;}
    catch(const std::exception& e){ctx.review(QStringLiteral("村庄范围"),QString::fromUtf8(e.what()));return {};}
}
// Prepare each area once for many point queries. Older GDAL SDKs use a ring query
// with cached component bounds instead of rebuilding a GEOS object for every point.
class AreaQuery {
    Geo shape;
    struct Part {const OGRPolygon* polygon;OGREnvelope box;};
    std::vector<Part> parts;
    struct BoundaryPart {const OGRGeometry* geometry;OGREnvelope box;};
    std::vector<BoundaryPart> boundaries;
    double epsilon;
#if GDAL_VERSION_NUM >= 3060000
    OGRPreparedGeometryH prepared=nullptr;
#endif
    void collect(const OGRGeometry* g) {
        if(!g||g->IsEmpty())return;
        const auto type=wkbFlatten(g->getGeometryType());
        if(type==wkbPolygon){Part p;p.polygon=g->toPolygon();g->getEnvelope(&p.box);parts.push_back(p);}
        else if(OGR_GT_IsSubClassOf(type,wkbGeometryCollection)){
            const auto* group=g->toGeometryCollection();for(int i=0;i<group->getNumGeometries();++i)collect(group->getGeometryRef(i));
        }
        else {BoundaryPart p;p.geometry=g;g->getEnvelope(&p.box);boundaries.push_back(p);}
    }
    // 0 outside, 1 inside, 2 on the ring boundary (including a hole boundary).
    int ring(const OGRLinearRing* r,double x,double y)const {
        if(!r||r->getNumPoints()<3)return 0;
        bool inside=false;
        for(int i=0,j=r->getNumPoints()-1;i<r->getNumPoints();j=i++) {
            const double ax=r->getX(j),ay=r->getY(j),bx=r->getX(i),by=r->getY(i);
            const double dx=bx-ax,dy=by-ay,size=std::hypot(dx,dy);
            if(size<=epsilon){if(std::hypot(x-ax,y-ay)<=epsilon)return 2;}
            else {
                const double t=((x-ax)*dx+(y-ay)*dy)/(size*size);
                if(t>=-epsilon/size&&t<=1+epsilon/size&&std::abs((x-ax)*dy-(y-ay)*dx)/size<=epsilon)return 2;
            }
            if((ay>y)!=(by>y))if(x<ax+(y-ay)/(by-ay)*dx)inside=!inside;
        }
        return inside?1:0;
    }
public:
    AreaQuery(Context& ctx,Geo geometry):shape(std::move(geometry)),epsilon(ctx.fromMm(1e-8)) {
        collect(shape.get());
#if GDAL_VERSION_NUM >= 3060000
        if(shape&&!shape->IsEmpty())prepared=OGRCreatePreparedGeometry(OGRGeometry::ToHandle(shape.get()));
#endif
    }
    AreaQuery(const AreaQuery&)=delete;
    AreaQuery& operator=(const AreaQuery&)=delete;
    ~AreaQuery(){
#if GDAL_VERSION_NUM >= 3060000
        if(prepared)OGRDestroyPreparedGeometry(prepared);
#endif
    }
    bool exists()const{return bool(shape);}
    bool covers(const Feature& f)const {
        if(!shape||shape->IsEmpty())return false;
#if GDAL_VERSION_NUM >= 3060000
        if(prepared)return OGRPreparedGeometryIntersects(prepared,OGRGeometry::ToHandle(f.geom.get()))!=0;
#endif
        const auto* p=f.geom->toPoint();const double x=p->getX(),y=p->getY();
        for(const auto& part:parts) {
            const auto& b=part.box;if(x<b.MinX-epsilon||x>b.MaxX+epsilon||y<b.MinY-epsilon||y>b.MaxY+epsilon)continue;
            const int outer=ring(part.polygon->getExteriorRing(),x,y);if(outer==2)return true;if(!outer)continue;
            bool inHole=false;
            for(int i=0;i<part.polygon->getNumInteriorRings();++i){const int v=ring(part.polygon->getInteriorRing(i),x,y);if(v==2)return true;if(v==1){inHole=true;break;}}
            if(!inHole)return true;
        }
        // Intersections may retain a shared edge or a tangent point. They remain
        // part of the region boundary, just as in the prepared-geometry branch.
        for(const auto& part:boundaries){
            const auto& b=part.box;if(x<b.MinX-epsilon||x>b.MaxX+epsilon||y<b.MinY-epsilon||y>b.MaxY+epsilon)continue;
            if(part.geometry->Intersects(f.geom.get()))return true;
        }
        return false;
    }
};
struct Zones {Geo frame,main,outer,dense,general;bool densityKnown=true;};
Zones zones(Context& ctx,const std::vector<Feature>& targetCenters,bool administrative) {
    Zones z;
    z.frame=safeArea(ctx,ctx.cfg.text(QStringLiteral("检查范围图层"),QStringLiteral("内图廓")));
    z.main=safeArea(ctx,ctx.cfg.text(QStringLiteral("主区范围图层"),QStringLiteral("行政区划面普色")));
    auto background=safeArea(ctx,ctx.cfg.text(QStringLiteral("外区范围图层"),QStringLiteral("外区普色")));
    if(z.main&&background)z.outer=operation(background->Difference(z.main.get()),QStringLiteral("外区扣除主区"));
    if(z.frame) {
        if(z.main)z.main=operation(z.main->Intersection(z.frame.get()),QStringLiteral("主区限定图廓"));
        if(z.outer)z.outer=operation(z.outer->Intersection(z.frame.get()),QStringLiteral("外区限定图廓"));
    }
    std::vector<Feature> basis;
    if(administrative) {
        basis=targetCenters;
        if(basis.size()!=ctx.targets.size())z.densityKnown=false;
    } else {
        const int reviews=ctx.reviews.size();
        try {basis=centers(ctx,symbols(ctx.actual(ctx.cfg.text(QStringLiteral("密度基准图层"),QStringLiteral("行政村"))),
                        ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id")),QStringLiteral("行政村密度基准定位"));}
        catch(const std::exception& e){ctx.review(QStringLiteral("行政村密度基准"),QString::fromUtf8(e.what()));}
        z.densityKnown=ctx.reviews.size()==reviews;
    }
    const double denseRadius=positive(ctx,QStringLiteral("密集区缓冲半径"),0.9),generalRadius=positive(ctx,QStringLiteral("一般密集区缓冲半径"),1.7);
    const double denseMin=positive(ctx,QStringLiteral("密集区最小面积"),2),generalMin=positive(ctx,QStringLiteral("一般密集区最小面积"),9);
    z.dense=buffered(ctx,basis,denseRadius,denseMin);
    z.general=buffered(ctx,basis,generalRadius,generalMin);
    if(z.dense&&z.general)z.general=operation(z.general->Difference(z.dense.get()),QStringLiteral("一般密集区扣除密集区"));
    Geo scope=z.frame?z.frame:unite({z.main,z.outer});
    if(scope){if(z.dense)z.dense=operation(z.dense->Intersection(scope.get()),QStringLiteral("密集区限定范围"));if(z.general)z.general=operation(z.general->Intersection(scope.get()),QStringLiteral("一般密集区限定范围"));}
    const double one=mapArea(ctx,1);
    ctx.info["density_basis_count"]=static_cast<int>(basis.size());ctx.info["density_complete"]=z.densityKnown;
    ctx.info["dense_area_mm2"]=area(z.dense.get())/one;ctx.info["general_area_mm2"]=area(z.general.get())/one;
    ctx.info["density_overlap_mm2"]=(z.dense&&z.general)?area(operation(z.dense->Intersection(z.general.get()),QStringLiteral("分区互斥核验")).get())/one:0;
    ctx.info["dense_buffer_radius_mm"]=denseRadius;ctx.info["general_buffer_radius_mm"]=generalRadius;
    if(pi*denseRadius*denseRadius>=denseMin||pi*generalRadius*generalRadius>=generalMin)
        ctx.notice(QStringLiteral("密度参数说明"),QStringLiteral("按XML给定的定位点半径和面积门槛执行；单个行政村的缓冲面可能通过面积筛选，并不要求多个村庄相连"));
    return z;
}
QString density(const Zones& z,const AreaQuery& dense,const AreaQuery& general,const Feature& village) {
    if(dense.covers(village))return QStringLiteral("密集区");
    if(!z.densityKnown)return QStringLiteral("无法判定");
    if(general.covers(village))return QStringLiteral("一般密集区");
    return QStringLiteral("稀疏区");
}
int community(Context& ctx,const Feature& village,const std::vector<Feature>& source,QJsonObject& detail) {
    const int found=match(village,source,ctx.fromMm(positive(ctx,QStringLiteral("候选匹配搜索半径"),2)));
    if(found<0){ctx.review(QStringLiteral("村庄社区类别"),QStringLiteral("密集区候选对象无法唯一匹配GDB，不能确定是否属于禁表社区"),&village);return -1;}
    const auto& ref=source[found];detail["reference_source"]=identity(ref);
    const QString name=ref.text(ctx.cfg.text(QStringLiteral("社区类别字段"),"NAME"));
    if(name.isEmpty()||name=="-"||name.compare("NULL",Qt::CaseInsensitive)==0){ctx.review(QStringLiteral("村庄社区类别"),QStringLiteral("社区识别字段为空或无效"),&village);return -1;}
    for(const auto& suffix:ctx.cfg.list(QStringLiteral("社区名称后缀"),QStringLiteral("社区;社区居委会")))if(name.endsWith(suffix))return 1;
    return 0;
}
struct XY {double x,y;};
XY position(const Feature& f) {const auto* p=f.geom->toPoint();return {p->getX(),p->getY()};}
class PointIndex {
    const std::vector<Feature>& points;
    std::vector<int> order;
public:
    explicit PointIndex(const std::vector<Feature>& p):points(p),order(p.size()) {
        std::iota(order.begin(),order.end(),0);
        std::sort(order.begin(),order.end(),[&](int a,int b){return position(points[a]).x<position(points[b]).x;});
    }
    std::vector<int> near(double x,double y,double radius)const {
        std::vector<int> out;
        auto first=std::lower_bound(order.begin(),order.end(),x-radius,[&](int n,double value){return position(points[n]).x<value;});
        for(auto i=first;i!=order.end();++i) {
            const auto p=position(points[*i]);if(p.x>x+radius)break;
            if(std::hypot(p.x-x,p.y-y)<=radius)out.push_back(*i);
        }
        std::sort(out.begin(),out.end());return out;
    }
};
struct Window {std::vector<int> members;XY center{};int windowCount=1;int maxWindowCount=0;};
class DisjointSet {
    std::vector<int> parent;
    std::vector<unsigned char> rank;
public:
    explicit DisjointSet(size_t count):parent(count),rank(count,0) {std::iota(parent.begin(),parent.end(),0);}
    int find(int value) {if(parent[value]!=value)parent[value]=find(parent[value]);return parent[value];}
    void unite(int first,int second) {
        first=find(first);second=find(second);if(first==second)return;
        if(rank[first]<rank[second])std::swap(first,second);
        parent[second]=first;if(rank[first]==rank[second])++rank[first];
    }
};
void countWindows(Context& ctx,const std::vector<Feature>& villages,const QString& rule,double areaCm2,int limit) {
    if(villages.size()<=static_cast<size_t>(limit))return;
    const double radiusMm=std::sqrt(areaCm2*100/pi),radius=ctx.fromMm(radiusMm),epsilon=ctx.fromMm(1e-7);
    PointIndex index(villages);std::map<std::vector<int>,XY> unique;
    auto candidate=[&](double x,double y) {
        auto members=index.near(x,y,radius+epsilon);
        if(members.size()>static_cast<size_t>(limit))unique.emplace(std::move(members),XY{x,y});
    };
    for(size_t i=0;i<villages.size();++i) {
        const auto a=position(villages[i]);candidate(a.x,a.y);
        for(const int j:index.near(a.x,a.y,2*radius+epsilon)) {
            if(j<=static_cast<int>(i))continue;
            const auto b=position(villages[j]);const double dx=b.x-a.x,dy=b.y-a.y,d=std::hypot(dx,dy);
            if(d<=epsilon||d>2*radius+epsilon)continue;
            const double h=std::sqrt(std::max(0.0,(radius-d/2)*(radius+d/2)));
            const double mx=a.x+dx/2,my=a.y+dy/2;
            candidate(mx-dy/d*h,my+dx/d*h);candidate(mx+dy/d*h,my-dx/d*h);
        }
    }
    std::vector<Window> windows;
    for(const auto& entry:unique)windows.push_back({entry.first,entry.second});
    std::sort(windows.begin(),windows.end(),[](const Window& a,const Window& b){return a.members.size()!=b.members.size()?a.members.size()>b.members.size():a.members<b.members;});
    std::vector<Window> maximal;
    for(const auto& w:windows) {
        bool redundant=false;
        for(const auto& other:maximal)if(std::includes(other.members.begin(),other.members.end(),w.members.begin(),w.members.end())){redundant=true;break;}
        if(!redundant)maximal.push_back(w);
    }
    if(maximal.empty())return;
    DisjointSet clusters(maximal.size());std::map<int,int> firstWindowByMember;
    for(size_t wi=0;wi<maximal.size();++wi)for(const int member:maximal[wi].members) {
        auto inserted=firstWindowByMember.emplace(member,static_cast<int>(wi));
        if(!inserted.second)clusters.unite(static_cast<int>(wi),inserted.first->second);
    }
    struct QuotaGroup {std::set<int> members;int windowCount=0;int maxWindowCount=0;};
    std::map<int,QuotaGroup> groups;
    for(size_t wi=0;wi<maximal.size();++wi) {
        auto& group=groups[clusters.find(static_cast<int>(wi))];++group.windowCount;
        group.maxWindowCount=std::max(group.maxWindowCount,static_cast<int>(maximal[wi].members.size()));
        group.members.insert(maximal[wi].members.begin(),maximal[wi].members.end());
    }
    maximal.clear();
    for(const auto& entry:groups) {
        Window group;group.windowCount=entry.second.windowCount;group.maxWindowCount=entry.second.maxWindowCount;
        group.members.assign(entry.second.members.begin(),entry.second.members.end());
        for(const int member:group.members){const auto p=position(villages[member]);group.center.x+=p.x;group.center.y+=p.y;}
        group.center.x/=group.members.size();group.center.y/=group.members.size();maximal.push_back(std::move(group));
    }
    for(const auto& w:maximal) {
        QJsonArray members;QStringList keys;
        for(const int n:w.members){members.append(identity(villages[n]));keys<<villages[n].key;}
        QJsonObject detail;detail["members"]=members;detail["affected_feature_count"]=static_cast<int>(w.members.size());
        detail["window_count"]=w.windowCount;detail["actual_count"]=w.maxWindowCount;detail["max_window_count"]=w.maxWindowCount;detail["allowed_count"]=limit;
        detail["excess_count"]=w.maxWindowCount-limit;detail["window_area_cm2"]=areaCm2;detail["window_radius_mm"]=radiusMm;
        detail["window_center_x"]=w.center.x;detail["window_center_y"]=w.center.y;detail["dedup_key"]=QString(rule+":"+keys.join("|"));
        auto decision=numericDetail(w.maxWindowCount,"<=",limit,QStringLiteral("个"),rule.contains(QStringLiteral("外区"))?QStringLiteral("外区数量上限"):QStringLiteral("主区数量上限"),QStringLiteral("数量超限"));
        for(auto it=decision.begin();it!=decision.end();++it)detail[it.key()]=it.value();
        Feature group=villages[w.members.front()];group.fids=QJsonArray();for(int member:w.members)for(const auto& fid:villages[member].fids)group.fids.append(fid);
        ctx.error(rule,QStringLiteral("%1 cm²范围内有%2个；XML最多允许%3个，超出%4个，数量超限。").arg(areaCm2).arg(w.maxWindowCount).arg(limit).arg(w.maxWindowCount-limit),group,detail,point(w.center.x,w.center.y));
    }
}
void spacing(Context& ctx,const std::vector<Feature>& villages,double minimumMm) {
    if(villages.size()<2)return;
    const double minimum=ctx.fromMm(minimumMm),epsilon=ctx.fromMm(1e-7);PointIndex index(villages);
    struct Pair {int first=0;int second=0;double distance=0;};std::vector<Pair> conflicts;
    for(size_t i=0;i<villages.size();++i) {
        const auto a=position(villages[i]);
        for(const int j:index.near(a.x,a.y,minimum)) {
            if(j<=static_cast<int>(i))continue;
            const auto b=position(villages[j]);const double d=std::hypot(b.x-a.x,b.y-a.y);
            if(d+epsilon>=minimum)continue;
            conflicts.push_back({static_cast<int>(i),j,ctx.mapMm(d)});
        }
    }
    if(conflicts.empty())return;
    DisjointSet clusters(villages.size());
    for(const auto& pair:conflicts)clusters.unite(pair.first,pair.second);
    std::map<int,std::vector<Pair>> groups;std::map<int,std::set<int>> groupMembers;
    for(const auto& pair:conflicts){const int root=clusters.find(pair.first);groups[root].push_back(pair);groupMembers[root].insert(pair.first);groupMembers[root].insert(pair.second);}
    for(const auto& entry:groups) {
        const int root=entry.first;const auto& pairs=entry.second;const auto& memberSet=groupMembers[root];
        QJsonArray members,pairRows;QStringList keys;double centerX=0,centerY=0;
        double closest=std::numeric_limits<double>::infinity(),farthest=0;
        for(const int member:memberSet){members.append(identity(villages[member]));keys<<villages[member].key;const auto p=position(villages[member]);centerX+=p.x;centerY+=p.y;}
        for(const auto& pair:pairs) {
            QJsonObject row;row["first_source_key"]=villages[pair.first].key;row["second_source_key"]=villages[pair.second].key;
            row["distance_mm"]=pair.distance;pairRows.append(row);closest=std::min(closest,pair.distance);farthest=std::max(farthest,pair.distance);
        }
        const double count=static_cast<double>(memberSet.size());centerX/=count;centerY/=count;
        QJsonObject detail;detail["members"]=members;detail["pairs"]=pairRows;detail["pair_count"]=static_cast<int>(pairs.size());
        detail["affected_feature_count"]=static_cast<int>(memberSet.size());detail["distance_mm"]=closest;detail["closest_distance_mm"]=closest;
        detail["farthest_distance_mm"]=farthest;detail["minimum_distance_mm"]=minimumMm;detail["required_distance_mm"]=minimumMm;
        detail["dedup_key"]=QString(QStringLiteral("行政村外区间距不足:")+keys.join("|"));
        auto decision=numericDetail(closest,">=",minimumMm,"mm",QStringLiteral("外区最小间距"),QStringLiteral("选取过密"));for(auto it=decision.begin();it!=decision.end();++it)detail[it.key()]=it.value();
        Feature group=villages[*memberSet.begin()];group.fids=QJsonArray();for(int member:memberSet)for(const auto& fid:villages[member].fids)group.fids.append(fid);
        ctx.error(QStringLiteral("行政村外区间距不足"),numericMessage(QStringLiteral("最近村庄间距"),detail),group,detail,point(centerX,centerY));
    }
}
}

void checkVillages(Context& ctx) {
    const QString object=ctx.cfg.text(QStringLiteral("检查对象"),ctx.paths.standardLayer);
    const bool administrative=object==QStringLiteral("行政村");
    if(!administrative&&object!=QStringLiteral("自然村"))fail(QStringLiteral("村庄检查对象必须为行政村或自然村"));
    const QString mapType=ctx.cfg.text(QStringLiteral("图种"));
    if(mapType!=QStringLiteral("市图")&&mapType!=QStringLiteral("县图"))fail(QStringLiteral("村庄检查必须明确配置图种为市图或县图，不能按比例尺推断"));
    mode(ctx,QStringLiteral("数量统计方式"),QStringLiteral("滑动圆窗"));
    mode(ctx,QStringLiteral("密度缓冲基准"),QStringLiteral("成果行政村符号定位点"));
    mode(ctx,QStringLiteral("密度缓冲方式"),QStringLiteral("定位点圆形缓冲"));
    mode(ctx,QStringLiteral("社区识别方式"),QStringLiteral("名称后缀"));
    if(ctx.cfg.text(QStringLiteral("社区类别字段"),"NAME").isEmpty()||ctx.cfg.list(QStringLiteral("社区名称后缀"),QStringLiteral("社区;社区居委会")).isEmpty())
        fail(QStringLiteral("社区类别字段和名称后缀字典不得为空"));
    mode(ctx,QStringLiteral("主外区边界归属"),QStringLiteral("主区优先"));
    mode(ctx,QStringLiteral("密度边界归属"),QStringLiteral("密集区优先;一般密集区其次;稀疏区最后"));
    for(const auto& key:{QStringLiteral("密集区最小面积"),QStringLiteral("一般密集区最小面积")})units(ctx,key,QStringLiteral("平方毫米"));
    for(const auto& key:{QStringLiteral("主区统计面积"),QStringLiteral("外区统计面积")})units(ctx,key,QStringLiteral("平方厘米"));
    const double mainArea=positive(ctx,QStringLiteral("主区统计面积"),administrative?1.5:2),outerArea=positive(ctx,QStringLiteral("外区统计面积"),2);
    mapArea(ctx,mainArea*100);mapArea(ctx,outerArea*100);
    const int mainLimit=quota(ctx,QStringLiteral("主区数量上限"),2),outerLimit=quota(ctx,QStringLiteral("外区数量上限"),1);
    const double outerSpacing=positive(ctx,QStringLiteral("外区最小间距"),10);
    ctx.targets=symbols(std::move(ctx.targets),ctx.cfg.text(QStringLiteral("成果分组字段"),"symbol_id"));
    ctx.info["total_count"]=static_cast<int>(ctx.targets.size());ctx.info["map_type"]=mapType;ctx.info["village_type"]=object;
    auto located=centers(ctx,ctx.targets,QStringLiteral("村庄定位点恢复"));ctx.info["located_count"]=static_cast<int>(located.size());
    QJsonArray classifications;
    if(!administrative&&mapType==QStringLiteral("市图")&&!ctx.cfg.flag(QStringLiteral("市图自然村是否允许表示"),false)) {
        AreaQuery frame(ctx,safeArea(ctx,ctx.cfg.text(QStringLiteral("检查范围图层"),QStringLiteral("内图廓"))));
        int outsideFrame=0;
        for(const auto& f:located){
            QJsonObject detail;detail["source_key"]=f.key;detail["source_fids"]=f.fids;detail["map_type"]=mapType;
            if(!frame.exists()){
                detail["region"]=QStringLiteral("无法判定");ctx.review(QStringLiteral("村庄图廓归属"),QStringLiteral("缺少有效内图廓，无法确认该自然村是否属于本次检查范围"),&f);
            }
            else if(!frame.covers(f)){++outsideFrame;detail["region"]=QStringLiteral("图廓外");}
            else {detail["region"]=QStringLiteral("本图");detail["xml_rule"]=QStringLiteral("市图自然村是否允许表示");ctx.error(QStringLiteral("市图自然村禁止表示"),QStringLiteral("市图自然村：XML规定不允许表示，成果已表示。"),f,detail);}
            classifications.append(detail);
        }
        ctx.info["outside_frame_count"]=outsideFrame;
        ctx.info["village_classifications"]=classifications;
        ctx.info["density_skipped_reason"]=QStringLiteral("市图自然村全部禁表，此禁表结论不依赖密度和主外区");return;
    }
    const Zones z=zones(ctx,located,administrative);
    const AreaQuery frameQuery(ctx,z.frame),mainQuery(ctx,z.main),outerQuery(ctx,z.outer),denseQuery(ctx,z.dense),generalQuery(ctx,z.general);
    std::vector<Feature> sources;
    try{sources=ctx.gdb(ctx.cfg.text(QStringLiteral("参考GDB图层"),administrative?"BOUP7":"BOUP8"),QString(),false);}
    catch(const std::exception& e){ctx.review(QStringLiteral("村庄参考源"),QString::fromUtf8(e.what()));}
    std::vector<Feature> mainPool,outerPool;int outsideFrame=0;
    for(const auto& village:located) {
        const auto p=position(village);
        QJsonObject detail;detail["source_key"]=village.key;detail["source_fids"]=village.fids;detail["symbol_id"]=village.text("symbol_id");
        detail["center_x"]=p.x;detail["center_y"]=p.y;detail["map_type"]=mapType;
        if(!frameQuery.exists()){
            detail["region"]=QStringLiteral("无法判定");ctx.review(QStringLiteral("村庄图廓归属"),QStringLiteral("缺少有效内图廓，无法确认是否属于本次空间检查范围"),&village);classifications.append(detail);continue;
        }
        if(frameQuery.exists()&&!frameQuery.covers(village)){++outsideFrame;detail["region"]=QStringLiteral("图廓外");classifications.append(detail);continue;}
        const bool inMain=mainQuery.covers(village);
        const bool inOuter=!inMain&&outerQuery.covers(village);
        const QString zone=density(z,denseQuery,generalQuery,village);detail["density"]=zone;
        detail["region"]=inMain?QStringLiteral("主区"):inOuter?QStringLiteral("外区"):QStringLiteral("无法判定");
        if(!inMain&&!inOuter){ctx.review(QStringLiteral("村庄主外区归属"),QStringLiteral("定位点不在已确认主区或外区内，不能把覆盖空缺自动当作外区"),&village);classifications.append(detail);continue;}
        if(inOuter) {
            detail["xml_rule"]=QStringLiteral("外区自然村是否允许表示");
            if(!administrative&&!ctx.cfg.flag(QStringLiteral("外区自然村是否允许表示"),false))
                ctx.error(QStringLiteral("外区自然村禁止表示"),QStringLiteral("外区自然村：XML规定不允许表示，成果已表示。"),village,detail);
            else outerPool.push_back(village);
        } else {
            bool eligible=true;
            if(!ctx.cfg.flag(QStringLiteral("密集区社区是否允许表示"),false)&&(zone==QStringLiteral("密集区")||zone==QStringLiteral("无法判定"))) {
                const int category=community(ctx,village,sources,detail);detail["community"]=category<0?QJsonValue(QJsonValue::Null):QJsonValue(category==1);
                if(category<0)eligible=false;
                else if(category==1) {
                    eligible=false;detail["xml_rule"]=QStringLiteral("密集区社区是否允许表示");
                    if(zone==QStringLiteral("密集区"))ctx.error(QStringLiteral("密集区社区禁止表示"),QStringLiteral("主区密集区社区：XML规定不允许表示，成果已表示。"),village,detail);
                    else ctx.review(QStringLiteral("社区密度归属"),QStringLiteral("社区身份已确认，但密度基准不完整，无法确认是否应禁表"),&village);
                }
            }
            if(eligible)mainPool.push_back(village);
        }
        classifications.append(detail);
    }
    ctx.info["village_classifications"]=classifications;ctx.info["outside_frame_count"]=outsideFrame;
    ctx.info["main_quota_candidate_count"]=static_cast<int>(mainPool.size());ctx.info["outer_quota_candidate_count"]=static_cast<int>(outerPool.size());
    ctx.info["quota_statistic"]=QStringLiteral("滑动圆窗");ctx.info["main_window_area_cm2"]=mainArea;ctx.info["outer_window_area_cm2"]=outerArea;
    countWindows(ctx,mainPool,object+QStringLiteral("主区数量超限"),mainArea,mainLimit);
    if(administrative){countWindows(ctx,outerPool,QStringLiteral("行政村外区数量超限"),outerArea,outerLimit);spacing(ctx,outerPool,outerSpacing);}
}
} // namespace redundancy
