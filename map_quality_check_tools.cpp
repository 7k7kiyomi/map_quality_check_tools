#include "map_quality_check_tools.h"

/*------------QGIS include-----------------*/
#include "qgisinterface.h"
#include "qgsguiutils.h"
#include "qgsproject.h"
#include "qgsmessagebar.h"
#include "qgsmapcanvas.h"
#include "qgsapplication.h"

/*-------------Qt--------------------*/
#include <QCoreApplication>
#include <QAction>
#include <QFileInfo>
#include <QMessageBox>
#include <QtCore/QProcess>
#include <QMenu>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>

/*-------------SE---------------------*/

#include "ui_class/se_preprocess_dialog.h"
//#include"ui_QtWidgets_shpmapping.h"
//#include"ui_class/QtWidgets_shpmapping.h"
#include"ui_class/xml_editor.h"
#include"ui_class/ScaleCheckerDialog.h"
#include"ui_class/ScaleCheckerNormativeDialog.h"
#include"ui_class/map_check_task_manager.h"
#include"ui_class/Drawing_Layout.h"
//#include"ui_class/json_docx.h"
#include"ui_class/se_boundary_jump_draw_check_algorithm.h"
#include"ui_class/se_boundarytuopu.h"
#ifndef MAP_QUALITY_DISABLE_IMAGE_QA
#include"ui_class/se_image_map_check.h"
#endif // MAP_QUALITY_DISABLE_IMAGE_QA
#include"ui_class/SeriesMapEdgeMatchTaskDialog.h"
#include"ui_class/SeriesCrossScaleExpressionTaskDialog.h"
#include"ui_class/mapdata_download.h"



//#include "ui_class/se_feature_extent_check.h"
//#include "ui_class/DBManagerDlg.h"
//#include "ui_class/se_theme_mapping.h"
/*------------------------------------*/



// 消息日志
#include "qgsmessagelog.h"
#include "qgsmessagebar.h"

static const QString sName = QObject::tr("地图成果质检工具箱7");
static const QString sDescription = QObject::tr("提供地图成果质检功能7。");
static const QString sCategory = QObject::tr("Tile");
static const QString sPluginVersion = QObject::tr("Version 1.0");
static const QgisPlugin::PluginType sPluginType = QgisPlugin::UI;

// 插件图标
static const QString sPluginIcon = QStringLiteral(":/ptp/images/ptp.png");

// 成员变量声明（在头文件中也需要相应声明）
QMenu* mQualityCheckMenu = nullptr;
QMenu* mSeriesContentCheckMenu = nullptr;

namespace {
	// 报告程序是独立进程，不能直接继承平台运行时的 QSS。
	// 主题以平台配置为唯一来源，在启动时选择同主题的报告程序。
	QString reportThemeName(const QString& installRoot)
	{
		const QString settingsPath = QDir(installRoot).filePath(QStringLiteral("config/platform_settings.ini"));
		QSettings settings(settingsPath, QSettings::IniFormat);
		return settings.value(QStringLiteral("platform/theme"), QStringLiteral("white"))
			.toString().trimmed().toLower();
	}

	QString reportExecutableName(const QString& theme)
	{
		if (theme == QStringLiteral("steel-warm-white")
			|| theme == QStringLiteral("steel_warm_white")
			|| theme == QStringLiteral("steelwarmwhite"))
			return QStringLiteral("report_steel_warm_white.exe");
		if (theme == QStringLiteral("field-notes")
			|| theme == QStringLiteral("field_notes")
			|| theme == QStringLiteral("fieldnotes"))
			return QStringLiteral("report_field_notes.exe");
		if (theme == QStringLiteral("black") || theme == QStringLiteral("dark"))
			return QStringLiteral("report_black.exe");
		if (theme == QStringLiteral("sci-fi") || theme == QStringLiteral("sci_fi") || theme == QStringLiteral("scifi"))
			return QStringLiteral("report_sci_fi.exe");
		return QStringLiteral("report_white.exe");
	}

	// 质检报告导出适配：把一期/二期生成的结果 JSON 统一整理成报告程序可识别的 input.json。
	QString docxReportCheckItemDetailText(const QJsonObject& checkItem)
	{
		QString checkName = checkItem.value(QStringLiteral("name")).toString().trimmed();
		if (checkName.isEmpty()) checkName = QStringLiteral("质量检查");

		const QJsonObject errors = checkItem.value(QStringLiteral("errors")).toObject();
		if (errors.isEmpty()) return QStringLiteral("未发现%1错误。").arg(checkName);

		QString example;
		for (auto it = errors.constBegin(); it != errors.constEnd(); ++it) {
			if (!it.value().isObject()) continue;
			const QJsonObject err = it.value().toObject();
			example = err.value(QStringLiteral("description")).toString().trimmed();
			if (example.isEmpty()) example = err.value(QStringLiteral("summary")).toString().trimmed();
			if (!example.isEmpty()) break;
		}
		if (example.isEmpty()) example = QStringLiteral("存在错误但未给出详情");

		return QStringLiteral("发现%1个%2错误，例如：%3").arg(errors.size()).arg(checkName).arg(example);
	}

