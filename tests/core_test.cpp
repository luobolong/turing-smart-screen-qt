#include "device.h"
#include "model.h"
#include "nrbf.h"
#include "renderer.h"
#include "linuxhardware.h"
#include "weather.h"
#include "systeminfo.h"
#include <QDirIterator>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QNetworkReply>
#include <QRawFont>
#include <QFontDatabase>
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
class WeatherReply : public QNetworkReply {
    QByteArray body;
    qint64 offset = 0;
public:
    WeatherReply(const QNetworkRequest &request, QByteArray body, QObject *parent)
        : QNetworkReply(parent), body(std::move(body)) {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished())
                return;
            setFinished(true);
            emit readyRead();
            emit finished();
        });
    }
    void abort() override {
        if (!isFinished()) {
            setError(OperationCanceledError, "cancelled");
            setFinished(true);
            emit finished();
        }
    }
    qint64 bytesAvailable() const override { return body.size() - offset + QNetworkReply::bytesAvailable(); }
    qint64 readData(char *data, qint64 maximum) override {
        const auto count = qMin(maximum, body.size() - offset);
        if (count <= 0)
            return -1;
        memcpy(data, body.constData() + offset, size_t(count));
        offset += count;
        return count;
    }
};
class WeatherNetwork : public QNetworkAccessManager {
public:
    QList<QByteArray> responses;
    QList<QUrl> requests;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        requests.append(request.url());
        return new WeatherReply(request, responses.isEmpty() ? QByteArray("{}") : responses.takeFirst(), this);
    }
};
#ifdef Q_OS_LINUX
static bool writeSensorFile(const QString &path, const QByteArray &value) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
}
#endif
#include "pseudoscreen.h"
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
        a = a.convertToFormat(QImage::Format_RGBA8888);
        b = b.convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(Protocol::changed(a, b), QRect(12, 17, 9, 9));
        QCOMPARE(Protocol::changed(a, a), QRect());
        b.setPixelColor(0, 0, QColor(0, 0, 0, 1));
        b.setPixelColor(479, 319, Qt::white);
        QCOMPARE(Protocol::changed(a, b), b.rect());
    }
    void imageCompositingRetainsIntegerAlphaAndClipping() {
        QImage source(7, 5, QImage::Format_RGBA8888);
        const int alphas[]{64, 127, 0, 1, 254, 255, 200};
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x)
                source.setPixelColor(x, y, QColor(x * 35, y * 55, (x + y) * 20, alphas[x]));
        QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); QVERIFY(source.save(&buffer, "PNG"));
        Renderer renderer;
        for (int destinationAlpha : {0, 1, 127, 254, 255}) {
            const QColor background(71, 113, 191, destinationAlpha);
            auto layer = newLayer("image", "Clipped image");
            layer["x"] = -2; layer["y"] = -1;
            layer["data"] = QString::fromLatin1(png.toBase64());
            layer["w"] = 7; layer["h"] = 5;
            Theme theme = Theme::starter();
            theme.data["background_color"] = rgba(background);
            theme.data["layers"] = QJsonArray{layer};
            const auto result = renderer.render(theme, {});
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 5; ++x) {
                const auto color = source.pixelColor(x + 2, y + 1);
                const quint32 a = color.alpha(), d = background.alpha();
                QColor expected;
                if (d == 0) expected = color;
                else if (d < 255) {
                    const quint32 combined = a * 255 + d * (255 - a);
                    expected = QColor((color.red() * a * 255 + background.red() * d * (255 - a)) / combined,
                                      (color.green() * a * 255 + background.green() * d * (255 - a)) / combined,
                                      (color.blue() * a * 255 + background.blue() * d * (255 - a)) / combined, combined / 255);
                } else expected = QColor((color.red() * a + background.red() * (255 - a)) / 255,
                                         (color.green() * a + background.green() * (255 - a)) / 255,
                                         (color.blue() * a + background.blue() * (255 - a)) / 255);
                QCOMPARE(result.pixelColor(x, y), expected);
            }
        }
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
        QSet<QString> registered;
        for (const auto &metric : metrics())
            registered.insert(metric.first);
        std::function<void(QJsonArray)> checkBindings = [&](QJsonArray layers) {
            for (const auto &value : layers) {
                const auto layer = value.toObject();
                const auto kind = layer["kind"].toString();
                if (kind == "sensor" || kind == "bar" || kind == "arc") {
                    const auto key = canonical(layer["metric"].toString());
                    QVERIFY2(registered.contains(key), qPrintable("Unregistered official metric: " + key));
                }
                checkBindings(layer["children"].toArray());
            }
        };
        QDirIterator it(assets() + "/official/config", {"*.data"}, QDir::Files);
        int count = 0;
        while (it.hasNext()) {
            auto file = it.next();
            if (file.endsWith("AppConfig.data"))
                continue;
            auto t = Theme::load(file);
            checkBindings(t.layers());
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
        s.values["Volume"] = 36;
        QCOMPARE(s.text("VulomeLoad"), QString("36 %"));
        QCOMPARE(s.text("VolumeLoad"), QString("36 %"));
        s.values["Volume"] = 125;
        QCOMPARE(s.text("Volume"), QString("125 %"));
        QCOMPARE(s.percentage("Volume"), 100.);
        s.sources["Volume"] = "test output";
        s.unavailable["CpuFan"] = "No driver";
        QCOMPARE(Snapshot::fromJson(s.json()).json(), s.json());
    }
    void themeOrientationValidation() {
        auto theme = Theme::starter();
        QCOMPARE(theme.orientation(), 2);
        theme.data["landscape"] = false;
        QCOMPARE(theme.orientation(), 0);
        theme.data["orientation"] = 3;
        theme.validate();
        QCOMPARE(Theme::fromJson(theme.data).orientation(), 3);
        for (const auto &bad : {QJsonValue(-1), QJsonValue(4), QJsonValue(1.5), QJsonValue("3")}) {
            theme.data["orientation"] = bad;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, theme.validate());
        }
    }
    void dashboardLayoutAndBundledIcons() {
        const auto theme = Theme::load(assets() + "/themes/nightfall.json");
        QCOMPARE(theme.size(), QSize(480, 320));
        QCOMPARE(theme.orientation(), 2);
        QSet<QString> groups, registered;
        for (const auto &metric : metrics())
            registered.insert(metric.first);
        QFont iconFont(symbolFontFamily());
        iconFont.setPixelSize(14);
        const auto rawFont = QRawFont::fromFont(iconFont);
        QVERIFY(rawFont.isValid());
        int icons = 0;
        QSet<QString> bound;
        std::function<void(QJsonArray, QPointF)> visit = [&](QJsonArray layers, QPointF offset) {
            for (const auto &value : layers) {
                const auto layer = value.toObject();
                const auto position = offset + QPointF(layer["x"].toDouble(), layer["y"].toDouble());
                if (layer["kind"] == "group") {
                    groups.insert(layer["name"].toString());
                    visit(layer["children"].toArray(), position);
                } else {
                    QVERIFY2(QRectF(QPointF(), theme.size()).contains(layerBounds(layer).translated(position)),
                             qPrintable(layer["name"].toString()));
                    QVERIFY(layer["kind"] != "arc");
                    if (layer["kind"] == "bar")
                        QVERIFY(layer["metric"] == "RamLoad" || layer["metric"] == "GpuRamLoad");
                    if (layer["kind"] == "sensor") {
                        QVERIFY(registered.contains(canonical(layer["metric"].toString())));
                        bound.insert(canonical(layer["metric"].toString()));
                    }
                    if (layer["font"].toObject()["family"] == "Turing Screen Symbols") {
                        ++icons;
                        for (const auto code : layer["text"].toString().toUcs4())
                            QVERIFY(rawFont.supportsCharacter(code));
                    }
                    QVERIFY(layer["text"].toString() != "CLOCK");
                    QVERIFY(layer["text"].toString() != "NIGHTFALL");
                }
            }
        };
        visit(theme.layers(), {});
        for (const auto &group : {"CPU", "GPU", "RAM", "NET", "CLOCK"})
            QVERIFY(groups.contains(group));
        QVERIFY(icons >= 14);
        for (const auto &removed : {"CpuFan", "CpuPower", "CpuVoltage", "RamValid", "HDDLoad", "HDDTemp",
                                   "Volume", "GpuFreq", "GpuRamLoad", "CpuModel", "GpuModel"})
            QVERIFY(!bound.contains(removed));
        for (const auto &required : {"OsName", "OsIcon", "KernelVersion", "WeatherCity", "WeatherTemp", "WeatherCondition",
                                    "WeatherIcon", "DateWeekday", "LocalIp"})
            QVERIFY(bound.contains(required));
        QCOMPARE(findLayer(theme.layers(), "nightfall-cpu-model")["text"].toString(), QString("AMD Ryzen 7 9800X3D"));
        QCOMPARE(findLayer(theme.layers(), "nightfall-gpu-model")["text"].toString(), QString("AMD Radeon RX 7900 XT"));
        for (int code : {0, 1, 2, 3, 45, 48, 51, 53, 55, 56, 57, 61, 63, 65, 66, 67,
                         71, 73, 75, 77, 80, 81, 82, 85, 86, 95, 96, 99, -1})
            for (const auto glyph : WeatherClient::icon(code).toUcs4())
                QVERIFY(rawFont.supportsCharacter(glyph));
        const auto latin = QRawFont::fromFont(QFont(latinFontFamily()));
        QVERIFY(latin.isValid());
        QVERIFY(latin.supportsCharacter(uint('W')));
        for (const auto &style : {"Regular", "SemiBold"}) {
            const auto id = QFontDatabase::addApplicationFont(assets() + "/fonts/TuringScreenLatin-" + style + ".ttf");
            QCOMPARE(QFontDatabase::applicationFontFamilies(id), QStringList{"Turing Screen Latin"});
            QFontDatabase::removeApplicationFont(id);
        }
        QFont monoFont(monoFontFamily());
        monoFont.setPixelSize(30);
        monoFont.setBold(true);
        const QFontMetricsF monoMetrics(monoFont);
        for (const auto digit : QString("0123456789:"))
            QCOMPARE(monoMetrics.horizontalAdvance(digit), monoMetrics.horizontalAdvance('0'));
        QCOMPARE(monoMetrics.horizontalAdvance("11:11:11"), monoMetrics.horizontalAdvance("23:58:06"));
        Snapshot longValues;
        longValues.strings["CpuModel"] = QString(200, 'M');
        longValues.strings["GpuModel"] = QString(200, 'M');
        longValues.strings["RamUsage"] = QString(200, 'M');
        longValues.values["InternetDown"] = 1e15;
        Renderer renderer;
        const auto image = renderer.render(theme, longValues);
        // Long model names and extreme rates stay inside their panels.
        for (int y = 70; y < 292; ++y)
            for (int x = 237; x < 244; ++x)
                QCOMPARE(image.pixelColor(x, y), QColor("#0B1018"));
    }
    void systemIdentityFormatting_data() {
        QTest::addColumn<QString>("product");
        QTest::addColumn<QString>("version");
        QTest::addColumn<QString>("pretty");
        QTest::addColumn<QString>("kernelType");
        QTest::addColumn<QString>("kernelVersion");
        QTest::addColumn<QString>("release");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("kernel");
        QTest::addColumn<uint>("glyph");
        QTest::newRow("win11-26h2")
            << "windows" << "11" << "Windows 11 Pro" << "winnt" << "10.0.26300.12345" << "26h2"
            << "Windows 11 26H2" << "NT 10.0.26300.12345" << uint(0xF05B3);
        QTest::newRow("win11-compatibility-name")
            << "windows" << "10" << "Windows 10 Pro" << "winnt" << "10.0.22631.1000" << "23H2"
            << "Windows 11 23H2" << "NT 10.0.22631.1000" << uint(0xF05B3);
        QTest::newRow("win11-release-missing")
            << "windows" << "11" << "Windows 11 Enterprise" << "winnt" << "10.0.26100" << ""
            << "Windows 11" << "NT 10.0.26100" << uint(0xF05B3);
        QTest::newRow("win10-22h2")
            << "windows" << "10" << "Windows 10 Home" << "winnt" << "10.0.19045.6500" << "22H2"
            << "Windows 10 22H2" << "NT 10.0.19045.6500" << uint(0xF05B3);
        QTest::newRow("win10-legacy-release")
            << "windows" << "10" << "Windows 10" << "winnt" << "10.0.18363" << "1909"
            << "Windows 10 1909" << "NT 10.0.18363" << uint(0xF05B3);
        QTest::newRow("server")
            << "windows" << "server2025" << "Windows Server Datacenter" << "winnt" << "10.0.26100" << "24H2"
            << "Windows Server 2025 24H2" << "NT 10.0.26100" << uint(0xF05B3);
        QTest::newRow("win-unavailable")
            << "windows" << "unknown" << "Windows" << "winnt" << "" << "unknown"
            << "Windows" << "--" << uint(0xF05B3);
        QTest::newRow("nixos")
            << "nixos" << "26.11" << "NixOS 26.11 (Example)" << "linux" << "7.3.0" << ""
            << "NixOS 26.11" << "Linux 7.3.0" << uint(0xF313);
        QTest::newRow("other-linux")
            << "ubuntu" << "26.04" << "Ubuntu 26.04 LTS" << "linux" << "6.17.0" << ""
            << "Ubuntu 26.04 LTS" << "Linux 6.17.0" << uint(0xF033D);
    }
    void systemIdentityFormatting() {
        QFETCH(QString, product);
        QFETCH(QString, version);
        QFETCH(QString, pretty);
        QFETCH(QString, kernelType);
        QFETCH(QString, kernelVersion);
        QFETCH(QString, release);
        QFETCH(QString, name);
        QFETCH(QString, kernel);
        QFETCH(uint, glyph);
        const auto identity = describeSystem(product, version, pretty, kernelType, kernelVersion, release);
        QCOMPARE(identity.name, name);
        QCOMPARE(identity.kernel, kernel);
        QCOMPARE(identity.icon.toUcs4(), QList<uint>{glyph});
        const auto font = QRawFont::fromFont(QFont(symbolFontFamily()));
        QVERIFY(font.supportsCharacter(glyph));
    }
    void nativeIconFit() {
        Renderer renderer;
        auto theme = Theme::starter();
        theme.data["background_color"] = rgba(Qt::transparent);
        const auto inkBounds = [](const QImage &image) {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (image.pixelColor(x, y).alpha() > 32)
                        bounds = bounds.united(QRect(x, y, 1, 1));
            return bounds;
        };
        auto label = newLayer("text", "NixOS");
        label["x"] = 10;
        label["y"] = 10;
        label["w"] = 40;
        label["h"] = 32;
        label["align"] = "center";
        label["valign"] = "bottom";
        label["fit"] = true;
        label["text"] = QString(QChar(0xF313));
        label["font"] = QJsonObject{{"family", "Turing Screen Symbols"}, {"size", 128}, {"color", rgba(Qt::white)}};
        theme.data["layers"] = QJsonArray{label};
        const auto icon = inkBounds(renderer.render(theme, {}));
        QVERIFY(!icon.isEmpty());
        QVERIFY(QRect(10, 10, 40, 32).contains(icon));
        QFont font(symbolFontFamily());
        font.setPixelSize(128);
        const auto original = QFontMetricsF(font).tightBoundingRect(label["text"].toString());
        const auto scale = qMin(40. / original.width(), 32. / original.height());
        QVERIFY(std::abs(icon.width() - original.width() * scale) < 3);
        QVERIFY(std::abs(icon.height() - original.height() * scale) < 3);
    }
    void nativeTextShrinkToFit() {
        Renderer renderer;
        auto theme = Theme::starter();
        theme.data["background_color"] = rgba(Qt::transparent);
        auto label = findLayer(Theme::load(assets() + "/themes/nightfall.json").layers(), "nightfall-system-version");
        label["kind"] = "text";
        label["metric"] = "";
        label["text"] = "Windows 11 26H2";
        theme.data["layers"] = QJsonArray{label};
        const auto fitted = renderer.render(theme, {});
        auto font = label["font"].toObject();
        const int maximumSize = font["size"].toInt();
        auto reference = label;
        reference["shrink_to_fit"] = false;
        reference["elide"] = false;
        reference["w"] = theme.size().width() - reference["x"].toInt();
        bool completeLabel = false;
        for (int size = 6; size < maximumSize; ++size) {
            auto referenceFont = font;
            referenceFont["size"] = size;
            reference["font"] = referenceFont;
            theme.data["layers"] = QJsonArray{reference};
            completeLabel |= fitted == renderer.render(theme, {});
        }
        // Compare with a complete, unclipped label at the same baseline;
        // font rasterizers may choose different fitting pixel sizes.
        QVERIFY(completeLabel);
        label["shrink_to_fit"] = false;
        theme.data["layers"] = QJsonArray{label};
        QVERIFY(fitted != renderer.render(theme, {}));
        label["text"] = "NixOS 26.11";
        theme.data["layers"] = QJsonArray{label};
        const auto shortLabel = renderer.render(theme, {});
        label["shrink_to_fit"] = true;
        theme.data["layers"] = QJsonArray{label};
        QCOMPARE(shortLabel, renderer.render(theme, {}));
    }
    void nativeTextBaselineStability() {
        Renderer renderer;
        auto theme = Theme::starter();
        theme.data["background_color"] = rgba(Qt::transparent);
        // A shared prefix must stay at exactly the same height when a suffix
        // introduces a descender or changes the last digit's ink bounds.
        for (const auto &alignment : {"bottom", "center", "baseline"}) {
            for (int size : {11, 20, 36}) {
                auto label = newLayer("text", "Stable text");
                label["x"] = 10;
                label["y"] = 10;
                label["w"] = 440;
                label["h"] = 80;
                label["align"] = "left";
                label["valign"] = alignment;
                if (QString::fromLatin1(alignment) == "baseline")
                    label["baseline"] = 60;
                QFont font(latinFontFamily());
                font.setPixelSize(size);
                label["font"] = QJsonObject{{"family", "Turing Screen Latin"}, {"size", size},
                                           {"color", rgba(Qt::white)}};
                const QFontMetricsF metrics(font);
                for (const auto &strings : {qMakePair(QString("AMD Ryzen 7 9800X3D"), QString("AMD Radeon RX 7900 XT")),
                                            qMakePair(QString("4"), QString("4g")),
                                            qMakePair(QString("4"), QString("40")),
                                            qMakePair(QString("3"), QString("34")),
                                            qMakePair(QString("8"), QString("89"))}) {
                    const auto prefix = strings.first.startsWith("AMD") ? QString("AMD") : strings.first;
                    const auto render = [&](const QString &value) {
                        label["text"] = value;
                        theme.data["layers"] = QJsonArray{label};
                        return renderer.render(theme, {}).copy(10, 0, int(metrics.horizontalAdvance(prefix)) - 1, 100);
                    };
                    const auto first = render(strings.first);
                    const auto second = render(strings.second);
                    QCOMPARE(first, second);
                    bool visible = false;
                    for (int y = 0; y < first.height(); ++y)
                        for (int x = 0; x < first.width(); ++x)
                            visible |= first.pixelColor(x, y).alpha() > 32;
                    QVERIFY(visible);
                }
            }
        }
    }
    void sensorDisplayFormats() {
        Renderer renderer;
        auto theme = Theme::starter();
        auto sensor = newLayer("sensor", "Formatted metric", "GpuRam");
        sensor["w"] = 180;
        sensor["h"] = 35;
        sensor["align"] = "right";
        sensor["value_format"] = QJsonObject{{"divisor", 1073741824.}, {"decimals", 1}, {"suffix", " GB"}};
        Snapshot sample;
        sample.values["GpuRam"] = 8375186227.;
        const auto check = [&](const QString &expected) {
            theme.data["layers"] = QJsonArray{sensor};
            const auto actual = renderer.render(theme, sample);
            auto label = sensor;
            label["kind"] = "text";
            label["text"] = expected;
            theme.data["layers"] = QJsonArray{label};
            QCOMPARE(actual, renderer.render(theme, sample));
        };
        check("7.8 GB");
        sample.values.clear();
        check("--");
        sensor["metric"] = "InternetDown";
        sensor["value_format"] = QJsonObject{{"auto_unit", "bytes/s"}, {"decimals", 1}};
        sample.values["InternetDown"] = 19503513.;
        check("18.6 MB/s");
        sample.values["InternetDown"] = 0;
        check("0.0 B/s");
    }
    void weatherCityCacheAndDisable() {
        WeatherNetwork network;
        WeatherClient weather(&network);
        Snapshot disabled;
        weather.apply(disabled);
        QVERIFY(network.requests.isEmpty());
        QVERIFY(disabled.unavailable.value("Weather").contains("disabled"));
        network.responses = {R"({"results":[{"name":"Taipei","latitude":25.03,"longitude":121.56}]})",
                             R"({"current":{"temperature_2m":23.5,"weather_code":61}})"};
        weather.configure(true, "Taipei, Taiwan");
        const auto sample = [&] { Snapshot s; weather.apply(s); return s; };
        QTRY_VERIFY(sample().strings.value("Weather").contains("23.5"));
        QCOMPARE(network.requests.size(), 2);
        QVERIFY(sample().strings.value("Weather").contains("Taipei"));
        QVERIFY(sample().strings.value("Weather").contains("Rain"));
        QCOMPARE(sample().strings.value("WeatherCity"), QString("Taipei"));
        QCOMPARE(sample().strings.value("WeatherCondition"), QString("Rain"));
        QCOMPARE(sample().strings.value("WeatherIcon"), WeatherClient::icon(61));
        QCOMPARE(sample().values.value("WeatherTemp", -999.), 23.5);
        QVERIFY(!sample().unavailable.contains("Weather"));
        QCOMPARE(network.requests.size(), 2);
        weather.configure(false, {});
        const auto off = sample();
        QVERIFY(!off.strings.contains("Weather"));
        QVERIFY(off.unavailable.value("Weather").contains("disabled"));
        QVERIFY(!off.strings.contains("WeatherIcon"));
        QVERIFY(off.unavailable.contains("WeatherIcon"));
    }
    void weatherIpAndInvalidResponses() {
        WeatherNetwork network;
        WeatherClient weather(&network);
        network.responses = {R"({"success":true,"city":"Test City","latitude":0,"longitude":0})",
                             R"({"current":{"temperature_2m":0,"weather_code":0}})"};
        weather.configure(true, {});
        const auto sample = [&] { Snapshot s; weather.apply(s); return s; };
        QTRY_VERIFY(sample().strings.contains("Weather"));
        QCOMPARE(network.requests.first().host(), QString("ipwho.is"));
        QVERIFY(sample().strings.value("Weather").contains("0.0"));
        QCOMPARE(sample().strings.value("WeatherIcon"), WeatherClient::icon(0));
        network.responses = {R"({"results":[{"name":"Bad","latitude":100,"longitude":0}]})"};
        weather.configure(true, "Bad City");
        QTRY_VERIFY(sample().unavailable.value("Weather").contains("City not found"));
        QVERIFY(!sample().strings.contains("Weather"));
        const auto count = network.requests.size();
        sample();
        QCOMPARE(network.requests.size(), count); // Errors back off instead of retrying every sample.
        network.responses = {R"({"results":[{"latitude":0,"longitude":0}]})",
                             R"({"current":{"temperature_2m":null,"weather_code":0}})"};
        weather.configure(true, "Missing temperature");
        QTRY_VERIFY(sample().unavailable.value("Weather").contains("no current conditions"));
        QVERIFY(!sample().strings.contains("Weather"));
    }
