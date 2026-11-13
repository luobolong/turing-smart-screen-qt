#pragma once
#include <QByteArray>
#include <QString>
namespace Screen {
struct MemoryInfo {
    QString specification, error;
};
// SMBIOS table bytes without the Windows RawSMBIOSData wrapper.
MemoryInfo parseMemoryInfo(const QByteArray &table);
MemoryInfo currentMemoryInfo(const QString &sysfsRoot = QStringLiteral("/sys"));
} // namespace Screen