	void normalizeDocxReportCheckItemInfo(QJsonObject& checkItem)
	{
		if (!checkItem.contains(QStringLiteral("errors"))) return;

		QJsonObject info = checkItem.value(QStringLiteral("info")).toObject();
		const QString description = info.value(QStringLiteral("description")).toString().trimmed();
		const QString summary = info.value(QStringLiteral("summary")).toString().trimmed();
		if (!description.isEmpty() && !summary.isEmpty()) return;

		const QString detailText = docxReportCheckItemDetailText(checkItem);
		if (description.isEmpty()) info[QStringLiteral("description")] = detailText;
		if (summary.isEmpty()) info[QStringLiteral("summary")] = detailText;
		checkItem[QStringLiteral("info")] = info;
	}

	void normalizeDocxReportTaskNode(QJsonObject& taskNode)
	{
		for (auto it = taskNode.begin(); it != taskNode.end(); ++it) {
			if (!it.key().startsWith(QStringLiteral("check_item")) || !it.value().isObject()) continue;
			QJsonObject checkItem = it.value().toObject();
			normalizeDocxReportCheckItemInfo(checkItem);
			it.value() = checkItem;
		}
	}

	void normalizeDocxReportGeopdfNode(QJsonObject& geopdfNode)
	{
		QJsonObject tasks = geopdfNode.value(QStringLiteral("tasks")).toObject();
		for (auto it = tasks.begin(); it != tasks.end(); ++it) {
			if (!it.key().startsWith(QStringLiteral("task")) || !it.value().isObject()) continue;
			QJsonObject taskNode = it.value().toObject();
			normalizeDocxReportTaskNode(taskNode);
			it.value() = taskNode;
		}
		geopdfNode[QStringLiteral("tasks")] = tasks;
	}

	QJsonObject buildDocxReportInputJson(const QStringList& sourceJsonPaths)
	{
		QJsonObject mergedRoot;
		int geopdfCount = 0;
		for (const QString& sourceJsonPath : sourceJsonPaths) {
			QFile sourceFile(sourceJsonPath);
			if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
			const QJsonDocument doc = QJsonDocument::fromJson(sourceFile.readAll());
			sourceFile.close();
			if (!doc.isObject()) continue;

			const QJsonObject sourceRoot = doc.object();
			if (mergedRoot.isEmpty() && sourceRoot.contains(QStringLiteral("timing"))) {
				mergedRoot[QStringLiteral("timing")] = sourceRoot.value(QStringLiteral("timing"));
			}

			bool hasGeopdfNode = false;
			for (auto it = sourceRoot.constBegin(); it != sourceRoot.constEnd(); ++it) {
				if (it.key().startsWith(QStringLiteral("Geopdf")) && it.value().isObject()) {
					QJsonObject geopdfNode = it.value().toObject();
					normalizeDocxReportGeopdfNode(geopdfNode);
					mergedRoot[geopdfCount++ == 0 ? QStringLiteral("Geopdf") : QStringLiteral("Geopdf%1").arg(geopdfCount)] = geopdfNode;
					hasGeopdfNode = true;
				}
			}

			if (!hasGeopdfNode && sourceRoot.contains(QStringLiteral("tasks"))) {
				QJsonObject geopdfNode = sourceRoot;
				if (geopdfNode.value(QStringLiteral("name")).toString().trimmed().isEmpty()) {
					QString mapName = QFileInfo(sourceJsonPath).completeBaseName();
					mapName.remove(QRegularExpression(QStringLiteral("_result_\\d+$")));
					geopdfNode[QStringLiteral("name")] = mapName;
				}
				normalizeDocxReportGeopdfNode(geopdfNode);
				mergedRoot[geopdfCount++ == 0 ? QStringLiteral("Geopdf") : QStringLiteral("Geopdf%1").arg(geopdfCount)] = geopdfNode;
			}
		}
		return mergedRoot;
	}

	bool prepareDocxReportInputJson(const QStringList& sourceJsonPaths, const QString& inputJsonPath)
	{
		const QJsonObject sourceRoot = buildDocxReportInputJson(sourceJsonPaths);
		if (sourceRoot.isEmpty()) return false;

		const QFileInfo inputInfo(inputJsonPath);
		QDir inputDir(inputInfo.absolutePath());
		if (!inputDir.exists() && !inputDir.mkpath(QStringLiteral("."))) return false;

		QFile inputFile(inputJsonPath);
		if (!inputFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) return false;
		inputFile.write("\xEF\xBB\xBF");
		inputFile.write(QJsonDocument(sourceRoot).toJson(QJsonDocument::Indented));
		inputFile.close();
		return true;
	}

	bool backupReportWorkingFile(const QString& filePath, const QString& backupPath, bool* hadOriginal)
	{
		const bool exists = QFile::exists(filePath);
		if (hadOriginal) *hadOriginal = exists;
		if (!exists) return true;

		QFile::remove(backupPath);
		return QFile::copy(filePath, backupPath);
	}

	void restoreReportWorkingFile(const QString& filePath, const QString& backupPath, bool hadOriginal)
	{
		QFile::remove(filePath);
		if (hadOriginal) QFile::copy(backupPath, filePath);
	}

