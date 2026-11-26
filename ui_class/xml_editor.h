#ifndef SE_TASK_MODEL_ARD_RULE
#define SE_TASK_MODEL_ARD_RULE

#include <QDialog>

#include "commontype/se_config.h"
#include "GeneratedFiles/Release/ui_xml_editor.h"
#include <vector>

#include "cpl_config.h"
#include "gdal_priv.h"
#include <gdal.h>
#include "cpl_conv.h"
#include "RuleSclect.h"

#include <qstringlist.h>
#include <QListWidget>
/*读取xml头文件*/
#include <libxml/parser.h>
using namespace std;

/*质检规则结构体*/
struct CheckRule
{
	QString ruleName;        // 规则名称
	QString description;     // 规则描述
	QString value;           // 规则值
	QString unit;            // 规则单位

    CheckRule()
	{
		ruleName = "";
		description = "";
		value = "";
		unit = "";
	}
};



/*质检项结构体*/
struct CheckItem
{
	QString itemName;                // 质检项名称
    QString id;           // 新增：比例尺节点的id
	QString severity;                // 严重程度：高 / 中 / 低
	vector<CheckRule> rules;         // 规则列表

	CheckItem()
	{
		itemName = "";
		severity = "中";
		rules.clear();
	}

};


/*比例尺配置结构体*/
struct ScaleConfig
{
    QString scaleValue;              // 比例尺值
    vector<CheckItem> checkItems;    // 质检项列表

    ScaleConfig()
    {
        scaleValue = "";
        checkItems.clear();
    }
};

/*质检方案结构体*/
struct CheckScheme
{
    vector<ScaleConfig> scaleConfigs; // 各比例尺配置

    CheckScheme()
    {
        scaleConfigs.clear();
    }
};



class SE_TaskModelARDRuleDlg : public QDialog
{
	Q_OBJECT

public:
	SE_TaskModelARDRuleDlg(QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
	~SE_TaskModelARDRuleDlg() override;

	void restoreState();



private:

	Ui_Task_Model_ARD_RuleDlg ui;

private slots:

	// 加载配置文件
	void pushButton_Open_clicked();

	// 加载文件夹中的质检方案
	void pushButton_LoadFolder_clicked();

	// 切换当前编辑的质检方案
	void onConfigFileSelectionChanged(int row);

	// 保存配置文件
	void pushButton_Save_clicked();

	// 确定
	void pushButton_OK_clicked();

	// 增加任务
	void pushButton_AddCheckItem_clicked();

	// 删除任务
	void pushButton_DeleteCheckItem_clicked();

	// 查找任务
	void pushButton_Query_clicked();

	// 取消
	void pushButton_Cancel_clicked();

	// 显示右键菜单
	void showContextMenu(const QPoint& pos);
    
    //新建xml
    void pushButton_newfile_clicked();

private:

    // 输入配置文件路径
	QString m_qstrInputDataPath;

	// 输出配置文件路径
	QString m_qstrOutputDataPath;

    // 质检方案信息
    CheckScheme m_checkScheme;

    // 存储质检方案名称（XML文件名）
    QString m_schemeName; 

    // 记录新建 XML 文件的路径，用于直接保存
    QString m_newXmlFilePath;

    // 左侧已加载的配置文件列表
    QListWidget* m_configFileList = nullptr;

    // 左侧方案文件搜索框
    QLineEdit* m_searchConfigFiles = nullptr;

    // 左侧的文件夹加载按钮
    QPushButton* m_loadFolderButton = nullptr;

    // 文件列表加载期间抑制选择事件
    bool m_loadingConfig = false;

    // 当前文件是否有未保存修改
    bool m_hasUnsavedChanges = false;

    // 当前编辑的配置文件路径
    QString m_currentConfigFilePath;

    // 解析scale节点
    void Parse_scale(xmlDocPtr doc, xmlNodePtr cur, ScaleConfig& info);

    // 解析check_item节点
    void Parse_check_item(xmlDocPtr doc, xmlNodePtr cur, CheckItem& info);

    // 解析rule节点
    void Parse_rule(xmlDocPtr doc, xmlNodePtr cur, CheckRule& info);

    // 将质检方案填充到树控件中
    void InitTreeWidget();

    // 从树控件中获取所有质检方案信息
    void GetCheckSchemeFromTreeWidget(CheckScheme& scheme);

    // 加载质检方案配置文件
    bool LoadXmlConfigFile(const char* szConfigXmlFile, CheckScheme& scheme);

    // 保存xml配置文件
    bool SaveXMLConfig();

    // 初始化左右分栏和文件列表
    void initializeFileManagerLayout();

    // 将文件加入左侧列表并选中最后一个有效文件
    void addConfigFiles(const QStringList& filePaths, bool selectFirst = false);

    // 切换文件前处理当前文件的未保存修改
    bool confirmPendingChanges();

    bool isRuleDuplicated(const CheckRules& newRule, const QString& targetScale);

    // 获取当前项的层级
    inline int GetItemLevel(QTreeWidgetItem* item) {
        int level = 0;
        while (item->parent()) {
            item = item->parent();
            level++;
        }
        return level;
    }

    // 删除当前节点下所有的节点
    inline void deleteItem(QTreeWidgetItem* item)
    {
        if (!item) return; // 空指针保护

        // 先从树控件中移除节点
        if (item->parent())
        {
            item->parent()->removeChild(item); // 移除子节点
        }
        else
        {
            // 移除顶层节点
            int index = ui.treeWidget_TaskModelData->indexOfTopLevelItem(item);
            if (index != -1)
            {
                ui.treeWidget_TaskModelData->takeTopLevelItem(index);
            }
        }

        // 递归删除所有子节点
        while (item->childCount() > 0)
        {
            deleteItem(item->child(0));
        }

        // 删除当前节点
        delete item;
    }

    // 获取当前节点下的所有子节点
    inline void GetChildItems(QTreeWidgetItem* item, vector<QString>& vItemNames)
    {
        vItemNames.clear();

        // 如果节点数大于0
        int iChildCount = item->childCount();
        if (iChildCount > 0)
        {
            for (int iIndex = 0; iIndex < iChildCount; ++iIndex)
            {
                QTreeWidgetItem* childItem = item->child(iIndex);
                if (childItem)
                {
                    vItemNames.push_back(childItem->text(0));
                }

            }
        }
    }

    // 从字符串数组中查找指定字符串
    bool contains(const vector<QString>& vec, const QString& str);

    // 检查文件或文件夹是否存在
    bool CheckFileOrDirExist(const QString& qstrFileDirOrPath);

};

#endif // SE_TASK_MODEL_ARD_RULE
