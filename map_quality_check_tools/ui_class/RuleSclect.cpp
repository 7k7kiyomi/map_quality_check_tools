#include "RuleSclect.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QHeaderView>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QLineEdit>
#include <QLabel>

RuleSelectionDialog::RuleSelectionDialog(QWidget* parent, const QList<CheckRules>& rules, const QString& standardLayerName)
    : QDialog(parent)
{
    m_standardLayerName = standardLayerName; // 保存从XML配置界面传入的文件名（无后缀）

    setWindowTitle(tr("选择质检规则"));
    setMinimumSize(800, 500);
    setModal(true);

    // 创建文件路径显示控件
    QLabel* pathLabel = new QLabel(tr("CSV文件路径:"), this);
    m_csvPathEdit = new QLineEdit(this);
    m_csvPathEdit->setReadOnly(true);
    QPushButton* browseBtn = new QPushButton(tr("浏览..."), this);
    connect(browseBtn, &QPushButton::clicked, this, &RuleSelectionDialog::browseCsvFile);

    // 路径布局
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->addWidget(pathLabel);
    pathLayout->addWidget(m_csvPathEdit);
    pathLayout->addWidget(browseBtn);

    // 创建查找控件
    QLabel* searchLabel = new QLabel(tr("查找:"), this);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("输入查找内容..."));
    m_searchBtn = new QPushButton(tr("查找"), this);
    connect(m_searchBtn, &QPushButton::clicked, this, &RuleSelectionDialog::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &RuleSelectionDialog::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &RuleSelectionDialog::onSearchTextChanged);

    m_ruleTreeWidget = new QTreeWidget(this);
    m_ruleTreeWidget->setColumnCount(2);
    m_ruleTreeWidget->setHeaderLabels(QStringList()
        << tr("id")
        << tr("质检项"));
    m_ruleTreeWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ruleTreeWidget->setAlternatingRowColors(true);
    m_ruleTreeWidget->header()->setSectionResizeMode(QHeaderView::Stretch);

    // 连接选择变化信号
    connect(m_ruleTreeWidget, &QTreeWidget::itemSelectionChanged,
        this, &RuleSelectionDialog::onSelectionChanged);

    // 创建按钮
    QPushButton* okBtn = new QPushButton(tr("确定"), this);
    QPushButton* cancelBtn = new QPushButton(tr("取消"), this);

    connect(okBtn, &QPushButton::clicked, this, &RuleSelectionDialog::onOkClicked);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);

    // 按钮和查找控件的水平布局
    QHBoxLayout* btnAndSearchLayout = new QHBoxLayout();
    btnAndSearchLayout->addWidget(searchLabel);
    btnAndSearchLayout->addWidget(m_searchEdit);
    btnAndSearchLayout->addWidget(m_searchBtn);
    btnAndSearchLayout->addStretch();
    btnAndSearchLayout->addWidget(okBtn);
    btnAndSearchLayout->addWidget(cancelBtn);
    btnAndSearchLayout->setSpacing(10);

    // 主布局
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(pathLayout);
    mainLayout->addWidget(m_ruleTreeWidget);
    mainLayout->addLayout(btnAndSearchLayout);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // 如果有初始规则则加载，否则尝试加载默认CSV
    if (!rules.isEmpty()) {
        loadRules(rules);
    } else {
        // 尝试加载默认CSV，实际使用时可注释掉
        // loadCsvFile("default_rules.csv");
    }
}

// 修改为返回选中的所有规则
QList<CheckRules> RuleSelectionDialog::getSelectedRules() const
{
    return m_selectedRules;
}

void RuleSelectionDialog::browseCsvFile()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        tr("选择CSV配置文件"),
        "",
        tr("CSV文件 (*.csv);;所有文件 (*)")
    );

    if (!filePath.isEmpty()) {
        m_csvPathEdit->setText(filePath);
        if (!loadCsvFile(filePath)) {
            QMessageBox::critical(this, tr("错误"), tr("无法加载CSV文件，请检查文件格式！"));
        }
    }
}

bool RuleSelectionDialog::loadCsvFile(const QString& filePath)
{
    m_ruleTreeWidget->clear();
    m_selectedRules.clear();
    m_itemRulesMap.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, tr("警告"), tr("无法打开文件：%1").arg(file.errorString()));
        return false;
    }

    QTextStream in(&file);
    bool isHeader = true; // 标记是否为表头行

    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.isEmpty())
            continue;

        if (isHeader)
        {
            isHeader = false;
            continue;
        }

        QStringList fields = line.split(",");
        if (fields.size() >= 7)
        {
            QString csvLayerName = fields[0].trimmed();
            if (!m_standardLayerName.isEmpty() && csvLayerName != m_standardLayerName)
            {
                continue;
            }

            CheckRules rule;
            rule.scale = "";
            rule.id = fields[1].trimmed();
            rule.itemName = fields[2].trimmed();
            rule.ruleName = fields[3].trimmed();
            rule.description = fields[4].trimmed();
            rule.value = fields[5].trimmed();
            rule.unit = fields[6].trimmed();

            QString key = rule.id + "|" + rule.itemName;
            m_itemRulesMap[key].append(rule);
        }
    }

    for (auto it = m_itemRulesMap.constBegin(); it != m_itemRulesMap.constEnd(); ++it)
    {
        const QStringList parts = it.key().split("|");
        if (parts.size() == 2)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem();
            item->setText(0, parts[0]);
            item->setText(1, parts[1]);
            item->setData(0, Qt::UserRole, it.key());
            m_ruleTreeWidget->addTopLevelItem(item);
        }
    }

    file.close();
    return true;
}