	QString docxPathFromProcessOutput(const QString& processOutput)
	{
		// json_docx.exe reports its real output path in its DEBUG messages. It can
		// ignore the requested output argument, so always use the reported path
		// for user-facing feedback instead of constructing a guessed filename.
		// A drive letter must be followed by a slash. This prevents the trailing
		// "G:" in the generator's "DEBUG:" prefix from being treated as a path.
		QRegularExpression pathPattern(QStringLiteral("(?:^|[\\r\\n\\s])([A-Za-z]:[\\\\/][^\\r\\n]*?\\.docx)"),
			QRegularExpression::CaseInsensitiveOption);
		QRegularExpressionMatchIterator matches = pathPattern.globalMatch(processOutput);
		QString reportPath;
		while (matches.hasNext()) {
			const QString candidate = matches.next().captured(1).trimmed();
			if (!candidate.isEmpty()) reportPath = QDir::cleanPath(candidate);
		}
		return reportPath;
	}

	QString latestResultJsonUnderDir(const QString& rootPath)
	{
		QDir rootDir(rootPath);
		if (!rootDir.exists()) return QString();

		QFileInfo newest;
		QDirIterator it(rootDir.absolutePath(), QStringList() << QStringLiteral("*_result_*.json"), QDir::Files, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QFileInfo candidate(it.next());
			if (!newest.exists() || candidate.lastModified() > newest.lastModified()) {
				newest = candidate;
			}
		}
		return newest.exists() ? newest.absoluteFilePath() : QString();
	}

	QString latestQualityReportResultJson()
	{
		// 菜单栏直接导出时，优先使用最近记录的结果 JSON；没有记录时再从一期配置快照的输出目录里查找。
		QSettings settings(QStringLiteral("GSZJPlugins"), QStringLiteral("map_quality_check_tools_test"));
		const QString persistedPath = settings.value(QStringLiteral("latest_report_result_json")).toString().trimmed();
		if (!persistedPath.isEmpty() && QFile::exists(persistedPath)) return QFileInfo(persistedPath).absoluteFilePath();

		const QByteArray snapshotRaw = settings.value(QStringLiteral("phase_one_config_snapshot")).toByteArray();
		const QJsonDocument snapshotDoc = QJsonDocument::fromJson(snapshotRaw);
		if (snapshotDoc.isObject()) {
			const QString outputDir = snapshotDoc.object().value(QStringLiteral("output_dir")).toString().trimmed();
			const QString latestPath = latestResultJsonUnderDir(outputDir);
			if (!latestPath.isEmpty()) {
				settings.setValue(QStringLiteral("latest_report_result_json"), latestPath);
				return latestPath;
			}
		}
		return QString();
	}
}

CGarMap_MapQualityCheckToolsPlugin::CGarMap_MapQualityCheckToolsPlugin(QgisInterface* qgisInterface)
	: QgisPlugin(sName, sDescription, sCategory, sPluginVersion, sPluginType)
	, mQGisIface(qgisInterface)
{

}

CGarMap_MapQualityCheckToolsPlugin::~CGarMap_MapQualityCheckToolsPlugin()
{
}


