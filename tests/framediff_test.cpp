#include "device.h"
#include "framediff.h"
#include "pseudoscreen.h"
#include <QtEndian>
#include <QtTest>
#include <QPainter>
#include <random>
using namespace Screen;

namespace {
QImage black(QSize size = {480, 320}) {
    QImage image(size, QImage::Format_RGB16);
    image.fill(Qt::black);
    return image;
}
struct Panel {
    QImage image;
    QList<QRect> rects;
    bool write(const QRect &rect, const QByteArray &packet) {
        if (!image.rect().contains(rect) || packet.size() != 6 + rect.width() * rect.height() * 2 ||
            packet.left(6) != Protocol::header(rect.x(), rect.y(), rect.right(), rect.bottom(), 0xc5))
            throw std::runtime_error("Invalid bitmap packet");
        rects.append(rect);
        int offset = 6;
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            auto *row = reinterpret_cast<quint16 *>(image.scanLine(y));
            for (int x = rect.left(); x <= rect.right(); ++x, offset += 2)
                row[x] = qFromLittleEndian<quint16>(packet.constData() + offset);
        }
        return true;
    }
    FrameDiff::Writer writer() {
        return [this](const QRect &r, const QByteArray &p) { return write(r, p); };
    }
};
int drain(FrameDiff &diff, const QImage &frame, Panel &panel, bool v2 = true, int budget = 7850) {
    for (int i = 0; i < 1000; ++i) {
        auto result = diff.send(frame, i * 50, v2, panel.writer(), budget);
        if (result.cost > budget || (result.pending && result.rectangles == 0)) return -1;
        if (!result.pending) return i + 1;
    }
    return -1;
}
} // namespace

