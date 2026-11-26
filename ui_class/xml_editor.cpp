#define _HAS_STD_BYTE 0
#include "xml_editor.h"
#include "ui_xml_editor.h"
#include "ui_class/RuleSclect.h"
#include "FileNameDialog.h"

#include "qgshelp.h"
#include "qgsmaplayer.h"
#include "qgsproject.h"
#include "qgsvectordataprovider.h"
#include "qgsvectorlayer.h"
#include "qgssettings.h"
#include "qgsapplication.h"
#include "qgsgui.h"
#include <QInputDialog>

#include <QFileDialog>
#include <QMessageBox>
#include <QComboBox>
#include <QHeaderView>
#include <QAbstractItemModel>
#include <QDirIterator>
#include <QListWidget>
#include <QSignalBlocker>
#include <qcheckbox.h>
#include <qmenu.h>

#include <qtreewidget.h>
#include <QFile>
#include "cpl_config.h"

#include "gdal_priv.h"
#include <ogrsf_frmts.h>
#include <ogr_feature.h>
// Qt XML DOM
#include <QDomDocument>
#include <QDomElement>

#ifdef OS_FAMILY_WINDOWS
#include <io.h>
#include <stdio.h>
#include <windows.h>
#else
#include <sys/io.h>
#include <dirent.h>
#include <sys/errno.h>

#define _atoi64(val)   strtoll(val,NULL,10)

/*--------------STL---------------*/
#include <filesystem>
#include <fstream>
#include <iostream>
#include "customize_ARD.h"
#include "se_task_model_ARD_rule.h"

/*--------------STL---------------*/

#endif //

using namespace std;

SE_TaskModelARDRuleDlg::SE_TaskModelARDRuleDlg(QWidget* parent, Qt::WindowFlags fl)
	: QDialog(parent, fl)
	, m_newXmlFilePath("")
{
	ui.setupUi(this);
	QgsGui::enableAutoGeometryRestore(this);
	initializeFileManagerLayout();

	// 未打开或新建 XML 时，树控件仍使用 .ui 中的默认六列表头。
	// 先初始化空方案，保证“严重程度”列在对话框打开时即可显示。
	m_loadingConfig = true;
	InitTreeWidget();
	m_loadingConfig = false;


	connect(ui.pushButton_OK, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_OK_clicked);
	connect(ui.pushButton_Cancel, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_Cancel_clicked);
	connect(ui.pushButton_Query, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_Query_clicked);
	connect(ui.pushButton_AddCheckItem, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_AddCheckItem_clicked);
	connect(ui.pushButton_DeleteCheckItem, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_DeleteCheckItem_clicked);
	connect(ui.pushButton_newfile, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_newfile_clicked);
	connect(ui.pushButton_Open, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_Open_clicked);
	connect(m_loadFolderButton, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_LoadFolder_clicked);
	connect(ui.pushButton_Save, &QPushButton::clicked, this, &SE_TaskModelARDRuleDlg::pushButton_Save_clicked);
	connect(m_configFileList, &QListWidget::currentRowChanged,
		this, &SE_TaskModelARDRuleDlg::onConfigFileSelectionChanged);
	connect(m_searchConfigFiles, &QLineEdit::textChanged, this,
		[this](const QString& keyword) {
			const QString searchText = keyword.trimmed();
			for (int row = 0; row < m_configFileList->count(); ++row)
			{
				QListWidgetItem* item = m_configFileList->item(row);
				const bool matches = searchText.isEmpty()
					|| item->text().contains(searchText, Qt::CaseInsensitive)
					|| item->toolTip().contains(searchText, Qt::CaseInsensitive);
				item->setHidden(!matches);
			}
		});
	connect(ui.treeWidget_TaskModelData->model(), &QAbstractItemModel::dataChanged,
		this, [this](const QModelIndex&, const QModelIndex&, const QVector<int>& roles) {
			if (!m_loadingConfig && (roles.isEmpty() || roles.contains(Qt::DisplayRole)
				|| roles.contains(Qt::EditRole)))
				m_hasUnsavedChanges = true;
		});
	connect(ui.treeWidget_TaskModelData->model(), &QAbstractItemModel::rowsInserted,
		this, [this]() { if (!m_loadingConfig) m_hasUnsavedChanges = true; });
	connect(ui.treeWidget_TaskModelData->model(), &QAbstractItemModel::rowsRemoved,
		this, [this]() { if (!m_loadingConfig) m_hasUnsavedChanges = true; });
	// 设置右键菜单
	ui.treeWidget_TaskModelData->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui.treeWidget_TaskModelData, &QTreeWidget::customContextMenuRequested, this, &SE_TaskModelARDRuleDlg::showContextMenu);

    // 规则节点仅“质检规则数值”列可编辑；严重程度下拉框常驻显示，无需双击。
    connect(ui.treeWidget_TaskModelData, &QTreeWidget::itemDoubleClicked,
        this, [=](QTreeWidgetItem* item, int column) {
            if (GetItemLevel(item) >= 2 && column == 5) {
                ui.treeWidget_TaskModelData->editItem(item, column);
            }
        });

	restoreState();
	ui.lineEdit_InputDataPath->clear();
	ui.lineEdit_OutputDataPath->setText(m_qstrOutputDataPath);

	// 颜色交叉
	ui.treeWidget_TaskModelData->setAlternatingRowColors(true);
}

void SE_TaskModelARDRuleDlg::initializeFileManagerLayout()
{
	setWindowTitle(tr("质检方案配置"));
	resize(1420, 860);
	setMinimumSize(1120, 680);

	ui.groupBox_2->setTitle(tr("质检方案"));
	ui.groupBox_3->setTitle(tr("质检方案"));
	ui.groupBox_4->setTitle(tr("保存当前方案"));
	ui.pushButton_Open->setText(tr("添加文件"));
	ui.pushButton_newfile->setText(tr("新建"));
	ui.pushButton_Save->setText(tr("另存当前方案"));
	ui.lineEdit_InputDataPath->setReadOnly(true);
	ui.lineEdit_InputDataPath->setPlaceholderText(tr("选择左侧文件后显示完整路径"));
	ui.lineEdit_OutputDataPath->setPlaceholderText(tr("另存为路径"));

	m_loadFolderButton = ui.groupBox_2->findChild<QPushButton*>(QStringLiteral("pushButton_LoadFolder"));
	m_configFileList = ui.groupBox_2->findChild<QListWidget*>(QStringLiteral("listWidget_ConfigFiles"));
	m_searchConfigFiles = ui.groupBox_2->findChild<QLineEdit*>(QStringLiteral("lineEdit_SearchConfigFiles"));
	Q_ASSERT(m_loadFolderButton && m_configFileList && m_searchConfigFiles);
	m_configFileList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_configFileList->setAlternatingRowColors(true);
	m_configFileList->setToolTip(tr("点击文件即可在右侧编辑质检方案"));
	m_searchConfigFiles->setClearButtonEnabled(true);
}

