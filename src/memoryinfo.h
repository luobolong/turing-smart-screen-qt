#pragma once
#include <QByteArray>
#include <QString>
namespace Screen {
struct MemoryInfo {
    QString specification, error;
};
// SMBIOS table bytes without the Windows RawSMBIOSData wrapper.
MemoryInfo parseMemoryInfo(const QByteArray &table);
MemoryInfo parseUdevMemoryInfo(const QByteArray &database);
MemoryInfo currentMemoryInfo(const QString &sysfsRoot = QStringLiteral("/sys"), const QString &udevDataFile = {});
} // namespace Screen
