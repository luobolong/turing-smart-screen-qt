#include "memoryinfo.h"
#include <QFile>
#include <QHash>
#include <QtEndian>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace Screen {
MemoryInfo parseMemoryInfo(const QByteArray &table) {
    const auto invalid = [] { return MemoryInfo{{}, "SMBIOS memory table is truncated or invalid"}; };
    if (table.isEmpty() || table.size() > 16 * 1024 * 1024) return invalid();
    const QHash<quint8, QString> types{{0x12, "DDR"}, {0x13, "DDR2"}, {0x14, "DDR2 FB-DIMM"},
        {0x18, "DDR3"}, {0x1a, "DDR4"}, {0x1b, "LPDDR"}, {0x1c, "LPDDR2"},
        {0x1d, "LPDDR3"}, {0x1e, "LPDDR4"}, {0x22, "DDR5"}, {0x23, "LPDDR5"}};
    QString type;
    quint32 speed = 0;
    bool populated = false, incomplete = false;
    for (qsizetype pos = 0; pos < table.size();) {
        if (table.size() - pos < 4) return invalid();
        const auto record = table.constData() + pos;
        const auto kind = quint8(record[0]), length = quint8(record[1]);
        if (length < 4 || pos + length > table.size()) return invalid();
        const auto end = table.indexOf(QByteArray(2, '\0'), pos + length);
        if (end < 0) return invalid();
        if (kind == 127) break;
        if (kind == 17 && length >= 0x15 && qFromLittleEndian<quint16>(record + 0x0c) != 0) {
            populated = true;
            const auto moduleType = types.value(quint8(record[0x12]));
            // Configured speed is the operating data rate. Speed at 0x15 is
            // the module's rated capability and must not substitute for it.
            quint32 moduleSpeed = length >= 0x22 ? qFromLittleEndian<quint16>(record + 0x20) : 0;
            if (moduleSpeed == 0xffff)
                moduleSpeed = length >= 0x5c ? qFromLittleEndian<quint32>(record + 0x58) : 0;
            if (moduleType.isEmpty() || moduleSpeed == 0 || moduleSpeed == 0xffffffff) incomplete = true;
            else {
                if ((!type.isEmpty() && type != moduleType) || (speed && speed != moduleSpeed))
                    return {{}, "SMBIOS memory devices report different DDR types or configured speeds"};
                type = moduleType; speed = moduleSpeed;
            }
        }
        pos = end + 2;
    }
    if (!populated || incomplete || type.isEmpty() || !speed)
        return {{}, "SMBIOS does not report DDR type and configured memory speed for all installed modules"};
    // Preserve the MHz label requested by the dashboard; SMBIOS defines this
    // number as the DDR effective transfer rate (MT/s), not the base clock.
    return {type + '/' + QString::number(speed) + "MHz", {}};
}
MemoryInfo currentMemoryInfo(const QString &sysfsRoot) {
#ifdef Q_OS_WIN
    Q_UNUSED(sysfsRoot)
    constexpr DWORD provider = 0x52534d42; // 'RSMB'
    const auto size = GetSystemFirmwareTable(provider, 0, nullptr, 0);
    if (size < 8 || size > 16 * 1024 * 1024) return {{}, "Windows does not expose SMBIOS memory information"};
    QByteArray bytes(qsizetype(size), '\0');
    if (GetSystemFirmwareTable(provider, 0, bytes.data(), size) != size)
        return {{}, "Cannot read Windows SMBIOS memory information"};
    const auto length = qFromLittleEndian<quint32>(bytes.constData() + 4);
    if (length > size - 8) return {{}, "Windows SMBIOS table length is invalid"};
    return parseMemoryInfo(bytes.mid(8, length));
#elif defined(Q_OS_LINUX)
    QFile file(sysfsRoot + "/firmware/dmi/tables/DMI");
    if (!file.open(QIODevice::ReadOnly))
        return {{}, "SMBIOS memory information unavailable (DMI read permission or firmware support)"};
    return parseMemoryInfo(file.read(16 * 1024 * 1024 + 1));
#else
    Q_UNUSED(sysfsRoot)
    return {{}, "SMBIOS memory information is not available on this platform"};
#endif
}
} // namespace Screen
