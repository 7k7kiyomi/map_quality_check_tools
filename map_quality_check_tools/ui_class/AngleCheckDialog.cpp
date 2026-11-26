#define _HAS_STD_BYTE 0
#include "AngleCheckDialog.h"
#include "GeometryValidator.h"
#include "JsonLogger.h"
#include "XmlConfigReader.h"

#include <qgis.h>
#include <qgsabstractgeometry.h>
#include <qgspointxy.h>
#include <qgsvectorlayer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgscurve.h>
#include <qgspolygon.h>
#include <qgslinestring.h>
#include <qgsvectorfilewriter.h>
#include <qgsgeometryutils.h>
#include <qgswkbtypes.h>

#include <QTextStream>
#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QDebug>

#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <stdexcept>

#include <QDomDocument>
#include <QFile>
#include <QRegularExpression>
#include <QMap>
#include <gdal_priv.h>
#include <ogrsf_frmts.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include <set>


namespace SharpAngleSourceDetail {
void require(bool yes,const QString& text){if(!yes)throw std::runtime_error(text.toUtf8().constData());}
struct DD{void operator()(GDALDataset* p)const{if(p)GDALClose(p);}};
struct GD{void operator()(OGRGeometry* p)const{if(p)OGRGeometryFactory::destroyGeometry(p);}};
struct FD{void operator()(OGRFeature* p)const{if(p)OGRFeature::DestroyFeature(p);}};
struct TD{void operator()(OGRCoordinateTransformation* p)const{if(p)OCTDestroyCoordinateTransformation(p);}};
using DS=std::unique_ptr<GDALDataset,DD>;using Geom=std::unique_ptr<OGRGeometry,GD>;
using Feat=std::unique_ptr<OGRFeature,FD>;
struct Condition{QString field,op,value;};
QStringList split(QString s){s.replace(QChar(0xff1b),';');QStringList out;for(auto x:s.split(';',Qt::SkipEmptyParts))if(!x.trimmed().isEmpty())out<<x.trimmed();return out;}
DS open(const QString& path){DS d(static_cast<GDALDataset*>(GDALOpenEx(path.toUtf8().constData(),GDAL_OF_VECTOR|GDAL_OF_READONLY,nullptr,nullptr,nullptr)));require(bool(d),"无法打开图层："+path);return d;}
bool passes(OGRFeature* f,const Condition& c){
 int i=f->GetFieldIndex(c.field.toUtf8().constData());require(i>=0,"源数据缺少筛选字段："+c.field);
 bool empty=!f->IsFieldSetAndNotNull(i)||QString::fromUtf8(f->GetFieldAsString(i)).trimmed().isEmpty();
 if(c.value=="为空")return empty;if(c.value=="不为空")return !empty;if(empty)return false;
 QString actual=QString::fromUtf8(f->GetFieldAsString(i));bool hit=false;
 for(const auto& value:split(c.value)){
  if(c.op=="包含"){hit=hit||actual.contains(value);continue;}
  auto type=f->GetDefnRef()->GetFieldDefn(i)->GetType();bool ok=false;
  if(type==OFTInteger||type==OFTInteger64){auto v=value.toLongLong(&ok);require(ok,"整数字段筛选值不合法："+c.field);hit=hit||(f->GetFieldAsInteger64(i)==v);}
  else if(type==OFTReal){double v=value.toDouble(&ok);require(ok&&std::isfinite(v),"数值字段筛选值不合法："+c.field);hit=hit||(f->GetFieldAsDouble(i)==v);}
  else hit=hit||actual==value;
 }
 return hit;
}
struct Source{Geom g;bool selected;OGREnvelope box;Geom buffer;};
double lineLength(const OGRGeometry* g){
 if(!g||g->IsEmpty())return 0;
 auto type=wkbFlatten(g->getGeometryType());if(type==wkbLineString)return g->toLineString()->get_Length();
 if(type==wkbMultiLineString||type==wkbGeometryCollection){double length=0;auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)length+=lineLength(c->getGeometryRef(i));return length;}
 return 0;
}
void addPolygons(const OGRGeometry* g,OGRMultiPolygon& out){
 if(wkbFlatten(g->getGeometryType())==wkbPolygon){out.addGeometry(g);return;}
 auto type=wkbFlatten(g->getGeometryType());require(type==wkbMultiPolygon||type==wkbGeometryCollection,"源河流缓冲区不是面");
 auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)addPolygons(c->getGeometryRef(i),out);
}
void lines(const OGRGeometry* g,std::vector<const OGRLineString*>& out){
 auto type=wkbFlatten(g->getGeometryType());
 if(type==wkbLineString){out.push_back(g->toLineString());return;}
 if(type==wkbMultiLineString){auto* c=g->toGeometryCollection();for(int i=0;i<c->getNumGeometries();++i)lines(c->getGeometryRef(i),out);return;}
 require(false,"启用源数据筛选时仅支持线要素，不能用点或面替代河流线关联");
}
}

