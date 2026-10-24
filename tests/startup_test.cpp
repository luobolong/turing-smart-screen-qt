#include "startup.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
using namespace Screen;
static bool put(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
class SystemdHarness {
public:
    QTemporaryDir dir;
    QString unit = dir.path() + "/systemd/user/turing-screen.service";
    bool enabled = false, external = false, unavailable = false;
    QString fail;
    QList<QStringList> calls;
    StartupCommandResult run(const QStringList &args) {
        calls.append(args);
        const auto command = args.value(0);
        if (unavailable) return {1, {}, "Failed to connect to user bus"};
        if (command == fail) { fail.clear(); return {1, {}, "Injected systemd failure"}; }
        if (command == "show") {
            const bool masked = QFileInfo(unit).symLinkTarget() == "/dev/null";
            const bool exists = QFileInfo::exists(unit) || external;
            return {0, QString("LoadState=%1\nUnitFileState=%2\nFragmentPath=%3\n")
                .arg(masked ? "masked" : exists ? "loaded" : "not-found",
                     masked ? "masked" : enabled ? "enabled" : exists ? "disabled" : "",
                     masked ? "/dev/null" : QFileInfo::exists(unit) ? unit : external ? "/etc/systemd/user/turing-screen.service" : ""), {}};
        }
        if (command == "enable") enabled = true;
        if (command == "disable" && !external) enabled = false;
        if (command == "mask") {
            QDir().mkpath(QFileInfo(unit).absolutePath());
            if (!QFile::link("/dev/null", unit)) return {1, {}, "Cannot mask an existing file"};
        }
        if (command == "unmask" && QFileInfo(unit).symLinkTarget() == "/dev/null") QFile::remove(unit);
        return {0, {}, {}};
    }
    std::shared_ptr<SystemdStartupBackend> backend() {
        return std::make_shared<SystemdStartupBackend>(dir.path(), "/tmp/app with spaces/run.sh", "/tmp/data % $name",
            [this](const QStringList &args) { return run(args); });
    }
};
class StartupTest : public QObject {
    Q_OBJECT
private slots:
    void startupExecutablePreservesPortableAndNixWrappers() {
#ifndef Q_OS_LINUX
        QSKIP("Linux launcher selection is separate from Windows registry commands");
#else
        QTemporaryDir dir;
        const auto binary = dir.path() + "/turing-screen-qt";
        const auto launcher = dir.path() + "/run.sh";
        const auto wrapped = dir.path() + "/.turing-screen-qt-wrapped";
        for (const auto &file : {binary, launcher, wrapped}) {
            QVERIFY(put(file, "#!/bin/sh\nexit 0\n"));
            QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        QCOMPARE(startupExecutable(binary), launcher);
        QVERIFY(QFile::remove(launcher));
        QCOMPARE(startupExecutable(wrapped), binary);
#endif
    }
    void windowsRunEntryPersistsAndPreservesOtherApps() {
        QTemporaryDir dir;
        const auto path = dir.path() + "/run.ini";
        const auto settings = [path] { return std::make_unique<QSettings>(path, QSettings::IniFormat); };
        { auto store = settings(); store->setValue("OtherApp", "do not change"); }
        const auto command = windowsStartupCommand("C:\\Program Files\\Turing Screen\\app.exe", "C:\\Users\\User Name\\Data\\");
        QCOMPARE(command, QString("\"C:\\Program Files\\Turing Screen\\app.exe\" --data-dir \"C:\\Users\\User Name\\Data\\\\\""));
        WindowsStartupBackend first(command, settings);
        QVERIFY(!first.inspect().enabled);
        QVERIFY(first.setEnabled(true).error.isEmpty());
        WindowsStartupBackend reopened(command, settings);
        QVERIFY(reopened.inspect().enabled);
        QCOMPARE(settings()->value("TuringScreen").toString(), command);
        QCOMPARE(settings()->value("OtherApp").toString(), QString("do not change"));
        QVERIFY(reopened.setEnabled(false).error.isEmpty());
        QVERIFY(!first.inspect().enabled);
        QCOMPARE(settings()->value("OtherApp").toString(), QString("do not change"));
        WindowsStartupBackend tooLong(QString(261, 'x'), settings);
        QVERIFY(!tooLong.setEnabled(true).error.isEmpty());
        QVERIFY(!first.inspect().enabled);
        QCOMPARE(windowsStartupCommand("app.exe", "a\\\"b"), QString("\"app.exe\" --data-dir \"a\\\\\\\"b\""));
    }
    void systemdEnableDisableAndRollback() {
#ifndef Q_OS_LINUX
        QSKIP("POSIX service files and /dev/null masks are exercised on Linux");
#else
        SystemdHarness host;
        auto backend = host.backend();
        QVERIFY(!backend->inspect().enabled);
        auto state = backend->setEnabled(true);
        QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
        QVERIFY(state.enabled);
        QFile file(host.unit);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto unit = file.readAll(); file.close();
        QVERIFY(unit.contains("Type=exec"));
        QVERIFY(unit.contains("WantedBy=graphical-session.target"));
        QVERIFY(unit.contains("\"/tmp/app with spaces/run.sh\" --data-dir \"/tmp/data %% $$name\""));
        state = backend->setEnabled(false);
        QVERIFY2(state.error.isEmpty(), qPrintable(state.error));
        QVERIFY(!state.enabled);
        QCOMPARE(QFileInfo(host.unit).symLinkTarget(), QString("/dev/null"));
        QVERIFY(backend->setEnabled(true).enabled);
        host.fail = "mask";
        state = backend->setEnabled(false);
        QVERIFY(!state.error.isEmpty());
        QVERIFY(state.enabled); // Restore the old entry after a partial failure.
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), unit); file.close();
        for (const auto &call : host.calls) {
            QVERIFY(!call.contains("--now"));
            QVERIFY(call.value(0) != "start" && call.value(0) != "stop" && call.value(0) != "restart");
        }
        SystemdHarness failed;
        failed.fail = "enable";
        const auto failure = failed.backend()->setEnabled(true);
        QVERIFY(!failure.error.isEmpty());
        QVERIFY(!QFileInfo::exists(failed.unit));
#endif
    }
    void systemdNixosServiceIsReusedAndUserMaskOverridesIt() {
#ifndef Q_OS_LINUX
        QSKIP("NixOS and systemd user masks are Linux-specific");
#else
        SystemdHarness host;
        host.external = host.enabled = true;
        auto backend = host.backend();
        QVERIFY(backend->inspect().enabled);
        QVERIFY(backend->setEnabled(true).error.isEmpty());
        QVERIFY(!QFileInfo::exists(host.unit)); // Do not overwrite / shadow NixOS's unit.
        QVERIFY(backend->setEnabled(false).error.isEmpty());
        QVERIFY(!backend->inspect().enabled);
        QCOMPARE(QFileInfo(host.unit).symLinkTarget(), QString("/dev/null"));
        QVERIFY(backend->setEnabled(true).error.isEmpty());
        QVERIFY(backend->inspect().enabled);
        QVERIFY(!QFileInfo(host.unit).isSymLink());
        QVERIFY(!QFileInfo::exists(host.unit));
        QVERIFY(put(host.unit, "[Service]\nExecStart=/custom/launcher\n"));
        const auto blocked = backend->setEnabled(false);
        QVERIFY(!blocked.available);
        QVERIFY(!blocked.error.isEmpty());
        QFile custom(host.unit); QVERIFY(custom.open(QIODevice::ReadOnly));
        QCOMPARE(custom.readAll(), QByteArray("[Service]\nExecStart=/custom/launcher\n"));
        host.unavailable = true;
        QVERIFY(!backend->inspect().available);
#endif
    }
    void generatedUnitPassesSystemdVerification() {
#ifndef Q_OS_LINUX
        QSKIP("systemd-analyze is available on Linux");
#else
        const auto analyzer = QStandardPaths::findExecutable("systemd-analyze");
        if (analyzer.isEmpty()) QSKIP("systemd-analyze is not installed");
        QTemporaryDir dir;
        const auto executable = dir.path() + "/app % $test launcher";
        QVERIFY(put(executable, "#!/bin/sh\nexit 0\n"));
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto unit = dir.path() + "/turing-screen.service";
        QVERIFY(put(unit, systemdStartupUnit(executable, dir.path() + "/data % $arg \"quoted\" \\ value")));
        for (const auto &target : {"basic", "shutdown", "graphical-session", "graphical-session-pre"})
            QVERIFY(put(dir.path() + '/' + target + ".target", "[Unit]\nDescription=Isolated test target\nDefaultDependencies=no\n"));
        QVERIFY(put(dir.path() + "/app.slice", "[Slice]\n"));
        QVERIFY(put(dir.path() + "/-.slice", "[Slice]\n"));
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SYSTEMD_UNIT_PATH", dir.path());
        environment.insert("XDG_RUNTIME_DIR", dir.path());
        process.setProcessEnvironment(environment);
        process.start(analyzer, {"--user", "--man=no", "--generators=no", "verify", unit});
        QVERIFY(process.waitForFinished(10000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, systemdStartupUnit("/tmp/invalid\npath", dir.path()));
#endif
    }
    void offlineSystemctlChangesOnlyTemporaryUserConfiguration() {
#ifndef Q_OS_LINUX
        QSKIP("Offline systemctl file operations are Linux-specific");
#else
        const auto systemctl = QStandardPaths::findExecutable("systemctl");
        if (systemctl.isEmpty()) QSKIP("systemctl is not installed");
        QTemporaryDir dir;
        const auto config = dir.path() + "/config";
        const auto local = config + "/systemd/user";
        const auto global = dir.path() + "/global/systemd/user";
        const auto name = QString("turing-screen.service");
        QVERIFY(put(global + '/' + name, systemdStartupUnit(QCoreApplication::applicationFilePath(), dir.path())));
        QVERIFY(put(local + '/' + name, systemdStartupUnit(QCoreApplication::applicationFilePath(), dir.path())));
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", config);
        environment.insert("XDG_CONFIG_DIRS", dir.path() + "/global");
        environment.insert("XDG_RUNTIME_DIR", dir.path() + "/runtime");
        environment.insert("SYSTEMD_UNIT_PATH", local + ':' + global);
        const auto run = [&](QStringList arguments) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(systemctl, QStringList{"--user", "--root=/"} + arguments);
            if (!process.waitForFinished(5000)) return StartupCommandResult{-1, {}, "offline command timed out"};
            return StartupCommandResult{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput()).trimmed(),
                                         QString::fromUtf8(process.readAllStandardError())};
        };
        QCOMPARE(run({"is-enabled", name}).output, QString("disabled"));
        const auto enable = run({"enable", name});
        QVERIFY2(enable.exitCode == 0, qPrintable(enable.error));
        QCOMPARE(run({"is-enabled", name}).output, QString("enabled"));
        const auto disable = run({"disable", name});
        QVERIFY2(disable.exitCode == 0, qPrintable(disable.error));
        QCOMPARE(run({"is-enabled", name}).output, QString("disabled"));
        QVERIFY(QFile::remove(local + '/' + name));
        const auto mask = run({"mask", name});
        QVERIFY2(mask.exitCode == 0, qPrintable(mask.error));
        QCOMPARE(QFileInfo(local + '/' + name).symLinkTarget(), QString("/dev/null"));
        QCOMPARE(run({"is-enabled", name}).output, QString("masked"));
        QVERIFY(QFileInfo::exists(global + '/' + name));
        const auto unmask = run({"unmask", name});
        QVERIFY2(unmask.exitCode == 0, qPrintable(unmask.error));
        QCOMPARE(run({"is-enabled", name}).output, QString("disabled"));
#endif
    }
};
QTEST_GUILESS_MAIN(StartupTest)
#include "startup_test.moc"
