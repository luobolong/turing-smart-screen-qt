#include "windowshardware.h"
#include "sensors.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace Screen {
namespace {
constexpr qsizetype maxBytes = 16 * 1024 * 1024;
QString stringAt(const QByteArray &bytes, qsizetype offset, qsizetype length) {
    auto field = bytes.mid(offset, length);
    auto end = field.indexOf('\0');
    if (end >= 0) field.truncate(end);
    return QString::fromLocal8Bit(field);
}
quint32 u32(const QByteArray &bytes, qsizetype offset) {
    return qFromLittleEndian<quint32>(bytes.constData() + offset);
}
QString category(const QString &name) {
    const auto lower = name.toLower();
    if (lower.contains("gpu") || lower.contains("radeon") || lower.contains("geforce")) return "GPU";
    if (lower.contains("cpu") || lower.contains("ryzen") || lower.contains("intel core")) return "CPU";
    if (lower.contains("drive") || lower.contains("disk") || lower.contains("nvme") || lower.contains("s.m.a.r.t.")) return "Disk";
    return "Board";
}
QString kindForUnit(const QString &unit) {
    if (unit == "MHz" || unit == "GHz" || unit == "kHz" || unit == "Hz") return "Clock";
    if (unit == "MB" || unit == "GB" || unit == "MiB" || unit == "GiB" || unit == "B") return "Data";
    if (unit == "%") return "Load";
    return "Other";
}
double memoryBytes(const HardwareReading &r) {
    if (r.unit == "GB" || r.unit == "GiB") return r.value * 1073741824.;
    if (r.unit == "MB" || r.unit == "MiB") return r.value * 1048576.;
    return r.value;
}
bool containsAny(const QString &name, std::initializer_list<const char *> words) {
    for (auto word : words) if (name.contains(QLatin1String(word))) return true;
    return false;
}
}
QString windowsSensorSource(QString source) {
    source = source.toLower();
    return QStringList{"hwinfo", "aida64", "librehardwaremonitor"}.contains(source) ? source : "system";
}
HardwareReadings parseHwinfo(const QByteArray &bytes, qint64 now) {
    HardwareReadings result;
    if (bytes.size() < 44 || bytes.size() > maxBytes || bytes.left(4) != "HWiS" || u32(bytes, 4) != 1) {
        result.error = "HWiNFO: shared memory inactive or unsupported header";
        return result;
    }
    const auto sensors = u32(bytes, 20), sensorSize = u32(bytes, 24), sensorCount = u32(bytes, 28);
    const auto readings = u32(bytes, 32), readingSize = u32(bytes, 36), readingCount = u32(bytes, 40);
    const auto fits = [&bytes](quint32 offset, quint32 size, quint32 count) {
        return offset >= 44 && quint64(offset) + quint64(size) * count <= quint64(bytes.size());
    };
    if (sensorSize < 264 || readingSize < 316 || sensorCount > 4096 || readingCount > 65536 ||
        !fits(sensors, sensorSize, sensorCount) || !fits(readings, readingSize, readingCount)) {
        result.error = "HWiNFO: truncated or invalid shared memory layout";
        return result;
    }
    result.pollTime = qFromLittleEndian<qint64>(bytes.constData() + 12);
    if (now > 0 && (result.pollTime <= 0 || now - result.pollTime > 30)) {
        result.error = "HWiNFO: sensor data stale; check Sensors and Shared Memory Support";
        return result;
    }
    for (quint32 i = 0; i < readingCount; ++i) {
        const qsizetype row = readings + qsizetype(i) * readingSize;
        const auto parent = u32(bytes, row + 4);
        if (parent >= sensorCount) continue;
        const qsizetype sensor = sensors + qsizetype(parent) * sensorSize;
        // Original labels are stable across user renames and localization.
        auto device = stringAt(bytes, sensor + 8, 128);
        const auto sensorName = stringAt(bytes, row + 12, 128);
        auto group = category(device);
        const auto gpu = QRegularExpression("GPU\\s*\\[#(\\d+)\\]", QRegularExpression::CaseInsensitiveOption).match(device);
        if (gpu.hasMatch()) device = "GPU #" + gpu.captured(1) + " / " + device;
        const auto bits = qFromLittleEndian<quint64>(bytes.constData() + row + 284);
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        if (!std::isfinite(value)) continue;
        const QStringList kinds{"Other", "Temperature", "Voltage", "Fan", "Current", "Power", "Clock", "Load"};
        auto kind = kinds.value(int(u32(bytes, row)), "Other");
        auto unit = stringAt(bytes, row + 268, 16);
        if (kind == "Other") kind = kindForUnit(unit);
        if (kind == "Temperature") unit = QString::fromUtf8("°C");
        result.readings.append({QString::number(u32(bytes, sensor)) + ":" + QString::number(u32(bytes, sensor + 4)) + ":" + QString::number(u32(bytes, row + 8)),
                                device, group, sensorName, kind, unit, value});
    }
    if (result.readings.isEmpty()) result.error = "HWiNFO: no exported sensors; enable Sensors and Shared Memory Support";
    return result;
}
HardwareReadings parseAida64(const QByteArray &bytes) {
    HardwareReadings result;
    if (bytes.isEmpty() || bytes.size() > maxBytes) {
        result.error = "AIDA64: empty or oversized shared memory";
        return result;
    }
    QString text;
    if (bytes.startsWith("\xff\xfe") || (bytes.size() >= 2 && bytes[1] == '\0')) {
        for (qsizetype i = bytes.startsWith("\xff\xfe") ? 2 : 0; i + 1 < bytes.size(); i += 2) {
            const auto c = qFromLittleEndian<quint16>(bytes.constData() + i);
            if (!c) break;
            text += QChar(c);
        }
    } else text = stringAt(bytes, 0, bytes.size());
    // AIDA64 publishes adjacent XML fragments, not a complete XML document.
    QXmlStreamReader xml("<sensors>" + text + "</sensors>");
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("sensors")) continue;
        const auto tag = xml.name().toString();
        if (!QStringList{"temp", "fan", "volt", "pwr", "curr", "sys"}.contains(tag)) { xml.skipCurrentElement(); continue; }
        QHash<QString, QString> fields;
        while (xml.readNextStartElement()) {
            const auto key = xml.name().toString();
            fields[key] = xml.readElementText();
        }
        bool ok = false;
        const auto value = QString(fields.value("value")).replace(',', '.').toDouble(&ok);
        if (!ok || !std::isfinite(value)) continue;
        const auto id = fields.value("id");
        const auto name = fields.value("label", id);
        const auto gpu = QRegularExpression("GPU(\\d*)").match(id);
        QString group = gpu.hasMatch() ? "GPU" : id.contains("CPU") || id.startsWith("SCC-") ? "CPU" : id.contains("HDD") ? "Disk" : "Board";
        QString device = gpu.hasMatch() ? "GPU #" + (gpu.captured(1).isEmpty() ? QString("1") : gpu.captured(1)) : group;
        const QHash<QString, QString> kinds{{"temp", "Temperature"}, {"fan", "Fan"}, {"volt", "Voltage"}, {"pwr", "Power"}, {"curr", "Current"}};
        const QHash<QString, QString> units{{"temp", "°C"}, {"fan", "RPM"}, {"volt", "V"}, {"pwr", "W"}, {"curr", "A"}};
        auto kind = kinds.value(tag, "Other"), unit = units.value(tag);
        if (tag == "sys") {
            if (id == "SCPUCLK" || id.startsWith("SCC-") || (gpu.hasMatch() && id.endsWith("CLK"))) { kind = "Clock"; unit = "MHz"; }
            else if (id.endsWith("UTI") || id == "SMEMUTI" || id == "SVMEMUSAGE") { kind = "Load"; unit = "%"; }
            else if (gpu.hasMatch() && (id.contains("DEMEM") || id.contains("VIDMEM"))) { kind = "Data"; unit = "MB"; }
        }
        result.readings.append({id, device, group, name, kind, unit, value});
    }
    if (xml.hasError()) { result.readings.clear(); result.error = "AIDA64: malformed shared memory XML"; }
    else if (result.readings.isEmpty()) result.error = "AIDA64: no exported sensors; enable shared memory and select sensor values";
    return result;
}
HardwareReadings parseLibreHardwareMonitor(const QByteArray &bytes) {
    HardwareReadings result;
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (bytes.size() > maxBytes || error.error != QJsonParseError::NoError || !json.isObject()) {
        result.error = "LibreHardwareMonitor: invalid data.json response";
        return result;
    }
    int nodes = 0;
    std::function<void(const QJsonObject &, QString, int)> visit = [&](const QJsonObject &node, QString device, int depth) {
        if (depth > 32 || ++nodes > 65536) return;
        if (node.contains("HardwareId") || node["ImageURL"].toString().contains("cpu.png") || node["ImageURL"].toString().contains("ati.png") ||
            node["ImageURL"].toString().contains("nvidia.png") || node["ImageURL"].toString().contains("intel.png")) device = node["Text"].toString();
        const auto id = node["SensorId"].toString();
        if (!id.isEmpty()) {
            const auto kind = node["Type"].toString();
            auto raw = node.contains("RawValue") ? node["RawValue"] : node["Value"];
            double value = 0;
            QString unit;
            bool ok = raw.isDouble();
            if (ok) value = raw.toDouble();
            else if (raw.isString()) {
                const auto match = QRegularExpression("^\\s*([+-]?[0-9]+(?:[.,][0-9]+)?)\\s*(.*?)\\s*$").match(raw.toString());
                if (match.hasMatch()) { value = QString(match.captured(1)).replace(',', '.').toDouble(&ok); unit = match.captured(2); }
            }
            if (unit.isEmpty()) unit = QHash<QString, QString>{{"Temperature", "°C"}, {"Clock", "MHz"}, {"Load", "%"}, {"Fan", "RPM"},
                {"Power", "W"}, {"Voltage", "V"}, {"SmallData", "MB"}, {"Data", "GB"}}.value(kind);
            if (kind == "Temperature" && unit == QString::fromUtf8("°F")) { value = (value - 32.) / 1.8; unit = "°C"; }
            auto group = category(id.section('/', 1, 1));
            if (ok && std::isfinite(value)) result.readings.append({id, id.section('/', 0, 2) + " / " + device, group,
                node["Text"].toString(), kind == "SmallData" ? QString("Data") : kind, unit, value});
        }
        for (const auto &child : node["Children"].toArray()) visit(child.toObject(), device, depth + 1);
    };
    visit(json.object(), {}, 0);
    if (result.readings.isEmpty()) result.error = "LibreHardwareMonitor: no sensor readings; enable hardware monitoring and check driver/administrator permissions";
    return result;
}
void applyHardwareReadings(Snapshot &s, const HardwareReadings &data, const QString &source, int gpuIndex) {
    const QStringList managed{"CpuTemp", "CpuFreq", "CpuPower", "CpuFan", "CpuPump", "CpuVoltage", "GpuModel", "GpuLoad", "GpuTemp", "GpuFreq", "GpuPower",
        "GpuFan", "GpuVoltage", "GpuRam", "GpuRamTotal", "GpuRamFree", "GpuRamLoad", "GpuRamUsage", "HDDTemp"};
    for (const auto &key : managed) {
        s.values.remove(key); s.strings.remove(key); s.sources.remove(key);
        s.unavailable[key] = data.error.isEmpty() ? source + ": selected hardware does not expose this sensor" : data.error;
    }
    if (!data.error.isEmpty()) return;
    QStringList gpus;
    for (const auto &r : data.readings) if (r.category == "GPU") {
        // HWiNFO can expose several sections for one physical GPU.
        const auto group = r.device.startsWith("GPU #") ? r.device.section(" / ", 0, 0) : r.device;
        if (!gpus.contains(group)) gpus.append(group);
    }
    const auto gpu = gpus.value(gpuIndex);
    QHash<QString, int> scores;
    auto put = [&](const QString &key, const HardwareReading &r, int score, double value) {
        if (!std::isfinite(value) || value < 0 || score < scores.value(key, -1)) return;
        if (score == scores.value(key, -1) && key == "CpuFreq" && value <= s.values.value(key)) return;
        if (score == scores.value(key, -1) && key != "CpuFreq") return;
        scores[key] = score; s.values[key] = value;
        s.sources[key] = source + " / " + r.device + " / " + r.name;
        s.unavailable.remove(key);
    };
    for (const auto &r : data.readings) {
        const auto n = QString(r.name.toLower()).replace(QRegularExpression("gpu\\d+"), "gpu");
        const auto id = r.id.toUpper();
        const bool cpu = r.category == "CPU";
        const auto group = r.device.startsWith("GPU #") ? r.device.section(" / ", 0, 0) : r.device;
        const bool selectedGpu = r.category == "GPU" && !gpu.isEmpty() && group == gpu;
        if (selectedGpu && !s.strings.contains("GpuModel") && !QRegularExpression("^GPU #\\d+$").match(r.device).hasMatch()) {
            s.strings["GpuModel"] = r.device.section(" / ", -1);
            s.sources["GpuModel"] = source; s.unavailable.remove("GpuModel");
        }
        if (r.kind == "Temperature") {
            if (cpu && containsAny(n, {"package", "tctl/tdie", "cpu die", "cpu (tctl", "cpu (tdie"})) put("CpuTemp", r, 100, r.value);
            else if (cpu && containsAny(n, {"core", "cpu"})) put("CpuTemp", r, 50, r.value);
            if (selectedGpu && containsAny(n, {"core", "temperature", "gpu", "edge"}) && !containsAny(n, {"hot spot", "hotspot", "memory", "junction", "vrm", "liquid", "ambient"})) put("GpuTemp", r, 100, r.value);
            if (r.category == "Disk") put("HDDTemp", r, 50, r.value);
        } else if (r.kind == "Clock") {
            double value = r.value * (r.unit == "GHz" ? 1000. : r.unit == "Hz" ? 0.000001 : r.unit == "kHz" ? 0.001 : 1.);
            if (cpu && !containsAny(n, {"bus", "uncore", "ring", "fabric", "memory", "effective", "average"}) &&
                (containsAny(n, {"core", "cpu clock"}) || id == "SCPUCLK" || id.startsWith("SCC-"))) put("CpuFreq", r, 100, value);
            if (selectedGpu && containsAny(n, {"core", "gpu clock", "gpu engine"}) && !n.contains("memory")) put("GpuFreq", r, 100, value);
        } else if (r.kind == "Load") {
            if (cpu && (containsAny(n, {"total cpu", "cpu total", "cpu utilization", "cpu usage"}) || id == "SCPUUTI")) put("CpuLoad", r, 100, r.value);
            if (selectedGpu && containsAny(n, {"gpu core", "gpu utilization", "gpu usage", "gpu d3d", "gpu load"})) put("GpuLoad", r, n.contains("d3d") ? 50 : 100, r.value);
            if (selectedGpu && containsAny(n, {"memory usage", "memory load", "memory utilization"})) put("GpuRamLoad", r, 50, r.value);
        } else if (r.kind == "Power") {
            if (cpu && containsAny(n, {"package", "ppt", "cpu power"})) put("CpuPower", r, n.contains("package") ? 100 : 80, r.value);
            if (selectedGpu && containsAny(n, {"gpu", "board", "total"})) put("GpuPower", r, containsAny(n, {"board", "total"}) ? 100 : 50, r.value);
        } else if (r.kind == "Fan" && r.unit.compare("RPM", Qt::CaseInsensitive) == 0) {
            if ((cpu || r.category == "Board") && n.contains("cpu")) put("CpuFan", r, 100, r.value);
            if ((cpu || r.category == "Board") && containsAny(n, {"pump", "aio"})) put("CpuPump", r, 100, r.value);
            if (selectedGpu) put("GpuFan", r, 100, r.value);
        } else if (r.kind == "Voltage") {
            if ((cpu || r.category == "Board") && containsAny(n, {"vcore", "cpu core", "core voltage"})) put("CpuVoltage", r, 100, r.value);
            if (selectedGpu && containsAny(n, {"core", "gpu voltage"})) put("GpuVoltage", r, 100, r.value);
        } else if (selectedGpu && (r.kind == "Data" || r.kind == "SmallData")) {
            if ((!containsAny(n, {"total", "free", "available"}) && containsAny(n, {"memory used", "dedicated memory", "used video memory", "memory allocated", "memory usage", "memory dedicated"})) || id.endsWith("USEDDEMEM")) put("GpuRam", r, 100, memoryBytes(r));
            if (containsAny(n, {"memory total", "total video memory", "total dedicated memory", "dedicated memory total"}) || (id.contains("DEMEM") && !id.contains("USED") && !id.contains("FREE"))) put("GpuRamTotal", r, 100, memoryBytes(r));
            if (containsAny(n, {"memory free", "free video memory"}) || id.endsWith("FREEDEMEM")) put("GpuRamFree", r, 100, memoryBytes(r));
        }
    }
    if (s.values.contains("GpuRam") && s.values.contains("GpuRamTotal") && s.values["GpuRamTotal"] > 0 && s.values["GpuRam"] <= s.values["GpuRamTotal"]) {
        const auto used = s.values["GpuRam"], total = s.values["GpuRamTotal"];
        s.values["GpuRamFree"] = total - used; s.values["GpuRamLoad"] = used / total * 100.;
        s.strings["GpuRamUsage"] = QString::number(used / 1073741824., 'f', 1) + " / " + QString::number(total / 1073741824., 'f', 1) + " GB";
        for (auto key : {"GpuRamFree", "GpuRamLoad", "GpuRamUsage"}) { s.sources[key] = source + " / GPU memory"; s.unavailable.remove(key); }
    }
}
namespace {
HardwareReadings readShared(const QString &source) {
    HardwareReadings result;
#ifdef Q_OS_WIN
    const auto name = source == "hwinfo" ? L"Global\\HWiNFO_SENS_SM2" : L"AIDA64_SensorValues";
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping) {
        result.error = source + (GetLastError() == ERROR_ACCESS_DENIED ? ": permission denied reading shared memory" :
            source == "hwinfo" ? ": start HWiNFO Sensors and enable Shared Memory Support (free edition: 12-hour limit)" :
                                 ": start AIDA64 and enable External Applications / shared memory");
        return result;
    }
    HANDLE mutex = source == "hwinfo" ? OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\HWiNFO_SM2_MUTEX") : nullptr;
    bool locked = false;
    if (mutex) {
        const auto status = WaitForSingleObject(mutex, 20);
        locked = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    }
    const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    QByteArray bytes;
    if (mutex && !locked) result.error = "HWiNFO: shared memory busy; retrying";
    else if (view) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(view, &info, sizeof(info)) && info.RegionSize <= size_t(maxBytes) && info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            bytes = QByteArray(static_cast<const char *>(view), qsizetype(info.RegionSize));
        else result.error = source + ": invalid shared memory region";
    } else result.error = source + ": cannot map shared memory";
    if (view) UnmapViewOfFile(view);
    if (locked) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    CloseHandle(mapping);
    if (result.error.isEmpty()) result = source == "hwinfo" ? parseHwinfo(bytes, QDateTime::currentSecsSinceEpoch()) : parseAida64(bytes);