AngleCheckDialog::SourceSelection AngleCheckDialog::selectSourceFeatures(const QString& parsedPath,const QString& sourcePaths,
 const QString& xmlPath,int scale,double toleranceMm){
 using namespace SharpAngleSourceDetail;
 SourceSelection result;QFile f(xmlPath);QDomDocument doc;
 require(f.open(QIODevice::ReadOnly)&&doc.setContent(&f),"无法解析110规则XML");
 QDomElement exact,generic;auto scales=doc.elementsByTagName("scale");
 for(int i=0;i<scales.size();++i){auto s=scales.at(i).toElement();auto value=s.firstChildElement("value").text().trimmed();
  bool match=!value.isEmpty()&&value.toInt()==scale;if(!value.isEmpty()&&!match)continue;
  auto items=s.elementsByTagName("check_item");for(int j=0;j<items.size();++j){auto item=items.at(j).toElement();
   if(item.firstChildElement("id").text().trimmed()!="110"&&item.firstChildElement("name").text().trimmed()!="削尖角度阈值控制")continue;
   auto& target=match?exact:generic;require(target.isNull(),"当前比例尺存在多条110配置，请为本任务单独提供XML");target=item;}}
 auto item=exact.isNull()?generic:exact;require(!item.isNull(),"未找到当前比例尺的110配置");
 QMap<QString,QString> rules;auto nodes=item.elementsByTagName("rule");
 for(int i=0;i<nodes.size();++i){auto n=nodes.at(i).toElement();auto name=n.firstChildElement("name").text().trimmed();
  if(name!="启用源数据属性筛选"&&!name.startsWith("属性筛选"))continue;
  require(!rules.contains(name),"110筛选规则重复："+name);rules[name]=n.firstChildElement("value").text().trimmed();}
 auto enabled=rules.value("启用源数据属性筛选","false").toLower();require(enabled=="true"||enabled=="false","启用源数据属性筛选只能填写true或false");
 result.enabled=enabled=="true";if(!result.enabled)return result;
 require(scale>0&&std::isfinite(toleranceMm)&&toleranceMm>0,"比例尺和源定位容差必须为正数");
 require(!split(sourcePaths).isEmpty(),"已启用源数据属性筛选，缺少对应GDB或源数据图层");
 std::set<int> numbers{1};QRegularExpression re("^属性筛选(?:字段|方式|值)([0-9]+)$");
 for(auto it=rules.begin();it!=rules.end();++it){auto match=re.match(it.key());if(match.hasMatch()){int n=match.captured(1).toInt();require(n>=2&&n<=1000&&QString::number(n)==match.captured(1),"属性筛选第一组不加1，后续从2开始编号");numbers.insert(n);}}
 std::vector<Condition> conditions;
 for(int n:numbers){QString suffix=n==1?QString():QString::number(n);Condition c{rules.value("属性筛选字段"+suffix),rules.value("属性筛选方式"+suffix),rules.value("属性筛选值"+suffix)};
  if(c.field.isEmpty()&&c.op.isEmpty()&&c.value.isEmpty())continue;
  require(!c.field.isEmpty()&&!c.value.isEmpty(),"属性筛选条件不完整");
  if(c.value=="为空"||c.value=="不为空")require(c.op.isEmpty()||c.op=="等于","空值筛选方式只能留空或等于");
  else require((c.op=="等于"||c.op=="包含")&&!split(c.value).isEmpty(),"属性筛选方式必须为等于或包含");conditions.push_back(c);}
 require(!conditions.empty(),"已启用源数据属性筛选，但没有有效筛选条件");
 GDALAllRegister();auto parsed=open(parsedPath);require(parsed->GetLayerCount()==1,"解析输入必须为单图层文件");auto* pl=parsed->GetLayer(0);
 require(pl->GetSpatialRef(),"解析图层缺少坐标系");OGRSpatialReference target(*pl->GetSpatialRef());target.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
 require(target.IsProjected(),"源数据空间定位要求解析图层为投影坐标系");double tolerance=toleranceMm*scale/1000.0/target.GetLinearUnits();require(std::isfinite(tolerance)&&tolerance>0,"源定位容差换算失败");
 std::vector<Source> sources;QSet<QString> paths;
 for(const auto& input:split(sourcePaths)){if(paths.contains(input.toCaseFolded()))continue;paths.insert(input.toCaseFolded());
  const auto parts=input.split('|');const QString path=parts.front();QString layerName;
  for(int i=1;i<parts.size();++i){require(parts[i].startsWith("layername="),"源图层路径仅支持|layername=图层名");layerName=parts[i].mid(10);require(!layerName.isEmpty(),"源图层名称为空");}
  auto ds=open(path);
  require(!layerName.isEmpty()||ds->GetLayerCount()==1,"请传入对应的单个源图层，不能传含多个图层的整库；可使用.gdb|layername=图层名或对应SHP");
  if(!layerName.isEmpty())require(ds->GetLayerByName(layerName.toUtf8().constData()),"对应源图层不存在："+layerName);
  for(int k=0;k<ds->GetLayerCount();++k){auto* l=ds->GetLayer(k);if(!layerName.isEmpty()&&layerName!=QString::fromUtf8(l->GetName()))continue;
   for(const auto& c:conditions)require(l->GetLayerDefn()->GetFieldIndex(c.field.toUtf8().constData())>=0,"源数据缺少筛选字段："+c.field);
   require(l->GetSpatialRef(),"源图层缺少坐标系");OGRSpatialReference from(*l->GetSpatialRef());from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
   std::unique_ptr<OGRCoordinateTransformation,TD> tr;if(!from.IsSame(&target)){tr.reset(OGRCreateCoordinateTransformation(&from,&target));require(bool(tr),"无法转换源图层坐标系");}
   while(Feat row{l->GetNextFeature()}){bool selected=true;for(const auto& c:conditions)selected=passes(row.get(),c)&&selected;
    auto* raw=row->GetGeometryRef();if(!raw||raw->IsEmpty())continue;Geom g(raw->getLinearGeometry());
    require(!tr||g->transform(tr.get())==OGRERR_NONE,"源几何坐标转换失败");std::vector<const OGRLineString*> parts;lines(g.get(),parts);
    require(g->IsValid(),"源河流包含无效几何");Source s{std::move(g),selected,{}};s.g->getEnvelope(&s.box);sources.push_back(std::move(s));if(selected)++result.selectedSources;
   }
  }
 }
 std::set<int> matched;
 while(Feat row{pl->GetNextFeature()}){auto* raw=row->GetGeometryRef();if(!raw||raw->IsEmpty())continue;Geom g(raw->getLinearGeometry());
  std::vector<const OGRLineString*> parts;lines(g.get(),parts);
  const double total=lineLength(g.get());if(!(total>0))continue;
  OGREnvelope box;g->getEnvelope(&box);std::vector<int> candidates;
  for(size_t i=0;i<sources.size();++i){if(!sources[i].selected)continue;auto& e=sources[i].box;if(e.MaxX+tolerance>=box.MinX&&e.MinX-tolerance<=box.MaxX&&e.MaxY+tolerance>=box.MinY&&e.MinY-tolerance<=box.MaxY)candidates.push_back(static_cast<int>(i));}
  if(candidates.empty())continue;
  OGRMultiPolygon buffers;
  for(int i:candidates){auto& s=sources[i];if(!s.buffer){s.buffer.reset(s.g->Buffer(tolerance,24));require(s.buffer&&s.buffer->IsValid(),"源河流缓冲区生成失败");}addPolygons(s.buffer.get(),buffers);}
  Geom united(buffers.UnionCascaded());require(united&&united->IsValid(),"源河流缓冲区合并失败");
  Geom inside(g->Intersection(united.get()));require(bool(inside),"解析线与源河流缓冲区求交失败");
  const double covered=lineLength(inside.get());require(std::isfinite(covered)&&covered>=0,"缓冲区内线长计算失败");
  if(covered+std::max(1e-8,total*1e-10)>=MIN_SOURCE_COVERAGE*total){
   result.parsedIds.insert(row->GetFID());
   for(int i:candidates){Geom part(g->Intersection(sources[i].buffer.get()));require(bool(part),"源河流关联统计求交失败");if(lineLength(part.get())>0)matched.insert(i);}
  }else if(covered>0)++result.ambiguousParsed;
 }
 result.matchedSources=static_cast<int>(matched.size());return result;
}

