#pragma once
#include <QtWidgets/QDialog>
#include "GeneratedFiles/Release/ui_Drawing_Layout.h"
#include <QObject>
#include <QItemDelegate> // 代理类基类
#include <QComboBox>      // 下拉框
#include <QStringList>    // 文件名列表

namespace Ui {
	class Drawing_Layout;
}

class Drawing_Layout : public QDialog
{
    Q_OBJECT

public:
    Drawing_Layout(const QString& shpPath, const QString& gdbShpPath, QWidget* parent = nullptr, Qt::WindowFlags fl = Qt::WindowFlags());
    ~Drawing_Layout(void);
protected:
    // 禁用下拉框区域的滚轮事件
    bool eventFilter(QObject* obj, QEvent* event) override;
private slots:

	//加载csv配置文件
	void on_pushButton_loadfile_clicked();
	//配置文件另存
	void on_pushButton_savefile_clicked();
	// 查找按钮点击槽函数
	void on_pushButton_find_clicked(); 
	//支持enter键进行查找
	void on_lineEdit_find_returnPressed();
	//“确认”按钮
	void on_pushButton_ok_clicked();
	//“关闭”按钮点击槽函数
	void on_pushButton_close_clicked();
	//“配置表检查”按钮
	void on_pushButton_check_clicked();

public:
	Ui::Drawing_Layout* ui;
	QString m_lastFindText; // 记录上一次查找的关键词，用于清除历史高亮
	//加载csv文件
	bool loadCsvFile(const QString& filePath);
	//保存配置文件
	bool saveCsvFile(const QString& filePath);

	// 为指定行的指定列创建下拉框（参数：行项、目标列、下拉选项列表）
	//void createComboBoxForCell(QTreeWidgetItem* item, int targetCol, const QStringList& options, const QString& initialText);

	// 创建带复选框的多选下拉框
	void createCheckableComboBox(QTreeWidgetItem* item, int targetCol, const QStringList& options, const QString& initialText);
	// 下拉框所在单元格的显示文本由控件负责绘制；配置值另存于 UserRole，避免底层文字叠绘。
	void setComboCellStoredText(QTreeWidgetItem* item, int column, const QString& value);
	QString comboCellStoredText(QTreeWidgetItem* item, int column) const;
	// 点击标准图层行时，重排实际图层及实际注记图层：已选项优先，其余按名称匹配度排序。
	void sortActualLayerOptions(QTreeWidgetItem* item);
	void sortLayerOptions(QTreeWidgetItem* item, int targetCol);
	int layerNameMatchScore(const QString& standardLayerName, const QString& actualLayerName) const;

	// 新增：存储 shp/gdb_shp 文件夹路径（用于后续创建下拉框时复用）
	QString m_shpPath;
	QString m_gdbShpPath;
	
	// 工具函数：从文件夹读取所有文件的「无后缀名」
	QStringList getFileNamesWithoutSuffix(const QString& folderPath);
	
	// 1. 获取“gdb图层”对应的所有 .shp 完整路径
	//QStringList getGdbShpPaths(QTreeWidgetItem* item);
	// 2. 获取“实际图层”对应的所有 .shp 完整路径
	//QStringList getActualShpPaths(QTreeWidgetItem* item);
	// 3. 获取“实际注记图层”对应的所有 .shp 完整路径
	//QStringList getActualAnnotShpPaths(QTreeWidgetItem* item);
};
