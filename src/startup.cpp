#include "startup.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <stdexcept>
namespace Screen {
static const QString unitName = QStringLiteral("turing-screen.service");
static const QByteArray unitMarker = "# Managed by Turing Screen.\n";
static void validArgument(const QString &text) {
    for (const auto ch : text)
        if (ch.isNull() || ch == '\n' || ch == '\r') throw std::runtime_error("Startup paths must not contain line breaks or NUL characters.");
}
static QString windowsQuote(const QString &text) {
    validArgument(text);
    QString result = "\"";
    int backslashes = 0;
    for (const auto ch : text) {
        if (ch == '\\') { ++backslashes; continue; }
        result += QString(backslashes * (ch == '"' ? 2 : 1) + (ch == '"' ? 1 : 0), '\\');
        backslashes = 0;
        result += ch;
    }
    return result + QString(backslashes * 2, '\\') + '"';
}
QString windowsStartupCommand(const QString &executable, const QString &dataDirectory) {
    return windowsQuote(executable) + " --data-dir " + windowsQuote(dataDirectory);
}
static QString systemdQuote(QString text, bool executable = false) {
    validArgument(text);
    if (executable) {
        for (const auto ch : text)
            if (ch.unicode() < 32 || ch.unicode() == 127 || ch == '\\' || ch == '"' || ch == '\'')
                throw std::runtime_error("Systemd cannot use this executable path. Move the application to a path without quotes, backslashes or control characters.");
    }
    text.replace('\\', "\\\\").replace('"', "\\\"").replace('\t', "\\t");
    text.replace('%', "%%");
    // The executable path is expanded separately, without environment expansion.
    if (!executable) text.replace('$', "$$");
    return '"' + text + '"';
}
QByteArray systemdStartupUnit(const QString &executable, const QString &dataDirectory) {
    return unitMarker + QString(
        "[Unit]\nDescription=Turing Screen smart display monitor\n"
        "After=graphical-session-pre.target\nPartOf=graphical-session.target\n\n"
        "[Service]\nType=exec\nExecStart=%1 --data-dir %2\nRestart=on-failure\nRestartSec=5s\n"
        "TimeoutStopSec=10s\nSlice=app.slice\n\n[Install]\nWantedBy=graphical-session.target\n")
        .arg(systemdQuote(executable, true), systemdQuote(dataDirectory)).toUtf8();
}
QString startupExecutable(const QString &applicationFile) {
    QFileInfo app(applicationFile);
#ifdef Q_OS_LINUX
    // Preserve the Qt environment supplied by Nix and portable launchers.
    if (app.fileName().startsWith('.') && app.fileName().endsWith("-wrapped")) {
        const auto wrapper = app.dir().filePath(app.fileName().mid(1).chopped(8));
        if (QFileInfo(wrapper).isExecutable()) app.setFile(wrapper);
    }
    const auto launcher = app.dir().filePath("run.sh");
    if (QFileInfo(launcher).isExecutable()) return launcher;
    const auto stable = QStandardPaths::findExecutable("turing-screen-qt");
    if (!stable.isEmpty() && QFileInfo(stable).canonicalFilePath() == app.canonicalFilePath()) return stable;
#endif
    return app.absoluteFilePath();
}
WindowsStartupBackend::WindowsStartupBackend(QString command, Settings settings)
    : command(std::move(command)), settings(std::move(settings)) {}
StartupState WindowsStartupBackend::inspect() {
    auto registry = settings();
    StartupState state;
    state.enabled = !registry->value("TuringScreen").toString().isEmpty();
    if (registry->status() != QSettings::NoError) {
        state.available = false;
        state.error = "Cannot read the current user's startup entry.";
    }
    return state;
}
StartupState WindowsStartupBackend::setEnabled(bool enabled) {
    auto registry = settings();
    if (enabled && command.size() > 260) {
        auto state = inspect();
        state.error = "The startup command exceeds the Windows Run entry limit (260 characters).";
        return state;
    }
    if (enabled) registry->setValue("TuringScreen", command);
    else registry->remove("TuringScreen");
    registry->sync();
    auto state = inspect();
    if (registry->status() != QSettings::NoError || state.enabled != enabled)
        state.error = "Could not update the current user's startup entry.";
    return state;
}
SystemdStartupBackend::SystemdStartupBackend(QString configDirectory, QString executable,
                                           QString dataDirectory, Runner runner)
    : unitFile(QDir(configDirectory).filePath("systemd/user/" + unitName)),
      executable(std::move(executable)), dataDirectory(std::move(dataDirectory)), runner(std::move(runner)) {}
bool SystemdStartupBackend::ownedUnit() const {
    if (QFileInfo(unitFile).isSymLink()) return false;
    QFile file(unitFile);
    return file.open(QIODevice::ReadOnly) && file.read(unitMarker.size()) == unitMarker;
}
void SystemdStartupBackend::command(const QStringList &arguments) {
    const auto result = runner(arguments);
    if (result.exitCode != 0)
        throw std::runtime_error((result.error.isEmpty() ? result.output : result.error).trimmed().toStdString());
    if (arguments.value(0) != "daemon-reload") {
        const auto reload = runner({"daemon-reload"});
        if (reload.exitCode != 0) throw std::runtime_error(reload.error.trimmed().toStdString());
    }
}
StartupState SystemdStartupBackend::inspect() {
    const auto result = runner({"show", unitName, "--property=LoadState,UnitFileState,FragmentPath"});
    QHash<QString, QString> properties;
    for (const auto &line : result.output.split('\n')) {
        const auto equals = line.indexOf('=');
        if (equals > 0) properties.insert(line.left(equals), line.mid(equals + 1));
    }
    StartupState state;
    state.unitState = properties.value("UnitFileState");
    state.fragment = properties.value("FragmentPath");
    state.enabled = state.unitState == "enabled";
    if (!properties.contains("LoadState")) {
        state.available = false;
        state.error = "Systemd user services are unavailable. " + result.error.trimmed();
    } else if ((QFileInfo::exists(unitFile) || QFileInfo(unitFile).isSymLink()) && !ownedUnit() &&
               QFileInfo(unitFile).symLinkTarget() != "/dev/null") {
        state.available = false;
        state.error = "This user service is managed outside the application. Change its startup setting in your system or Home Manager configuration.";
    }
    return state;
}
static void saveUnit(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) throw std::runtime_error("Cannot create the user service directory.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        throw std::runtime_error("Cannot save the user service.");
}
StartupState SystemdStartupBackend::setEnabled(bool enabled) {
    const auto before = inspect();
    if (!before.available) return before;
    const bool wasMasked = before.unitState.startsWith("masked");
    const bool wasOwned = ownedUnit();
    QByteArray original;
    if (wasOwned) { QFile file(unitFile); if (file.open(QIODevice::ReadOnly)) original = file.readAll(); }
    bool edited = false;
    try {
        if (enabled) {
            if (wasMasked) {
                command({"unmask", unitName});
                command({"unmask", "--runtime", unitName});
            }
            const auto current = inspect();
            if (!current.available) throw std::runtime_error(current.error.toStdString());
            if (wasOwned || current.fragment.isEmpty()) {
                if (QFileInfo(unitFile).isSymLink()) throw std::runtime_error("The user service path is managed externally.");
                saveUnit(unitFile, systemdStartupUnit(executable, dataDirectory));
                edited = true;
                command({"daemon-reload"});
            }
            if (!inspect().enabled) command({"enable", unitName});
        } else {
            if (before.unitState == "masked") return before;
            command({"disable", unitName});
            if (wasOwned) {
                if (!QFile::remove(unitFile)) throw std::runtime_error("Cannot remove the application-managed user service.");
                edited = true;
                command({"daemon-reload"});
            }
            // A user mask also overrides NixOS's read-only /etc enable links.
            // Never use --now: the current display session must keep running.
            command({"mask", unitName});
        }
        const auto after = inspect();
        if (!after.available || after.enabled != enabled)
            throw std::runtime_error("Systemd did not apply the requested startup setting.");
        return after;
    } catch (const std::exception &failure) {
        // Restore our unit and enable state after a partial operation.
        try {
            if (inspect().unitState.startsWith("masked")) command({"unmask", unitName});
            if (edited) {
                if (wasOwned) saveUnit(unitFile, original);
                else if (ownedUnit()) QFile::remove(unitFile);
                command({"daemon-reload"});
            }
            if (wasMasked) command(before.unitState == "masked-runtime"
                ? QStringList{"mask", "--runtime", unitName} : QStringList{"mask", unitName});
            else command({before.enabled ? "enable" : "disable", unitName});
        } catch (const std::exception &) {}
        auto state = inspect();
        state.error = QString::fromUtf8(failure.what());
        return state;
    }
}
std::shared_ptr<StartupBackend> nativeStartupBackend(const QString &dataDirectory) {
    const auto executable = startupExecutable(QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    return std::make_shared<WindowsStartupBackend>(windowsStartupCommand(QDir::toNativeSeparators(executable),
        QDir::toNativeSeparators(QDir(dataDirectory).absolutePath())), [] {
            return std::make_unique<QSettings>("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                                QSettings::Registry64Format);
        });
#else
    const auto systemctl = QStandardPaths::findExecutable("systemctl");
    QStringList unitPaths{QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user.control",
                          QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user"};
    const auto addRoots = [&unitPaths](const QStringList &roots) {
        for (const auto &root : roots) if (!root.isEmpty()) unitPaths.append(root + "/systemd/user");
    };
    addRoots(qEnvironmentVariable("XDG_CONFIG_DIRS", "/etc/xdg").split(':', Qt::SkipEmptyParts));
    unitPaths.append("/etc/systemd/user");
    addRoots(QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation));
    unitPaths.append({"/usr/local/lib/systemd/user", "/usr/lib/systemd/user"});
    for (const auto &path : qEnvironmentVariable("SYSTEMD_UNIT_PATH").split(':', Qt::SkipEmptyParts))
        if (!path.startsWith("/run/user/") && !path.startsWith("/run/systemd/") && !path.contains("/systemd/generator"))
            unitPaths.append(path);
    unitPaths.removeDuplicates();
    return std::make_shared<SystemdStartupBackend>(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
        executable, QDir(dataDirectory).absolutePath(), [systemctl, unitPaths](const QStringList &arguments) {
            const auto execute = [&](const QStringList &args, bool persistent) {
                StartupCommandResult result;
                if (systemctl.isEmpty()) { result.error = "systemctl was not found."; return result; }
                QProcess process;
                if (persistent) {
                    auto environment = QProcessEnvironment::systemEnvironment();
                    environment.insert("SYSTEMD_UNIT_PATH", unitPaths.join(':'));
                    process.setProcessEnvironment(environment);
                }
                process.start(systemctl, QStringList{"--user", "--no-pager"} + args);
                if (!process.waitForStarted(3000)) { result.error = process.errorString(); return result; }
                if (!process.waitForFinished(4000)) {
                    process.kill(); process.waitForFinished(1000);
                    result.error = "Systemd request timed out."; return result;
                }
                result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
                result.output = QString::fromUtf8(process.readAllStandardOutput());
                result.error = QString::fromUtf8(process.readAllStandardError());
                return result;
            };
            if (arguments.value(0) == "show") {
                // Query next-login configuration, not a same-named transient instance.
                const auto manager = execute({"show", "--property=Version"}, false);
                if (manager.exitCode != 0) return manager;
                const auto state = execute({"--root=/", "is-enabled", unitName}, true);
                QString fragment;
                for (const auto &directory : unitPaths) {
                    const QFileInfo file(QDir(directory).filePath(unitName));
                    if (!file.exists() && !file.isSymLink()) continue;
                    fragment = file.symLinkTarget() == "/dev/null" ? "/dev/null" : file.absoluteFilePath();
                    break;
                }
                if (!fragment.isEmpty() && state.output.trimmed().isEmpty()) return state;
                return StartupCommandResult{0, QString("LoadState=%1\nUnitFileState=%2\nFragmentPath=%3\n")
                    .arg(fragment.isEmpty() ? "not-found" : fragment == "/dev/null" ? "masked" : "loaded",
                         state.output.trimmed(), fragment), state.error};
            }
            if (arguments.value(0) == "daemon-reload") return execute(arguments, false);
            // File operations stay separate from running units; reload never starts/stops them.
            return execute(QStringList{"--root=/"} + arguments, true);
        });
#endif
}
StartupController::StartupController(std::shared_ptr<StartupBackend> backend, QObject *parent)
    : QObject(parent), backend(std::move(backend)) {}
StartupController::~StartupController() {
    if (worker) { worker->wait(); delete worker; }
}
void StartupController::refresh() { run({}); }
void StartupController::setEnabled(bool enabled) { run(enabled); }
void StartupController::run(std::optional<bool> enabled) {
    if (busy()) return;
    auto result = std::make_shared<StartupState>();
    worker = QThread::create([backend = backend, result, enabled] {
        try { *result = enabled ? backend->setEnabled(*enabled) : backend->inspect(); }
        catch (const std::exception &error) { result->error = QString::fromUtf8(error.what()); result->available = false; }
    });
    connect(worker, &QThread::finished, this, [this, result] {
        worker->deleteLater(); worker = nullptr;
        current = *result; loaded = true;
        emit changed();
    });
    worker->start();
    emit changed();
}
} // namespace Screen