// 构造函数
AngleCheckDialog::AngleCheckDialog(QObject *parent)
    : QObject(parent)
{
}

// 析构函数
AngleCheckDialog::~AngleCheckDialog()
{
}

bool AngleCheckDialog::executeSharpAngleCheck(
    QgsVectorLayer *layer,
    const QString &xmlConfigPath,
    const QString &logFilePath,
    const QString &outputErrorShpPath,
    const QString &jsonPath,
    int scale,
    GeometryValidator *geometryValidator,
    const QgsFeatureIds* checkedIds,
    const QString& outputPointPath)
{
    m_checkedIds=checkedIds;
    const QString pointPath=outputPointPath.isEmpty()
        ?QFileInfo(outputErrorShpPath).dir().filePath(QFileInfo(outputErrorShpPath).completeBaseName()+"_points.shp")
        :outputPointPath;
    try
    {
        // 写入开始日志
        writeLogFile(logFilePath, "开始执行尖角检查...");

        // 获取实际图层名
        QString actualLayerName = layer->name();
        QString standardLayerName = QFileInfo(xmlConfigPath).baseName();

        writeLogFile(logFilePath, QString("实际图层名: %1, 标准图层名: %2").arg(actualLayerName).arg(standardLayerName));
        writeLogFile(logFilePath, QString("输出文件 - JSON: %1, 错误SHP: %2").arg(jsonPath).arg(outputErrorShpPath));

        // 确保输出目录存在
        QFileInfo jsonInfo(jsonPath);
        QFileInfo shpInfo(outputErrorShpPath);
        QDir jsonDir = jsonInfo.dir();
        QDir shpDir = shpInfo.dir();

        if (!jsonDir.exists() && !jsonDir.mkpath("."))
        {
            writeLogFile(logFilePath, QString("错误：无法创建JSON输出目录: %1").arg(jsonDir.path()));
            return false;
        }
        if (!shpDir.exists() && !shpDir.mkpath("."))
        {
            writeLogFile(logFilePath, QString("错误：无法创建SHP输出目录: %1").arg(shpDir.path()));
            return false;
        }

        // === XML配置检查 ===
        if (xmlConfigPath.isEmpty())
        {
            writeLogFile(logFilePath, "未提供XML配置路径，使用默认配置");

            // 直接构建简单的JSON结构
            QJsonObject checkItem;
            checkItem["id"] = "110";
            checkItem["name"] = QString::fromUtf8(u8"削尖角度阈值控制");
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = true;
            checkItem["info"] = "未提供XML配置路径";

            QJsonObject jsonLog;
            jsonLog["check_item"] = checkItem;

            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
                writeLogFile(logFilePath, QString("已生成错误JSON文件: %1").arg(jsonPath));
            }

            return false;
        }

        writeLogFile(logFilePath, QString("加载XML配置文件: %1").arg(xmlConfigPath));
        XmlConfigReader configReader;
        if (!configReader.loadConfig(xmlConfigPath))
        {
            writeLogFile(logFilePath, QString("错误：无法加载XML配置文件: %1").arg(xmlConfigPath));

            // 直接构建简单的JSON结构
            QJsonObject checkItem;
            checkItem["id"] = "110";
            checkItem["name"] = QString::fromUtf8(u8"削尖角度阈值控制");
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = true;
            checkItem["info"] = QString("无法加载XML配置: %1").arg(xmlConfigPath);

            QJsonObject jsonLog;
            jsonLog["check_item"] = checkItem;

            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }

            return false;
        }

        // 检查比例尺和检查项匹配
        QString targetItemName = QString::fromUtf8(u8"削尖角度阈值控制");
        writeLogFile(logFilePath, QString("检查项名称: %1, 比例尺: %2").arg(targetItemName).arg(scale));

        if (!configReader.isCheckItemAvailable(scale, targetItemName))
        {
            QList<int> availableScales = configReader.getAvailableScales();
            QStringList scaleStrings;
            for (int s : availableScales)
            {
                scaleStrings << QString::number(s);
            }

            QString errorMsg;
            if (!configReader.hasScale(scale))
            {
                errorMsg = QString::fromUtf8(u8"XML配置中不存在比例尺1:%1，可用比例尺: %2")
                               .arg(scale)
                               .arg(scaleStrings.join(", "));
            }
            else
            {
                QList<QString> itemNames = configReader.getCheckItemNames(scale);
                errorMsg = QString::fromUtf8(u8"比例尺1:%1下不存在检查项'%2'，可用检查项: %3")
                               .arg(scale)
                               .arg(targetItemName)
                               .arg(itemNames.join(", "));
            }

            writeLogFile(logFilePath, QString("配置不匹配: %1").arg(errorMsg));

            // 直接构建简单的JSON结构
            QJsonObject checkItem;
            checkItem["id"] = "110";
            checkItem["name"] = targetItemName;
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = true;
            checkItem["info"] = errorMsg;

            QJsonObject jsonLog;
            jsonLog["check_item"] = checkItem;

            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }

            return false;
        }

        // 读取角度阈值
        double maxAngle = configReader.getRuleValue(
            scale,
            targetItemName,
            QString::fromUtf8(u8"削尖角度阈值"),
            -1.0);

        writeLogFile(logFilePath, QString("从XML配置读取的角度阈值: %1").arg(maxAngle));

        if (maxAngle <= 0)
        {
            writeLogFile(logFilePath, QString("错误：角度阈值必须大于0度，当前值: %1").arg(maxAngle));

            // 直接构建简单的JSON结构
            QJsonObject checkItem;
            checkItem["id"] = "110";
            checkItem["name"] = targetItemName;
            checkItem["status"] = "质检失败";
            checkItem["err_exist"] = true;
            checkItem["info"] = QString::fromUtf8(u8"XML配置错误：削尖角度阈值必须大于0度，当前值: %1").arg(maxAngle);

            QJsonObject jsonLog;
            jsonLog["check_item"] = checkItem;

            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
            }

            return false;
        }

        const double FIXED_MIN_ANGLE = 0.0;
        writeLogFile(logFilePath, QString("使用角度阈值 - 最小: %1, 最大: %2").arg(FIXED_MIN_ANGLE).arg(maxAngle));

        // === 几何有效性检查 ===
        writeLogFile(logFilePath, "开始几何有效性检查...");
        QList<QgsFeature> invalidGeometryFeatures;

        // 使用智能指针管理 GeometryValidator
        std::unique_ptr<GeometryValidator> tempValidator;
        GeometryValidator *validator = geometryValidator;

        if (!validator)
        {
            tempValidator = std::make_unique<GeometryValidator>();
            validator = tempValidator.get();
            writeLogFile(logFilePath, "创建临时GeometryValidator实例");
        }

        // 创建临时的 QTextStream 来满足参数要求
        QString tempLog;
        QTextStream tempStream(&tempLog);
        tempStream.setCodec("UTF-8");

        invalidGeometryFeatures = validator->validateLayer(layer, "几何有效性检查", tempStream);
        if(m_checkedIds){
            for(auto it=invalidGeometryFeatures.begin();it!=invalidGeometryFeatures.end();)
                if(!m_checkedIds->contains(it->id()))it=invalidGeometryFeatures.erase(it);else ++it;
        }

        int invalidFeaturesCount = invalidGeometryFeatures.size();
        int totalFeatures = m_checkedIds?m_checkedIds->size():layer->featureCount();
        // Remove previous results only after input/config validation, including all SHP sidecars.
        if(QFile::exists(outputErrorShpPath)&&!QgsVectorFileWriter::deleteShapeFile(outputErrorShpPath))
            throw std::runtime_error("无法清理旧尖角错误要素");
        if(QFile::exists(pointPath)&&!QgsVectorFileWriter::deleteShapeFile(pointPath))
            throw std::runtime_error("无法清理旧尖角位置点");

        writeLogFile(logFilePath, QString("几何有效性检查完成 - 总要素: %1, 无效要素: %2").arg(totalFeatures).arg(invalidFeaturesCount));

        // 存在无效几何时停止检查并输出结果
        if (invalidFeaturesCount > 0)
        {
            writeLogFile(logFilePath, QString("发现%1个无效几何要素，停止尖角检查").arg(invalidFeaturesCount));

            // 生成几何无效的错误要素路径
            QString invalidGeometryShpPath = generateInvalidGeometryOutputPath(outputErrorShpPath, actualLayerName);

            // 生成几何无效错误信息
            std::vector<AngleError> geometryErrors;
            for (const QgsFeature &invalidFeature : invalidGeometryFeatures)
            {
                AngleError error;
                error.featureId = invalidFeature.id();
                error.partIndex = -1;
                error.ringIndex = -1;
                error.vertexIndex = -1;
                error.vertexPosition = QgsPointXY();
                error.angle = 0.0;
                error.errorType = ErrorType::InvalidGeometry;
                error.detectionTime = QDateTime::currentDateTime();
                error.isFromInvalidGeometry = true;
                error.detectionSource = "geometry_validity_check";

                QString description = invalidFeature.attribute("DESCRIPTION").toString();
                error.errorSubType = description.isEmpty() ? "几何无效" : description;

                geometryErrors.push_back(error);
            }

            // 汇总错误信息
            std::map<QgsFeatureId, FeatureErrorSummary> summaryMap;
            for (const auto &error : geometryErrors)
            {
                FeatureErrorSummary &summary = summaryMap[error.featureId];
                summary.featureId = error.featureId;
                summary.errorCount = 1;
                summary.invalidGeometryCount = 1;
                summary.thresholdAngle = maxAngle;
                summary.errors.push_back(error);
            }

            std::vector<FeatureErrorSummary> summaries;
            for (const auto &pair : summaryMap)
            {
                summaries.push_back(pair.second);
            }

            // 生成错误Shapefile
            bool shapefileSuccess = false;
            if (!summaries.empty())
            {
                writeLogFile(logFilePath, QString("生成几何无效错误Shapefile: %1").arg(invalidGeometryShpPath));
                shapefileSuccess = createErrorShapefile(layer, invalidGeometryShpPath, summaries);
                writeLogFile(logFilePath, QString("Shapefile生成结果: %1").arg(shapefileSuccess));
            }

            // 生成JSON日志
            QJsonObject geometryErrorsObj;
            int errorCount = 0;

            for (const QgsFeature &invalidFeature : invalidGeometryFeatures)
            {
                QString description = invalidFeature.attribute("DESCRIPTION").toString();
                QString errorDesc = description.isEmpty() ? "几何无效" : description;
                geometryErrorsObj[QString("error%1").arg(++errorCount)] = errorDesc;
            }

            QJsonObject checkItem;
            checkItem["id"] = "110";
            checkItem["name"] = targetItemName;
            checkItem["status"] = "质检成功";
            checkItem["err_exist"] = true;
            checkItem["info"] = QString(u8"%1存在几何无效，请先修复几何无效问题").arg(actualLayerName);
            checkItem["errors"] = geometryErrorsObj;

            QJsonObject jsonLog;
            jsonLog["check_item"] = checkItem;

            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
                writeLogFile(logFilePath, QString("已生成几何无效JSON报告: %1").arg(jsonPath));
            }

            return true;
        }

        // === 几何有效，执行尖角检查 ===
        writeLogFile(logFilePath, "开始尖角检查...");
        std::vector<AngleError> errors = checkSharpAngles(layer, FIXED_MIN_ANGLE, maxAngle);
        writeLogFile(logFilePath, QString("尖角检查完成，发现错误数量: %1").arg(errors.size()));

        // 汇总错误信息
        std::map<QgsFeatureId, FeatureErrorSummary> summaryMap;
        int sharpAngleCount = 0; // 添加尖角错误计数

        for (const auto &error : errors)
        {
            FeatureErrorSummary &summary = summaryMap[error.featureId];
            summary.featureId = error.featureId;
            summary.errorCount++;
            summary.thresholdAngle = maxAngle;

            if (error.errorType == ErrorType::SharpAngle)
            {
                summary.sharpAngleCount++;
                sharpAngleCount++; // 统计总尖角错误数

                if (summary.sharpAngleCount == 1)
                {
                    summary.minAngle = error.angle;
                    summary.maxAngle = error.angle;
                }
                else
                {
                    if (error.angle < summary.minAngle)
                        summary.minAngle = error.angle;
                    if (error.angle > summary.maxAngle)
                        summary.maxAngle = error.angle;
                }
            }
            else if (error.errorType == ErrorType::InvalidGeometry)
            {
                summary.invalidGeometryCount++;
            }
            summary.errors.push_back(error);
        }

        std::vector<FeatureErrorSummary> summaries;
        for (const auto &pair : summaryMap)
        {
            summaries.push_back(pair.second);
        }

        writeLogFile(logFilePath, QString("错误汇总完成，涉及要素数量: %1").arg(summaries.size()));

        // 生成错误Shapefile
        bool shapefileSuccess = false;
        if (!summaries.empty())
        {
            writeLogFile(logFilePath, QString("生成尖角错误Shapefile: %1").arg(outputErrorShpPath));
            shapefileSuccess = createErrorShapefile(layer, outputErrorShpPath, summaries);
            writeLogFile(logFilePath, QString("Shapefile生成结果: %1").arg(shapefileSuccess));
            if(!shapefileSuccess)throw std::runtime_error("尖角错误要素输出失败");
            if(!createAnglePoints(layer,pointPath,errors,maxAngle))throw std::runtime_error("尖角位置点输出失败");
            writeLogFile(logFilePath,QString("尖角位置点：%1；点数：%2").arg(pointPath).arg(sharpAngleCount));
        }
        else
        {
            writeLogFile(logFilePath, "无尖角错误，跳过Shapefile生成");
        }

        // 生成JSON日志
        bool hasSharpAngleErrors = sharpAngleCount > 0;

        // 构建尖角错误详细信息
        QJsonObject sharpAngleErrors;
        int errorIndex = 1;
        for (const auto &error : errors)
        {
            if (error.errorType == ErrorType::SharpAngle)
            {
                QString errorDesc = QString("图层[%1] 要素ID[%2] 顶点[%3] 角度[%4°] 位置[%5]")
                                        .arg(actualLayerName)
                                        .arg(error.featureId)
                                        .arg(error.vertexIndex)
                                        .arg(error.angle, 0, 'f', 1)
                                        .arg(pointToString(error.vertexPosition));
                sharpAngleErrors[QString("error%1").arg(errorIndex++)] = errorDesc;
            }
        }

        QJsonObject checkItem;
        checkItem["id"] = "110";
        checkItem["name"] = targetItemName;
        checkItem["status"] = "质检成功";
        checkItem["err_exist"] = hasSharpAngleErrors;
        checkItem["info"] = hasSharpAngleErrors ? QString(u8"%1存在尖角问题").arg(actualLayerName) : QString(u8"%1尖角检查通过").arg(actualLayerName);

        // 只有存在错误时才添加errors字段
        if (hasSharpAngleErrors)
        {
            checkItem["errors"] = sharpAngleErrors;
        }

        QJsonObject jsonLog;
        jsonLog["check_item"] = checkItem;

        QJsonDocument doc(jsonLog);
        QFile jsonFile(jsonPath);
        if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            jsonFile.write(doc.toJson(QJsonDocument::Indented));
            jsonFile.close();
            writeLogFile(logFilePath, QString("已生成JSON报告: %1").arg(jsonPath));
        }

        writeLogFile(logFilePath, "尖角检查执行完成");
        return true; // 只要执行完成就返回true
    }
    catch (const std::exception &e)
    {
        QString errorMsg = QString("executeSharpAngleCheck 发生异常: %1").arg(e.what());
        writeLogFile(logFilePath, errorMsg);

        // 创建错误报告
        QJsonObject checkItem;
        checkItem["id"] = "110";
        checkItem["name"] = QString::fromUtf8(u8"削尖角度阈值控制");
        checkItem["status"] = "质检失败";
        checkItem["err_exist"] = true;
        checkItem["info"] = QString("程序异常: %1").arg(e.what());

        QJsonObject jsonLog;
        jsonLog["check_item"] = checkItem;

        // 尝试保存错误日志
        try
        {
            QJsonDocument doc(jsonLog);
            QFile jsonFile(jsonPath);
            if (jsonFile.open(QIODevice::WriteOnly | QIODevice::Text))
            {
                jsonFile.write(doc.toJson(QJsonDocument::Indented));
                jsonFile.close();
                writeLogFile(logFilePath, QString("已保存错误日志: %1").arg(jsonPath));
            }
        }
        catch (...)
        {
            writeLogFile(logFilePath, "无法保存错误日志");
        }

        return false;
    }
    catch (...)
    {
        writeLogFile(logFilePath, "executeSharpAngleCheck 发生未知异常");
        return false;
    }
}

