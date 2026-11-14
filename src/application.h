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
                          std::function<bool()> trayAvailable = QSystemTrayIcon::isSystemTrayAvailable,
                          bool autostart = false);
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
    QMenu *portsMenu = nullptr;
    QSystemTrayIcon *tray;
    QAction *showAction = nullptr, *collectAction = nullptr, *lightAction = nullptr, *sendAction = nullptr, *quitAction = nullptr;
    QTimer countdown;
    QTimer trayRetry;
    int trayRetryCount = 0;
    std::function<bool()> trayAvailable;
    std::function<QList<Port>()> listPorts = Device::ports;
    bool closed = false, unsaved = false, trayMenuOpen = false;
    void makeWindow();
    void makeTray();
    void showTrayMenu();
    void updateTrayState();
    void refreshPortMenu();
    void updatePortChecks(const DeviceStatus &state);
    void connectTrayPort(const QString &port);
    void scheduleLightweight();
    QJsonObject editorState() const;
    void restoreEditor(const QJsonObject &state);
    QString L(const char *zh, const char *en) const;
};
} // namespace Screen
