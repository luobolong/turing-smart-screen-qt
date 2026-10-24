#include "weather.h"
#include "sensors.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QUrlQuery>
#include <cmath>
namespace Screen {
WeatherClient::WeatherClient(QNetworkAccessManager *manager, QObject *parent)
    : QObject(parent), network(manager ? manager : new QNetworkAccessManager(this)) {}
WeatherClient::~WeatherClient() {
    ++generation;
    if (active)
        active->abort();
}
void WeatherClient::configure(bool useWeather, const QString &newCity) {
    if (enabled == useWeather && city == newCity.trimmed())
        return;
    ++generation;
    if (active)
        active->abort();
    active = nullptr;
    enabled = useWeather;
    city = newCity.trimmed();
    located = busy = false;
    summary.clear();
    error.clear();
    nextAttempt = updated = 0;
}
QString WeatherClient::description(int code) {
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: case 66: case 67: return "Freezing rain";
    case 61: case 63: case 65: return "Rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: case 96: case 99: return "Thunderstorm";
    default: return "Unknown";
    }
}
QString WeatherClient::icon(int code) {
    char32_t glyph;
    switch (code) {
    case 0: glyph = 0xF0599; break; // Sunny
    case 1: case 2: glyph = 0xF0595; break; // Partly cloudy
    case 3: glyph = 0xF0590; break;
    case 45: case 48: glyph = 0xF0591; break;
    case 51: case 53: case 55: case 61: case 63: case 65: glyph = 0xF0597; break;
    case 56: case 57: case 66: case 67: glyph = 0xF067F; break;
    case 71: case 73: case 75: case 77: case 85: case 86: glyph = 0xF0598; break;
    case 80: case 81: case 82: glyph = 0xF0596; break;
    case 95: case 96: case 99: glyph = 0xF067E; break;
    default: glyph = 0xF0F2F; break;
    }
    return QString::fromUcs4(&glyph, 1);
}
void WeatherClient::fail(const QString &message) {
    busy = false;
    error = message;
    nextAttempt = QDateTime::currentMSecsSinceEpoch() + 60000;
}
void WeatherClient::fetch(const QUrl &url, std::function<void(const QJsonObject &)> success) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "TuringScreen/" SCREEN_VERSION);
    request.setTransferTimeout(8000);
    auto reply = network->get(request);
    active = reply;
    const auto requestGeneration = generation;
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > 65536)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, requestGeneration, success = std::move(success)] {
        reply->deleteLater();
        if (requestGeneration != generation)
            return;
        active = nullptr;
        if (reply->error() != QNetworkReply::NoError) {
            fail("Weather request failed: " + reply->errorString());
            return;
        }
        const auto bytes = reply->readAll();
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (bytes.size() > 65536 || parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail("Invalid weather response");
            return;
        }
        success(document.object());
    });
}
bool WeatherClient::coordinates(const QJsonObject &object) {
    if (!object["latitude"].isDouble() || !object["longitude"].isDouble())
        return false;
    latitude = object["latitude"].toDouble();
    longitude = object["longitude"].toDouble();
    return std::isfinite(latitude) && std::isfinite(longitude) && std::abs(latitude) <= 90 &&
           std::abs(longitude) <= 180;
}
void WeatherClient::refresh() {
    busy = true;
    if (located) {
        forecast();
    } else if (city.isEmpty()) {
        fetch(QUrl("https://ipwho.is/?fields=success,city,latitude,longitude"), [this](const QJsonObject &o) {
            if (!o["success"].toBool() || !coordinates(o)) {
                fail("IP location unavailable; enter a city in Settings");
                return;
            }
            location = o["city"].toString();
            located = true;
            forecast();
        });
    } else {
        QUrl url("https://geocoding-api.open-meteo.com/v1/search");
        QUrlQuery query;
        query.addQueryItem("name", city);
        query.addQueryItem("count", "1");
        query.addQueryItem("language", "en");
        url.setQuery(query);
        fetch(url, [this](const QJsonObject &o) {
            const auto results = o["results"].toArray();
            const auto result = results.isEmpty() ? QJsonObject() : results.first().toObject();
            if (!coordinates(result)) {
                fail("City not found; include country or region in Settings");
                return;
            }
            location = result["name"].toString(city);
            located = true;
            forecast();
        });
    }
}
void WeatherClient::forecast() {
    QUrl url("https://api.open-meteo.com/v1/forecast");
    QUrlQuery query;
    query.addQueryItem("latitude", QString::number(latitude, 'f', 5));
    query.addQueryItem("longitude", QString::number(longitude, 'f', 5));
    query.addQueryItem("current", "temperature_2m,weather_code");
    query.addQueryItem("timezone", "auto");
    url.setQuery(query);
    fetch(url, [this](const QJsonObject &o) {
        const auto current = o["current"].toObject();
        const auto reading = current["temperature_2m"];
        const auto code = current["weather_code"];
        if (!reading.isDouble() || !std::isfinite(reading.toDouble()) || !code.isDouble()) {
            fail("Weather response has no current conditions");
            return;
        }
        temperature = reading.toDouble();
        conditions = description(code.toInt());
        conditionIcon = icon(code.toInt());
        summary = location + ' ' + conditions + ' ' + QString::number(temperature, 'f', 1) + QStringLiteral("°C");
        updated = QDateTime::currentMSecsSinceEpoch();
        nextAttempt = updated + 600000;
        error.clear();
        busy = false;
    });
}
void WeatherClient::apply(Snapshot &s) {
    if (!enabled) {
        for (const auto &key : {"Weather", "WeatherCity", "WeatherTemp", "WeatherCondition", "WeatherIcon"})
            s.unavailable[key] = "Weather disabled; enable it in Settings";
        return;
    }
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!busy && now >= nextAttempt)
        refresh();
    if (!summary.isEmpty() && now - updated <= 1800000) {
        s.strings["Weather"] = summary;
        s.strings["WeatherCity"] = location;
        s.strings["WeatherCondition"] = conditions;
        s.strings["WeatherIcon"] = conditionIcon;
        s.values["WeatherTemp"] = temperature;
        for (const auto &key : {"Weather", "WeatherCity", "WeatherTemp", "WeatherCondition", "WeatherIcon"})
            s.sources[key] = "Open-Meteo (CC BY 4.0)";
    } else {
        for (const auto &key : {"Weather", "WeatherCity", "WeatherTemp", "WeatherCondition", "WeatherIcon"})
            s.unavailable[key] = error.isEmpty() ? "Weather loading" : error;
    }
}
} // namespace Screen
