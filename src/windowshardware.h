#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QVector>
namespace Screen {
struct Snapshot;
struct HardwareReading {
    QString id, device, category, name, kind, unit;
    double value = 0;
};
struct HardwareReadings {
    QVector<HardwareReading> readings;
    QString error;
    qint64 pollTime = 0;
};
// Parsers are platform independent so the published wire formats can be tested
// without installing a monitor or accessing a real driver.
HardwareReadings parseHwinfo(const QByteArray &bytes, qint64 now = 0);
HardwareReadings parseAida64(const QByteArray &bytes);
HardwareReadings parseLibreHardwareMonitor(const QByteArray &bytes);
void applyHardwareReadings(Snapshot &sample, const HardwareReadings &data,
                           const QString &source, int gpuIndex = 0);
QString windowsSensorSource(QString source);
class WindowsHardware : public QObject {
public:
    explicit WindowsHardware(QNetworkAccessManager *network = nullptr);
    ~WindowsHardware() override;
    void configure(const QString &source, int port = 8085, int gpuIndex = 0);
    void apply(Snapshot &sample);
private:
    QString source = "system";
    int port = 8085, gpuIndex = 0;
    QNetworkAccessManager *network = nullptr;
    QPointer<QNetworkReply> active;
    HardwareReadings cached;
    QElapsedTimer clock;
    qint64 requested = -1000, received = -1;
    quint64 generation = 0;
    void fetchLibre();
};
} // namespace Screen
