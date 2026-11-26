#define MAP_QUALITY_ENABLE_TEST_ACCESS
#include "SeriesMapEdgeMatchTaskDialog.h"
#include "SeriesCrossScaleExpressionTaskDialog.h"
#include "quality_result_output.h"
#include <qgsapplication.h>
#include <qgsvectorfilewriter.h>
#include <QDateTime>
#include <QJsonDocument>
#include <QTextEdit>
#include <QTimer>
#include <QMessageBox>
#include <iostream>
#include <cstdlib>
#define REQUIRE(x) do {if(!(x)){std::cerr<<"FAIL "<<__LINE__<<": "<<#x<<std::endl;std::exit(1);}} while(0)
static QString runRoot,sourcePath,xml501,xml502;
static QJsonObject readJson(const QString& path){QFile f(path);REQUIRE(f.open(QIODevice::ReadOnly));auto d=QJsonDocument::fromJson(f.readAll());REQUIRE(d.isObject());return d.object();}
static void writeText(const QString& path,const QString& text){QFile f(path);REQUIRE(f.open(QIODevice::WriteOnly));REQUIRE(f.write(text.toUtf8())>=0);}
static QgsVectorLayer* onlyLayer(){auto layers=QgsProject::instance()->mapLayers();REQUIRE(layers.size()==1);auto* layer=qobject_cast<QgsVectorLayer*>(layers.first());REQUIRE(layer&&layer->isValid());REQUIRE(layer->featureCount()==1);return layer;}
static void checkTree(QgsVectorLayer* layer,const QString& map,const QString& severity=QString()){
 auto* node=QgsProject::instance()->layerTreeRoot()->findLayer(layer->id());REQUIRE(node);auto* group=dynamic_cast<QgsLayerTreeGroup*>(node->parent());REQUIRE(group);
 if(!severity.isEmpty()){REQUIRE(group->name()==severity+QStringLiteral("严重程度"));group=dynamic_cast<QgsLayerTreeGroup*>(group->parent());REQUIRE(group);}
 REQUIRE(group->name()==QStringLiteral("质检错误结果"));REQUIRE(dynamic_cast<QgsLayerTreeGroup*>(group->parent())->name()==map);
}
struct SeriesMapEdgeMatchDialogTest {
 static void run(){
  SeriesMapEdgeMatchTaskDialog owner;const QString root=runRoot+QStringLiteral("/501");owner.m_outputDirEdit->setText(root);owner.m_xmlEdit->setText(xml501);owner.m_indexFileEdit->setText(sourcePath);
  const QString map=QStringLiteral("甲县");owner.m_checkedLayersByMap[map].insert(QStringLiteral("道路"));
  QJsonObject error{{QStringLiteral("decision_status"),QStringLiteral("confirmed_error")},{QStringLiteral("error_type"),QStringLiteral("line_edge_mismatch")},{QStringLiteral("sub_rule"),QStringLiteral("线要素接边容差")},{QStringLiteral("left_sheet"),map},{QStringLiteral("right_sheet"),QStringLiteral("乙县")},{QStringLiteral("layer"),QStringLiteral("道路")},{QStringLiteral("left_shp_path"),sourcePath},{QStringLiteral("left_shp"),QStringLiteral("国道.shp")},{QStringLiteral("left_fid"),QStringLiteral("0")},{QStringLiteral("description"),QStringLiteral("测试确认错误")}};
  owner.m_lastErrorRecords.push_back(error);
  REQUIRE(owner.writeCurrentOutputs());auto* layer=onlyLayer();checkTree(layer,map,QStringLiteral("高"));const QString path=layer->source();REQUIRE(path.contains(QStringLiteral("质检错误结果/高严重程度")));
  const QString itemPath=root+QStringLiteral("/甲县/质检Json文件/图幅接边与系列化表达.json");
  auto item=readJson(itemPath).value(QStringLiteral("check_item1")).toObject();REQUIRE(item.value(QStringLiteral("severity")).toString()==QStringLiteral("高"));REQUIRE(item.value(QStringLiteral("info")).toObject().value(QStringLiteral("auto_loaded_layer_count")).toInt()==1);REQUIRE(item.value(QStringLiteral("info")).toObject().value(QStringLiteral("error_line_shp")).toString()==path);
  QStringList failures;REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(),{path,path},&failures)==1);REQUIRE(failures.isEmpty());REQUIRE(QgsProject::instance()->mapLayers().size()==1);
  REQUIRE(owner.writeCurrentOutputs());onlyLayer();REQUIRE(QgsProject::instance()->mapLayers().size()==1);
  owner.m_lastErrorRecords.clear();REQUIRE(owner.writeCurrentOutputs());REQUIRE(QgsProject::instance()->mapLayers().isEmpty());REQUIRE(!QFileInfo::exists(path));
  item=readJson(itemPath).value(QStringLiteral("check_item1")).toObject();REQUIRE(item.value(QStringLiteral("info")).toObject().value(QStringLiteral("auto_loaded_layer_count")).toInt()==0);REQUIRE(item.value(QStringLiteral("errors")).toObject().isEmpty());
  std::cout<<"PASS 501: export and load, configured severity, JSON paths, grouping, deduplication, overwrite, and zero-error cleanup"<<std::endl;
 }
 static QJsonObject itemFor(const QString& root,const QString& map){
  return readJson(root+QLatin1Char('/')+map+QStringLiteral("/质检Json文件/图幅接边与系列化表达.json")).value(QStringLiteral("check_item1")).toObject();
 }
 static int reportErrors(const QJsonObject& object){
  int count=0;for(auto it=object.constBegin();it!=object.constEnd();++it){
   if(it.key()==QStringLiteral("errors"))count+=it.value().toObject().size();
   else if(it.value().isObject())count+=reportErrors(it.value().toObject());
  }return count;
 }
 static void runMainOnly(){
  SeriesMapEdgeMatchTaskDialog owner;const QString root=runRoot+QStringLiteral("/501_main_only");
  owner.m_outputDirEdit->setText(root);owner.m_xmlEdit->setText(xml501);owner.m_indexFileEdit->setText(sourcePath);
  const QString main=QStringLiteral("甲县"),peer=QStringLiteral("乙县");
  owner.m_checkedLayersByMap[main].insert(QStringLiteral("道路"));owner.m_checkedLayersByMap[peer].insert(QStringLiteral("道路"));
  QJsonObject error{{QStringLiteral("decision_status"),QStringLiteral("confirmed_error")},{QStringLiteral("error_type"),QStringLiteral("line_edge_mismatch")},{QStringLiteral("sub_rule"),QStringLiteral("线要素接边容差")},{QStringLiteral("left_sheet"),peer},{QStringLiteral("right_sheet"),main},{QStringLiteral("layer"),QStringLiteral("道路")},{QStringLiteral("left_shp_path"),sourcePath},{QStringLiteral("left_shp"),QStringLiteral("国道.shp")},{QStringLiteral("left_fid"),QStringLiteral("0")},{QStringLiteral("right_shp_path"),sourcePath},{QStringLiteral("right_shp"),QStringLiteral("国道.shp")},{QStringLiteral("right_fid"),QStringLiteral("0")},{QStringLiteral("description"),QStringLiteral("双侧要素接边测试")}};
  // Seed an already-loaded peer output so the next run must clear old duplicates.
  owner.m_lastErrorRecords={error};REQUIRE(owner.writeCurrentOutputs());
  auto* seeded=onlyLayer();checkTree(seeded,peer,QStringLiteral("高"));const QString oldPeerPath=seeded->source();
  error[QStringLiteral("left_sheet")]=main;error[QStringLiteral("right_sheet")]=peer;
  error[QStringLiteral("pair_name")]=QStringLiteral("甲县 - 乙县");
  error[QStringLiteral("left_x")]=0.;error[QStringLiteral("left_y")]=0.;error[QStringLiteral("right_x")]=5.;error[QStringLiteral("right_y")]=5.;
  owner.m_pairRunContext[QStringLiteral("甲县 - 乙县")]=QJsonObject{{QStringLiteral("left_name"),main},{QStringLiteral("right_name"),peer},{QStringLiteral("result_status"),QStringLiteral("质检成功")},{QStringLiteral("result_err_exist"),true}};
  QJsonObject review=error;review[QStringLiteral("decision_status")]=QStringLiteral("needs_review");review[QStringLiteral("error_type")]=QStringLiteral("unmatched_review");
  owner.m_originalErrorRecords={review};owner.m_lastErrorRecords={error};REQUIRE(owner.writeCurrentOutputs());
  REQUIRE(!QFileInfo::exists(oldPeerPath));REQUIRE(QgsProject::instance()->mapLayers().size()==2);
  for(QgsMapLayer* layer:QgsProject::instance()->mapLayers()){
   auto* vector=qobject_cast<QgsVectorLayer*>(layer);REQUIRE(vector&&vector->isValid()&&vector->featureCount()==1);checkTree(vector,main,QStringLiteral("高"));
   if(vector->fields().indexFromName(QStringLiteral("src_sheet"))>=0){QgsFeature f;REQUIRE(vector->getFeatures().nextFeature(f));REQUIRE(f.attribute(QStringLiteral("src_sheet")).toString()==main);}
  }
  auto mainItem=itemFor(root,main),peerItem=itemFor(root,peer);
  REQUIRE(mainItem.value(QStringLiteral("errors")).toObject().size()==1);REQUIRE(mainItem.value(QStringLiteral("review_errors")).toObject().size()==1);
  const auto record=mainItem.value(QStringLiteral("errors")).toObject().begin().value().toObject();
  REQUIRE(record.value(QStringLiteral("output_map")).toString()==main);REQUIRE(record.value(QStringLiteral("right_sheet")).toString()==peer);REQUIRE(record.value(QStringLiteral("right_fid")).toString()==QStringLiteral("0"));
  REQUIRE(mainItem.value(QStringLiteral("info")).toObject().value(QStringLiteral("error_line_count")).toInt()==1);REQUIRE(mainItem.value(QStringLiteral("info")).toObject().value(QStringLiteral("error_link_count")).toInt()==1);
  REQUIRE(peerItem.value(QStringLiteral("errors")).toObject().isEmpty());REQUIRE(peerItem.value(QStringLiteral("review_errors")).toObject().isEmpty());REQUIRE(!peerItem.value(QStringLiteral("err_exist")).toBool());
  REQUIRE(peerItem.value(QStringLiteral("info")).toObject().value(QStringLiteral("auto_loaded_layer_count")).toInt()==0);REQUIRE(peerItem.value(QStringLiteral("info")).toObject().value(QStringLiteral("error_pair_count")).toInt()==0);
  for(const QString& map:QStringList{main,peer}){
   QDir dir(root+QLatin1Char('/')+map);const auto totals=dir.entryList({QStringLiteral("*_result_*.json")},QDir::Files);REQUIRE(totals.size()==1);
   REQUIRE(reportErrors(readJson(dir.filePath(totals.first())))==(map==main?1:0));
  }
  REQUIRE(owner.writeCurrentOutputs());REQUIRE(QgsProject::instance()->mapLayers().size()==2);
  // Main-map geometry is missing: retain a single locating point under the main.
  error.remove(QStringLiteral("left_shp_path"));error.remove(QStringLiteral("left_fid"));
  for(const QString& key:QStringList{QStringLiteral("left_x"),QStringLiteral("left_y"),QStringLiteral("right_x"),QStringLiteral("right_y")})error.remove(key);
  owner.m_originalErrorRecords.clear();owner.m_lastErrorRecords={error};REQUIRE(owner.writeCurrentOutputs());
  auto* marker=onlyLayer();checkTree(marker,main,QStringLiteral("高"));REQUIRE(marker->geometryType()==QgsWkbTypes::PointGeometry);
  QgsFeature feature;REQUIRE(marker->getFeatures().nextFeature(feature));REQUIRE(feature.attribute(QStringLiteral("src_sheet")).toString()==main);
  REQUIRE(itemFor(root,main).value(QStringLiteral("errors")).toObject().size()==1);REQUIRE(itemFor(root,peer).value(QStringLiteral("errors")).toObject().isEmpty());
  // Embedded cross-scale records retain their existing two-map export behavior.
  error[QStringLiteral("left_shp_path")]=sourcePath;error[QStringLiteral("left_fid")]=QStringLiteral("0");error[QStringLiteral("small_scale_sheet")]=peer;
  owner.m_lastErrorRecords={error};REQUIRE(owner.writeCurrentOutputs());REQUIRE(QgsProject::instance()->mapLayers().size()==2);
  REQUIRE(itemFor(root,main).value(QStringLiteral("errors")).toObject().size()==1);REQUIRE(itemFor(root,peer).value(QStringLiteral("errors")).toObject().size()==1);
  owner.m_lastErrorRecords.clear();REQUIRE(owner.writeCurrentOutputs());REQUIRE(QgsProject::instance()->mapLayers().isEmpty());
  std::cout<<"PASS 501 main-only: one relationship under configured main, peer metadata retained, main-only links/JSON/reviews, peer file and loaded-layer cleanup, single-sided fallback, cross-scale behavior preserved"<<std::endl;
 }
};
struct SeriesCrossScaleExpressionTaskDialogTestAccess {
 static void run(){
  SeriesCrossScaleExpressionTaskDialog owner;const QString root=runRoot+QStringLiteral("/502");owner.m_outputDirEdit->setText(root);owner.m_xmlEdit->setText(xml502);
  const QString map=QStringLiteral("乙县");
  QJsonObject task{{QStringLiteral("pair_name"),QStringLiteral("甲县—乙县")},{QStringLiteral("large_scale_map"),QStringLiteral("甲县")},{QStringLiteral("small_scale_map"),map},{QStringLiteral("large_scale"),110000},{QStringLiteral("small_scale"),300000},{QStringLiteral("execution_state"),QStringLiteral("completed")},{QStringLiteral("check_crs"),QgsCoordinateReferenceSystem(3857).toWkt()},{QStringLiteral("layer_summaries"),QJsonArray{QJsonObject{{QStringLiteral("target_layer"),QStringLiteral("道路")},{QStringLiteral("execution_state"),QStringLiteral("completed")}}}}};
  QJsonObject warning{{QStringLiteral("target_layer"),QStringLiteral("道路")},{QStringLiteral("decision_status"),QStringLiteral("confirmed_error")},{QStringLiteral("error_type"),QStringLiteral("cross_scale_missing_support")},{QStringLiteral("small_shp"),sourcePath},{QStringLiteral("small_feature_id"),QStringLiteral("0")},{QStringLiteral("description"),QStringLiteral("测试确认错误")}};
  task[QStringLiteral("warnings")]=QJsonArray{warning};QString details;
  REQUIRE(owner.writeOutputs({task},&details));REQUIRE(details.isEmpty());auto* layer=onlyLayer();checkTree(layer,map);const QString path=layer->source();REQUIRE(path.contains(QStringLiteral("质检错误结果")));REQUIRE(!path.contains(QStringLiteral("/质检结果/")));REQUIRE(QFileInfo::exists(QFileInfo(path).path()+QLatin1Char('/')+QFileInfo(path).completeBaseName()+QStringLiteral(".qml")));
  const QString itemPath=root+QStringLiteral("/乙县/质检Json文件/同系列跨比例尺表达检查.json");auto item=readJson(itemPath).value(QStringLiteral("check_item1")).toObject();REQUIRE(item.value(QStringLiteral("severity")).toString()==QStringLiteral("未配置"));REQUIRE(item.value(QStringLiteral("info")).toObject().value(QStringLiteral("error_line_shp")).toString()==path);REQUIRE(item.value(QStringLiteral("info")).toObject().value(QStringLiteral("auto_loaded_layer_count")).toInt()==1);
  // Re-running must replace a loaded SHP and retain its readable attribute style.
  REQUIRE(owner.writeOutputs({task},&details));layer=onlyLayer();REQUIRE(layer->fields().indexFromName(QStringLiteral("status"))>=0);REQUIRE(layer->attributeAlias(layer->fields().indexFromName(QStringLiteral("status")))==QStringLiteral("内部状态代码"));
  warning[QStringLiteral("decision_status")]=QStringLiteral("needs_review");task[QStringLiteral("warnings")]=QJsonArray{warning};REQUIRE(owner.writeOutputs({task},&details));onlyLayer();item=readJson(itemPath).value(QStringLiteral("check_item1")).toObject();auto info=item.value(QStringLiteral("info")).toObject();REQUIRE(info.value(QStringLiteral("confirmed_error_output_count")).toInt()==0);REQUIRE(info.value(QStringLiteral("needs_review_output_count")).toInt()==1);
  task[QStringLiteral("warnings")]=QJsonArray();REQUIRE(owner.writeOutputs({task},&details));REQUIRE(QgsProject::instance()->mapLayers().isEmpty());REQUIRE(!QFileInfo::exists(path));
  std::cout<<"PASS 502: unified result folder, automatic load, JSON/QML paths and field aliases, overwrite, review status preserved, zero-result cleanup"<<std::endl;
 }
};
int main(int argc,char** argv){
 runRoot=QDir::currentPath()+QStringLiteral("/run_")+QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));REQUIRE(QDir().mkpath(runRoot));
 QgsApplication::setPrefixPath(QStringLiteral("F:/LTZK/QGIS_32815"),true);QgsApplication app(argc,argv,true,runRoot+QStringLiteral("/qgis_profile"));QgsApplication::initQgis();
 QTimer modalGuard;QObject::connect(&modalGuard,&QTimer::timeout,[](){for(QWidget* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget)){std::cerr<<"FAIL modal dialog: "<<box->text().toUtf8().constData()<<std::endl;std::exit(1);}});modalGuard.start(500);
 sourcePath=runRoot+QStringLiteral("/国道.shp");
 {QgsFields fields;fields.append(QgsField(QStringLiteral("name"),QVariant::String));QgsVectorFileWriter writer(sourcePath,QStringLiteral("UTF-8"),fields,QgsWkbTypes::LineString,QgsCoordinateReferenceSystem(3857),QStringLiteral("ESRI Shapefile"));REQUIRE(writer.hasError()==QgsVectorFileWriter::NoError);QgsFeature f(fields);f.setGeometry(QgsGeometry::fromWkt(QStringLiteral("LINESTRING(0 0, 100 100)")));f.setAttribute(0,QStringLiteral("国道"));REQUIRE(writer.addFeature(f));}
 xml501=runRoot+QStringLiteral("/501.xml");xml502=runRoot+QStringLiteral("/502.xml");writeText(xml501,QStringLiteral("<check_list><scale><check_item><id>501</id><name>图幅接边与系列化表达</name><severity>高</severity></check_item></scale></check_list>"));writeText(xml502,QStringLiteral("<check_list><scale><check_item><id>502</id><name>同系列跨比例尺表达检查</name><rule><name>series_cross_scale_output_severity</name><value>预警</value></rule></check_item></scale></check_list>"));
 REQUIRE(QualityResultOutput::severityFromXml(xml502,QStringLiteral("502"))==QStringLiteral("未配置"));REQUIRE(QualityResultOutput::directory(runRoot,QStringLiteral("预警"))==runRoot+QStringLiteral("/质检错误结果"));
 REQUIRE(QgsProject::instance()->mapLayers().isEmpty());QStringList failures;REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(),{},&failures)==0);REQUIRE(failures.isEmpty());REQUIRE(QgsProject::instance()->layerTreeRoot()->children().isEmpty());
 SeriesMapEdgeMatchDialogTest::run();SeriesMapEdgeMatchDialogTest::runMainOnly();SeriesCrossScaleExpressionTaskDialogTestAccess::run();
 REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(),{runRoot+QStringLiteral("/missing.shp")},&failures)==0);REQUIRE(failures.size()==1);
 std::cout<<"PASS shared loader: no empty placeholders, failed-path reporting, explicit-path loading"<<std::endl;QgsApplication::exitQgis();return 0;
}
