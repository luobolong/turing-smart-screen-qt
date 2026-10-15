#include "device.h"
#include "model.h"
#include "nrbf.h"
#include "renderer.h"
#include <QDirIterator>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <random>
#ifndef Q_OS_WIN
#include <atomic>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
using namespace Screen;
#ifndef Q_OS_WIN
class PseudoScreen {
public:
    int master = -1;
    QString port;
    QMutex mutex;
    QByteArray received;
    std::atomic<bool> stopping{false};
    std::thread worker;
    PseudoScreen() {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0 || grantpt(master) || unlockpt(master))
            throw std::runtime_error("Cannot create serial test PTY");
        port = QString::fromLocal8Bit(ptsname(master));
        worker = std::thread([this] {
            bool replied = false;
            while (!stopping) {
                pollfd descriptor{master, POLLIN, 0};
                if (poll(&descriptor, 1, 10) <= 0)
                    continue;
                char buffer[16384];
                auto count = ::read(master, buffer, sizeof(buffer));
                if (count <= 0)
                    continue;
                QMutexLocker lock(&mutex);
                received.append(buffer, int(count));
                if (!replied && received.size() >= 6) {
                    ::write(master, "V2", 2);
                    replied = true;
                }
            }
        });
    }
    ~PseudoScreen() {
        stopping = true;
        worker.join();
        ::close(master);
    }
    QByteArray bytes() {
        QMutexLocker lock(&mutex);
        return received;
    }
};
#endif
class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void protocol() {
        QCOMPARE(Protocol::header(0, 0, 319, 479, 0xc5).toHex(), QByteArray("000004fddfc5"));
        QCOMPARE(Protocol::header(0, 0, 479, 319, 0xc5).toHex(), QByteArray("0000077d3fc5"));
        QCOMPARE(Protocol::orientation(2).toHex(), QByteArray("0000000000796601e001400000000000"));
        QCOMPARE(Protocol::brightness(75).toHex(), QByteArray("0fc00000006e"));
        QImage image(3, 1, QImage::Format_RGB32);
        image.setPixelColor(0, 0, Qt::red);
        image.setPixelColor(1, 0, Qt::green);
        image.setPixelColor(2, 0, Qt::blue);
        QCOMPARE(Protocol::bitmap(image).mid(6).toHex(), QByteArray("00f8e0071f00"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Protocol::header(1024, 0, 0, 0, 1));
        std::mt19937 rng(35);
        for (int n = 0; n < 1000; ++n) {
            int x = int(rng() % 1024), y = int(rng() % 1024), ex = int(rng() % 1024), ey = int(rng() % 1024);
            auto b = Protocol::header(x, y, ex, ey, 0xc5);
            quint64 p = 0;
            for (int i = 0; i < 5; ++i)
                p = (p << 8) | quint8(b[i]);
            QCOMPARE(int((p >> 30) & 1023), x);
            QCOMPARE(int((p >> 20) & 1023), y);
            QCOMPARE(int((p >> 10) & 1023), ex);
            QCOMPARE(int(p & 1023), ey);
        }
    }
    void changedRegion() {
        QImage a(480, 320, QImage::Format_RGB32);
        a.fill(Qt::black);
        auto b = a;
        QCOMPARE(Protocol::changed(a, b), QRect());
        b.setPixelColor(12, 17, Qt::red);
        b.setPixelColor(20, 25, Qt::white);
        QCOMPARE(Protocol::changed(a, b), QRect(12, 17, 9, 9));
    }
    void serialWorkerAndPartialFrames() {
#ifdef Q_OS_WIN
        QSKIP("PTY integration test runs on Linux; protocol golden tests run on both platforms");
#else
        PseudoScreen screen;
        Device device;
        device.connectPort(screen.port, 2, 75, false);
        QTRY_VERIFY_WITH_TIMEOUT(device.status().connected, 3000);
        QByteArray initialization = Protocol::header(0, 0, 0, 0, 0xff) + Protocol::orientation(2) +
                                    Protocol::mirror(false) + Protocol::brightness(75);
        QTRY_VERIFY_WITH_TIMEOUT(screen.bytes().size() >= initialization.size(), 3000);
        QCOMPARE(screen.bytes(), initialization);
        QImage frame(480, 320, QImage::Format_RGB32);
        frame.fill(Qt::black);
        auto packet = Protocol::bitmap(frame);
        device.submit(frame);
        QTRY_COMPARE_WITH_TIMEOUT(device.status().frames, quint64(1), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(screen.bytes().size(), initialization.size() + packet.size(), 3000);
        QCOMPARE(screen.bytes().mid(initialization.size()), packet);
        frame.setPixelColor(12, 17, Qt::red);
        device.submit(frame);
        QTRY_COMPARE_WITH_TIMEOUT(device.status().frames, quint64(2), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(screen.bytes().size(), initialization.size() + packet.size() + 8, 3000);
        QCOMPARE(screen.bytes().right(8),
                 Protocol::header(12, 17, 12, 17, 0xc5) + QByteArray::fromHex("00f8"));
        device.submit(frame);
        QTest::qWait(50);
        QCOMPARE(device.status().frames, quint64(2));
        device.disconnectPort();
        QTRY_VERIFY_WITH_TIMEOUT(!device.status().connected, 3000);
#endif
    }
    void officialThemes() {
        Renderer renderer;
        QDirIterator it(assets() + "/official/config", {"*.data"}, QDir::Files);
        int count = 0;
        while (it.hasNext()) {
            auto file = it.next();
            if (file.endsWith("AppConfig.data"))
                continue;
            auto t = Theme::load(file);
            auto image = renderer.render(t, {});
            QCOMPARE(image.size(), t.size());
            QVERIFY(renderer.cachedBytes() <= 4 * 1024 * 1024);
            auto copy = Theme::fromJson(t.data);
            QCOMPARE(copy.data, t.data);
            ++count;
        }
        QVERIFY(count >= 36);
    }
    void jsonMigration() {
        Theme t = Theme::starter();
        QTemporaryDir dir;
        auto path = dir.path() + "/theme.json";
        t.save(path);
        QCOMPARE(Theme::load(path).data, t.data);
        QFile legacy(QString::fromUtf8(SCREEN_FIXTURE_DIR) + "/legacy.json");
        if (legacy.open(QIODevice::ReadOnly)) {
            auto migrated = Theme::fromJson(QJsonDocument::fromJson(legacy.readAll()).object());
            QVERIFY(!migrated.layers().isEmpty());
        }
    }
    void hierarchyAndCopies() {
        auto a = QJsonArray{component("CPU"), component("RAM")};
        auto g = a[0].toObject(), child = g["children"].toArray()[1].toObject();
        auto id = child["id"].toString(), gid = g["id"].toString();
        QCOMPARE(parentOf(a, id), gid);
        QCOMPARE(normalizedSelection(a, {gid, id}), QStringList{gid});
        auto pos = absolutePosition(a, id);
        QVERIFY(updateLayer(a, gid, [](QJsonObject &o) { o["x"] = 101; }));
        QCOMPARE(absolutePosition(a, id).x(), pos.x() + 85);
        QVERIFY(updateLayer(a, gid, [](QJsonObject &o) { o["locked"] = true; }));
        QVERIFY(layerLocked(a, id));
        auto before = a;
        QVERIFY(updateLayer(a, id, [](QJsonObject &o) { o["name"] = "Renamed"; }));
        QCOMPARE(findLayer(before, id)["name"].toString(), QString("CPU"));
        auto copied = findLayer(a, gid);
        regenerateIds(copied);
        QVERIFY(copied["id"] != gid);
    }
    void malformedFiles() {
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Nrbf::Document::parse("bad"));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, Theme::fromJson({{"layers", QJsonArray()}}));
        QFile f(assets() + "/official/config/微星.data");
        QVERIFY(f.open(QIODevice::ReadOnly));
        auto bytes = f.readAll();
        for (int n : {0, 1, 10, int(bytes.size() / 2)})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, Theme::fromOfficial(bytes.left(n)));
    }
    void imageLimits() {
        Renderer renderer;
        QImage huge(1537, 1, QImage::Format_RGB32);
        huge.fill(Qt::black);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        huge.save(&buffer, "PNG");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, renderer.decode(QString::fromLatin1(bytes.toBase64())));
    }
    void officialEditorGolden() {
        QFile fixture(QString::fromUtf8(SCREEN_FIXTURE_DIR) + "/official-editor.json");
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        auto sample = Snapshot::fromJson(QJsonDocument::fromJson(fixture.readAll()).object());
        Renderer renderer;
        auto actual = renderer.render(Theme::load(assets() + "/official/config/微星.data"), sample, true);
        QImage reference(QString::fromUtf8(SCREEN_FIXTURE_DIR) + "/msi-editor-reference.png");
        QVERIFY(!reference.isNull());
        QCOMPARE(actual.convertToFormat(QImage::Format_RGB32),
                 reference.convertToFormat(QImage::Format_RGB32));
        QCOMPARE(Protocol::bitmap(actual), Protocol::bitmap(reference));
    }
    void metricFormatting() {
        Snapshot s;
        s.values = {{"CpuLoad", 83}, {"CpuFreq", 4800}, {"InternetDown", 81920}, {"RamValue", 34359738368.}};
        QCOMPARE(s.text("CPULOAD", true), QString("83"));
        QCOMPARE(s.text("CpuClock", true), QString("4.8"));
        QCOMPARE(s.text("UPSpeed"), QString("--"));
        QCOMPARE(s.text("DOWNDSPEED", true), QString("80.0"));
        QCOMPARE(s.text("Ram", true), QString("32.0"));
    }
};
QTEST_MAIN(CoreTest)
#include "core_test.moc"
