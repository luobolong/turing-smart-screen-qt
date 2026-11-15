#include "framediff.h"
#include "device.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace Screen {
namespace {
struct Damage {
    QRect rect;
    qint64 since = 0;
    int bytes() const { return rect.width() * rect.height() * 2; }
};
} // namespace

void FrameDiff::invalidate() {
    shown = {};
    known.clear();
    since.clear();
}

FrameDiff::Result FrameDiff::send(const QImage &next, qint64 now, bool v2, const Writer &write, int budget) {
    if (next.isNull() || next.format() != QImage::Format_RGB16)
        throw std::runtime_error("Expected an RGB565 frame");
    const int w = next.width(), h = next.height();
    const int columns = (w + TileWidth - 1) / TileWidth;
    const int bands = (h + TileHeight - 1) / TileHeight;
    // V1 needs 10 ms after the header and another 10 ms after the payload.
    const int overhead = 6 + (v2 ? 0 : Bandwidth * 20 / 1000);
    if (budget < overhead + w * 2)
        throw std::runtime_error("Refresh budget must fit one screen row and command overhead");
    if (shown.size() != next.size()) {
        shown = QImage(next.size(), QImage::Format_RGB16);
        shown.fill(0);
        known.fill(false, w * h);
        since.fill(-1, columns * bands);
    }

    QList<Damage> rects;
    std::map<std::pair<int, int>, qsizetype> open;
    for (int ty = 0; ty < bands; ++ty) {
        QList<Damage> tiles;
        for (int tx = 0; tx < columns; ++tx) {
            const int x0 = tx * TileWidth, y0 = ty * TileHeight;
            const int x1 = qMin(x0 + TileWidth, w), y1 = qMin(y0 + TileHeight, h);
            int left = x1, top = y1, right = -1, bottom = -1;
            for (int y = y0; y < y1; ++y) {
                const auto *a = reinterpret_cast<const quint16 *>(next.constScanLine(y));
                const auto *b = reinterpret_cast<const quint16 *>(shown.constScanLine(y));
                for (int x = x0; x < x1; ++x) {
                    if (known.testBit(y * w + x) && a[x] == b[x])
                        continue;
                    left = qMin(left, x); right = qMax(right, x);
                    top = qMin(top, y); bottom = y;
                }
            }
            const int i = ty * columns + tx;
            if (right < left) {
                since[i] = -1;
                tiles.append(Damage{});
            } else {
                if (since[i] < 0) since[i] = now;
                tiles.append({QRect(QPoint(left, top), QPoint(right, bottom)), since[i]});
            }
        }
        // Merge horizontal tile runs, extending runs with the same columns in the band above.
        // Uniting exact per-tile damage bounds also trims unchanged edges of every rectangle.
        std::map<std::pair<int, int>, qsizetype> continued;
        for (int tx = 0; tx < columns;) {
            if (tiles[tx].rect.isEmpty()) { ++tx; continue; }
            const int start = tx;
            Damage run = tiles[tx++];
            while (tx < columns && !tiles[tx].rect.isEmpty()) {
                run.rect = run.rect.united(tiles[tx].rect);
                run.since = qMin(run.since, tiles[tx++].since);
            }
            const auto key = std::make_pair(start, tx);
            const auto found = open.find(key);
            if (found != open.end()) {
                auto &rect = rects[found->second];
                rect.rect = rect.rect.united(run.rect);
                rect.since = qMin(rect.since, run.since);
                continued[key] = found->second;
            } else {
                continued[key] = rects.size();
                rects.append(run);
            }
        }
        open = std::move(continued);
    }
    Result result;
    if (rects.isEmpty()) return result;

    // Compare wire bytes plus firmware delays, including the extra commands caused by slicing.
    const auto cost = [=](const QRect &r) {
        const int rows = (budget - overhead) / (r.width() * 2);
        return r.width() * r.height() * 2 + ((r.height() + rows - 1) / rows) * overhead;
    };
    Damage single = rects.front();
    qint64 multipleCost = 0;
    for (const auto &r : rects) {
        single.rect = single.rect.united(r.rect);
        single.since = qMin(single.since, r.since);
        multipleCost += cost(r.rect);
    }
    // A single bound becomes a full-screen refresh when damage reaches every edge.
    if (cost(single.rect) <= multipleCost)
        rects = {single};
    std::stable_sort(rects.begin(), rects.end(), [=](const Damage &a, const Damage &b) {
        return (now - a.since) * Bandwidth - qint64(a.bytes()) * 1000 >
               (now - b.since) * Bandwidth - qint64(b.bytes()) * 1000;
    });

    int remaining = budget;
    for (auto r : rects) {
        const int rows = (remaining - overhead) / (r.rect.width() * 2);
        if (rows <= 0) { result.pending = true; break; }
        const bool split = rows < r.rect.height();
        r.rect.setHeight(qMin(rows, r.rect.height()));
        const auto packet = Protocol::bitmap(next.copy(r.rect), r.rect.topLeft());
        if (!write(r.rect, packet)) { result.pending = true; break; }
        // Commit only after the complete header/payload succeeded. Unknown pixels are
        // tracked individually so arbitrary row slices (including partial tiles) converge.
        for (int y = r.rect.top(); y <= r.rect.bottom(); ++y) {
            std::memcpy(shown.scanLine(y) + r.rect.x() * 2,
                        next.constScanLine(y) + r.rect.x() * 2, size_t(r.rect.width()) * 2);
            for (int x = r.rect.left(); x <= r.rect.right(); ++x)
                known.setBit(y * w + x);
        }
        ++result.rectangles;
        result.bytes += packet.size();
        const int used = int(packet.size()) + overhead - 6;
        remaining -= used;
        result.cost += used;
        if (split) { result.pending = true; break; }
    }
    return result;
}
} // namespace Screen