// 安全读取XML节点文本，空内容返回空串，避免空指针崩溃
static inline QString readXmlNodeText(xmlDocPtr doc, xmlNodePtr node)
{
    if (!doc || !node) return QString();
    xmlChar* key = xmlNodeListGetString(doc, node->xmlChildrenNode, 1);
    QString text = key ? QString::fromUtf8(reinterpret_cast<const char*>(key)) : QString();
    if (key) xmlFree(key);
    return text;
}
static inline QString sanitizeXmlText(const QString& s)
{
    if (s.isNull() || s.isEmpty()) return QString();
    QString out;
    for (int i = 0; i < s.size(); ++i)
    {
        const QChar c = s[i];
        const ushort u = c.unicode();
        if (u == 0x9 || u == 0xA || u == 0xD || (u >= 0x20 && u <= 0xD7FF) || (u >= 0xE000 && u <= 0xFFFD))
            out.append(c);
    }
    return out;
}

static inline bool isConfiguredSeverity(const QString& severity)
{
    return severity == QStringLiteral("高")
        || severity == QStringLiteral("中")
        || severity == QStringLiteral("低");
}

// 采用图面配置相同的嵌入式控件：下拉框直接占据整个单元格，而非临时编辑器。
static void installSeverityComboBox(QTreeWidget* treeWidget, QTreeWidgetItem* item)
{
    if (!treeWidget || !item) return;

    QComboBox* comboBox = new QComboBox(treeWidget);
    comboBox->setFixedWidth(110);
    comboBox->addItems(QStringList() << QStringLiteral("高")
        << QStringLiteral("中") << QStringLiteral("低"));

    const int severityIndex = comboBox->findText(item->text(2).trimmed());
    comboBox->setCurrentIndex(severityIndex >= 0 ? severityIndex : 1);
    item->setText(2, comboBox->currentText());

    QObject::connect(comboBox,
        static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentIndexChanged),
        comboBox, [item](const QString& severity) {
            item->setText(2, severity);
        });
    treeWidget->setItemWidget(item, 2, comboBox);
}

// 使用Qt QDomDocument解析规则XML，作为更稳健的实现
static bool parseWithQDom(const QString& xmlPath, CheckScheme& outScheme)
{
    outScheme.scaleConfigs.clear();

    QFile file(xmlPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QDomDocument doc;
    QString errorMsg; int errorLine = 0, errorColumn = 0;
    if (!doc.setContent(&file, true, &errorMsg, &errorLine, &errorColumn))
    {
        qWarning() << "XML解析错误:" << errorMsg << "行:" << errorLine << "列:" << errorColumn;
        file.close();
        return false;
    }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != "check_list")
        return false;

    QDomNodeList scaleNodes = root.elementsByTagName("scale");
    // 预分配比例尺数量，避免频繁扩容导致的析构/拷贝开销
    outScheme.scaleConfigs.reserve(scaleNodes.count());
    for (int i = 0; i < scaleNodes.count(); ++i)
    {
        QDomElement scaleElem = scaleNodes.at(i).toElement();
        ScaleConfig scaleCfg;

        QDomElement valueElem = scaleElem.firstChildElement("value");
        scaleCfg.scaleValue = valueElem.isNull() ? QString() : valueElem.text().trimmed();

        QDomNodeList itemNodes = scaleElem.elementsByTagName("check_item");
        // 预分配质检项容量
        scaleCfg.checkItems.reserve(itemNodes.count());
        for (int j = 0; j < itemNodes.count(); ++j)
        {
            QDomElement itemElem = itemNodes.at(j).toElement();
            CheckItem item;

            QDomElement idElem = itemElem.firstChildElement("id");
            if (!idElem.isNull()) item.id = idElem.text().trimmed();

            QDomElement nameElem = itemElem.firstChildElement("name");
            if (!nameElem.isNull()) item.itemName = nameElem.text().trimmed();

            // XML 字段和界面展示名称均使用 severity（严重程度）。
            QDomElement severityElem = itemElem.firstChildElement("severity");
            if (!severityElem.isNull() && isConfiguredSeverity(severityElem.text().trimmed()))
                item.severity = severityElem.text().trimmed();

            QDomNodeList ruleNodes = itemElem.elementsByTagName("rule");
            // 预分配规则容量
            item.rules.reserve(ruleNodes.count());
            for (int k = 0; k < ruleNodes.count(); ++k)
            {
                QDomElement ruleElem = ruleNodes.at(k).toElement();
                CheckRule rule;
                QDomElement rname = ruleElem.firstChildElement("name");
                QDomElement rdesc = ruleElem.firstChildElement("description");
                QDomElement rval = ruleElem.firstChildElement("value");
                QDomElement runit = ruleElem.firstChildElement("unit");
                if (!rname.isNull()) rule.ruleName = rname.text().trimmed();
                if (!rdesc.isNull()) rule.description = rdesc.text().trimmed();
                if (!rval.isNull()) rule.value = rval.text().trimmed();
                if (!runit.isNull()) rule.unit = runit.text().trimmed();
                // 使用就地构造/移动，减少拷贝与析构
                item.rules.emplace_back(std::move(rule));
            }

            scaleCfg.checkItems.emplace_back(std::move(item));
        }

        outScheme.scaleConfigs.emplace_back(std::move(scaleCfg));
    }

    return true;
}

