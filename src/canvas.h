#pragma once
#include "model.h"
#include <QImage>
#include <QWidget>
namespace Screen {
class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget *parent = nullptr);
    Theme *theme = nullptr;
    QStringList selected;
    QString tool = "move";
    bool editing = false, grid = false, snap = true, mirrored = false;
    void setFrame(QImage frame);
    void setFit(bool fit);
    void setZoom(double zoom);
    double zoom() const {
        return scale;
    }
    QRectF screenRect() const;
signals:
    void selectionChanged();
    void transientEdited();
    void committed(QJsonObject before);
    void createRequested(QString kind, QPointF point, QSizeF size);
    void contextRequested(QPoint global);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    QImage frame;
    double scale = 1;
    bool fit = true, dragging = false, resizing = false, marquee = false;
    QPointF start, last;
    QJsonObject before;
    QRectF selectionBox;
    QString hit(const QJsonArray &, QPointF p, bool deep = false, QPointF offset = {}) const;
    QString dragTarget(QPointF point, bool deep) const;
    bool canResize() const;
    QPointF logical(QPointF p) const;
    QRectF bounds(const QString &id) const;
};
} // namespace Screen
