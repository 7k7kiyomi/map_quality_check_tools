#include "Drawing_Layout.h"
#include "ui_Drawing_Layout.h"
#include <QFile>
#include <QFileDialog>
#include <QTextStream>
#include <QMessagebox>
#include <QDir>          // 文件夹操作
#include <QFileInfo>     // 文件信息（提取无后缀名）
#include <QListView>
#include <QStyledItemDelegate>
#include <QHeaderView>
#include <qstandarditemmodel.h>
#include <QSortFilterProxyModel>
#include <QLineEdit>
#include <QPalette>
#include <QBrush>
#include <QRegularExpression>
#include <QFrame>
#include <QVBoxLayout>
#include <QSizeGrip>
#include <QEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QGuiApplication>
#include <QInputMethod>
#include <QSet>
#include <QVector>
#include <QVariant>
#include <qgsmessagelog.h>
#include <algorithm>
#include <functional>

// QTreeWidgetItem 的 DisplayRole 会在嵌入控件下方再次绘制文字；下拉框配置值改存此角色。
static constexpr int kComboCellValueRole = Qt::UserRole + 101;

namespace {

// 多选候选项按整行点击切换，避免只能点中很小的复选框区域。
class CheckableRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool editorEvent(QEvent* event, QAbstractItemModel* model,
                     const QStyleOptionViewItem& option, const QModelIndex& index) override
    {
        if (!model || !index.isValid() || !(index.flags() & Qt::ItemIsUserCheckable)) {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        bool shouldToggle = false;
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            shouldToggle = mouseEvent->button() == Qt::LeftButton && option.rect.contains(mouseEvent->pos());
        } else if (event->type() == QEvent::KeyPress) {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            shouldToggle = keyEvent->key() == Qt::Key_Space || keyEvent->key() == Qt::Key_Select;
        }

        if (!shouldToggle) {
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        const Qt::CheckState currentState = static_cast<Qt::CheckState>(index.data(Qt::CheckStateRole).toInt());
        const Qt::CheckState nextState = currentState == Qt::Checked ? Qt::Unchecked : Qt::Checked;
        model->setData(index, nextState, Qt::CheckStateRole);
        return true;
    }
};

// 保留 QSizeGrip 的拖拽调整逻辑，但不绘制平台默认的浅色背景。
class PopupSizeGrip final : public QSizeGrip
{
public:
    explicit PopupSizeGrip(QWidget* parent = nullptr)
        : QSizeGrip(parent)
    {
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        // 尺寸柄通过鼠标光标和 tooltip 提示，不额外覆盖系统主题颜色。
    }
};

// 多选下拉框使用独立弹出面板：顶部固定检索框，底部为可滚动的复选列表。
class SearchableCheckComboBox final : public QComboBox
{
public:
    explicit SearchableCheckComboBox(QWidget* parent = nullptr)
        : QComboBox(parent)
    {
    }

    void setCheckableSourceModel(QStandardItemModel* sourceModel)
    {
        m_proxyModel = new QSortFilterProxyModel(this);
        m_proxyModel->setSourceModel(sourceModel);
        m_proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_proxyModel->setFilterKeyColumn(0);
        setModel(m_proxyModel);
        if (lineEdit()) {
            lineEdit()->installEventFilter(this);
        }
    }

    ~SearchableCheckComboBox() override
    {
        delete m_popup;
    }

    void setSelectionTextUpdater(const std::function<void()>& updater)
    {
        m_selectionTextUpdater = updater;
    }

    void setBeforeShowCallback(const std::function<void()>& callback)
    {
        m_beforeShowCallback = callback;
    }

    bool isPopupOpen() const
    {
        return m_popupOpen;
    }