#else
    result.error = source + ": shared memory collection is only available on Windows";
#endif
    return result;
}
}
WindowsHardware::WindowsHardware(QNetworkAccessManager *manager) : network(manager) { clock.start(); }
WindowsHardware::~WindowsHardware() {
    ++generation;
    if (active) { active->disconnect(this); active->abort(); }
}
void WindowsHardware::configure(const QString &value, int newPort, int index) {
    const auto normalized = windowsSensorSource(value);
    newPort = qBound(1, newPort, 65535); index = qMax(0, index);
    if (source == normalized && port == newPort && gpuIndex == index) return;
    ++generation;
    if (active) { active->abort(); active = nullptr; }
    source = normalized; port = newPort; gpuIndex = index;
    cached = {}; requested = -1000; received = -1;
}
void WindowsHardware::fetchLibre() {
    if (!network) {
        network = new QNetworkAccessManager(this);
        network->setProxy(QNetworkProxy::NoProxy);
    }
    QNetworkRequest request(QUrl("http://127.0.0.1:" + QString::number(port) + "/data.json"));
    request.setTransferTimeout(3000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    requested = clock.elapsed();
    auto reply = network->get(request);
    active = reply;
    const auto current = generation;
    connect(reply, &QNetworkReply::readyRead, reply, [reply] { if (reply->bytesAvailable() > maxBytes) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, current] {
        reply->deleteLater();
        if (generation != current) return;
        active = nullptr;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            cached = {}; cached.error = "LibreHardwareMonitor: enable Options / Remote Web Server on port " + QString::number(port) +
                (status == 401 ? "; HTTP 401: disable Web Server authentication for this local connection" : "; " + reply->errorString());
            received = -1;
        } else { cached = parseLibreHardwareMonitor(reply->readAll()); received = clock.elapsed(); }
    });
}
void WindowsHardware::apply(Snapshot &sample) {
    if (source == "system") return;
    HardwareReadings data;
    QString label;
    if (source == "librehardwaremonitor") {
        label = "LibreHardwareMonitor";
        if (!active && clock.elapsed() - requested >= 1000) fetchLibre();
        data = cached;
        if (received < 0 && data.error.isEmpty()) data.error = "LibreHardwareMonitor: connecting to local Web Server";
        else if (received >= 0 && clock.elapsed() - received > 10000) { data = {}; data.error = "LibreHardwareMonitor: sensor data stale"; }
    } else {
        label = source == "hwinfo" ? "HWiNFO" : "AIDA64";
        data = readShared(source);
    }
    applyHardwareReadings(sample, data, label, gpuIndex);
}
} // namespace Screen