void CGarMap_MapQualityCheckToolsPlugin::initGui()
{
	//delete mActionTest;
	//delete mActionshpmapping;
	delete mActionxmleditor;
	delete mActionscale;
	//delete mActionnormative;
	delete mActiontask;
	delete mActionlayout;
	delete mActionreport;

	delete mActionjump;
	delete mActiontp;
	delete mActionmapdata;

	// 程序运行目录
	QString curExePath = QCoreApplication::applicationDirPath();
	const QString extentIconPath = QStringLiteral(":/theme_data_process_tools/images/feature_extent_check.png");

	/*【专题图制作】*/
	/*QIcon iconThemeMapping;
	QString strThemeMapping = curExePath + "/resource/toolbox/theme_data_mapping.png";
	iconThemeMapping.addFile(strThemeMapping);
	mActionTest = new QAction(iconThemeMapping, tr("测试矢量数据格式转换"), this);
	mActionTest->setObjectName(QStringLiteral("mActionThemeMapping"));
	mActionTest->setWhatsThis(tr("测试矢量数据格式转换"));
	connect(mActionTest, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Test);
	mQGisIface->addToolBarIcon(mActionTest);
	mQGisIface->addPluginToVectorMenu(tr("&测试矢量数据格式转换"), mActionTest);
	mActionTest->setEnabled(true);*/

	/*【SHP加载至QGIS】*/
	/*QIcon iconshpmapping;
	QString strshpmapping = "D:/Apps/QtWidgets_shp2/add_ptp.png";
	iconshpmapping.addFile(strshpmapping);
	mActionshpmapping = new QAction(iconshpmapping, tr("加载shp文件至图层"), this);
	mActionshpmapping->setObjectName(QStringLiteral("mActionshpmapping"));
	mActionshpmapping->setWhatsThis(tr("加载shp文件至图层"));
	connect(mActionshpmapping, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::shpmapping);
	mQGisIface->addVectorToolBarIcon(mActionshpmapping);
	mQGisIface->addPluginToVectorMenu(tr("&mapping"), mActionshpmapping);
	mActionshpmapping->setEnabled(true);*/


	///*【自定义xml文件】*/
	/*QIcon iconxmleditor;
	QString strxmleditor = curExePath + "/resource/toolbox/feature_extent_check.png";
	iconxmleditor.addFile(strxmleditor);
	mActionxmleditor = new QAction(iconxmleditor, tr("质检方案配置"), this);
	mActionxmleditor->setObjectName(QStringLiteral("mActionxmleditor"));
	mActionxmleditor->setWhatsThis(tr("质检方案配置"));
	connect(mActionxmleditor, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::xmleditor);
	mQGisIface->addToolBarIcon(mActionxmleditor);
	mQGisIface->addPluginToVectorMenu(tr("&质检方案配置"), mActionxmleditor);
	mActionxmleditor->setEnabled(true);*/

	///*【数字比例尺完整性】*/
	QIcon iconscale;
	QString strscale = curExePath + "/resource/toolbox/feature_attribute_check.png";
	iconscale.addFile(strscale);
	mActionscale = new QAction(iconscale, tr("数字比例尺数值准确性"), this);
	mActionscale->setObjectName(QStringLiteral("mActionscale"));
	mActionscale->setWhatsThis(tr("数字比例尺数值准确性"));
	connect(mActionscale, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::scale);
	mQGisIface->addToolBarIcon(mActionscale);
	mQGisIface->addPluginToVectorMenu(tr("&数字比例尺数值准确性"), mActionscale);
	mActionscale->setEnabled(true);

	///*【境界缺失和跳绘检查】*/
	QIcon iconjump;
	QString strjump = curExePath + "/resource/toolbox/feature_attribute_check.png";
	iconjump.addFile(strjump);
	mActionjump = new QAction(iconjump, tr("境界缺失和跳绘检查"), this);
	mActionjump->setObjectName(QStringLiteral("mActionjump"));
	mActionjump->setWhatsThis(tr("境界缺失和跳绘检查"));
	connect(mActionjump, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::jump);
	mQGisIface->addToolBarIcon(mActionjump);
	mQGisIface->addPluginToVectorMenu(tr("&境界缺失和跳绘检查"), mActionjump);
	mActionjump->setEnabled(true);

	///*【拓扑连通检查】*/
	QIcon icontp;
	QString strtp = curExePath + "/resource/toolbox/feature_attribute_check.png";
	icontp.addFile(strtp);
	mActiontp = new QAction(icontp, tr("拓扑连通检查"), this);
	mActiontp->setObjectName(QStringLiteral("mActiontp"));
	mActiontp->setWhatsThis(tr("拓扑连通检查"));
	connect(mActiontp, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::tuopu);
	mQGisIface->addToolBarIcon(mActiontp);
	mQGisIface->addPluginToVectorMenu(tr("&拓扑连通检查"), mActiontp);
	mActiontp->setEnabled(true);


	///*【数字比例尺规范性】*/
	/*QIcon iconnormative;
	QString strnormative = curExePath + "/resource/toolbox/feature_extent_check.png";
	iconnormative.addFile(strnormative);
	mActionnormative = new QAction(iconnormative, tr("数字比例尺规范性检查..."), this);
	mActionnormative->setObjectName(QStringLiteral("mActionnormative"));
	mActionnormative->setWhatsThis(tr("检查数字比例尺是否符合规范"));
	connect(mActionnormative, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::normative);
	mQGisIface->addToolBarIcon(mActionnormative);
	mQGisIface->addPluginToVectorMenu(tr("&数字比例尺规范性检查"), mActionnormative);
	mActionnormative->setEnabled(true);*/

	///*【质检任务管理】*/
	/*QIcon icontask;
	QString strtask = curExePath + "/resource/toolbox/feature_extent_check.png";
	icontask.addFile(strtask);
	mActiontask = new QAction(icontask, tr("质检任务管理"), this);
	mActiontask->setObjectName(QStringLiteral("mActiontask"));
	mActiontask->setWhatsThis(tr("质检任务管理"));
	connect(mActiontask, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Task);
	mQGisIface->addToolBarIcon(mActiontask);
	mQGisIface->addPluginToVectorMenu(tr("&质检任务管理"), mActiontask);
	mActiontask->setEnabled(true);*/

	///*【图面配置】*/
	QIcon iconlayout;
	QString strlayout = curExePath + "/resource/toolbox/feature_attribute_check.png";
	iconlayout.addFile(strlayout);
	mActionlayout = new QAction(iconlayout, tr("图面配置"), this);
	mActionlayout->setObjectName(QStringLiteral("mActionlayout"));
	mActionlayout->setWhatsThis(tr("图面配置"));
	connect(mActionlayout, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::layout);
	mQGisIface->addToolBarIcon(mActionlayout);
	mQGisIface->addPluginToVectorMenu(tr("&图面配置"), mActionlayout);
	mActionlayout->setEnabled(true);

	// 1. 创建主菜单"地图质检工具"
	if (!mQualityCheckMenu)
	{
		mQualityCheckMenu = new QMenu(tr("&地图质检工具"), mQGisIface->mainWindow());
		mQualityCheckMenu->setObjectName(QStringLiteral("mQualityCheckMenu"));
		// 将菜单添加到QGIS界面
		mQGisIface->addPluginToMenu(tr("&地图质检工具"), mQualityCheckMenu->menuAction());
	

		// 地图数据下载
		QIcon iconMapDataDownload(extentIconPath);
		mActionmapdata = new QAction(iconMapDataDownload, tr("数据库连接"), this);
		mActionmapdata->setObjectName(QStringLiteral("mActionmapdata"));
		mActionmapdata->setWhatsThis(tr("连接到PostGIS数据库"));
		connect(mActionmapdata, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::mapdata);
		mQualityCheckMenu->addAction(mActionmapdata);
		mQGisIface->addToolBarIcon(mActionmapdata);

		// 地图数据预处理
		QIcon icondata;
		QString strdata = curExePath + "/resource/toolbox/feature_extent_check.png";
		icondata.addFile(strdata);
		mActiondata = new QAction(icondata, tr("地图数据预处理"), this);
		mActiondata->setObjectName(QStringLiteral("mActiondata"));
		mActiondata->setWhatsThis(tr("地图数据预处理"));
		connect(mActiondata, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::data);
		mQualityCheckMenu->addAction(mActiondata);
		mQGisIface->addToolBarIcon(mActiondata);

		//质检方案配置
		QIcon iconxmleditor;
		QString strxmleditor = curExePath + "/resource/toolbox/feature_extent_check.png";
		iconxmleditor.addFile(strxmleditor);
		mActionxmleditor = new QAction(iconxmleditor, tr("质检方案配置"), this);
		mActionxmleditor->setObjectName(QStringLiteral("mActionxmleditor"));
		mActionxmleditor->setWhatsThis(tr("质检方案配置"));
		connect(mActionxmleditor, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::xmleditor);
		mQualityCheckMenu->addAction(mActionxmleditor);
		mQGisIface->addToolBarIcon(mActionxmleditor);

		//质检任务管理
		QIcon icontask;
		QString strtask = curExePath + "/resource/toolbox/feature_extent_check.png";
		icontask.addFile(strtask);
		mActiontask = new QAction(icontask, tr("质检任务管理"), this);
		mActiontask->setObjectName(QStringLiteral("mActiontask"));
		mActiontask->setWhatsThis(tr("质检任务管理"));
		connect(mActiontask, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::Task);
		mQualityCheckMenu->addAction(mActiontask);
		mQGisIface->addToolBarIcon(mActiontask);

		//图片地图质检
#ifndef MAP_QUALITY_DISABLE_IMAGE_QA
		QIcon iconimagemap;
		QString strimagemap = curExePath + "/resource/toolbox/feature_extent_check.png";
		iconimagemap.addFile(strimagemap);
		mActionimagemap = new QAction(iconimagemap, tr("图片地图质检"), this);
		mActionimagemap->setObjectName(QStringLiteral("mActionimagemap"));
		mActionimagemap->setWhatsThis(tr("图片地图质检"));
		connect(mActionimagemap, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::imagemap);
		mQualityCheckMenu->addAction(mActionimagemap);
		mQGisIface->addToolBarIcon(mActionimagemap);
#endif // MAP_QUALITY_DISABLE_IMAGE_QA

		// 系列图内容检查
		mSeriesContentCheckMenu = new QMenu(tr("系列图内容检查"), mQualityCheckMenu);
		mSeriesContentCheckMenu->setObjectName(QStringLiteral("mSeriesContentCheckMenu"));
		mQualityCheckMenu->addMenu(mSeriesContentCheckMenu);
		QIcon iconSeries(extentIconPath);
		mActionSeriesMapEdgeTask = new QAction(iconSeries, tr("图幅接边与系列化表达"), this);
		mActionSeriesMapEdgeTask->setObjectName(QStringLiteral("mActionSeriesMapEdgeTask"));
		mActionSeriesMapEdgeTask->setWhatsThis(tr("图幅接边与系列化表达"));
		connect(mActionSeriesMapEdgeTask, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::seriesMapEdgeTask);
		mSeriesContentCheckMenu->addAction(mActionSeriesMapEdgeTask);
		mQGisIface->addToolBarIcon(mActionSeriesMapEdgeTask);

		QIcon iconCrossScale(QStringLiteral(":/icons/warn.png"));
		mActionCrossScaleSeriesExpressionTask = new QAction(iconCrossScale, tr("同系列跨比例尺表达检查"), this);
		mActionCrossScaleSeriesExpressionTask->setObjectName(QStringLiteral("mActionCrossScaleSeriesExpressionTask"));
		mActionCrossScaleSeriesExpressionTask->setWhatsThis(tr("同系列跨比例尺表达检查"));
		connect(mActionCrossScaleSeriesExpressionTask, &QAction::triggered, this, &CGarMap_MapQualityCheckToolsPlugin::seriesCrossScaleExpressionTask);
		mSeriesContentCheckMenu->addAction(mActionCrossScaleSeriesExpressionTask);
		mQGisIface->addToolBarIcon(mActionCrossScaleSeriesExpressionTask);

		//质检报告生成与导出
		QIcon iconreport(extentIconPath);
		mActionreport = new QAction(iconreport, tr("质检报告生成与导出"), this);
		mActionreport->setObjectName(QStringLiteral("mActionreport"));
		mActionreport->setWhatsThis(tr("质检报告生成与导出"));
		connect(mActionreport, &QAction::triggered, this, static_cast<void (CGarMap_MapQualityCheckToolsPlugin::*)()>(&CGarMap_MapQualityCheckToolsPlugin::report));
		mQualityCheckMenu->addAction(mActionreport);
		mQGisIface->addToolBarIcon(mActionreport);
	}

	updateActions();
}