    void setFilterText(const QString& text)
    {
        if (m_proxyModel) {
            m_proxyModel->setFilterFixedString(text);
        }
    }

protected:
    void showPopup() override
    {
        if (m_beforeShowCallback) {
            m_beforeShowCallback();
        }
        ensurePopup();
        setProperty("checkablePopupOpen", true);
        m_popupOpen = true;
        setFilterText(QString());
        m_searchEdit->clear();

        const int visibleRows = qMin(10, qMax(1, m_proxyModel->rowCount()));
        const int popupWidth = width();
        const int popupHeight = 42 + visibleRows * 29;
        m_popup->setMinimumWidth(popupWidth);
        if (!m_popupSizeInitialized) {
            // 第一次展开时，面板宽度与所在单元格一致。
            m_popup->resize(popupWidth, popupHeight);
            m_popupSizeInitialized = true;
        } else {
            // 后续展开时仍随单元格宽度变化，同时保留用户额外拉宽的部分。
            const int extraWidth = qMax(0, m_popup->width() - m_lastCellWidth);
            m_popup->resize(popupWidth + extraWidth, m_popup->height());
        }
        m_lastCellWidth = popupWidth;
        m_popup->move(mapToGlobal(QPoint(0, height())));
        m_popup->show();
        m_popup->raise();
        m_popup->activateWindow();
        // 第一次 show 前子控件尚未完成主题初始化，此时读取到的可能是默认浅色调色板。
        // 显示后立即及下一轮事件循环各同步一次，保证首次展开也使用当前系统主题。
        m_popup->ensurePolished();
        m_popupListView->ensurePolished();
        syncPopupBackground();
        QTimer::singleShot(0, this, [this]() {
            if (m_popup && m_popup->isVisible()) {
                syncPopupBackground();
                m_popup->update();
            }
        });
        // 中文等复杂文本不是普通按键文本，而是由输入法通过 QInputMethodEvent 提交。
        // 弹出窗口刚显示时主动激活检索框，并在下一轮事件循环再次同步输入法焦点，
        // 避免 Windows IME 仍把组合输入发送给下拉框本体的只读 lineEdit。
        m_searchEdit->setFocus(Qt::OtherFocusReason);
        QTimer::singleShot(0, this, [this]() {
            if (!m_popup || !m_popup->isVisible() || !m_searchEdit) {
                return;
            }
            m_searchEdit->setFocus(Qt::OtherFocusReason);
            if (QGuiApplication::inputMethod()) {
                QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
            }
        });
    }

    void hidePopup() override
    {
        if (m_popup && m_popup->isVisible()) {
            m_popup->hide();
        } else {
            finishPopup();
        }
    }

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == lineEdit() && event->type() == QEvent::MouseButtonPress) {
            const QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                showPopup();
                return true;
            }
        }
        // Qt::Popup 在点到控件外或按 Esc 时会自行隐藏，统一在此恢复显示文本。
        if (watched == m_popup && event->type() == QEvent::Hide) {
            finishPopup();
        }
        return QComboBox::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        // QTreeWidget 首次点击通常会先处理行选中；此处直接展开，保证一次点击即可使用。
        if (event->button() == Qt::LeftButton) {
            showPopup();
            event->accept();
            return;
        }
        QComboBox::mousePressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QComboBox::resizeEvent(event);
        if (!m_popup || !m_popupSizeInitialized) {
            return;
        }

        // 表格列被拖动改变宽度时，弹出面板也同步变化，且不丢失用户手动拉宽的增量。
        const int previousCellWidth = m_lastCellWidth > 0 ? m_lastCellWidth : event->oldSize().width();
        const int extraWidth = qMax(0, m_popup->width() - previousCellWidth);
        m_popup->setMinimumWidth(width());
        m_popup->resize(width() + extraWidth, m_popup->height());
        m_lastCellWidth = width();
    }

private:
    void syncPopupBackground()
    {
        if (!m_popup || !m_popupListView) {
            return;
        }

        // 外层面板没有专用 QSS 时，直接使用列表当前的 Base 色。
        // 颜色来自运行时主题，不在这里固化任何 RGB 值。
        const QPalette listPalette = m_popupListView->viewport()->palette();
        QPalette popupPalette = m_popup->palette();
        const QBrush listBackground = listPalette.brush(QPalette::Base);
        popupPalette.setBrush(QPalette::Window, listBackground);
        popupPalette.setBrush(QPalette::Base, listBackground);
        m_popup->setPalette(popupPalette);
        m_popup->setAutoFillBackground(true);
        m_popup->setBackgroundRole(QPalette::Window);
    }

    void ensurePopup()
    {
        if (m_popup) {
            return;
        }

        // 保留 Popup 行为，同时挂到当前下拉框；移除系统窗口框，避免出现平台默认的浅色边框。
        m_popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
        m_popup->setObjectName(QStringLiteral("layerMappingPopup"));
        m_popup->setFrameShape(QFrame::NoFrame);
        m_popup->setMinimumHeight(100);
        QVBoxLayout* layout = new QVBoxLayout(m_popup);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        m_searchEdit = new QLineEdit(m_popup);
        m_searchEdit->setPlaceholderText(tr("输入图层名检索"));
        m_searchEdit->setClearButtonEnabled(true);
        m_searchEdit->setFocusPolicy(Qt::StrongFocus);
        m_searchEdit->setInputMethodHints(Qt::ImhNone);
        // QLineEdit 通常会自行启用输入法；这里显式设置，确保它作为自定义
        // Qt::Popup 的焦点代理时仍能接收中文输入法的组合及提交事件。
        m_searchEdit->setAttribute(Qt::WA_InputMethodEnabled, true);
        m_popup->setFocusProxy(m_searchEdit);
        layout->addWidget(m_searchEdit);

        m_popupListView = new QListView(m_popup);
        m_popupListView->setModel(m_proxyModel);
        m_popupListView->setItemDelegate(new CheckableRowDelegate(m_popupListView));
        m_popupListView->setUniformItemSizes(true);
        // 搜索已能快速定位候选；隐藏原生浅色滚动条，鼠标滚轮仍可浏览列表。
        m_popupListView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_popupListView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        layout->addWidget(m_popupListView, 1);

        PopupSizeGrip* sizeGrip = new PopupSizeGrip(m_popup);
        sizeGrip->setToolTip(tr("拖动以调整列表大小"));
        layout->addWidget(sizeGrip, 0, Qt::AlignRight);

        connect(m_searchEdit, &QLineEdit::textChanged, this,
                [this](const QString& filterText) { setFilterText(filterText); });
        m_popup->installEventFilter(this);
    }

    void finishPopup()
    {
        if (!m_popupOpen) {
            return;
        }
        m_popupOpen = false;
        setProperty("checkablePopupOpen", false);
        setFilterText(QString());
        if (m_selectionTextUpdater) {
            m_selectionTextUpdater();
        }
    }

    QSortFilterProxyModel* m_proxyModel = nullptr;
    QFrame* m_popup = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QListView* m_popupListView = nullptr;
    std::function<void()> m_selectionTextUpdater;
    std::function<void()> m_beforeShowCallback;
    bool m_popupOpen = false;
    bool m_popupSizeInitialized = false;
    int m_lastCellWidth = 0;
};

