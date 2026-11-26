#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPathItem>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QSet>
#include <functional>
#include <algorithm>

// Geometry is supplied in scene coordinates (east, -north). No project layers
// or providers are created, so the overview cannot affect the main QGIS map.
class SeriesSheetOverview : public QGraphicsView
{
public:
    struct Sheet {
        qint64 id;
        QString name;
        QString tooltip;
        QPainterPath outline;
        QPointF labelPoint;
    };

    explicit SeriesSheetOverview(QWidget* parent = nullptr) : QGraphicsView(parent)
    {
        setObjectName(QStringLiteral("sheetOverview"));
        setScene(new QGraphicsScene(this));
        setMinimumSize(280, 170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setRenderHint(QPainter::Antialiasing);
        setBackgroundBrush(QColor(QStringLiteral("#f7faf9")));
        setFrameShape(QFrame::StyledPanel);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMouseTracking(true);
        setToolTip(QStringLiteral("点击图幅定位配置；滚轮缩放，拖动平移"));
    }

    std::function<void(qint64)> sheetClicked;

    void setSheets(const QVector<Sheet>& sheets)
    {
        m_sheets = sheets;
        scene()->clear();
        for (const auto& sheet : m_sheets) {
            auto* item = scene()->addPath(sheet.outline);
            item->setData(0, sheet.id);
            item->setToolTip(sheet.tooltip);
        }
        // Keep a small border around the map. Labels are drawn in viewport
        // coordinates, so a large scene-space margin is unnecessary here.
        const QRectF bounds = scene()->itemsBoundingRect();
        if (bounds.isEmpty()) {
            scene()->setSceneRect(QRectF());
        } else {
            const qreal padX = qMax(bounds.width() * .025, qreal(1e-9));
            const qreal padY = qMax(bounds.height() * .025, qreal(1e-9));
            scene()->setSceneRect(bounds.adjusted(-padX, -padY, padX, padY));
        }
        updateStyles();
        showAll();
    }

    void setRelatedSheets(const QVector<Sheet>& sheets, const QSet<qint64>& relatedIds)
    {
        QVector<Sheet> relatedSheets;
        relatedSheets.reserve(sheets.size());
        for (const Sheet& sheet : sheets)
            if (relatedIds.contains(sheet.id)) relatedSheets.push_back(sheet);
        setSheets(relatedSheets);
    }

    void setFocusSheets(qint64 primary, const QSet<qint64>& related)
    {
        m_primary = primary;
        m_related = related;
        updateStyles();
        viewport()->update();
    }

    void showAll()
    {
        m_fitted = true;
        if (!m_sheets.isEmpty() && !scene()->sceneRect().isEmpty()) {
            // fitInView adjusts the existing transform; reset first so Show
            // All behaves consistently after arbitrary zooming or panning.
            resetTransform();
            fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
            centerOn(scene()->sceneRect().center());
        } else {
            resetTransform();
        }
        m_fitScale = transform().m11();
        viewport()->update();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QGraphicsView::resizeEvent(event);
        if (m_fitted) showAll();
    }

    void wheelEvent(QWheelEvent* event) override
    {
        if (m_sheets.isEmpty() || event->angleDelta().y() == 0) { event->accept(); return; }
        const qreal factor = event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2;
        const qreal ratio = transform().m11() * factor / qMax(m_fitScale, 1e-12);
        if (ratio >= .5 && ratio <= 20) { scale(factor,factor); m_fitted = false; }
        viewport()->update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        m_pressed = event->pos();
        QGraphicsView::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton) && (event->pos()-m_pressed).manhattanLength()>5)
            m_fitted = false;
        QGraphicsView::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        QGraphicsView::mouseReleaseEvent(event);
        if (event->button()!=Qt::LeftButton || (event->pos()-m_pressed).manhattanLength()>5) return;
        const QPointF point = mapToScene(event->pos());
        const Sheet* selected = nullptr;
        for (const auto& sheet : m_sheets) {
            if (!sheet.outline.contains(point)) continue;
            const auto area = [](const Sheet& s) {
                const QRectF rect = s.outline.boundingRect(); return rect.width()*rect.height();
            };
            if (!selected || area(sheet) < area(*selected)) selected = &sheet;
        }
        if (selected && sheetClicked) sheetClicked(selected->id);
    }

    void drawForeground(QPainter* painter, const QRectF&) override
    {
        painter->save();
        // Draw all boundaries above every fill, including containing sheets.
        painter->setBrush(Qt::NoBrush);
        for (auto* base : scene()->items(Qt::AscendingOrder)) {
            auto* item=static_cast<QGraphicsPathItem*>(base);
            painter->setPen(item->pen());
            painter->drawPath(item->path());
        }
        painter->resetTransform();
        if (m_sheets.isEmpty()) {
            painter->setPen(QColor(QStringLiteral("#7a8986")));
            painter->drawText(viewport()->rect().adjusted(14,10,-14,-10), Qt::AlignCenter | Qt::TextWordWrap,
                QStringLiteral("暂无可显示的图幅\n请加载图幅索引或配置地图"));
            painter->restore();
            return;
        }
        QVector<const Sheet*> ordered;
        for (const auto& s : m_sheets) ordered.push_back(&s);
        std::stable_sort(ordered.begin(), ordered.end(), [this](const Sheet* a, const Sheet* b) {
            auto priority = [this](qint64 id) { return id==m_primary ? 2 : m_related.contains(id) ? 1 : 0; };
            return priority(a->id) > priority(b->id);
        });
        QVector<QRect> occupied;
        for (const Sheet* sheet : ordered) {
            const QRect bounds = mapFromScene(sheet->outline.boundingRect()).boundingRect();
            const bool focused = sheet->id==m_primary || m_related.contains(sheet->id);
            if (!bounds.intersects(viewport()->rect()) || (!focused && (bounds.width()<42 || bounds.height()<22))) continue;
            QFont labelFont = font(); labelFont.setPointSize(9); labelFont.setBold(sheet->id==m_primary);
            painter->setFont(labelFont);
            const QFontMetrics metrics(labelFont);
            const QString label = metrics.elidedText(sheet->name,Qt::ElideMiddle,
                qMax(45,qMin(bounds.width()-8,160)));
            const QSize size(metrics.horizontalAdvance(label)+10,metrics.height()+4);
            // pointOnSurface anchors labels inside concave/multipart outlines.
            QPoint center = mapFromScene(sheet->labelPoint);
            QRect rect(center-QPoint(size.width()/2,size.height()/2),size);
            bool overlaps = false;
            for (const auto& used : occupied) if (used.intersects(rect.adjusted(-3,-2,3,2))) { overlaps = true; break; }
            if (overlaps && focused) {
                for (int offset : {1,-1,2,-2}) {
                    const QRect alternative=rect.translated(0,offset*(size.height()+4));
                    bool free=viewport()->rect().contains(alternative);
                    for (const auto& used : occupied) if (used.intersects(alternative.adjusted(-3,-2,3,2))) free=false;
                    if (free) { rect=alternative; overlaps=false; break; }
                }
            }
            if (overlaps) continue;
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(255,255,255,225));
            painter->drawRoundedRect(rect,3,3);
            painter->setPen(QColor(sheet->id==m_primary ? QStringLiteral("#155e91") : QStringLiteral("#314b48")));
            painter->drawText(rect,Qt::AlignCenter,label);
            occupied.push_back(rect);
        }
        painter->restore();
    }

private:
    void updateStyles()
    {
        for (auto* base : scene()->items()) {
            auto* item = static_cast<QGraphicsPathItem*>(base);
            const qint64 id = item->data(0).toLongLong();
            const bool primary = id==m_primary, related = m_related.contains(id);
            QPen pen(QColor(primary ? QStringLiteral("#237bb6") : related ? QStringLiteral("#47988d") : QStringLiteral("#9eafab")));
            pen.setCosmetic(true); pen.setWidthF(primary ? 2.5 : related ? 2 : 1);
            item->setPen(pen);
            QColor fill(primary ? QStringLiteral("#7fbfe8") : related ? QStringLiteral("#91cbbb") : QStringLiteral("#d8e4df"));
            fill.setAlpha(primary ? 85 : 65);
            item->setBrush(fill);
            item->setZValue(primary ? 2 : related ? 1 : 0);
        }
    }
    QVector<Sheet> m_sheets;
    qint64 m_primary = -1;
    QSet<qint64> m_related;
    QPoint m_pressed;
    bool m_fitted = true;
    qreal m_fitScale = 1;
};
