#pragma once
#include "model.h"
#include "sensors.h"
#include <QCache>
#include <QImage>
#include <QPainter>
namespace Screen {
QString screenFontFamily();
class Renderer {
public:
    Renderer();
    QImage render(const Theme &theme, const Snapshot &sample, bool example = false);
    QImage decode(const QString &data);
    int cachedBytes() const {
        return images.totalCost();
    }

private:
    struct Glyphs {
        QJsonObject metrics;
        QImage fixed, natural;
    };
    QVector<Glyphs> glyphs;
    QCache<QString, QImage> images{4 * 1024 * 1024};
    QString fontFamily;
    void layers(QImage &out, const QJsonArray &a, const Snapshot &s, QPoint offset, bool example);
    void text(QImage &out, const QString &text, QPoint at, const QJsonObject &font, bool longText);
    void bar(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example);
    void arc(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example);
};
} // namespace Screen