QString normalizedLayerName(const QString& name)
{
    QString normalized = name.trimmed().toLower();
    normalized.remove(QRegularExpression(QStringLiteral("[\\s_\\-]+")));
    return normalized;
}

int longestCommonSubstringLength(const QString& left, const QString& right)
{
    if (left.isEmpty() || right.isEmpty()) {
        return 0;
    }

    QVector<int> previous(right.size() + 1, 0);
    QVector<int> current(right.size() + 1, 0);
    int longest = 0;
    for (int i = 1; i <= left.size(); ++i) {
        for (int j = 1; j <= right.size(); ++j) {
            if (left.at(i - 1) == right.at(j - 1)) {
                current[j] = previous[j - 1] + 1;
                longest = qMax(longest, current[j]);
            } else {
                current[j] = 0;
            }
        }
        previous.swap(current);
        current.fill(0);
    }
    return longest;
}

} // namespace

Drawing_Layout::Drawing_Layout(const QString& shpPath, const QString& gdbShpPath, QWidget* parent, Qt::WindowFlags fl)
    : QDialog(parent, fl)
    , ui(new Ui::Drawing_Layout)
    , m_shpPath(shpPath)  // 保存shp文件夹路径
    , m_gdbShpPath(gdbShpPath)  // 保存gdb_shp文件夹路径
{
    ui->setupUi(this); 

    // Keep the three path/search sections at their single-row size and give
    // all remaining vertical space to the layer-relation table.
    ui->groupBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->groupBox_3->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->groupBox_2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->treeWidget_csv->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    ui->treeWidget_csv->setMinimumHeight(340);
    ui->gridLayout_2->setRowStretch(0, 0);
    ui->gridLayout_2->setRowStretch(1, 1);
    ui->gridLayout_2->setRowStretch(2, 0);
    ui->gridLayout_2->setRowStretch(3, 0);
    ui->gridLayout_2->setRowStretch(4, 0);

    // 设置 QTreeWidget 的列数，根据实际 CSV 文件表头列数调整，这里有6列
    ui->treeWidget_csv->setColumnCount(6);
    // 设置表头
    QStringList headers;
    headers << "标准图层" << "gdb图层" << "实际图层" << "实际注记图层"<<"对应注记层" << "图例字段";
    ui->treeWidget_csv->setHeaderLabels(headers);

    // 下拉框宽度由所在列决定。为 gdb、实际图层及实际注记图层预留更大的初始空间，避免控件文字被截断。
    QHeaderView* header = ui->treeWidget_csv->header();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setMinimumSectionSize(80);
    ui->treeWidget_csv->setColumnWidth(0, 110);
    ui->treeWidget_csv->setColumnWidth(1, 180);
    ui->treeWidget_csv->setColumnWidth(2, 180);
    ui->treeWidget_csv->setColumnWidth(3, 180);
    ui->treeWidget_csv->setColumnWidth(4, 120);
    header->setStretchLastSection(true);

    // 允许“双击单元格”触发编辑（确保用户能触发编辑操作）
    ui->treeWidget_csv->setEditTriggers(QAbstractItemView::DoubleClicked);

    // 选择某个标准图层时，仅调整该行的实际图层候选顺序，不修改任何勾选结果。
    connect(ui->treeWidget_csv, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
                sortActualLayerOptions(current);
            });
    connect(ui->treeWidget_csv, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem* item, int) {
                sortActualLayerOptions(item);
            });
}

Drawing_Layout::~Drawing_Layout()
{
    delete ui;
}

