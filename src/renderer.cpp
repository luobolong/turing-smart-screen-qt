#include "renderer.h"
#include <QBuffer>
#include <QFile>
#include <QFontDatabase>
#include <QImageReader>
#include <QJsonDocument>
#include <cmath>
#include <cstring>
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
QString symbolFontFamily() {
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(assets() + "/fonts/TuringScreenSymbols.ttf");
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (names.isEmpty())
            throw std::runtime_error("Missing theme symbol font");
        return names.first();
    }();
    return family;
}
QString latinFontFamily() {
    static const QString family = [] {
        QString result;
        for (const auto &style : {"Regular", "SemiBold"}) {
            const auto file = assets() + "/fonts/TuringScreenLatin-" + style + ".ttf";
            const auto id = QFontDatabase::addApplicationFont(file);
            const auto names = QFontDatabase::applicationFontFamilies(id);
            if (names.isEmpty())
                throw std::runtime_error("Missing dashboard text font");
            result = names.first();
        }
        return result;
    }();
    return family;
}
QString monoFontFamily() {
    static const QString family = [] {
        const int id = QFontDatabase::addApplicationFont(assets() + "/fonts/TuringScreenMono.ttf");
        const auto names = QFontDatabase::applicationFontFamilies(id);
        if (names.isEmpty())
            throw std::runtime_error("Missing dashboard monospace font");
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
    const auto src = source.format() == QImage::Format_RGBA8888
                         ? source : source.convertToFormat(QImage::Format_RGBA8888);
    const int x0 = qMax(0, -at.x()), y0 = qMax(0, -at.y());
    const int x1 = qMin(src.width(), out.width() - at.x()), y1 = qMin(src.height(), out.height() - at.y());
    if (x0 >= x1 || y0 >= y1) return;
    const auto *input = src.constBits();
    auto *output = out.bits();
    const auto inputStride = src.bytesPerLine(), outputStride = out.bytesPerLine();
    for (int y = y0; y < y1; ++y) {
        const auto *pixel = input + y * inputStride + x0 * 4;
        auto *dest = output + (y + at.y()) * outputStride + (x0 + at.x()) * 4;
        for (int x = x0; x < x1; ++x, pixel += 4, dest += 4) {
            const quint32 a = pixel[3], d = dest[3];
            if (a == 255 || d == 0) {
                std::memcpy(dest, pixel, 4);
            } else if (d < 255) {
                const quint32 combined = a * 255 + d * (255 - a);
                for (int channel = 0; channel < 3; ++channel)
                    dest[channel] = uchar((pixel[channel] * a * 255 + dest[channel] * d * (255 - a)) / combined);
                dest[3] = uchar(combined / 255);
            } else {
                for (int channel = 0; channel < 3; ++channel)
                    dest[channel] = uchar((pixel[channel] * a + dest[channel] * (255 - a)) / 255);
            }
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
}
void Renderer::clearCaches() {
    images.clear();
    glyphs.clear();
    fontFamily.clear();
}
void Renderer::loadGlyphMetrics() {
    if (!glyphs.isEmpty()) return;
    QFile f(assets() + "/fonts/geforce-metrics.json");
    if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing glyph metrics");
    for (const auto &value : QJsonDocument::fromJson(f.readAll()).array())
        glyphs.append({value.toObject(), {}, {}});
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
    out.fill(color(t.data["background_color"], QColor(13, 20, 29)));
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
static QString sensorText(const QJsonObject &layer, const Snapshot &sample) {
    const auto key = layer["metric"].toString();
    const auto format = layer["value_format"].toObject();
    const auto value = sample.values.constFind(canonical(key));
    if (format.isEmpty() || value == sample.values.cend())
        return sample.text(key, layer["length"].toInt() > 0);
    double number = *value;
    auto suffix = format["suffix"].toString();
    if (format["auto_unit"] == "bytes/s") {
        const QStringList units{"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
        int unit = 0;
        while (std::abs(number) >= 1024 && unit + 1 < units.size()) {
            number /= 1024.;
            ++unit;
        }
        suffix = ' ' + units[unit];
    } else {
        const auto divisor = format["divisor"].toDouble(1.);
        if (!std::isfinite(divisor) || divisor <= 0)
            return "--";
        number /= divisor;
    }
    if (!std::isfinite(number))
        return "--";
    return QString::number(number, 'f', qBound(0, format["decimals"].toInt(), 6)) + suffix;
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
            auto txt = kind == "text" ? l["text"].toString() : sensorText(l, s);
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
            auto font = l["font"].toObject();
            if (kind == "sensor" && l.contains("missing_text") &&
                !s.values.contains(canonical(key)) && !s.strings.contains(canonical(key)))
                txt = l["missing_text"].toString();
            if (l.contains("align") || l.contains("valign") || l.contains("baseline") || l["fit"].toBool() ||
                l["shrink_to_fit"].toBool() ||
                l["clip"].toBool() || l["elide"].toBool() || l["wrap"].toBool()) {
                font["_box_width"] = l["w"];
                font["_box_height"] = l["h"];
                font["_align"] = l["align"];
                font["_elide"] = l["elide"];
                font["_wrap"] = l["wrap"];
                font["_valign"] = l["valign"];
                font["_fit"] = l["fit"];
                font["_baseline"] = l["baseline"];
                font["_shrink_to_fit"] = l["shrink_to_fit"];
            }
            text(out, txt, at, font, l["long_text"].toBool(true));
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
            else if (kind == "rectangle" && l["corner_radius"].toDouble() > 0)
                p.drawRoundedRect(r.adjusted(sw / 2., sw / 2., -sw / 2., -sw / 2.),
                                  l["corner_radius"].toDouble(), l["corner_radius"].toDouble());
            else
                p.drawRect(r.adjusted(sw / 2., sw / 2., -sw / 2., -sw / 2.));
        }
    }
}
void Renderer::text(QImage &out, const QString &str, QPoint at, const QJsonObject &f, bool longText) {
    int px = f["size"].toInt(18);
    QColor tint = color(f["color"]);
    bool official = f["family"] == "GeForce" || f["height"].toInt() > 0;
    Glyphs *g = nullptr;
    if (official) loadGlyphMetrics();
    if (official)
        for (auto &v : glyphs)
            if (v.metrics["size"].toInt() == px) {
                g = &v;
                break;
            }
    double cursor = at.x();
    if (g) {
        bool fixed = !longText && f["width"].toInt() > 0 && f["height"].toInt() > 0;
        auto &atlas = fixed ? g->fixed : g->natural;
        if (atlas.isNull())
            atlas = QImage(assets() + QString("/fonts/geforce-%1-%2.png").arg(px).arg(fixed ? "fixed" : "natural"))
                        .convertToFormat(QImage::Format_Alpha8);
        if (atlas.isNull()) throw std::runtime_error("Missing glyph atlas");
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
    const bool symbols = f["family"] == "Turing Screen Symbols";
    QFont font(symbols ? symbolFontFamily()
                      : f["family"] == "Turing Screen Latin" ? latinFontFamily()
                      : f["family"] == "Turing Screen Mono" ? monoFontFamily() : fontFamily);
    font.setPixelSize(px);
    font.setBold(f["bold"].toBool());
    font.setStyleStrategy(QFont::StyleStrategy(QFont::NoFontMerging | QFont::PreferAntialias));
    // Keep the common UI font small; load the complete CJK face only when a
    // user actually enters a character outside the shipped subset.
    QFontMetrics metrics(font);
    for (auto ch : str.left(1024)) {
        if (!symbols && !metrics.inFont(ch)) {
            static QString completeFamily = [] {
                int id = QFontDatabase::addApplicationFont(assets() + "/fonts/NotoSansSC.ttf");
                return QFontDatabase::applicationFontFamilies(id).value(0, screenFontFamily());
            }();
            font.setFamily(completeFamily);
            break;
        }
    }
    const QFontMetricsF lineMetrics(font);
    const int boxWidth = f["_box_width"].toInt();
    if (boxWidth > 0 && f["_shrink_to_fit"].toBool()) {
        // Keep the configured baseline while fitting long system names into
        // their column; surrounding rows and the clock must not move.
        for (int size = px; size > 6; --size) {
            if (QFontMetricsF(font).horizontalAdvance(str.left(1024)) <= boxWidth)
                break;
            font.setPixelSize(size - 1);
        }
    }
    p.setFont(font);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFontMetricsF fm(font);
    QString display = str.left(1024);
    const bool explicitBaseline = f["_baseline"].isDouble();
    double baseline = at.y() + (explicitBaseline ? f["_baseline"].toDouble() : lineMetrics.ascent());
    if (boxWidth > 0) {
        const int boxHeight = qMax(1, f["_box_height"].toInt());
        p.setClipRect(QRect(at, QSize(boxWidth, boxHeight)));
        if (f["_fit"].toBool() && !display.isEmpty()) {
            // Nerd Font glyphs can extend beyond the font's ascent and advance.
            // Place the complete visible glyph inside its box before clipping.
            const auto ink = fm.tightBoundingRect(display);
            if (ink.isEmpty())
                return;
            const double scale = qMin(1., qMin(boxWidth / ink.width(), boxHeight / ink.height()));
            const auto alignment = f["_align"].toString();
            const auto vertical = f["_valign"].toString();
            double x = at.x(), y = at.y();
            if (alignment == "right")
                x += boxWidth - ink.width() * scale;
            else if (alignment == "center")
                x += (boxWidth - ink.width() * scale) / 2.;
            if (vertical == "bottom")
                y += boxHeight - ink.height() * scale;
            else if (vertical == "center")
                y += (boxHeight - ink.height() * scale) / 2.;
            p.translate(x - ink.left() * scale, y - ink.top() * scale);
            p.scale(scale, scale);
            p.drawText(QPointF(), display);
            return;
        }
        if (f["_wrap"].toBool()) {
            const auto alignment = f["_align"].toString();
            const int flags = Qt::TextWordWrap | (alignment == "right" ? Qt::AlignRight
                                                : alignment == "center" ? Qt::AlignHCenter : Qt::AlignLeft);
            p.drawText(QRectF(at, QSizeF(boxWidth, f["_box_height"].toInt())), flags, display);
            return;
        }
        if (f["_elide"].toBool())
            display = fm.elidedText(display, Qt::ElideRight, boxWidth);
        // Text alignment follows the font's line metrics, not the current
        // string's ink bounds: descenders and changing digits must not move it.
        if (!explicitBaseline) {
            if (f["_valign"] == "bottom")
                baseline = at.y() + boxHeight - lineMetrics.descent();
            else if (f["_valign"] == "center")
                baseline = at.y() + (boxHeight - lineMetrics.height()) / 2. + lineMetrics.ascent();
        }
        const auto alignment = f["_align"].toString();
        if (alignment == "right")
            cursor += boxWidth - fm.horizontalAdvance(display);
        else if (alignment == "center")
            cursor += (boxWidth - fm.horizontalAdvance(display)) / 2.;
    }
    double spacing = f["spacing"].toDouble();
    if (spacing > 0) {
        for (auto c : display) {
            p.drawText(QPointF(cursor, baseline), QString(c));
            cursor += spacing;
        }
    } else
        p.drawText(QPointF(cursor, baseline), display);
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
    if (front.isEmpty() && back.isEmpty() && l["corner_radius"].toDouble() > 0) {
        QPainter painter(&out);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const double radius = qMin(l["corner_radius"].toDouble(), h / 2.);
        painter.setBrush(color(l["back"], QColor("#253041")));
        painter.drawRoundedRect(QRectF(at, QSizeF(w, h)), radius, radius);
        if (fill > 0) {
            painter.setBrush(color(l["front"], QColor("#28c9bf")));
            painter.drawRoundedRect(QRectF(at, QSizeF(qMin(fill, w), h)), radius, radius);
        }
        return;
    }
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