SE_TaskModelARDRuleDlg::~SE_TaskModelARDRuleDlg()
{
	// 设置当前保存路径
	QgsSettings settings;
	settings.setValue(QStringLiteral("TASK_MODEL_ARD_RULE/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("TASK_MODEL_ARD_RULE/OutputDataPath"), m_qstrOutputDataPath, QgsSettings::Section::Plugins);
}

void SE_TaskModelARDRuleDlg::restoreState()
{
	const QgsSettings settings;
	m_qstrInputDataPath = settings.value(QStringLiteral("TASK_MODEL_ARD_RULE/InputDataPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();
	m_qstrOutputDataPath = settings.value(QStringLiteral("TASK_MODEL_ARD_RULE/OutputDataPath"), QDir::homePath(), QgsSettings::Section::Plugins).toString();

}

bool SE_TaskModelARDRuleDlg::isRuleDuplicated(const CheckRules& newRule, const QString& targetScale)
{
    QTreeWidget* treeWidget = ui.treeWidget_TaskModelData;

    // 1. 遍历所有顶层比例尺节点（不再根据比例尺过滤）
    for (int i = 0; i < treeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* scaleItem = treeWidget->topLevelItem(i);

        // 2. 遍历当前比例尺下的所有质检项节点
        for (int j = 0; j < scaleItem->childCount(); ++j)
        {
            QTreeWidgetItem* itemNode = scaleItem->child(j);
            // 匹配质检项ID（待添加规则的ID与现有质检项ID一致）
            if (itemNode->text(0) != newRule.id)
                continue;

            // 3. 遍历当前质检项节点下的所有规则，检查是否重复
            for (int k = 0; k < itemNode->childCount(); ++k)
            {
                QTreeWidgetItem* existRuleNode = itemNode->child(k);
                // 判定标准：质检规则名称完全一致（可根据需求增加“规则数值/单位”等维度）
                if (existRuleNode->text(3) == newRule.ruleName)
                {
                    return true; // 找到重复规则
                }
            }
        }
    }

	return false; // 无重复规则
}


/*加载配置文件*/
void SE_TaskModelARDRuleDlg::pushButton_Open_clicked()
{
	QString curPath = QCoreApplication::applicationDirPath();
	QStringList fileNames = QFileDialog::getOpenFileNames(
		this,
		tr("选择一个或多个质检方案配置文件"),
		curPath,
		tr("XML 文件 (*.xml)")
	);

	if (!fileNames.isEmpty())
		addConfigFiles(fileNames);
}

void SE_TaskModelARDRuleDlg::pushButton_LoadFolder_clicked()
{
	const QString folderPath = QFileDialog::getExistingDirectory(
		this, tr("选择质检方案文件夹"), QCoreApplication::applicationDirPath(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (folderPath.isEmpty())
		return;

	QStringList xmlFiles;
	QDirIterator it(folderPath, QStringList() << QStringLiteral("*.xml"),
		QDir::Files, QDirIterator::Subdirectories);
	while (it.hasNext())
		xmlFiles.append(it.next());
	xmlFiles.sort(Qt::CaseInsensitive);

	if (xmlFiles.isEmpty())
	{
		QMessageBox::information(this, tr("没有找到文件"),
			tr("所选文件夹及其子文件夹中没有 XML 文件。"));
		return;
	}
	addConfigFiles(xmlFiles);
}

void SE_TaskModelARDRuleDlg::addConfigFiles(const QStringList& filePaths, bool selectFirst)
{
	QStringList rejectedFiles;
	int firstAddedRow = -1;
	int existingRow = -1;
	QSignalBlocker blocker(m_configFileList);

	for (const QString& inputPath : filePaths)
	{
		QFileInfo fileInfo(inputPath);
		if (!fileInfo.exists() || !fileInfo.isFile()
			|| fileInfo.suffix().compare(QStringLiteral("xml"), Qt::CaseInsensitive) != 0)
		{
			rejectedFiles.append(inputPath);
			continue;
		}

		const QString filePath = QDir::cleanPath(fileInfo.absoluteFilePath());
		bool alreadyListed = false;
		for (int row = 0; row < m_configFileList->count(); ++row)
		{
			if (m_configFileList->item(row)->data(Qt::UserRole).toString().compare(
				filePath, Qt::CaseInsensitive) == 0)
			{
				alreadyListed = true;
				existingRow = row;
				break;
			}
		}
		if (alreadyListed)
			continue;

		QListWidgetItem* item = new QListWidgetItem(fileInfo.fileName(), m_configFileList);
		item->setData(Qt::UserRole, filePath);
		item->setToolTip(filePath);
		if (firstAddedRow < 0)
			firstAddedRow = m_configFileList->row(item);
	}

	const int selectedRow = selectFirst
		? (firstAddedRow >= 0 ? firstAddedRow : existingRow)
		: -1;
	if (selectedRow >= 0)
		m_configFileList->setCurrentRow(selectedRow);
	else if (m_currentConfigFilePath.isEmpty())
		m_configFileList->setCurrentRow(-1);

	// Selection is deliberately applied after releasing the blocker so the normal
	// file-switch path can protect edits in the currently open scheme.
	blocker.unblock();
	if (selectedRow >= 0)
		onConfigFileSelectionChanged(selectedRow);

	if (!rejectedFiles.isEmpty())
	{
		QMessageBox::warning(this, tr("部分文件未加载"),
			tr("以下路径不是有效的 XML 文件，已跳过：\n%1")
				.arg(rejectedFiles.join(QStringLiteral("\n"))));
	}
}

void SE_TaskModelARDRuleDlg::onConfigFileSelectionChanged(int row)
{
	if (row < 0 || row >= m_configFileList->count())
		return;

	QListWidgetItem* selectedItem = m_configFileList->item(row);
	const QString filePath = selectedItem->data(Qt::UserRole).toString();
	if (filePath.isEmpty() || filePath == m_currentConfigFilePath)
		return;

	if (!confirmPendingChanges())
	{
		int previousRow = -1;
		for (int i = 0; i < m_configFileList->count(); ++i)
		{
			if (m_configFileList->item(i)->data(Qt::UserRole).toString() == m_currentConfigFilePath)
			{
				previousRow = i;
				break;
			}
		}
		const QSignalBlocker blocker(m_configFileList);
		m_configFileList->setCurrentRow(previousRow);
		return;
	}

	CheckScheme loadedScheme;
	const QByteArray localPath = filePath.toLocal8Bit();
	if (!LoadXmlConfigFile(localPath.constData(), loadedScheme))
	{
		QMessageBox::critical(this, tr("加载失败"),
			tr("无法读取质检方案文件：\n%1").arg(filePath));
		int previousRow = -1;
		for (int i = 0; i < m_configFileList->count(); ++i)
		{
			if (m_configFileList->item(i)->data(Qt::UserRole).toString() == m_currentConfigFilePath)
			{
				previousRow = i;
				break;
			}
		}
		const QSignalBlocker blocker(m_configFileList);
		m_configFileList->setCurrentRow(previousRow);
		return;
	}

	m_loadingConfig = true;
	m_checkScheme = std::move(loadedScheme);
	m_currentConfigFilePath = filePath;
	m_qstrInputDataPath = filePath;
	m_qstrOutputDataPath = filePath;
	m_schemeName = QFileInfo(filePath).baseName();
	if (m_newXmlFilePath != filePath)
		m_newXmlFilePath.clear();
	ui.lineEdit_InputDataPath->setText(filePath);
	ui.lineEdit_OutputDataPath->setText(filePath);
	InitTreeWidget();
	m_hasUnsavedChanges = false;
	m_loadingConfig = false;
}

bool SE_TaskModelARDRuleDlg::confirmPendingChanges()
{
	if (!m_hasUnsavedChanges)
		return true;

	QMessageBox messageBox(this);
	messageBox.setWindowTitle(tr("方案尚未保存"));
	messageBox.setText(tr("当前质检方案有未保存的修改。"));
	messageBox.setInformativeText(tr("切换文件前要保存这些修改吗？"));
	QPushButton* saveButton = messageBox.addButton(QMessageBox::Save);
	QPushButton* discardButton = messageBox.addButton(QMessageBox::Discard);
	messageBox.addButton(QMessageBox::Cancel);
	messageBox.setDefaultButton(saveButton);
	messageBox.exec();

	if (messageBox.clickedButton() == saveButton)
		return SaveXMLConfig();
	if (messageBox.clickedButton() == discardButton)
	{
		m_hasUnsavedChanges = false;
		return true;
	}
	return false;
}



/*保存配置文件*/
void SE_TaskModelARDRuleDlg::pushButton_Save_clicked()
{
	SaveXMLConfig();
}

void SE_TaskModelARDRuleDlg::pushButton_OK_clicked()
{
	// 如果文件不存在
	if (!CheckFileOrDirExist(ui.lineEdit_InputDataPath->text()))
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle(tr("质检方案配置工具"));
		msgBox.setText(tr("文件：%1不存在！").arg(ui.lineEdit_InputDataPath->text()));
		msgBox.setStandardButtons(QMessageBox::Yes);
		msgBox.setDefaultButton(QMessageBox::Yes);
		msgBox.exec();
		return;
	}

	// 如果文件不存在
	if (!CheckFileOrDirExist(ui.lineEdit_OutputDataPath->text()))
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle(tr("质检方案配置工具"));
		msgBox.setText(tr("文件：%1不存在！").arg(ui.lineEdit_OutputDataPath->text()));
		msgBox.setStandardButtons(QMessageBox::Yes);
		msgBox.setDefaultButton(QMessageBox::Yes);
		msgBox.exec();
		return;
	}



	// 设置当前保存路径
	QgsSettings settings;
	settings.setValue(QStringLiteral("TASK_MODEL_ARD_RULE/InputDataPath"), m_qstrInputDataPath, QgsSettings::Section::Plugins);
	settings.setValue(QStringLiteral("TASK_MODEL_ARD_RULE/OutputDataPath"), m_qstrOutputDataPath, QgsSettings::Section::Plugins);
	accept();
}

/*增加质检项按钮 - 严格匹配目标布局，从对话框选择内容填充*/
void SE_TaskModelARDRuleDlg::pushButton_AddCheckItem_clicked()
{
	// 提取lineEdit_InputDataPath的文件名
	QString inputFilePath = ui.lineEdit_InputDataPath->text().trimmed();
	QString standardLayerName; // 存储无后缀的文件名（即标准图层名称）
	if (!inputFilePath.isEmpty())
	{
		QFileInfo fileInfo(inputFilePath);
		standardLayerName = fileInfo.baseName(); // 获取文件名（不含路径和后缀）
	}

	// 实例化规则选择对话框
	RuleSelectionDialog ruleDialog(this, QList<CheckRules>(), standardLayerName);

	// 显示对话框并处理结果
	if (ruleDialog.exec() == QDialog::Accepted)
	{
        QList<CheckRules> selectedRules = ruleDialog.getSelectedRules();
        QTreeWidget* treeWidget = ui.treeWidget_TaskModelData;

        // 2. 循环处理每条选中的规则
        foreach(const CheckRules & selectedRule, selectedRules)
        {
            QString targetId = selectedRule.id;
            QString targetItemName = selectedRule.itemName;

            // 重复规则校验
            if (isRuleDuplicated(selectedRule, QString()))
            {
                // 弹出重复提示，跳过当前规则的添加
                QMessageBox::warning(this, tr("规则重复"),
                    tr("此质检规则已存在，无需重复添加！"));
                continue; // 跳过当前规则，处理下一条
            }

            // 查找是否存在重复质检项
            QTreeWidgetItem* existingScaleItem = nullptr; // 已存在的比例尺节点
            QTreeWidgetItem* existingItemNode = nullptr;  // 已存在的质检项节点

            for (int i = 0; i < treeWidget->topLevelItemCount(); ++i)
            {
                QTreeWidgetItem* scaleItem = treeWidget->topLevelItem(i);
                for (int j = 0; j < scaleItem->childCount(); ++j)
                {
                    QTreeWidgetItem* itemNode = scaleItem->child(j);
                    if (itemNode->text(0) == targetId && itemNode->text(1) == targetItemName)
                    {
                        existingScaleItem = scaleItem;
                        existingItemNode = itemNode;
                        break;
                    }
                }
                if (existingItemNode) break;
            }

			// 合并规则或新建节点
            if (existingItemNode)
            {
                // 情况1：存在重复（比例尺+id+质检项都匹配）→ 仅合并规则，不新建质检项
                QTreeWidgetItem* newRuleNode = new QTreeWidgetItem(QStringList{
                    "", "", "",
                    selectedRule.ruleName,
                    selectedRule.description,
                    selectedRule.value,
                    selectedRule.unit
                    });
                newRuleNode->setFlags(newRuleNode->flags() | Qt::ItemIsEditable);
                newRuleNode->setData(5, Qt::UserRole, QVariant(true));
                existingItemNode->addChild(newRuleNode); // 合并到现有质检项节点下
                existingItemNode->setExpanded(true);     // 展开节点方便查看
                treeWidget->setCurrentItem(newRuleNode); // 选中新添加的规则
            }
            else
            {
                // 情况2：无重复 → 按原逻辑新建比例尺→质检项→规则节点
                QTreeWidgetItem* scaleItem = existingScaleItem;
                // 2.1 新建比例尺节点（若不存在匹配的比例尺）
                if (!scaleItem)
                {
                    scaleItem = new QTreeWidgetItem(QStringList{
                        "", "", "", "", "", "", ""
                        });
                    treeWidget->addTopLevelItem(scaleItem);
                }
                scaleItem->setExpanded(true);

                // 2.2 新建质检项节点
                QTreeWidgetItem* itemNode = new QTreeWidgetItem(QStringList{
                    targetId,
                    targetItemName,
                    "中", "", "", "", ""
                    });
                itemNode->setFlags(itemNode->flags() | Qt::ItemIsEditable);
                scaleItem->addChild(itemNode);
                itemNode->setExpanded(true);
                installSeverityComboBox(treeWidget, itemNode);

                // 2.3 新建规则节点
                QTreeWidgetItem* ruleNode = new QTreeWidgetItem(QStringList{
                    "", "", "",
                    selectedRule.ruleName,
                    selectedRule.description,
                    selectedRule.value,
                    selectedRule.unit
                    });
                ruleNode->setFlags(ruleNode->flags() | Qt::ItemIsEditable);
                ruleNode->setData(5, Qt::UserRole, QVariant(true));
                itemNode->addChild(ruleNode);

                // 选中并编辑质检项名称
                //treeWidget->setCurrentItem(itemNode);
                //treeWidget->editItem(itemNode, 2);
            }
		}
		// 选中最后一个添加的节点（可选）
		if (!selectedRules.isEmpty())
		{
			// 可以添加逻辑定位到最后一个添加的节点
		}

		ui.lineEdit_CheckItemName->clear();
	}
}

/*删除质检项按钮*/
void SE_TaskModelARDRuleDlg::pushButton_DeleteCheckItem_clicked()
{
	// 获取当前选中项
	QTreeWidgetItem* currentItem = ui.treeWidget_TaskModelData->currentItem();
	if (!currentItem)
	{
		QMessageBox msgBox;
		msgBox.setWindowTitle(tr("质检方案配置工具"));
		msgBox.setText(tr("请选择要删除的节点！"));
		msgBox.setStandardButtons(QMessageBox::Yes);
		msgBox.setDefaultButton(QMessageBox::Yes);
		msgBox.exec();
		return;
	}

	// 1. 记录当前节点的层级和相关父节点
	int currentLevel = GetItemLevel(currentItem); // 0=比例尺，1=质检项，≥2=规则
	QTreeWidgetItem* parentItem = currentItem->parent();         // 当前节点的父节点
	QTreeWidgetItem* grandParentItem = parentItem ? parentItem->parent() : nullptr; // 祖父节点
	QTreeWidgetItem* scaleItem = nullptr;     // 比例尺节点
	QTreeWidgetItem* checkItem = nullptr;     // 质检项节点

	// 2. 确定各层级节点
	if (currentLevel == 0)
	{
		// 情况1：删除的是比例尺节点
		deleteItem(currentItem);
		return;
	}
	else if (currentLevel == 1)
	{
		// 情况2：删除的是质检项节点
		checkItem = currentItem;
		scaleItem = parentItem;
	}
	else if (currentLevel >= 2)
	{
		// 情况3：删除的是规则节点
		checkItem = parentItem;          // 规则的父节点是质检项
		scaleItem = grandParentItem;     // 质检项的父节点是比例尺
	}

	// 3. 先删除当前选中的节点
	deleteItem(currentItem);

	// 4. 检查：如果删除的是规则节点，且质检项已无其他规则，则删除质检项
	if (currentLevel >= 2 && checkItem)
	{
		// 若质检项下已无规则节点
		if (checkItem->childCount() == 0)
		{
			// 保存质检项的父节点（比例尺）以便后续检查
			scaleItem = checkItem->parent();
			// 删除空质检项
			deleteItem(checkItem);
		}
	}

	// 5. 检查：如果质检项被删除后比例尺已无其他质检项，则删除比例尺
	if (scaleItem)
	{
		// 若比例尺下已无质检项节点
		if (scaleItem->childCount() == 0)
		{
			// 从树控件中移除并删除空比例尺节点
			int topLevelIndex = ui.treeWidget_TaskModelData->indexOfTopLevelItem(scaleItem);
			if (topLevelIndex != -1)
			{
				ui.treeWidget_TaskModelData->takeTopLevelItem(topLevelIndex);
				delete scaleItem;
			}
		}
	}
}

void SE_TaskModelARDRuleDlg::pushButton_Query_clicked()
{
	QString queryText = ui.lineEdit_CheckItemName->text().trimmed();
	if (queryText.isEmpty())
	{
		QMessageBox::information(this, tr("提示"), tr("请输入查找内容"));
		return;
	}

	// 定义用于重置节点背景色的函数对象，明确类型
	std::function<void(QTreeWidgetItem*)> resetNode;
	resetNode = [&](QTreeWidgetItem* item) {
		if (!item) return;
		// 重置当前节点所有列
		for (int col = 0; col < item->treeWidget()->columnCount(); ++col)
		{
			item->setBackground(col, QBrush());
		}
		// 递归处理子节点
		for (int i = 0; i < item->childCount(); ++i)
		{
			resetNode(item->child(i));
		}
	};

	// 重置所有顶层节点
	for (int i = 0; i < ui.treeWidget_TaskModelData->topLevelItemCount(); ++i)
	{
		resetNode(ui.treeWidget_TaskModelData->topLevelItem(i));
	}

	// 递归查找所有节点和列
	int matchCount = 0;
	std::function<int(QTreeWidgetItem*)> searchNode = [&](QTreeWidgetItem* item) {
		if (!item) return 0;

		int count = 0;
		bool hasMatch = false;

		// 检查当前节点的所有列
		for (int col = 0; col < item->treeWidget()->columnCount(); ++col)
		{
			if (item->text(col).contains(queryText, Qt::CaseInsensitive))
			{
				item->setBackground(col, QColor(255, 255, 153)); // 浅黄色高亮
				count++;
				hasMatch = true;
			}
		}

		// 如果有匹配项，展开所有父节点
		if (hasMatch)
		{
			QTreeWidgetItem* parent = item->parent();
			while (parent)
			{
				parent->setExpanded(true);
				parent = parent->parent();
			}
		}

		// 递归查找子节点
		for (int i = 0; i < item->childCount(); ++i)
		{
			count += searchNode(item->child(i));
		}

		return count;
	};

	// 开始查找所有顶层节点
	for (int i = 0; i < ui.treeWidget_TaskModelData->topLevelItemCount(); ++i)
	{
		matchCount += searchNode(ui.treeWidget_TaskModelData->topLevelItem(i));
	}

	// 显示查找结果
	QMessageBox msgBox;
	msgBox.setWindowTitle(tr("查找结果"));
	if (matchCount == 0)
	{
		msgBox.setText(tr("未找到包含 \"%1\" 的内容").arg(queryText));
	}
	else
	{
		msgBox.setText(tr("找到 %1 个匹配项").arg(matchCount));
	}
	msgBox.setStandardButtons(QMessageBox::Yes);
	msgBox.exec();
}


void SE_TaskModelARDRuleDlg::pushButton_Cancel_clicked()
{
	reject();
}



// 读取属性检查配置信息
bool SE_TaskModelARDRuleDlg::LoadXmlConfigFile(const char* szConfigXmlFile, CheckScheme& scheme)
{
    // 如果xml路径为空
    if (!szConfigXmlFile)
        return false;

    // 优先使用Qt DOM解析，更稳健且与保存逻辑一致
    QString xmlPath = QString::fromLocal8Bit(szConfigXmlFile);
    if (parseWithQDom(xmlPath, scheme))
        return true;

    // Fallback：若Qt DOM解析失败，再尝试libxml解析（保留原逻辑）
    scheme.scaleConfigs.clear();
    xmlDocPtr doc = xmlParseFile(szConfigXmlFile);
    if (doc == nullptr) return false;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == nullptr) { xmlFreeDoc(doc); return false; }
    for (xmlNodePtr cur = root->xmlChildrenNode; cur != nullptr; cur = cur->next)
    {
        if (!xmlStrcmp(cur->name, (const xmlChar*)"scale"))
        {
            ScaleConfig config;
            Parse_scale(doc, cur, config);
            scheme.scaleConfigs.push_back(config);
        }
    }
    xmlFreeDoc(doc);
    return true;
}

/*解析scale节点*/
void SE_TaskModelARDRuleDlg::Parse_scale(xmlDocPtr doc, xmlNodePtr cur, ScaleConfig& info)
{
    cur = cur->xmlChildrenNode;
    while (cur != NULL)
    {
        // 从<value>标签读取比例尺数值（关键修改）
        if ((!xmlStrcmp(cur->name, (const xmlChar*)"value")))
        {
            QString val = readXmlNodeText(doc, cur);
            info.scaleValue = val; // 允许为空（表示与比例尺无关）
        }
        // 质检项解析保持不变
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"check_item")))
        {
            CheckItem item;
            Parse_check_item(doc, cur, item);
            info.checkItems.push_back(item);
        }

		cur = cur->next;
	}
}

