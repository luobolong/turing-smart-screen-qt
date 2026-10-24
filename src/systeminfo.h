#pragma once
#include <QString>

namespace Screen {
struct SystemIdentity {
    QString name, kernel, icon;
};
SystemIdentity describeSystem(const QString &productType, const QString &productVersion,
                              const QString &prettyName, const QString &kernelType,
                              const QString &kernelVersion, const QString &displayVersion = {});
SystemIdentity currentSystemIdentity();
} // namespace Screen
