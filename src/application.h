#pragma once
#include "mainwindow.h"
#include <QTemporaryFile>
#include <functional>
class QMenu;
namespace Screen {
class ApplicationController : public QObject {
    Q_OBJECT
    friend class GuiTest;
    friend class PerformanceProbe;
public:
    ApplicationController(QString directory, const QString &theme = {},
                          std::function<bool()> trayAvailable = QSystemTrayIcon::isSystemTrayAvailable);
    ~ApplicationController() override;
    MainWindow *window() const { return editor.get(); }
    DisplayRuntime &display() const { return *runtime; }
    bool isLightweight() const { return runtime->lightweight; }
    bool enterLightweight();
    void showWindow();
    void quit();
private:
    std::shared_ptr<DisplayRuntime> runtime;
    std::unique_ptr<MainWindow> editor;
    std::unique_ptr<QTemporaryFile> suspended;
    std::unique_ptr<QMenu> menu;
    QSystemTrayIcon *tray;
    QTimer countdown;
    std::function<bool()> trayAvailable;
    bool closed = false, unsaved = false;
    void makeWindow();
    void makeTray();
    void scheduleLightweight();
    QJsonObject editorState() const;
    void restoreEditor(const QJsonObject &state);
    QString L(const char *zh, const char *en) const;
};
} // namespace Screen