bool AngleCheckDialog::executeSharpAngleCheckStatic(
    QgsVectorLayer *layer,
    const QString &xmlConfigPath,
    const QString &logFilePath,
    const QString &outputErrorShpPath,
    const QString &jsonPath,
    int scale,
    GeometryValidator *geometryValidator,
    const QgsFeatureIds* checkedIds,
    const QString& outputPointPath)
{
    AngleCheckDialog checker;
    return checker.executeSharpAngleCheck(layer, xmlConfigPath, logFilePath,
                                          outputErrorShpPath, jsonPath, scale, geometryValidator,checkedIds,outputPointPath);
}

// 生成几何无效输出路径
QString AngleCheckDialog::generateInvalidGeometryOutputPath(const QString &originalErrorShpPath, const QString &layerName)
{
    QFileInfo fileInfo(originalErrorShpPath);
    QString dir = fileInfo.path();
    QString baseName = fileInfo.baseName();
    QString extension = fileInfo.suffix();

    // 在基础文件名后添加图层名和无效几何标识
    QString newBaseName = baseName + "_" + layerName + "_无效几何_err";

    return dir + "/" + newBaseName + "." + extension;
}

// 点坐标转字符串
QString AngleCheckDialog::pointToString(const QgsPointXY &point)
{
    return QString("(%1, %2)").arg(point.x(), 0, 'f', 6).arg(point.y(), 0, 'f', 6);
}

