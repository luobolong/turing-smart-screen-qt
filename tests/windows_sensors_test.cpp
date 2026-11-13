#include "windowshardware.h"
#include "sensors.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QScopeGuard>
#include <QtEndian>
#include <QtTest>
#include <cstring>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace Screen;
namespace {
QByteArray hwinfoFixture() {
    QByteArray bytes(44 + 2 * 264 + 4 * 316, '\0');
    const auto put32 = [&](int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); };
    const auto putString = [&](int offset, const char *value) { std::memcpy(bytes.data() + offset, value, std::strlen(value)); };
    putString(0, "HWiS"); put32(4, 1);
    qToLittleEndian(QDateTime::currentSecsSinceEpoch(), bytes.data() + 12);
    put32(20, 44); put32(24, 264); put32(28, 2);
    put32(32, 572); put32(36, 316); put32(40, 4);
    putString(52, "CPU [#0]: Intel Core Test");
    putString(316, "GPU [#0]: NVIDIA Test");
    for (int i = 0; i < 4; ++i) {
        const int row = 572 + i * 316;
        put32(row, i == 0 || i == 3 ? 1 : 6); put32(row + 4, i == 3 ? 1 : 0); put32(row + 8, quint32(i));
        putString(row + 12, i == 0 ? "CPU Package" : i == 1 ? "Core 0 Clock" : i == 2 ? "Core 1 Clock" : "GPU Temperature");
        putString(row + 140, "User renamed label");
        putString(row + 268, i == 0 || i == 3 ? "C" : "MHz");
        const double value = i == 0 ? 65.5 : i == 1 ? 4200. : i == 2 ? 4900. : 53.;
        quint64 bits; std::memcpy(&bits, &value, sizeof(bits)); qToLittleEndian(bits, bytes.data() + row + 284);
    }
    return bytes;
}
QByteArray aidaFixture() {
    return "<temp><id>TCPUDIO</id><label>CPU Package</label><value>66.5</value></temp>"
           "<sys><id>SCPUCLK</id><label>CPU Clock</label><value>4700</value></sys>"
           "<sys><id>SCPUUTI</id><label>CPU Utilization</label><value>24</value></sys>"
           "<temp><id>TGPU1</id><label>GPU1 Diode</label><value>51</value></temp>"
           "<sys><id>SGPU1UTI</id><label>GPU1 Utilization</label><value>42</value></sys>"
           "<sys><id>SGPU1USEDDEMEM</id><label>GPU1 Used Dedicated Memory</label><value>2048</value></sys>"
           "<sys><id>SGPU1DEMEM</id><label>GPU1 Total Dedicated Memory</label><value>8192</value></sys>"
           "<temp><id>TGPU2</id><label>GPU2 Diode</label><value>61</value></temp>"
           "<fan><id>FCPU</id><label>CPU &amp; Fan</label><value>1200</value></fan>";
}
QByteArray libreFixture() {
    return R"JSON({"Text":"Machine","Children":[
      {"HardwareId":"/amdcpu/0","Text":"Ryzen Test","Children":[
        {"SensorId":"/amdcpu/0/temperature/0","Text":"Core (Tctl/Tdie)","Type":"Temperature","RawValue":68.25},
        {"SensorId":"/amdcpu/0/clock/0","Text":"Bus Speed","Type":"Clock","RawValue":100},
        {"SensorId":"/amdcpu/0/clock/1","Text":"CPU Core #1","Type":"Clock","RawValue":"4,8 GHz"},
        {"SensorId":"/amdcpu/0/clock/2","Text":"CPU Core #2","Type":"Clock","RawValue":5100},
        {"SensorId":"/amdcpu/0/clock/3","Text":"CPU Core #3","Type":"Clock","RawValue":null,"Value":"7000 MHz"}]},
      {"HardwareId":"/nvidiagpu/0","Text":"GPU A","Children":[
        {"SensorId":"/nvidiagpu/0/temperature/0","Text":"GPU Core","Type":"Temperature","RawValue":"131 °F"},
        {"SensorId":"/nvidiagpu/0/load/0","Text":"GPU Core","Type":"Load","RawValue":23},
        {"SensorId":"/nvidiagpu/0/smalldata/0","Text":"GPU Memory Used","Type":"SmallData","RawValue":2048},
        {"SensorId":"/nvidiagpu/0/smalldata/1","Text":"GPU Memory Total","Type":"SmallData","RawValue":8192},
        {"SensorId":"/nvidiagpu/0/control/0","Text":"GPU Fan","Type":"Control","RawValue":30}]},
      {"HardwareId":"/atigpu/0","Text":"GPU B","Children":[
        {"SensorId":"/atigpu/0/temperature/0","Text":"GPU Core","Type":"Temperature","Value":"60.5 °C"}]}]})JSON";
}
}
class WindowsSensorsTest : public QObject {
    Q_OBJECT
private slots:
    void hwinfoLayoutAndLiveClocks() {
        const auto data = parseHwinfo(hwinfoFixture());
        QVERIFY2(data.error.isEmpty(), qPrintable(data.error));
        QCOMPARE(data.readings.size(), 4);
        Snapshot s;
        applyHardwareReadings(s, data, "HWiNFO");
        QCOMPARE(s.values.value("CpuTemp"), 65.5);
        QCOMPARE(s.values.value("CpuFreq"), 4900.);
        QCOMPARE(s.values.value("GpuTemp"), 53.);
        QVERIFY(s.sources["CpuFreq"].contains("Core 1 Clock"));
        auto bytes = hwinfoFixture();
        bytes.truncate(bytes.size() - 1);
        QVERIFY(!parseHwinfo(bytes).error.isEmpty());
        bytes = hwinfoFixture(); qToLittleEndian(quint32(0xffffffff), bytes.data() + 40);
        QVERIFY(!parseHwinfo(bytes).error.isEmpty());
        bytes = hwinfoFixture(); bytes[0] = 'X';
        QVERIFY(!parseHwinfo(bytes).error.isEmpty());
        bytes = hwinfoFixture();
        const auto now = QDateTime::currentSecsSinceEpoch();
        qToLittleEndian(now - 60, bytes.data() + 12);
        const auto stale = parseHwinfo(bytes, now);
        QVERIFY(stale.error.contains("stale"));
        QVERIFY(stale.readings.isEmpty());
    }
    void aidaFragmentsUnicodeAndGpuSelection() {
        auto bytes = aidaFixture();
        const auto parsed = parseAida64(bytes);
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        QByteArray wide;
        for (auto c : QString::fromUtf8(bytes)) { char encoded[2]; qToLittleEndian(c.unicode(), encoded); wide.append(encoded, 2); }
        wide.append("\0\0", 2);
        QCOMPARE(parseAida64(wide).readings.size(), parsed.readings.size());
        Snapshot s;
        applyHardwareReadings(s, parsed, "AIDA64");
        QCOMPARE(s.values.value("CpuFreq"), 4700.);
        QCOMPARE(s.values.value("CpuLoad"), 24.);
        QCOMPARE(s.values.value("CpuFan"), 1200.);
        QCOMPARE(s.values.value("GpuTemp"), 51.);
        QCOMPARE(s.values.value("GpuLoad"), 42.);
        QCOMPARE(s.values.value("GpuRamLoad"), 25.);
        QCOMPARE(s.values.value("GpuRamFree"), 6144. * 1048576.);
        applyHardwareReadings(s, parsed, "AIDA64", 1);
        QCOMPARE(s.values.value("GpuTemp"), 61.);
        QVERIFY(!s.values.contains("GpuRam"));
        QVERIFY(!parseAida64("<temp><id>broken").error.isEmpty());
        QVERIFY(!parseAida64("<temp><id>TCPUDIO</id><value>TRIAL</value></temp>").error.isEmpty());
    }
    void libreNumericFormattedNullAndUnits() {
        const auto parsed = parseLibreHardwareMonitor(libreFixture());
        QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
        Snapshot s;
        applyHardwareReadings(s, parsed, "LibreHardwareMonitor");
        QCOMPARE(s.values.value("CpuTemp"), 68.25);
        QCOMPARE(s.values.value("CpuFreq"), 5100.);
        QCOMPARE(s.values.value("GpuTemp"), 55.);
        QCOMPARE(s.values.value("GpuRamLoad"), 25.);
        QVERIFY(!s.values.contains("GpuFan")); // A percentage is not an RPM reading.
        QCOMPARE(s.strings.value("GpuModel"), QString("GPU A"));
        applyHardwareReadings(s, parsed, "LibreHardwareMonitor", 1);
        QCOMPARE(s.values.value("GpuTemp"), 60.5);
        QCOMPARE(s.strings.value("GpuModel"), QString("GPU B"));
        QVERIFY(!parseLibreHardwareMonitor("{bad").error.isEmpty());
    }
    void errorsAndMissingSensorsClearOldValues() {
        Snapshot s;
        s.values["CpuLoad"] = 12; s.values["RamValue"] = 12345;
        applyHardwareReadings(s, parseLibreHardwareMonitor(libreFixture()), "LibreHardwareMonitor");
        HardwareReadings failure; failure.error = "HWiNFO: permission denied";
        applyHardwareReadings(s, failure, "HWiNFO");
        QVERIFY(!s.values.contains("CpuFreq"));
        QVERIFY(!s.values.contains("GpuTemp"));
        QVERIFY(!s.strings.contains("GpuModel"));
        QVERIFY(!s.sources.contains("GpuTemp"));
        QCOMPARE(s.unavailable.value("GpuTemp"), failure.error);
        QCOMPARE(s.values.value("RamValue"), 12345.);
        QCOMPARE(s.values.value("CpuLoad"), 12.);
        applyHardwareReadings(s, parseLibreHardwareMonitor(libreFixture()), "LibreHardwareMonitor", 10);
        QVERIFY(!s.values.contains("GpuTemp")); // Never fall back to another GPU.
        HardwareReadings gpuOnly;
        gpuOnly.readings = {{"gpu", "GPU", "GPU", "GPU Core", "Temperature", "°C", 99}};
        applyHardwareReadings(s, gpuOnly, "Test");
        QVERIFY(!s.values.contains("CpuTemp"));
        QCOMPARE(windowsSensorSource("invalid"), QString("system"));
    }
    void localHttpAcquisitionAndDisconnect() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray body = libreFixture();
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto request = socket->readAll();
                if (!request.contains("\r\n\r\n")) return;
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        WindowsHardware source;
        source.configure("librehardwaremonitor", server.serverPort());
        Snapshot sample;
        source.apply(sample);
        QVERIFY(!sample.values.contains("GpuTemp"));
        QTRY_VERIFY_WITH_TIMEOUT(([&] { source.apply(sample); return sample.values.contains("GpuTemp"); })(), 2000);
        QCOMPARE(sample.values.value("CpuFreq"), 5100.);
        source.configure("librehardwaremonitor", server.serverPort(), 1);
        source.apply(sample);
        QVERIFY(!sample.values.contains("GpuTemp"));
        QTRY_VERIFY_WITH_TIMEOUT(([&] { source.apply(sample); return sample.values.contains("GpuTemp"); })(), 2000);
        QCOMPARE(sample.values.value("GpuTemp"), 60.5);
        server.close();
        QTest::qWait(1050);
        source.apply(sample);
        QTRY_VERIFY_WITH_TIMEOUT(([&] { source.apply(sample); return !sample.values.contains("GpuTemp"); })(), 5000);
        QVERIFY(sample.unavailable.value("GpuTemp").contains("Web Server"));
    }
