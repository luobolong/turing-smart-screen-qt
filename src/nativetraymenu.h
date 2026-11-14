#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <QHash>
#include <QPointer>
#include <QtCore/qt_windows.h>
class QAction;
class QMenu;
namespace Screen {
// QMenu remains the action model; Windows owns all popup rendering.
class NativeTrayMenu {
public:
    explicit NativeTrayMenu(QMenu *source);
    ~NativeTrayMenu();
    Q_DISABLE_COPY_MOVE(NativeTrayMenu)
    bool popup();
private:
    friend class GuiTest;
    HMENU handle = nullptr;
    QHash<UINT, QPointer<QAction>> commands;
    HMENU build(QMenu *source);
};
} // namespace Screen
#endif
