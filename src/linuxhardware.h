#pragma once
#include <QHash>
#include <QString>
namespace Screen {
struct Snapshot;
// Internal sysfs collector; paths and elapsed time are injectable for hardware-independent tests.
void collectLinuxHardware(Snapshot &sample, const QString &root, const QByteArray &diskDevice,
                          double seconds, QHash<QString, double> &previousEnergy);
} // namespace Screen