//加载配置文件
void Drawing_Layout::on_pushButton_loadfile_clicked()
{
    QString filePath = QFileDialog::getOpenFileName(this
        , tr("选择CSV文件")
        , ""
        , tr("CSV Files (*.csv)"));
    if (!filePath.isEmpty()) {
        ui->lineEdit_loadfile->setText(filePath);
        if (!loadCsvFile(filePath)) {
            QMessageBox::critical(this, tr("错误"), tr("无法加载CSV文件，请检查文件格式！"));
        }
    }
}

//读取csv文件中的内容
bool Drawing_Layout::loadCsvFile(const QString& filePath)
{
    // 清空现有数据
    ui->treeWidget_csv->clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, tr("警告"), tr("无法打开文件：%1").arg(file.errorString()));
        return false;
    }

    QTextStream in(&file);
    in.setCodec("UTF-8");
    bool isHeader = true; // 标记是否为表头行

    // 预先获取下拉框选项（来自文件夹）
    QStringList gdbShpOptions = getFileNamesWithoutSuffix(m_gdbShpPath);
    QStringList shpOptions = getFileNamesWithoutSuffix(m_shpPath);

    // 已配置名称与目录中的 Shp 名称按“忽略大小写的完整名称”匹配。
    // 只保留实际解析到的文件名：命中时使用磁盘上的实际名称，例如将
    // CSV 中的 BOUP6 更新为 boup6；未命中的旧 CSV 值不回填，使单元格保持空白。
    auto resolveConfiguredValues = [](const QStringList& options, const QString& text) -> QString {
        QStringList selections = text.split("|", Qt::SkipEmptyParts);
        QStringList resolved;
        for (const QString& sel : selections) {
            QString t = sel.trimmed();
            if (t.isEmpty()) continue;

            QString resolvedName;
            for (const QString& option : options) {
                if (option.compare(t, Qt::CaseInsensitive) == 0) {
                    resolvedName = option;
                    break;
                }
            }
            if (!resolvedName.isEmpty() && !resolved.contains(resolvedName, Qt::CaseInsensitive)) {
                resolved << resolvedName;
            }
        }
        return resolved.join("|");
    };

    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.isEmpty())
            continue;

        if (isHeader)
        {
            isHeader = false; // 表头行处理完后，后续行都为数据行
            continue;
        }

        // 按分隔符分割每行数据
        QStringList fields = line.split(",", Qt::KeepEmptyParts);
        while (fields.size() < 6) fields << "";  

        // 创建行项并设置各列初始文本。文件名列只回填目录中已解析到的值。
        QTreeWidgetItem* item = new QTreeWidgetItem(ui->treeWidget_csv);
        item->setText(0, fields[0].trimmed()); // 标准图层
        QStringList rowGdbOptions = gdbShpOptions;
        QString gdbLayerText = resolveConfiguredValues(rowGdbOptions, fields[1].trimmed());
        setComboCellStoredText(item, 1, gdbLayerText);
        QStringList rowActualOptions = shpOptions;
        QString actualLayerText = resolveConfiguredValues(rowActualOptions, fields[2].trimmed());
        setComboCellStoredText(item, 2, actualLayerText);
        QStringList rowAnnotOptions = shpOptions;
        QString actualAnnotLayerText = resolveConfiguredValues(rowAnnotOptions, fields[3].trimmed());
        setComboCellStoredText(item, 3, actualAnnotLayerText);
        // “对应注记层”不要求存在同名 SHP；作为配置值原样保留。
        // 213 的跨类型校验分组实际使用第4列“实际注记图层”。
        QString correspondAnnotLayerText = fields[4].trimmed();
        item->setText(4, correspondAnnotLayerText);
        item->setText(5, fields[5].trimmed()); // 图例字段

        // 设置权限：第0列不可编辑，其他列可编辑
        item->setFlags(item->flags() & ~Qt::ItemIsEditable); // 第0列禁用
        for (int col = 1; col < 6; ++col) {
            item->setFlags(item->flags() | Qt::ItemIsEditable); // 其他列启用
        }

        // -------------------------- 为目标列创建下拉框（仅索引1/2/3） --------------------------
        createCheckableComboBox(item, 1, rowGdbOptions, gdbLayerText);
        createCheckableComboBox(item, 2, rowActualOptions, actualLayerText);
        createCheckableComboBox(item, 3, rowAnnotOptions, actualAnnotLayerText);
    }

    file.close();
    return true;
}

//另存配置文件
void Drawing_Layout::on_pushButton_savefile_clicked()
{
    QString filePath = QFileDialog::getSaveFileName(this
        , tr("另存为CSV文件")
        , ""
        , tr("CSV Files (*.csv)"));
    if (!filePath.isEmpty()) {
        ui->lineEdit_savefile->setText(filePath);
        if (!saveCsvFile(filePath)) {
            QMessageBox::critical(this, tr("错误"), tr("无法保存CSV文件！"));
        }
        else {
            QMessageBox::information(this, tr("成功"), tr("CSV文件保存成功！"));
        }
    }
}

