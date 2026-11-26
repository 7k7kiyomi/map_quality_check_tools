#define MAP_QUALITY_ENABLE_TEST_ACCESS
#include "SeriesMapEdgeMatchTaskDialog.h"
#include "SeriesCrossScaleExpressionTaskDialog.h"
#include "quality_result_output.h"
#include <qgsapplication.h>
#include <qgsvectorfilewriter.h>
#include <QDateTime>
#include <QJsonDocument>
#include <QTextEdit>
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
 sourcePath=runRoot+QStringLiteral("/国道.shp");
 {QgsFields fields;fields.append(QgsField(QStringLiteral("name"),QVariant::String));QgsVectorFileWriter writer(sourcePath,QStringLiteral("UTF-8"),fields,QgsWkbTypes::LineString,QgsCoordinateReferenceSystem(3857),QStringLiteral("ESRI Shapefile"));REQUIRE(writer.hasError()==QgsVectorFileWriter::NoError);QgsFeature f(fields);f.setGeometry(QgsGeometry::fromWkt(QStringLiteral("LINESTRING(0 0, 100 100)")));f.setAttribute(0,QStringLiteral("国道"));REQUIRE(writer.addFeature(f));}
 xml501=runRoot+QStringLiteral("/501.xml");xml502=runRoot+QStringLiteral("/502.xml");writeText(xml501,QStringLiteral("<check_list><scale><check_item><id>501</id><name>图幅接边与系列化表达</name><severity>高</severity></check_item></scale></check_list>"));writeText(xml502,QStringLiteral("<check_list><scale><check_item><id>502</id><name>同系列跨比例尺表达检查</name><rule><name>series_cross_scale_output_severity</name><value>预警</value></rule></check_item></scale></check_list>"));
 REQUIRE(QualityResultOutput::severityFromXml(xml502,QStringLiteral("502"))==QStringLiteral("未配置"));REQUIRE(QualityResultOutput::directory(runRoot,QStringLiteral("预警"))==runRoot+QStringLiteral("/质检错误结果"));
 REQUIRE(QgsProject::instance()->mapLayers().isEmpty());QStringList failures;REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(),{},&failures)==0);REQUIRE(failures.isEmpty());REQUIRE(QgsProject::instance()->layerTreeRoot()->children().isEmpty());
 SeriesMapEdgeMatchDialogTest::run();SeriesCrossScaleExpressionTaskDialogTestAccess::run();
 REQUIRE(QualityResultOutput::loadLayers(QgsProject::instance(),{runRoot+QStringLiteral("/missing.shp")},&failures)==0);REQUIRE(failures.size()==1);
 std::cout<<"PASS shared loader: no empty placeholders, failed-path reporting, explicit-path loading"<<std::endl;QgsApplication::exitQgis();return 0;
}
