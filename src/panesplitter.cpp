#include "panesplitter.h"
#include <QFrame>
#include <QToolButton>
#include <QEvent>
#include <QTimer>
#include <QResizeEvent>
namespace Screen {
class PaneSplitterHandle : public QSplitterHandle {
    PaneSplitter *owner;
    QFrame *separator;
    QToolButton *toggle;
    bool hovered = false;
public:
    PaneSplitterHandle(Qt::Orientation orientation, PaneSplitter *parent)
        : QSplitterHandle(orientation, parent), owner(parent), separator(new QFrame(this)), toggle(new QToolButton(this)) {
        separator->setObjectName("paneSeparator");
        separator->setFrameShadow(QFrame::Plain);
        separator->setLineWidth(1);
        separator->setForegroundRole(QPalette::Mid);
        separator->setAttribute(Qt::WA_TransparentForMouseEvents);
        toggle->setObjectName("paneToggle");
        toggle->setAutoRaise(true);
        toggle->setFocusPolicy(Qt::NoFocus);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->hide();
        connect(toggle, &QToolButton::clicked, this, [this] {
            const auto index = owner->controlledPane(this);
            if (index >= 0) owner->setPaneCollapsed(index, !owner->paneCollapsed(index));
            updateControl();
        });
    }
    void updateControl() {
        separator->setFrameShape(orientation() == Qt::Horizontal ? QFrame::VLine : QFrame::HLine);
        separator->setGeometry(rect());
        const auto index = owner->controlledPane(this);
        if (index < 0) { toggle->hide(); return; }
        const bool collapsed = owner->paneCollapsed(index);
        const bool atStart = index == 0;
        const bool forward = atStart == collapsed;
        toggle->setArrowType(orientation() == Qt::Horizontal ? (forward ? Qt::RightArrow : Qt::LeftArrow)
            : (forward ? Qt::DownArrow : Qt::UpArrow));
        toggle->setProperty("paneIndex", index);
        toggle->setProperty("paneCollapsed", collapsed);
        toggle->setToolTip((collapsed ? owner->showPaneText : owner->hidePaneText).arg(owner->panes[index].name));
        toggle->setAccessibleName(toggle->toolTip());
        if (orientation() == Qt::Horizontal) toggle->setGeometry(0, qMax(0, (height() - 36) / 2), width(), qMin(36, height()));
        else toggle->setGeometry(qMax(0, (width() - 36) / 2), 0, qMin(36, width()), height());
        toggle->setVisible(hovered);
        toggle->raise();
    }
protected:
    void enterEvent(QEnterEvent *event) override { hovered = true; QSplitterHandle::enterEvent(event); updateControl(); }
    void leaveEvent(QEvent *event) override { hovered = false; QSplitterHandle::leaveEvent(event); toggle->hide(); }
    void resizeEvent(QResizeEvent *event) override { QSplitterHandle::resizeEvent(event); updateControl(); }
    bool event(QEvent *event) override {
        const bool result = QSplitterHandle::event(event);
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::LayoutRequest) updateControl();
        return result;
    }
};
PaneSplitter::PaneSplitter(QWidget *parent) : QSplitter(parent) {
    setHandleWidth(12);
    setChildrenCollapsible(false);
    connect(this, &QSplitter::splitterMoved, this, [this] { synchronizePanes(); });
}
QSplitterHandle *PaneSplitter::createHandle() { return new PaneSplitterHandle(orientation(), this); }
int PaneSplitter::controlledPane(QSplitterHandle *selected) const {
    for (int n = 1; n < count(); ++n) if (handle(n) == selected) {
        if (n == 1 && panes.contains(0)) return 0;
        if (n == count() - 1 && panes.contains(count() - 1)) return count() - 1;
    }
    return -1;
}
void PaneSplitter::addCollapsiblePane(int index, const QString &name, bool collapsed) {
    panes[index] = {name, false, 280, 200};
    setCollapsible(index, true);
    if (collapsed) setPaneCollapsed(index, true);
    updateHandles();
}
bool PaneSplitter::paneCollapsed(int index) const { return panes.value(index).collapsed; }
void PaneSplitter::setPaneCollapsed(int index, bool collapsed) {
    if (!panes.contains(index) || index < 0 || index >= count()) return;
    auto &pane = panes[index];
    if (pane.collapsed == collapsed) return;
    auto values = sizes();
    const int center = index == 0 ? 1 : index - 1;
    auto &saved = orientation() == Qt::Horizontal ? pane.width : pane.height;
    if (collapsed) {
        saved = qMax(1, values[index]);
        values[center] += values[index];
        values[index] = 0;
    } else {
        const auto minimum = orientation() == Qt::Horizontal ? widget(index)->minimumSizeHint().width() : widget(index)->minimumSizeHint().height();
        values[index] = qMax(saved, minimum);
        values[center] = qMax(1, values[center] - values[index]);
    }
    pane.collapsed = collapsed;
    setSizes(values);
    updateHandles();
    if (collapsedChanged) collapsedChanged(index, collapsed);
}
void PaneSplitter::setPaneOrientation(Qt::Orientation next, const QList<int> &defaults) {
    if (orientation() == next) return;
    const auto previous = sizes();
    for (auto it = panes.begin(); it != panes.end(); ++it) if (!it->collapsed && it.key() < previous.size())
        (orientation() == Qt::Horizontal ? it->width : it->height) = previous[it.key()];
    setOrientation(next);
    auto values = defaults;
    for (auto it = panes.begin(); it != panes.end(); ++it) if (it->collapsed) values[it.key()] = 0;
    setSizes(values);
    updateHandles();
}
void PaneSplitter::synchronizePanes() {
    const auto values = sizes();
    for (auto it = panes.begin(); it != panes.end(); ++it) {
        const bool collapsed = values[it.key()] == 0;
        if (!collapsed) (orientation() == Qt::Horizontal ? it->width : it->height) = values[it.key()];
        if (collapsed != it->collapsed) {
            it->collapsed = collapsed;
            if (collapsedChanged) collapsedChanged(it.key(), collapsed);
        }
    }
    updateHandles();
}
void PaneSplitter::updateHandles() {
    for (int n = 1; n < count(); ++n) static_cast<PaneSplitterHandle *>(handle(n))->updateControl();
}
}
