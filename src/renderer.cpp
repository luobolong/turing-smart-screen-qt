#include "renderer.h"
#include <QBuffer>
#include <QFile>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonDocument>
#include <cmath>
#include <stdexcept>
namespace Screen {
QString screenFontFamily() {
    static QString family = [] {
        int id = QFontDatabase::addApplicationFont(assets() + "/fonts/TuringScreenSans.ttf");
        auto names = QFontDatabase::applicationFontFamilies(id);
        if (names.isEmpty())
            throw std::runtime_error("Missing application font");
        return names.first();
    }();
    return family;
}
static void blend(QImage &out, int x, int y, QColor c) {
    if (x < 0 || y < 0 || x >= out.width() || y >= out.height())
        return;
    auto *p = out.scanLine(y) + x * 4;
    quint32 a = c.alpha();
    if (p[3] == 0) {
        p[0] = uchar(c.red());
        p[1] = uchar(c.green());
        p[2] = uchar(c.blue());
        p[3] = uchar(a);
        return;
    }
    if (p[3] < 255) {
        quint32 d = p[3], combined = a * 255 + d * (255 - a);
        if (!combined)
            return;
        p[0] = uchar((quint32(c.red()) * a * 255 + p[0] * d * (255 - a)) / combined);
        p[1] = uchar((quint32(c.green()) * a * 255 + p[1] * d * (255 - a)) / combined);
        p[2] = uchar((quint32(c.blue()) * a * 255 + p[2] * d * (255 - a)) / combined);
        p[3] = uchar(combined / 255);
        return;
    }
    p[0] = uchar((c.red() * a + p[0] * (255 - a)) / 255);
    p[1] = uchar((c.green() * a + p[1] * (255 - a)) / 255);
    p[2] = uchar((c.blue() * a + p[2] * (255 - a)) / 255);
}
static void image(QImage &out, const QImage &source, QPoint at) {
    auto src =
        source.format() == QImage::Format_RGBA8888 ? source : source.convertToFormat(QImage::Format_RGBA8888);
    for (int y = qMax(0, -at.y()); y < qMin(src.height(), out.height() - at.y()); ++y) {
        const auto *row = src.constScanLine(y);
        for (int x = qMax(0, -at.x()); x < qMin(src.width(), out.width() - at.x()); ++x) {
            const auto *p = row + 4 * x;
            blend(out, at.x() + x, at.y() + y, QColor(p[0], p[1], p[2], p[3]));
        }
    }
}
static QByteArray bytes(const QJsonObject &l, const char *key) {
    return QByteArray::fromBase64(l[key].toString().toLatin1());
}
static QColor rgb565(const QByteArray &data, int offset, QColor fallback) {
    if (offset < 0 || offset + 1 >= data.size())
        return fallback;
    quint16 c = quint8(data[offset]) | (quint16(quint8(data[offset + 1])) << 8);
    return QColor((((c >> 11) & 31) << 3) | (((c >> 11) & 31) >> 2),
                  (((c >> 5) & 63) << 2) | (((c >> 5) & 63) >> 4), ((c & 31) << 3) | ((c & 31) >> 2));
}
Renderer::Renderer() {
    resourceTrace("renderer start");
    QFile f(assets() + "/fonts/geforce-metrics.json");
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing glyph metrics");
    for (auto v : QJsonDocument::fromJson(f.readAll()).array()) {
        auto m = v.toObject();
        int size = m["size"].toInt();
        glyphs.append({m,
                       QImage(assets() + QString("/fonts/geforce-%1-fixed.png").arg(size))
                           .convertToFormat(QImage::Format_Alpha8),
                       QImage(assets() + QString("/fonts/geforce-%1-natural.png").arg(size))
                           .convertToFormat(QImage::Format_Alpha8)});
    }
}
QImage Renderer::decode(const QString &data) {
    if (data.isEmpty())
        return {};
    if (auto c = images.object(data))
        return *c;
    auto b = QByteArray::fromBase64(data.toLatin1());
    QBuffer buffer(&b);
    buffer.open(QIODevice::ReadOnly);
    QImageReader r(&buffer);
    auto size = r.size();
    if (size.width() > 1536 || size.height() > 1536 || size.width() <= 0 || size.height() <= 0)
        throw std::runtime_error("Image exceeds 1536 x 1536");
    auto img = r.read().convertToFormat(QImage::Format_RGBA8888);
    if (img.isNull())
        throw std::runtime_error("Cannot decode theme image");
    int cost = int(img.sizeInBytes());
    if (cost <= images.maxCost())
        images.insert(data, new QImage(img), cost);
    return img;
}
QImage Renderer::render(const Theme &t, const Snapshot &s, bool example) {
    QImage out(t.size(), QImage::Format_RGBA8888);
    out.fill(QColor(13, 20, 29));
    auto bg = decode(t.data["background"].toString());
    if (!bg.isNull()) {
        if (bg.size() != out.size())
            bg = bg.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image(out, bg, {});
    }
    auto overlay =
        decode(t.data[t.data["use_text_background"].toBool() ? "text_background" : "overlay"].toString());
    if (!overlay.isNull())
        image(out, overlay, {});
    layers(out, t.layers(), s, {}, example);
    return out;
}
void Renderer::layers(QImage &out, const QJsonArray &a, const Snapshot &s, QPoint offset, bool example) {
    for (auto v : a) {
        auto l = v.toObject();
        if (!l["visible"].toBool(true))
            continue;
        auto at = offset + QPoint(l["x"].toInt(), l["y"].toInt());
        auto kind = l["kind"].toString();
        if (kind == "group") {
            auto bg = decode(l["background"].toString());
            if (!bg.isNull())
                image(out, bg, at);
            layers(out, l["children"].toArray(), s, at, example);
        } else if (kind == "bar")
            bar(out, l, at, s, example);
        else if (kind == "arc")
            arc(out, l, at, s, example);
        else if (kind == "text" || kind == "sensor") {
            auto key = l["metric"].toString();
            auto txt = kind == "text" ? l["text"].toString() : s.text(key, l["length"].toInt() > 0);
            if (example && kind == "sensor") {
                txt = canonical(key) == "Time"      ? QString("23:58")
                      : canonical(key) == "CpuFreq" ? QString("4.8")
                                                    : l["text"].toString().replace('x', '8');
                if (txt.isEmpty())
                    txt = s.text(key, l["length"].toInt() > 0);
            }
            int length = l["length"].toInt();
            if (!l["long_text"].toBool(true) && length > 0)
                txt = txt.rightJustified(length, ' ').right(length);
            text(out, txt, at, l["font"].toObject(), l["long_text"].toBool(true));
        } else if (kind == "image") {
            auto img = decode(l["data"].toString());
            if (!img.isNull()) {
                img = img.scaled(qBound(1, l["w"].toInt(100), 1536), qBound(1, l["h"].toInt(100), 1536),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                int opacity = l["opacity"].toInt(255);
                if (opacity < 255) {
                    img = img.convertToFormat(QImage::Format_RGBA8888);
                    for (int y = 0; y < img.height(); ++y) {
                        auto *p = img.scanLine(y);
                        for (int x = 0; x < img.width(); ++x)
                            p[4 * x + 3] = uchar(p[4 * x + 3] * opacity / 255);
                    }
                }
                image(out, img, at);
            }
        } else {
            QPainter p(&out);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(color(l["fill"], QColor("#28c9bf")));
            int sw = l["stroke_width"].toInt(1);
            p.setPen(sw > 0 ? QPen(color(l["stroke"]), sw) : QPen(Qt::NoPen));
            QRectF r(at, QSizeF(l["w"].toDouble(100), l["h"].toDouble(30)));
            if (kind == "ellipse")
                p.drawEllipse(r);
            else if (kind == "line")
                p.drawLine(r.topLeft(), r.bottomRight());
            else
                p.drawRect(r.adjusted(sw / 2., sw / 2., -sw / 2., -sw / 2.));
        }
    }
}
void Renderer::text(QImage &out, const QString &str, QPoint at, const QJsonObject &f, bool longText) {
    int px = f["size"].toInt(18);
    QColor tint = color(f["color"]);
    bool official = f["family"] == "GeForce" || f["height"].toInt() > 0;
    const Glyphs *g = nullptr;
    if (official)
        for (auto &v : glyphs)
            if (v.metrics["size"].toInt() == px) {
                g = &v;
                break;
            }
    double cursor = at.x();
    if (g) {
        bool fixed = !longText && f["width"].toInt() > 0 && f["height"].toInt() > 0;
        const auto &atlas = fixed ? g->fixed : g->natural;
        int w = fixed ? g->metrics["width"].toInt() : 64, h = fixed ? g->metrics["height"].toInt() : 64;
        auto advances = g->metrics["advances"].toArray();
        int ox = fixed ? f["offset_x"].toInt() - g->metrics["offset_x"].toInt() : 0,
            oy = fixed ? f["offset_y"].toInt() - g->metrics["offset_y"].toInt() : 0;
        for (auto ch : str.left(1024)) {
            int index = ch.unicode() - 32;
            if (index >= 0 && index < 95) {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        int tx = x + ox, ty = y + oy;
                        if (fixed &&
                            (tx < 0 || ty < 0 || tx >= f["width"].toInt() || ty >= f["height"].toInt()))
                            continue;
                        int sx = index * w + x;
                        if (sx >= atlas.width() || y >= atlas.height())
                            continue;
                        int a = atlas.constScanLine(y)[sx];
                        if (a) {
                            auto c = tint;
                            c.setAlpha(a * tint.alpha() / 255);
                            blend(out, int(std::round(cursor)) + tx, at.y() + ty, c);
                        }
                    }
                cursor += fixed ? f["spacing"].toDouble(w) : advances[index].toDouble();
            } else {
                auto nf = f;
                nf["family"] = "Noto Sans SC";
                nf["height"] = 0;
                text(out, QString(ch), QPoint(int(std::round(cursor)), at.y()), nf, true);
                cursor += px;
            }
        }
        return;
    }
    if (fontFamily.isEmpty())
        fontFamily = screenFontFamily();
    QPainter p(&out);
    p.setPen(tint);
    QFont font(fontFamily);
    font.setPixelSize(px);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::NoFontMerging | QFont::PreferAntialias));
    // Keep the common UI font small; load the complete CJK face only when a
    // user actually enters a character outside the shipped subset.
    QFontMetrics metrics(font);
    for (auto ch : str.left(1024)) {
        if (!metrics.inFont(ch)) {
            static QString completeFamily = [] {
                int id = QFontDatabase::addApplicationFont(assets() + "/fonts/NotoSansSC.ttf");
                return QFontDatabase::applicationFontFamilies(id).value(0, screenFontFamily());
            }();
            font.setFamily(completeFamily);
            break;
        }
    }
    p.setFont(font);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFontMetricsF fm(font);
    double spacing = f["spacing"].toDouble();
    if (spacing > 0) {
        for (auto c : str.left(1024)) {
            p.drawText(QPointF(cursor, at.y() + fm.ascent()), QString(c));
            cursor += spacing;
        }
    } else
        p.drawText(QPointF(at.x(), at.y() + fm.ascent()), str.left(1024));
}
void Renderer::bar(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example) {
    int w = qBound(1, l["w"].toInt(160), 1536), h = qBound(1, l["h"].toInt(10), 1536),
        step = qMax(1, l["step"].toInt(1));
    if (example) {
        QPainter p(&out);
        p.fillRect(QRect(at, QSize(w, 13)), Qt::blue);
        return;
    }
    int fill = int(std::round(w * s.percentage(l["metric"].toString()) / 100.)) / step * step;
    auto front = bytes(l, "front_pixels"), back = bytes(l, "back_pixels");
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) {
            int col = back.size() == h * 2 ? 0 : x;
            auto c = x < fill ? rgb565(front, (y * step + x % step) * 2, color(l["front"], QColor("#28c9bf")))
                              : rgb565(back, (col * h + y) * 2, color(l["back"], QColor("#253041")));
            blend(out, at.x() + x, at.y() + y, c);
        }
}
void Renderer::arc(QImage &out, const QJsonObject &l, QPoint at, const Snapshot &s, bool example) {
    double per = example ? 60. / 3.6 : s.percentage(l["metric"].toString());
    auto points = bytes(l, "points"), counts = bytes(l, "point_counts");
    QColor front = color(l["front"], QColor("#28c9bf")), back = color(l["back"], QColor("#253041"));
    if (!points.isEmpty() && counts.size() == 100) {
        int offset = 0;
        for (int n = 0; n < 100; ++n) {
            int end = qMin(offset + quint8(counts[n]), int(points.size()));
            for (int i = offset; i + 1 < end; i += 2)
                blend(out, at.x() + quint8(points[i]), at.y() + quint8(points[i + 1]),
                      n < per ? front : back);
            offset = end;
        }
        return;
    }
    double r = qMin(512., l["radius"].toDouble(42)), width = l["width"].toDouble(5),
           pad = std::ceil(width / 2. + 1), start = l["start_percent"].toDouble(-25) / 100.;
    int bound = int(std::ceil(2 * (r + pad)));
    for (int y = 0; y < bound; ++y)
        for (int x = 0; x < bound; ++x) {
            double fx = x - r - pad, fy = y - r - pad, d = std::hypot(fx, fy);
            if (d > r + width / 2 || d < r - width / 2)
                continue;
            double angle = std::atan2(fy, fx) / (2 * 3.141592653589793) + .25;
            double fraction = std::fmod(angle - start + 2., 1.);
            if (l["segmented"].toBool() && std::fmod(fraction * 100, 1.) > .65)
                continue;
            blend(out, at.x() + x, at.y() + y, fraction * 100 < per ? front : back);
        }
}
} // namespace Screen
