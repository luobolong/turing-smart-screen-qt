#pragma once
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
    DisplayRuntime(QString directory, const QString &initialTheme = {});
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
    quint64 renderedFrames = 0;
    void configureWeather();
    void setLightweight(bool value);
    void tick();
    void render();
    void toggleSending();
    bool persist(QString *error = nullptr);
signals:
    void updated();
private:
    bool backgroundIdle = false;
};
QString themeOrientationKey(const QString &file);
} // namespace Screen
