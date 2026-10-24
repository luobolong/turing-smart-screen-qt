// Optional diagnostic executable; never installed or run by the application.
#include "mainwindow.h"
#include "application.h"
#include <QtWidgets>
#include <QDirIterator>
#include <QFontDatabase>
#include <QLibraryInfo>
#include <algorithm>
#include <memory>

namespace Screen {
class ProbeApplication : public QApplication {
public:
    using QApplication::QApplication;
    qint64 paintNanoseconds = 0;
    quint64 paints = 0, resizeEvents = 0, layoutRequests = 0;
    bool notify(QObject *object, QEvent *event) override {
        resizeEvents += event->type() == QEvent::Resize;
        layoutRequests += event->type() == QEvent::LayoutRequest;
        if (event->type() != QEvent::Paint)
            return QApplication::notify(object, event);
        QElapsedTimer elapsed;
        elapsed.start();
        const bool result = QApplication::notify(object, event);
        paintNanoseconds += elapsed.nsecsElapsed();
        ++paints;
        return result;
    }
};

class PerformanceProbe {
    struct Timing {
        qint64 nanoseconds = 0;
        quint64 calls = 0;
        double maximumMs = 0;
        template<class Function> void run(Function &&function) {
            QElapsedTimer clock;
            clock.start();
            function();
            const auto elapsed = clock.nsecsElapsed();
            nanoseconds += elapsed;
            ++calls;
            maximumMs = qMax(maximumMs, elapsed / 1e6);
        }
        QJsonObject json() const {
            return {{"calls", double(calls)}, {"total_ms", nanoseconds / 1e6},
                    {"mean_ms", calls ? nanoseconds / 1e6 / calls : 0}, {"max_ms", maximumMs}};
        }
    };
    static Snapshot fixture(const QString &file) {
        QFile input(file);
        if (!input.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read profiling snapshot");
        return Snapshot::fromJson(QJsonDocument::fromJson(input.readAll()).object());
    }
    static void write(const QString &file, const QJsonObject &json) {
        QFile output(file);
        if (!output.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot write profiling output");
        output.write(QJsonDocument(json).toJson());
    }
    static QJsonObject windowState(const MainWindow &window) {
        int thumbnails = 0;
        for (int i = 0; i < window.library->count(); ++i)
            thumbnails += !window.library->item(i)->icon().isNull();
        return {{"image_cache_bytes", window.renderer.cachedBytes()},
                {"theme_json_bytes", QJsonDocument(window.theme.data).toJson(QJsonDocument::Compact).size()},
                {"theme", window.theme.name()}, {"drafts", window.drafts.size()},
                {"undo_states", window.undoHistory.size()}, {"catalog_entries", window.entries.size()},
                {"thumbnail_count", thumbnails}, {"widget_count", QApplication::allWidgets().size()},
                {"window_width", window.width()}, {"window_height", window.height()},
                {"visible", window.isVisible()},
                {"device_pixel_ratio", window.devicePixelRatioF()},
                {"screen_width", window.screen()->geometry().width()},
                {"screen_height", window.screen()->geometry().height()},
                {"weather_ready", window.sample.strings.contains("Weather")},
                {"process", processStats().json()}};
    }
    static int micro(ProbeApplication &app, const QCommandLineParser &parser) {
        const QString output = parser.value("output");
        QJsonArray rows;
        const auto measure = [&](QString kind, QString name, int count, auto &&function) {
            Timing timing;
            count = qMax(1, count);
            for (int i = 0; i < count; ++i)
                timing.run(function);
            auto row = timing.json();
            row["kind"] = kind;
            row["name"] = name;
            rows.append(row);
        };
        const double factor = qBound(.01, parser.value("factor").toDouble(), 100.);
        auto sample = fixture(parser.value("snapshot"));
        // Also populate legacy themes' clock and disk fields.
        sample.strings["Time"] = "23:58";
        sample.values["HDDLoad"] = 43;
        sample.values["HDDTemp"] = 48;
        const auto before = processStats().json();
        QImage image;
        Renderer renderer;
        {
            Collector collector;
            collector.collect();
            measure("collect", "all_sensors", 100 * factor, [&] { sample = collector.collect(); });
        }
        sample = fixture(parser.value("snapshot"));
        sample.strings["Time"] = "23:58";
        const auto starter = Theme::starter();
        renderer.render(starter, sample);
        measure("render", "System overview", 100 * factor, [&] { image = renderer.render(starter, sample); });
        QStringList files{assets() + "/themes/nightfall.json"};
        QDirIterator iterator(assets() + "/official/config", {"*.data"}, QDir::Files);
        while (iterator.hasNext()) {
            const auto file = iterator.next();
            if (!file.endsWith("AppConfig.data"))
                files.append(file);
        }
        for (const auto &file : files) {
            Theme theme;
            measure("load", QFileInfo(file).fileName(), 5 * factor, [&] { theme = Theme::load(file); });
            image = renderer.render(theme, sample);
            const int repetitions = file.endsWith("nightfall.json") ? 100 : 15;
            measure("render", theme.name(), repetitions * factor, [&] { image = renderer.render(theme, sample); });
        }
        image = renderer.render(Theme::load(files.first()), sample);
        QByteArray packet;
        measure("protocol", "RGB565_full_frame", 200 * factor, [&] { packet = Protocol::bitmap(image); });
        auto next = image.copy();
        next.setPixelColor(240, 120, Qt::red);
        QRect changed;
        measure("protocol", "changed_rectangle", 200 * factor, [&] { changed = Protocol::changed(image, next); });
        write(output, {{"mode", "micro"}, {"qt", qVersion()}, {"rows", rows},
                       {"image_cache_bytes", renderer.cachedBytes()}, {"before", before},
                       {"after", processStats().json()}, {"packet_bytes", packet.size()},
                       {"changed_width", changed.width()}, {"changed_height", changed.height()}});
        Q_UNUSED(app)
        return 0;
    }
public:
    static int run(ProbeApplication &app, const QCommandLineParser &parser) {
        const auto mode = parser.value("mode");
        const auto scenario = parser.value("scenario");
        const int seconds = qMax(5, parser.value("seconds").toInt());
        const auto output = parser.value("output");
        if (mode == "micro")
            return micro(app, parser);
        std::unique_ptr<MainWindow> window;
        std::unique_ptr<ApplicationController> controller;
        std::unique_ptr<Collector> collector;
        std::unique_ptr<Renderer> renderer;
        Theme theme;
        Snapshot snapshot = fixture(parser.value("snapshot"));
        Timing work;
        int switches = 0;
        const bool weather = parser.isSet("weather");
        QTimer timer, workload;
        timer.setTimerType(Qt::CoarseTimer);
        timer.setInterval(1000);
        QElapsedTimer elapsed;
        elapsed.start();
        const auto started = processStats().json();
        if (mode == "session") {
            const auto data = parser.value("data-dir");
            QDir().mkpath(data);
            write(data + "/config.json", {{"weather_enabled", weather}, {"language", "english"}, {"interval_ms", 1000}});
            // A bounded diagnostic run can restore/exit without a tray host.
            controller = std::make_unique<ApplicationController>(data, parser.value("theme"), [] { return true; });
            controller->window()->setAttribute(Qt::WA_ShowWithoutActivating);
            controller->window()->setFixedSize(1360, 900);
            controller->showWindow();
            auto &display = controller->display();
            display.sampling.stop();
            if (!parser.value("port").isEmpty()) {
                display.device.connectPort(parser.value("port"), display.theme.orientation(), 75, false);
                display.sending = true;
            }
            QObject::connect(&timer, &QTimer::timeout, &app, [&] { work.run([&] { controller->display().tick(); }); });
            timer.start();
            QTimer::singleShot(5000, &app, [&] {
                if (!controller->enterLightweight()) qFatal("Could not enter lightweight mode");
            });
            if (scenario == "cycles") {
                workload.setInterval(5000);
                QObject::connect(&workload, &QTimer::timeout, &app, [&] {
                    if (elapsed.elapsed() < 9000) return;
                    if (controller->isLightweight()) {
                        controller->showWindow();
                        controller->window()->setFixedSize(1360, 900);
                    } else controller->enterLightweight();
                    ++switches;
                });
                workload.start();
            }
        } else if (mode == "window") {
            const auto data = parser.value("data-dir");
            QDir().mkpath(data);
            write(data + "/config.json", {{"weather_enabled", weather}, {"weather_city", ""},
                                          {"language", parser.value("language")},
                                          {"palette", parser.value("palette")}, {"interval_ms", 1000},
                                          {"minimize_to_tray", false}});
            window = std::make_unique<MainWindow>(data, parser.value("theme"));
            window->setAttribute(Qt::WA_ShowWithoutActivating);
            if (!parser.isSet("free-size"))
                window->setFixedSize(1360, 900);
            window->show();
            window->sampling.stop();
            if (scenario == "editor") window->setEditing(true);
            if (scenario == "sensors") window->setPage(1);
            if (scenario == "settings") window->setPage(2);
            if (scenario == "diagnostics") window->setPage(3);
            if (scenario == "hidden")
                QTimer::singleShot(5000, window.get(), [&] { window->hide(); });
            if (scenario == "empty") {
                window->theme = Theme::fromJson(QJsonObject{{"format", "turing-screen-qt"}, {"layers", QJsonArray()},
                                                          {"name", "Empty"}, {"landscape", true}});
                window->canvas->theme = &window->theme;
                window->render();
            }
            if (scenario != "frozen") {
                QObject::connect(&timer, &QTimer::timeout, &app, [&] {
                    work.run([&] {
                        if (scenario == "render-only") window->render();
                        else window->tick();
                    });
                });
                timer.start();
            }
            if (scenario == "switch" || scenario == "scroll" || scenario == "edit") {
                workload.setInterval(250);
                QObject::connect(&workload, &QTimer::timeout, &app, [&] {
                    if (elapsed.elapsed() < 5000 || elapsed.elapsed() > (seconds - 5) * 1000)
                        return;
                    work.run([&] {
                        if (scenario == "switch") {
                            window->choose(window->entries[switches % window->entries.size()].path);
                            window->setEditing(switches % 2);
                        } else if (scenario == "scroll") {
                            const int n = switches % window->library->count();
                            window->library->scrollToItem(window->library->item(n), QAbstractItemView::PositionAtTop);
                            window->thumbnails();
                        } else {
                            window->setEditing(true);
                            window->addComponent("CPU");
                            if (switches % 10 == 9) window->undo();
                        }
                        ++switches;
                    });
                });
                workload.start();
            }
        } else if (mode == "collector") {
            collector = std::make_unique<Collector>();
            collector->setWeather(weather);
            QObject::connect(&timer, &QTimer::timeout, &app, [&] { work.run([&] { snapshot = collector->collect(); }); });
            timer.start();
        } else if (mode == "render") {
            renderer = std::make_unique<Renderer>();
            theme = parser.value("theme").isEmpty() ? Theme::starter() : Theme::load(parser.value("theme"));
            QObject::connect(&timer, &QTimer::timeout, &app, [&] { work.run([&] { renderer->render(theme, snapshot); }); });
            timer.start();
        } else if (mode != "baseline") {
            throw std::runtime_error("Invalid profiling mode");
        }
        QJsonObject ready{{"mode", mode}, {"scenario", scenario}, {"ready_ms", elapsed.elapsed()},
                          {"process", processStats().json()}};
        if (window) ready["window"] = windowState(*window);
        if (controller) ready["window"] = windowState(*controller->window());
        write(output + ".ready.json", ready);
        QTimer::singleShot(5000, &app, [&] {
            QJsonObject steady{{"seconds", elapsed.elapsed() / 1000.}, {"process", processStats().json()}};
            steady["paint_events"] = double(app.paints);
            steady["resize_events"] = double(app.resizeEvents);
            steady["layout_requests"] = double(app.layoutRequests);
            if (window) steady["window"] = windowState(*window);
            if (controller) steady["lightweight"] = controller->isLightweight();
            write(output + ".steady.json", steady);
        });
        QTimer::singleShot(seconds * 1000, &app, [&] {
            const auto ended = processStats().json();
            QJsonObject report{{"mode", mode}, {"scenario", scenario}, {"qt", qVersion()},
                               {"platform", QGuiApplication::platformName()}, {"weather", weather},
                               {"seconds", elapsed.elapsed() / 1000.}, {"started", started}, {"ended", ended},
                               {"work", work.json()}, {"actions", switches},
                               {"paint_events", double(app.paints)}, {"paint_ms", app.paintNanoseconds / 1e6}};
            report["resize_events"] = double(app.resizeEvents);
            report["layout_requests"] = double(app.layoutRequests);
            if (window) {
                report["window"] = windowState(*window);
                window->dirty = false;
                window->quitting = true;
            }
            if (renderer) report["image_cache_bytes"] = renderer->cachedBytes();
            if (controller) {
                const auto &display = controller->display();
                report["lightweight"] = controller->isLightweight();
                report["widget_count"] = QApplication::allWidgets().size();
                report["rendered_frames"] = double(display.renderedFrames);
                report["sent_frames"] = double(controller->display().device.status().frames);
                report["device_connected"] = controller->display().device.status().connected;
                report["weather_ready"] = display.sample.strings.contains("Weather");
                report["image_cache_bytes"] = display.renderer.cachedBytes();
                if (controller->window()) report["window"] = windowState(*controller->window());
            }
            write(output, report);
            app.quit();
        });
        return app.exec();
    }
};
} // namespace Screen

int main(int argc, char **argv) {
    if (argc == 2 && QByteArray(argv[1]) == "--qt-plugin-path") {
        QTextStream(stdout) << QLibraryInfo::path(QLibraryInfo::PluginsPath) << Qt::endl;
        return 0;
    }
    Screen::ProbeApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setStyle("Fusion");
    QCoreApplication::setOrganizationName("TuringScreen");
    QCoreApplication::setApplicationName("TuringScreenPerformanceProbe");
    QFont font(Screen::screenFontFamily());
    font.setPixelSize(14);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::NoFontMerging | QFont::PreferAntialias));
    app.setFont(font);
    Screen::resourceTrace("probe QApplication");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{"mode", "baseline, collector, render, window, micro", "mode", "window"},
                       {"scenario", "Window scenario", "scenario", "preview"},
                       {"theme", "Theme file", "file"}, {"snapshot", "Snapshot file", "file"},
                       {"output", "Report file", "file"}, {"data-dir", "Temporary profile", "directory"},
                       {"seconds", "Run duration", "seconds", "30"}, {"weather", "Enable IP weather"},
                       {"free-size", "Let the window manager choose the window size"},
                       {"palette", "UI palette", "palette", "graphite"},
                       {"language", "UI language", "language", "english"},
                       {"factor", "Microbenchmark iteration multiplier", "factor", "1"},
                       {"port", "Explicit serial test port (session mode only)", "port"}});
    parser.process(app);
    try { return Screen::PerformanceProbe::run(app, parser); }
    catch (const std::exception &error) { qCritical("%s", error.what()); return 1; }
}
