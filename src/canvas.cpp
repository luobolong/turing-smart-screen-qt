#include "canvas.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>
namespace Screen {
Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 220);
    setAutoFillBackground(false);
}
void Canvas::setFrame(QImage f) {
    frame = std::move(f);
    update();
}
void Canvas::setFit(bool v) {
    fit = v;
    setMinimumSize(v ? QSize(240, 220)
                     : QSize(int(frame.width() * scale + 48), int(frame.height() * scale + 48)));
    update();
}
void Canvas::setZoom(double v) {
    scale = qBound(.25, v, 4.);
    fit = false;
    setMinimumSize(int(frame.width() * scale + 48), int(frame.height() * scale + 48));
    updateGeometry();
    update();
}
QRectF Canvas::screenRect() const {
    QSize size = frame.isNull() ? (theme ? theme->size() : QSize(480, 320)) : frame.size();
    double z = fit ? qMax(.1, qMin((width() - 48.) / size.width(), (height() - 48.) / size.height())) : scale;
    return QRectF(QPointF((width() - size.width() * z) / 2., (height() - size.height() * z) / 2.),
                  QSizeF(size) * z);
}
QPointF Canvas::logical(QPointF p) const {
    auto r = screenRect();
    auto out = (p - r.topLeft()) / (r.width() / qMax(1, frame.width()));
    if (mirrored)
        out.setX(frame.width() - out.x());
    return out;
}
QRectF Canvas::bounds(const QString &id) const {
    if (!theme)
        return {};
    auto l = findLayer(theme->layers(), id);
    auto r = layerBounds(l).translated(absolutePosition(theme->layers(), id));
    if (mirrored)
        r.moveLeft(theme->size().width() - r.right());
    auto screen = screenRect();
    double z = screen.width() / theme->size().width();
    return QRectF(screen.topLeft() + r.topLeft() * z, r.size() * z);
}
void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#0c1117"));
    auto r = screenRect();
    p.fillRect(r, Qt::black);
    if (!frame.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(r, mirrored ? frame.mirrored(true, false) : frame);
    }
    if (!editing || !theme)
        return;
    double z = r.width() / theme->size().width();
    if (grid) {
        p.setPen(QColor(180, 195, 210, 38));
        for (int x = 0; x <= theme->size().width(); x += 8)
            p.drawLine(QPointF(r.left() + x * z, r.top()), QPointF(r.left() + x * z, r.bottom()));
        for (int y = 0; y <= theme->size().height(); y += 8)
            p.drawLine(QPointF(r.left(), r.top() + y * z), QPointF(r.right(), r.top() + y * z));
    }
    for (auto id : selected) {
        auto b = bounds(id);
        p.setPen(QPen(QColor("#388dff"), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawRect(b);
        if (!layerLocked(theme->layers(), id) && findLayer(theme->layers(), id)["kind"] != "group") {
            p.setBrush(Qt::white);
            for (auto pt : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
                p.drawRect(QRectF(pt - QPointF(3, 3), QSizeF(6, 6)));
        }
    }
    if (marquee) {
        p.setPen(QPen(QColor("#388dff"), 1, Qt::DashLine));
        p.setBrush(QColor(56, 141, 255, 25));
        p.drawRect(QRectF(start, last).normalized());
    }
}
QString Canvas::hit(const QJsonArray &a, QPointF point, bool deep, QPointF offset) const {
    for (int n = int(a.size()) - 1; n >= 0; --n) {
        auto l = a[n].toObject();
        if (!l["visible"].toBool(true))
            continue;
        auto at = offset + QPointF(l["x"].toDouble(), l["y"].toDouble());
        auto child = hit(l["children"].toArray(), point, deep, at);
        if (!child.isEmpty())
            return deep ? child : l["id"].toString();
        if (layerBounds(l).translated(at).contains(point))
            return l["id"].toString();
    }
    return {};
}
QString Canvas::dragTarget(QPointF point, bool deep) const {
    const auto layers = theme->layers();
    const auto leaf = hit(layers, point, true);
    if (deep)
        return leaf;
    // Keep an explicitly selected group (or a child selected in the layer tree)
    // when pressing one of its descendants. Respect the topmost object at the pointer.
    auto id = leaf;
    while (!id.isEmpty()) {
        if (selected.contains(id))
            return id;
        id = parentOf(layers, id);
    }
    return hit(layers, point);
}
bool Canvas::canResize() const {
    return theme && selected.size() == 1 && !layerLocked(theme->layers(), selected.first()) &&
           findLayer(theme->layers(), selected.first())["kind"] != "group";
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    setFocus();
    if (e->button() == Qt::RightButton) {
        emit contextRequested(e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton || !editing || !theme)
        return;
    start = last = e->position();
    before = theme->data;
    dragging = false;
    resizing = false;
    marquee = false;
    if (tool == "text") {
        emit createRequested("text", logical(start), QSizeF(120, 30));
        return;
    }
    if (tool == "rectangle" || tool == "ellipse" || tool == "line") {
        marquee = true;
        update();
        return;
    }
    if (!(e->modifiers() & Qt::AltModifier) && canResize() &&
        QRectF(bounds(selected.first()).bottomRight() - QPointF(7, 7), QSizeF(14, 14)).contains(start)) {
        resizing = true;
        selectionBox = layerBounds(findLayer(theme->layers(), selected.first()));
        return;
    }
    auto id = dragTarget(logical(start), e->modifiers() & Qt::AltModifier);
    if (!id.isEmpty()) {
        if (e->modifiers() & Qt::ControlModifier) {
            if (selected.contains(id))
                selected.removeAll(id);
            else
                selected.append(id);
        } else if (!selected.contains(id))
            selected = {id};
        dragging = true;
    } else {
        if (!(e->modifiers() & Qt::ControlModifier))
            selected.clear();
        marquee = true;
    }
    emit selectionChanged();
    update();
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (!theme || !editing)
        return;
    last = e->position();
    if (!(e->buttons() & Qt::LeftButton)) {
        if (canResize() &&
            QRectF(bounds(selected.first()).bottomRight() - QPointF(6, 6), QSizeF(12, 12)).contains(last))
            setCursor(Qt::SizeFDiagCursor);
        else
            setCursor(Qt::ArrowCursor);
        return;
    }
    if (resizing && selected.size() == 1) {
        auto a = theme->layers();
        auto delta = logical(last) - logical(start);
        updateLayer(a, selected.first(), [&](QJsonObject &l) {
            if (l["kind"] == "group")
                return;
            l["w"] = qBound(1, int(selectionBox.width() + delta.x()), 1536);
            l["h"] = qBound(1, int(selectionBox.height() + delta.y()), 1536);
            if (l["kind"] == "text" || l["kind"] == "sensor") {
                auto f = l["font"].toObject();
                auto old = findLayer(before["layers"].toArray(), selected.first())["font"].toObject();
                f["size"] = qBound(
                    6, int(old["size"].toDouble(20) * l["h"].toDouble() / qMax(1., selectionBox.height())),
                    128);
                if (f["family"] != "Turing Screen Symbols" && f["family"] != "Turing Screen Latin" &&
                    f["family"] != "Turing Screen Mono")
                    f["family"] = "Noto Sans SC";
                f["height"] = 0;
                f["width"] = 0;
                f["spacing"] = 0;
                l["font"] = f;
            }
        });
        theme->data["layers"] = a;
        emit transientEdited();
    } else if (dragging) {
        auto a = theme->layers();
        auto delta = logical(last) - logical(start);
        for (auto id : normalizedSelection(a, selected)) {
            if (layerLocked(a, id))
                continue;
            auto original = findLayer(before["layers"].toArray(), id);
            updateLayer(a, id, [&](QJsonObject &l) {
                auto round = [&](double n) { return snap ? int(std::round(n / 4) * 4) : int(std::round(n)); };
                l["x"] = round(original["x"].toDouble() + delta.x());
                l["y"] = round(original["y"].toDouble() + delta.y());
            });
        }
        theme->data["layers"] = a;
        emit transientEdited();
    }
    update();
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !theme)
        return;
    last = e->position();
    if (marquee) {
        if (tool == "rectangle" || tool == "ellipse" || tool == "line") {
            auto r = QRectF(logical(start), logical(last)).normalized();
            if (r.width() > 1 && r.height() > 1)
                emit createRequested(tool, r.topLeft(), r.size());
        } else {
            auto r = QRectF(start, last).normalized();
            std::function<void(QJsonArray)> visit = [&](QJsonArray a) {
                for (auto v : a) {
                    auto o = v.toObject();
                    auto id = o["id"].toString();
                    if (!o["visible"].toBool(true))
                        continue;
                    if (r.contains(bounds(id))) {
                        if (!selected.contains(id))
                            selected.append(id);
                    } else if (o["kind"] == "group")
                        visit(o["children"].toArray());
                }
            };
            visit(theme->layers());
            emit selectionChanged();
        }
    }
    if ((dragging || resizing) && before != theme->data)
        emit committed(before);
    dragging = resizing = marquee = false;
    update();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (editing && theme) {
        auto id = hit(theme->layers(), logical(e->position()), true);
        if (!id.isEmpty()) {
            selected = {id};
            emit selectionChanged();
            update();
        }
    }
}
void Canvas::wheelEvent(QWheelEvent *e) {
    if (e->modifiers() & Qt::ControlModifier) {
        double current = screenRect().width() / qMax(1, frame.width());
        setZoom(current * (e->angleDelta().y() > 0 ? 1.1 : .9));
        e->accept();
    } else
        QWidget::wheelEvent(e);
}
void Canvas::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    update();
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (theme && editing && e->key() >= Qt::Key_Left && e->key() <= Qt::Key_Down) {
        auto old = theme->data;
        auto a = theme->layers();
        int step = e->modifiers() & Qt::ShiftModifier ? 10 : 1;
        QPoint delta(e->key() == Qt::Key_Left    ? -step
                     : e->key() == Qt::Key_Right ? step
                                                 : 0,
                     e->key() == Qt::Key_Up     ? -step
                     : e->key() == Qt::Key_Down ? step
                                                : 0);
        for (auto id : normalizedSelection(a, selected))
            if (!layerLocked(a, id))
                updateLayer(a, id, [&](QJsonObject &o) {
                    o["x"] = o["x"].toInt() + delta.x();
                    o["y"] = o["y"].toInt() + delta.y();
                });
        theme->data["layers"] = a;
        emit transientEdited();
        emit committed(old);
        return;
    }
    QWidget::keyPressEvent(e);
}
} // namespace Screen