/*解析check_item节点*/
void SE_TaskModelARDRuleDlg::Parse_check_item(xmlDocPtr doc, xmlNodePtr cur, CheckItem& info)
{
    cur = cur->xmlChildrenNode;
    while (cur != nullptr)
    {
        // 质检项名称
        if ((!xmlStrcmp(cur->name, (const xmlChar*)"name")))
        {
            QString name = readXmlNodeText(doc, cur);
            info.itemName = name;
        }
        // 新增：解析check_item节点下的id子节点
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"id")))
        {
            QString idText = readXmlNodeText(doc, cur);
            info.id = idText; // 允许为空
        }

        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"severity")))
        {
            const QString severity = readXmlNodeText(doc, cur).trimmed();
            if (isConfiguredSeverity(severity)) info.severity = severity;
        }

        // 质检规则
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"rule")))
        {
            CheckRule rule;
            Parse_rule(doc, cur, rule);
            info.rules.push_back(rule);
        }

		cur = cur->next;
	}
}


/*解析rule节点*/
void SE_TaskModelARDRuleDlg::Parse_rule(xmlDocPtr doc, xmlNodePtr cur, CheckRule& info)
{
    cur = cur->xmlChildrenNode;
    while (cur != nullptr)
    {
        // 规则名称
        if ((!xmlStrcmp(cur->name, (const xmlChar*)"name")))
        {
            info.ruleName = readXmlNodeText(doc, cur);
        }

        // 规则描述
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"description")))
        {
            info.description = readXmlNodeText(doc, cur);
        }

        // 规则值
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"value")))
        {
            info.value = readXmlNodeText(doc, cur);
        }

        // 规则单位
        else if ((!xmlStrcmp(cur->name, (const xmlChar*)"unit")))
        {
            info.unit = readXmlNodeText(doc, cur);
        }

        cur = cur->next;
    }
}

