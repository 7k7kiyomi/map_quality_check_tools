#include "map_path_display_edit.h"
#include "series_layer_exclusions.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QDir>
#include <QFontDatabase>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << std::endl; return 1; } } while (0)
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const int fontId=QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
    CHECK(fontId>=0);
    app.setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).first(),11));
    using namespace SeriesLayerExclusions;
    CHECK(countyMap(QStringLiteral("D:/分县地图/永登县/永登县.pdf")));
    CHECK(countyMap(QStringLiteral("永登县地图——新版2.pdf")));
    CHECK(countyMap(QStringLiteral("红古区_shp")));
    CHECK(countyMap(QStringLiteral("阿拉善左旗地图.pdf")));
    CHECK(!countyMap(QStringLiteral("兰州市政区图.pdf")));
    CHECK(!countyMap(QStringLiteral("兰州分县政区.pdf")));
    CHECK(!countyMap(QStringLiteral("甘肃省行政区划图.pdf")));
    const QStringList excludes{QStringLiteral("国道代码标注"), QStringLiteral("乡镇代码标注"),QStringLiteral("道路代码注记"),
        QStringLiteral("引线"),QStringLiteral("道路引线"),QStringLiteral("快速路"),QStringLiteral("快速路名称标注"),
        QStringLiteral("地图整饰"),QStringLiteral("图例"),QStringLiteral("渠流向"),QStringLiteral("渠道流向"),
        QStringLiteral("省级表面注记_标注"),QStringLiteral("县级级表面注记"),QStringLiteral("乡镇级表面注记"),
        QStringLiteral("行政区划面普色"),QStringLiteral("行政区划普色"),QStringLiteral("行政区画面普色"),QStringLiteral("位置图")};
    for (const auto& name:excludes) for(const QString& suffix:{QStringLiteral(".shp"),QStringLiteral("_LineString.shp"),QStringLiteral("_LineStrings.shp"),QStringLiteral("_Polygon.shp"),QStringLiteral("_Point.SHP")})
        CHECK(excluded(name+suffix,true));
    for (const QString& name:{QStringLiteral("乡镇级行政区界线.shp"),QStringLiteral("乡镇界线_LineString.shp"),QStringLiteral("乡界.shp"),QStringLiteral("晕带_Polygon.shp")}) {
        CHECK(excluded(name,false,true)); CHECK(!excluded(name,false,false));
    }
    for (const QString& name:{QStringLiteral("国道.shp"),QStringLiteral("村道_LineStrings.shp"),QStringLiteral("河流面_Polygon.shp"),QStringLiteral("县级行政区界线.shp"),QStringLiteral("乡镇名称标注.shp"),QStringLiteral("山峰.shp")}) {
        CHECK(!excluded(name,true)); CHECK(!excluded(name,false,true));
    }
    QJsonObject roads; SeriesLayerMapping::appendRow(roads,{QStringLiteral("a/国道.shp")},{QStringLiteral("b/国道_LineStrings.shp")});
    SeriesLayerMapping::appendRow(roads,{QStringLiteral("a/快速路.shp")},{QStringLiteral("b/快速路.shp")});
    QJsonObject boundary; SeriesLayerMapping::appendRow(boundary,{QStringLiteral("a/县级行政区界线.shp")},{QStringLiteral("b/县级行政区界线.shp")});
    SeriesLayerMapping::appendRow(boundary,{QStringLiteral("a/乡镇级行政区界线.shp")},{QStringLiteral("b/乡镇级行政区界线.shp")});
    QJsonObject root{{QStringLiteral("layers"),QJsonObject{{QStringLiteral("道路"),roads},{QStringLiteral("境界线"),boundary}}}};
    const auto cross=filterMapping(root,true).value(QStringLiteral("layers")).toObject();
    CHECK(cross.value(QStringLiteral("道路")).toObject().value(QStringLiteral("rows")).toArray().size()==1);
    CHECK(cross.value(QStringLiteral("境界线")).toObject().value(QStringLiteral("rows")).toArray().size()==2);
    const auto county=filterMapping(root,false,true).value(QStringLiteral("layers")).toObject();
    CHECK(county.value(QStringLiteral("道路")).toObject().value(QStringLiteral("rows")).toArray().size()==2);
    CHECK(county.value(QStringLiteral("境界线")).toObject().value(QStringLiteral("rows")).toArray().size()==1);
    QJsonObject mixed; SeriesLayerMapping::appendRow(mixed,{QStringLiteral("a/引线.shp")},{QStringLiteral("b/国道.shp")});
    CHECK(filterMapping(QJsonObject{{QStringLiteral("layers"),QJsonObject{{QStringLiteral("道路"),mixed}}}},true).value(QStringLiteral("layers")).toObject().isEmpty());
    MapPathDisplayEdit edit;
    const QString original=QStringLiteral("D:/Desktop/测试/红古区/红古区.pdf;D:/Desktop/测试/永登县地图——新版2/永登县地图——新版2.pdf");
    QLineEdit* reader=&edit;
    reader->setText(original);
    CHECK(reader->text()==original);
    CHECK(edit.displayedMapNames()==QStringLiteral("红古区.pdf；永登县地图——新版2.pdf"));
    CHECK(!edit.displayedMapNames().contains(QLatin1Char('/')));
    CHECK(edit.toolTip()==original);
    edit.resize(570,36); edit.show(); app.processEvents();
    CHECK(edit.grab().save(QStringLiteral("map_names_preview.png")));
    reader->clear(); CHECK(edit.displayedMapNames().isEmpty()); CHECK(reader->text().isEmpty());
    reader->setText(QStringLiteral("D:\\Desktop\\永登县\\永登县.pdf"));
    CHECK(edit.displayedMapNames()==QStringLiteral("永登县.pdf"));
    int excludedCount=0,keptCount=0;
    const QDir real(QStringLiteral("D:/Desktop/LTZK/图幅接边与跨比例尺检查/永登县/永登县_shp"));
    for(const QString& name:real.entryList({QStringLiteral("*.shp")},QDir::Files)) {
        if(excluded(name,true)) ++excludedCount; else ++keptCount;
    }
    CHECK(excludedCount>0 && keptCount>0);
    std::cout << "PASS: map basename display/original paths, county classification, exclusions and geometry suffixes, kept layers, saved mapping sanitization; real SHP directory excluded=" << excludedCount << " kept=" << keptCount << std::endl;
    return 0;
}