// 线段连续性检查
bool AngleCheckDialog::isContinuousSegment(const QVector<QgsPointXY> &points, int centerIndex)
{
    if (centerIndex <= 0 || centerIndex >= points.size() - 1)
        return false;

    const QgsPointXY &prev = points[centerIndex - 1];
    const QgsPointXY &curr = points[centerIndex];
    const QgsPointXY &next = points[centerIndex + 1];

    bool hasDuplicatePoints =
        (std::fabs(prev.x() - curr.x()) < EPS && std::fabs(prev.y() - curr.y()) < EPS) ||
        (std::fabs(curr.x() - next.x()) < EPS && std::fabs(curr.y() - next.y()) < EPS) ||
        (std::fabs(prev.x() - next.x()) < EPS && std::fabs(prev.y() - next.y()) < EPS);

    if (hasDuplicatePoints)
    {
        return false;
    }

    return true;
}

// 角度计算
double AngleCheckDialog::calculateAngle(const QgsPointXY &pPrev, const QgsPointXY &pCurr, const QgsPointXY &pNext, bool isPolygon)
{
    // 重复点检查
    if ((std::fabs(pPrev.x() - pCurr.x()) < EPS && std::fabs(pPrev.y() - pCurr.y()) < EPS) ||
        (std::fabs(pNext.x() - pCurr.x()) < EPS && std::fabs(pNext.y() - pCurr.y()) < EPS) ||
        (std::fabs(pPrev.x() - pNext.x()) < EPS && std::fabs(pPrev.y() - pNext.y()) < EPS))
    {
        return -1.0;
    }

    // 计算向量
    double v1x = pPrev.x() - pCurr.x();
    double v1y = pPrev.y() - pCurr.y();
    double v2x = pNext.x() - pCurr.x();
    double v2y = pNext.y() - pCurr.y();

    // 向量模长
    double mag1 = std::sqrt(v1x * v1x + v1y * v1y);
    double mag2 = std::sqrt(v2x * v2x + v2y * v2y);

    if (mag1 < EPS || mag2 < EPS)
    {
        return -1.0;
    }

    // 点积计算
    double dot = v1x * v2x + v1y * v2y;
    double cosAngle = std::max(-1.0, std::min(1.0, dot / (mag1 * mag2)));

    // 处理精度问题
    if (cosAngle > 1.0)
        cosAngle = 1.0;
    if (cosAngle < -1.0)
        cosAngle = -1.0;

    // 弧度转角度
    double angleRad = std::acos(cosAngle);
    double angleDeg = angleRad * 180.0 / M_PI;

    return angleDeg;
}