//保存新的配置文件
bool Drawing_Layout::saveCsvFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(this, tr("警告"), tr("无法打开文件进行写入：%1").arg(file.errorString()));
        return false;
    }

    QTextStream out(&file);
    file.write("\xEF\xBB\xBF");
    out.setCodec("UTF-8"); // 设置为 UTF-8 编码

    // 通过QStringList构造表头，避免直接写死字符串可能的编码问题
    QStringList headers;
    headers << "标准图层"
        << "gdb图层"
        << "实际图层"
        << "实际注记图层"
        << "对应注记层"
        << "图例字段";
    out << headers.join(",") << "\n";

    for (int i = 0; i < ui->treeWidget_csv->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = ui->treeWidget_csv->topLevelItem(i);
        QString line = QString("%1,%2,%3,%4,%5,%6")
            .arg(item->text(0))
            .arg(comboCellStoredText(item, 1))
            .arg(comboCellStoredText(item, 2))
            .arg(comboCellStoredText(item, 3))
            .arg(item->text(4))
            .arg(item->text(5));
        out << line << "\n";
    }

    file.close();
    return true;
}

void Drawing_Layout::on_pushButton_find_clicked()
{
    QString findText = ui->lineEdit_find->text().trimmed();
    if (findText.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("请输入要查找的标准图层名称！"));
        return;
    }

    // 1. 清除上一次查找的高亮（仅恢复背景透明，无需处理文字色）
    for (int i = 0; i < ui->treeWidget_csv->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = ui->treeWidget_csv->topLevelItem(i);
        // 仅恢复所有列的背景为透明（默认状态），保留文字色默认设置
        for (int col = 0; col < ui->treeWidget_csv->columnCount(); ++col) {
            item->setBackground(col, QBrush(Qt::transparent));
        }
    }

    // 2. 查找第一列（标准图层）匹配项（支持模糊匹配，忽略大小写）
    bool found = false;
    for (int i = 0; i < ui->treeWidget_csv->topLevelItemCount(); ++i)
    {
        QTreeWidgetItem* item = ui->treeWidget_csv->topLevelItem(i);
        // 模糊匹配（包含关键词即命中），若需精确匹配，改为 item->text(0) == findText
        if (item->text(0).contains(findText, Qt::CaseInsensitive)) 
        {
            //ui->treeWidget_csv->setCurrentItem(item); // 选中项
            ui->treeWidget_csv->scrollToItem(item);   // 滚动到可见区域
            // 设置整行背景高亮（黄色）
            for (int col = 0; col < ui->treeWidget_csv->columnCount(); ++col) {
                item->setBackground(col, QBrush(Qt::yellow));
            }
            found = true;
        }
    }

    // 3. 提示查找结果
    if (found) {
        m_lastFindText = findText; // 更新上一次查找关键词
    }
    else {
        QMessageBox::information(this, tr("提示"), tr("未找到包含“%1”的标准图层！").arg(findText));
        m_lastFindText = ""; // 未找到，重置关键词
    }
}

//支持enter键查找
void Drawing_Layout::on_lineEdit_find_returnPressed()
{
    // 直接调用查找按钮的槽函数，避免重复代码
    on_pushButton_find_clicked();
}

void Drawing_Layout::on_pushButton_ok_clicked()
{
    QString loadFilePath = ui->lineEdit_loadfile->text().trimmed();
    QString saveFilePath = ui->lineEdit_savefile->text().trimmed();

    // 验证加载文件路径有效性
    if (loadFilePath.isEmpty()) {
        QMessageBox::warning(this, tr("警告"), tr("加载文件路径不能为空！"));
        return;
    }
    QFile loadFile(loadFilePath);
    if (!loadFile.exists()) {
        QMessageBox::warning(this, tr("警告"), tr("加载文件不存在！"));
        return;
    }

    // 验证保存文件路径有效性 - 增加自动补全.csv后缀功能
    if (saveFilePath.isEmpty()) {
        // 如果未输入保存路径，弹出文件选择对话框
        saveFilePath = QFileDialog::getSaveFileName(
            this,
            tr("保存图面配置"),
            QString(),
            tr("CSV文件 (*.csv)")
        );
        if (saveFilePath.isEmpty()) {
            return; // 用户取消选择
        }
    }
    // 确保文件后缀为.csv
    if (!saveFilePath.endsWith(".csv", Qt::CaseInsensitive)) {
        saveFilePath += ".csv";
    }
    // 更新保存路径到输入框（关键：让主窗口能读取到正确路径）
    ui->lineEdit_savefile->setText(saveFilePath);

    // 保存配置（调用已有的保存CSV文件函数）
    if (saveCsvFile(saveFilePath)) {
        QMessageBox::information(this, tr("成功"), tr("配置保存成功！"));
        accept(); // 用accept()关闭，而不是close()，确保主窗口能判断对话框状态
    }
    else {
        QMessageBox::critical(this, tr("错误"), tr("配置保存失败！"));
    }
}

void Drawing_Layout::on_pushButton_close_clicked()
{
    reject();
}

