#pragma once
#include "model.h"
#include "sensors.h"
#include <QCache>
#include <QImage>
#include <QPainter>
namespace Screen {
QString screenFontFamily();
QString symbolFontFamily();
QString latinFontFamily();
QString monoFontFamily();
class Renderer {
public:
    Renderer();
    QImage render(const Theme &theme, const Snapshot &sample, bool example = false);
    QImage renderFull(const Theme &theme, const Snapshot &sample, bool example = false);
    void setStaticCaching(bool enabled);
    bool unchanged(const Theme &theme, const Snapshot &sample) const;
    qint64 staticCachedBytes() const { return base.sizeInBytes() + retained.sizeInBytes(); }
    QRect lastDirtyRect() const { return dirtyRect; }
    QImage decode(const QString &data);
    void clearCaches();
    int cachedBytes() const {
        return images.totalCost();
    }

private:
    struct PaintNode {
        QJsonObject layer;
        QPoint offset;
        QRect bounds;
        QString value;
        bool dynamic = false;
    };
    bool staticCaching = true;
    QJsonObject cachedTheme;
    QVector<PaintNode> nodes;
    QImage base, retained;
    int staticPrefix = 0;
    QRect paintClip, dirtyRect;
    void flatten(const QJsonArray &layers, QPoint offset = {});
    QRect nodeBounds(const PaintNode &node, const Snapshot &sample, QSize size);
    QImage background(const Theme &theme);
    struct Glyphs {
        QJsonObject metrics;
        QImage fixed, natural;
    };
    QVector<Glyphs> glyphs;
    QCache<QString, QImage> images{4 * 1024 * 1024};
    QString fontFamily;
    void loadGlyphMetrics();
    void layers(QImage &out, const QJsonArray &a, const Snapshot &s, QPoint offset, bool example);
    void text(QImage &out, const QString &text, QPoint at, const QJsonObject &font, bool longText);
    void bar(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example);
    void arc(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example);
};
} // namespace Screen
