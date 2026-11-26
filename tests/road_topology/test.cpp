#include "ReadShp_interface.h"
#include "map_check_algorithms.h"
#include <qgsapplication.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsproject.h>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTextEdit>
#include <iostream>
#include <functional>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void add(QgsVectorLayer& layer, const QString& wkt) {
    QgsFeature f(layer.fields()); f.setGeometry(QgsGeometry::fromWkt(wkt));
    QgsFeatureList fs{f}; require(layer.dataProvider()->addFeatures(fs), "add feature"); layer.updateExtents();
}
static std::unique_ptr<QgsVectorLayer> save(const QString& dir, const QString& name,
    const QStringList& wkts, const QString& type=QStringLiteral("LineString"), const QString& crs=QStringLiteral("EPSG:3857")) {
    QgsVectorLayer mem(type+"?crs="+crs,name,"memory");
    for (const auto& wkt:wkts) add(mem,wkt);
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName="ESRI Shapefile"; options.fileEncoding="UTF-8";
    const QString path=dir+"/"+name+".shp";
    require(QgsVectorFileWriter::writeAsVectorFormatV3(&mem,path,QgsProject::instance()->transformContext(),options)==QgsVectorFileWriter::NoError,"write fixture SHP");
    auto result=std::make_unique<QgsVectorLayer>(path,name,"ogr"); require(result->isValid(),"load fixture SHP"); return result;
}
static QByteArray configXml() {
    return u8R"(<check_list><scale><value>145000</value><check_item><name>线要素拓扑检查</name><id>112</id>
    <rule><name>悬挂线容差</name><value>0.003</value><unit>毫米</unit></rule>
    <rule><name>连通性容差</name><value>0.034</value><unit>毫米</unit></rule>
    <rule><name>孤立距离</name><value>0.069</value><unit>毫米</unit></rule>
    <rule><name>最小线段长度</name><value>0.034</value><unit>毫米</unit></rule>
    <rule><name>道路连接缓冲距离</name><value>0.2</value><unit>毫米</unit></rule>
    <rule><name>居民点缓冲距离</name><value>1</value><unit>毫米</unit></rule>
    </check_item></scale></check_list>)";
}
static void write(const QString& path,const QByteArray& bytes) { QFile f(path); require(f.open(QIODevice::WriteOnly),"write fixture"); require(f.write(bytes)==bytes.size(),"write complete fixture"); }
static bool has(const std::vector<QualityCheckResult>& rs,const QString& text) {
    for(const auto& r:rs) if(r.errorType.contains(text)) return true; return false;
}
static int actualReplay() {
    const QString source=QString::fromUtf8(u8"D:/AAAGSZJTESTMAP/TESTMAP/永登县地图——新版2_shp");
    const QString reference=QString::fromUtf8(u8"D:/AAAGSZJTESTMAP/永登县/永登县_gdb_shp");
    const QString rules=QDir::current().filePath(QString::fromUtf8(u8"永登县新版2_道路拓扑修订XML_20261030"));
    const QString output=QDir::current().filePath("tmp/road_topology_replay_20261030");
    QJsonArray reports;
    for (const QString& standard:QStringList{QString::fromUtf8(u8"村道"),QString::fromUtf8(u8"省道"),QString::fromUtf8(u8"县道"),QString::fromUtf8(u8"公路隧道"),QString::fromUtf8(u8"高速公路")}) {
        AlgorithmFilePaths paths;paths.standardLayer=standard;
        if(standard==QString::fromUtf8(u8"村道"))
            paths.actualShpSemicolon=source+QString::fromUtf8(u8"/村道_LineString.shp;")+source+QString::fromUtf8(u8"/村道_Polygon.shp");
        else paths.actualShpSemicolon=source+"/"+standard+".shp";
        const QString referenceName=standard==QString::fromUtf8(u8"村道")?"LVLL":(standard==QString::fromUtf8(u8"公路隧道")?"LFCL":"LRDL");
        paths.gdbShpSemicolon=reference+"/"+referenceName+".shp";
        paths.csvPath=rules+QString::fromUtf8(u8"/图层对应关系配置表_UTF8.csv");
        paths.xmlPaths=QStringList{rules+"/"+standard+".xml"};
        CheckItemResult result;::checkLineTopology(QString::fromUtf8(u8"线要素拓扑检查"),paths,145000,output+"/"+standard,result);
        require(result.status==QString::fromUtf8(u8"质检成功"),"actual topology run failed");
        const QString json=output+"/"+standard+QString::fromUtf8(u8"/质检Json文件/线要素拓扑_")+standard+".json";
        QFile f(json);require(f.open(QIODevice::ReadOnly),"actual result JSON missing");
        const QJsonObject item=QJsonDocument::fromJson(f.readAll()).object().value("check_item1").toObject();
        const auto info=item.value("info").toObject();
        if(standard==QString::fromUtf8(u8"村道"))require(info.value("input_line_feature_count").toInt()==1259,"village line input count is not 1259");
        QJsonObject report{{"layer",standard},{"status",item.value("status")},{"info",info},{"output_json",json}};
        reports.append(report);std::cout<<QJsonDocument(report).toJson(QJsonDocument::Compact).constData()<<std::endl;
        // These are proven connections in this exact map, not guessed screenshot identities.
        for(const auto& v:item.value("errors").toArray()) {
            const auto e=v.toObject();const int fid=e.value("feature_id").toInt();
            require(!(standard==QString::fromUtf8(u8"高速公路")&&fid==2),"highway FID 2 has an endpoint inside tunnel portal 7 and the other on the frame");
            require(!(standard==QString::fromUtf8(u8"县道")&&fid==66),"county road FID 66 crosses the inner frame and should not dangle");
            if((standard==QString::fromUtf8(u8"省道")&&fid==26)||(standard==QString::fromUtf8(u8"村道")&&fid==1049))
                require(!e.value("error_type").toString().contains(QString::fromUtf8(u8"独立")),"a proven attached road was still independent");
        }
    }
    write(output+"/summary.json",QJsonDocument(reports).toJson(QJsonDocument::Indented));
    std::cout<<"ACTUAL_REPLAY_PASS "<<output.toStdString()<<std::endl;return 0;
}
int main(int argc,char** argv) {
    std::cerr<<"START road topology regression"<<std::endl;
    QgsApplication app(argc,argv,false);
    std::cerr<<"APPLICATION ready"<<std::endl;
    QgsApplication::setPrefixPath("E:/LTZK/QGIS_32815",true);
    QgsApplication::setPluginPath("E:/LTZK/QGIS_32815/plugins"); QgsApplication::initQgis();
    if(argc>1&&QString::fromLocal8Bit(argv[1])=="--actual")return actualReplay();
    int failed=0,passed=0;
    auto test=[&](const char* name,std::function<void()> run) {
        std::cerr<<"RUN "<<name<<std::endl;
        try { run(); std::cout<<"PASS "<<name<<std::endl; ++passed; }
        catch(const std::exception& e) {std::cout<<"FAIL "<<name<<": "<<e.what()<<std::endl;++failed;}
    };
    auto check=[&](QgsVectorLayer* road,QgsVectorLayer* ref,const QString& dir) {
        const QString xml=dir+"/rule.xml";write(xml,configXml());
        ReadShpDialog dlg;require(dlg.m_config->loadFromFile(xml),"load config");dlg.m_mapScale=145000;dlg.updateParametersFromConfig();
        return dlg.checkRoadLineTopology(road,ref,road->name()+".shp");
    };
    test("one settlement end cannot hide the other dangling end",[&] {
        QTemporaryDir d; auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto s=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(0 0)"},"Point");
        QgsVectorLayer ref("LineString?crs=EPSG:3857","LVLL","memory");add(ref,"LINESTRING(0 0,1000 0)");
        auto rs=check(r.get(),&ref,d.path());require(has(rs,QString::fromUtf8(u8"悬挂")),"free end was erased by the populated end");require(!has(rs,QString::fromUtf8(u8"独立")),"settlement-anchored line is not independent");
    });
    test("three detached but connected road segments classify each endpoint",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)","LINESTRING(1000 0,2000 0)","LINESTRING(2000 0,3000 0)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(rs.size()==2,"three-road chain should report two outer roads only");
        for(const auto& x:rs) {
            require(x.errorType==QString::fromUtf8(u8"道路悬挂"),"chain outer road should be dangling, not independent");
            require(x.featureId!=1,"middle road with both ends connected should not be reported");
        }
    });
    test("both ends within 29 metres of other road grades are connected",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto other=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(-29 -500,-29 500)","LINESTRING(1029 -500,1029 500)"});
        require(check(r.get(),nullptr,d.path()).empty(),"cross-grade buffer connections were ignored");
    });
    test("reference overlap cannot connect an isolated result road",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        QgsVectorLayer ref("LineString?crs=EPSG:3857","LVLL","memory");add(ref,"LINESTRING(0 0,1000 0)");
        auto rs=check(r.get(),&ref,d.path());require(has(rs,QString::fromUtf8(u8"独立")),"reference line suppressed result-network isolation");require(!has(rs,QString::fromUtf8(u8"悬挂")),"independent component duplicated as dangling");
    });
    test("dangling distance describes the free endpoint rather than connected start",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto road=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(-500 0,0 0)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(rs.size()==1&&rs[0].errorType.contains(QString::fromUtf8(u8"悬挂"))&&rs[0].calculatedValue>999,"connected start's zero distance hid the free endpoint's distance");
        require(rs[0].description.contains(QString::fromUtf8(u8"道路有一端没有接上其他道路")) && rs[0].description.contains(QString::fromUtf8(u8"线的最后一个点")) && !rs[0].description.contains(QString::fromUtf8(u8"部件")),"dangling explanation is not understandable");
    });
    test("both free ends are independent even when middle crosses a road",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto road=save(d.path(),QString::fromUtf8(u8"国道"),{"LINESTRING(500 -500,500 500)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(rs.size()==1&&has(rs,QString::fromUtf8(u8"独立"))&&!has(rs,QString::fromUtf8(u8"悬挂")),"two free ends were not classified independent");
        require(rs[0].description.contains(QString::fromUtf8(u8"两端")),"independent explanation must identify both ends");
        require(rs[0].calculatedValue>499,"distance must use the endpoints, not the middle intersection");
    });
    test("middle resident does not exempt two free ends",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto road=save(d.path(),QString::fromUtf8(u8"公路隧道"),{"LINESTRING(500 -500,500 500)"});
        auto resident=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(500 0)"},"Point");
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"mid-line contacts incorrectly replaced endpoint checks");
    });
    test("one endpoint resident remains dangling when middle meets road",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto road=save(d.path(),QString::fromUtf8(u8"国道"),{"LINESTRING(500 -500,500 500)"});
        auto resident=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(0 0)"},"Point");
        const auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"one reasonable endpoint must remain a dangling classification");
    });
    test("detached chain reports only dangling outer ends",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)","LINESTRING(1000 0,2000 0)"});
        auto rs=check(r.get(),nullptr,d.path());require(rs.size()==2&&has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"chain outer ends should be dangling");
    });
    test("open roads forming a loop have connected endpoints",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0,1000 1000)"});
        auto connector=save(d.path(),QString::fromUtf8(u8"连接道"),{"LINESTRING(1000 1000,0 1000,0 0)"});
        auto main=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(10000 0,20000 0)"});
        auto main2=save(d.path(),QString::fromUtf8(u8"省道"),{"LINESTRING(20000 0,30000 0)"});
        require(check(r.get(),nullptr,d.path()).empty(),"loop endpoints join another road and should not be reported");
    });
    test("open road endpoint classification does not depend on main network",[&] {
        QTemporaryDir d;auto county=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(0 0,1000 0,1000 1000)","LINESTRING(1000 1000,0 1000,0 0)"});
        auto province=save(d.path(),QString::fromUtf8(u8"省道"),{"LINESTRING(10000 0,11000 0,11000 1000)","LINESTRING(11000 1000,10000 1000,10000 0)"});
        const bool a=has(check(county.get(),nullptr,d.path()),QString::fromUtf8(u8"独立"));
        const bool b=has(check(province.get(),nullptr,d.path()),QString::fromUtf8(u8"独立"));
        require(!a&&!b,"connected open road endpoints must not be called independent");
    });
    test("closed road isolated from main network remains independent",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0,1000 1000,0 1000,0 0)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"detached closed road must retain independent check");
    });
    test("road tunnels join roads but tunnel mouth symbols do not",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"省道"),{"LINESTRING(0 0,1000 0)"});
        auto tunnel=save(d.path(),QString::fromUtf8(u8"公路隧道"),{"LINESTRING(-500 0,0 0)","LINESTRING(1000 0,1500 0)"});
        require(check(r.get(),nullptr,d.path()).empty(),"tunnel did not join road endpoints");
        tunnel.reset();QFile::remove(d.path()+QString::fromUtf8(u8"/公路隧道.shp"));
        auto mouth=save(d.path(),QString::fromUtf8(u8"公路隧道口"),{"LINESTRING(0 -10,0 10)","LINESTRING(1000 -10,1000 10)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"tunnel symbols falsely joined the road");
    });
    test("road endpoint inside closed tunnel portal and other end on frame",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"高速公路"),{"LINESTRING(0 0,1000 0)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"公路隧道口_LineString"),{"LINESTRING(-100 -100,100 -100,100 100,-100 100,-100 -100)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(1000 -100,1000 100)"});
        require(check(r.get(),nullptr,d.path()).empty(),"portal interior did not connect road");
    });
    test("one portal endpoint does not hide the other dangling end",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"公路隧道口"),{"LINESTRING(-100 -100,100 -100,100 100,-100 100,-100 -100)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"portal should anchor component but not hide other end");
    });
    test("railway tunnel portal connects only its own endpoint",[&] {
        QTemporaryDir d;
        auto tunnel=save(d.path(),QString::fromUtf8(u8"铁路隧道"),{"LINESTRING(0 0,1000 0)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"铁路隧道口"),{"LINESTRING(-20 -20,20 -20,20 20,-20 20,-20 -20)"});
        const auto rs=check(tunnel.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),
                "railway portal should anchor one endpoint and leave the other dangling");
    });
    test("portal area uses 29m tolerance outside its actual shape",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"高速公路"),{"LINESTRING(0 0,1000 0)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"公路隧道口"),{"LINESTRING(-129 -100,-29 -100,-29 100,-129 100,-129 -100)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(1000 -100,1000 100)"});
        require(check(r.get(),nullptr,d.path()).empty(),"portal at exactly 29m did not connect");
    });
    test("outside portal tolerance and inside bounding box are not exempt",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"高速公路"),{"LINESTRING(90 90,1000 90)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"公路隧道口"),{"LINESTRING(0 0,100 0,0 100,0 0)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(1000 0,1000 200)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"悬挂")),"portal bounding box incorrectly exempted endpoint beyond 29m");
    });
    test("open portal outline does not create an area",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto portal=save(d.path(),QString::fromUtf8(u8"公路隧道口"),{"LINESTRING(-100 -100,100 -100,100 100,-100 100)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"open portal was silently closed and used to exempt road");
    });
    test("resident buffer is 145 metres and applies to each endpoint",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto s=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(0 145)","POINT(1000 145)"},"Point");
        require(check(r.get(),nullptr,d.path()).empty(),"145 metre populated endpoints were not exempted");
    });
    test("suffixed settlement polygons exempt endpoints in their interior",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(100 100,1100 100)"});
        auto s=save(d.path(),QString::fromUtf8(u8"街区面_Polygon"),{"POLYGON((0 0,200 0,200 200,0 200,0 0))","POLYGON((1000 0,1200 0,1200 200,1000 200,1000 0))"},"Polygon");
        require(check(r.get(),nullptr,d.path()).empty(),"resident polygon suffix or interior was ignored");
    });
    test("street block lines use road tolerance rather than settlement tolerance",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto block=save(d.path(),QString::fromUtf8(u8"街区面_LineString"),{"LINESTRING(-30 -100,-30 100)","LINESTRING(1030 -100,1030 100)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"street block lines at 30m wrongly used the 145m settlement tolerance");
    });
    test("street block lines connect at 29m without hiding the other end",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto block=save(d.path(),QString::fromUtf8(u8"街区面_LineString"),{"LINESTRING(-29 -100,-29 100)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"one block-connected end hid the other end or was considered isolated");
        require(rs[0].description.contains(QString::fromUtf8(u8"街区面")),"error description omitted street blocks");
    });
    test("street block polygon outside 29m cannot exempt endpoints",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto block=save(d.path(),QString::fromUtf8(u8"街区面_Polygon"),{"POLYGON((-100 -100,-30 -100,-30 100,-100 100,-100 -100))","POLYGON((1030 -100,1100 -100,1100 100,1030 100,1030 -100))"},"Polygon");
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"street block polygon at 30m wrongly used the 145m settlement tolerance");
    });
    test("closed street block lines do not fill their interiors",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(100 100,200 100)"});
        auto block=save(d.path(),QString::fromUtf8(u8"街区面_LineString"),{"LINESTRING(0 0,300 0,300 300,0 300,0 0)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"closed block line falsely treated its interior as a polygon");
    });
    test("street block points do not act as line or polygon connections",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto block=save(d.path(),QString::fromUtf8(u8"街区面_Point"),{"POINT(0 0)","POINT(1000 0)"},"Point");
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"street block point incorrectly acted as a settlement");
    });
    test("road context is transformed before metre buffer comparisons",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto other=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(0 -0.01,0 0.01)","LINESTRING(0.008983152841 -0.01,0.008983152841 0.01)"},"LineString","EPSG:4326");
        require(check(r.get(),nullptr,d.path()).empty(),"geographic road coordinates were compared directly to metres");
    });
    test("railways and walls do not join road endpoints",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto rail=save(d.path(),QString::fromUtf8(u8"普通铁路"),{"LINESTRING(0 -500,0 500)","LINESTRING(1000 -500,1000 500)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"railways hid isolation");
    });
    test("inner frame line anchors only nearby endpoints",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(-29 -100,-29 100)","LINESTRING(1029 -100,1029 100)"});
        require(check(r.get(),nullptr,d.path()).empty(),"frame endpoints at tolerance were not exempted");
    });
    test("inner frame polygon uses its boundary rather than its interior",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(1000 1000,2000 1000)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓_Polygon"),{"POLYGON((0 0,3000 0,3000 3000,0 3000,0 0))"},"Polygon");
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"polygon interior falsely exempted an isolated road");
    });
    test("frame outside tolerance does not hide isolated road",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(1030 -100,1030 100)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"out-of-range frame hid isolation");
    });
    test("one frame-connected endpoint does not hide the other",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(0 -100,0 100)"});
        auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"frame-connected road needs its other end checked");
    });
    test("crossing the frame connects outside ends beyond tolerance",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(-100 500,500 500,1100 500)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(0 0,1000 0,1000 1000,0 1000,0 0)"});
        require(check(r.get(),nullptr,d.path()).empty(),"outside ends across frame should be connected");
    });
    test("crossing frame does not exempt unrelated end inside",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"县道"),{"LINESTRING(-100 500,500 500)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(0 0,1000 0,1000 1000,0 1000,0 0)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"悬挂"))&&!has(rs,QString::fromUtf8(u8"独立")),"inside free end must remain dangling");
        require(rs[0].description.contains(QString::fromUtf8(u8"线的最后一个点")),"wrong endpoint was exempted");
    });
    test("tangent outside frame does not connect outside ends",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(-200 -200,0 0,-200 200)"});
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(0 0,1000 0,1000 1000,0 1000,0 0)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"outside tangent must not anchor road");
    });
    test("frame crossing affects only its multipart road segment",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"县道"),{"MULTILINESTRING((-100 500,0 500,1000 500),(400 400,600 400))"},"MultiLineString");
        auto frame=save(d.path(),QString::fromUtf8(u8"内图廓"),{"LINESTRING(0 0,1000 0,1000 1000,0 1000,0 0)"});
        const auto rs=check(r.get(),nullptr,d.path());
        require(has(rs,QString::fromUtf8(u8"独立")),"unrelated multipart segment must remain isolated");
    });
    test("street lines remain outside road context",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto street=save(d.path(),QString::fromUtf8(u8"街道线"),{"LINESTRING(0 -100,0 100)","LINESTRING(1000 -100,1000 100)"});
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"独立")),"street lines unexpectedly anchor roads");
    });
    test("multipart roads check every part",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"MULTILINESTRING((0 0,1000 0),(3000 0,4000 0))"},"MultiLineString");
        auto s=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(0 0)","POINT(1000 0)","POINT(3000 0)"},"Point");
        require(has(check(r.get(),nullptr,d.path()),QString::fromUtf8(u8"悬挂")),"multipart free endpoint was skipped");
    });
    test("merged multipart errors retain every erroneous part geometry",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"MULTILINESTRING((0 0,1000 0),(3000 0,4000 0))"},"MultiLineString");
        auto s=save(d.path(),QString::fromUtf8(u8"自然村"),{"POINT(0 0)","POINT(3000 0)"},"Point");
        auto rs=check(r.get(),nullptr,d.path());mergeDuplicateLineTopologyErrorRecords(rs);
        ReadShpDialog dlg;require(rs.size()==1&&dlg.extractLineStrings(rs[0].errorGeometry).size()==2,"deduplication discarded an erroneous part");
    });
    test("semicolon line plus polygon input runs instead of file-not-found",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道_LineString"),{"LINESTRING(0 0,1000 0)"});
        auto poly=save(d.path(),QString::fromUtf8(u8"村道_Polygon"),{"POLYGON((0 0,10 0,10 10,0 0))"},"Polygon");
        auto ref=save(d.path(),"LVLL",{"LINESTRING(0 0,1000 0)"});
        QString xml=d.path()+"/rule.xml";write(xml,configXml());
        auto parsed=(r->source()+"; "+poly->source()).toUtf8();auto reference=ref->source().toUtf8();auto cfg=xml.toUtf8();
        auto csv=QString::fromUtf8(u8"D:/AAAGSZJTESTMAP/挂图XML文件汇总_10.28/图层对应关系配置表_UTF8.csv").toUtf8();
        auto standard=QString::fromUtf8(u8"村道").toUtf8();auto shp=(d.path()+"/errors.shp").toUtf8();auto json=(d.path()+"/result.json").toUtf8();auto logs=(d.path()+"/logs").toUtf8();
        const auto code=checkLineTopologyWithStandardLayer(parsed,reference,cfg,csv,standard,shp,json,logs,145000);
        require(code==READSHP_SUCCESS,"line/polygon path string failed before topology");
        QFile f(QString::fromUtf8(json));require(f.open(QIODevice::ReadOnly),"open result");auto item=QJsonDocument::fromJson(f.readAll()).object().value("check_item1").toObject();
        require(item.value("status").toString()==QString::fromUtf8(u8"质检成功"),"check never executed");
        require(item.value("info").toObject().value("input_line_feature_count").toInt()==1,"missing actual participation count");
    });
    test("task cannot silently substitute another road XML",[&] {
        QTemporaryDir d;auto r=save(d.path(),QString::fromUtf8(u8"村道"),{"LINESTRING(0 0,1000 0)"});
        auto ref=save(d.path(),"LVLL",{"LINESTRING(0 0,1000 0)"});
        const QString wrong=d.path()+QString::fromUtf8(u8"/县道.xml");write(wrong,configXml());
        AlgorithmFilePaths paths;paths.actualShpSemicolon=r->source();paths.gdbShpSemicolon=ref->source();
        paths.standardLayer=QString::fromUtf8(u8"村道");paths.xmlPaths=QStringList{wrong};
        paths.csvPath=QString::fromUtf8(u8"D:/AAAGSZJTESTMAP/挂图XML文件汇总_10.28/图层对应关系配置表_UTF8.csv");
        CheckItemResult result;::checkLineTopology(QString::fromUtf8(u8"线要素拓扑检查"),paths,145000,d.path()+"/out",result);
        require(result.status==QString::fromUtf8(u8"质检失败"),"unmatched rule was silently accepted");
    });
    test("missing input is not a successful topology run",[&] {
        QTemporaryDir d;AlgorithmFilePaths paths;paths.standardLayer=QString::fromUtf8(u8"村道");
        CheckItemResult result;::checkLineTopology(QString::fromUtf8(u8"线要素拓扑检查"),paths,145000,d.path(),result);
        require(result.status==QString::fromUtf8(u8"质检失败"),"missing input returned success");
    });
    std::cout<<"TOTAL passed="<<passed<<" failed="<<failed<<std::endl;return failed?1:0;
}