#ifdef Q_OS_WIN
    void realWindowsSharedMemoryAcquisition() {
        // Exercise Win32 mapping as well as the parser; never replace a running monitor.
        HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, L"AIDA64_SensorValues");
        if (existing) { CloseHandle(existing); QSKIP("AIDA64 is already running"); }
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16384, L"AIDA64_SensorValues");
        QVERIFY(mapping);
        auto view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
        const auto cleanup = qScopeGuard([&] { if (view) UnmapViewOfFile(view); if (mapping) CloseHandle(mapping); });
        QVERIFY(view);
        const auto bytes = aidaFixture(); std::memcpy(view, bytes.constData(), size_t(bytes.size()));
        WindowsHardware source;
        source.configure("aida64");
        Snapshot sample; source.apply(sample);
        QCOMPARE(sample.values.value("CpuFreq"), 4700.);
        QCOMPARE(sample.values.value("GpuTemp"), 51.);
        UnmapViewOfFile(view); CloseHandle(mapping);
        view = nullptr; mapping = nullptr;
        source.apply(sample);
        QVERIFY(!sample.values.contains("CpuFreq"));
        QVERIFY(!sample.values.contains("GpuTemp"));
    }
    void realHwinfoMappingAndStaleSamples() {
        HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\HWiNFO_SENS_SM2");
        if (existing) { CloseHandle(existing); QSKIP("HWiNFO shared memory is already active"); }
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 16384, L"Global\\HWiNFO_SENS_SM2");
        if (!mapping) QSKIP("Creating a Global mapping requires the Windows global-object privilege");
        auto view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
        const auto cleanup = qScopeGuard([&] { if (view) UnmapViewOfFile(view); CloseHandle(mapping); });
        QVERIFY(view);
        const auto bytes = hwinfoFixture(); std::memcpy(view, bytes.constData(), size_t(bytes.size()));
        WindowsHardware source; source.configure("hwinfo");
        Snapshot sample; source.apply(sample);
        QCOMPARE(sample.values.value("CpuTemp"), 65.5);
        QCOMPARE(sample.values.value("CpuFreq"), 4900.);
        qToLittleEndian(QDateTime::currentSecsSinceEpoch() - 60, static_cast<char *>(view) + 12);
        source.apply(sample);
        QVERIFY(!sample.values.contains("CpuFreq"));
        QVERIFY(!sample.values.contains("CpuTemp"));
        QVERIFY(sample.unavailable.value("CpuTemp").contains("stale"));
    }
#endif
};
QTEST_GUILESS_MAIN(WindowsSensorsTest)
#include "windows_sensors_test.moc"
