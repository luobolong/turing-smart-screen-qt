#include "mainwindow.h"
#include "application.h"
#include "uiappearance.h"
#include "pseudoscreen.h"
#include "weathernetwork.h"
#include <QJsonDocument>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QtTest>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Screen {
class TestProfile : public QTemporaryDir {
public:
    TestProfile() {
        QFile config(path() + "/config.json");
        const auto root = path() + "/themes";
        QDir().mkpath(root + "/custom"); QDir().mkpath(root + "/official"); QDir().mkpath(root + "/deleted");
        QFile::copy(assets() + "/themes/custom/nightfall.json", root + "/custom/nightfall.json");
        QFile::copy(assets() + "/themes/custom/system overview.json", root + "/custom/system overview.json");
        QFile::copy(assets() + "/themes/official/微星.data", root + "/official/微星.data");
        const auto offline = QJsonDocument(QJsonObject{{"weather_enabled", false}, {"theme_library_root", root},
            {"theme_library_seeded", true}, {"theme_library_version", 1}}).toJson();
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
    void narrowDevicePanelKeepsControlsInsideViewport() {
        TestProfile dir;
        MainWindow window(dir.path());
        window.show(); window.sampling.stop();
        window.hide(); window.show();
        const auto shots = qEnvironmentVariable("TURING_WEATHER_UI_SHOTS");
        if (!shots.isEmpty()) window.selectUiTheme("catppuccin-macchiato");
        auto scroll = window.findChild<QScrollArea *>("deviceScroll");
        QVERIFY(scroll);
        auto font = window.font();
        for (int size : {9, 12}) {
            font.setPointSize(size); window.setFont(font);
            for (int width : {200, 220, 260}) {
                window.devicePanel->setFixedWidth(width);
                QCoreApplication::processEvents();
                QTest::qWait(20);
                QVERIFY(scroll->widget()->width() <= scroll->viewport()->width());
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                for (QWidget *widget : QList<QWidget *>{window.portBox, window.directionBox,
                        window.brightness, window.connectButton, window.sendButton,
                        window.findChild<QToolButton *>("refreshDevicePorts")}) {
                    const auto left = widget->mapTo(scroll->viewport(), QPoint()).x();
                    QVERIFY2(left >= 0 && left + widget->width() <= scroll->viewport()->width(), qPrintable(widget->objectName()));
                }
                if (!shots.isEmpty() && size == 9 && width == 220) {
                    QDir().mkpath(shots);
                    QVERIFY(window.grab().save(shots + "/device-narrow.png"));
                }
            }
        }
        window.quitting = true;
    }
    void manualWeatherSettingsShowsErrorsSuccessAndRespectsCollectionPause() {
        TestProfile dir;
        WeatherNetwork network;
        auto runtime = std::make_shared<DisplayRuntime>(dir.path(), QString(), &network);
        MainWindow window(runtime);
        window.show(); window.hide(); window.show();
        window.sampling.stop(); window.setPage(MainWindow::SettingsPage);
        auto enabled = window.findChild<QCheckBox *>("weatherEnabled");
        auto city = window.findChild<QLineEdit *>("weatherCity");
        auto fetch = window.findChild<QPushButton *>("fetchWeatherNow");
        auto status = window.findChild<QLabel *>("weatherStatus");
        QVERIFY(enabled && city && fetch && status);
        QVERIFY(!fetch->isEnabled());
        enabled->setChecked(true);
        city->setText("Test City");
        QVERIFY(QMetaObject::invokeMethod(city, "editingFinished"));
        QVERIFY(network.requests.isEmpty()); // Opening settings does not poll the weather.
        QVERIFY(fetch->isEnabled());
        const auto timestamp = window.sample.timestamp;
        network.responses = {"network-timeout"};
        fetch->click();
        QVERIFY(!fetch->isEnabled());
        QTRY_COMPARE(status->property("statusTone").toString(), QString("error"));
        QVERIFY(fetch->isEnabled());
        QVERIFY(status->accessibleName().contains("timed out"));
        const auto shots = qEnvironmentVariable("TURING_WEATHER_UI_SHOTS");
        const auto capture = [&](const QString &name) {
            if (shots.isEmpty()) return;
            window.selectUiTheme("catppuccin-macchiato");
            for (auto parent = fetch->parentWidget(); parent; parent = parent->parentWidget())
                if (auto scroll = qobject_cast<QScrollArea *>(parent)) { scroll->ensureWidgetVisible(fetch); break; }
            QCoreApplication::processEvents();
            QDir().mkpath(shots); QVERIFY(window.grab().save(shots + '/' + name + ".png"));
        };
        capture("weather-error");
        network.responses = {R"({"results":[{"name":"Test City","latitude":25,"longitude":121}]})",
                             R"({"current":{"temperature_2m":23.5,"weather_code":61}})"};
        fetch->click();
        QTRY_COMPARE(status->property("statusTone").toString(), QString("success"));
        QVERIFY(status->accessibleName().contains("23.5"));
        QCOMPARE(window.sample.values.value("WeatherTemp"), 23.5);
        QCOMPARE(window.sample.timestamp, timestamp); // No unrelated sensor collection.
        QCOMPARE(network.requests.size(), 3);
        capture("weather-success");
        runtime->setCollectionEnabled(false);
        QVERIFY(!fetch->isEnabled());
        const auto requests = network.requests.size();
        QVERIFY(!runtime->requestWeather());
        QCOMPARE(network.requests.size(), requests);
        QVERIFY(status->accessibleName().contains("暂停") || status->accessibleName().contains("paused"));
        runtime->setCollectionEnabled(true);
        network.responses = {R"({"results":[{"name":"Cancelled","latitude":25,"longitude":121}]})"};
        fetch->click(); enabled->setChecked(false);
        QCoreApplication::processEvents();
        QVERIFY(!runtime->collector.weatherStatus().enabled);
        QVERIFY(!fetch->isEnabled());
        QCOMPARE(network.requests.size(), requests + 1); // Canceled geocoding cannot start a forecast.
        window.quitting = true;
    }
    void zoomDeviceAndThemeControlsFollowPreviewAndEditorLayout() {
        TestProfile dir;
        MainWindow window(dir.path());
        window.show();
        window.sampling.stop();
        QTest::qWait(80);
        // A hidden process launch can consume Windows' first ShowWindow call.
        window.hide(); window.show(); window.activateWindow();
        QTest::qWait(80);
        for (const auto &prefix : {QString("preview"), QString("editor")}) {
            window.setPage(prefix == "preview" ? MainWindow::LibraryPage : MainWindow::EditorPage);
            QCoreApplication::processEvents();
            auto target = prefix == "preview" ? window.previewCanvas : window.canvas;
            auto controls = window.findChild<QWidget *>(prefix + "ZoomControls");
            auto slider = controls->findChild<QSlider *>(prefix + "ZoomSlider");
            auto value = controls->findChild<QDoubleSpinBox *>(prefix + "ZoomValue");
            auto minus = controls->findChild<QToolButton *>(prefix + "ZoomOut");
            auto plus = controls->findChild<QToolButton *>(prefix + "ZoomIn");
            QVERIFY(controls && slider && value && minus && plus);
            slider->setValue(150);
            QCOMPARE(target->zoom(), 1.5); QCOMPARE(value->value(), 150.);
            value->setValue(123.45);
            QCOMPARE(value->suffix(), QString("%"));
            QVERIFY(std::abs(target->zoom() - 1.2345) < .00001);
            plus->click(); QVERIFY(target->zoom() > 1.2345);
            QVERIFY(std::abs(value->value() - target->zoom() * 100.) < .006);
            minus->click(); QVERIFY(std::abs(target->zoom() - 1.2345) < .00001);
            controls->findChild<QToolButton *>(prefix + "Fit")->click();
            QVERIFY(std::abs(value->value() - target->zoom() * 100.) < .006);
            controls->findChild<QToolButton *>(prefix + "ActualSize")->click();
            QCOMPARE(value->value(), 100.);
            QCoreApplication::processEvents();
            QVERIFY(minus->geometry().left() > value->geometry().right());
            QVERIFY(plus->geometry().left() > minus->geometry().right());
            QVERIFY(controls->mapTo(&window, QPoint()).y() >= target->mapTo(&window, target->rect().bottomLeft()).y());
        }
        window.setPage(MainWindow::LibraryPage);
        auto refresh = window.findChild<QToolButton *>("refreshDevicePorts");
        QVERIFY(refresh && !refresh->icon().isNull() && refresh->text().isEmpty());
        QVERIFY(refresh->mapTo(&window, QPoint()).x() > window.portBox->mapTo(&window, window.portBox->rect().topRight()).x());
        QVERIFY(window.connectButton->mapTo(&window, QPoint()).y() < window.sendButton->mapTo(&window, QPoint()).y());
        auto menu = window.findChild<QMenu *>("themeOptionsMenu");
        auto types = window.findChild<QMenu *>("themeTypeMenu");
        QVERIFY(menu && types);
        QCOMPARE(types->actions().size(), 2);
        for (auto action : menu->actions()) {
            QVERIFY(!action->text().contains("text background", Qt::CaseInsensitive));
            QVERIFY(!action->text().contains("回收站"));
            QVERIFY(!action->text().contains("recycle", Qt::CaseInsensitive));
        }
        QVERIFY(QMetaObject::invokeMethod(types, "aboutToShow"));
        QCOMPARE(types->actions()[0]->isChecked(), window.theme.data["landscape"].toBool(true));
        types->actions()[1]->trigger();
        QVERIFY(!window.theme.data["landscape"].toBool(true));
        QVERIFY(QMetaObject::invokeMethod(types, "aboutToShow"));
        QVERIFY(types->actions()[1]->isChecked() && !types->actions()[0]->isChecked());
        QCOMPARE(window.directionBox->style()->styleHint(QStyle::SH_ComboBox_Popup, nullptr, window.directionBox), 0);
        window.activateWindow();
        QTest::qWait(80);
        window.directionBox->showPopup();
        QCoreApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(window.directionBox->view()->isVisible(), 1000);
        const auto bottom = window.directionBox->mapToGlobal(window.directionBox->rect().bottomLeft()).y();
        QVERIFY(window.directionBox->view()->window()->geometry().top() >= bottom);
        window.directionBox->hidePopup();
        const auto shots = qEnvironmentVariable("TURING_CPU_UI_SHOTS");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            window.selectUiTheme("catppuccin-macchiato");
            types->actions()[0]->trigger();
            window.tick();
            window.findChild<QToolButton *>("previewFit")->click();
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(shots + "/preview.png"));
            window.setPage(MainWindow::EditorPage);
            window.findChild<QToolButton *>("editorFit")->click();
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(shots + "/editor.png"));
            window.setPage(MainWindow::SettingsPage);
            QVERIFY(window.grab().save(shots + "/settings.png"));
        }
        window.dirty = false; window.quitting = true;
    }
    void zoomPercentagesRemainVisibleAcrossPalettesAndFontSizes() {
        TestProfile dir;
        MainWindow window(dir.path());
        window.show();
        window.sampling.stop();
        for (const auto &palette : {QString("catppuccin-latte"), QString("catppuccin-macchiato")}) {
            window.selectUiTheme(palette);
            for (int size : {9, 12}) {
                for (const auto &prefix : {QString("preview"), QString("editor")}) {
                    window.setPage(prefix == "preview" ? MainWindow::LibraryPage : MainWindow::EditorPage);
                    auto value = window.findChild<QDoubleSpinBox *>(prefix + "ZoomValue");
                    QVERIFY(value && value->isVisible());
                    const auto edit = value->findChild<QLineEdit *>();
                    QVERIFY(edit);
                    auto font = value->font(); font.setPointSize(size); value->setFont(font);
                    auto controls = window.findChild<QWidget *>(prefix + "ZoomControls");
                    const auto capture = [&] {
                        const auto frame = controls->grab();
                        const auto scale = frame.devicePixelRatioF();
                        return frame.toImage().copy(QRect(value->pos() * scale, value->size() * scale));
                    };
                    value->clearFocus();
                    value->setValue(100.);
                    edit->deselect();
                    QCoreApplication::processEvents();
                    const auto first = capture();
                    value->setValue(200.);
                    edit->deselect();
                    QCoreApplication::processEvents();
                    const auto second = capture();
                    const auto shots = qEnvironmentVariable("TURING_ZOOM_UI_SHOTS");
                    if (!shots.isEmpty()) {
                        QDir().mkpath(shots);
                        QVERIFY(controls->grab().save(
                            shots + '/' + palette + '-' + QString::number(size) + '-' + prefix + ".png"));
                    }
                    QVERIFY2(edit->width() >= edit->fontMetrics().horizontalAdvance(value->text()),
                        qPrintable(palette + '/' + prefix + ": the percentage editor must fit its text"));
                    QVERIFY2(first != second, qPrintable(palette + '/' + prefix + " font=" + QString::number(size)
                        + ": changing the percentage must change the painted text"));
                    value->selectAll();
                    QTest::keyClicks(value, "175.50");
                    QTest::keyClick(value, Qt::Key_Return);
                    QCOMPARE(value->value(), 175.5);
                    const auto target = prefix == "preview" ? window.previewCanvas : window.canvas;
                    QVERIFY(std::abs(target->zoom() - 1.755) < .00001);
                }
            }
        }
        window.quitting = true;
    }
    void collectionToggleSyncsPersistsAndReleasesLightweightCaches() {
        TestProfile dir;
        {
            ApplicationController app(dir.path(), {}, [] { return true; });
            auto &runtime = app.display();
            auto settings = app.window()->findChild<QCheckBox *>("collectData");
            auto device = app.window()->findChild<QCheckBox *>("deviceCollectData");
            QVERIFY(settings && device && app.collectAction);
            QVERIFY(settings->isChecked() && device->isChecked() && app.collectAction->isChecked());
            QVERIFY(app.menu->actions().indexOf(app.collectAction) < app.menu->actions().indexOf(app.portsMenu->menuAction()));
            QVERIFY(runtime.sampling.isActive());
            QVERIFY(runtime.renderer.staticCachedBytes() > 0);
            const auto sample = runtime.sample.json();
            const auto frame = runtime.frame;
            settings->setChecked(false);
            QVERIFY(!runtime.sampling.isActive());
            QVERIFY(!app.collectAction->isChecked() && !device->isChecked());
            runtime.tick();
            QTest::qWait(1100);
            QCOMPARE(runtime.sample.json(), sample);
            QCOMPARE(runtime.frame, frame);
            QVERIFY(app.enterLightweight());
            QCOMPARE(runtime.renderer.staticCachedBytes(), 0);
            runtime.render();
            QCOMPARE(runtime.renderer.staticCachedBytes(), 0);
            app.showWindow();
            QVERIFY(!app.window()->findChild<QCheckBox *>("collectData")->isChecked());
            QVERIFY(!runtime.sampling.isActive());
            app.collectAction->trigger();
            QVERIFY(runtime.sampling.isActive());
            QVERIFY(app.window()->findChild<QCheckBox *>("collectData")->isChecked());
            QVERIFY(runtime.sample.json() != sample);
            app.collectAction->trigger();
        }
        ApplicationController reopened(dir.path(), {}, [] { return true; });
        QVERIFY(!reopened.display().collectionEnabled());
        QVERIFY(!reopened.display().sampling.isActive());
        QVERIFY(reopened.display().sample.values.isEmpty());
        QVERIFY(!reopened.collectAction->isChecked());
    }
    void runtimeRequestsVisibleThemeBindingsAndDiagnosticsOnlyOnSensorPage() {
        TestProfile dir;
        MainWindow window(dir.path());
        auto hidden = component("GPU"); hidden["visible"] = false;
        auto fixed = newLayer("text", "Fixed", "Volume"); fixed["text"] = "Static";
        auto sensor = newLayer("sensor", "Clock", "Time");
        window.theme.data["layers"] = QJsonArray{hidden, fixed, sensor};
        QCOMPARE(window.runtime->requestedMetrics(), QSet<QString>{"Time"});
        window.tick();
        QCOMPARE(window.sample.strings.keys(), QStringList{"Time"});
        QVERIFY(window.sample.values.isEmpty());
        window.show();
        window.setPage(MainWindow::SensorsPage);
        QVERIFY(window.runtime->requestedMetrics().contains("RamLoad"));
        QVERIFY(window.sample.values.contains("RamLoad"));
        window.hide();
        QCOMPARE(window.runtime->requestedMetrics(), QSet<QString>{"Time"});
        window.quitting = true;
    }