class RefreshTest : public QObject {
    Q_OBJECT
private slots:
    void quantizedPixelsDoNotCauseTraffic() {
        FrameDiff diff;
        auto frame = black({35, 19}); // RGB16 scan lines have padding at odd widths.
        Panel panel{frame};
        QVERIFY(drain(diff, frame, panel) > 0);
        QImage rgba(frame.size(), QImage::Format_RGBA8888);
        rgba.fill(QColor(7, 3, 7, 1));
        QCOMPARE(Protocol::rgb565(rgba), frame);
        auto result = diff.send(Protocol::rgb565(rgba), 100, true, panel.writer());
        QCOMPARE(result.bytes, 0);
        QVERIFY(!result.pending);
        rgba.setPixelColor(34, 18, QColor(8, 4, 8, 99));
        result = diff.send(Protocol::rgb565(rgba), 150, true, panel.writer());
        QCOMPARE(result.bytes, 8);
        QCOMPARE(panel.rects.last(), QRect(34, 18, 1, 1));
        QCOMPARE(panel.image, Protocol::rgb565(rgba));
    }
    void tileRunsMergeAndTrimEdges() {
        auto frame = black();
        FrameDiff diff;
        Panel panel{frame};
        QVERIFY(drain(diff, frame, panel) > 0);
        panel.rects.clear();
        for (int y : {6, 10}) for (int x : {7, 24}) frame.setPixelColor(x, y, Qt::red);
        frame.setPixelColor(470, 310, Qt::green);
        auto result = diff.send(frame, 3000, true, panel.writer());
        QCOMPARE(result.rectangles, 2);
        QVERIFY(panel.rects.contains(QRect(7, 6, 18, 5)));
        QVERIFY(panel.rects.contains(QRect(470, 310, 1, 1)));
        QCOMPARE(result.bytes, 18 * 5 * 2 + 2 + 12);
        QCOMPARE(panel.image, frame);
    }
    void firmwareCostChoosesSingleOrMultipleRects() {
        for (bool v2 : {false, true}) {
            auto frame = black();
            FrameDiff diff;
            Panel panel{frame};
            QVERIFY(drain(diff, frame, panel, v2) > 0);
            panel.rects.clear();
            frame.setPixelColor(1, 1, Qt::red);
            frame.setPixelColor(40, 1, Qt::red);
            auto result = diff.send(frame, 4000, v2, panel.writer());
            QCOMPARE(result.rectangles, v2 ? 2 : 1);
            QCOMPARE(result.bytes, v2 ? 16 : 86);
            QCOMPARE(panel.image, frame);
        }
        auto frame = black();
        FrameDiff diff;
        Panel panel{frame};
        const auto result = diff.send(frame, 0, true, panel.writer(), 400000);
        QCOMPARE(result.rectangles, 1);
        QCOMPARE(panel.rects.first(), frame.rect());
        QCOMPARE(result.bytes, 307206);
    }
    void unknownPixelsAndInvalidationConverge_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<bool>("v2");
        QTest::newRow("landscape-v2") << QSize(480, 320) << true;
        QTest::newRow("portrait-v1") << QSize(320, 480) << false;
        QTest::newRow("partial-tiles") << QSize(35, 19) << true;
    }
    void unknownPixelsAndInvalidationConverge() {
        QFETCH(QSize, size);
        QFETCH(bool, v2);
        auto frame = black(size);
        FrameDiff diff;
        Panel panel{frame};
        panel.image.fill(Qt::magenta); // Even target black must overwrite unknown device memory.
        const int budget = 6 + (v2 ? 0 : 3140) + size.width() * 2 * 3;
        QVERIFY(drain(diff, frame, panel, v2, budget) > 1);
        QCOMPARE(panel.image, frame);
        QCOMPARE(diff.send(frame, 20000, v2, panel.writer(), budget).bytes, 0);
        diff.invalidate();
        panel.image.fill(Qt::white);
        QVERIFY(drain(diff, frame, panel, v2, budget) > 1);
        QCOMPARE(panel.image, frame);
    }
    void latestFrameReplacesUnsentPixels() {
        auto initial = black();
        FrameDiff diff;
        Panel panel{initial};
        QVERIFY(drain(diff, initial, panel) > 0);
        auto old = initial;
        old.fill(Qt::red);
        QVERIFY(diff.send(old, 3000, true, panel.writer()).pending);
        QVERIFY(panel.image != old);
        // Returning to the original image still needs to repair the already sent red rows.
        auto latest = initial;
        latest.setPixelColor(479, 319, Qt::blue);
        QVERIFY(drain(diff, latest, panel) > 0);
        QCOMPARE(panel.image, latest);
        QCOMPARE(diff.send(latest, 6000, true, panel.writer()).bytes, 0);
    }
    void smallChangesTakePriorityAndLargeOnesProgress() {
        auto frame = black();
        FrameDiff diff;
        Panel panel{frame};
        QVERIFY(drain(diff, frame, panel) > 0);
        { QPainter painter(&frame); painter.fillRect(0, 0, 200, 100, Qt::red); }
        frame.setPixelColor(470, 310, Qt::blue);
        panel.rects.clear();
        const auto result = diff.send(frame, 3000, true, panel.writer());
        QCOMPARE(panel.rects.first(), QRect(470, 310, 1, 1));
        QVERIFY(result.pending);
        QVERIFY(result.cost <= 7850);
        for (int i = 1; i < 30; ++i) {
            frame.setPixelColor(470, 310, i % 2 ? Qt::green : Qt::blue);
            diff.send(frame, 3000 + i * 50, true, panel.writer());
        }
        QCOMPARE(panel.image, frame);
    }
    void interruptedAndFailedWritesDoNotCommit() {
        auto frame = black({64, 32});
        FrameDiff diff;
        Panel panel{frame};
        QVERIFY(drain(diff, frame, panel) > 0);
        frame.setPixelColor(1, 1, Qt::red);
        frame.setPixelColor(60, 30, Qt::red);
        int writes = 0;
        auto result = diff.send(frame, 3000, true, [&](const QRect &r, const QByteArray &p) {
            return ++writes == 1 ? panel.write(r, p) : false;
        });
        QCOMPARE(result.rectangles, 1);
        QVERIFY(result.pending);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
            diff.send(frame, 3100, true, [](const QRect &, const QByteArray &) -> bool {
                throw std::runtime_error("Simulated partial transport failure");
            }));
        panel.rects.clear();
        result = diff.send(frame, 3200, true, panel.writer());
        QCOMPARE(result.rectangles, 1); // The successful first rectangle stays committed.
        QCOMPARE(panel.rects.first(), QRect(60, 30, 1, 1));
        QCOMPARE(panel.image, frame);
    }
    void randomizedOverwritesEventuallyMatchPanel() {
        std::mt19937 rng(565);
        for (bool v2 : {false, true}) {
            auto frame = black({65, 33});
            FrameDiff diff;
            Panel panel{frame};
            for (int step = 0; step < 100; ++step) {
                for (int n = 0; n < 100; ++n)
                    frame.setPixelColor(int(rng() % 65), int(rng() % 33),
                                        QColor(int(rng() % 256), int(rng() % 256), int(rng() % 256)));
                if (step % 17 == 0) diff.invalidate();
                const auto result = diff.send(frame, step * 50, v2, panel.writer(), 3500);
                QVERIFY(result.cost <= 3500);
            }
            QVERIFY(drain(diff, frame, panel, v2, 3500) > 0);
            QCOMPARE(panel.image, frame);
        }
    }
    void workerKeepsLatestFrameAndHandlesCommands_data() {
        QTest::addColumn<bool>("v2");
        QTest::newRow("v1-portrait") << false;
        QTest::newRow("v2-landscape") << true;
    }
    void workerKeepsLatestFrameAndHandlesCommands() {
#ifdef Q_OS_WIN
        QSKIP("PTY integration runs on Linux; refresh unit tests run on both platforms");
#else
        QFETCH(bool, v2);
        const int direction = v2 ? 2 : 0;
        const QSize size = v2 ? QSize(480, 320) : QSize(320, 480);
        PseudoScreen serial(v2);
        Device device;
        device.connectPort(serial.port, direction, 75, false);
        QTRY_VERIFY_WITH_TIMEOUT(device.status().connected, 3000);
        const auto init = Protocol::header(0, 0, 0, 0, 0xff) + Protocol::orientation(direction) +
                          Protocol::mirror(false) + Protocol::brightness(75);
        QTRY_COMPARE_WITH_TIMEOUT(serial.bytes(), init, 3000);
        auto frame = black(size);
        frame.fill(Qt::red);
        device.submit(frame);
        QTRY_VERIFY_WITH_TIMEOUT(device.status().frames > 0, 3000);
        QVERIFY(device.status().bytes < 307206); // The first large frame yielded at its budget.
        for (int i = 0; i < 20; ++i) {
            frame.fill(i % 2 ? Qt::blue : Qt::green);
            device.submit(frame);
        }
        frame.fill(Qt::black);
        frame.setPixelColor(size.width() - 1, size.height() - 1, Qt::white);
        device.submit(frame);
        device.command(3, 60); // A control command must land between complete bitmap packets.

        Panel panel{black(size)};
        panel.image.fill(Qt::magenta);
        qsizetype consumed = init.size();
        bool brightnessSeen = false;
        auto consume = [&] {
            const auto bytes = serial.bytes();
            while (consumed + 6 <= bytes.size()) {
                if (quint8(bytes[consumed + 5]) == 0x6e) {
                    if (bytes.mid(consumed, 6) != Protocol::brightness(60)) return false;
                    brightnessSeen = true;
                    consumed += 6;
                    continue;
                }
                quint64 packed = 0;
                for (int i = 0; i < 5; ++i) packed = (packed << 8) | quint8(bytes[consumed + i]);
                QRect r(QPoint(int((packed >> 30) & 1023), int((packed >> 20) & 1023)),
                        QPoint(int((packed >> 10) & 1023), int(packed & 1023)));
                if (!frame.rect().contains(r) || r.isEmpty()) return false;
                const int length = 6 + r.width() * r.height() * 2;
                if (consumed + length > bytes.size()) break;
                panel.write(r, bytes.mid(consumed, length));
                consumed += length;
            }
            return brightnessSeen && panel.image == frame && consumed == bytes.size();
        };
        QTRY_VERIFY_WITH_TIMEOUT(consume(), 5000); // Unsubmitted remainder drains on its own.
        const auto completedBytes = device.status().bytes;
        QTest::qWait(100);
        QCOMPARE(device.status().bytes, completedBytes);
        device.submit(frame);
        QTest::qWait(100);
        QCOMPARE(device.status().bytes, completedBytes);
        device.disconnectPort();
        QTRY_VERIFY_WITH_TIMEOUT(!device.status().connected, 3000);
#endif
    }
};
QTEST_GUILESS_MAIN(RefreshTest)
#include "framediff_test.moc"
