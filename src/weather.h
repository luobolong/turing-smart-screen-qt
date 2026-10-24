#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <functional>
namespace Screen {
struct Snapshot;
class WeatherClient : public QObject {
public:
    explicit WeatherClient(QNetworkAccessManager *network = nullptr, QObject *parent = nullptr);
    ~WeatherClient() override;
    void configure(bool enabled, const QString &city);
    void apply(Snapshot &sample);
    static QString description(int code);
    static QString icon(int code);

private:
    QNetworkAccessManager *network;
    QPointer<QNetworkReply> active;
    bool enabled = false, busy = false, located = false;
    QString city, location, summary, conditions, conditionIcon, error;
    double temperature = 0;
    double latitude = 0, longitude = 0;
    qint64 nextAttempt = 0, updated = 0;
    quint64 generation = 0;
    void refresh();
    void forecast();
    void fail(const QString &message);
    void fetch(const QUrl &url, std::function<void(const QJsonObject &)> success);
    bool coordinates(const QJsonObject &object);
};
} // namespace Screen