#ifdef Q_OS_LINUX
    void memoryAmountsInGb() {
        QTemporaryDir sysfs, proc;
        QVERIFY(writeSensorFile(proc.path() + "/meminfo",
                                "MemTotal: 67108864 kB\nMemAvailable: 58720256 kB\n"));
        Collector collector(sysfs.path(), proc.path());
        const auto sample = collector.collect();
        QCOMPARE(sample.text("RamValue"), QString("8.0 GB"));
        QCOMPARE(sample.text("RamValid"), QString("56.0 GB"));
        QCOMPARE(sample.text("RamTotal"), QString("64.0 GB"));
        QCOMPARE(sample.text("RamUsage"), QString("8.0 / 64.0 GB"));
        QCOMPARE(sample.values.value("RamLoad"), 12.5);
        const auto ram = component("RAM")["children"].toArray();
        bool containsGb = false;
        for (const auto &layer : ram)
            containsGb |= layer.toObject()["metric"].toString() == "RamUsage";
        QVERIFY(containsGb);
    }
    void cpuPowerRolloverAndLabelledFans() {
        QTemporaryDir sysfs;
        const auto root = sysfs.path();
        const auto cpu = root + "/class/hwmon/hwmon0/";
        const auto board = root + "/class/hwmon/hwmon1/";
        QVERIFY(writeSensorFile(cpu + "name", "k10temp"));
        QVERIFY(writeSensorFile(cpu + "temp1_input", "55000"));
        QVERIFY(writeSensorFile(board + "name", "nct6798"));
        QVERIFY(writeSensorFile(board + "fan1_label", "CPU Fan"));
        QVERIFY(writeSensorFile(board + "fan1_input", "0"));
        QVERIFY(writeSensorFile(board + "fan2_label", "Pump"));
        QVERIFY(writeSensorFile(board + "fan2_input", "1400"));
        QVERIFY(writeSensorFile(board + "in0_label", "Vcore"));
        QVERIFY(writeSensorFile(board + "in0_input", "1100"));
        const auto power = root + "/class/powercap/intel-rapl:0/";
        QVERIFY(writeSensorFile(power + "name", "package-0"));
        QVERIFY(writeSensorFile(power + "energy_uj", "9000000"));
        QVERIFY(writeSensorFile(power + "max_energy_range_uj", "10000000"));
        QHash<QString, double> previous;
        Snapshot first;
        collectLinuxHardware(first, root, {}, 2., previous);
        QCOMPARE(first.values.value("CpuTemp", -1), 55.);
        QCOMPARE(first.values.value("CpuFan", -1), 0.);
        QCOMPARE(first.values.value("CpuPump", -1), 1400.);
        QCOMPARE(first.values.value("CpuVoltage", -1), 1.1);
        QVERIFY(!first.values.contains("CpuPower"));
        QVERIFY(writeSensorFile(power + "energy_uj", "1000000"));
        Snapshot second;
        collectLinuxHardware(second, root, {}, 2., previous);
        QCOMPARE(second.values.value("CpuPower", -1), 1.);
        QVERIFY(writeSensorFile(power + "energy_uj", "bad data"));
        Snapshot invalid;
        collectLinuxHardware(invalid, root, {}, 2., previous);
        QVERIFY(!invalid.values.contains("CpuPower"));
        QVERIFY(invalid.unavailable.contains("CpuPower"));
        QVERIFY(previous.isEmpty());
    }
    void diskTemperatureTracksSelectedBackingDevice() {
        QTemporaryDir sysfs;
        const auto root = sysfs.path();
        QVERIFY(QDir().mkpath(root + "/class/block"));
        QVERIFY(QDir().mkpath(root + "/class/hwmon"));
        for (int i = 0; i < 2; ++i) {
            const auto controller = root + "/devices/nvme" + QString::number(i);
            const auto disk = controller + "/nvme" + QString::number(i) + "n1";
            const auto name = "nvme" + QString::number(i) + "n1p3";
            QVERIFY(writeSensorFile(disk + '/' + name + "/partition", "3"));
            QVERIFY(writeSensorFile(controller + "/hwmon0/name", "nvme"));
            QVERIFY(writeSensorFile(controller + "/hwmon0/temp1_input", i ? "99000" : "48000"));
            QVERIFY(QFile::link(controller, disk + "/device"));
            QVERIFY(QFile::link(disk + '/' + name, root + "/class/block/" + name));
            QVERIFY(QFile::link(controller + "/hwmon0", root + "/class/hwmon/hwmon" + QString::number(i)));
        }
        QHash<QString, double> energy;
        Snapshot selected;
        collectLinuxHardware(selected, root, "/dev/nvme0n1p3", 2., energy);
        QCOMPARE(selected.values.value("HDDTemp", -1), 48.);
        QVERIFY(QDir().mkpath(root + "/devices/dm-0/slaves"));
        QVERIFY(QFile::link(root + "/class/block/nvme0n1p3", root + "/devices/dm-0/slaves/nvme0n1p3"));
        QVERIFY(QFile::link(root + "/devices/dm-0", root + "/class/block/dm-0"));
        Snapshot mapped;
        collectLinuxHardware(mapped, root, "/dev/dm-0", 2., energy);
        QCOMPARE(mapped.values.value("HDDTemp", -1), 48.);
        Snapshot absent;
        collectLinuxHardware(absent, root, "/dev/missing", 2., energy);
        QVERIFY(!absent.values.contains("HDDTemp"));
    }
    void amdGpuSelectionAndUnits() {
        QTemporaryDir sysfs;
        QVERIFY(sysfs.isValid());
        const auto root = sysfs.path() + "/class/drm/";
        const auto integrated = root + "card0/device/";
        const auto discrete = root + "card1/device/";
        const auto connector = root + "card1-DP-1/device/";
        const auto unsupported = root + "card2/device/";
        for (const auto &base : {integrated, discrete, connector, unsupported}) {
            QVERIFY(writeSensorFile(base + "vendor", "0x1002\n"));
            QVERIFY(writeSensorFile(base + "uevent", "DRIVER=amdgpu\n"));
            QVERIFY(writeSensorFile(base + "boot_vga", "1\n"));
            QVERIFY(writeSensorFile(base + "mem_info_vram_total", "34359738368\n"));
            QVERIFY(writeSensorFile(base + "gpu_busy_percent", "11\n"));
        }
        QVERIFY(writeSensorFile(integrated + "boot_vga", "0\n"));
        QVERIFY(writeSensorFile(discrete + "mem_info_vram_total", "21474836480\n"));
        QVERIFY(writeSensorFile(discrete + "gpu_busy_percent", "73\n"));
        QVERIFY(writeSensorFile(discrete + "mem_info_vram_used", "3221225472\n"));
        QVERIFY(writeSensorFile(unsupported + "uevent", "DRIVER=radeon\n"));
        const auto hwmon = discrete + "hwmon/hwmon7/";
        QVERIFY(writeSensorFile(hwmon + "name", "amdgpu\n"));
        QVERIFY(writeSensorFile(hwmon + "temp1_input", "65500\n"));
        QVERIFY(writeSensorFile(hwmon + "freq1_input", "1750000000\n"));
        QVERIFY(writeSensorFile(hwmon + "power1_average", "74000000\n"));
        QVERIFY(writeSensorFile(hwmon + "fan1_input", "0\n"));
        QVERIFY(writeSensorFile(hwmon + "in0_input", "708\n"));
        QVERIFY(writeSensorFile(discrete + "pp_dpm_sclk", "0: 500Mhz\n1: 2000Mhz *\n"));
        Collector collector(sysfs.path());
        const auto sample = collector.collect();
        QCOMPARE(sample.values.value("GpuLoad", -1), 73.);
        QCOMPARE(sample.values.value("GpuTemp", -1), 65.5);
        QCOMPARE(sample.values.value("GpuFreq", -1), 1750.);
        QCOMPARE(sample.values.value("GpuRam", -1), 3221225472.);
        QCOMPARE(sample.values.value("GpuRamLoad", -1), 15.);
        QCOMPARE(sample.values.value("GpuRamFree", -1), 18253611008.);
        QCOMPARE(sample.values.value("GpuPower", -1), 74.);
        QCOMPARE(sample.values.value("GpuFan", -1), 0.);
        QCOMPARE(sample.values.value("GpuVoltage", -1), .708);
        QCOMPARE(sample.text("GPUCLOCK", true), QString("1.8"));
        QCOMPARE(sample.text("GpuRam"), QString("3072 MB"));
        // With no boot GPU marked, choose the largest VRAM without mixing cards.
        QVERIFY(writeSensorFile(discrete + "boot_vga", "0\n"));
        const auto other = collector.collect();
        QCOMPARE(other.values.value("GpuLoad", -1), 11.);
        QCOMPARE(other.text("GpuTemp"), QString("--"));
        QCOMPARE(other.text("GpuFreq"), QString("--"));
        QCOMPARE(other.text("GpuRam"), QString("--"));
    }
    void amdGpuFallbackAndUnavailableReadings() {
        QTemporaryDir sysfs;
        QVERIFY(sysfs.isValid());
        Collector collector(sysfs.path());
        for (const auto &metric : {"GpuLoad", "GpuTemp", "GpuFreq", "GpuRam"})
            QCOMPARE(collector.collect().text(metric), QString("--"));
        const auto base = sysfs.path() + "/class/drm/card10/device/";
        QVERIFY(writeSensorFile(base + "vendor", "0x1002\n"));
        QVERIFY(writeSensorFile(base + "uevent", "DRIVER=amdgpu\n"));
        QVERIFY(writeSensorFile(base + "gpu_busy_percent", "0\n"));
        QVERIFY(writeSensorFile(base + "mem_info_vram_used", "0\n"));
        QVERIFY(writeSensorFile(base + "pp_dpm_sclk", "0: 500Mhz\nS: 19Mhz *\n2: 2000Mhz\n"));
        auto sample = collector.collect();
        QCOMPARE(sample.text("GpuLoad"), QString("0 %"));
        QCOMPARE(sample.text("GpuRam"), QString("0 MB"));
        QCOMPARE(sample.values.value("GpuFreq", -1), 19.);
        // Invalid input is unavailable, including after a successful sample.
        QVERIFY(writeSensorFile(base + "gpu_busy_percent", "101\n"));
        QVERIFY(writeSensorFile(base + "mem_info_vram_used", "-1\n"));
        QVERIFY(writeSensorFile(base + "pp_dpm_sclk", "0: 500Mhz\n1: invalidMhz *\n"));
        const auto hwmon = base + "hwmon/hwmon0/";
        QVERIFY(writeSensorFile(hwmon + "name", "amdgpu\n"));
        QVERIFY(writeSensorFile(hwmon + "temp1_input", "NaN\n"));
        QVERIFY(writeSensorFile(hwmon + "freq1_input", "inf\n"));
        sample = collector.collect();
        for (const auto &metric : {"GpuLoad", "GpuTemp", "GpuFreq", "GpuRam"})
            QCOMPARE(sample.text(metric), QString("--"));
    }
#endif
};
QTEST_MAIN(CoreTest)
#include "core_test.moc"