/*初始化树控件*/
void SE_TaskModelARDRuleDlg::InitTreeWidget()
{
    // 清空树控件并设置列数
    ui.treeWidget_TaskModelData->clear();
    ui.treeWidget_TaskModelData->setColumnCount(7);
    ui.treeWidget_TaskModelData->setHeaderLabels(QStringList() << "比例尺 / id" << "质检项" << "严重程度" << "质检规则名称" << "质检规则描述" << "质检规则数值" << "质检规则单位");
    // 严重程度仅含高/中/低，但需保证下拉框和文字始终完整显示。
    ui.treeWidget_TaskModelData->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui.treeWidget_TaskModelData->setColumnWidth(2, 120);

	// 填充比例尺节点数据（包含id）
	for (int i = 0; i < m_checkScheme.scaleConfigs.size(); ++i)
	{
		const ScaleConfig& scale = m_checkScheme.scaleConfigs[i];
        QTreeWidgetItem* scaleItem = new QTreeWidgetItem(QStringList()
            << scale.scaleValue << "" << "" << "" << "" << "" << "");
		scaleItem->setFlags(scaleItem->flags() | Qt::ItemIsEditable);
		scaleItem->setData(0, Qt::UserRole, QVariant(scale.scaleValue));
		ui.treeWidget_TaskModelData->addTopLevelItem(scaleItem);

		// 填充质检项节点（子节点id列留空，继承顶级id）
		for (int j = 0; j < scale.checkItems.size(); ++j)
		{
            QTreeWidgetItem* item = new QTreeWidgetItem(QStringList()
                << scale.checkItems[j].id
                << scale.checkItems[j].itemName
                << scale.checkItems[j].severity
                << "" << "" << "" << "");
			item->setFlags(item->flags() | Qt::ItemIsEditable);
			scaleItem->addChild(item);
			installSeverityComboBox(ui.treeWidget_TaskModelData, item);

			// 填充规则节点
			for (int k = 0; k < scale.checkItems[j].rules.size(); ++k)
			{
				const CheckRule& rule = scale.checkItems[j].rules[k];
                QTreeWidgetItem* ruleItem = new QTreeWidgetItem(QStringList()
                    << "" << "" << ""  // id、质检项、严重程度列留空
                    << rule.ruleName
                    << rule.description
                    << rule.value
                    << rule.unit);
				// 仅设置第六列（质检规则数值）可编辑
				//ruleItem->setFlags(ruleItem->flags() | Qt::ItemIsEditable);
				//ruleItem->setData(5, Qt::EditRole, rule.value);
				// 规则节点仅第六列可编辑（通过双击事件控制）
                ruleItem->setFlags(ruleItem->flags() | Qt::ItemIsEditable);
                item->addChild(ruleItem);
            }
		}
		scaleItem->setExpanded(true);
	}

    ui.treeWidget_TaskModelData->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    ui.treeWidget_TaskModelData->header()->setDefaultAlignment(Qt::AlignHCenter);
}