#ifdef Q_OS_WIN
    void windowsSensorSourcePersistsAndAppliesImmediately() {
        TestProfile dir;
        {
            MainWindow w(dir.path());
            w.sampling.stop();
            w.setPage(MainWindow::SettingsPage);
            auto source = w.findChild<QComboBox *>("windowsSensorSource");
            auto gpu = w.findChild<QSpinBox *>("windowsGpuIndex");
            auto port = w.findChild<QSpinBox *>("lhmPort");
            auto hint = w.findChild<QLabel *>("windowsSensorHint");
            QVERIFY(source && gpu && port && hint);
            QCOMPARE(source->count(), 4);
            source->setCurrentIndex(source->findData("hwinfo"));
            QVERIFY(hint->text().contains("12"));
            QCOMPARE(w.config["windows_sensor_source"].toString(), QString("hwinfo"));
            QVERIFY(w.sample.sources.value("CpuFreq") != "System API");
            QVERIFY(port->isHidden());
            source->setCurrentIndex(source->findData("aida64"));
            QVERIFY(hint->text().contains("AIDA64"));
            source->setCurrentIndex(source->findData("librehardwaremonitor"));
            gpu->setValue(2);
            port->setValue(18085);
            QVERIFY(!port->isHidden());
            QCOMPARE(w.config["windows_gpu_index"].toInt(), 1);
            QCOMPARE(w.config["lhm_port"].toInt(), 18085);
        }
        MainWindow reopened(dir.path());
        reopened.sampling.stop();
        QCOMPARE(reopened.findChild<QComboBox *>("windowsSensorSource")->currentData().toString(), QString("librehardwaremonitor"));
        QCOMPARE(reopened.findChild<QSpinBox *>("windowsGpuIndex")->value(), 2);
        QCOMPARE(reopened.findChild<QSpinBox *>("lhmPort")->value(), 18085);
        auto source = reopened.findChild<QComboBox *>("windowsSensorSource");
        source->setCurrentIndex(source->findData("system"));
        QVERIFY(!reopened.sample.values.contains("CpuFreq"));
        QVERIFY(!reopened.sample.values.contains("GpuTemp"));
    }
