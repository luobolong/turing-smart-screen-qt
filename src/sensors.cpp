#include "sensors.h"
#include "model.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStorageInfo>
#include <QThread>
#include <cmath>
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
    s.timestamp = o["timestamp"].toString();
    return s;
}
QJsonObject Snapshot::json() const {
    QJsonObject v, s;
    for (auto i = values.begin(); i != values.end(); ++i)
        v[i.key()] = i.value();
    for (auto i = strings.begin(); i != strings.end(); ++i)
        s[i.key()] = i.value();
    return {{"values", v}, {"strings", s}, {"timestamp", timestamp}};
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
    } else if (k == "RamValue" || k == "RamValid") {
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
Collector::Collector() {
    elapsed.start();
}
#ifndef Q_OS_WIN
static QByteArray read(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
#endif
Snapshot Collector::collect(const QString &disk) {
    Snapshot s;
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
    auto stat = read("/proc/stat").split('\n').value(0).simplified().split(' ');
    for (int n = 1; n < qMin(9, int(stat.size())); ++n)
        total += stat[n].toULongLong();
    idle = stat.value(4).toULongLong() + stat.value(5).toULongLong();
    QHash<QByteArray, quint64> memory;
    for (auto line : read("/proc/meminfo").split('\n')) {
        auto parts = line.simplified().split(' ');
        if (parts.size() > 1)
            memory[parts[0]] = parts[1].toULongLong() * 1024;
    }
    double all = double(memory.value("MemTotal:")), available = double(memory.value("MemAvailable:"));
    if (all > 0) {
        s.values["RamValue"] = all - available;
        s.values["RamValid"] = available;
        s.values["RamLoad"] = (all - available) * 100 / all;
        s.strings["RamModel"] = QString::number(all / 1073741824., 'f', 0) + " GB";
    }
    for (auto line : read("/proc/net/dev").split('\n')) {
        auto p = line.split(':');
        if (p.size() != 2 || p[0].trimmed() == "lo")
            continue;
        auto c = p[1].simplified().split(' ');
        down += c.value(0).toULongLong();
        up += c.value(8).toULongLong();
    }
    const auto cpu = read("/proc/cpuinfo");
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
    for (auto dir : QDir("/sys/class/hwmon").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString base = "/sys/class/hwmon/" + dir;
        auto name = read(base + "/name").trimmed();
        if (name == "coretemp" || name == "k10temp" || name == "zenpower") {
            bool ok = false;
            double v = read(base + "/temp1_input").trimmed().toDouble(&ok);
            if (ok)
                s.values["CpuTemp"] = v / 1000.;
        }
    }
#endif
    if (previousTotal && total > previousTotal && idle >= previousIdle)
        s.values["CpuLoad"] = qBound(0.,
                                     100. * double((total - previousTotal) - (idle - previousIdle)) /
                                         double(total - previousTotal),
                                     100.);
    double seconds = qMax(.001, elapsed.restart() / 1000.);
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
    auto now = QDateTime::currentDateTime();
    s.timestamp = now.toString(Qt::ISODateWithMs);
    s.strings["Time"] = now.toString("HH:mm");
    s.strings["Date"] = now.toString("yyyy/M/d");
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