/*从树控件获取质检方案信息*/
void SE_TaskModelARDRuleDlg::GetCheckSchemeFromTreeWidget(CheckScheme& scheme)
{
	scheme.scaleConfigs.clear();
	// 遍历所有顶层比例尺节点（0级）
	for (int i = 0; i < ui.treeWidget_TaskModelData->topLevelItemCount(); ++i)
	{
		QTreeWidgetItem* scaleItem = ui.treeWidget_TaskModelData->topLevelItem(i);
		ScaleConfig scaleConfig;
    scaleConfig.scaleValue = scaleItem->text(0).trimmed();
		// 遍历质检项节点（1级）
		for (int j = 0; j < scaleItem->childCount(); ++j)
		{
			QTreeWidgetItem* item = scaleItem->child(j);
            CheckItem checkItem;
            checkItem.itemName = item->text(1);
            checkItem.id = item->text(0);
            checkItem.severity = item->text(2).trimmed();

			// 读取子规则节点数据（如果有）
			for (int k = 0; k < item->childCount(); ++k)
			{
				QTreeWidgetItem* ruleItem = item->child(k);
				CheckRule subRule;
                subRule.ruleName = ruleItem->text(3);
                subRule.description = ruleItem->text(4);
                subRule.value = ruleItem->text(5);
                subRule.unit = ruleItem->text(6);
				checkItem.rules.push_back(subRule);
			}

			scaleConfig.checkItems.push_back(checkItem);
		}

		scheme.scaleConfigs.push_back(scaleConfig);
	}
}


