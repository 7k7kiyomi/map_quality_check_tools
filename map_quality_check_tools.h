#ifndef MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
#define MAP_QUALITY_CHECK_TOOLS_PLUGIN_H

#include "qgisplugin.h"
#include <QObject>
#include <QStringList>

#include <string>
#include <vector>
using namespace std;

class QAction;
class QgisInterface;

class CGarMap_MapQualityCheckToolsPlugin : public QObject, public QgisPlugin
{
	Q_OBJECT

public:
	explicit CGarMap_MapQualityCheckToolsPlugin(QgisInterface* qgisInterface);
	~CGarMap_MapQualityCheckToolsPlugin() override;

public slots:

	//! 初始化Gui
	void initGui() override;
	//! actions
	
	// 测试矢量数据格式转换
	//void Test();

	//加载至QGIS
	//void shpmapping();

	//自定义xml文件
	void xmleditor();

	//数字比例尺完整性检查
	void scale();

	//数字比例尺规范性检查
	//void normative();

	//质检任务管理器
	void Task();

	//! 卸载插件
	void unload() override;

	//配置文件加载
	void layout();

	//地图数据预处理
	void data();

	//质检报告生成与导出
	void report();
	void report(const QString& resultJsonPath);
	void report(const QStringList& resultJsonPaths);
	void report(const QStringList& resultJsonPaths, const QString& finalOutputPath);

	// 系列图内容检查 - 图幅接边与系列化表达
	void seriesMapEdgeTask();

	// 系列图内容检查 - 同系列跨比例尺表达检查
	void seriesCrossScaleExpressionTask();

	//跳绘
	void jump();

	//拓扑连通
	void tuopu();

	//图片地图质检
	void imagemap();

	//地图数据下载
	void mapdata();
private:
	
	//! Pointer to the QGIS interface object
	QgisInterface* mQGisIface = nullptr;
	

	//!pointer to the qaction for this plugin
	
	// “矢量数据格式转换”菜单
	//QAction* mActionTest = nullptr;

	//QAction* mActionshpmapping = nullptr;//加载进QGIS

	QAction* mActionxmleditor = nullptr;//自定义xml文件

	QAction* mActionscale = nullptr;//数字比例尺完整性检查

	//QAction* mActionnormative = nullptr;//数字比例尺规范性检查

	QAction* mActiontask = nullptr;//质检任务管理器

	QAction* mActiondata = nullptr;//地图数据预处理

	QAction* mActionreport = nullptr;//质检报告生成与导出

	QAction* mActionlayout = nullptr;//图面文件配置

	QAction* mActionjump = nullptr;//跳绘检查

	QAction* mActiontp = nullptr;//拓扑连通性检查

	QAction* mActionimagemap = nullptr;//图片地图质检

	QAction* mActionSeriesMapEdgeTask = nullptr;//系列图内容检查-图幅接边与系列化表达
	QAction* mActionCrossScaleSeriesExpressionTask = nullptr;//系列图内容检查-同系列跨比例尺表达检查

	QAction* mActionmapdata = nullptr;//地图数据下载
private slots:

	void updateActions();

private:



};

#endif // MAP_QUALITY_CHECK_TOOLS_PLUGIN_H