void AngleCheckDialog::processPolygonGeometry(const QgsPolygon *polygon, const QgsFeature &feature,
                                              int partIdx, double minAngle, double maxAngle, std::vector<AngleError> &errors)
{
    if (!polygon)
    {
        qWarning() << "processPolygonGeometry: 多边形指针为空";
        return;
    }

    try
    {
        // 处理外环
        const QgsCurve *exteriorRing = polygon->exteriorRing();
        if (exteriorRing)
        {
            std::unique_ptr<QgsLineString> exteriorLine(exteriorRing->curveToLine());
            if (exteriorLine && exteriorLine->numPoints() > 0)
            {
                QVector<QgsPointXY> points;
                points.reserve(exteriorLine->numPoints());

                for (int i = 0; i < exteriorLine->numPoints(); i++)
                {
                    points.append(exteriorLine->pointN(i));
                }

                // 移除重复终点
                if (points.size() >= 4 && points.first() == points.last())
                {
                    points.pop_back();
                }

                if (points.size() >= 3)
                {
                    processPointsForAngles(points, feature, partIdx, -1, minAngle, maxAngle, errors, "外环", true, false);
                }
            }
        }

        // 处理内环
        for (int ringIdx = 0; ringIdx < polygon->numInteriorRings(); ++ringIdx)
        {
            const QgsCurve *interiorRing = polygon->interiorRing(ringIdx);
            if (interiorRing)
            {
                std::unique_ptr<QgsLineString> interiorLine(interiorRing->curveToLine());
                if (interiorLine && interiorLine->numPoints() > 0)
                {
                    QVector<QgsPointXY> points;
                    points.reserve(interiorLine->numPoints());

                    for (int i = 0; i < interiorLine->numPoints(); i++)
                    {
                        points.append(interiorLine->pointN(i));
                    }

                    // 移除重复终点
                    if (points.size() >= 4 && points.first() == points.last())
                    {
                        points.pop_back();
                    }

                    if (points.size() >= 3)
                    {
                        processPointsForAngles(points, feature, partIdx, ringIdx, minAngle, maxAngle, errors, "内环", true, false);
                    }
                }
            }
        }
    }
    catch (const std::exception &e)
    {
        qCritical() << "处理多边形几何时发生异常:" << e.what();
    }
    catch (...)
    {
        qCritical() << "处理多边形几何时发生未知异常";
    }
}

