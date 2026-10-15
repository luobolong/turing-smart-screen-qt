#include "mainwindow.h"
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
            QByteArray(argv[i]) == "--packet" || QByteArray(argv[i]) == "--list-ports")
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
    QCoreApplication::setApplicationVersion(SCREEN_VERSION);
    app.setStyle("Fusion");
    QFont uiFont(Screen::screenFontFamily());
    uiFont.setPixelSize(14);
    uiFont.setStyleStrategy(QFont::StyleStrategy(QFont::NoFontMerging | QFont::PreferAntialias));
    app.setFont(uiFont);
    Screen::resourceTrace("QApplication");
    QImageReader::setAllocationLimit(16);
    QCommandLineParser parser;
    parser.setApplicationDescription("Native Qt/C++ USB smart screen");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOptions({{"data-dir", "Data directory", "directory"},
                       {"theme", "Theme to open", "file"},
                       {"render-theme", "Render a theme without showing a window", "file"},
                       {"check-theme", "Validate a theme", "file"},
                       {"output", "Output file", "file"},
                       {"snapshot", "Sensor sample JSON", "file"},
                       {"editor-preview", "Official editor sample rendering"},
                       {"packet", "Export a RGB565 device packet from a theme", "file"},
                       {"list-ports", "Enumerate serial ports"},
                       {"smoke-test", "Native GUI screenshots and resource test", "directory"},
                       {"stress-test", "Theme/editor/resource workload", "directory"},
                       {"test-seconds", "Test duration in seconds", "seconds", "60"}});
    parser.process(app);
    try {
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
        Screen::MainWindow window(dir, parser.value("theme"));
        window.show();
        Screen::resourceTrace("window shown");
        if (parser.isSet("smoke-test") || parser.isSet("stress-test")) {
            int seconds = qBound(5, parser.value("test-seconds").toInt(), 600);
            window.runSmoke(parser.value(parser.isSet("stress-test") ? "stress-test" : "smoke-test"),
                            parser.isSet("stress-test"), seconds);
        }
        return app.exec();
    } catch (const std::exception &e) {
        QTextStream(stderr) << e.what() << Qt::endl;
        return 1;
    }
}