void Drawing_Layout::on_pushButton_check_clicked()
{
    QStringList shpOptions = getFileNamesWithoutSuffix(m_shpPath);

    auto gatherSelectedLower = [this](int col) -> QSet<QString> {
        QSet<QString> selected;
        for (int i = 0; i < ui->treeWidget_csv->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = ui->treeWidget_csv->topLevelItem(i);
            if (!item) continue;
            QWidget* w = ui->treeWidget_csv->itemWidget(item, col);
            QComboBox* combo = qobject_cast<QComboBox*>(w);
            if (!combo) continue;
            QStandardItemModel* model = qobject_cast<QStandardItemModel*>(
                combo->property("checkableSourceModel").value<QObject*>());
            if (!model) continue;
            for (int r = 0; r < model->rowCount(); ++r) {
                QStandardItem* modelItem = model->item(r);
                if (!modelItem) continue;
                if (modelItem->data(Qt::CheckStateRole) == Qt::Checked) {
                    selected.insert(modelItem->text().trimmed().toLower());
                }
            }
        }
        return selected;
    };

    QSet<QString> shpSelectedLower = gatherSelectedLower(2);
    shpSelectedLower.unite(gatherSelectedLower(3));

    QStringList shpUnselected;
    for (const QString& opt : shpOptions) {
        if (!shpSelectedLower.contains(opt.trimmed().toLower())) {
            shpUnselected << opt;
        }
    }

    QString displayHeader = ui->treeWidget_csv->headerItem()->text(2);
    const QString logTag = QStringLiteral("DrawingLayout");

    if (!shpUnselected.isEmpty()) {
        QString msg = tr("%1 未配置项（%2）：%3")
                          .arg(displayHeader)
                          .arg(shpUnselected.size())
                          .arg(shpUnselected.join("|"));
        QgsMessageLog::logMessage(msg, logTag, Qgis::Info);
        QMessageBox::information(this,
                                 tr("提示"),
                                 tr("存在未配置的图层，请查看日志消息面板的“%1”标签。").arg(logTag));
    } else {
        QString msg = tr("%1 未配置项：无").arg(displayHeader);
        QgsMessageLog::logMessage(msg, logTag, Qgis::Info);
    }
}

void Drawing_Layout::createCheckableComboBox(QTreeWidgetItem* item, int targetCol, const QStringList& options, const QString& initialText)
{
    // 1. 创建可检索的多选下拉框。输入内容仅用于过滤候选图层，不会直接写入配置。
    SearchableCheckComboBox* combo = new SearchableCheckComboBox(ui->treeWidget_csv);
    combo->setEditable(true);
    combo->lineEdit()->setReadOnly(true);

    // 2. 创建带复选框的模型
    QStandardItemModel* model = new QStandardItemModel(combo);
    for (const QString& option : options) {
        QStandardItem* modelItem = new QStandardItem(option);
        modelItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable); // 启用交互
        modelItem->setData(Qt::Unchecked, Qt::CheckStateRole); // 默认未勾选
        model->appendRow(modelItem);
    }
    combo->setCheckableSourceModel(model);
    combo->setProperty("checkableSourceModel", QVariant::fromValue<QObject*>(model));
    if (targetCol == 2 || targetCol == 3) {
        combo->setBeforeShowCallback([this, item, targetCol]() {
            sortLayerOptions(item, targetCol);
        });
    }

    // 3. 初始化选中状态（按CSV原始值勾选）
    QStringList initialSelections = initialText.split("|", Qt::SkipEmptyParts);
    for (int i = 0; i < model->rowCount(); ++i) {
        QStandardItem* modelItem = model->item(i);
        if (initialSelections.contains(modelItem->text(), Qt::CaseInsensitive)) {
            modelItem->setData(Qt::Checked, Qt::CheckStateRole);
        }
    }

    auto updateDisplay = [this, combo, item, targetCol, model]() {
        QStringList selectedOptions;
        for (int i = 0; i < model->rowCount(); ++i) {
            QStandardItem* modelItem = model->item(i);
            if (modelItem && modelItem->data(Qt::CheckStateRole) == Qt::Checked) {
                selectedOptions << modelItem->text();
            }
        }
        QString result = selectedOptions.join("|");
        setComboCellStoredText(item, targetCol, result);
        if (!combo->isPopupOpen()) {
            // 复选模型中的候选项不代表 QComboBox 的单选当前项；保持 -1，避免空配置被默认第一个候选项填充。
            combo->setCurrentIndex(-1);
            combo->setEditText(result);
        }
        if (result.isEmpty()) {
            combo->setCurrentIndex(-1);
            if (!combo->isPopupOpen()) {
                combo->lineEdit()->clear();
            }
        }
    };
    connect(model, &QStandardItemModel::itemChanged, this,
            [updateDisplay](QStandardItem*) { updateDisplay(); });
    connect(model, &QStandardItemModel::dataChanged, this,
            [updateDisplay](const QModelIndex&, const QModelIndex&, const QVector<int>&) { updateDisplay(); });
    combo->setSelectionTextUpdater(updateDisplay);

    // 5. 初始显示CSV原始值
    combo->setCurrentIndex(-1);
    combo->setEditText(initialText);
    setComboCellStoredText(item, targetCol, initialText);

    // 6. 禁用单元格下拉框本体的滚轮切换；弹出列表仍可滚动浏览。
    combo->installEventFilter(this);

    ui->treeWidget_csv->setItemWidget(item, targetCol, combo);
}

