#pragma once
#include "themelibrary.h"
#include "device.h"
#include "renderer.h"
#include "sensors.h"
#include <QObject>
#include <QTimer>
namespace Screen {
// Owns the display pipeline independently of the editor window's lifetime.
class DisplayRuntime : public QObject {
    Q_OBJECT
public:
    ThemeLibrary library;
    DisplayRuntime(QString directory, const QString &initialTheme = {}, QNetworkAccessManager *weatherNetwork = nullptr);
    QString dataDir, path, error;
    QJsonObject config;
    Theme theme;
    Snapshot sample;
    Renderer renderer;
    Collector collector;
    Device device;
    QImage frame;
    QTimer sampling;
    bool sending = false, preview = false, lightweight = false;
    bool allMetrics = false;
    bool weatherSettingsVisible = false;
    quint64 renderedFrames = 0;
    void configureWeather(bool force = false);
    bool requestWeather();
    void configureSensors();
    bool collectionEnabled() const { return config.value("collect_data").toBool(true); }
    void setCollectionEnabled(bool enabled);
    QSet<QString> requestedMetrics() const;
    void setLightweight(bool value);
    void startSending();
    void tick();
    void render();
    void toggleSending();
    bool persist(QString *error = nullptr);
signals:
    void updated();
    void weatherStatusChanged();
private:
    bool backgroundIdle = false;
};
QString themeOrientationKey(const QString &file);
} // namespace Screen
