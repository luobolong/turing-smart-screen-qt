#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QStringList>
namespace Screen {
struct Snapshot {
    QHash<QString, double> values;
    QHash<QString, QString> strings;
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
    Collector();
    Snapshot collect(const QString &disk = {});

private:
    quint64 previousTotal = 0, previousIdle = 0, previousUp = 0, previousDown = 0;
    QElapsedTimer elapsed;
};
} // namespace Screen
