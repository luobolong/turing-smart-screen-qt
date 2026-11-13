#pragma once
#include "audio.h"
#include "weather.h"
#include "windowshardware.h"
#include "memoryinfo.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QSet>
namespace Screen {
struct Snapshot {
    QHash<QString, double> values;
    QHash<QString, QString> strings;
    QHash<QString, QString> sources, unavailable;
    QString timestamp;
    static Snapshot fromJson(const QJsonObject &json);
    QJsonObject json() const;
    QString text(const QString &metric, bool bare = false) const;
    double percentage(const QString &metric) const;
};
struct ProcessStats {
    quint64 resident = 0, peakResident = 0, privateBytes = 0;
    double cpuSeconds = 0;
    int cores = 1;
    QJsonObject json() const;
};
ProcessStats processStats();
void resourceTrace(const char *stage);
class Collector {
public:
    explicit Collector(const QString &sysfsRoot = QStringLiteral("/sys"),
                       const QString &procRoot = QStringLiteral("/proc"), QNetworkAccessManager *weatherNetwork = nullptr);
    // nullptr requests the complete diagnostic sensor set; an empty set does no I/O.
    Snapshot collect(const QString &disk = {}, const QSet<QString> *requested = nullptr);
    void setWeather(bool enabled, const QString &city = {});
    bool refreshWeather();
    WeatherStatus weatherStatus() const;
    void applyWeather(Snapshot &sample);
    std::function<void()> weatherChanged;
    void setAudioEnabled(bool enabled);
    void setWindowsSource(const QString &source, int port = 8085, int gpuIndex = 0);

private:
    QString sysfsRoot, procRoot;
    QNetworkAccessManager *weatherNetwork = nullptr;
    MemoryInfo memoryInfo;
    bool memoryInfoReady = false, audioEnabled = true, networkReady = false;
    QString cpuModel;
    Snapshot localIp;
    QElapsedTimer localIpAge;
    std::unique_ptr<AudioVolume> audio;
    std::unique_ptr<WeatherClient> weather;
    std::unique_ptr<WindowsHardware> windowsHardware;
    QHash<QString, double> previousEnergy;
    quint64 previousTotal = 0, previousIdle = 0, previousUp = 0, previousDown = 0;
    QElapsedTimer elapsed;
};
} // namespace Screen