/*保存XML配置文件*/
bool SE_TaskModelARDRuleDlg::SaveXMLConfig()
{
    // 直接从树控件读取，避免中间拷贝造成的潜在字符串损坏
    QTreeWidget* tree = ui.treeWidget_TaskModelData;

    // 统计是否存在任何规则节点
    int totalRuleCount = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* scaleItem = tree->topLevelItem(i);
        for (int j = 0; j < scaleItem->childCount(); ++j)
        {
            QTreeWidgetItem* itemNode = scaleItem->child(j);
            totalRuleCount += itemNode->childCount();
        }
    }
    if (totalRuleCount == 0)
    {
        QMessageBox msgBox;
        msgBox.setWindowTitle(tr("质检方案配置工具"));
        msgBox.setText(tr("质检方案配置规则数为0，无法保存！"));
        msgBox.setStandardButtons(QMessageBox::Yes);
        msgBox.exec();
        return false;
    }

    QDomDocument doc;
    QDomProcessingInstruction instruction = doc.createProcessingInstruction("xml", "version=\"1.0\" encoding=\"UTF-8\"");
    doc.appendChild(instruction);

    QDomElement root = doc.createElement("check_list");
    doc.appendChild(root);

    for (int i = 0; i < tree->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* scaleItem = tree->topLevelItem(i);
        QDomElement scaleNode = doc.createElement("scale");
        root.appendChild(scaleNode);

        QDomElement valueNode = doc.createElement("value");
        valueNode.appendChild(doc.createTextNode(sanitizeXmlText(scaleItem->text(0).trimmed())));
        scaleNode.appendChild(valueNode);

        for (int j = 0; j < scaleItem->childCount(); ++j)
        {
            QTreeWidgetItem* itemNodeTree = scaleItem->child(j);
            QDomElement itemNode = doc.createElement("check_item");
            scaleNode.appendChild(itemNode);

            const QString idText = itemNodeTree->text(0);
            if (!idText.isEmpty())
            {
                QDomElement idNode = doc.createElement("id");
                idNode.appendChild(doc.createTextNode(sanitizeXmlText(idText)));
                itemNode.appendChild(idNode);
            }

            const QString nameText = itemNodeTree->text(1);
            QDomElement nameNode = doc.createElement("name");
            nameNode.appendChild(doc.createTextNode(sanitizeXmlText(nameText)));
            itemNode.appendChild(nameNode);

            const QString severityText = itemNodeTree->text(2).trimmed();
            if (!isConfiguredSeverity(severityText))
            {
                QMessageBox::warning(this, tr("严重程度配置错误"),
                    tr("质检项“%1”的严重程度只能选择：高、中、低。")
                        .arg(nameText));
                return false;
            }

            // XML 中使用 severity 保存严重程度。
            QDomElement severityNode = doc.createElement("severity");
            severityNode.appendChild(doc.createTextNode(severityText));
            itemNode.appendChild(severityNode);

            for (int k = 0; k < itemNodeTree->childCount(); ++k)
            {
                QTreeWidgetItem* ruleItemTree = itemNodeTree->child(k);
                QDomElement ruleNode = doc.createElement("rule");
                itemNode.appendChild(ruleNode);

                QDomElement ruleNameNode = doc.createElement("name");
                ruleNameNode.appendChild(doc.createTextNode(sanitizeXmlText(ruleItemTree->text(3))));
                ruleNode.appendChild(ruleNameNode);

                QDomElement descNode = doc.createElement("description");
                descNode.appendChild(doc.createTextNode(sanitizeXmlText(ruleItemTree->text(4))));
                ruleNode.appendChild(descNode);

                QDomElement valNode = doc.createElement("value");
                valNode.appendChild(doc.createTextNode(sanitizeXmlText(ruleItemTree->text(5))));
                ruleNode.appendChild(valNode);

                QDomElement unitNode = doc.createElement("unit");
                unitNode.appendChild(doc.createTextNode(sanitizeXmlText(ruleItemTree->text(6))));
                ruleNode.appendChild(unitNode);
            }
        }
    }

	// -------------------------- 新增：文件名与路径限制逻辑 --------------------------
	QString saveFilePath;
	// 1. 区分“新建文件”和“加载文件”场景
	if (!m_newXmlFilePath.isEmpty())
	{
		// 场景1：新建文件（无初始加载路径）→ 保持原有逻辑，允许自定义文件名
		saveFilePath = m_newXmlFilePath;
	}
	else if (!m_qstrInputDataPath.isEmpty())
	{
		// 场景2：加载已有文件 → 强制文件名与加载文件一致，路径无限制
		QFileInfo inputFileInfo(m_qstrInputDataPath);
		QString fixedFileName = inputFileInfo.fileName(); // 提取加载文件的完整文件名（含后缀）
		QString defaultDir = inputFileInfo.path(); // 默认目录设为加载文件所在目录

		// 弹出文件保存对话框（仅允许修改路径，文件名固定）
		saveFilePath = QFileDialog::getSaveFileName(
			this,
			tr("另存质检方案配置文件（文件名固定为：%1）").arg(fixedFileName),
			QDir(defaultDir).filePath(fixedFileName), // 默认路径+固定文件名
			tr("xml 文件(*.xml);;所有文件(*)")
		);

		if (saveFilePath.isEmpty())
			return false; // 用户取消选择，退出保存

		// 强制文件名：若用户手动修改了文件名，重新拼接“选择的路径+固定文件名”
		QFileInfo selectedFileInfo(saveFilePath);
		QString correctSavePath = QDir(selectedFileInfo.path()).filePath(fixedFileName);
		saveFilePath = correctSavePath;
	}
	else
	{
		// 异常场景：无加载路径也无新建路径 → 提示错误
		QMessageBox::critical(this, tr("错误"), tr("未找到有效的文件路径，无法保存！"));
		return false;
	}

	// 2. 更新输出路径显示（原有逻辑保留）
	if (m_newXmlFilePath.isEmpty())
	{
		ui.lineEdit_OutputDataPath->setText(saveFilePath);
		m_qstrOutputDataPath = saveFilePath;
	}
	// -----------------------------------------------------------------------------

	// 写入文件
	QFile file(saveFilePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
	{
		QMessageBox::critical(this, tr("错误"), tr("无法打开文件写入: %1").arg(file.errorString()));
		return false;
	}

	QTextStream out(&file);
	out.setCodec("UTF-8");
	doc.save(out, 4, QDomNode::EncodingFromTextStream);
	file.close();

	QMessageBox msgBox;
	msgBox.setWindowTitle(tr("成功"));
	msgBox.setText(tr("配置文件已保存至: %1").arg(saveFilePath));
	msgBox.exec();
	m_hasUnsavedChanges = false;
	return true;
}