void Drawing_Layout::setComboCellStoredText(QTreeWidgetItem* item, int column, const QString& value)
{
    if (!item) {
        return;
    }

    const QString text = value.trimmed();
    item->setData(column, kComboCellValueRole, text);
    item->setToolTip(column, text);
    // 嵌入的下拉框会自行绘制值，必须清空 DisplayRole，避免表格底层文字与控件文字重叠。
    item->setText(column, QString());
}

QString Drawing_Layout::comboCellStoredText(QTreeWidgetItem* item, int column) const
{
    if (!item) {
        return QString();
    }

    if (ui && ui->treeWidget_csv) {
        if (QComboBox* combo = qobject_cast<QComboBox*>(ui->treeWidget_csv->itemWidget(item, column))) {
            const QString comboText = combo->currentText().trimmed();
            if (!comboText.isEmpty()) {
                return comboText;
            }
        }
    }

    const QString storedText = item->data(column, kComboCellValueRole).toString().trimmed();
    if (!storedText.isEmpty()) {
        return storedText;
    }
    return item->text(column).trimmed();
}

int Drawing_Layout::layerNameMatchScore(const QString& standardLayerName, const QString& actualLayerName) const
{
    const QString standard = normalizedLayerName(standardLayerName);
    const QString actual = normalizedLayerName(actualLayerName);
    if (standard.isEmpty() || actual.isEmpty()) {
        return 0;
    }
    if (standard == actual) {
        return 100000;
    }
    if (actual.contains(standard)) {
        return 90000 - (actual.size() - standard.size());
    }
    if (standard.contains(actual)) {
        return 80000 - (standard.size() - actual.size());
    }

    const int commonLength = longestCommonSubstringLength(standard, actual);
    int commonCharacterCount = 0;
    QSet<QChar> actualCharacters;
    for (const QChar& character : actual) {
        actualCharacters.insert(character);
    }
    for (const QChar& character : standard) {
        if (actualCharacters.contains(character)) {
            ++commonCharacterCount;
        }
    }
    return commonLength * 1000 + commonCharacterCount * 10;
}

void Drawing_Layout::sortActualLayerOptions(QTreeWidgetItem* item)
{
    // “实际图层”和“实际注记图层”均支持一对多，使用同一套候选排序规则。
    sortLayerOptions(item, 2);
    sortLayerOptions(item, 3);
}

void Drawing_Layout::sortLayerOptions(QTreeWidgetItem* item, int targetCol)
{
    if (!item) {
        return;
    }

    QComboBox* combo = qobject_cast<QComboBox*>(ui->treeWidget_csv->itemWidget(item, targetCol));
    if (!combo || combo->property("checkablePopupOpen").toBool()) {
        return;
    }
    QStandardItemModel* model = qobject_cast<QStandardItemModel*>(
        combo->property("checkableSourceModel").value<QObject*>());
    if (!model || model->rowCount() < 2) {
        return;
    }

    struct LayerOption {
        QStandardItem* modelItem;
        bool isChecked;
        int matchScore;
    };

    QVector<LayerOption> layerOptions;
    layerOptions.reserve(model->rowCount());
    const QString standardLayerName = item->text(0);
    for (int row = 0; row < model->rowCount(); ++row) {
        QStandardItem* modelItem = model->item(row);
        if (!modelItem) {
            continue;
        }
        layerOptions.append({
            modelItem,
            modelItem->data(Qt::CheckStateRole) == Qt::Checked,
            layerNameMatchScore(standardLayerName, modelItem->text())
        });
    }

    std::stable_sort(layerOptions.begin(), layerOptions.end(), [](const LayerOption& left, const LayerOption& right) {
        if (left.isChecked != right.isChecked) {
            return left.isChecked;
        }
        if (left.matchScore != right.matchScore) {
            return left.matchScore > right.matchScore;
        }
        return QString::localeAwareCompare(left.modelItem->text(), right.modelItem->text()) < 0;
    });

    // takeRow/appendRow 保留原有复选状态及所有 item 数据，只改变候选显示顺序。
    for (int row = model->rowCount() - 1; row >= 0; --row) {
        model->takeRow(row);
    }
    for (const LayerOption& option : layerOptions) {
        model->appendRow(option.modelItem);
    }

    // takeRow/appendRow 会让 QComboBox 自动切换到模型的第一个选项；恢复“仅由复选状态决定”的显示。
    QStringList selectedOptions;
    for (const LayerOption& option : layerOptions) {
        if (option.isChecked) selectedOptions << option.modelItem->text();
    }
    const QString selectedText = selectedOptions.join("|");
    combo->setCurrentIndex(-1);
    combo->setEditText(selectedText);
    setComboCellStoredText(item, targetCol, selectedText);
}

