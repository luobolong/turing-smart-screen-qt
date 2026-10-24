#include "linuxhardware.h"
#include "sensors.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <cmath>
namespace Screen {
static QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed() : QByteArray();
}
static bool number(const QString &path, double &value) {
    bool ok = false;
    const auto parsed = readFile(path).toDouble(&ok);
    if (!ok || !std::isfinite(parsed) || parsed < 0)
        return false;
    value = parsed;
    return true;
}
static QSet<QString> storageParents(const QString &root, QString device, QSet<QString> &visited) {
    const auto canonical = QFileInfo(device).canonicalFilePath();
    if (!canonical.isEmpty())
        device = canonical;
    auto block = QFileInfo(root + "/class/block/" + QFileInfo(device).fileName()).canonicalFilePath();
    if (block.isEmpty() || visited.contains(block))
        return {};
    visited.insert(block);
    if (QFile::exists(block + "/partition"))
        block = QFileInfo(block).dir().path();
    QSet<QString> result;
    const auto parent = QFileInfo(block + "/device").canonicalFilePath();
    if (!parent.isEmpty())
        result.insert(parent);
    for (const auto &slave : QDir(block + "/slaves").entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        result.unite(storageParents(root, slave, visited));
    return result;
}
void collectLinuxHardware(Snapshot &s, const QString &root, const QByteArray &diskDevice,
                          double seconds, QHash<QString, double> &previousEnergy) {
    QSet<QString> visited;
    const auto parents = storageParents(root, QString::fromUtf8(diskDevice), visited);
    const auto hwmon = root + "/class/hwmon";
    for (const auto &dir : QDir(hwmon).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = hwmon + '/' + dir;
        const auto name = readFile(base + "/name");
        const QString source = "hwmon / " + QString::fromUtf8(name);
        double value = 0;
        const bool cpu = name == "coretemp" || name == "k10temp" || name == "zenpower" ||
                         name == "k8temp" || name == "cpu_thermal";
        if (cpu) {
            if (number(base + "/temp1_input", value)) {
                if (!s.values.contains("CpuTemp") || value / 1000. > s.values["CpuTemp"])
                    s.values["CpuTemp"] = value / 1000.;
                s.sources["CpuTemp"] = source;
            }
            if (number(base + "/power1_average", value) || number(base + "/power1_input", value)) {
                s.values["CpuPower"] += value / 1000000.;
                s.sources["CpuPower"] = source;
            }
        }
        if (name == "amdgpu" || name == "radeon" || name == "nouveau" || name.startsWith("nvme") ||
            name == "drivetemp") {
            if (name.startsWith("nvme") || name == "drivetemp") {
                const auto realPath = QFileInfo(base).canonicalFilePath();
                for (const auto &parent : parents) {
                    if (realPath.startsWith(parent + '/') && number(base + "/temp1_input", value)) {
                        if (!s.values.contains("HDDTemp") || value / 1000. > s.values["HDDTemp"])
                            s.values["HDDTemp"] = value / 1000.;
                        s.sources["HDDTemp"] = source + " / " + QString::fromUtf8(diskDevice);
                    }
                }
            }
            continue;
        }
        for (const auto &file : QDir(base).entryList({"fan*_input", "in*_input"}, QDir::Files)) {
            const auto labelPath = base + '/' + file.left(file.size() - 6) + "_label";
            const auto label = readFile(labelPath).toLower();
            const auto metric = file.startsWith("fan") && label.contains("pump") ? "CpuPump"
                              : file.startsWith("fan") && label.contains("cpu") ? "CpuFan"
                              : file.startsWith("in") && (label == "vcore" || label == "cpu vcore" ||
                                                         label == "vddcr_cpu") ? "CpuVoltage"
                                                                               : "";
            if (*metric && number(base + '/' + file, value)) {
                s.values[metric] = value / (file.startsWith("in") ? 1000. : 1.);
                s.sources[metric] = source + " / " + QString::fromUtf8(label);
            }
        }
    }
    // RAPL reports cumulative energy, so power requires two successful samples.
    QHash<QString, double> energy;
    double watts = 0;
    int packages = 0, measured = 0;
    const auto powercap = root + "/class/powercap";
    for (const auto &dir : QDir(powercap).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto base = powercap + '/' + dir;
        if (!readFile(base + "/name").startsWith("package-"))
            continue;
        ++packages;
        double current = 0;
        if (!number(base + "/energy_uj", current)) {
            s.unavailable["CpuPower"] = QFile::exists(base + "/energy_uj")
                ? "CPU energy counter is not readable (permission or driver restriction)"
                : "Driver does not expose CPU energy";
            continue;
        }
        energy[base] = current;
        if (!previousEnergy.contains(base))
            continue;
        double delta = current - previousEnergy[base];
        if (delta < 0) {
            double range = 0;
            if (!number(base + "/max_energy_range_uj", range) || range <= 0 ||
                current >= range || previousEnergy[base] >= range)
                continue;
            delta += range;
        }
        if (seconds > 0 && delta >= 0) {
            watts += delta / 1000000. / seconds;
            ++measured;
        }
    }
    previousEnergy = energy;
    if (!s.values.contains("CpuPower") && packages > 0 && measured == packages) {
        s.values["CpuPower"] = watts;
        s.sources["CpuPower"] = "sysfs / powercap (package energy delta)";
        s.unavailable.remove("CpuPower");
    } else if (!s.values.contains("CpuPower") && !energy.isEmpty() && measured == 0) {
        s.unavailable["CpuPower"] = "Waiting for a second CPU energy sample";
    }
}
} // namespace Screen
