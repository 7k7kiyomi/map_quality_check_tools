#pragma once
#ifndef RULE_SELECTION_DIALOG_H
#define RULE_SELECTION_DIALOG_H

#include <QDialog>
#include <QTreeWidget>
#include <QList>
#include <QString>
#include <QHash>

// 唯一的CheckRule结构体定义
struct CheckRules
{
    QString scale;         // 比例尺
    QString id;            // 规则ID
    QString itemName;      // 质检项
    QString ruleName;      // 规则名称
    QString description;   // 规则描述
    QString value;         // 规则数值
    QString unit;          // 规则单位
};

class RuleSelectionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RuleSelectionDialog(QWidget* parent, const QList<CheckRules>& rules, const QString& standardLayerName);

    // 修改返回类型为QList<CheckRules>以支持多选结果返回
    QList<CheckRules> getSelectedRules() const;

private slots:
    void onSelectionChanged();
    void onOkClicked();
    void browseCsvFile();  // 浏览并选择CSV文件
    void onSearchClicked();  // 查找按钮点击事件
    void onSearchTextChanged(const QString& text);  // 搜索文本变化事件

private:
    QTreeWidget* m_ruleTreeWidget;
    CheckRules m_selectedRule;  // 现在可以正确识别CheckRule
    QLineEdit* m_csvPathEdit;       // 显示CSV文件路径
    QLineEdit* m_searchEdit;  // 查找输入框
    QPushButton* m_searchBtn;  // 查找按钮
    QString m_standardLayerName; // 存储从XML配置界面传入的标准图层名称

  // 将单个选中规则改为列表
    QList<CheckRules> m_selectedRules;
    QHash<QString, QList<CheckRules>> m_itemRulesMap;

    bool loadCsvFile(const QString& filePath);  // 加载CSV文件

    void loadRules(const QList<CheckRules>& rules);
    //QList<CheckRules> getDefaultRules();
    void highlightSearchResults(const QString& text);  // 高亮并选中查找结果

};

#endif // RULE_SELECTION_DIALOG_H