/*显示右键菜单*/
void SE_TaskModelARDRuleDlg::showContextMenu(const QPoint& pos)
{
	QTreeWidgetItem* item = ui.treeWidget_TaskModelData->itemAt(pos);
	if (item)
	{
		int iItemLevel = GetItemLevel(item);
		QMenu contextMenu(this);

		// 所有级别都可以重命名和删除
		QAction* renameAction = new QAction(tr("重命名"), this);
		connect(renameAction, &QAction::triggered, [=]() {
			if (iItemLevel == 0)
				ui.treeWidget_TaskModelData->editItem(item, 0); // 比例尺节点编辑第0列
			else if (iItemLevel == 1)
				ui.treeWidget_TaskModelData->editItem(item, 1); // 质检项节点编辑第1列
			else
				ui.treeWidget_TaskModelData->editItem(item, 2); // 规则节点编辑第2列
			});
		contextMenu.addAction(renameAction);

		QAction* deleteAction = new QAction(tr("删除"), this);
		connect(deleteAction, &QAction::triggered, [=]() {
			deleteItem(item);
			});
		contextMenu.addAction(deleteAction);

		contextMenu.exec(ui.treeWidget_TaskModelData->viewport()->mapToGlobal(pos));
	}
}

void SE_TaskModelARDRuleDlg::pushButton_newfile_clicked()
{
	if (!confirmPendingChanges())
		return;

	QString curPath = QCoreApplication::applicationDirPath();

	// -------------------------- 第一步：弹出FileNameDialog（无需传入文件名列表） --------------------------
	FileNameDialog fileNameDialog(this); // 移除文件名列表参数
	if (fileNameDialog.exec() != QDialog::Accepted)
	{
		return; // 用户取消选择文件名
	}

	// 获取选中的文件名
	QString selectedFileName = fileNameDialog.getSelectedFileName();
	if (selectedFileName.isEmpty())
	{
		QMessageBox::warning(this, tr("警告"), tr("请选择一个文件名！"));
		return;
	}

	// -------------------------- 后续路径选择、文件创建等逻辑不变 --------------------------
	// 确保文件名以.xml结尾
	if (!selectedFileName.endsWith(".xml", Qt::CaseInsensitive))
	{
		selectedFileName += ".xml";
	}

	// 选择保存目录
	QString savePath = QFileDialog::getExistingDirectory(
		this,
		tr("选择XML文件保存路径"),
		curPath,
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
	);

	if (savePath.isEmpty())
		return;

	QString fullFilePath = QDir(savePath).filePath(selectedFileName);

	// 检查文件是否已存在
	if (QFile::exists(fullFilePath))
	{
		QMessageBox::StandardButton ret = QMessageBox::question(
			this,
			tr("文件已存在"),
			tr("文件 \"%1\" 已存在，是否覆盖？").arg(fullFilePath),
			QMessageBox::Yes | QMessageBox::No
		);
		if (ret == QMessageBox::No)
			return;
	}

	// 创建XML文件并更新路径显示
	QFile file(fullFilePath);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
	{
		QTextStream out(&file);
		out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		out << "<check_list>\n</check_list>";
		file.close();

		m_newXmlFilePath = fullFilePath;
		addConfigFiles(QStringList() << fullFilePath, true);

		QMessageBox::information(this, tr("成功"), tr("新建XML文件成功：\n%1").arg(fullFilePath));
	}
	else
	{
		QMessageBox::critical(this, tr("错误"), tr("无法创建文件：\n%1\n原因：%2").arg(fullFilePath).arg(file.errorString()));
	}
}



/*检查字符串是否在向量中*/
bool SE_TaskModelARDRuleDlg::contains(const vector<QString>& vec, const QString& str)
{
	return std::find(vec.begin(), vec.end(), str) != vec.end();
}

#pragma region  "检查文件或文件夹是否存在"

bool SE_TaskModelARDRuleDlg::CheckFileOrDirExist(const QString& qstrFileDirOrPath)
{
	QFileInfo objFileInfo(qstrFileDirOrPath);
	return objFileInfo.exists(); // 直接检查文件或目录是否存在
}

#pragma endregion