// 处理线几何
void AngleCheckDialog::processLineStringGeometry(const QgsLineString *line, const QgsFeature &feature,
                                                 int partIdx, double minAngle, double maxAngle, std::vector<AngleError> &errors)
{
    if (!line)
        return;

    QVector<QgsPointXY> points;
    for (int i = 0; i < line->numPoints(); i++)
    {
        points.append(line->pointN(i));
    }

    // 过滤重复点
    QVector<QgsPointXY> filteredPoints;
    for (const auto &p : points)
    {
        if (filteredPoints.empty())
        {
            filteredPoints.push_back(p);
        }
        else
        {
            const QgsPointXY &lastP = filteredPoints.back();
            if (std::fabs(p.x() - lastP.x()) > EPS || std::fabs(p.y() - lastP.y()) > EPS)
            {
                filteredPoints.push_back(p);
            }
        }
    }

    processPointsForAngles(filteredPoints, feature, partIdx, -1, minAngle, maxAngle, errors, "线", false, false);
}

// 顶点角度检查
void AngleCheckDialog::processPointsForAngles(const QVector<QgsPointXY> &points, const QgsFeature &feature,
                                              int partIdx, int ringIdx, double minAngle, double maxAngle, std::vector<AngleError> &errors,
                                              const QString &geometryType, bool isPolygon, bool isClockwise)
{
    if (points.size() < 3)
    {
        return;
    }

    for (int i = 1; i < points.size() - 1; ++i)
    {
        // 连续性检查
        if (!isContinuousSegment(points, i))
        {
            continue;
        }

        // 计算角度
        double angle = calculateAngle(points[i - 1], points[i], points[i + 1], isPolygon);
        if (angle < 0)
        {
            continue;
        }

        // 阈值判断（考虑补角）
        double complementAngle = 360.0 - angle;
        bool isSharpAngle = (angle >= minAngle && angle <= maxAngle) ||
                            (complementAngle >= minAngle && complementAngle <= maxAngle);

        if (isSharpAngle)
        {
            AngleError error;
            error.featureId = feature.id();
            error.partIndex = partIdx;
            error.ringIndex = ringIdx;
            error.vertexIndex = i;
            error.vertexPosition = points[i];
            error.angle = angle;
            error.errorType = ErrorType::SharpAngle;
            error.detectionTime = QDateTime::currentDateTime();
            error.isFromInvalidGeometry = false;
            error.detectionSource = "sharp_angle";

            QString coordString = pointToString(points[i]);
            error.errorSubType = QString("角度%1° 位置%2").arg(angle, 0, 'f', 1).arg(coordString);

            errors.push_back(error);
        }
    }
}

// 有效几何尖角检测
void AngleCheckDialog::detectAnglesInValidGeometry(const QgsGeometry &geometry, const QgsFeature &feature,
                                                   double minAngle, double maxAngle, std::vector<AngleError> &errors)
{
    const QgsAbstractGeometry *abstractGeom = geometry.constGet();
    if (!abstractGeom)
    {
        return;
    }

    QgsWkbTypes::GeometryType geomType = QgsWkbTypes::geometryType(abstractGeom->wkbType());

    // 处理面要素
    if (geomType == QgsWkbTypes::PolygonGeometry)
    {
        const QgsPolygon *polygon = qgsgeometry_cast<const QgsPolygon *>(abstractGeom);
        if (polygon)
        {
            processPolygonGeometry(polygon, feature, 0, minAngle, maxAngle, errors);
        }
        else
        {
            const QgsMultiPolygon *multiPolygon = qgsgeometry_cast<const QgsMultiPolygon *>(abstractGeom);
            if (multiPolygon)
            {
                for (int partIdx = 0; partIdx < multiPolygon->numGeometries(); partIdx++)
                {
                    const QgsPolygon *partPolygon = qgsgeometry_cast<const QgsPolygon *>(multiPolygon->geometryN(partIdx));
                    if (partPolygon)
                    {
                        processPolygonGeometry(partPolygon, feature, partIdx, minAngle, maxAngle, errors);
                    }
                }
            }
        }
    }
    // 处理线要素
    else if (geomType == QgsWkbTypes::LineGeometry)
    {
        const QgsLineString *line = qgsgeometry_cast<const QgsLineString *>(abstractGeom);
        if (line)
        {
            processLineStringGeometry(line, feature, 0, minAngle, maxAngle, errors);
        }
        else
        {
            const QgsMultiLineString *multiLine = qgsgeometry_cast<const QgsMultiLineString *>(abstractGeom);
            if (multiLine)
            {
                for (int partIdx = 0; partIdx < multiLine->numGeometries(); partIdx++)
                {
                    const QgsLineString *partLine = qgsgeometry_cast<const QgsLineString *>(multiLine->geometryN(partIdx));
                    if (partLine)
                    {
                        processLineStringGeometry(partLine, feature, partIdx, minAngle, maxAngle, errors);
                    }
                }
            }
        }
    }
}

// 主检测函数
std::vector<AngleError> AngleCheckDialog::checkSharpAngles(QgsVectorLayer *layer, double minAngle, double maxAngle)
{
    std::vector<AngleError> errors;
    if (!layer || !layer->isValid())
    {
        return errors;
    }

    if (layer->geometryType() != QgsWkbTypes::LineGeometry && layer->geometryType() != QgsWkbTypes::PolygonGeometry)
    {
        return errors;
    }

    // 遍历要素检查
    QgsFeatureRequest request;
    if(m_checkedIds)request.setFilterFids(*m_checkedIds);
    QgsFeatureIterator features = layer->getFeatures(request);
    QgsFeature feature;

    while (features.nextFeature(feature))
    {
        QgsGeometry geometry = feature.geometry();
        if (geometry.isNull() || !geometry.isGeosValid())
        {
            continue; // 跳过无效几何（已在前期检查）
        }

        // 检测尖角
        detectAnglesInValidGeometry(geometry, feature, minAngle, maxAngle, errors);
    }

    return errors;
}

