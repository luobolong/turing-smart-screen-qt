#pragma once
#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QThread>
#include <functional>
#include <memory>
#include <optional>
namespace Screen {
struct StartupState {
    bool enabled = false, available = true;
    QString error, unitState, fragment;
};
class StartupBackend {
public:
    virtual ~StartupBackend() = default;
    virtual StartupState inspect() = 0;
    virtual StartupState setEnabled(bool enabled) = 0;
};
struct StartupCommandResult {
    int exitCode = -1;
    QString output, error;
};
QString windowsStartupCommand(const QString &executable, const QString &dataDirectory);
QByteArray systemdStartupUnit(const QString &executable, const QString &dataDirectory);
QString startupExecutable(const QString &applicationFile);
class WindowsStartupBackend : public StartupBackend {
public:
    using Settings = std::function<std::unique_ptr<QSettings>()>;
    WindowsStartupBackend(QString command, Settings settings);
    StartupState inspect() override;
    StartupState setEnabled(bool enabled) override;
private:
    QString command;
    Settings settings;
};
class SystemdStartupBackend : public StartupBackend {
public:
    using Runner = std::function<StartupCommandResult(const QStringList &)>;
    SystemdStartupBackend(QString configDirectory, QString executable, QString dataDirectory, Runner runner);
    StartupState inspect() override;
    StartupState setEnabled(bool enabled) override;
private:
    QString unitFile, executable, dataDirectory;
    Runner runner;
    void command(const QStringList &arguments);
    bool ownedUnit() const;
};
std::shared_ptr<StartupBackend> nativeStartupBackend(const QString &dataDirectory);
class StartupController : public QObject {
    Q_OBJECT
public:
    explicit StartupController(std::shared_ptr<StartupBackend> backend, QObject *parent = nullptr);
    ~StartupController() override;
    const StartupState &state() const { return current; }
    bool busy() const { return worker != nullptr; }
    bool ready() const { return loaded; }
    void refresh();
    void setEnabled(bool enabled);
signals:
    void changed();
private:
    StartupState current;
    bool loaded = false;
    std::shared_ptr<StartupBackend> backend;
    QThread *worker = nullptr;
    void run(std::optional<bool> enabled);
};
} // namespace Screen
