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
DisplayRuntime::DisplayRuntime(QString directory, const QString &initialTheme, QNetworkAccessManager *weatherNetwork)
    : dataDir(std::move(directory)), collector(QStringLiteral("/sys"), QStringLiteral("/proc"), weatherNetwork) {
    collector.weatherChanged = [this] {
        const auto state = collector.weatherStatus();
        if (collectionEnabled() && !state.busy && (state.updated > 0 || !state.error.isEmpty())) {
            collector.applyWeather(sample);
            if (preview || (sending && device.status().connected)) render();
            else emit updated();
        }
        emit weatherStatusChanged();
    };
    QDir().mkpath(dataDir + "/themes");
    QFile input(dataDir + "/config.json");
    if (input.open(QIODevice::ReadOnly)) config = QJsonDocument::fromJson(input.readAll()).object();
    library.initialize(dataDir, config);
    config.remove("palette"); // Retire the application palette preference from older versions.
    const auto selected = library.resolve(initialTheme.isEmpty() ? config["selected_theme"].toString() : initialTheme, dataDir);
    const auto relative = QDir(assets()).relativeFilePath(QFileInfo(selected).absoluteFilePath());
    const auto key = !relative.startsWith("../") && !QDir::isAbsolutePath(relative)
                         ? "builtin:" + relative : themeOrientationKey(selected);
    if (!selected.isEmpty() && (!initialTheme.isEmpty() || !config["hidden_themes"].toArray().contains(key))) {
        try {
            theme = Theme::load(selected);
            if (selected.endsWith(".data", Qt::CaseInsensitive)) theme.data["name"] = QFileInfo(selected).completeBaseName();
            path = selected;
        } catch (const std::exception &) {}
    }
    if (path.isEmpty()) {
        path = library.defaultThemeFile();
        theme = Theme::load(path);
    }
    config["selected_theme"] = path;
    auto direction = config["theme_orientations"].toObject().value(themeOrientationKey(path));
    if (direction.isUndefined() && initialTheme.isEmpty() && !theme.data.contains("orientation"))
        direction = config["orientation"];
    if (direction.isDouble() && direction.toInt(-1) >= 0 && direction.toInt(-1) <= 3 &&
        direction.toDouble() == direction.toInt()) theme.data["orientation"] = direction.toInt();
    configureWeather();
    configureSensors();
    connect(&sampling, &QTimer::timeout, this, [this] {
        if (preview || allMetrics || (sending && device.status().connected)) tick();
    });
    connect(&device, &Device::statusChanged, this, [this] {
        // Lightweight autostart starts sending before the asynchronous serial
        // connection completes, so wait for the first usable frame immediately.
        if (sending && device.status().connected) tick();
    }, Qt::QueuedConnection);
    sampling.setTimerType(Qt::CoarseTimer);
    sampling.setInterval(qBound(1000, config.value("interval_ms").toInt(1000), 10000));
    if (collectionEnabled()) {
        sampling.start();
        const auto requested = requestedMetrics();
        sample = collector.collect(config["disk"].toString(), &requested);
    }
}
QSet<QString> DisplayRuntime::requestedMetrics() const {
    QSet<QString> result;
    if (allMetrics && !lightweight) {
        for (const auto &metric : metrics()) result.insert(metric.first);
        return result;
    }
    std::function<void(const QJsonArray &)> visit = [&](const QJsonArray &layers) {
        for (const auto &value : layers) {
            const auto layer = value.toObject();
            if (!layer["visible"].toBool(true)) continue;
            const auto kind = layer["kind"].toString();
            if (kind == "sensor" || kind == "bar" || kind == "arc") {
                const auto metric = canonical(layer["metric"].toString());
                if (!metric.isEmpty()) result.insert(metric);
            }
            if (kind == "group") visit(layer["children"].toArray());
        }
    };
    visit(theme.layers());
    return result;
}
void DisplayRuntime::setCollectionEnabled(bool enabled) {
    if (enabled == collectionEnabled()) return;
    config["collect_data"] = enabled;
    if (enabled) sampling.start();
    else sampling.stop();
    configureSensors();
    configureWeather();
    collector.setAudioEnabled(enabled && !backgroundIdle);
    persist();
    if (enabled) tick();
    emit updated();
}
void DisplayRuntime::configureWeather(bool force) {
    const auto options = theme.data.value("weather").toObject();
    collector.setWeather(collectionEnabled() && !backgroundIdle && (force || weatherSettingsVisible || (allMetrics && !lightweight) || usesMetric(theme.layers(), "Weather")) &&
                         config["weather_enabled"].toBool(options["enabled"].toBool()),
                         config["weather_city"].toString(options["city"].toString()));
}
bool DisplayRuntime::requestWeather() {
    if (!collectionEnabled() || lightweight) return false;
    configureWeather(true);
    return collector.refreshWeather();
}
void DisplayRuntime::configureSensors() {
    const auto requested = requestedMetrics();
    bool hardware = false;
    for (const auto &metric : requested)
        hardware |= metric.startsWith("Gpu") || metric == "CpuFreq" || metric == "CpuTemp" ||
                    metric == "CpuPower" || metric == "CpuFan" || metric == "CpuPump" ||
                    metric == "CpuVoltage" || metric == "HDDTemp";
    collector.setWindowsSource(collectionEnabled() && !backgroundIdle && hardware ? config["windows_sensor_source"].toString("system") : QStringLiteral("system"),
                               config["lhm_port"].toInt(8085), config["windows_gpu_index"].toInt(0));
}
void DisplayRuntime::setLightweight(bool value) {
    lightweight = value;
    renderer.setStaticCaching(!value);
    backgroundIdle = value && !(sending && device.status().connected);
    collector.setAudioEnabled(collectionEnabled() && !backgroundIdle);
    configureSensors();
    configureWeather();
    if (backgroundIdle) {
        frame = {};
        if (collectionEnabled()) sample = {};
        renderer.clearCaches();
    }
}
void DisplayRuntime::startSending() {
    if (sending) return;
    auto port = config["port"].toString();
    if (port.isEmpty()) {
        // Without a saved choice, only connect when there is no ambiguity.
        const auto ports = Device::ports();
        if (ports.size() == 1) port = ports.first().name;
    }
    if (port.isEmpty()) return;
    sending = true;
    const auto status = device.status();
    if (!status.connected && !status.connecting)
        device.connectPort(port, theme.orientation(), config["brightness"].toInt(75),
                           config["mirror"].toBool());
    if (lightweight) setLightweight(true);
    if (status.connected) tick();
}
void DisplayRuntime::tick() {
    if (!collectionEnabled()) return;
    // A paused/disconnected background session has no consumer for samples.
    if (lightweight && !(sending && device.status().connected)) {
        if (!backgroundIdle) setLightweight(true);
        return;
    }
    if (backgroundIdle) setLightweight(lightweight);
    configureSensors();
    configureWeather();
    const auto requested = requestedMetrics();
    sample = collector.collect(config["disk"].toString(), &requested);
    if (preview || (sending && device.status().connected)) render();
    else emit updated();
}
void DisplayRuntime::render() {
    try {
        if (frame.isNull() || !renderer.unchanged(theme, sample)) {
            frame = renderer.render(theme, sample);
            ++renderedFrames;
        }
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
    if (sending) { if (collectionEnabled()) tick(); else render(); }
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