/*void CGarMap_MapQualityCheckToolsPlugin::Test()
{
	CSE_VectorFormatConversionDialog* pDlg = new CSE_VectorFormatConversionDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::shpmapping()
{
	QtWidgets_shpmapping* pDlg = new QtWidgets_shpmapping(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}*/

void CGarMap_MapQualityCheckToolsPlugin::xmleditor()
{
	SE_TaskModelARDRuleDlg* pDlg = new SE_TaskModelARDRuleDlg(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::scale()
{
	ScaleCheckerDialog* pDlg = new ScaleCheckerDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::layout()
{
	Drawing_Layout* pDlg = new Drawing_Layout("", "",nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::data()
{
	CSE_PreprocessDialog* pDlg = new CSE_PreprocessDialog(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::report()
{
	// 无参导出用于菜单栏入口：自动查找最近一次质检结果，再走统一的报告导出流程。
	const QString resultJsonPath = latestQualityReportResultJson();
	if (resultJsonPath.isEmpty()) {
		QMessageBox::warning(nullptr, QStringLiteral("提示"), QStringLiteral("未找到最近一次质检结果 JSON，请先执行检查。"));
		return;
	}
	report(resultJsonPath);
}

void CGarMap_MapQualityCheckToolsPlugin::report(const QString& resultJsonPath)
{
	const QString sourceJsonPath = resultJsonPath.trimmed();
	if (sourceJsonPath.isEmpty()) {
		report();
		return;
	}
	report(QStringList() << sourceJsonPath);
}

void CGarMap_MapQualityCheckToolsPlugin::report(const QStringList& resultJsonPaths)
{
	report(resultJsonPaths, QString());
}

void CGarMap_MapQualityCheckToolsPlugin::report(const QStringList& resultJsonPaths, const QString& finalOutputPath)
{
	// 统一报告导出入口：支持一个或多个结果 JSON，并按平台当前主题调用对应报告程序生成 docx。
	QStringList sourceJsonPaths;
	for (const QString& resultJsonPath : resultJsonPaths) {
		const QString sourceJsonPath = resultJsonPath.trimmed();
		if (!sourceJsonPath.isEmpty() && QFile::exists(sourceJsonPath)) sourceJsonPaths << QFileInfo(sourceJsonPath).absoluteFilePath();
	}
	if (sourceJsonPaths.isEmpty()) {
		QMessageBox::warning(nullptr, QStringLiteral("错误"), QStringLiteral("找不到已选择的质检结果 JSON。"));
		return;
	}

	QByteArray prefix = qgetenv("MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01");
	QString baseDir = QString::fromUtf8(prefix);
	QString exePath;
	QString selectedTheme;

	if (!baseDir.isEmpty())
	{
		const QString exeDir = QDir(baseDir).filePath(QStringLiteral("MPQIS/jsondoc"));
		selectedTheme = reportThemeName(baseDir);
		exePath = QDir(exeDir).filePath(reportExecutableName(selectedTheme));
	}

	if (exePath.isEmpty() || !QFile::exists(exePath)) {
		QString msg;
		if (baseDir.isEmpty())
			msg = QStringLiteral("未设置环境变量: MPQIS_INSTALL_DIRECTORY_PREFIX_V_0_00_01");
		else
			msg = QStringLiteral("找不到主题“%1”的报告程序:\n%2")
				.arg(selectedTheme.isEmpty() ? QStringLiteral("white") : selectedTheme, exePath);
		QMessageBox::warning(nullptr, QStringLiteral("错误"), msg);
		return;
	}

	const QString mpqisBinDir = QDir(baseDir).filePath(QStringLiteral("MPQIS/bin"));
	const QString templatePath = QDir(mpqisBinDir).filePath(QStringLiteral("template.docx"));
	if (!QFile::exists(templatePath)) {
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("找不到报告模板文件：\n%1").arg(templatePath));
		return;
	}

	const QFileInfo firstSourceInfo(sourceJsonPaths.first());
	QTemporaryDir reportWorkDir(QDir(firstSourceInfo.absolutePath()).filePath(QStringLiteral(".docx_report_XXXXXX")));
	if (!reportWorkDir.isValid()) {
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("无法创建报告导出临时目录。"));
		return;
	}
	const QString inputJsonPath = QDir(mpqisBinDir).filePath(QStringLiteral("input.json"));
	const QString generatedOutputPath = QDir(reportWorkDir.path()).filePath(QStringLiteral("output.docx"));
	const QString inputBackupPath = QDir(reportWorkDir.path()).filePath(QStringLiteral("input.json.backup"));
	const QString finalOutputBackupPath = QDir(reportWorkDir.path()).filePath(QStringLiteral("report.docx.backup"));
	QString resolvedOutputPath = finalOutputPath.trimmed();
	if (resolvedOutputPath.isEmpty()) {
		const QString outputBaseName = sourceJsonPaths.size() == 1
			? firstSourceInfo.completeBaseName() + QStringLiteral("_report.docx")
			: QStringLiteral("selected_result_report.docx");
		resolvedOutputPath = QDir(firstSourceInfo.absolutePath()).filePath(outputBaseName);
	}

	bool hadInputJson = false;
	bool hadFinalOutput = false;
	if (!backupReportWorkingFile(inputJsonPath, inputBackupPath, &hadInputJson) ||
		!backupReportWorkingFile(resolvedOutputPath, finalOutputBackupPath, &hadFinalOutput)) {
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("无法备份已有报告工作文件。"));
		return;
	}
	auto restoreReportInputFile = [&]() {
		restoreReportWorkingFile(inputJsonPath, inputBackupPath, hadInputJson);
	};
	auto restoreReportWorkingFiles = [&]() {
		restoreReportInputFile();
		restoreReportWorkingFile(resolvedOutputPath, finalOutputBackupPath, hadFinalOutput);
	};

	QFile::remove(inputJsonPath);
	QFile::remove(generatedOutputPath);
	if (QFile::exists(resolvedOutputPath) && !QFile::remove(resolvedOutputPath)) {
		restoreReportWorkingFiles();
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("无法覆盖已有报告文件。\n%1").arg(resolvedOutputPath));
		return;
	}
	if (!prepareDocxReportInputJson(sourceJsonPaths, inputJsonPath)) {
		restoreReportWorkingFiles();
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("无法从已选择的质检结果 JSON 生成报告输入文件。"));
		return;
	}
	QProcess process;
	QStringList args;
	args << inputJsonPath << generatedOutputPath;
	process.setWorkingDirectory(mpqisBinDir);
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start(exePath, args);

	if (!process.waitForStarted()) {
		restoreReportWorkingFiles();
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("外部程序启动失败！"));
		return;
	}

	while (process.state() != QProcess::NotRunning) {
		process.waitForFinished(200);
		QCoreApplication::processEvents();
	}
	// 报告程序将调试信息（包括中文文件名）以 UTF-8 输出。
	// Decoding them with the Windows local code page corrupts those paths.
	const QString processOutput = QString::fromUtf8(process.readAll()).trimmed();
	QString actualGeneratedOutputPath = docxPathFromProcessOutput(processOutput);
	const bool generatorReportedSuccess = processOutput.contains(QStringLiteral("报告生成成功"));
	const QString reportedOutputPath = actualGeneratedOutputPath;
	if (actualGeneratedOutputPath.isEmpty()) actualGeneratedOutputPath = generatedOutputPath;
	const QFileInfo generatedReportInfo(actualGeneratedOutputPath);
	const bool generatedReportExists = generatedReportInfo.exists() && generatedReportInfo.isFile() && generatedReportInfo.size() > 0;
	if (!generatorReportedSuccess && !generatedReportExists) {
		restoreReportWorkingFiles();
		QMessageBox::critical(nullptr, QStringLiteral("错误"), QStringLiteral("质检报告导出失败。\n%1").arg(processOutput));
		return;
	}
	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		qDebug() << "report generator returned a non-zero exit status after creating the report:"
			<< process.exitStatus() << process.exitCode();
	}

	restoreReportInputFile();
	const QString successMessage = reportedOutputPath.isEmpty()
		? QStringLiteral("质检报告导出完成！\n请以导出窗口显示的保存路径为准。")
		: QStringLiteral("质检报告导出完成！\n%1").arg(reportedOutputPath);
	QMessageBox::information(nullptr, QStringLiteral("提示"), successMessage);
}



