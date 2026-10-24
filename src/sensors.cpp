#include "sensors.h"
#include "model.h"
#include "linuxhardware.h"
#include "systeminfo.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QStorageInfo>
#include <QThread>
#include <QSysInfo>
#include <QLocale>
#include <cmath>
#include <limits>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <powrprof.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace Screen {
void resourceTrace(const char *stage) {
    auto file = qEnvironmentVariable("TURING_QT_TRACE");
    if (file.isEmpty())
        return;
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        auto o = processStats().json();
        o["stage"] = QString::fromLatin1(stage);
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.write("\n");
    }
}
#ifdef Q_OS_WIN
struct CpuPowerInfo {
    ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};
#endif
Snapshot Snapshot::fromJson(const QJsonObject &o) {
    Snapshot s;
    auto v = o["values"].toObject();
    for (auto i = v.begin(); i != v.end(); ++i)
        s.values.insert(i.key(), i.value().toDouble());
    auto t = o["strings"].toObject();
    for (auto i = t.begin(); i != t.end(); ++i)
        s.strings.insert(i.key(), i.value().toString());
    for (const auto &key : {"sources", "unavailable"}) {
        const auto entries = o[key].toObject();
        auto &target = QString::fromLatin1(key) == "sources" ? s.sources : s.unavailable;
        for (auto i = entries.begin(); i != entries.end(); ++i)
            target[i.key()] = i.value().toString();
    }
    s.timestamp = o["timestamp"].toString();
    return s;
}
QJsonObject Snapshot::json() const {
    QJsonObject v, s;
    for (auto i = values.begin(); i != values.end(); ++i)
        v[i.key()] = i.value();
    for (auto i = strings.begin(); i != strings.end(); ++i)
        s[i.key()] = i.value();
    QJsonObject sourcesJson, unavailableJson;
    for (auto i = sources.begin(); i != sources.end(); ++i)
        sourcesJson[i.key()] = i.value();
    for (auto i = unavailable.begin(); i != unavailable.end(); ++i)
        unavailableJson[i.key()] = i.value();
    return {{"values", v}, {"strings", s}, {"timestamp", timestamp},
            {"sources", sourcesJson}, {"unavailable", unavailableJson}};
}
QString Snapshot::text(const QString &metric, bool bare) const {
    auto k = canonical(metric);
    if (strings.contains(k))
        return strings.value(k);
    if (!values.contains(k))
        return "--";
    double v = values.value(k);
    QString unit;
    int decimals = 0;
    if (k.endsWith("Temp"))
        unit = "°C";
    else if (k.endsWith("Load") || k == "Volume")
        unit = "%";
    else if (k.endsWith("Freq")) {
        v /= 1000;
        unit = "GHz";
        decimals = 1;
    } else if (k == "RamValue" || k == "RamValid" || k == "RamTotal") {
        v /= 1073741824.;
        unit = "GB";
        decimals = 1;
    } else if (k.startsWith("Gpu") && k.contains("Ram")) {
        v /= 1048576.;
        unit = "MB";
    } else if (k.startsWith("Internet")) {
        v /= 1024;
        unit = "KB/s";
        decimals = 1;
    } else if (k.endsWith("Power"))
        unit = "W";
    else if (k.endsWith("Fan") || k == "CpuPump")
        unit = "RPM";
    else if (k.endsWith("Voltage")) {
        unit = "V";
        decimals = 1;
    }
    return QString::number(v, 'f', decimals) + (bare || unit.isEmpty() ? QString() : " " + unit);
}
double Snapshot::percentage(const QString &metric) const {
    auto k = canonical(metric);
    double v = values.value(k, 0.);
    if (k == "CpuFreq")
        v /= 50.;
    else if (k == "GpuFreq")
        v /= 30.;
    return qBound(0., v, 100.);
}
Collector::Collector(const QString &sysfsRoot, const QString &procRoot) : sysfsRoot(sysfsRoot), procRoot(procRoot) {
    elapsed.start();
    setAudioEnabled(true);
}
void Collector::setWeather(bool enabled, const QString &city) {
    if (!enabled) { weather.reset(); return; }
    if (!weather) weather = std::make_unique<WeatherClient>();
    weather->configure(true, city);
}
void Collector::setAudioEnabled(bool enabled) {
    if (!enabled) audio.reset();
    else if (!audio) audio = std::make_unique<AudioVolume>();
}
#ifndef Q_OS_WIN
static QByteArray read(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
#endif
static void collectLocalIp(Snapshot &s, const QString &procRoot) {
    QString defaultInterface;
#ifdef Q_OS_WIN
    Q_UNUSED(procRoot)
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = 0x01010101; // Route lookup only; no packets are sent.
    DWORD index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr *>(&destination), &index) == NO_ERROR)
        defaultInterface = QNetworkInterface::interfaceFromIndex(int(index)).name();
