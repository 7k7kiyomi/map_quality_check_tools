#include "searchable_layer_combo_box.h"
#include "series_layer_mapping.h"
#include <QApplication>
#include <QtTest/QTest>
#include <QInputMethodEvent>
#include <QTreeWidget>
#include <iostream>
#include "matching_helpers.inc"
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL line " << __LINE__ << ": " << #x << std::endl; return 1; } } while (0)
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // The visible candidates use pinyin order, independently of insertion order.
    LayerSelection::SearchableLayerComboBox sorted;
    const QStringList unsorted{QStringLiteral("乡道.shp"),QStringLiteral("水系线.shp"),QStringLiteral("村道_LineStrings.shp"),QStringLiteral("国道.shp"),QStringLiteral("湖泊水库.shp"),QStringLiteral("县道.shp"),QStringLiteral("河流面.shp"),QStringLiteral("干渠.shp"),QStringLiteral("常年河.shp"),QStringLiteral("高速公路.shp"),QStringLiteral("村道.shp")};
    sorted.addItem(QStringLiteral("未选择"),QString());
    for(const QString& name:unsorted)sorted.addItem(name,QStringLiteral("maps/")+name);
    sorted.setCurrentIndex(sorted.findData(QStringLiteral("maps/国道.shp")));int sortedChanged=0;
    QObject::connect(&sorted,QOverload<int>::of(&QComboBox::currentIndexChanged),[&](int){++sortedChanged;});
    sorted.enableSingleSelectionSearch();sorted.resize(360,32);sorted.show();
    CHECK(sortedChanged==0);CHECK(sorted.currentData().toString()==QStringLiteral("maps/国道.shp"));
    QTest::mouseClick(&sorted,Qt::LeftButton);app.processEvents();
    auto* sortedSearch=sorted.findChild<QLineEdit*>(QStringLiteral("layerSearchEdit"));auto* sortedView=sorted.findChild<QListView*>(QStringLiteral("layerSearchResults"));CHECK(sortedSearch&&sortedView);
    auto visibleNames=[&](){QStringList names;for(int row=0;row<sortedView->model()->rowCount();++row)names<<sortedView->model()->index(row,0).data().toString();return names;};
    const QStringList expected{QStringLiteral("未选择"),QStringLiteral("常年河.shp"),QStringLiteral("村道.shp"),QStringLiteral("村道_LineStrings.shp"),QStringLiteral("干渠.shp"),QStringLiteral("高速公路.shp"),QStringLiteral("国道.shp"),QStringLiteral("河流面.shp"),QStringLiteral("湖泊水库.shp"),QStringLiteral("水系线.shp"),QStringLiteral("县道.shp"),QStringLiteral("乡道.shp")};
    if(visibleNames()!=expected){std::cerr<<"Actual sort: "<<visibleNames().join(QStringLiteral(" | ")).toUtf8().constData()<<std::endl;}CHECK(visibleNames()==expected);
    sortedSearch->setText(QStringLiteral("道"));app.processEvents();
    CHECK(visibleNames()==QStringList({QStringLiteral("村道.shp"),QStringLiteral("村道_LineStrings.shp"),QStringLiteral("国道.shp"),QStringLiteral("县道.shp"),QStringLiteral("乡道.shp")}));
    CHECK(sortedChanged==0);QTest::keyClick(sortedSearch,Qt::Key_Down);QTest::keyClick(sortedSearch,Qt::Key_Return);app.processEvents();
    CHECK(sorted.currentData().toString()==QStringLiteral("maps/村道_LineStrings.shp"));CHECK(sortedChanged==1);
    // A later candidate must be sorted in without moving the selected source path.
    sorted.addItem(QStringLiteral("道路.shp"),QStringLiteral("maps/道路.shp"));QTest::mouseClick(&sorted,Qt::LeftButton);app.processEvents();sortedSearch->setText(QStringLiteral("道"));app.processEvents();
    CHECK(visibleNames()==QStringList({QStringLiteral("村道.shp"),QStringLiteral("村道_LineStrings.shp"),QStringLiteral("道路.shp"),QStringLiteral("国道.shp"),QStringLiteral("县道.shp"),QStringLiteral("乡道.shp")}));
    QTest::keyClick(sortedSearch,Qt::Key_Escape);CHECK(sorted.currentData().toString()==QStringLiteral("maps/村道_LineStrings.shp"));
    // Latin names ignore case; numeric suffixes use human-readable numeric order.
    LayerSelection::SearchableLayerComboBox latin;latin.addItem(QStringLiteral("（不对应）"),QString());
    for(const QString& name:QStringList{QStringLiteral("zebra.shp"),QStringLiteral("Layer10.shp"),QStringLiteral("alpha.shp"),QStringLiteral("layer2.shp")})latin.addItem(name,QStringLiteral("maps/")+name);
    latin.enableSingleSelectionSearch();latin.resize(360,32);latin.show();QTest::mouseClick(&latin,Qt::LeftButton);app.processEvents();auto* latinView=latin.findChild<QListView*>(QStringLiteral("layerSearchResults"));CHECK(latinView);
    QStringList latinNames;for(int row=0;row<latinView->model()->rowCount();++row)latinNames<<latinView->model()->index(row,0).data().toString();
    CHECK(latinNames==QStringList({QStringLiteral("（不对应）"),QStringLiteral("alpha.shp"),QStringLiteral("layer2.shp"),QStringLiteral("Layer10.shp"),QStringLiteral("zebra.shp")}));QTest::keyClick(latin.findChild<QLineEdit*>(QStringLiteral("layerSearchEdit")),Qt::Key_Escape);
    std::cout<<"PASS: pinyin candidate sorting, filtered order, unset first, case-insensitive and numeric sorting, live insertions, unchanged source paths and keyboard mapping"<<std::endl;
    QMap<QString, QString> aliases;
    addSeriesLayerAliasGroupForDialog(aliases, QStringLiteral("道路"), {QStringLiteral("村道"), QStringLiteral("国道")});
    CHECK(SeriesLayerMapping::layerBaseName(QStringLiteral("村道_LineStrings.shp")) == QStringLiteral("村道"));
    CHECK(SeriesLayerMapping::layerBaseName(QStringLiteral("村道_MultiLineString.SHP")) == QStringLiteral("村道"));
    CHECK(SeriesLayerMapping::layerBaseName(QStringLiteral("村道名称标注.shp")) == QStringLiteral("村道名称标注"));
    CHECK(seriesLayerCategoryKeyForDialog(QStringLiteral("村道_LineStrings.shp"), aliases) == QStringLiteral("道路"));
    CHECK(seriesLayerNameMatchScoreForDialog(QStringLiteral("村道.shp"),QStringLiteral("村道.shp"),aliases) == 3);
    CHECK(seriesLayerNameMatchScoreForDialog(QStringLiteral("村道.shp"),QStringLiteral("村道_LineStrings.shp"),aliases) == 2);
    CHECK(seriesLayerNameMatchScoreForDialog(QStringLiteral("村道.shp"),QStringLiteral("国道.shp"),aliases) == 0);
    CHECK(seriesLayerNameMatchScoreForDialog(QStringLiteral("村道.shp"),QStringLiteral("村道名称标注.shp"),aliases) == 0);
    addSeriesLayerAliasGroupForDialog(aliases,QStringLiteral("市级行政区界线"),{QStringLiteral("市州级行政区界线")});
    CHECK(seriesLayerNameMatchScoreForDialog(QStringLiteral("市级行政区界线.shp"),QStringLiteral("市州级行政区界线_LineString.shp"),aliases) == 1);
    LayerSelection::SearchableLayerComboBox combo;
    combo.addItem(QStringLiteral("未选择"), QString());
    combo.addItem(QStringLiteral("村道.shp"), QStringLiteral("main/村道.shp"));
    combo.addItem(QStringLiteral("村道_LineStrings.shp"), QStringLiteral("related/村道_LineStrings.shp"));
    combo.addItem(QStringLiteral("国道.shp"), QStringLiteral("related/国道.shp"));
    combo.setCurrentIndex(3);
    combo.enableSingleSelectionSearch();
    int changed=0;
    QObject::connect(&combo,QOverload<int>::of(&QComboBox::currentIndexChanged),[&](int){++changed;});
    combo.resize(330,32); combo.show();
    QTest::mouseClick(&combo,Qt::LeftButton); app.processEvents();
    auto* search=combo.findChild<QLineEdit*>(QStringLiteral("layerSearchEdit"));
    auto* view=combo.findChild<QListView*>(QStringLiteral("layerSearchResults"));
    CHECK(search && view);
    QInputMethodEvent preedit(QStringLiteral("cun"), {});
    QApplication::sendEvent(search,&preedit);
    QTest::keyClick(search,Qt::Key_Return);
    CHECK(changed == 0);
    QInputMethodEvent ime; ime.setCommitString(QStringLiteral("村道"));
    QApplication::sendEvent(search,&ime); app.processEvents();
    CHECK(view->model()->rowCount() == 2);
    CHECK(combo.currentData().toString() == QStringLiteral("related/国道.shp"));
    CHECK(changed == 0);
    QTest::keyClick(search,Qt::Key_Down);
    QTest::keyClick(search,Qt::Key_Return); app.processEvents();
    CHECK(combo.currentData().toString() == QStringLiteral("related/村道_LineStrings.shp"));
    CHECK(changed == 1);
    QTest::mouseClick(&combo,Qt::LeftButton); app.processEvents();
    search->setText(QStringLiteral("不存在")); app.processEvents();
    CHECK(view->model()->rowCount() == 0);
    QTest::keyClick(search,Qt::Key_Return);
    CHECK(changed == 1);
    QTest::keyClick(search,Qt::Key_Escape); app.processEvents();
    CHECK(combo.currentData().toString() == QStringLiteral("related/村道_LineStrings.shp"));
    // Mouse selection and clearing must map filtered rows back to original paths.
    QTest::mouseClick(&combo,Qt::LeftButton); app.processEvents();
    search->setText(QStringLiteral("国")); app.processEvents();
    auto index=view->model()->index(0,0);
    QTest::mouseClick(view->viewport(),Qt::LeftButton,Qt::NoModifier,view->visualRect(index).center());
    CHECK(combo.currentData().toString() == QStringLiteral("related/国道.shp"));
    QTest::mouseClick(&combo,Qt::LeftButton); app.processEvents();
    search->clear(); QTest::keyClick(search,Qt::Key_Home);
    search->setText(QStringLiteral("未选择")); QTest::keyClick(search,Qt::Key_Return);
    CHECK(combo.currentData().toString().isEmpty());
    // Existing multi-select configuration retains checks across searches.
    LayerSelection::SearchableLayerComboBox multi;
    multi.setEditable(true); multi.lineEdit()->setReadOnly(true);
    auto* model=new QStandardItemModel(&multi);
    for (const QString& name: {QStringLiteral("村道"),QStringLiteral("国道")}) {
        auto* item=new QStandardItem(name);
        item->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked); model->appendRow(item);
    }
    multi.setCheckableSourceModel(model); multi.resize(330,32); multi.show();
    QTest::mouseClick(&multi,Qt::LeftButton); app.processEvents();
    auto* multiSearch=multi.findChild<QLineEdit*>(QStringLiteral("layerSearchEdit"));
    auto* multiView=multi.findChild<QListView*>(QStringLiteral("layerSearchResults"));
    multiSearch->setText(QStringLiteral("村")); app.processEvents();
    CHECK(multiView->model()->rowCount()==1);
    auto mi=multiView->model()->index(0,0);
    QTest::mouseClick(multiView->viewport(),Qt::LeftButton,Qt::NoModifier,multiView->visualRect(mi).center());
    CHECK(model->item(0)->checkState()==Qt::Checked);
    multiSearch->setText(QStringLiteral("国")); app.processEvents();
    CHECK(model->item(0)->checkState()==Qt::Checked);
    multiSearch->clear(); app.processEvents();
    CHECK(multiView->model()->rowCount()==2);
    QJsonObject mapping; SeriesLayerMapping::appendRow(mapping,{QStringLiteral("a/村道.shp")},{QStringLiteral("b/村道_LineStrings.shp")});
    CHECK(SeriesLayerMapping::allowsPair(mapping,QStringLiteral("a/村道.shp"),QStringLiteral("b/村道_LineStrings.shp")));
    CHECK(!SeriesLayerMapping::allowsPair(mapping,QStringLiteral("a/村道.shp"),QStringLiteral("b/国道.shp")));
    std::cout << "PASS: names, aliases, Chinese IME filtering, keyboard/mouse selection, cancellation, empty results, clear selection, multi-select and saved row paths" << std::endl;
    return 0;
}
