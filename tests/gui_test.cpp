#include "mainwindow.h"
#include "application.h"
#include "pseudoscreen.h"
#include <QJsonDocument>
#include <QMouseEvent>
#include <QtTest>
#include <QtWidgets>
#include <algorithm>
#include <stdexcept>
namespace Screen {
class TestProfile : public QTemporaryDir {
public:
    TestProfile() {
        QFile config(path() + "/config.json");
        const QByteArray offline = "{\"weather_enabled\":false}";
        if (!config.open(QIODevice::WriteOnly) || config.write(offline) != offline.size())
            throw std::runtime_error("Cannot create offline test profile");
    }
};
class TestStartupBackend : public StartupBackend {
public:
    bool enabled = true;
    QString failure;
    int writes = 0;
    StartupState inspect() override { return {enabled, true, {}}; }
    StartupState setEnabled(bool value) override {
        ++writes;
        if (!failure.isEmpty()) return {enabled, true, failure};
        enabled = value;
        return inspect();
    }
};
static QPoint canvasPoint(const Canvas &canvas, QPointF logical) {
    if (canvas.mirrored)
        logical.setX(canvas.theme->size().width() - logical.x());
    const auto screen = canvas.screenRect();
    return (screen.topLeft() + logical * (screen.width() / canvas.theme->size().width())).toPoint();
}
static void dragCanvas(Canvas &canvas, QPointF from, QPointF to,
                       Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto start = canvasPoint(canvas, from), end = canvasPoint(canvas, to);
    QTest::mousePress(&canvas, Qt::LeftButton, modifiers, start);
    QMouseEvent move(QEvent::MouseMove, QPointF(end), QPointF(canvas.mapToGlobal(end)),
                     Qt::NoButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(&canvas, &move);
    QTest::mouseRelease(&canvas, Qt::LeftButton, modifiers, end);
}
class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void trayLightweightToggleRestoresWindowAndReflectsFailures() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto *light = app.menu->findChild<QAction *>("lightweightModeAction");
        auto *send = app.menu->findChild<QAction *>("sendDataAction");
        QVERIFY(light && send);
        QVERIFY(light->isCheckable());
        QVERIFY(send->isCheckable());
        QVERIFY(!light->isChecked());
        QVERIFY(!send->isChecked());
        QVERIFY(!send->isEnabled());
        light->trigger();
        QTRY_VERIFY(app.isLightweight());
        QVERIFY(light->isChecked());
        QVERIFY(light->isEnabled());
        QCOMPARE(app.menu->findChild<QAction *>("lightweightModeAction"), light);
        light->trigger();
        QTRY_VERIFY(!app.isLightweight());
        QVERIFY(app.window()->isVisible());
        QVERIFY(!light->isChecked());
        app.display().dataDir = dir.path() + "/missing/directory";
        light->trigger();
        QTRY_VERIFY(!light->isChecked());
        QVERIFY(!app.isLightweight());
        QVERIFY(app.window()->isVisible());
        app.display().dataDir = dir.path();
    }
    void traySendingToggleTracksWindowControlsAndDisconnects() {
#ifdef Q_OS_WIN
        QSKIP("Serial state integration uses a POSIX PTY; checkable light mode runs on both platforms");
#else
        TestProfile dir;
        PseudoScreen screen;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto *send = app.menu->findChild<QAction *>("sendDataAction");
        auto &runtime = app.display();
        runtime.sampling.stop();
        runtime.device.connectPort(screen.port, runtime.theme.orientation(), 75, false);
        QTRY_VERIFY(send->isEnabled());
        QVERIFY(!send->isChecked());
        send->trigger();
        QVERIFY(runtime.sending);
        QVERIFY(send->isChecked());
        app.window()->toggleSend();
        QVERIFY(!send->isChecked());
        app.window()->toggleSend();
        QVERIFY(send->isChecked());
        QVERIFY(app.enterLightweight());
        QVERIFY(send->isChecked());
        send->trigger();
        QVERIFY(!runtime.sending);
        QVERIFY(!send->isChecked());
        send->trigger();
        QVERIFY(runtime.sending);
        QVERIFY(send->isChecked());
        runtime.device.disconnectPort();
        QTRY_VERIFY(!send->isEnabled());
        QVERIFY(!send->isChecked()); // Works even when background sampling is paused.
#endif
    }
    void startupToggleReadsSystemStateAndRevertsOnFailure() {
        TestProfile dir;
        auto backend = std::make_shared<TestStartupBackend>();
        {
            MainWindow w(dir.path(), {}, nullptr, backend);
            w.show();
            QVERIFY(!w.startup->ready()); // No background polling before Settings is opened.
            w.setPage(2);
            QTRY_VERIFY(w.startup->ready() && !w.startup->busy());
            QVERIFY(w.startupToggle->isChecked());
            QVERIFY(w.startupToggle->isEnabled());
            QCOMPARE(backend->writes, 0);
            QTest::mouseClick(w.startupToggle, Qt::LeftButton, Qt::NoModifier, QPoint(8, w.startupToggle->height() / 2));
            QTRY_VERIFY(!w.startup->busy());
            QVERIFY(!w.startupToggle->isChecked());
            QCOMPARE(backend->writes, 1);
            backend->failure = "Permission denied for test entry";
            QTest::mouseClick(w.startupToggle, Qt::LeftButton, Qt::NoModifier, QPoint(8, w.startupToggle->height() / 2));
            QTRY_VERIFY(!w.startup->busy());
            QVERIFY(!w.startupToggle->isChecked());
            QVERIFY(w.startupStatus->text().contains("Permission denied"));
            backend->failure.clear();
        }
        MainWindow reopened(dir.path(), {}, nullptr, backend);
        reopened.setPage(2);
        QTRY_VERIFY(reopened.startup->ready() && !reopened.startup->busy());
        QVERIFY(!reopened.startupToggle->isChecked());
        backend->enabled = true; // Simulate an external change in OS startup settings.
        reopened.setPage(0);
        reopened.setPage(2);
        QTRY_VERIFY(!reopened.startup->busy());
        QVERIFY(reopened.startupToggle->isChecked());
    }
    void lightweightPreservesDocumentsAndDestroysWindow() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->sampling.stop();
        w->addComponent("CPU");
        w->choose(assets() + "/themes/nightfall.json");
        w->setEditing(true);
        w->addComponent("RAM");
        const auto current = w->theme.data, saved = w->saved;
        const auto undo = w->undoHistory;
        const auto selected = w->selection();
        const auto draft = w->drafts.value("__starter__").theme.data;
        QPointer<MainWindow> old = w;
        QPointer<Canvas> oldCanvas = w->canvas;
        auto *runtime = &app.display();
        for (int repeat = 0; repeat < 3; ++repeat) {
            QVERIFY(app.enterLightweight());
            QVERIFY(app.isLightweight());
            QVERIFY(!app.window());
            QVERIFY(old.isNull());
            QVERIFY(oldCanvas.isNull());
            QVERIFY(!runtime->preview);
            QCOMPARE(runtime->renderer.cachedBytes(), 0);
            const auto count = runtime->renderedFrames;
            runtime->tick(); // No connected display: no unnecessary rendering.
            QCOMPARE(runtime->renderedFrames, count);
            QVERIFY(app.suspended && QFileInfo::exists(app.suspended->fileName()));
            app.showWindow();
            w = app.window();
            QCOMPARE(&app.display(), runtime);
            QCOMPARE(w->theme.data, current);
            QCOMPARE(w->saved, saved);
            QCOMPARE(w->undoHistory, undo);
            QCOMPARE(w->selection(), selected);
            QCOMPARE(w->drafts.value("__starter__").theme.data, draft);
            QVERIFY(w->dirty);
            QVERIFY(w->editing);
            QVERIFY(!app.suspended);
            old = w; oldCanvas = w->canvas;
        }
        w->undo();
        QVERIFY(w->theme.data != current);
        w->redo();
        QCOMPARE(w->theme.data, current);
    }
    void lightweightCountdownStartsOnCloseAndReopenCancels() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        auto enabled = w->findChild<QCheckBox *>("autoLightweight");
        auto delay = w->findChild<QSpinBox *>("lightweightDelay");
        QVERIFY(enabled && delay);
        enabled->setChecked(true);
        delay->setValue(1);
        QVERIFY(!app.countdown.isActive());
        QTest::qWait(1100);
        QVERIFY(!app.isLightweight()); // An open idle window never starts the countdown.
        w->close();
        QVERIFY(!w->isVisible());
        QVERIFY(app.countdown.isActive());
        app.showWindow();
        QVERIFY(!app.countdown.isActive());
        QTest::qWait(1100);
        QVERIFY(!app.isLightweight());
        w->close();
        QTRY_VERIFY_WITH_TIMEOUT(app.isLightweight(), 2500);
        app.showWindow();
        QVERIFY(app.window()->config["auto_lightweight"].toBool());
        QCOMPARE(app.window()->config["lightweight_delay_seconds"].toInt(), 1);
        app.window()->findChild<QCheckBox *>("autoLightweight")->setChecked(false);
        app.window()->config["minimize_to_tray"] = true;
        app.window()->close();
        QVERIFY(!app.countdown.isActive());
        QTest::qWait(1100);
        QVERIFY(!app.isLightweight());
    }
    void lightweightFailureKeepsEditorAndUnavailableTrayIsSafe() {
        TestProfile dir;
        ApplicationController noTray(dir.path(), {}, [] { return false; });
        noTray.showWindow();
        QVERIFY(!noTray.enterLightweight());
        QVERIFY(noTray.window()->isVisible());
        TestProfile other;
        ApplicationController app(other.path(), {}, [] { return true; });
        app.showWindow();
        app.window()->addComponent("CPU");
        const auto before = app.window()->theme.data;
        app.display().dataDir = other.path() + "/not-a-directory/missing";
        QVERIFY(!app.enterLightweight());
        QVERIFY(app.window());
        QVERIFY(app.window()->isVisible());
        QCOMPARE(app.window()->theme.data, before);
        QVERIFY(app.window()->dirty);
        app.display().dataDir = other.path();
    }
    void lightweightKeepsSerialSessionAndTimerRunning() {
#ifdef Q_OS_WIN
        QSKIP("Native serial lifetime integration uses a POSIX PTY; UI lifecycle tests also run on Windows");
#else
        TestProfile dir;
        PseudoScreen screen;
        ApplicationController app(dir.path(), assets() + "/themes/nightfall.json", [] { return true; });
        app.showWindow();
        auto &runtime = app.display();
        runtime.device.connectPort(screen.port, runtime.theme.orientation(), 75, false);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().connected, 3000);
        runtime.sending = true;
        runtime.render();
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().frames > 0, 3000);
        const auto before = runtime.device.status().frames;
        const auto rendered = runtime.renderedFrames;
        QVERIFY(app.enterLightweight());
        QVERIFY(!app.window());
        QVERIFY(runtime.sampling.isActive());
        QVERIFY(runtime.sending);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().frames > before, 3500);
        QVERIFY(runtime.renderedFrames > rendered);
        QCOMPARE(runtime.frame.size(), QSize(480, 320));
        QVERIFY(runtime.device.status().connected);
        runtime.toggleSending();
        QVERIFY(!runtime.sending);
        const auto paused = runtime.renderedFrames;
        runtime.tick();
        QCOMPARE(runtime.renderedFrames, paused);
        QVERIFY(runtime.frame.isNull());
        runtime.toggleSending();
        QVERIFY(runtime.sending);
        QVERIFY(runtime.renderedFrames > paused);
        app.showWindow();
        QVERIFY(runtime.sending);
        QVERIFY(runtime.device.status().connected);
        const auto resumed = runtime.device.status().frames;
        QVERIFY(app.enterLightweight());
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().frames > resumed, 3500);
#endif
    }
    void deleteThemeConfirmationAndRecovery() {
        TestProfile dir;
        QString removed;
        QByteArray original;
        {
            MainWindow w(dir.path());
            w.show();
            QVERIFY(!w.deleteThemeButton->isEnabled());
            w.newTheme();
            removed = w.path;
            const auto directionKey = w.orientationKey();
            QFile file(removed);
            QVERIFY(file.open(QIODevice::ReadOnly));
            original = file.readAll();
            file.close();
            w.addComponent("CPU");
            w.directionBox->setCurrentIndex(3);
            w.leftPages->setCurrentIndex(0);
            const auto edited = w.theme.data;
            bool cancellationSeen = false, unsavedSeen = false;
            QTimer::singleShot(0, &w, [&] {
                auto dialog = w.findChild<QMessageBox *>("deleteThemeConfirmation");
                QVERIFY(dialog);
                cancellationSeen = true;
                unsavedSeen = dialog->text().contains("unsaved") || dialog->text().contains(QStringLiteral("未保存"));
                QTest::keyClick(dialog, Qt::Key_Escape);
            });
            QTest::mouseClick(w.deleteThemeButton, Qt::LeftButton);
            QVERIFY(cancellationSeen);
            QVERIFY(unsavedSeen);
            QCOMPARE(w.theme.data, edited);
            QCOMPARE(w.path, removed);
            QVERIFY(QFileInfo::exists(removed));
            bool deletionSeen = false;
            QTimer::singleShot(0, &w, [&] {
                auto dialog = w.findChild<QMessageBox *>("deleteThemeConfirmation");
                QVERIFY(dialog);
                for (const auto button : dialog->buttons())
                    if (dialog->buttonRole(button) == QMessageBox::DestructiveRole) {
                        deletionSeen = true;
                        button->click();
                        return;
                    }
                dialog->reject();
            });
            QTest::mouseClick(w.deleteThemeButton, Qt::LeftButton);
            QVERIFY(deletionSeen);
            QVERIFY(!QFileInfo::exists(removed));
            QVERIFY(w.path.isEmpty());
            QVERIFY(!w.dirty);
            QVERIFY(!w.sending);
            QVERIFY(w.undoHistory.isEmpty());
            QVERIFY(!w.drafts.contains(removed));
            QVERIFY(!w.config["theme_orientations"].toObject().contains(directionKey));
            QVERIFY(!w.deleteThemeButton->isEnabled());
            const auto backups = QDir(dir.path() + "/deleted-themes").entryList({"*.json"}, QDir::Files);
            QCOMPARE(backups.size(), 1);
            QFile backup(dir.path() + "/deleted-themes/" + backups.first());
            QVERIFY(backup.open(QIODevice::ReadOnly));
            QCOMPARE(backup.readAll(), original);
            w.undo();
            QVERIFY(w.path.isEmpty());
            QVERIFY(!QFileInfo::exists(removed));
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QVERIFY(std::none_of(reopened.entries.begin(), reopened.entries.end(),
                             [&](const MainWindow::Entry &e) { return e.path == removed; }));
        QVERIFY(reopened.path.isEmpty());
        reopened.quitting = true;
    }
    void deleteInactiveThemePreservesCurrentAndFilter() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.newTheme();
        const auto removed = w.path;
        w.addComponent("CPU");
        w.newTheme();
        const auto kept = w.path;
        const auto before = w.theme.data;
        QVERIFY(w.drafts.contains(removed));
        w.search->setText("no matching theme name");
        QString error;
        QVERIFY2(w.removeTheme(removed, error), qPrintable(error));
        QCOMPARE(w.path, kept);
        QCOMPARE(w.theme.data, before);
        QVERIFY(!w.drafts.contains(removed));
        QVERIFY(QFileInfo::exists(kept));
        for (int n = 0; n < w.library->count(); ++n)
            QVERIFY(w.library->item(n)->isHidden());
        w.quitting = true;
    }
    void deleteBuiltinPersistsAndRestores() {
        TestProfile dir;
        const auto file = assets() + "/themes/nightfall.json";
        QFile originalFile(file);
        QVERIFY(originalFile.open(QIODevice::ReadOnly));
        const auto original = originalFile.readAll();
        originalFile.close();
        {
            MainWindow w(dir.path());
            w.choose(file);
            QString error;
            QVERIFY2(w.removeTheme(file, error), qPrintable(error));
            QVERIFY(w.hiddenTheme(file));
            QVERIFY(QFileInfo::exists(file));
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QVERIFY(std::none_of(reopened.entries.begin(), reopened.entries.end(),
                             [&](const MainWindow::Entry &e) { return e.path == file; }));
        reopened.restoreBuiltinThemes();
        QVERIFY(!reopened.hiddenTheme(file));
        QVERIFY(std::any_of(reopened.entries.begin(), reopened.entries.end(),
                            [&](const MainWindow::Entry &e) { return e.path == file; }));
        QVERIFY(originalFile.open(QIODevice::ReadOnly));
        QCOMPARE(originalFile.readAll(), original);
        reopened.quitting = true;
    }
    void removeExternalThemePreservesSourceAndCanReimport() {
        TestProfile dir, source;
        const auto file = source.path() + "/external.json";
        Theme::starter().save(file);
        {
            MainWindow w(dir.path());
            w.choose(file);
            w.choose(assets() + "/themes/nightfall.json");
            w.quitting = true;
        }
        {
            MainWindow w(dir.path());
            QVERIFY(std::any_of(w.entries.begin(), w.entries.end(),
                                [&](const MainWindow::Entry &e) { return e.path == file; }));
            const auto active = w.path;
            QString error;
            QVERIFY2(w.removeTheme(file, error), qPrintable(error));
            QCOMPARE(w.path, active);
            QVERIFY(QFileInfo::exists(file));
            QVERIFY(w.config["external_themes"].toArray().isEmpty());
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QVERIFY(std::none_of(reopened.entries.begin(), reopened.entries.end(),
                             [&](const MainWindow::Entry &e) { return e.path == file; }));
        reopened.choose(file);
        QVERIFY(!reopened.hiddenTheme(file));
        QVERIFY(std::any_of(reopened.entries.begin(), reopened.entries.end(),
                            [&](const MainWindow::Entry &e) { return e.path == file; }));
        reopened.quitting = true;
    }
    void deleteThemeFailureKeepsDocumentAndFile() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.newTheme();
        const auto file = w.path;
        w.addComponent("CPU");
        const auto before = w.theme.data, config = w.config;
        const auto count = w.entries.size();
        QFile blocked(dir.path() + "/deleted-themes");
        QVERIFY(blocked.open(QIODevice::WriteOnly));
        blocked.write("not a directory");
        blocked.close();
        QString error;
        QVERIFY(!w.removeTheme(file, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(w.path, file);
        QCOMPARE(w.theme.data, before);
        QCOMPARE(w.config, config);
        QCOMPARE(w.entries.size(), count);
        QVERIFY(QFile::remove(blocked.fileName()));
        QVERIFY(QFile::remove(dir.path() + "/config.json"));
        QVERIFY(QDir().mkpath(dir.path() + "/config.json"));
        // A settings failure after moving the file restores it before returning.
        QVERIFY(!w.removeTheme(file, error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(file));
        QCOMPARE(w.path, file);
        QCOMPARE(w.theme.data, before);
        QCOMPARE(w.config, config);
        QCOMPARE(w.entries.size(), count);
        QVERIFY(QDir(dir.path() + "/deleted-themes").entryList(QDir::Files).isEmpty());
        w.dirty = false;
        w.quitting = true;
    }
    void groupCanvasDrag_data() {
        QTest::addColumn<bool>("mirrored");
        QTest::newRow("normal") << false;
        QTest::newRow("mirrored") << true;
    }
    void groupCanvasDrag() {
        QFETCH(bool, mirrored);
        auto theme = Theme::starter();
        auto leaf = newLayer("rectangle", "Leaf");
        leaf["x"] = 0;
        leaf["y"] = 0;
        leaf["w"] = 60;
        leaf["h"] = 40;
        const auto leafId = leaf["id"].toString();
        auto inner = newLayer("group", "Inner");
        inner["x"] = 12;
        inner["y"] = 12;
        inner["children"] = QJsonArray{leaf};
        const auto innerId = inner["id"].toString();
        auto group = newLayer("group", "Outer");
        group["x"] = 40;
        group["y"] = 50;
        group["children"] = QJsonArray{inner};
        const auto groupId = group["id"].toString();
        theme.data["layers"] = QJsonArray{group};
        Canvas canvas;
        canvas.theme = &theme;
        canvas.editing = true;
        canvas.snap = false;
        canvas.mirrored = mirrored;
        QImage frame(theme.size(), QImage::Format_RGB32);
        frame.fill(Qt::black);
        canvas.setFrame(frame);
        canvas.setZoom(1);
        canvas.resize(528, 368);
        canvas.show();
        QSignalSpy committed(&canvas, &Canvas::committed);
        dragCanvas(canvas, {70, 75}, {90, 85});
        QCOMPARE(canvas.selected, QStringList{groupId});
        QCOMPARE(absolutePosition(theme.layers(), groupId), QPointF(60, 60));
        QCOMPARE(findLayer(theme.layers(), leafId), leaf);
        QCOMPARE(committed.count(), 1);
        // A group selected in the tree remains selected when dragging a descendant.
        canvas.selected = {innerId};
        dragCanvas(canvas, {90, 85}, {100, 95});
        QCOMPARE(canvas.selected, QStringList{innerId});
        QCOMPARE(absolutePosition(theme.layers(), groupId), QPointF(60, 60));
        QCOMPARE(absolutePosition(theme.layers(), innerId), QPointF(82, 82));
        // Alt-click reaches the leaf; a locked ancestor still prevents movement.
        dragCanvas(canvas, {100, 95}, {108, 101}, Qt::AltModifier);
        QCOMPARE(canvas.selected, QStringList{leafId});
        QCOMPARE(findLayer(theme.layers(), leafId)["x"].toInt(), 8);
        auto layers = theme.layers();
        QVERIFY(updateLayer(layers, groupId, [](QJsonObject &o) { o["locked"] = true; }));
        theme.data["layers"] = layers;
        const auto locked = theme.data;
        dragCanvas(canvas, {108, 101}, {120, 115}, Qt::AltModifier);
        QCOMPARE(theme.data, locked);
        canvas.selected.clear();
        dragCanvas(canvas, {30, 40}, {180, 145});
        QCOMPARE(canvas.selected, QStringList{groupId});
    }
    void selectedGroupDragUndoRedo() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.show();
        w.setEditing(true);
        w.canvas->snap = false;
        w.selection({w.theme.layers()[0].toObject()["id"].toString()});
        const auto id = w.selection().first();
        const auto before = w.theme.data;
        const auto original = absolutePosition(w.theme.layers(), id);
        dragCanvas(*w.canvas, original + QPointF(30, 35), original + QPointF(50, 45));
        QCOMPARE(w.selection(), QStringList{id});
        QVERIFY(w.theme.data != before);
        const auto moved = w.theme.data;
        w.undo();
        QCOMPARE(w.theme.data, before);
        w.redo();
        QCOMPARE(w.theme.data, moved);
        w.dirty = false;
        w.quitting = true;
    }
    void orientationBelongsToEachTheme() {
        TestProfile dir;
        const auto first = dir.path() + "/first.json", second = dir.path() + "/second.json";
        auto a = Theme::starter(), b = Theme::starter();
        a.data["orientation"] = 3;
        b.data["orientation"] = 1;
        a.save(first);
        b.save(second);
        {
            MainWindow w(dir.path(), first);
            QVERIFY(w.directionBox->isEnabled()); // Does not require a connected display.
            QCOMPARE(w.directionBox->currentIndex(), 3);
            w.directionBox->setCurrentIndex(2);
            QVERIFY(!w.dirty);
            w.choose(second);
            QCOMPARE(w.directionBox->currentIndex(), 1);
            w.directionBox->setCurrentIndex(0);
            w.choose(first);
            QCOMPARE(w.directionBox->currentIndex(), 2);
            w.addComponent("CPU");
            w.directionBox->setCurrentIndex(3);
            w.undo();
            QCOMPARE(w.theme.orientation(), 3);
            QVERIFY(!w.dirty);
            const auto exported = dir.path() + "/export.json";
            w.theme.save(exported);
            QCOMPARE(Theme::load(exported).orientation(), 3);
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QCOMPARE(reopened.directionBox->currentIndex(), 3);
        reopened.choose(second);
        QCOMPARE(reopened.directionBox->currentIndex(), 0);
        reopened.choose(first);
        QCOMPARE(reopened.directionBox->currentIndex(), 3);
        reopened.choose(dir.path() + "/export.json");
        reopened.directionBox->setCurrentIndex(1);
        reopened.choose(first);
        QCOMPARE(reopened.directionBox->currentIndex(), 3);
        reopened.quitting = true;
    }
    void legacyOrientationMigration() {
        TestProfile dir;
        QFile config(dir.path() + "/config.json");
        QVERIFY(config.open(QIODevice::WriteOnly));
        config.write("{\"orientation\":3}");
        config.close();
        {
            MainWindow w(dir.path());
            QCOMPARE(w.theme.orientation(), 3);
            w.persist();
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QCOMPARE(reopened.theme.orientation(), 3);
        reopened.quitting = true;
    }
    void nightfallInLibraryAndIconEditing() {
        TestProfile dir;
        MainWindow w(dir.path());
        QCOMPARE(w.sampling.interval(), 1000);
        const auto file = assets() + "/themes/nightfall.json";
        QVERIFY(std::any_of(w.entries.begin(), w.entries.end(),
                            [&](const MainWindow::Entry &entry) { return entry.path == file; }));
        w.choose(file);
        QCOMPARE(w.theme.orientation(), 2);
        QCOMPARE(w.frame.size(), QSize(480, 320));
        w.setEditing(true);
        w.selection({"nightfall-cpu-chip"});
        w.fontBox->setValue(16);
        w.propertyEdited();
        QCOMPARE(findLayer(w.theme.layers(), "nightfall-cpu-chip")["font"].toObject()["family"].toString(),
                 QString("Turing Screen Symbols"));
        w.selection({"nightfall-cpu-model"});
        w.fontBox->setValue(14);
        w.propertyEdited();
        QCOMPARE(findLayer(w.theme.layers(), "nightfall-cpu-model")["font"].toObject()["family"].toString(),
                 QString("Turing Screen Latin"));
        w.selection({"nightfall-clock-time"});
        w.fontBox->setValue(28);
        w.propertyEdited();
        QCOMPARE(findLayer(w.theme.layers(), "nightfall-clock-time")["font"].toObject()["family"].toString(),
                 QString("Turing Screen Mono"));
        w.dirty = false;
        w.quitting = true;
    }
    void themeWeatherDefaultsRespectUserPreference() {
        TestProfile dir;
        MainWindow w(dir.path());
        QCOMPARE(w.theme.data, w.saved);
        w.choose(assets() + "/themes/nightfall.json");
        const auto checkbox = w.findChild<QCheckBox *>("weatherEnabled");
        QVERIFY(checkbox);
        QVERIFY(!checkbox->isChecked()); // Explicit user opt-out wins over the theme default.
        w.config.remove("weather_enabled");
        w.configureWeather();
        QVERIFY(checkbox->isChecked());
        // No collect/event loop runs while enabled in this offline UI test.
        w.choose({});
        QVERIFY(!checkbox->isChecked());
        w.quitting = true;
    }
    void editingWorkflow() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.show();
        QTest::qWait(100);
        w.setEditing(true);
        w.addComponent("CPU");
        auto id = w.selection().first();
        w.layerName->setText("Renamed CPU");
        w.propertyEdited();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("Renamed CPU"));
        w.undo();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("CPU"));
        w.redo();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("Renamed CPU"));
        w.selection({id});
        w.layerAction("duplicate");
        auto copy = w.selection().first();
        QVERIFY(copy != id);
        QVERIFY(findLayer(w.theme.layers(), copy)["kind"] == "group");
        w.selection({id, copy});
        w.layerAction("group");
        auto group = w.selection().first();
        QCOMPARE(findLayer(w.theme.layers(), group)["children"].toArray().size(), 2);
        auto at = absolutePosition(w.theme.layers(), copy);
        w.reparent({copy}, {}, -1);
        QCOMPARE(absolutePosition(w.theme.layers(), copy), at);
        w.selection({copy});
        w.lockedBox->setChecked(true);
        w.propertyEdited();
        auto before = w.theme.data;
        w.layerAction("delete");
        QCOMPARE(w.theme.data, before);
        w.lockedBox->setChecked(false);
        w.propertyEdited();
        w.layerAction("delete");
        QVERIFY(findLayer(w.theme.layers(), copy).isEmpty());
        w.undo();
        QVERIFY(!findLayer(w.theme.layers(), copy).isEmpty());
        const QString file = dir.path() + "/edited.json";
        w.theme.save(file);
        QCOMPARE(Theme::load(file).data, w.theme.data);
        w.dirty = false;
        w.quitting = true;
    }
    void nestedFoldersAndCycleProtection() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.setEditing(true);
        w.addComponent("CPU");
        auto a = w.selection().first();
        w.addComponent("RAM");
        auto b = w.selection().first();
        w.reparent({b}, a);
        QCOMPARE(parentOf(w.theme.layers(), b), a);
        auto before = w.theme.data;
        w.reparent({a}, b);
        QCOMPARE(w.theme.data, before);
        w.selection({a});
        w.layerAction("ungroup");
        QVERIFY(findLayer(w.theme.layers(), a).isEmpty());
        QVERIFY(!findLayer(w.theme.layers(), b).isEmpty());
        w.undo();
        QVERIFY(!findLayer(w.theme.layers(), a).isEmpty());
        w.dirty = false;
        w.quitting = true;
    }
    void pageIsolationPreferencesAndGeometry() {
        TestProfile dir;
        {
            MainWindow w(dir.path());
            w.show();
            QTest::qWait(50);
            w.setPage(2);
            QCOMPARE(w.pages->currentIndex(), 2);
            QVERIFY(!w.left->isVisible());
            w.resize(1100, 720);
            QTest::qWait(50);
            QCOMPARE(w.size(), QSize(1100, 720));
            w.setPage(0);
            QVERIFY(w.left->isVisible());
            w.config["minimize_to_tray"] = true;
            w.config["interval_ms"] = 3000;
            w.config["language"] = "english";
            w.config["palette"] = "forest";
            w.persist();
            w.dirty = false;
            w.quitting = true;
        }
        MainWindow copy(dir.path());
        QVERIFY(copy.config["minimize_to_tray"].toBool());
        QCOMPARE(copy.config["interval_ms"].toInt(), 3000);
        QVERIFY(copy.english);
        QCOMPARE(copy.paletteName, QString("forest"));
        copy.quitting = true;
    }
    void repeatedSamplingDoesNotMutateTheme() {
        TestProfile dir;
        MainWindow w(dir.path());
        auto before = w.theme.data;
        for (int i = 0; i < 3; ++i)
            w.tick();
        QCOMPARE(before, w.theme.data);
        QVERIFY(!w.sample.timestamp.isEmpty());
        w.quitting = true;
    }
    void memoryGbAndWeatherSettings() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.show();
        QTest::qWait(50);
        w.setPage(1);
        const auto all = metrics();
        for (const auto &key : {"RamValue", "RamTotal", "RamValid", "RamUsage"}) {
            int row = -1;
            for (int n = 0; n < all.size(); ++n)
                if (all[n].first == key)
                    row = n;
            QVERIFY(row >= 0);
            QVERIFY(w.sensorTable->item(row, 1));
            QCOMPARE(w.sensorTable->item(row, 1)->text(), w.sample.text(key));
            QVERIFY(w.sensorTable->item(row, 1)->text().endsWith("GB"));
            QVERIFY(!w.sensorTable->item(row, 2)->text().isEmpty());
        }
        w.setPage(2);
        auto city = w.findChild<QLineEdit *>("weatherCity");
        auto enabled = w.findChild<QCheckBox *>("weatherEnabled");
        QVERIFY(city);
        QVERIFY(enabled);
        QVERIFY(!enabled->isChecked());
        city->setText("Taipei, Taiwan");
        QVERIFY(QMetaObject::invokeMethod(city, "editingFinished"));
        enabled->setChecked(true);
        QFile config(dir.path() + "/config.json");
        QVERIFY(config.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(config.readAll()).object();
        QCOMPARE(saved["weather_city"].toString(), QString("Taipei, Taiwan"));
        QVERIFY(saved["weather_enabled"].toBool());
        enabled->setChecked(false); // This UI test never requests online weather.
        w.quitting = true;
    }
    void ungroupPreservesOfficialPixels() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.choose(assets() + "/official/config/微星.data");
        auto before = w.renderer.render(w.theme, w.sample, true);
        QStringList groups;
        for (auto value : w.theme.layers()) {
            auto layer = value.toObject();
            if (layer["kind"] == "group")
                groups.append(layer["id"].toString());
        }
        QVERIFY(!groups.isEmpty());
        w.selection(groups);
        w.layerAction("ungroup");
        QCOMPARE(w.renderer.render(w.theme, w.sample, true), before);
        w.dirty = false;
        w.quitting = true;
    }
};
} // namespace Screen
QTEST_MAIN(Screen::GuiTest)
#include "gui_test.moc"
