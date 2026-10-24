#include "runtime.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTransform>
namespace Screen {
static bool usesMetric(const QJsonArray &layers, const QString &prefix) {
    for (const auto &value : layers) {
        const auto layer = value.toObject();
        if (!layer["visible"].toBool(true)) continue;
        if (canonical(layer["metric"].toString()).startsWith(prefix) || usesMetric(layer["children"].toArray(), prefix)) return true;
    }
    return false;
}
QString themeOrientationKey(const QString &file) {
    if (file.isEmpty()) return "__starter__";
    const auto relative = QDir(assets() + "/themes").relativeFilePath(QFileInfo(file).absoluteFilePath());
    if (!relative.startsWith("../") && !QDir::isAbsolutePath(relative)) return "builtin:" + relative;
    const auto canonical = QFileInfo(file).canonicalFilePath();
    return canonical.isEmpty() ? QFileInfo(file).absoluteFilePath() : canonical;
}
DisplayRuntime::DisplayRuntime(QString directory, const QString &initialTheme)
    : dataDir(std::move(directory)) {
    QDir().mkpath(dataDir + "/themes");
    QFile input(dataDir + "/config.json");
    if (input.open(QIODevice::ReadOnly)) config = QJsonDocument::fromJson(input.readAll()).object();
    theme = Theme::starter();
    const auto selected = initialTheme.isEmpty() ? config["selected_theme"].toString() : initialTheme;
    const auto relative = QDir(assets()).relativeFilePath(QFileInfo(selected).absoluteFilePath());
    const auto key = !relative.startsWith("../") && !QDir::isAbsolutePath(relative)
                         ? "builtin:" + relative : themeOrientationKey(selected);
    if (!selected.isEmpty() && (!initialTheme.isEmpty() || !config["hidden_themes"].toArray().contains(key))) {
        try { theme = Theme::load(selected); path = selected; } catch (const std::exception &) {}
    }
    auto direction = config["theme_orientations"].toObject().value(themeOrientationKey(path));
    if (direction.isUndefined() && initialTheme.isEmpty() && !theme.data.contains("orientation"))
        direction = config["orientation"];
    if (direction.isDouble() && direction.toInt(-1) >= 0 && direction.toInt(-1) <= 3 &&
        direction.toDouble() == direction.toInt()) theme.data["orientation"] = direction.toInt();
    configureWeather();
    connect(&sampling, &QTimer::timeout, this, &DisplayRuntime::tick);
    sampling.setTimerType(Qt::CoarseTimer);
    sampling.start(qBound(1000, config.value("interval_ms").toInt(1000), 10000));
    sample = collector.collect(config["disk"].toString());
}
void DisplayRuntime::configureWeather() {
    const auto options = theme.data["weather"].toObject();
    collector.setWeather(!backgroundIdle && (!lightweight || usesMetric(theme.layers(), "Weather")) &&
                         config["weather_enabled"].toBool(options["enabled"].toBool()),
                         config["weather_city"].toString(options["city"].toString()));
}
void DisplayRuntime::setLightweight(bool value) {
    lightweight = value;
    backgroundIdle = value && !(sending && device.status().connected);
    collector.setAudioEnabled(!backgroundIdle && (!value || usesMetric(theme.layers(), "Volume")));
    configureWeather();
    if (backgroundIdle) {
        frame = {};
        sample = {};
        renderer.clearCaches();
    }
}
void DisplayRuntime::tick() {
    // A paused/disconnected background session has no consumer for samples.
    if (lightweight && !(sending && device.status().connected)) {
        if (!backgroundIdle) setLightweight(true);
        return;
    }
    if (backgroundIdle) setLightweight(lightweight);
    sample = collector.collect(config["disk"].toString());
    if (preview || (sending && device.status().connected)) render();
    else emit updated();
}
void DisplayRuntime::render() {
    try {
        frame = renderer.render(theme, sample);
        ++renderedFrames;
        error.clear();
        if (sending && device.status().connected) {
            QImage image = frame;
            if ((theme.orientation() >= 2) != (image.width() > image.height()))
                image = image.transformed(QTransform().rotate(90));
            device.submit(image);
        }
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
    emit updated();
}
void DisplayRuntime::toggleSending() {
    if (!device.status().connected) {
        const auto port = config["port"].toString();
        if (!port.isEmpty()) device.connectPort(port, theme.orientation(), config["brightness"].toInt(75),
                                               config["mirror"].toBool());
        return;
    }
    sending = !sending;
    if (lightweight) setLightweight(true);
    if (sending) { sample = collector.collect(config["disk"].toString()); render(); }
    else emit updated();
}
bool DisplayRuntime::persist(QString *failure) {
    config["selected_theme"] = path;
    config["orientation"] = theme.orientation();
    auto directions = config["theme_orientations"].toObject();
    directions[themeOrientationKey(path)] = theme.orientation();
    config["theme_orientations"] = directions;
    QSaveFile file(dataDir + "/config.json");
    const auto bytes = QJsonDocument(config).toJson();
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    if (!ok && failure) *failure = file.errorString();
    return ok;
}
} // namespace Screen