bool AngleCheckDialog::createAnglePoints(QgsVectorLayer* layer,const QString& path,
    const std::vector<AngleError>& errors,double threshold)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QgsFields fields;
    fields.append(QgsField("SRC_FID",QVariant::LongLong));
    fields.append(QgsField("SRC_LAYER",QVariant::String,"",100));
    fields.append(QgsField("PART_IDX",QVariant::Int));
    fields.append(QgsField("RING_IDX",QVariant::Int));
    fields.append(QgsField("VERTEX_IDX",QVariant::Int));
    fields.append(QgsField("ANGLE_DEG",QVariant::Double,"",18,8));
    fields.append(QgsField("LIMIT_DEG",QVariant::Double,"",18,8));
    fields.append(QgsField("X",QVariant::Double,"",24,10));
    fields.append(QgsField("Y",QVariant::Double,"",24,10));
    QgsVectorFileWriter::SaveVectorOptions options;options.driverName="ESRI Shapefile";options.fileEncoding="UTF-8";
    std::unique_ptr<QgsVectorFileWriter> writer(QgsVectorFileWriter::create(path,fields,QgsWkbTypes::Point,layer->crs(),QgsCoordinateTransformContext(),options));
    if(!writer||writer->hasError()!=QgsVectorFileWriter::NoError)return false;
    for(const auto& error:errors){if(error.errorType!=ErrorType::SharpAngle)continue;
        QgsFeature f(fields);f.setGeometry(QgsGeometry::fromPointXY(error.vertexPosition));
        f.setAttribute("SRC_FID",QVariant::fromValue<qlonglong>(error.featureId));
        f.setAttribute("SRC_LAYER",layer->name());f.setAttribute("PART_IDX",error.partIndex);
        f.setAttribute("RING_IDX",error.ringIndex);f.setAttribute("VERTEX_IDX",error.vertexIndex);
        f.setAttribute("ANGLE_DEG",error.angle);f.setAttribute("LIMIT_DEG",threshold);
        f.setAttribute("X",error.vertexPosition.x());f.setAttribute("Y",error.vertexPosition.y());
        if(!writer->addFeature(f))return false;
    }
    return true;
}

// 创建错误要素文件
bool AngleCheckDialog::createErrorShapefile(QgsVectorLayer *sourceLayer, const QString &outputPath,
                                            const std::vector<FeatureErrorSummary> &summaries)
{
    if (!sourceLayer || !sourceLayer->isValid())
    {
        return false;
    }

    if (summaries.empty())
    {
        return true;
    }

    // 确保输出目录存在
    QFileInfo outputInfo(outputPath);
    QDir outputDir = outputInfo.dir();
    if (!outputDir.exists() && !outputDir.mkpath("."))
    {
        return false;
    }

    // 定义输出字段
    QgsFields fields = sourceLayer->fields();
    fields.append(QgsField("error_type", QVariant::String, "", 20));
    fields.append(QgsField("error_subtype", QVariant::String, "", 100));
    fields.append(QgsField("error_count", QVariant::Int, "", 10));
    fields.append(QgsField("sharp_angle", QVariant::Double, "", 10, 2));
    fields.append(QgsField("vertex_idx", QVariant::Int, "", 10));
    fields.append(QgsField("vertex_coord", QVariant::String, "", 50));

    // 配置Shapefile写入选项
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "ESRI Shapefile";
    options.fileEncoding = "UTF-8";

    // 移除已有文件
    if (QFile::exists(outputPath))
    {
        QFile::remove(outputPath);
    }

    // 创建写入器
    QString errorMessage;
    std::unique_ptr<QgsVectorFileWriter> writer(
        QgsVectorFileWriter::create(
            outputPath,
            fields,
            sourceLayer->wkbType(),
            sourceLayer->crs(),
            QgsCoordinateTransformContext(),
            options));

    if (!writer || writer->hasError() != QgsVectorFileWriter::NoError)
    {
        return false;
    }

    // 提取问题要素ID
    int exportedCount = 0;
    QgsFeatureIds errorFeatureIds;
    for (const auto &summary : summaries)
    {
        errorFeatureIds.insert(summary.featureId);
    }

    // 写入问题要素
    QgsFeatureRequest request;
    request.setFilterFids(errorFeatureIds);
    QgsFeatureIterator features = sourceLayer->getFeatures(request);
    QgsFeature feature;

    while (features.nextFeature(feature))
    {
        if (!feature.isValid())
            continue;

        // 查找对应错误汇总
        auto it = std::find_if(summaries.begin(), summaries.end(),
                               [&](const FeatureErrorSummary &s)
                               { return s.featureId == feature.id(); });

        if (it != summaries.end())
        {
            for (const auto &error : it->errors)
            {
                QgsFeature outputFeature(fields);
                outputFeature.setGeometry(feature.geometry());

                // 复制源要素属性
                const QgsFields sourceFields = sourceLayer->fields();
                for (int i = 0; i < sourceFields.count() && i < fields.count(); ++i)
                {
                    outputFeature.setAttribute(sourceFields.at(i).name(), feature.attribute(i));
                }

                // 设置错误属性
                outputFeature.setAttribute("error_count", it->errorCount);
                outputFeature.setAttribute("vertex_idx", error.vertexIndex);

                QString coordString = pointToString(error.vertexPosition);
                outputFeature.setAttribute("vertex_coord", coordString);

                if (error.errorType == ErrorType::SharpAngle)
                {
                    QString errorSubtype = QString("角度%1° 位置%2").arg(error.angle, 0, 'f', 1).arg(coordString);
                    outputFeature.setAttribute("error_type", "尖角问题");
                    outputFeature.setAttribute("error_subtype", errorSubtype);
                    outputFeature.setAttribute("sharp_angle", error.angle);
                }
                else if (error.errorType == ErrorType::InvalidGeometry)
                {
                    outputFeature.setAttribute("error_type", "几何无效");
                    outputFeature.setAttribute("error_subtype", error.errorSubType);
                    outputFeature.setAttribute("sharp_angle", QVariant());
                    outputFeature.setAttribute("vertex_coord", QVariant());
                }

                // 写入要素
                if (writer->addFeature(outputFeature))
                {
                    exportedCount++;
                }
            }
        }
    }

    return exportedCount > 0;
}

// 日志写入函数
void AngleCheckDialog::writeLogFile(const QString &logPath, const QString &message)
{
    if (logPath.isEmpty())
        return;

    try
    {
        QFile file(logPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        {
            QTextStream stream(&file);
            stream.setCodec("UTF-8");
            stream << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << " - " << message;
            if (!message.endsWith('\n'))
            {
                stream << "\n";
            }
            file.close();
        }
    }
    catch (...)
    {
        qCritical() << "无法写入日志文件:" << logPath;
    }
}