#endif
    void sidebarNavigationSeparatesLibraryAndEditor() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.show();
        QTRY_VERIFY(w.library->isVisible());
        QVERIFY(w.previewCanvas->isVisible());
        QVERIFY(!w.canvas->isVisible());
        QVERIFY(!w.editing);
        QVERIFY(!w.sidebarExpanded);
        QCOMPARE(w.sidebar->width(), 60);
        QVERIFY(w.titleBar->width() < w.root->width());
        QCOMPARE(w.sidebar->mapTo(w.root, QPoint()).y(), 0);
        QVERIFY(w.sidebar->isAncestorOf(w.sidebarToggle));
        QVERIFY(!w.sidebarToggle->icon().isNull());
        QVERIFY(w.sidebarToggle->text().isEmpty());
        QVERIFY(w.sidebarToggle->mapTo(w.sidebar, QPoint()).y() > w.findChild<QToolButton *>("sidebarLightweight")->mapTo(w.sidebar, QPoint()).y());
        QCOMPARE(w.sidebarBrand->objectName(), QString("sidebarBrand"));
        QVERIFY(!w.saveButton->isVisible());
        QVERIFY(!w.findChild<QLabel *>("applicationTitle"));
        QVERIFY(!w.findChild<QToolButton *>("sidebarNew"));
        QCOMPARE(w.navigation->count(), 4);
        QCOMPARE(w.library->columnCount(), 1);
        QVERIFY(w.library->isHeaderHidden());
        QCOMPARE(w.librarySplit->count(), 3);
        QVERIFY(w.devicePanel->isVisible());
        QVERIFY(w.devicePanel->isAncestorOf(w.portBox));
        QVERIFY(w.devicePanel->isAncestorOf(w.sendButton));
        QVERIFY(w.portBox->isVisible());
        QVERIFY(w.devicePanel->mapTo(&w, QPoint()).x() > w.previewCanvas->mapTo(&w, QPoint()).x());
        auto custom = w.library->topLevelItem(1);
        QCOMPARE(custom->text(0), QString("custom"));
        custom->setExpanded(true);
        const auto firstRow = w.library->visualItemRect(custom->child(0));
        const auto secondRow = w.library->visualItemRect(custom->child(1));
        QCOMPARE(firstRow.left(), secondRow.left());
        QVERIFY(secondRow.top() > firstRow.bottom());
        w.mirrorBox->setChecked(true);
        QVERIFY(w.previewCanvas->mirrored);
        QVERIFY(w.canvas->mirrored);
        w.mirrorBox->setChecked(false);
        QCOMPARE(w.navigation->currentRow(), int(MainWindow::LibraryPage));
        w.choose(dir.path() + "/themes/custom/nightfall.json");
        const auto original = w.theme.data;
        w.modeButton->click();
        QTRY_VERIFY(w.canvas->isVisible());
        QVERIFY(!w.library->isVisible());
        QVERIFY(!w.devicePanel->isVisible());
        QVERIFY(w.editing);
        QVERIFY(w.saveButton->isVisible());
        QCOMPARE(w.navigation->currentRow(), int(MainWindow::EditorPage));
        w.addComponent("RAM");
        const auto edited = w.theme.data;
        const auto selected = w.selection();
        QVERIFY(edited != original);
        const auto libraryItem = w.navigation->item(MainWindow::LibraryPage);
        QTest::mouseClick(w.navigation->viewport(), Qt::LeftButton, Qt::NoModifier,
                         w.navigation->visualItemRect(libraryItem).center());
        QTRY_VERIFY(w.library->isVisible());
        QVERIFY(!w.canvas->isVisible());
        QVERIFY(!w.editing);
        QCOMPARE(w.theme.data, edited);
        QCOMPARE(w.selection(), selected);
        for (const auto action : w.findChildren<QAction *>("shortcut"))
            if (action->shortcut() == QKeySequence(QKeySequence::Undo))
                action->trigger();
        QCOMPARE(w.theme.data, edited); // The preview does not accept editing shortcuts.
        w.modeButton->click();
        QCOMPARE(w.page, int(MainWindow::EditorPage));
        QCOMPARE(w.theme.data, edited);
        w.undo();
        QCOMPARE(w.theme.data, original);
        w.redo();
        QCOMPARE(w.theme.data, edited);
        w.selection(selected);
        w.search->setText("Nightfall");
        w.inspectorTabs->setCurrentIndex(1);
        w.english = !w.english;
        w.build();
        QCOMPARE(w.pages->currentIndex(), int(MainWindow::EditorPage));
        QCOMPARE(w.search->text(), QString("Nightfall"));
        QCOMPARE(w.selection(), selected);
        QCOMPARE(w.inspectorTabs->currentIndex(), 1);
        QCOMPARE(w.theme.data, edited);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Horizontal);
        w.modeButton->click();
        QCOMPARE(w.page, int(MainWindow::LibraryPage));
        QVERIFY(w.previewCanvas->isVisible());
        QCOMPARE(w.theme.data, edited);
        w.sidebarToggle->click();
        QVERIFY(w.sidebarExpanded);
        w.build();
        QVERIFY(w.sidebarExpanded);
        w.quitting = true;
    }
    void sidebarIconsStayAlignedAndPageHeadersHaveEqualHeight() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.sampling.stop();
        w.selectUiTheme("catppuccin-mocha");
        w.show();
        for (bool english : {false, true}) {
            w.english = english;
            w.build();
            for (int width : {1280, 760}) {
                w.resize(width, 820);
                w.setSidebarExpanded(false);
                w.setPage(MainWindow::LibraryPage);
                QCoreApplication::processEvents();
                auto light = w.findChild<QToolButton *>("sidebarLightweight");
                QCOMPARE(w.sidebarBrand->height(), 64);
                QCOMPARE(light->height(), 44);
                QCOMPARE(w.sidebarToggle->height(), 36);
                const auto iconImages = [&] {
                    QList<QImage> images;
                    for (auto b : {w.sidebarBrand, light}) {
                        const auto y = b->mapTo(w.sidebar, QPoint(0, b->height() / 2)).y();
                        images.append(w.sidebar->grab(QRect(14, y - 16, 32, 32)).toImage());
                    }
                    for (int n = 0; n < w.navigation->count(); ++n) {
                        const auto row = w.navigation->visualItemRect(w.navigation->item(n));
                        const auto y = w.navigation->viewport()->mapTo(w.sidebar, row.center()).y();
                        images.append(w.sidebar->grab(QRect(19, y - 11, 22, 22)).toImage());
                    }
                    return images;
                };
                const auto collapsed = iconImages();
                const auto logoPosition = w.sidebarBrand->mapTo(w.sidebar, QPoint());
                const auto togglePosition = w.sidebarToggle->mapTo(w.sidebar, QPoint());
                const auto expandIcon = w.sidebarToggle->icon().pixmap(22, 22).toImage();
                w.sidebarBrand->click();
                QTRY_VERIFY(w.sidebarExpanded);
                QCoreApplication::processEvents();
                QCOMPARE(w.sidebar->width(), 208);
                QCOMPARE(iconImages(), collapsed);
                QCOMPARE(w.sidebarBrand->mapTo(w.sidebar, QPoint()), logoPosition);
                QCOMPARE(w.sidebarToggle->mapTo(w.sidebar, QPoint()), togglePosition);
                QVERIFY(w.sidebarToggle->text().isEmpty());
                QVERIFY(w.sidebarToggle->icon().pixmap(22, 22).toImage() != expandIcon);
                w.sidebarToggle->click();
                QTRY_VERIFY(!w.sidebarExpanded);
                QCoreApplication::processEvents();
                QCOMPARE(iconImages(), collapsed);
                int titleCenter = -1;
                for (int page = 0; page < MainWindow::PageCount; ++page) {
                    w.setPage(page);
                    QCoreApplication::processEvents();
                    QCOMPARE(w.titleBar->height(), 64);
                    const auto center = w.pageTitle->mapTo(w.titleBar, w.pageTitle->rect().center()).y();
                    if (titleCenter < 0) titleCenter = center;
                    QCOMPARE(center, titleCenter);
                    QCOMPARE(w.titleBar->y(), 0);
                    const auto shots = qEnvironmentVariable("TURING_PANEL_SHOTS");
                    if (!shots.isEmpty() && width == 1280) {
                        QDir().mkpath(shots);
                        const auto name = QString("/sidebar-%1-page-%2").arg(english ? "en" : "zh").arg(page);
                        QVERIFY(w.grab().save(shots + name + "-collapsed.png"));
                        w.sidebarBrand->click();
                        QCoreApplication::processEvents();
                        QVERIFY(w.grab().save(shots + name + "-expanded.png"));
                        w.sidebarToggle->click();
                        QCoreApplication::processEvents();
                    }
                }
                QCOMPARE(w.findChild<QToolButton *>("libraryExpandSelected")->toolTip(),
                         english ? QString("Expand selected folder") : QString::fromUtf8("展开所选文件夹"));
                QCOMPARE(w.findChild<QToolButton *>("libraryImport")->toolTip(),
                         english ? QString("Import theme") : QString::fromUtf8("导入主题"));
            }
        }
        w.quitting = true;
    }
    void compactLayoutReflowsAndSidebarRemainsUsable() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.config["sidebar_expanded"] = true;
        w.setSidebarExpanded(true);
        w.show();
        QTRY_COMPARE(w.librarySplit->orientation(), Qt::Horizontal);
        QVERIFY(w.sidebarExpanded);
        w.setPage(MainWindow::SettingsPage);
        w.resize(760, 680);
        QTRY_COMPARE(w.size(), QSize(760, 680));
        w.setPage(MainWindow::LibraryPage);
        QTRY_COMPARE(w.librarySplit->orientation(), Qt::Vertical);
        QCOMPARE(w.library->columnCount(), 1);
        auto custom = w.library->topLevelItem(1);
        custom->setExpanded(true);
        QCoreApplication::processEvents();
        const auto first = w.library->visualItemRect(custom->child(0));
        const auto second = w.library->visualItemRect(custom->child(1));
        QCOMPARE(first.left(), second.left());
        QVERIFY(second.top() > first.bottom());
        for (int n = 0; n < w.library->topLevelItemCount(); ++n) QVERIFY(w.library->topLevelItem(n)->text(0) != "themes");
        auto separator = w.librarySplit->handle(1)->findChild<QFrame *>("paneSeparator");
        QVERIFY(separator && separator->isVisible());
        QCOMPARE(separator->frameShape(), QFrame::HLine);
        QVERIFY(!w.sidebarExpanded);
        QCOMPARE(w.sidebar->width(), 60);
        QVERIFY(w.navigation->item(MainWindow::SettingsPage)->text().isEmpty());
        QVERIFY(!w.navigation->item(MainWindow::SettingsPage)->toolTip().isEmpty());
        w.setEditing(true);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Vertical);
        QVERIFY(w.canvas->isVisible());
        QVERIFY(w.inspectorTabs->isVisible());
        w.sidebarToggle->click();
        QVERIFY(w.sidebarExpanded);
        QCOMPARE(w.sidebar->width(), 208);
        QVERIFY(!w.navigation->item(MainWindow::SettingsPage)->text().isEmpty());
        QCOMPARE(w.page, int(MainWindow::EditorPage));
        w.sidebarToggle->click();
        QVERIFY(!w.sidebarExpanded);
        w.resize(640, 520);
        QTRY_COMPARE(w.size(), QSize(640, 520));
        for (int page = 0; page < MainWindow::PageCount; ++page) {
            w.setPage(page);
            QCoreApplication::processEvents();
            QCOMPARE(w.pages->currentIndex(), page);
            QCOMPARE(w.navigation->currentRow(), page);
            QVERIFY(w.pageTitle->isVisible());
            QVERIFY(w.pages->width() <= w.width() - w.sidebar->width());
            QVERIFY(w.pages->height() > 300);
        }
        w.resize(1280, 820);
        w.setPage(MainWindow::LibraryPage);
        QTRY_COMPARE(w.librarySplit->orientation(), Qt::Horizontal);
        QCOMPARE(w.library->columnCount(), 1);
        QTRY_COMPARE(separator->frameShape(), QFrame::VLine);
        w.setEditing(true);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Horizontal);
        w.setSidebarExpanded(true);
        w.resize(1020, 760);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Horizontal);
        w.sidebarToggle->click();
        QVERIFY(!w.sidebarExpanded);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Horizontal);
        w.quitting = true;
    }
    void lightweightRestoresNavigationAndSidebarPreference() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->setPage(MainWindow::SensorsPage);
        w->setSidebarExpanded(true);
        w->sidebarToggle->click();
        QVERIFY(!w->sidebarExpanded);
        QVERIFY(app.enterLightweight());
        app.showWindow();
        w = app.window();
        QCOMPARE(w->page, int(MainWindow::SensorsPage));
        QCOMPARE(w->navigation->currentRow(), int(MainWindow::SensorsPage));
        QVERIFY(!w->sidebarExpanded);
        QVERIFY(w->sensorTable->isVisible());
        QVERIFY(!w->portBox->isVisible());
        QVERIFY(!w->canvas->isVisible());
        QVERIFY(!app.display().preview);
    }
    void autostartSkipsEditorAndRestoresFromTray() {
        TestProfile dir;
        const auto themePath = dir.path() + "/themes/custom/nightfall.json";
        {
            QFile config(dir.path() + "/config.json");
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write(QJsonDocument(QJsonObject{
                {"weather_enabled", false}, {"selected_theme", themePath},
                {"auto_lightweight", false},
                {"theme_orientations", QJsonObject{{themeOrientationKey(themePath), 3}}}}).toJson());
        }
        ApplicationController app(dir.path(), {}, [] { return true; }, true);
        QVERIFY(app.isLightweight());
        QVERIFY(!app.window());
        QVERIFY(!app.suspended);
        QVERIFY(!app.countdown.isActive());
        QVERIFY(app.lightAction->isChecked());
        QVERIFY(app.tray->isVisible());
        auto &runtime = app.display();
        QCOMPARE(runtime.path, themePath);
        QCOMPARE(runtime.theme.orientation(), 3);
        QVERIFY(runtime.sampling.isActive());
        QVERIFY(!runtime.preview);
        QVERIFY(runtime.frame.isNull());
        QCOMPARE(runtime.renderedFrames, quint64(0));
        QVERIFY(QDir(dir.path()).entryList({"lightweight-session-*"}, QDir::Files).isEmpty());
        const auto theme = runtime.theme.data;
        app.showAction->trigger();
        QVERIFY(!app.isLightweight());
        QVERIFY(app.window());
        QVERIFY(app.window()->isVisible());
        QVERIFY(!app.lightAction->isChecked());
        QCOMPARE(runtime.theme.data, theme);
        QCOMPARE(runtime.theme.orientation(), 3);
        QVERIFY(!runtime.frame.isNull());
        QVERIFY(!runtime.sample.timestamp.isEmpty());
        QVERIFY(!app.window()->dirty);
        app.lightAction->trigger();
        QTRY_VERIFY(app.isLightweight());
        QVERIFY(!app.window());
    }
    void autostartConnectsSavedPortAndSendsInLightweight() {
#ifndef Q_OS_WIN
        TestProfile dir;
        PseudoScreen screen;
        const auto themePath = dir.path() + "/themes/custom/nightfall.json";
        {
            QFile config(dir.path() + "/config.json");
            QVERIFY(config.open(QIODevice::WriteOnly));
            config.write(QJsonDocument(QJsonObject{
                {"weather_enabled", false}, {"selected_theme", themePath},
                {"port", screen.port}, {"interval_ms", 1000}}).toJson());
        }
        ApplicationController app(dir.path(), {}, [] { return false; }, true);
        QVERIFY(app.isLightweight());
        QVERIFY(!app.window());
        auto &runtime = app.display();
        QVERIFY(runtime.sending);
        QCOMPARE(runtime.path, themePath);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().connected, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(runtime.device.status().frames > 0, 3000);
        QVERIFY(runtime.renderedFrames > 0);
        QVERIFY(!runtime.frame.isNull());
        QVERIFY(app.sendAction->isChecked());
        QVERIFY(app.sendAction->isEnabled());
        QVERIFY(app.lightAction->isChecked());
        QVERIFY(!app.window()); // Connecting and sending never create the editor.
        QTRY_VERIFY(!screen.bytes().isEmpty());
#else
        QSKIP("Automatic serial connection uses a POSIX PTY");
#endif
    }
    void autostartWithoutTrayStaysHeadless() {
        TestProfile dir;
        bool available = false;
        ApplicationController app(dir.path(), {}, [&] { return available; }, true);
        QVERIFY(app.isLightweight());
        QVERIFY(!app.window());
        QVERIFY(app.lightAction->isChecked());
        QVERIFY(!app.lightAction->isEnabled());
        QVERIFY(app.trayRetry.isActive());
        available = true;
        QTRY_VERIFY(!app.trayRetry.isActive());
        QVERIFY(app.lightAction->isEnabled());
        QVERIFY(!app.window());
    }
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
            w.setPage(MainWindow::SettingsPage);
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
        reopened.setPage(MainWindow::SettingsPage);
        QTRY_VERIFY(reopened.startup->ready() && !reopened.startup->busy());
        QVERIFY(!reopened.startupToggle->isChecked());
        backend->enabled = true; // Simulate an external change in OS startup settings.
        reopened.setPage(MainWindow::LibraryPage);
        reopened.setPage(MainWindow::SettingsPage);
        QTRY_VERIFY(!reopened.startup->busy());
        QVERIFY(reopened.startupToggle->isChecked());
    }
    void lightweightPreservesDocumentsAndDestroysWindow() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->sampling.stop();
        const auto defaultPath = w->path;
        w->addComponent("CPU");
        w->choose(dir.path() + "/themes/custom/nightfall.json");
        w->setEditing(true);
        w->addComponent("RAM");
        const auto current = w->theme.data, saved = w->saved;
        const auto undo = w->undoHistory;
        const auto selected = w->selection();
        const auto draft = w->drafts.value(defaultPath).theme.data;
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
            QCOMPARE(w->drafts.value(defaultPath).theme.data, draft);
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
    void lightweightCountdownSettingsAreConditionalAndKeepZero() {
        TestProfile dir;
        MainWindow w(dir.path(), {}, nullptr, std::make_shared<TestStartupBackend>());
        w.setPage(MainWindow::SettingsPage);
        w.show();
        auto enabled = w.findChild<QCheckBox *>("autoLightweight");
        auto delay = w.findChild<QSpinBox *>("lightweightDelay");
        auto label = w.findChild<QLabel *>("lightweightDelayLabel");
        QVERIFY(enabled && delay && label);
        QVERIFY(!enabled->isChecked());
        QVERIFY(!delay->isVisible());
        QVERIFY(!label->isVisible());
        enabled->setChecked(true);
        QVERIFY(delay->isVisible());
        QVERIFY(label->isVisible());
        QCOMPARE(delay->minimum(), 0);
        delay->setValue(0);
        enabled->setChecked(false);
        QVERIFY(!delay->isVisible());
        QVERIFY(!label->isVisible());
        QCOMPARE(w.config["lightweight_delay_seconds"].toInt(-1), 0);
        QTRY_VERIFY(w.startup->ready() && !w.startup->busy());
        w.english = true;
        w.build();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        enabled = w.findChild<QCheckBox *>("autoLightweight");
        delay = w.findChild<QSpinBox *>("lightweightDelay");
        label = w.findChild<QLabel *>("lightweightDelayLabel");
        QTRY_VERIFY(enabled->isVisible());
        QCOMPARE(delay->value(), 0);
        QVERIFY(!delay->isVisible());
        enabled->setChecked(true);
        QVERIFY(delay->isVisible());
        QVERIFY(label->isVisible());
        for (auto text : w.findChildren<QLabel *>()) {
            QVERIFY(!text->text().startsWith(QString::fromUtf8("原生界面")));
            QVERIFY(!text->text().startsWith("Native interface"));
        }
        w.quitting = true;
    }
    void zeroCountdownEntersAfterCloseAndReopeningCancelsIt() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->findChild<QCheckBox *>("autoLightweight")->setChecked(true);
        w->findChild<QSpinBox *>("lightweightDelay")->setValue(0);
        QVERIFY(!app.countdown.isActive());
        QPointer<MainWindow> old = w;
        w->close();
        QVERIFY(old); // Do not destroy a window while its closeEvent is still on the stack.
        QVERIFY(app.countdown.isActive());
        QCOMPARE(app.countdown.interval(), 0);
        QTRY_VERIFY(app.isLightweight());
        QVERIFY(old.isNull());
        QVERIFY(!app.window());
        app.showWindow();
        QCOMPARE(app.window()->findChild<QSpinBox *>("lightweightDelay")->value(), 0);
        QVERIFY(app.window()->findChild<QCheckBox *>("autoLightweight")->isChecked());
        app.window()->close();
        app.showWindow();
        QCoreApplication::processEvents();
        QVERIFY(!app.isLightweight());
        QVERIFY(app.window()->isVisible());
        QVERIFY(!app.countdown.isActive());
        app.window()->close();
        QTRY_VERIFY(app.isLightweight());
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
        ApplicationController app(dir.path(), dir.path() + "/themes/custom/nightfall.json", [] { return true; });
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
            QTimer errorDialogs;
            connect(&errorDialogs, &QTimer::timeout, &w, [&] {
                auto dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (dialog && dialog->objectName() != "deleteThemeConfirmation") {
                    qWarning() << dialog->windowTitle() << dialog->text(); dialog->reject();
                }
            });
            errorDialogs.start(100);
            w.show();
            QVERIFY(w.deleteThemeAction->isEnabled());
            w.newTheme();
            removed = w.path;
            const auto directionKey = w.orientationKey();
            QFile file(removed);
            QVERIFY(file.open(QIODevice::ReadOnly));
            original = file.readAll();
            file.close();
            w.addComponent("CPU");
            w.directionBox->setCurrentIndex(3);
            w.setPage(MainWindow::LibraryPage);
            const auto edited = w.theme.data;
            bool cancellationSeen = false, unsavedSeen = false;
            QTimer::singleShot(0, &w, [&] {
                auto dialog = w.findChild<QMessageBox *>("deleteThemeConfirmation");
                QVERIFY(dialog);
                cancellationSeen = true;
                unsavedSeen = dialog->text().contains("unsaved") || dialog->text().contains(QStringLiteral("未保存"));
                QTest::keyClick(dialog, Qt::Key_Escape);
            });
            w.deleteTheme(w.path);
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
            w.deleteTheme(w.path);
            QVERIFY(deletionSeen);
            QVERIFY(!QFileInfo::exists(removed));
            QCOMPARE(w.path, w.runtime->library.defaultThemeFile());
            QVERIFY(!w.dirty);
            QVERIFY(!w.sending);
            QVERIFY(w.undoHistory.isEmpty());
            QVERIFY(!w.drafts.contains(removed));
            QVERIFY(!w.config["theme_orientations"].toObject().contains(directionKey));
            QVERIFY(w.deleteThemeAction->isEnabled());
            const auto backups = QDir(dir.path() + "/themes/deleted").entryList({"*.json"}, QDir::Files);
            QCOMPARE(backups.size(), 1);
            QFile backup(dir.path() + "/themes/deleted/" + backups.first());
            QVERIFY(backup.open(QIODevice::ReadOnly));
            QCOMPARE(backup.readAll(), original);
            w.undo();
            QCOMPARE(w.path, w.runtime->library.defaultThemeFile());
            QVERIFY(!QFileInfo::exists(removed));
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        QVERIFY(std::none_of(reopened.entries.begin(), reopened.entries.end(),
                             [&](const MainWindow::Entry &e) { return e.path == removed; }));
        QCOMPARE(reopened.path, reopened.runtime->library.defaultThemeFile());
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
        w.searchLibrary();
        QString error;
        QVERIFY2(w.removeTheme(removed, error), qPrintable(error));
        QCOMPARE(w.path, kept);
        QCOMPARE(w.theme.data, before);
        QVERIFY(!w.drafts.contains(removed));
        QVERIFY(QFileInfo::exists(kept));
        for (auto item : w.libraryItems) QVERIFY(item->isHidden());
        w.quitting = true;
    }
    void recycleThemePersistsAndRestores() {
        TestProfile dir;
        const auto file = dir.path() + "/themes/custom/nightfall.json";
        QFile originalFile(file); QVERIFY(originalFile.open(QIODevice::ReadOnly));
        const auto original = originalFile.readAll(); originalFile.close();
        {
            MainWindow w(dir.path()); w.choose(file);
            QString error; QVERIFY2(w.removeTheme(file, error), qPrintable(error));
            QVERIFY(!QFileInfo::exists(file));
            QCOMPARE(w.path, w.runtime->library.defaultThemeFile()); w.quitting = true;
        }
        MainWindow reopened(dir.path());
        const auto backups = QDir(dir.path() + "/themes/deleted").entryList({"*.json"}, QDir::Files);
        QCOMPARE(backups.size(), 1);
        reopened.restoreBuiltinThemes();
        QCOMPARE(reopened.libraryFolder, dir.path() + "/themes/deleted");
        reopened.restoreLibraryItem(dir.path() + "/themes/deleted/" + backups.first());
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(originalFile.open(QIODevice::ReadOnly)); QCOMPARE(originalFile.readAll(), original);
        reopened.quitting = true;
    }
    void removeExternalThemePreservesSourceAndCanReimport() {
        TestProfile dir, source;
        const auto file = source.path() + "/external.json";
        Theme::starter().save(file);
        {
            MainWindow w(dir.path());
            w.choose(file);
            w.choose(dir.path() + "/themes/custom/nightfall.json");
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
        QVERIFY(QDir().rmdir(dir.path() + "/themes/deleted"));
        QFile blocked(dir.path() + "/themes/deleted");
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
        QVERIFY(QDir().mkpath(dir.path() + "/themes/deleted"));
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
        QVERIFY(QDir(dir.path() + "/themes/deleted").entryList(QDir::Files).isEmpty());
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
        const auto file = dir.path() + "/themes/custom/nightfall.json";
        QVERIFY(std::any_of(w.entries.begin(), w.entries.end(),
                            [&](const MainWindow::Entry &entry) { return entry.path == file; }));
        w.choose(file);
        QCOMPARE(w.theme.orientation(), 3);
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
        w.choose(dir.path() + "/themes/custom/nightfall.json");
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
    void layerVisibilityIconsPreserveEditingAndLightweightLabels() {
        TestProfile dir;
        const auto restore = qScopeGuard([&] { UiAppearance::instance().apply("system", dir.path()); });
        MainWindow w(dir.path());
        const auto close = qScopeGuard([&] { w.dirty = false; w.quitting = true; });
        w.sampling.stop();
        w.setEditing(true);
        w.addComponent("CPU");
        const auto group = w.selection().first();
        const auto child = findLayer(w.theme.layers(), group)["children"].toArray().first().toObject()["id"].toString();
        w.show();
        QTest::qWait(50);
        const auto itemFor = [&](const QString &id) -> QTreeWidgetItem * {
            QTreeWidgetItemIterator it(w.tree);
            while (*it) {
                if ((*it)->data(0, Qt::UserRole).toString() == id) return *it;
                ++it;
            }
            return nullptr;
        };
        const auto clickIndicator = [&](const QString &id, int column) {
            auto item = itemFor(id);
            if (!item) return false;
            w.tree->scrollToItem(item);
            QStyleOptionViewItem option;
            option.initFrom(w.tree);
            option.widget = w.tree;
            option.rect = w.tree->visualRect(w.tree->indexFromItem(item, column));
            option.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay;
            option.text = item->text(column);
            const auto rect = w.tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, w.tree);
            if (rect.isEmpty()) return false;
            QTest::mouseClick(w.tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
            return true;
        };
        QVERIFY(clickIndicator(child, 0));
        QVERIFY(!findLayer(w.theme.layers(), child)["visible"].toBool(true));
        QCOMPARE(itemFor(child)->checkState(0), Qt::Unchecked);
        w.undo();
        QVERIFY(findLayer(w.theme.layers(), child)["visible"].toBool(true));
        w.redo();
        QVERIFY(!findLayer(w.theme.layers(), child)["visible"].toBool(true));
        w.undo();
        QVERIFY(clickIndicator(group, 0));
        QVERIFY(!findLayer(w.theme.layers(), group)["visible"].toBool(true));
        w.undo();
        QVERIFY(clickIndicator(group, 1));
        QVERIFY(findLayer(w.theme.layers(), group)["locked"].toBool());
        const auto locked = w.theme.data;
        QVERIFY(clickIndicator(child, 0));
        QCOMPARE(w.theme.data, locked); // A locked ancestor still blocks visibility changes.
        QCOMPARE(itemFor(child)->checkState(0), Qt::Checked);
        QVERIFY(clickIndicator(group, 1));
        QVERIFY(!findLayer(w.theme.layers(), group)["locked"].toBool());
        w.tree->setCurrentItem(itemFor(child), 0);
        QTest::keyClick(w.tree, Qt::Key_Space);
        QVERIFY(!findLayer(w.theme.layers(), child)["visible"].toBool(true));
        const auto edited = w.theme.data;
        const auto shots = qEnvironmentVariable("TURING_LAYER_UI_SHOTS");
        for (const bool english : {false, true}) {
            w.english = english;
            w.build();
            w.setEditing(true);
            QSignalSpy request(&w, &MainWindow::lightweightRequested);
            for (const bool expanded : {false, true}) {
                w.setSidebarExpanded(expanded);
                auto light = w.findChild<QToolButton *>("sidebarLightweight");
                QVERIFY(light && !light->icon().isNull());
                QCOMPARE(light->toolButtonStyle(), expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
                QCOMPARE(light->text(), english ? QString("Lightweight Mode") : QString::fromUtf8("轻量模式"));
                light->click();
                QCOMPARE(request.count(), expanded ? 2 : 1);
                if (!shots.isEmpty()) {
                    QDir().mkpath(shots);
                    w.selectUiTheme("catppuccin-frappe");
                    QCoreApplication::processEvents();
                    QVERIFY(w.grab().save(shots + (english ? "/english" : "/chinese")
                        + (expanded ? "-expanded.png" : "-collapsed.png")));
                }
            }
            QCOMPARE(w.theme.data, edited);
        }
    }
    void bundledUiPalettesSwitchAndSurviveLightweightMode() {
        TestProfile dir;
        const auto restore = qScopeGuard([&] { UiAppearance::instance().apply("system", dir.path()); });
        ApplicationController app(dir.path(), dir.path() + "/themes/custom/nightfall.json", [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->sampling.stop();
        w->setPage(MainWindow::SettingsPage);
        const auto frame = w->frame;
        const auto displayTheme = w->theme.data;
        const auto palettes = uiThemes(dir.path());
        QVERIFY(palettes.size() >= 12);
        QCOMPARE(w->uiThemeBox->count(), palettes.size() + 1);
        const auto frappe = w->uiThemeBox->itemText(w->uiThemeBox->findData("catppuccin-frappe"));
        QVERIFY(frappe.contains(QString::fromUtf8("Catppuccin Frapp\xc3\xa9")));
        const auto luminance = [](const QColor &color) {
            const auto channel = [](double c) { return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
            return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) + .0722 * channel(color.blueF());
        };
        const auto contrast = [&](const QColor &a, const QColor &b) {
            const auto x = luminance(a), y = luminance(b);
            return (qMax(x, y) + .05) / (qMin(x, y) + .05);
        };
        QSet<QString> modes;
        for (const auto &palette : palettes) {
            QVERIFY2(palette.error.isEmpty(), qPrintable(palette.error));
            QVERIFY(QFileInfo::exists(palette.file));
            modes.insert(palette.mode);
            const int row = w->uiThemeBox->findData(palette.id);
            QVERIFY(row >= 0);
            w->uiThemeBox->setCurrentIndex(row);
            QCoreApplication::processEvents();
            QCOMPARE(w->config["ui_theme"].toString(), palette.id);
            QCOMPARE(QApplication::palette().color(QPalette::Window), palette.colors["window"]);
            QCOMPARE(QApplication::palette().color(QPalette::Text), palette.colors["text"]);
            QCOMPARE(w->sidebar->palette().color(QPalette::Window), palette.colors["base"]);
            QCOMPARE(w->sensorTable->palette().color(QPalette::Base), palette.colors["base"]);
            QCOMPARE(app.menu->palette().color(QPalette::Text), UiAppearance::instance().systemMenuPalette().color(QPalette::Text));
            QTextDocument links;
            links.setHtml(w->findChild<QLabel *>("weatherCredit")->text());
            const auto link = links.find("Open-Meteo");
            QVERIFY(!link.isNull());
            QCOMPARE(link.charFormat().foreground().color(), palette.colors["link"]);
            QCOMPARE(w->findChild<QFrame *>("sidebarSeparator")->palette().color(QPalette::Mid), palette.colors["border"]);
            for (auto background : {"window", "base", "button"})
                QVERIFY2(contrast(palette.colors["text"], palette.colors[background]) >= 4.5, qPrintable(palette.id + '/' + background));
            QVERIFY2(contrast(palette.colors["accent_text"], palette.colors["accent"]) >= 4.5, qPrintable(palette.id + "/selection"));
            QCOMPARE(w->theme.data, displayTheme);
            QCOMPARE(w->frame, frame);
            const auto shots = qEnvironmentVariable("TURING_UI_PALETTE_SHOTS");
            if (!shots.isEmpty()) {
                QDir().mkpath(shots);
                QVERIFY(w->grab().save(shots + '/' + palette.id + ".png"));
                if (palette.id == "catppuccin-mocha") {
                    w->setPage(MainWindow::LibraryPage);
                    QCoreApplication::processEvents();
                    QVERIFY(w->grab().save(shots + "/mocha-library.png"));
                    w->setPage(MainWindow::SettingsPage);
                    w->resize(760, 680);
                    QCoreApplication::processEvents();
                    QVERIFY(w->grab().save(shots + "/mocha-compact.png"));
                    w->resize(1280, 820);
                }
            }
        }
        QCOMPARE(modes, (QSet<QString>{"light", "dark"}));
        w->uiThemeBox->setCurrentIndex(w->uiThemeBox->findData("catppuccin-mocha"));
        const auto palette = QApplication::palette();
        QVERIFY(app.enterLightweight());
        QVERIFY(!app.window());
        QTRY_COMPARE(app.menu->palette().color(QPalette::Window), UiAppearance::instance().systemMenuPalette().color(QPalette::Window));
        app.showWindow();
        app.window()->sampling.stop();
        QCOMPARE(app.window()->uiThemeBox->currentData().toString(), QString("catppuccin-mocha"));
        QCOMPARE(QApplication::palette(), palette);
    }
    void folderLibraryPreservesDraftsAndShowsExplorerActions() {
        TestProfile dir;
        MainWindow w(dir.path());
        const auto close = qScopeGuard([&] { w.dirty = false; w.quitting = true; });
        QString unexpectedDialog;
        QTimer popupWatcher;
        connect(&popupWatcher, &QTimer::timeout, &w, [&] {
            if (auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                unexpectedDialog = box->text(); qWarning() << unexpectedDialog; box->reject();
            }
        });
        popupWatcher.start(100);
        w.sampling.stop(); w.show();
        w.newTheme(); const auto first = w.path;
        w.addComponent("CPU"); const auto content = w.theme.layers();
        w.newTheme(); QVERIFY(w.drafts.contains(first));
        QString error;
        QVERIFY2(w.renameLibraryPath(first, "Renamed", error), qPrintable(error));
        const auto renamed = dir.path() + "/themes/custom/Renamed.json";
        QVERIFY(w.drafts.contains(renamed)); QVERIFY(!QFileInfo::exists(first));
        w.choose(renamed); QCOMPARE(w.theme.layers(), content); QVERIFY(w.dirty);
        QCOMPARE(w.title->text(), QString("Renamed *"));
        QCOMPARE(w.editorThemeTitle->text(), w.title->text());
        QVERIFY(w.findChild<QToolButton *>("renamePreviewTheme"));
        QVERIFY(w.findChild<QToolButton *>("renameEditorTheme"));
        QVERIFY2(w.renameLibraryPath(w.path, "Active renamed", error), qPrintable(error));
        QCOMPARE(w.title->text(), QString("Active renamed *"));
        QCOMPARE(w.path, dir.path() + "/themes/custom/Active renamed.json");
        const auto folder = w.runtime->library.createFolder(w.runtime->library.root + "/custom", "Nested");
        w.transferLibraryItem(w.path, folder, false);
        QCOMPARE(w.path, folder + "/Active renamed.json"); QCOMPARE(w.theme.layers(), content); QVERIFY(w.dirty);
        w.setPage(MainWindow::LibraryPage); w.selectLibraryFolder(folder); w.buildFolderTree();
        QCOMPARE(w.library, w.folderTree);
        QCOMPARE(w.library->currentItem()->data(0, Qt::UserRole).toString(), folder);
        QCOMPARE(w.library->currentItem()->childCount(), 1);
        QVERIFY(w.library->currentItem()->child(0)->text(0).startsWith("Active renamed"));
        bool inspected = false;
        QTimer::singleShot(0, &w, [&] {
            auto menu = w.findChild<QMenu *>("themeLibraryMenu"); QVERIFY(menu);
            const auto closeMenu = qScopeGuard([&] { menu->close(); });
            for (auto action : {"libraryRename", "libraryCopy", "libraryCut", "libraryDelete", "libraryPurge"})
                QVERIFY(menu->findChild<QAction *>(action));
            inspected = true; menu->close();
        });
        w.libraryMenu(w.path, QPoint(30, 30)); QVERIFY(inspected);
        QVERIFY2(w.removeTheme(folder, error), qPrintable(error));
        QCOMPARE(w.path, w.runtime->library.defaultThemeFile()); QVERIFY(!QFileInfo::exists(folder));
        const auto recycled = w.runtime->library.root + "/deleted/Nested";
        w.restoreLibraryItem(recycled); QVERIFY(QFileInfo::exists(folder + "/Active renamed.json"));
        QVERIFY2(w.removeTheme(folder, error, true), qPrintable(error));
        QVERIFY(!QFileInfo::exists(folder));
        QVERIFY(!w.findChild<QLabel *>("diskPartitionHint"));
        for (auto button : w.statusBar()->findChildren<QPushButton *>()) QVERIFY(button->text() != "Exit" && button->text() != QString::fromUtf8("退出"));
        QVERIFY(w.dataState->mapTo(&w, QPoint()).x() > w.width() / 2);
        QVERIFY2(unexpectedDialog.isEmpty(), qPrintable(unexpectedDialog));
    }
    void trayMenuKeepsSystemStyleAcrossPalettes() {
        TestProfile dir;
        const auto restore = qScopeGuard([&] { UiAppearance::instance().apply("system", dir.path()); });
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        const auto system = UiAppearance::instance().systemMenuPalette();
        const auto actions = app.menu->actions();
        QCOMPARE(actions.size(), 9);
        QCOMPARE(actions[0], app.showAction);
        QVERIFY(actions[1]->isSeparator());
        QCOMPARE(actions[2], app.collectAction);
        QCOMPARE(actions[3]->menu(), app.portsMenu);
        QCOMPARE(actions[4], app.sendAction);
        QVERIFY(actions[5]->isSeparator());
        QCOMPARE(actions[6], app.lightAction);
        QVERIFY(actions[7]->isSeparator());
        QCOMPARE(actions[8], app.quitAction);
        for (const auto &id : {QString("catppuccin-mocha"), QString("catppuccin-latte"), QString("dracula")}) {
            app.window()->selectUiTheme(id);
            QCOMPARE(app.menu->palette().color(QPalette::Window), system.color(QPalette::Window));
            QCOMPARE(app.menu->palette().color(QPalette::Text), system.color(QPalette::Text));
            QVERIFY(app.menu->property("nativeMenuStyle").toBool());
            QVERIFY(!app.tray->icon().isNull());
        }
        const auto shots = qEnvironmentVariable("TURING_UI_PALETTE_SHOTS");
        app.menu->popup(QPoint(10, 10));
        app.menu->setActiveAction(app.lightAction);
        QCoreApplication::processEvents();
        const auto rendered = app.menu->grab().toImage();
        QCOMPARE(rendered.pixelColor(10, 5), system.color(QPalette::Window));
        const auto hovered = app.menu->actionGeometry(app.lightAction);
        QCOMPARE(rendered.pixelColor(hovered.right() - 18, hovered.center().y()), system.color(QPalette::Highlight));
        QVERIFY(app.menu->width() >= 174);
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            QVERIFY(rendered.save(shots + "/system-tray-menu.png"));
        }
        app.menu->hide();
    }
    void libraryToolbarSearchAndTreeActions() {
        TestProfile dir;
        QDir().mkpath(dir.path() + "/themes/official/Category/Nested");
        MainWindow w(dir.path());
        w.sampling.stop(); w.show();
        w.choose(dir.path() + "/themes/custom/nightfall.json");
        const auto original = w.theme.data;
        QVERIFY(w.librarySearchPanel->isHidden());
        QVERIFY(!w.findChild<QPushButton *>("deleteThemeButton"));
        auto toolbar = w.findChild<QWidget *>("libraryTools"); QVERIFY(toolbar);
        QCOMPARE(toolbar->findChildren<QToolButton *>().size(), 7);
        QVERIFY(w.findChild<QToolButton *>("libraryImport"));
        w.findChild<QToolButton *>("libraryToggleSearch")->click();
        QVERIFY(w.librarySearchPanel->isVisible());
        w.search->setText("No matching theme");
        QCOMPARE(w.libraryQuery, QString("No matching theme"));
        QVERIFY(w.libraryEmpty->isVisible());
        w.search->setText("Nightfall");
        QCOMPARE(w.libraryQuery, QString("Nightfall"));
        QVERIFY(!w.findChild<QToolButton *>("librarySearchSubmit"));
        QVERIFY(!w.libraryEmpty->isVisible());
        auto newMenu = w.findChild<QMenu *>("libraryNewMenu"); QVERIFY(newMenu);
        QCOMPARE(newMenu->actions().size(), 2);
        QVERIFY(newMenu->actions()[0]->text().contains("theme", Qt::CaseInsensitive) || newMenu->actions()[0]->text().contains(QString::fromUtf8("主题")));
        w.search->clear();
        w.findChild<QToolButton *>("libraryCollapseAll")->click();
        for (int n = 0; n < w.library->topLevelItemCount(); ++n) QVERIFY(!w.library->topLevelItem(n)->isExpanded());
        auto official = w.library->topLevelItem(0);
        w.library->setCurrentItem(official);
        w.findChild<QToolButton *>("libraryExpandSelected")->click();
        QVERIFY(official->isExpanded());
        QVERIFY(official->child(0)->isExpanded());
        w.findChild<QToolButton *>("libraryLocate")->click();
        QVERIFY(w.libraryQuery.isEmpty());
        QCOMPARE(w.library->currentItem()->data(0, Qt::UserRole).toString(), w.path);
        QVERIFY(w.library->currentItem()->parent()->isExpanded());
        QCOMPARE(w.theme.data, original);
        const auto external = dir.path() + "/Imported theme.json";
        Theme::starter().save(external);
        w.selectLibraryFolder(dir.path() + "/themes/custom");
        const bool nativeDialogsDisabled = qApp->testAttribute(Qt::AA_DontUseNativeDialogs);
        qApp->setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restoreDialogs = qScopeGuard([&] { qApp->setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled); });
        bool importDialogSeen = false;
        QTimer::singleShot(0, &w, [&] {
            auto dialog = w.findChild<QFileDialog *>();
            QVERIFY(dialog);
            importDialogSeen = true;
            dialog->selectFile(external);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        w.findChild<QToolButton *>("libraryImport")->click();
        QVERIFY(importDialogSeen);
        QCOMPARE(w.path, dir.path() + "/themes/custom/Imported theme.json");
        QVERIFY(QFileInfo::exists(w.path));
        QVERIFY(QFileInfo::exists(external));
        w.findChild<QToolButton *>("libraryHide")->click();
        QVERIFY(w.librarySplit->paneCollapsed(0));
        QCOMPARE(w.librarySplit->sizes()[0], 0);
        w.selectOpenedTheme();
        QVERIFY(!w.librarySplit->paneCollapsed(0));
        QVERIFY(w.librarySplit->sizes()[0] > 0);
        const auto shots = qEnvironmentVariable("TURING_PANEL_SHOTS");
        if (!shots.isEmpty()) { QDir().mkpath(shots); QVERIFY(w.grab().save(shots + "/toolbar-search.png")); }
        w.quitting = true;
    }
    void overviewHasAFileAndFolderOpenKeepsThumbnails() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.sampling.stop(); w.show();
        const auto file = dir.path() + "/themes/custom/system overview.json";
        QCOMPARE(w.path, file);
        QCOMPARE(w.config["selected_theme"].toString(), file);
        QVERIFY(QFileInfo::exists(w.path));
        for (auto item : w.libraryItems) {
            const auto path = item->data(0, Qt::UserRole).toString();
            QVERIFY(!path.isEmpty());
            QCOMPARE(item->text(0).section('\n', 0, 0), QFileInfo(path).fileName());
        }
        auto folder = w.library->topLevelItem(1);
        folder->setExpanded(true);
        QTRY_VERIFY(!folder->child(0)->icon(0).isNull());
        QTRY_VERIFY(!folder->child(1)->icon(0).isNull());
        const auto icon0 = folder->child(0)->icon(0).cacheKey(), icon1 = folder->child(1)->icon(0).cacheKey();
        const auto selected = w.path;
        QTimer::singleShot(0, &w, [&] {
            auto menu = w.findChild<QMenu *>("themeLibraryMenu"); QVERIFY(menu);
            const auto close = qScopeGuard([&] { menu->close(); });
            auto open = menu->findChild<QAction *>("libraryOpen"); QVERIFY(open); open->trigger();
        });
        w.libraryMenu(dir.path() + "/themes/custom", QPoint(20, 20));
        QCOMPARE(w.path, selected);
        QCOMPARE(folder->child(0)->icon(0).cacheKey(), icon0);
        QCOMPARE(folder->child(1)->icon(0).cacheKey(), icon1);
        w.buildLibrary();
        folder = w.library->topLevelItem(1);
        QVERIFY(!folder->child(0)->icon(0).isNull());
        QVERIFY(!folder->child(1)->icon(0).isNull());
        w.sidebarToggle->click();
        QVERIFY(!w.sidebarToggle->icon().isNull());
        QVERIFY(w.sidebarToggle->text().isEmpty());
        QVERIFY(w.findChild<QToolButton *>("sidebarLightweight")->mapTo(w.sidebar, QPoint()).y() < w.sidebarToggle->mapTo(w.sidebar, QPoint()).y());
        const auto shots = qEnvironmentVariable("TURING_PANEL_SHOTS");
        if (!shots.isEmpty()) {
            QDir().mkpath(shots);
            QCoreApplication::processEvents();
            QVERIFY(w.grab().save(shots + "/overview-file-and-sidebar.png"));
        }
        w.quitting = true;
    }
    void collapsiblePanesRestoreAcrossOrientationsAndWindowRebuild() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.sampling.stop(); w.show();
        QTest::qWait(50);
        const auto capture = [&](const QString &name) {
            const auto shots = qEnvironmentVariable("TURING_PANEL_SHOTS");
            if (!shots.isEmpty()) {
                QDir().mkpath(shots); QCoreApplication::processEvents();
                QVERIFY(w.grab().save(shots + '/' + name + ".png"));
            }
        };
        const auto clickToggle = [&](PaneSplitter *splitter, int index, Qt::ArrowType before, Qt::ArrowType after) {
            auto handle = splitter->handle(index);
            auto toggle = handle->findChild<QToolButton *>("paneToggle");
            QVERIFY(handle && toggle);
            QCOMPARE(toggle->arrowType(), before);
            QEvent leave(QEvent::Leave); QApplication::sendEvent(handle, &leave);
            QVERIFY(!toggle->isVisible());
            const auto point = handle->rect().center();
            QEnterEvent enter(point, handle->mapTo(&w, point), handle->mapToGlobal(point));
            QApplication::sendEvent(handle, &enter);
            QTRY_VERIFY(toggle->isVisible());
            QTest::mouseClick(toggle, Qt::LeftButton);
            QCoreApplication::processEvents();
            QCOMPARE(toggle->arrowType(), after);
        };
        clickToggle(w.librarySplit, 1, Qt::LeftArrow, Qt::RightArrow);
        QVERIFY(w.librarySplit->paneCollapsed(0)); QCOMPARE(w.librarySplit->sizes()[0], 0);
        capture("library-hidden-horizontal");
        clickToggle(w.librarySplit, 2, Qt::RightArrow, Qt::LeftArrow);
        QVERIFY(w.librarySplit->paneCollapsed(2)); QCOMPARE(w.librarySplit->sizes()[2], 0);
        w.resize(760, 680);
        QTRY_COMPARE(w.librarySplit->orientation(), Qt::Vertical);
        QVERIFY(w.librarySplit->paneCollapsed(0) && w.librarySplit->paneCollapsed(2));
        QCOMPARE(w.librarySplit->sizes()[0], 0); QCOMPARE(w.librarySplit->sizes()[2], 0);
        clickToggle(w.librarySplit, 1, Qt::DownArrow, Qt::UpArrow);
        clickToggle(w.librarySplit, 2, Qt::UpArrow, Qt::DownArrow);
        QVERIFY(w.librarySplit->sizes()[0] > 0 && w.librarySplit->sizes()[2] > 0);
        capture("panes-restored-vertical");
        w.setEditing(true);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Vertical);
        clickToggle(w.editorSplit, 1, Qt::DownArrow, Qt::UpArrow);
        QVERIFY(w.editorSplit->paneCollapsed(1)); QCOMPARE(w.editorSplit->sizes()[1], 0);
        capture("editor-hidden-vertical");
        w.resize(1280, 820);
        QTRY_COMPARE(w.editorSplit->orientation(), Qt::Horizontal);
        clickToggle(w.editorSplit, 1, Qt::LeftArrow, Qt::RightArrow);
        QVERIFY(w.editorSplit->sizes()[1] > 0);
        w.librarySplit->setPaneCollapsed(0, true);
        w.librarySplit->setPaneCollapsed(2, true);
        w.editorSplit->setPaneCollapsed(1, true);
        w.build();
        QVERIFY(w.librarySplit->paneCollapsed(0) && w.librarySplit->paneCollapsed(2));
        QVERIFY(w.editorSplit->paneCollapsed(1));
        QCOMPARE(w.editorSplit->sizes()[1], 0);
        w.quitting = true;
    }
    void traySerialMenuListsPortsAndMarksOnlyConnectedPort() {
        TestProfile dir;
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.listPorts = [] { return QList<Port>{{"TEST-MISSING-PORT-A", "First serial port", false}, {"TEST-MISSING-PORT-B", "Second serial port", false}}; };
        app.refreshPortMenu();
        QList<QAction *> ports;
        for (auto action : app.portsMenu->actions()) if (!action->property("serialPort").toString().isEmpty()) ports.append(action);
        QCOMPARE(ports.size(), 2);
        QCOMPARE(ports[0]->text(), QString("TEST-MISSING-PORT-A"));
        QVERIFY(!ports[0]->isChecked() && !ports[1]->isChecked());
        DeviceStatus connected; connected.connected = true; connected.port = "TEST-MISSING-PORT-B";
        app.updatePortChecks(connected);
        QVERIFY(!ports[0]->isChecked()); QVERIFY(ports[1]->isChecked());
        connected.connected = false; app.updatePortChecks(connected);
        QVERIFY(!ports[0]->isChecked() && !ports[1]->isChecked());
        ports[0]->trigger();
        QCOMPARE(app.display().config["port"].toString(), QString("TEST-MISSING-PORT-A"));
        QTRY_VERIFY(!app.display().device.status().connecting);
        QVERIFY(!app.display().device.status().connected);
        app.updateTrayState(); QVERIFY(!ports[0]->isChecked());
        QVERIFY(!app.isLightweight()); // A failed serial open does not change window mode.
        app.listPorts = [] { return QList<Port>(); };
        app.refreshPortMenu();
        QVERIFY(!app.portsMenu->actions().first()->isEnabled());
        QVERIFY(!app.portsMenu->findChild<QAction *>("trayDisconnect")->isEnabled());
    }
    void customUiPaletteFilesReloadValidateAndRestore() {
        TestProfile dir;
        const auto restore = qScopeGuard([&] { UiAppearance::instance().apply("system", dir.path()); });
        QDir().mkpath(dir.path() + "/ui-themes");
        const auto fileName = dir.path() + "/ui-themes/desk.json";
        auto json = QJsonObject{{"format", "turing-ui-palette"}, {"version", 1}, {"name", "Desk"},
            {"extends", "catppuccin-mocha"}, {"colors", QJsonObject{{"window", "#112233"}, {"accent", "#ffcc88"}}}};
        const auto write = [&](const QJsonObject &value) {
            QFile file(fileName);
            return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(value).toJson()) > 0;
        };
        QVERIFY(write(json));
        {
            MainWindow w(dir.path());
            w.sampling.stop();
            const auto display = w.frame;
            w.uiThemeBox->setCurrentIndex(w.uiThemeBox->findData("custom:desk"));
            QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#112233"));
            QCOMPARE(w.config["ui_theme"].toString(), QString("custom:desk"));
            const auto valid = QApplication::palette();
            json["colors"] = QJsonObject{{"window", "not-a-color"}};
            QVERIFY(write(json));
            w.findChild<QPushButton *>("reloadUiThemes")->click();
            QCOMPARE(QApplication::palette(), valid);
            QVERIFY(!w.uiThemeError.isEmpty());
            QCOMPARE(w.frame, display);
            json["colors"] = QJsonObject{{"window", "#223344"}};
            QVERIFY(write(json));
            w.findChild<QPushButton *>("reloadUiThemes")->click();
            QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#223344"));
            QVERIFY(w.uiThemeError.isEmpty());
            json["colors"] = QJsonObject{{"typo", "#223344"}};
            QVERIFY(write(json));
            QVERIFY(loadUiTheme("custom:desk", dir.path()).error.contains("unknown color role"));
            json["colors"] = QJsonObject{{"window", "#223344"}};
            QVERIFY(write(json));
            QVERIFY(!loadUiTheme("custom:../desk", dir.path()).error.isEmpty());
        }
        {
            MainWindow copy(dir.path());
            copy.sampling.stop();
            QCOMPARE(copy.uiThemeBox->currentData().toString(), QString("custom:desk"));
            QCOMPARE(QApplication::palette().color(QPalette::Window), QColor("#223344"));
            copy.findChild<QPushButton *>("copyUiTheme")->click();
            QCOMPARE(copy.config["ui_theme"].toString(), QString("custom:my-palette"));
            const auto exported = loadUiTheme("custom:my-palette", dir.path());
            QVERIFY(exported.error.isEmpty());
            QCOMPARE(exported.colors["window"], QColor("#223344"));
            QFile broken(dir.path() + "/ui-themes/my-palette.json");
            QVERIFY(broken.open(QIODevice::WriteOnly));
            broken.write("{broken");
        }
        MainWindow fallback(dir.path());
        fallback.sampling.stop();
        QCOMPARE(UiAppearance::instance().currentId(), QString("system"));
        QVERIFY(!fallback.uiThemeError.isEmpty());
        QCOMPARE(fallback.config["ui_theme"].toString(), QString("custom:my-palette"));
    }
    void statusIndicatorsAndFourPageNavigation() {
        TestProfile dir;
        const auto restore = qScopeGuard([&] { UiAppearance::instance().apply("system", dir.path()); });
        MainWindow w(dir.path());
        QCOMPARE(w.navigation->count(), 4);
        QCOMPARE(w.pages->count(), 4);
        for (int n = 0; n < w.navigation->count(); ++n)
            QVERIFY(!w.navigation->item(n)->data(Qt::AccessibleTextRole).toString().contains("诊断"));
        for (auto button : w.findChildren<QPushButton *>()) {
            QVERIFY(!button->text().contains("导出诊断"));
            QVERIFY(!button->text().contains("Export diagnostics"));
        }
        w.updateStatus();
        QCOMPARE(w.dataState->property("statusTone").toString(), QString("success"));
        QCOMPARE(w.connection->property("statusTone").toString(), QString("muted"));
        for (const auto &id : {"catppuccin-mocha", "catppuccin-latte"}) {
            w.selectUiTheme(id);
            QTextDocument text;
            text.setHtml(w.dataState->text());
            const auto dot = text.find("●");
            QCOMPARE(dot.charFormat().foreground().color(), loadUiTheme(id, dir.path()).colors["success"]);
            QVERIFY(!w.dataState->accessibleName().isEmpty());
        }
        w.sampling.stop();
        w.updateStatus();
        QCOMPARE(w.dataState->property("statusTone").toString(), QString("muted"));
    }
    void systemPaletteAndFontDriveFlatAppearance() {
        TestProfile dir;
        {
            QFile file(dir.path() + "/config.json");
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{\"palette\":\"forest\",\"language\":\"english\",\"weather_enabled\":false}");
        }
        const auto previousPalette = QApplication::palette();
        const auto previousFont = QApplication::font();
        const auto restore = qScopeGuard([&] {
            QApplication::setPalette(previousPalette);
            QApplication::setFont(previousFont);
        });
        auto desktopPalette = previousPalette;
        desktopPalette.setColor(QPalette::Window, QColor("#eadfc9"));
        desktopPalette.setColor(QPalette::Highlight, QColor("#a34976"));
        QApplication::setPalette(desktopPalette);
        auto desktopFont = previousFont;
        desktopFont.setPointSize(12);
        QApplication::setFont(desktopFont);
        ApplicationController app(dir.path(), {}, [] { return true; });
        app.showWindow();
        auto w = app.window();
        w->sampling.stop();
        QVERIFY(!w->config.contains("palette"));
        const auto verifyAppearance = [&] {
            QCoreApplication::processEvents();
            QVERIFY(qApp->styleSheet().contains("Shared flat widget design"));
            QCOMPARE(QApplication::palette().color(QPalette::Window), desktopPalette.color(QPalette::Window));
            QCOMPARE(QApplication::palette().color(QPalette::Highlight), desktopPalette.color(QPalette::Highlight));
            const QList<QWidget *> widgets{w, w->root, w->navigation, w->library, w->tree,
                                          w->portBox, w->connectButton, w->brightness, w->mirrorBox,
                                          w->sensorTable, w->canvas};
            for (auto widget : widgets) {
                QVERIFY(widget->styleSheet().isEmpty());
                QCOMPARE(widget->font(), QApplication::font(widget));
            }
            for (auto combo : w->findChildren<QComboBox *>()) {
                QCOMPARE(combo->findData("graphite"), -1);
                QCOMPARE(combo->findData("forest"), -1);
            }
            QCOMPARE(w->canvas->grab().toImage().pixelColor(0, 0), desktopPalette.color(QPalette::Window));
        };
        verifyAppearance();
        const auto preview = w->frame;
        desktopPalette.setColor(QPalette::Window, QColor("#252c35"));
        desktopPalette.setColor(QPalette::Highlight, QColor("#8fbc70"));
        QApplication::setPalette(desktopPalette);
        desktopFont.setPointSize(14);
        QApplication::setFont(desktopFont);
        verifyAppearance();
        w->english = false;
        w->build();
        w->render();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        verifyAppearance();
        QCOMPARE(w->frame, preview); // Desktop appearance must not alter the device's pixels.
        w->persist();
        QFile file(dir.path() + "/config.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!QJsonDocument::fromJson(file.readAll()).object().contains("palette"));
    }
    void pageIsolationPreferencesAndGeometry() {
        TestProfile dir;
        {
            MainWindow w(dir.path());
            w.show();
            QTest::qWait(50);
            w.setPage(MainWindow::SettingsPage);
            QCOMPARE(w.pages->currentIndex(), int(MainWindow::SettingsPage));
            QVERIFY(!w.left->isVisible());
            w.resize(1100, 720);
            QTest::qWait(50);
            QCOMPARE(w.size(), QSize(1100, 720));
            w.setPage(MainWindow::LibraryPage);
            QVERIFY(w.library->isVisible());
            QVERIFY(!w.canvas->isVisible());
            w.config["minimize_to_tray"] = true;
            w.config["interval_ms"] = 3000;
            w.config["language"] = "english";
            w.persist();
            w.dirty = false;
            w.quitting = true;
        }
        MainWindow copy(dir.path());
        QVERIFY(copy.config["minimize_to_tray"].toBool());
        QCOMPARE(copy.config["interval_ms"].toInt(), 3000);
        QVERIFY(copy.english);
        QVERIFY(!copy.config.contains("palette"));
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
    void sensorColumnsResizeFromHeaderAndKeepTheirWidths() {
        TestProfile dir;
        QJsonArray widths;
        {
            MainWindow w(dir.path());
            w.sampling.stop(); w.show();
            w.selectUiTheme("catppuccin-mocha");
            w.setPage(MainWindow::SensorsPage);
            QCoreApplication::processEvents();
            auto table = w.sensorTable;
            auto header = table->horizontalHeader();
            QVERIFY(table->showGrid());
            QVERIFY(!header->stretchLastSection());
            for (int column = 0; column < 3; ++column)
                QCOMPARE(header->sectionResizeMode(column), QHeaderView::Interactive);
            const int before = table->columnWidth(0);
            const QPoint start(header->sectionViewportPosition(0) + before - 1, header->height() / 2);
            const QPoint end = start + QPoint(64, 0);
            QTest::mousePress(header->viewport(), Qt::LeftButton, Qt::NoModifier, start);
            QMouseEvent move(QEvent::MouseMove, QPointF(end), QPointF(header->viewport()->mapToGlobal(end)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(header->viewport(), &move);
            QTest::mouseRelease(header->viewport(), Qt::LeftButton, Qt::NoModifier, end);
            QTRY_COMPARE(table->columnWidth(0), before + 64);
            table->setColumnWidth(1, 310);
            table->setColumnWidth(2, 440);
            widths = w.config["sensor_column_widths"].toArray();
            QCOMPARE(widths, QJsonArray({before + 64, 310, 440}));
            w.tick();
            w.resize(760, 680);
            QCoreApplication::processEvents();
            for (int column = 0; column < 3; ++column) QCOMPARE(table->columnWidth(column), widths[column].toInt());
            w.english = !w.english;
            w.build();
            QCoreApplication::processEvents();
            for (int column = 0; column < 3; ++column) QCOMPARE(w.sensorTable->columnWidth(column), widths[column].toInt());
            const auto shots = qEnvironmentVariable("TURING_PANEL_SHOTS");
            if (!shots.isEmpty()) {
                QDir().mkpath(shots);
                w.resize(1280, 820);
                QCoreApplication::processEvents();
                QVERIFY(w.grab().save(shots + "/sensors-resizable-columns.png"));
            }
            w.quitting = true;
        }
        MainWindow reopened(dir.path());
        reopened.sampling.stop(); reopened.show();
        reopened.setPage(MainWindow::SensorsPage);
        QCoreApplication::processEvents();
        for (int column = 0; column < 3; ++column) QCOMPARE(reopened.sensorTable->columnWidth(column), widths[column].toInt());
        reopened.quitting = true;
    }
    void memoryGbAndWeatherSettings() {
        TestProfile dir;
        MainWindow w(dir.path());
        w.show();
        QTest::qWait(50);
        w.setPage(MainWindow::SensorsPage);
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
        w.setPage(MainWindow::SettingsPage);
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
        w.choose(assets() + "/themes/official/微星.data");
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
int main(int argc, char **argv) {
    qputenv("TURING_QT_ISOLATED_LIBRARY", "1");
    QApplication app(argc, argv);
    Screen::GuiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "gui_test.moc"
