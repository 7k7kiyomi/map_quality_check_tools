#pragma once
#include <QComboBox>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QLineEdit>
#include <QListView>
#include <QFrame>
#include <QVBoxLayout>
#include <QSizeGrip>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QPalette>
#include <QBrush>
#include <QCollator>
#include <QLocale>
#include <QFileInfo>
#include <functional>

namespace LayerSelection {
// Chinese names use pinyin collation independently of the host UI language.
// Sort only the popup proxy so existing selections keep their actual source paths.
class LayerNameSortFilterProxyModel final : public QSortFilterProxyModel
{
public:
    explicit LayerNameSortFilterProxyModel(QObject* parent = nullptr)
        : QSortFilterProxyModel(parent), m_collator(QLocale(QLocale::Chinese, QLocale::China))
    {
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
        m_collator.setNumericMode(true);
        setDynamicSortFilter(true);
    }

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override
    {
        const QString leftPath = left.data(Qt::UserRole).toString();
        const QString rightPath = right.data(Qt::UserRole).toString();
        // Keep "未选择" / "不对应" ahead of real layers.
        if (leftPath.isEmpty() != rightPath.isEmpty()) return leftPath.isEmpty();
        const QString leftName = left.data(Qt::DisplayRole).toString();
        const QString rightName = right.data(Qt::DisplayRole).toString();
        int comparison = m_collator.compare(QFileInfo(leftName).completeBaseName(), QFileInfo(rightName).completeBaseName());
        if (comparison != 0) return comparison < 0;
        comparison = m_collator.compare(leftName, rightName);
        if (comparison != 0) return comparison < 0;
        comparison = m_collator.compare(leftPath, rightPath);
        if (comparison != 0) return comparison < 0;
        comparison = QString::compare(leftPath, rightPath, Qt::CaseSensitive);
        return comparison != 0 ? comparison < 0 : left.row() < right.row();
    }

private:
    QCollator m_collator;
};

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

// 图层选择共用搜索面板：顶部固定检索框，下方按包含文字过滤；支持单选与多选。
class SearchableLayerComboBox final : public QComboBox
{
public:
    explicit SearchableLayerComboBox(QWidget* parent = nullptr)
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

    // Filter only the popup: the combo retains its source row and actual path.
    void enableSingleSelectionSearch()
    {
        m_singleSelection = true;
        m_proxyModel = new LayerNameSortFilterProxyModel(this);
        m_proxyModel->setSourceModel(model());
        m_proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
        m_proxyModel->setFilterKeyColumn(modelColumn());
        m_proxyModel->sort(modelColumn(), Qt::AscendingOrder);
    }

    ~SearchableLayerComboBox() override
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
            if (m_singleSelection && m_popupListView)
                m_popupListView->setCurrentIndex(m_proxyModel->index(0, 0));
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
        if (m_singleSelection) {
            const QModelIndex selected = m_proxyModel->mapFromSource(model()->index(currentIndex(), modelColumn()));
            m_popupListView->setCurrentIndex(selected);
            m_popupListView->scrollTo(selected);
        }

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
        if (watched == m_searchEdit && event->type() == QEvent::InputMethod) {
            m_preeditActive = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
        }
        if (watched == lineEdit() && event->type() == QEvent::MouseButtonPress) {
            const QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                showPopup();
                return true;
            }
        }
        if (m_singleSelection && (watched == m_searchEdit || watched == m_popupListView)
            && event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            // Enter first commits the Chinese IME composition; it must not select
            // a candidate until the committed text has reached the search field.
            if (m_preeditActive && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter))
                return false;
            if (key->key() == Qt::Key_Escape) { hidePopup(); return true; }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                chooseSingleItem(m_popupListView->currentIndex());
                return true;
            }
            if (watched == m_searchEdit && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
                const int count = m_proxyModel->rowCount();
                if (count > 0) {
                    const int current = m_popupListView->currentIndex().row();
                    const int row = qBound(0, current + (key->key() == Qt::Key_Down ? 1 : -1), count - 1);
                    m_popupListView->setCurrentIndex(m_proxyModel->index(row, 0));
                    m_popupListView->scrollTo(m_popupListView->currentIndex());
                }
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

    void wheelEvent(QWheelEvent* event) override
    {
        if (m_singleSelection) { event->ignore(); return; }
        QComboBox::wheelEvent(event);
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
    void chooseSingleItem(const QModelIndex& index)
    {
        if (!index.isValid()) return;
        const int sourceRow = m_proxyModel->mapToSource(index).row();
        hidePopup();
        setCurrentIndex(sourceRow);
    }

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
        m_searchEdit->setObjectName(QStringLiteral("layerSearchEdit"));
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
        m_popupListView->setObjectName(QStringLiteral("layerSearchResults"));
        m_popupListView->setModel(m_proxyModel);
        if (m_singleSelection) {
            m_popupListView->setEditTriggers(QAbstractItemView::NoEditTriggers);
            m_popupListView->setSelectionMode(QAbstractItemView::SingleSelection);
            connect(m_popupListView, &QListView::clicked, this,
                    [this](const QModelIndex& index) { chooseSingleItem(index); });
            m_searchEdit->installEventFilter(this);
            m_popupListView->installEventFilter(this);
        } else {
            m_popupListView->setItemDelegate(new CheckableRowDelegate(m_popupListView));
        }
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
        m_preeditActive = false;
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
    bool m_singleSelection = false;
    bool m_preeditActive = false;
    bool m_popupOpen = false;
    bool m_popupSizeInitialized = false;
    int m_lastCellWidth = 0;
};

} // namespace LayerSelection
