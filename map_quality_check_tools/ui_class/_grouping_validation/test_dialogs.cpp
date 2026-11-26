#define MAP_QUALITY_ENABLE_TEST_ACCESS
#include "SeriesMapEdgeMatchTaskDialog.h"
#include "SeriesCrossScaleExpressionTaskDialog.h"
#include "series_mapping_tree_groups.h"
#include "searchable_layer_combo_box.h"
#include <qgsapplication.h>
#include <qgsvectorlayer.h>
#include <QApplication>
#include <QTimer>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QPushButton>
#include <iostream>
#include <cstdlib>
#define REQUIRE(x) do { if(!(x)) { std::cerr << "FAIL " << __LINE__ << ": " << #x << std::endl; std::exit(1); } } while(0)
static QString base=QStringLiteral("D:/Desktop/LTZK/图幅接边与跨比例尺检查");
static QString dir(const QString& name) {return base+QLatin1Char('/')+name+QLatin1Char('/')+name+QStringLiteral("_shp");}
static void inspectCandidateOrder(QComboBox* combo) {
    REQUIRE(combo);const QString selected=combo->currentData().toString();
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(5,5),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(combo,&press);QApplication::processEvents();
    auto* view=combo->findChild<QListView*>(QStringLiteral("layerSearchResults"));
    auto* search=combo->findChild<QLineEdit*>(QStringLiteral("layerSearchEdit"));REQUIRE(view&&search);
    REQUIRE(view->model()->rowCount()==combo->count());REQUIRE(view->model()->index(0,0).data(Qt::UserRole).toString().isEmpty());
    QCollator collator(QLocale(QLocale::Chinese,QLocale::China));collator.setCaseSensitivity(Qt::CaseInsensitive);collator.setNumericMode(true);
    for(int i=2;i<view->model()->rowCount();++i){
        const QString previous=view->model()->index(i-1,0).data().toString(),current=view->model()->index(i,0).data().toString();
        REQUIRE(collator.compare(QFileInfo(previous).completeBaseName(),QFileInfo(current).completeBaseName())<=0);
    }
    REQUIRE(combo->currentData().toString()==selected);
    search->setText(QStringLiteral("道"));QApplication::processEvents();
    for(int i=0;i<view->model()->rowCount();++i)REQUIRE(view->model()->index(i,0).data().toString().contains(QStringLiteral("道")));
    REQUIRE(combo->currentData().toString()==selected);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(search,&escape);QApplication::processEvents();
    REQUIRE(combo->currentData().toString()==selected);
}
static QSet<QString> inspectRows(QTreeWidget* tree,int labelCol,int leftCol,int rightCol,int rightRole) {
    auto* task=tree->topLevelItem(0); QSet<QString> selected;
    auto rows=SeriesMappingTreeGroups::mappingRows(task); REQUIRE(!rows.isEmpty());
    for(auto* row:rows) {
        QString left=row->data(0,Qt::UserRole+3).toString(),right=row->data(0,rightRole).toString();
        const QString name=SeriesMappingTreeGroups::detailName(left,right);
        REQUIRE(row->text(labelCol)==name || (left.isEmpty()&&right.isEmpty()));
        REQUIRE(tree->itemWidget(row,leftCol) && tree->itemWidget(row,rightCol));
        if(row->checkState(0)==Qt::Checked) {
            REQUIRE(!left.isEmpty()&&!right.isEmpty());
            QgsVectorLayer l(left,QStringLiteral("l"),QStringLiteral("ogr")),r(right,QStringLiteral("r"),QStringLiteral("ogr"));
            REQUIRE(l.isValid() && r.isValid()); REQUIRE(l.geometryType()==r.geometryType());
            selected.insert(row->data(0,Qt::UserRole+2).toString()+QLatin1Char('|')+left+QLatin1Char('|')+right);
        }
    }
    REQUIRE(!selected.isEmpty()); return selected;
}
static QSet<QString> savedRows(const QJsonObject& mapping) {
    QSet<QString> rows;
    auto layers=mapping.value(QStringLiteral("layers")).toObject();
    for(auto it=layers.begin();it!=layers.end();++it) for(auto rv:it.value().toObject().value(QStringLiteral("rows")).toArray()) {
        auto row=rv.toObject();auto l=row.value(QStringLiteral("left")).toArray(),r=row.value(QStringLiteral("right")).toArray();
        REQUIRE(l.size()==1 && r.size()==1);
        rows.insert(it.key()+QLatin1Char('|')+l.first().toString()+QLatin1Char('|')+r.first().toString());
    }
    return rows;
}
struct SeriesMapEdgeMatchDialogTest {
 static void run(bool cross) {
    SeriesMapEdgeMatchTaskDialog owner;
    owner.m_xmlEdit->setText(QStringLiteral("D:/Desktop/挂图XML文件汇总_11.4/501图幅接边与系列化表达.xml"));
    QMap<QString,QString> dirs{{QStringLiteral("皋兰县"),dir(QStringLiteral("皋兰县"))},{QStringLiteral("永登县"),dir(QStringLiteral("永登县"))}};
    QMap<QString,QString> mappings;QSet<QString> expected;bool inspected=false;
    QTimer::singleShot(0,[&](){
        auto* dlg=qobject_cast<QDialog*>(QApplication::activeModalWidget()); REQUIRE(dlg);
        auto* tree=dlg->findChild<QTreeWidget*>();REQUIRE(tree && tree->topLevelItemCount()==1);
        auto* task=tree->topLevelItem(0);REQUIRE(task->childCount()==4);
        QSet<QString> cats;QTreeWidgetItem* water=nullptr;
        for(int i=0;i<task->childCount();++i) {auto* c=task->child(i);cats.insert(c->text(1));if(c->text(1)==QStringLiteral("水系面")) water=c;}
        REQUIRE(cats==QSet<QString>({QStringLiteral("境界线"),QStringLiteral("道路"),QStringLiteral("水系线"),QStringLiteral("水系面")}));
        REQUIRE(water);int riverGroups=0,lakeGroups=0;
        for(int i=0;i<water->childCount();++i) {
            auto* c=water->child(i);
            if(c->text(1)==QStringLiteral("河流面")){++riverGroups;REQUIRE(c->childCount()>=2);c->setExpanded(true);}
            if(c->text(1)==QStringLiteral("湖泊水库")){++lakeGroups;REQUIRE(c->childCount()>=2);c->setExpanded(true);}
        }
        REQUIRE(riverGroups==1 && lakeGroups==1);
        expected=inspectRows(tree,1,2,3,Qt::UserRole+4);
        auto* existing=SeriesMappingTreeGroups::mappingRows(task).first();tree->setCurrentItem(existing);
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(existing,2)));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(existing,3)));
        const int oldCount=SeriesMappingTreeGroups::mappingRows(task).size();
        dlg->findChild<QPushButton*>(QStringLiteral("addEdgeMatchMappingRowButton"))->click();
        REQUIRE(SeriesMappingTreeGroups::mappingRows(task).size()==oldCount+1);
        auto* added=tree->currentItem();REQUIRE(added->checkState(0)==Qt::Unchecked);REQUIRE(tree->itemWidget(added,2)&&tree->itemWidget(added,3));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(added,2)));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(added,3)));
        if(cross){task->child(1)->setExpanded(false);task->child(2)->setExpanded(false);dlg->resize(1250,750);QApplication::processEvents();REQUIRE(dlg->grab().save(QStringLiteral("501_grouping_preview.png")));}
        inspected=true;dlg->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    if(cross) {
        SeriesMapEdgeMatchTaskDialog::CoverageGroup group;group.smallScaleName=QStringLiteral("永登县");group.largeScaleNames=QStringList{QStringLiteral("皋兰县")};group.displayName=QStringLiteral("测试：皋兰县—永登县");group.largeScale=110000;group.smallScale=300000;
        REQUIRE(owner.confirmSeriesLayerMappings({}, {group},dirs,110000,&mappings));
    } else {
        SeriesMapEdgeMatchTaskDialog::AdjacentPair pair;pair.leftName=QStringLiteral("皋兰县");pair.rightName=QStringLiteral("永登县");
        REQUIRE(owner.confirmSeriesLayerMappings({pair},{},dirs,110000,&mappings));
    }
    REQUIRE(inspected && mappings.size()==1);REQUIRE(savedRows(QJsonDocument::fromJson(mappings.first().toUtf8()).object())==expected);
    std::cout << "PASS 501 " << (cross?"cross-scale":"same-scale") << ": 4 headers, river/lake groups, full filenames, typed matches, add row, exact saved relations=" << expected.size() << std::endl;
 }
};
struct SeriesCrossScaleExpressionTaskDialogTestAccess {
 static void run() {
    SeriesCrossScaleExpressionTaskDialog owner;owner.m_xmlEdit->setText(QStringLiteral("D:/Desktop/挂图XML文件汇总_11.4/502同系列跨比例尺表达检查.xml"));
    SeriesCrossScaleExpressionTaskDialog::MapTask task;task.pairName=QStringLiteral("测试：皋兰县—永登县");task.largeScaleName=QStringLiteral("皋兰县");task.smallScaleName=QStringLiteral("永登县");task.largeShpDir=dir(task.largeScaleName);task.smallShpDir=dir(task.smallScaleName);task.largeScale=110000;task.smallScale=300000;
    QMap<QString,QJsonObject> mappings;QSet<QString> expected;bool inspected=false;
    QTimer::singleShot(0,[&](){
        auto* dlg=qobject_cast<QDialog*>(QApplication::activeModalWidget());REQUIRE(dlg);
        auto* tree=dlg->findChild<QTreeWidget*>();REQUIRE(tree);auto* root=tree->topLevelItem(0);REQUIRE(root->childCount()>1);
        for(int i=0;i<root->childCount();++i)REQUIRE(root->child(i)->data(0,Qt::UserRole).toString()==QStringLiteral("CategoryGroupRow"));
        expected=inspectRows(tree,0,1,2,Qt::UserRole+5);
        auto* existing=SeriesMappingTreeGroups::mappingRows(root).first();tree->setCurrentItem(existing);int before=SeriesMappingTreeGroups::mappingRows(root).size();
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(existing,1)));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(existing,2)));
        dlg->findChild<QPushButton*>(QStringLiteral("addCrossScaleMappingRowButton"))->click();
        REQUIRE(SeriesMappingTreeGroups::mappingRows(root).size()==before+1);auto* added=tree->currentItem();REQUIRE(tree->itemWidget(added,1)&&tree->itemWidget(added,2));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(added,1)));
        inspectCandidateOrder(qobject_cast<QComboBox*>(tree->itemWidget(added,2)));
        dlg->resize(1250,750);QApplication::processEvents();REQUIRE(dlg->grab().save(QStringLiteral("502_grouping_preview.png")));
        inspected=true;dlg->accept();
    });
    REQUIRE(owner.confirmLayerMappings({task},&mappings));REQUIRE(inspected && mappings.size()==1);REQUIRE(savedRows(mappings.first())==expected);
    std::cout << "PASS 502: grouped headers, original filenames, typed matches, add row, recursive saved relations=" << expected.size() << std::endl;
 }
};
int main(int argc,char** argv) {
    QgsApplication::setPrefixPath(QStringLiteral("F:/LTZK/QGIS_32815"),true);
    QgsApplication app(argc,argv,true);QgsApplication::initQgis();
    const int font=QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));REQUIRE(font>=0);app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(),10));
    QTimer watchdog;watchdog.setSingleShot(true);QObject::connect(&watchdog,&QTimer::timeout,[](){std::cerr<<"FAIL dialog timeout"<<std::endl;std::exit(2);});watchdog.start(45000);
    SeriesMapEdgeMatchDialogTest::run(false);SeriesMapEdgeMatchDialogTest::run(true);SeriesCrossScaleExpressionTaskDialogTestAccess::run();
    QgsApplication::exitQgis();return 0;
}