void RuleSelectionDialog::onSelectionChanged()
{
    m_selectedRules.clear();
    QList<QTreeWidgetItem*> selectedItems = m_ruleTreeWidget->selectedItems();
    foreach(QTreeWidgetItem * item, selectedItems)
    {
        QString key = item->data(0, Qt::UserRole).toString();
        if (m_itemRulesMap.contains(key))
        {
            m_selectedRules.append(m_itemRulesMap.value(key));
        }
    }
}

void RuleSelectionDialog::onOkClicked()
{
    if (m_selectedRules.isEmpty())
    {
        QMessageBox::warning(this, tr("警告"), tr("请选择一条规则！"));
        return;
    }
    accept();
}

void RuleSelectionDialog::loadRules(const QList<CheckRules>& rules)
{
    m_ruleTreeWidget->clear();
    m_selectedRules.clear();
    m_itemRulesMap.clear();

    for (const auto& r : rules)
    {
        QString key = r.id + "|" + r.itemName;
        m_itemRulesMap[key].append(r);
    }

    for (auto it = m_itemRulesMap.constBegin(); it != m_itemRulesMap.constEnd(); ++it)
    {
        const QStringList parts = it.key().split("|");
        if (parts.size() == 2)
        {
            QTreeWidgetItem* item = new QTreeWidgetItem();
            item->setText(0, parts[0]);
            item->setText(1, parts[1]);
            item->setData(0, Qt::UserRole, it.key());
            m_ruleTreeWidget->addTopLevelItem(item);
        }
    }
}

// 查找按钮点击事件
void RuleSelectionDialog::onSearchClicked()
{
    QString searchText = m_searchEdit->text().trimmed();
    highlightSearchResults(searchText);
}

// 搜索文本变化事件
void RuleSelectionDialog::onSearchTextChanged(const QString& text)
{
    // 当文本为空时，清除所有选中状态
    if (text.trimmed().isEmpty())
    {
        m_ruleTreeWidget->clearSelection();
    }
}

// 高亮并选中查找结果
void RuleSelectionDialog::highlightSearchResults(const QString& text)
{
    if (text.isEmpty())
    {
        m_ruleTreeWidget->clearSelection();
        // 重置所有项的前景颜色为默认
        for (int i = 0; i < m_ruleTreeWidget->topLevelItemCount(); ++i)
        {
            QTreeWidgetItem* item = m_ruleTreeWidget->topLevelItem(i);
            for (int col = 0; col < m_ruleTreeWidget->columnCount(); ++col)
            {
                item->setForeground(col, QBrush());
            }
        }
        return;
    }

    // 清除现有选择
    //m_ruleTreeWidget->clearSelection();

    // 先重置所有项的前景颜色为默认
    for (int i = 0; i < m_ruleTreeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = m_ruleTreeWidget->topLevelItem(i);
        for (int col = 0; col < m_ruleTreeWidget->columnCount(); ++col)
        {
            item->setForeground(col, QBrush());
        }
    }

    QList<QTreeWidgetItem*> matchingItems;

    // 遍历所有项查找匹配内容
    for (int i = 0; i < m_ruleTreeWidget->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = m_ruleTreeWidget->topLevelItem(i);
        bool isMatch = false;

        // 检查所有列是否包含查找文本
        for (int col = 0; col < m_ruleTreeWidget->columnCount(); ++col)
        {
            if (item->text(col).contains(text, Qt::CaseInsensitive))
            {
                isMatch = true;
                break;
            }
        }

        if (isMatch)
        {
            matchingItems.append(item);
        }
    }

    // 为匹配项设置红色前景色（可根据需要修改颜色）
    QColor highlightColor(Qt::red);
    foreach(QTreeWidgetItem * item, matchingItems)
    {
        //item->setSelected(true);
        for (int col = 0; col < m_ruleTreeWidget->columnCount(); ++col)
        {
            item->setForeground(col, QBrush(highlightColor));
        }
    }

    // 如果有匹配项，滚动到第一个匹配项
    if (!matchingItems.isEmpty())
    {
        m_ruleTreeWidget->scrollToItem(matchingItems.first());
    }
    else
    {
        QMessageBox::information(this, tr("查找结果"), tr("未找到包含 \"%1\" 的内容").arg(text));
    }
}
