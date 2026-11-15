#include "canvas.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QBuffer>
#include <QImageReader>
#include <cmath>
namespace Screen {
Canvas::Canvas(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 220);
    setAutoFillBackground(false);
}
void Canvas::setFrame(QImage f) {
    if (frame.cacheKey() == f.cacheKey()) return;
    frame = std::move(f);
    update();
    emit zoomChanged(zoom());
}
void Canvas::setFit(bool v) {
    fit = v;
    setMinimumSize(v ? QSize(240, 220)
                     : QSize(int(frame.width() * scale + 48), int(frame.height() * scale + 48)));
    update();
    emit zoomChanged(zoom());
}
void Canvas::setZoom(double v) {
    scale = qBound(.25, v, 4.);
    fit = false;
    // Keep the viewport's layout stable; zoom changes the drawing, not the pane size.
    setMinimumSize(160, 120);
    updateGeometry();
    update();
    emit zoomChanged(zoom());
}
double Canvas::zoom() const {
    const int width = frame.isNull() ? (theme ? theme->size().width() : 480) : frame.width();
    return fit ? screenRect().width() / qMax(1, width) : scale;
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
QLineF Canvas::screenLine(const QString &id) const {
    auto line = layerLine(findLayer(theme->layers(), id)).translated(absolutePosition(theme->layers(), id));
    const auto screen = screenRect();
    const double viewScale = screen.width() / theme->size().width();
    const auto point = [&](QPointF p) {
        if (mirrored) p.setX(theme->size().width() - p.x());
        return screen.topLeft() + p * viewScale;
    };
    return QLineF(point(line.p1()), point(line.p2()));
}
void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), palette().brush(QPalette::Window));
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
        auto gridColor = palette().color(QPalette::Mid);
        gridColor.setAlpha(96);
        p.setPen(gridColor);
        for (int x = 0; x <= theme->size().width(); x += 8)
            p.drawLine(QPointF(r.left() + x * z, r.top()), QPointF(r.left() + x * z, r.bottom()));
        for (int y = 0; y <= theme->size().height(); y += 8)
            p.drawLine(QPointF(r.left(), r.top() + y * z), QPointF(r.right(), r.top() + y * z));
    }
    if (!drawingLine) for (auto id : selected) {
        const auto layer = findLayer(theme->layers(), id);
        if (layer["kind"] == "line") {
            const auto line = screenLine(id);
            p.setPen(QPen(palette().brush(QPalette::Highlight), 1.4));
            p.drawLine(line);
            if (!layerLocked(theme->layers(), id)) {
                p.setBrush(palette().brush(QPalette::HighlightedText));
                for (const auto point : {line.p1(), line.p2()})
                    p.drawRect(QRectF(point - QPointF(3, 3), QSizeF(6, 6)));
            }
            continue;
        }
        auto b = bounds(id);
        p.setPen(QPen(palette().brush(QPalette::Highlight), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawRect(b);
        if (!layerLocked(theme->layers(), id) && findLayer(theme->layers(), id)["kind"] != "group") {
            p.setBrush(palette().brush(QPalette::HighlightedText));
            for (auto pt : {b.topLeft(), b.topRight(), b.bottomLeft(), b.bottomRight()})
                p.drawRect(QRectF(pt - QPointF(3, 3), QSizeF(6, 6)));
        }
    }
    if (drawingLine) {
        p.setClipRect(r);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#66d9df"), z));
        p.drawLine(start, last);
    } else if (marquee) {
        p.setPen(QPen(palette().brush(QPalette::Highlight), 1, Qt::DashLine));
        auto selectionColor = palette().color(QPalette::Highlight);
        selectionColor.setAlpha(25);
        p.setBrush(selectionColor);
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
        if (l["kind"] == "line") {
            const auto line = layerLine(l).translated(at);
            const auto delta = line.p2() - line.p1();
            const double lengthSquared = QPointF::dotProduct(delta, delta);
            const double fraction = lengthSquared > 0 ? qBound(0., QPointF::dotProduct(point - line.p1(), delta) / lengthSquared, 1.) : 0.;
            const double tolerance = qMax(4. / zoom(), l["stroke_width"].toInt(1) / 2.);
            if (QLineF(point, line.p1() + delta * fraction).length() <= tolerance) return l["id"].toString();
        } else if (layerBounds(l).translated(at).adjusted(-layerStrokeWidth(l), -layerStrokeWidth(l),
                                                         layerStrokeWidth(l), layerStrokeWidth(l)).contains(point))
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
int Canvas::resizeHandle(QPointF point) const {
    if (!canResize()) return -1;
    const auto layer = findLayer(theme->layers(), selected.first());
    if (layer["kind"] == "line") return -1;
    const auto rect = bounds(selected.first());
    const QList<QPointF> corners{rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()};
    for (int i = 0; i < corners.size(); ++i) {
        if (layer["kind"] != "image" && i != 3) continue;
        if (QRectF(corners[i] - QPointF(7, 7), QSizeF(14, 14)).contains(point)) return i;
    }
    return -1;
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    setFocus();
    if (e->button() == Qt::RightButton) {
        if (!editing || !theme) return;
        drawingLine = false;
        const auto id = dragTarget(logical(e->position()), e->modifiers() & Qt::AltModifier);
        if (id.isEmpty() || !selected.contains(id)) {
            selected = id.isEmpty() ? QStringList() : QStringList{id};
            emit selectionChanged();
            update();
        }
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
    drawingLine = false;
    lineEndpoint = -1;
    resizeCorner = -1;
    imageOriginalSize = {};
    if (tool == "text") {
        emit createRequested("text", logical(start), QSizeF(120, 30));
        return;
    }
    if (tool == "line") {
        drawingLine = true;
        update();
        return;
    }
    if (tool == "rectangle" || tool == "ellipse") {
        marquee = true;
        update();
        return;
    }
    if (!(e->modifiers() & Qt::AltModifier) && canResize() &&
        findLayer(theme->layers(), selected.first())["kind"] == "line") {
        const auto line = screenLine(selected.first());
        for (int endpoint = 0; endpoint < 2; ++endpoint) {
            const auto point = endpoint ? line.p2() : line.p1();
            if (!QRectF(point - QPointF(7, 7), QSizeF(14, 14)).contains(start)) continue;
            resizing = true;
            lineEndpoint = endpoint;
            const auto layer = findLayer(theme->layers(), selected.first());
            selectionLine = layerLine(layer).translated(layer["x"].toDouble(), layer["y"].toDouble());
            return;
        }
    } else if (!(e->modifiers() & Qt::AltModifier)) {
        const int corner = resizeHandle(start);
        if (corner >= 0) {
            resizing = true;
            const auto layer = findLayer(theme->layers(), selected.first());
            selectionBox = layerBounds(layer);
            resizeCorner = corner ^ (mirrored ? 1 : 0);
            if (layer["kind"] == "image") {
                auto bytes = QByteArray::fromBase64(layer["data"].toString().toLatin1());
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::ReadOnly);
                imageOriginalSize = QImageReader(&buffer).size();
                if (!imageOriginalSize.isValid()) imageOriginalSize = selectionBox.size().toSize();
            }
            return;
        }
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
        if (canResize() && findLayer(theme->layers(), selected.first())["kind"] == "line") {
            const auto line = screenLine(selected.first());
            const bool endpoint = QLineF(last, line.p1()).length() <= 7 || QLineF(last, line.p2()).length() <= 7;
            setCursor(endpoint ? Qt::CrossCursor : Qt::ArrowCursor);
        } else {
            const int corner = resizeHandle(last);
            setCursor(corner < 0 ? Qt::ArrowCursor : (corner == 0 || corner == 3 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor));
        }
        return;
    }
    if (resizing && selected.size() == 1) {
        auto a = theme->layers();
        auto delta = logical(last) - logical(start);
        updateLayer(a, selected.first(), [&](QJsonObject &l) {
            if (l["kind"] == "group")
                return;
            if (l["kind"] == "line") {
                auto line = selectionLine;
                if (lineEndpoint == 0) line.setP1(line.p1() + delta);
                else line.setP2(line.p2() + delta);
                setLayerLine(l, line);
                return;
            }
            if (l["kind"] == "image") {
                const bool right = resizeCorner & 1, bottom = resizeCorner & 2;
                double width = qMax(1., selectionBox.width() + (right ? delta.x() : -delta.x()));
                double height = qMax(1., selectionBox.height() + (bottom ? delta.y() : -delta.y()));
                if ((e->modifiers() & Qt::ShiftModifier) && imageOriginalSize.isValid()) {
                    const bool horizontal = qAbs(delta.x()) / selectionBox.width() >= qAbs(delta.y()) / selectionBox.height();
                    double imageScale = horizontal ? width / imageOriginalSize.width() : height / imageOriginalSize.height();
                    imageScale = qMin(imageScale, qMin(1536. / imageOriginalSize.width(), 1536. / imageOriginalSize.height()));
                    width = imageOriginalSize.width() * imageScale;
                    height = imageOriginalSize.height() * imageScale;
                }
                const int w = qBound(1, qRound(width), 1536), h = qBound(1, qRound(height), 1536);
                const auto original = findLayer(before["layers"].toArray(), selected.first());
                l["x"] = qRound(original["x"].toDouble() + (right ? 0 : selectionBox.width() - w));
                l["y"] = qRound(original["y"].toDouble() + (bottom ? 0 : selectionBox.height() - h));
                l["w"] = w;
                l["h"] = h;
                return;
            }
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
    if (drawingLine) {
        const auto first = logical(start), end = logical(last);
        if (first.toPoint() != end.toPoint())
            emit createRequested("line", first, QSizeF(end.x() - first.x(), end.y() - first.y()));
    } else if (marquee) {
        if (tool == "rectangle" || tool == "ellipse") {
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
    dragging = resizing = marquee = drawingLine = false;
    lineEndpoint = -1;
    resizeCorner = -1;
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
    emit zoomChanged(zoom());
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (drawingLine && e->key() == Qt::Key_Escape) {
        drawingLine = false;
        update();
        return;
    }
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