#else
    quint64 lowestMetric = std::numeric_limits<quint64>::max();
    for (const auto &line : read(procRoot + "/net/route").split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 7 || fields[1] != "00000000")
            continue;
        bool flagsValid = false, metricValid = false;
        const auto flags = fields[3].toUInt(&flagsValid, 16);
        const auto metric = fields[6].toULongLong(&metricValid);
        if (flagsValid && metricValid && (flags & 1) && metric < lowestMetric) {
            defaultInterface = QString::fromUtf8(fields[0]);
            lowestMetric = metric;
        }
    }
#endif
    int bestScore = -1;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            const auto address = entry.ip();
            if (address.isNull() || address.isLoopback() || address.isLinkLocal() || address.isMulticast())
                continue;
            int score = address.protocol() == QAbstractSocket::IPv4Protocol ? 10000 : 0;
            if (interface.name() == defaultInterface)
                score += 1000;
            if (interface.type() == QNetworkInterface::Ethernet || interface.type() == QNetworkInterface::Wifi)
                score += 100;
            if (score > bestScore) {
                bestScore = score;
                s.strings["LocalIp"] = address.toString();
                s.sources["LocalIp"] = "Active network interface: " + interface.name();
            }
        }
    }
    if (bestScore < 0)
        s.unavailable["LocalIp"] = "No active network address";
}
#ifdef Q_OS_LINUX
static bool readNumber(const QString &path, double &value) {
    bool ok = false;
    const double parsed = read(path).trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || parsed < 0)
        return false;
    value = parsed;
    return true;
}
static void collectAmdGpu(Snapshot &s, const QString &sysfsRoot) {
    const QString drm = sysfsRoot + "/class/drm";
    QString device;
    bool primary = false;
    double largestMemory = 0;
    for (const auto &card : QDir(drm).entryList({"card*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        // Connector directories (card0-DP-1, etc.) are not GPU devices.
        bool isCard = false;
        card.mid(4).toUInt(&isCard);
        if (!isCard)
            continue;
        const QString base = drm + '/' + card + "/device";
        if (read(base + "/vendor").trimmed() != "0x1002" ||
            !read(base + "/uevent").split('\n').contains("DRIVER=amdgpu"))
            continue;
        const bool boot = read(base + "/boot_vga").trimmed() == "1";
        double memory = 0;
        readNumber(base + "/mem_info_vram_total", memory);
        // Prefer the boot/display GPU, then VRAM capacity, rather than card0.
        if (device.isEmpty() || (boot && !primary) || (boot == primary && memory > largestMemory)) {
            device = base;
            primary = boot;
            largestMemory = memory;
        }
    }
    if (device.isEmpty())
        return;
    double value = 0;
    if (readNumber(device + "/gpu_busy_percent", value) && value <= 100)
        s.values["GpuLoad"] = value;
    if (readNumber(device + "/mem_info_vram_used", value))
        s.values["GpuRam"] = value;
    if (readNumber(device + "/mem_info_vram_total", value) && value > 0) {
        s.values["GpuRamTotal"] = value;
        if (s.values.contains("GpuRam")) {
            s.values["GpuRamFree"] = qMax(0., value - s.values["GpuRam"]);
            s.values["GpuRamLoad"] = qBound(0., 100. * s.values["GpuRam"] / value, 100.);
            s.strings["GpuRamUsage"] = QString::number(s.values["GpuRam"] / 1073741824., 'f', 1) + " / " +
                                       QString::number(value / 1073741824., 'f', 1) + " GB";
        }
    }
    s.strings["GpuModel"] = QString::fromUtf8(read(device + "/product_name").trimmed());
    if (s.strings["GpuModel"].isEmpty())
        s.strings["GpuModel"] = "AMD GPU (1002:" + QString::fromUtf8(read(device + "/device").trimmed().mid(2)) + ')';
    const QString hwmon = device + "/hwmon";
    for (const auto &dir : QDir(hwmon).entryList({"hwmon*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = hwmon + '/' + dir;
        if (read(base + "/name").trimmed() != "amdgpu")
            continue;
        if (readNumber(base + "/temp1_input", value))
            s.values["GpuTemp"] = value / 1000.;
        if (readNumber(base + "/freq1_input", value))
            s.values["GpuFreq"] = value / 1000000.; // hwmon Hz -> snapshot MHz
        if (readNumber(base + "/power1_average", value) || readNumber(base + "/power1_input", value))
            s.values["GpuPower"] = value / 1000000.;
        if (readNumber(base + "/fan1_input", value))
            s.values["GpuFan"] = value;
        if (readNumber(base + "/in0_input", value))
            s.values["GpuVoltage"] = value / 1000.;
        break;
    }
    if (!s.values.contains("GpuFreq")) {
        for (const auto &line : read(device + "/pp_dpm_sclk").split('\n')) {
            if (!line.trimmed().endsWith('*'))
                continue;
            auto frequency = line.simplified().split(' ').value(1).toLower();
            if (!frequency.endsWith("mhz"))
                continue;
            bool ok = false;
            value = frequency.chopped(3).toDouble(&ok);
            if (ok && std::isfinite(value) && value >= 0)
                s.values["GpuFreq"] = value;
            break;
        }
    }
    for (auto i = s.values.cbegin(); i != s.values.cend(); ++i)
        if (i.key().startsWith("Gpu"))
            s.sources[i.key()] = "amdgpu / " + QFileInfo(device).dir().dirName();
    s.sources["GpuModel"] = "amdgpu / PCI ID";
    if (s.strings.contains("GpuRamUsage"))
        s.sources["GpuRamUsage"] = "amdgpu / " + QFileInfo(device).dir().dirName();
}
#endif
Snapshot Collector::collect(const QString &disk) {
    Snapshot s;
    const double seconds = qMax(.001, elapsed.restart() / 1000.);
    quint64 total = 0, idle = 0, up = 0, down = 0;
#ifdef Q_OS_WIN
    FILETIME i, k, u;
    auto time = [](FILETIME f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    if (GetSystemTimes(&i, &k, &u)) {
        idle = time(i);
        total = time(k) + time(u);
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        s.values["RamValue"] = double(memory.ullTotalPhys - memory.ullAvailPhys);
        s.values["RamValid"] = double(memory.ullAvailPhys);
        s.values["RamTotal"] = double(memory.ullTotalPhys);
        s.values["RamLoad"] = memory.dwMemoryLoad;
        s.strings["RamModel"] = QString::number(double(memory.ullTotalPhys) / 1073741824., 'f', 0) + " GB";
    }
    MIB_IF_TABLE2 *table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR) {
        for (ULONG n = 0; n < table->NumEntries; ++n) {
            const auto &row = table->Table[n];
            if (row.Type != IF_TYPE_SOFTWARE_LOOPBACK && row.Type != IF_TYPE_TUNNEL &&
                row.OperStatus == IfOperStatusUp) {
                up += row.OutOctets;
                down += row.InOctets;
            }
        }
        FreeMibTable(table);
    }
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    QVector<CpuPowerInfo> powers(int(info.dwNumberOfProcessors));
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, powers.data(),
                               ULONG(powers.size() * sizeof(CpuPowerInfo))) == 0 &&
        !powers.isEmpty())
        s.values["CpuFreq"] = powers[0].CurrentMhz;
    wchar_t name[256];
    DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS)
        s.strings["CpuModel"] = QString::fromWCharArray(name).trimmed();
#else
    auto stat = read(procRoot + "/stat").split('\n').value(0).simplified().split(' ');
    for (int n = 1; n < qMin(9, int(stat.size())); ++n)
        total += stat[n].toULongLong();
    idle = stat.value(4).toULongLong() + stat.value(5).toULongLong();
    QHash<QByteArray, quint64> memory;
    for (auto line : read(procRoot + "/meminfo").split('\n')) {
        auto parts = line.simplified().split(' ');
        if (parts.size() > 1)
            memory[parts[0]] = parts[1].toULongLong() * 1024;
    }
    double all = double(memory.value("MemTotal:")), available = double(memory.value("MemAvailable:"));
    if (all > 0) {
        s.values["RamValue"] = all - available;
        s.values["RamValid"] = available;
        s.values["RamTotal"] = all;
        s.values["RamLoad"] = (all - available) * 100 / all;
        s.strings["RamModel"] = QString::number(all / 1073741824., 'f', 0) + " GB";
    }
    for (auto line : read(procRoot + "/net/dev").split('\n')) {
        auto p = line.split(':');
        if (p.size() != 2 || p[0].trimmed() == "lo")
            continue;
        auto c = p[1].simplified().split(' ');
        down += c.value(0).toULongLong();
        up += c.value(8).toULongLong();
    }
    const auto cpu = read(procRoot + "/cpuinfo");
    for (auto line : cpu.split('\n')) {
        int at = line.indexOf(':');
        if (at < 0)
            continue;
        auto key = line.left(at).trimmed();
        if (key == "model name")
            s.strings["CpuModel"] = QString::fromUtf8(line.mid(at + 1).trimmed());
        if (key == "cpu MHz")
            s.values["CpuFreq"] = line.mid(at + 1).trimmed().toDouble();
        if (s.values.contains("CpuFreq") && s.strings.contains("CpuModel"))
            break;
    }
#endif
#ifdef Q_OS_LINUX
    collectAmdGpu(s, sysfsRoot);
#endif
    if (previousTotal && total > previousTotal && idle >= previousIdle)
        s.values["CpuLoad"] = qBound(0.,
                                     100. * double((total - previousTotal) - (idle - previousIdle)) /
                                         double(total - previousTotal),
                                     100.);
    if (previousTotal) {
        if (up >= previousUp)
            s.values["InternetUp"] = double(up - previousUp) / seconds;
        if (down >= previousDown)
            s.values["InternetDown"] = double(down - previousDown) / seconds;
    }
    previousTotal = total;
    previousIdle = idle;
    previousUp = up;
    previousDown = down;
    QStorageInfo storage(disk.isEmpty() ? QDir::rootPath() : disk);
    if (storage.isReady() && storage.bytesTotal() > 0) {
        s.values["HDDLoad"] =
            100. * double(storage.bytesTotal() - storage.bytesAvailable()) / double(storage.bytesTotal());
    }
#ifdef Q_OS_LINUX
    collectLinuxHardware(s, sysfsRoot, storage.device(), seconds, previousEnergy);
#endif
    if (s.values.contains("RamValue") && s.values.contains("RamTotal")) {
        s.strings["RamUsage"] = QString::number(s.values["RamValue"] / 1073741824., 'f', 1) + " / " +
                                QString::number(s.values["RamTotal"] / 1073741824., 'f', 1) + " GB";
    }
    if (const auto volume = audio ? audio->sample() : std::optional<double>()) {
        s.values["Volume"] = *volume;
        s.sources["Volume"] = "System default audio output";
    } else {
        s.unavailable["Volume"] = "Audio service/default output unavailable or connecting";
    }
    auto now = QDateTime::currentDateTime();
    s.timestamp = now.toString(Qt::ISODateWithMs);
    s.strings["Time"] = now.toString("HH:mm");
    s.strings["TimeSeconds"] = now.toString("HH:mm:ss");
    s.strings["Date"] = now.toString("yyyy/M/d");
    s.strings["DateWeekday"] = now.toString("yyyy/MM/dd") + "  " +
                               QLocale(QLocale::English).dayName(now.date().dayOfWeek(), QLocale::ShortFormat);
    const auto system = currentSystemIdentity();
    s.strings["OsName"] = system.name;
    s.strings["KernelVersion"] = system.kernel;
    s.strings["OsIcon"] = system.icon;
    for (const auto &key : {"OsName", "KernelVersion", "OsIcon"})
        s.sources[key] = QSysInfo::kernelType() == "winnt" ? "Windows system API / CurrentVersion" : "System API";
    s.strings["Weekday"] = QStringList{QStringLiteral("星期一"), QStringLiteral("星期二"),
                                     QStringLiteral("星期三"), QStringLiteral("星期四"),
                                     QStringLiteral("星期五"), QStringLiteral("星期六"),
                                     QStringLiteral("星期日")}.at(now.date().dayOfWeek() - 1);
    collectLocalIp(s, procRoot);
    if (weather) weather->apply(s);
    else for (const auto &key : {"Weather", "WeatherCity", "WeatherTemp", "WeatherCondition", "WeatherIcon"})
        s.unavailable[key] = "Weather disabled; enable it in Settings";
    for (const auto &metric : metrics()) {
        if (s.values.contains(metric.first) || s.strings.contains(metric.first)) {
            if (!s.sources.contains(metric.first))
                s.sources[metric.first] = "System API";
            s.unavailable.remove(metric.first);
        } else if (!s.unavailable.contains(metric.first)) {
            s.unavailable[metric.first] = "Driver does not expose this sensor";
        }
    }
    return s;
}
QJsonObject ProcessStats::json() const {
    return {{"resident_bytes", double(resident)},
            {"peak_resident_bytes", double(peakResident)},
            {"private_bytes", double(privateBytes)},
            {"cpu_seconds", cpuSeconds},
            {"logical_processors", cores}};
}
ProcessStats processStats() {
    ProcessStats s;
    s.cores = qMax(1, QThread::idealThreadCount());
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&m),
                             sizeof(m))) {
        s.resident = m.WorkingSetSize;
        s.peakResident = m.PeakWorkingSetSize;
        s.privateBytes = m.PrivateUsage;
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        s.cpuSeconds = (double((quint64(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                        double((quint64(u.dwHighDateTime) << 32) | u.dwLowDateTime)) /
                       1e7;
#else
    for (auto line : read("/proc/self/status").split('\n')) {
        auto p = line.simplified().split(' ');
        if (p.size() < 2)
            continue;
        auto bytes = p[1].toULongLong() * 1024;
        if (p[0] == "VmRSS:")
            s.resident = bytes;
        else if (p[0] == "VmHWM:")
            s.peakResident = bytes;
        else if (p[0] == "RssAnon:")
            s.privateBytes = bytes;
    }
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) == 0)
        s.cpuSeconds = double(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
                       double(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
#endif
    return s;
}
} // namespace Screen