void CGarMap_MapQualityCheckToolsPlugin::jump()
{
	se_boundary_jump_draw_check_algorithm* pDlg = new se_boundary_jump_draw_check_algorithm("", "", nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::tuopu()
{
	se_boundarytuopu* pDlg = new se_boundarytuopu("", "", nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::imagemap()
{
#ifndef MAP_QUALITY_DISABLE_IMAGE_QA
	SE_ImageMapCheckDialog* pDlg = new SE_ImageMapCheckDialog(mQGisIface, nullptr, Qt::WindowCloseButtonHint);
	pDlg->setModal(false);
	pDlg->show();
#endif // MAP_QUALITY_DISABLE_IMAGE_QA
}

void CGarMap_MapQualityCheckToolsPlugin::mapdata()
{
	// 使用无父窗口的顶层 Window，使下载窗口在任务栏中独立显示，
	// 不再作为质检平台主窗口的附属对话框。
	auto* dialog = new mapdata_download(nullptr, Qt::Window | Qt::WindowCloseButtonHint);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setModal(false);
	dialog->show();
}

void CGarMap_MapQualityCheckToolsPlugin::Task()
{
	map_check_task_manager* pDlg = new map_check_task_manager(nullptr, Qt::WindowCloseButtonHint);
	pDlg->setAttribute(Qt::WA_DeleteOnClose);  // 对话框关闭时自动删除，避免信号重复连接
	connect(pDlg, &map_check_task_manager::reportRequested, this, static_cast<void (CGarMap_MapQualityCheckToolsPlugin::*)(const QString&)>(&CGarMap_MapQualityCheckToolsPlugin::report));
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::seriesMapEdgeTask()
{
	SeriesMapEdgeMatchTaskDialog* pDlg = new SeriesMapEdgeMatchTaskDialog(
		mQGisIface,
		nullptr,
		Qt::WindowCloseButtonHint);
	// 图幅接边窗口内的“质检报告导出”按钮复用一期报告导出能力，不要求必须先在当前窗口执行检查。
	pDlg->setReportExportHandler([this](const QStringList& resultJsonPaths) {
		if (resultJsonPaths.isEmpty()) {
			report();
			return;
		}
		report(resultJsonPaths);
		});
	pDlg->setAttribute(Qt::WA_DeleteOnClose);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::seriesCrossScaleExpressionTask()
{
	SeriesCrossScaleExpressionTaskDialog* pDlg = new SeriesCrossScaleExpressionTaskDialog(nullptr, Qt::WindowCloseButtonHint);
	// 跨比例尺窗口内的“质检报告导出”按钮复用一期报告导出能力，优先导出窗口收集到的结果 JSON。
	pDlg->setReportExportHandler([this](const QStringList& resultJsonPaths) {
		if (resultJsonPaths.isEmpty()) {
			report();
			return;
		}
		report(resultJsonPaths);
		});
	pDlg->setAttribute(Qt::WA_DeleteOnClose);
	pDlg->setModal(false);
	pDlg->show();
}

void CGarMap_MapQualityCheckToolsPlugin::unload()
{
	//// 去掉ui界面
	/*mQGisIface->removePluginVectorMenu(tr("&测试矢量数据格式转换"), mActionTest);
	mQGisIface->removeToolBarIcon(mActionTest);

	mQGisIface->removePluginVectorMenu(tr("&mapping"), mActionshpmapping);
	mQGisIface->removeVectorToolBarIcon(mActionshpmapping);*/

	mQGisIface->removePluginVectorMenu(tr("&xmleditor"), mActionxmleditor);
	mQGisIface->removeVectorToolBarIcon(mActionxmleditor);

    mQGisIface->removePluginVectorMenu(tr("&数字比例尺一致性检查"), mActionscale);
	mQGisIface->removeToolBarIcon(mActionscale);

	/*mQGisIface->removePluginVectorMenu(tr("&数字比例尺规范性检查"), mActionnormative);
	mQGisIface->removeToolBarIcon(mActionnormative);*/

	mQGisIface->removePluginVectorMenu(tr("&任务管理器"), mActiontask);
	mQGisIface->removeToolBarIcon(mActiontask);

	mQGisIface->removePluginVectorMenu(tr("&图面配置"), mActionlayout);
	mQGisIface->removeToolBarIcon(mActionlayout);

	mQGisIface->removePluginVectorMenu(tr("&报告导出"), mActionreport);
	mQGisIface->removeToolBarIcon(mActionreport);

#ifndef MAP_QUALITY_DISABLE_IMAGE_QA
	mQGisIface->removePluginVectorMenu(tr("&图片地图质检"), mActionimagemap);
	mQGisIface->removeToolBarIcon(mActionimagemap);
#endif // MAP_QUALITY_DISABLE_IMAGE_QA

	mQGisIface->removePluginVectorMenu(tr("&地图数据下载"), mActionmapdata);
	mQGisIface->removeToolBarIcon(mActionmapdata);

	//delete mActionTest;
	//delete mActionshpmapping;
	delete mActionxmleditor;
	delete mActionscale;
	//delete mActionnormative;
	delete mActiontask;
	delete mActionlayout;
	delete mActionreport;
	delete mActionimagemap;
	delete mActionmapdata;
	delete mActionSeriesMapEdgeTask;
	delete mActionCrossScaleSeriesExpressionTask;
	
 //   delete mActionFeatureExtentCheck;
	//delete mActionDataStorageManagement;
	//delete mActionThemeMapping;
}


void CGarMap_MapQualityCheckToolsPlugin::updateActions()
{
	// 更新菜单状态
}

/**
 * Required extern functions needed  for every plugin
 * These functions can be called prior to creating an instance
 * of the plugin class
 */
 // Class factory to return a new instance of the plugin class
QGISEXTERN QgisPlugin* classFactory(QgisInterface* qgisInterfacePointer)
{
	return new CGarMap_MapQualityCheckToolsPlugin(qgisInterfacePointer);
}

// Return the name of the plugin - note that we do not user class members as
// the class may not yet be insantiated when this method is called.
QGISEXTERN const QString* name()
{
	return &sName;
}

// Return the description
QGISEXTERN const QString* description()
{
	return &sDescription;
}

// Return the category
QGISEXTERN const QString* category()
{
	return &sCategory;
}

// Return the type (either UI or MapLayer plugin)
QGISEXTERN int type()
{
	return sPluginType;
}

// Return the version number for the plugin
QGISEXTERN const QString* version()
{
	return &sPluginVersion;
}

QGISEXTERN const QString* icon()
{
	return &sPluginIcon;
}

// Delete ourself
QGISEXTERN void unload(QgisPlugin* pluginPointer)
{
	delete pluginPointer;
}