QStringList Drawing_Layout::getFileNamesWithoutSuffix(const QString& folderPath)
{
    QStringList names;
    QDir dir(folderPath);
    if (!dir.exists()) {
        QMessageBox::warning(this, tr("警告"), tr("文件夹不存在：%1").arg(folderPath));
        return names;
    }

    // 仅筛选.shp文件
    QStringList filters;
    filters << "*.shp"; // 只保留shp文件
    dir.setNameFilters(filters);

    // 遍历文件（排除目录和隐藏文件）
    QFileInfoList fileInfos = dir.entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name
    );

    //合并相同类的不同（点线面）要素
    for (const QFileInfo& info : fileInfos) {
        if (info.isHidden()) continue;
        QString baseName = info.baseName(); // 原始文件名（如 "图例_LineString"）

        // 正则匹配：裁剪掉 _LineString、_Point、_Polygon 后缀
        QRegularExpression regex("(.+)_(LineString|Point|Polygon)$");
        QRegularExpressionMatch match = regex.match(baseName);
        if (match.hasMatch()) {
            baseName = match.captured(1); // 提取前缀部分（如 "图例"）
        }

        // 去重也忽略大小写，避免同一逻辑图层出现两个候选项。
        if (!names.contains(baseName, Qt::CaseInsensitive)) {
            names << baseName;
        }
    }
    return names;
}

// 屏蔽鼠标滚轮事件：当鼠标停在下拉框或其弹出的列表上时，不因滚轮而改变选择
bool Drawing_Layout::eventFilter(QObject* obj, QEvent* event)
{
    if (event && event->type() == QEvent::Wheel) {
        // 仅针对 QComboBox 本体和其视图屏蔽滚轮
        if (qobject_cast<QComboBox*>(obj) || qobject_cast<QListView*>(obj)) {
            return true; // 吃掉事件
        }
    }
    return QDialog::eventFilter(obj, event);
}

// 1. 处理“gdb图层”（列1，文件夹路径 m_gdbShpPath）
/*QStringList Drawing_Layout::getGdbShpPaths(QTreeWidgetItem* item) {
    QStringList shpPaths;
    if (!item) return shpPaths;

    // 提取列1的图层名（如 "图层1|图层2"）
    QString layerText = item->text(1).trimmed();
    // 按 | 分割为单个图层名
    QStringList layerNames = layerText.split("|", Qt::SkipEmptyParts);

    // 生成每个图层的完整 .shp 路径
    for (const QString& layerName : layerNames) {
        QString shpPath = QDir(m_gdbShpPath).filePath(layerName + ".shp");
        // 验证文件是否存在（避免路径错误/文件缺失）
        if (QFile::exists(shpPath)) {
            shpPaths << shpPath;
        }
        else {
            QMessageBox::warning(this, tr("警告"),
                tr("gdb图层 .shp 文件不存在：%1").arg(shpPath));
        }
    }
    return shpPaths;
}
*/
// 2. 处理“实际图层”（列3，文件夹路径 m_shpPath）
/*QStringList Drawing_Layout::getActualShpPaths(QTreeWidgetItem* item) {
    QStringList shpPaths;
    if (!item) return shpPaths;

    QString layerText = item->text(2).trimmed();
    QStringList layerNames = layerText.split("|", Qt::SkipEmptyParts);

    for (const QString& layerName : layerNames) {
        QString shpPath = QDir(m_shpPath).filePath(layerName + ".shp");
        if (QFile::exists(shpPath)) {
            shpPaths << shpPath;
        }
        else {
            QMessageBox::warning(this, tr("警告"),
                tr("实际图层 .shp 文件不存在：%1").arg(shpPath));
        }
    }
    return shpPaths;
}*/

// 3. 处理“实际注记图层”（列4，文件夹路径 m_shpPath）
/*QStringList Drawing_Layout::getActualAnnotShpPaths(QTreeWidgetItem* item) {
    QStringList shpPaths;
    if (!item) return shpPaths;

    QString layerText = item->text(3).trimmed();
    QStringList layerNames = layerText.split("|", Qt::SkipEmptyParts);

    for (const QString& layerName : layerNames) {
        QString shpPath = QDir(m_shpPath).filePath(layerName + ".shp");
        if (QFile::exists(shpPath)) {
            shpPaths << shpPath;
        }
        else {
            QMessageBox::warning(this, tr("警告"),
                tr("实际注记图层 .shp 文件不存在：%1").arg(shpPath));
        }
    }
    return shpPaths;
}*/
