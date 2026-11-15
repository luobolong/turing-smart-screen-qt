#pragma once
#include <QBitArray>
#include <QImage>
#include <QList>
#include <functional>

namespace Screen {
// RGB565 damage tracking, inspired by Ember's FrameDiff (see THIRD_PARTY.md).
class FrameDiff {
public:
    static constexpr int TileWidth = 16, TileHeight = 8;
    static constexpr int Bandwidth = 157000, SliceMs = 50;
    struct Result {
        qint64 bytes = 0;
        int rectangles = 0, cost = 0;
        bool pending = false;
    };
    // Return false to yield to a newer frame/control command before starting a rectangle.
    using Writer = std::function<bool(const QRect &, const QByteArray &)>;
    // Budget/cost use equivalent wire bytes: payload + headers + firmware wait time * Bandwidth.
    Result send(const QImage &rgb565, qint64 nowMs, bool v2, const Writer &write,
                int budget = Bandwidth * SliceMs / 1000);
    void invalidate();

private:
    QImage shown;
    QBitArray known;
    QList<qint64> since;
};
} // namespace Screen
