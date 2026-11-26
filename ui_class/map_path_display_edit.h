#pragma once
#include <QLineEdit>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QStylePainter>
#include <QStyleOptionFrame>

// Keep QLineEdit::text() as the original paths for all existing readers. Only
// painting changes; choosing/replacing maps still uses the adjacent buttons.
class MapPathDisplayEdit final : public QLineEdit
{
public:
    explicit MapPathDisplayEdit(QWidget* parent = nullptr) : QLineEdit(parent)
    {
        setReadOnly(true);
        connect(this, &QLineEdit::textChanged, this, [this](const QString& paths) {
            setToolTip(paths);
            update();
        });
    }

    QString displayedMapNames() const
    {
        QStringList names;
        for (const QString& path : text().split(QRegularExpression(QStringLiteral("[;；、\\r\\n\\t]+")), Qt::SkipEmptyParts))
            names << QFileInfo(QDir::fromNativeSeparators(path.trimmed())).fileName();
        return names.join(QStringLiteral("；"));
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        if (text().isEmpty()) { QLineEdit::paintEvent(event); return; }
        QStylePainter painter(this);
        QStyleOptionFrame option;
        initStyleOption(&option);
        painter.drawPrimitive(QStyle::PE_PanelLineEdit, option);
        QRect content = style()->subElementRect(QStyle::SE_LineEditContents, &option, this);
        const QMargins margins = textMargins();
        content.adjust(margins.left() + 2, margins.top(), -margins.right() - 2, -margins.bottom());
        painter.setClipRect(content);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::Text));
        painter.drawText(content, Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(displayedMapNames(), Qt::ElideRight, content.width()));
    }
};
