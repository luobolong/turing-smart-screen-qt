#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QRectF>
#include <QStringList>
#include <functional>
namespace Screen {
QString assets();
QJsonArray rgba(QColor c);
QColor color(QJsonValue value, QColor fallback = Qt::white);
QString uid();
struct Theme {
    QJsonObject data;
    static Theme load(const QString &path);
    static Theme fromJson(const QJsonObject &json);
    static Theme fromOfficial(const QByteArray &bytes);
    static Theme starter();
    QString name() const {
        return data.value("name").toString();
    }
    QSize size() const {
        return data.value("landscape").toBool(true) ? QSize(480, 320) : QSize(320, 480);
    }
    int orientation() const {
        return data.value("orientation").toInt(data.value("landscape").toBool(true) ? 2 : 0);
    }
    QJsonArray layers() const {
        return data.value("layers").toArray();
    }
    void save(const QString &path) const;
    void validate() const;
};
QJsonObject findLayer(const QJsonArray &layers, const QString &id);
bool updateLayer(QJsonArray &layers, const QString &id, const std::function<void(QJsonObject &)> &fn);
QJsonArray takeLayers(QJsonArray &layers, const QStringList &ids);
bool insertLayers(QJsonArray &layers, const QString &parent, const QJsonArray &items, int index = -1);
QString parentOf(const QJsonArray &layers, const QString &id);
QStringList normalizedSelection(const QJsonArray &layers, const QStringList &ids);
QPointF absolutePosition(const QJsonArray &layers, const QString &id, QPointF offset = {});
QRectF layerBounds(const QJsonObject &layer);
bool layerLocked(const QJsonArray &layers, const QString &id, bool inherited = false);
QJsonObject newLayer(const QString &kind, const QString &name, const QString &metric = {});
QJsonObject component(const QString &kind);
void regenerateIds(QJsonObject &layer);
QString canonical(QString key);
const QVector<QPair<QString, QString>> &metrics();
} // namespace Screen
