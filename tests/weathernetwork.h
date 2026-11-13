#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <cstring>

// Deterministic replies for the weather client and its settings UI; no Internet I/O.
class WeatherReply : public QNetworkReply {
    QByteArray body;
    qint64 offset = 0;
public:
    WeatherReply(const QNetworkRequest &request, QByteArray body, QObject *parent)
        : QNetworkReply(parent), body(std::move(body)) {
        setRequest(request); setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished()) return;
            if (this->body == "network-timeout") setError(TimeoutError, "Test connection timed out");
            setFinished(true); emit readyRead(); emit finished();
        });
    }
    void abort() override {
        if (!isFinished()) {
            setError(OperationCanceledError, "cancelled"); setFinished(true); emit finished();
        }
    }
    qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
    qint64 readData(char *data, qint64 maximum) override {
        const auto count = qMin(maximum, body.size() - offset);
        if (count <= 0) return -1;
        std::memcpy(data, body.constData() + offset, size_t(count)); offset += count;
        return count;
    }
};
class WeatherNetwork : public QNetworkAccessManager {
public:
    QList<QByteArray> responses;
    QList<QUrl> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request.url());
        return new WeatherReply(request, responses.isEmpty() ? QByteArray("{}") : responses.takeFirst(), this);
    }
};
