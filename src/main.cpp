#include "application.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTextStream>
int main(int argc, char **argv) {
    bool cli = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--render-theme" || QByteArray(argv[i]) == "--check-theme" ||
            QByteArray(argv[i]) == "--packet" || QByteArray(argv[i]) == "--list-ports" ||
            QByteArray(argv[i]) == "--dump-sensors")
            cli = true;
    if (cli && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // Qt's DirectWrite fallback database has a large first-paint allocation on
    // Windows. GDI keeps the native Widgets interface within the memory budget.
    if (!cli && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=gdi");
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("TuringScreen");
    QCoreApplication::setApplicationName("TuringScreenQt");
    QGuiApplication::setApplicationDisplayName("Turing Screen");
    QCoreApplication::setApplicationVersion(SCREEN_VERSION);
    // Keep the platform's style, palette and font, including Qt style plugins.
    Screen::resourceTrace("QApplication");
    QImageReader::setAllocationLimit(16);
    QCommandLineParser parser;
    parser.setApplicationDescription("Turing Screen USB smart display");
    parser.addHelpOption();
    parser.addOption({{"v", "version"}, "Show version"});
    parser.addOptions({{"data-dir", "Data directory", "directory"},
                       {"autostart", "Connect the saved serial port, send frames and start in lightweight mode"},
                       {"theme", "Theme to open", "file"},
                       {"render-theme", "Render a theme without showing a window", "file"},
                       {"check-theme", "Validate a theme", "file"},
                       {"output", "Output file", "file"},
                       {"snapshot", "Sensor sample JSON", "file"},
                       {"editor-preview", "Official editor sample rendering"},
                       {"packet", "Export a RGB565 device packet from a theme", "file"},
                       {"list-ports", "Enumerate serial ports"},
                       {"dump-sensors", "Print live sensor values and unavailable reasons as JSON"},
                       {"windows-sensor-source", "Windows hardware source: system, hwinfo, aida64, librehardwaremonitor", "source"},
                       {"lhm-port", "LibreHardwareMonitor local Web Server port", "port"},
                       {"gpu-index", "GPU index in the selected monitor, starting at zero", "index"},
                       {"weather-city", "Enable weather for a city (empty uses IP location)", "city"},
                       {"smoke-test", "Native GUI screenshots and resource test", "directory"},
                       {"stress-test", "Theme/editor/resource workload", "directory"},
                       {"test-seconds", "Test duration in seconds", "seconds", "60"}});
    parser.process(app);
    if (parser.isSet("version")) {
        QTextStream(stdout) << "Turing Screen " << SCREEN_VERSION << Qt::endl;
        return 0;
    }
    try {
        if (parser.isSet("dump-sensors")) {
            Screen::Collector collector;
            auto dir = parser.value("data-dir");
            if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QFile configFile(dir + "/config.json");
            QJsonObject config;
            if (configFile.open(QIODevice::ReadOnly)) config = QJsonDocument::fromJson(configFile.readAll()).object();
            const auto source = parser.isSet("windows-sensor-source") ? parser.value("windows-sensor-source") : config["windows_sensor_source"].toString("system");
            if (Screen::windowsSensorSource(source) != source) throw std::runtime_error("Unknown Windows sensor source");
            bool portOk = true, gpuOk = true;
            const int port = parser.isSet("lhm-port") ? parser.value("lhm-port").toInt(&portOk) : config["lhm_port"].toInt(8085);
            const int gpu = parser.isSet("gpu-index") ? parser.value("gpu-index").toInt(&gpuOk) : config["windows_gpu_index"].toInt(0);
            if (!portOk || port < 1 || port > 65535 || !gpuOk || gpu < 0 || gpu > 31) throw std::runtime_error("Invalid LHM port or GPU index");
            collector.setWindowsSource(source, port, gpu);
            if (parser.isSet("weather-city"))
                collector.setWeather(true, parser.value("weather-city"));
            collector.collect();
            QTimer::singleShot(parser.isSet("weather-city") ? 20000 : source == "librehardwaremonitor" ? 4000 : 250, &app, [&] {
                QTextStream(stdout) << QJsonDocument(collector.collect().json()).toJson();
                app.quit();
            });
            return app.exec();
        }
        if (parser.isSet("list-ports")) {
            QTextStream out(stdout);
            for (auto p : Screen::Device::ports())
                out << p.name << '\t' << p.description << '\n';
            return 0;
        }
        if (parser.isSet("check-theme")) {
            auto t = Screen::Theme::load(parser.value("check-theme"));
            QTextStream(stdout) << t.name() << ' ' << t.size().width() << 'x' << t.size().height()
                                << Qt::endl;
            return 0;
        }
        if (parser.isSet("render-theme") || parser.isSet("packet")) {
            auto t = Screen::Theme::load(parser.value(parser.isSet("packet") ? "packet" : "render-theme"));
            Screen::Snapshot s;
            if (parser.isSet("snapshot")) {
                QFile f(parser.value("snapshot"));
                if (!f.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Cannot read snapshot");
                s = Screen::Snapshot::fromJson(QJsonDocument::fromJson(f.readAll()).object());
            } else {
                Screen::Collector collector;
                s = collector.collect();
            }
            Screen::Renderer renderer;
            auto image = renderer.render(t, s, parser.isSet("editor-preview"));
            auto output = parser.value("output");
            if (output.isEmpty())
                throw std::runtime_error("--output is required");
            if (parser.isSet("packet")) {
                QFile file(output);
                if (!file.open(QIODevice::WriteOnly))
                    throw std::runtime_error("Cannot write packet");
                file.write(Screen::Protocol::bitmap(image));
            } else if (!image.save(output))
                throw std::runtime_error("Cannot save image");
            return 0;
        }
        QString dir = parser.value("data-dir");
        if (dir.isEmpty())
            dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        Screen::ApplicationController controller(dir, parser.value("theme"),
                                                QSystemTrayIcon::isSystemTrayAvailable, parser.isSet("autostart"));
        if (!controller.isLightweight() || parser.isSet("smoke-test") || parser.isSet("stress-test"))
            controller.showWindow();
        Screen::resourceTrace(controller.isLightweight() ? "lightweight autostart" : "window shown");
        if (parser.isSet("smoke-test") || parser.isSet("stress-test")) {
            int seconds = qBound(5, parser.value("test-seconds").toInt(), 600);
            controller.window()->runSmoke(parser.value(parser.isSet("stress-test") ? "stress-test" : "smoke-test"),
                            parser.isSet("stress-test"), seconds);
        }
        return app.exec();
    } catch (const std::exception &e) {
        QTextStream(stderr) << e.what() << Qt::endl;
        return 1;
    }
}
