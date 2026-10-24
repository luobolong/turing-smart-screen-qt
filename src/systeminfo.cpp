#include "systeminfo.h"
#include <QRegularExpression>
#include <QSysInfo>
#include <QVersionNumber>
#ifdef Q_OS_WIN
#include <QSettings>
#endif

namespace Screen {
SystemIdentity describeSystem(const QString &productType, const QString &productVersion,
                              const QString &prettyName, const QString &kernelType,
                              const QString &kernelVersion, const QString &displayVersion) {
    SystemIdentity result;
    char32_t glyph = 0xF0379; // Generic system / monitor.
    const auto type = productType.toLower();
    const auto kernel = kernelType.toLower();
    auto kernelLabel = kernelType;
    result.name = prettyName.section(" (", 0, 0).trimmed();
    if (type == "windows" || kernel == "winnt") {
        glyph = 0xF05B3;
        kernelLabel = "NT";
        auto version = productVersion.trimmed();
        const bool server = version.startsWith("server", Qt::CaseInsensitive) ||
                            prettyName.contains("Windows Server", Qt::CaseInsensitive);
        const auto nt = QVersionNumber::fromString(kernelVersion);
        if (server) {
            const auto year = QRegularExpression("(\\d{4})").match(version + ' ' + prettyName);
            version = year.hasMatch() ? "Server " + year.captured(1) : "Server";
        } else if (nt.majorVersion() == 10 &&
                   (version == "10" || version == "11" || version == "unknown" || version.isEmpty())) {
            // Both Windows 10 and 11 use NT 10.0; do not rely on the registry's
            // ProductName, which can still say Windows 10 on Windows 11.
            version = nt.microVersion() >= 22000 ? "11" : "10";
        } else if (!QRegularExpression("^\\d+(?:\\.\\d+)?$").match(version).hasMatch()) {
            version.clear();
        }
        result.name = "Windows" + (version.isEmpty() ? QString() : ' ' + version);
        const auto release = displayVersion.trimmed().toUpper();
        if (QRegularExpression("^(?:\\d{2}H[12]|\\d{4})$").match(release).hasMatch())
            result.name += ' ' + release;
    } else if (kernel == "linux") {
        glyph = type == "nixos" ? 0xF313 : 0xF033D;
        kernelLabel = "Linux";
    } else if (kernel == "darwin") {
        glyph = 0xF0035;
        kernelLabel = "Darwin";
    }
    if (result.name.isEmpty() || result.name == "unknown")
        result.name = kernelLabel;
    result.kernel = kernelVersion.isEmpty() ? QString("--") : kernelLabel + ' ' + kernelVersion;
    result.icon = QString::fromUcs4(&glyph, 1);
    return result;
}

SystemIdentity currentSystemIdentity() {
    static const auto identity = [] {
        auto kernel = QSysInfo::kernelVersion();
        QString release;
#ifdef Q_OS_WIN
        // Read the native registry view even when running a 32-bit executable.
        QSettings version("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          QSettings::Registry64Format);
        release = version.value("DisplayVersion").toString();
        const auto nt = QVersionNumber::fromString(kernel);
        if (release.isEmpty() && nt.majorVersion() == 10 && nt.microVersion() < 22000)
            release = version.value("ReleaseId").toString();
        bool buildValid = false, revisionValid = false;
        const auto build = version.value("CurrentBuildNumber").toUInt(&buildValid);
        const auto revision = version.value("UBR").toUInt(&revisionValid);
        if (buildValid && revisionValid && nt.segmentCount() == 3 && nt.microVersion() >= 0 &&
            build == quint32(nt.microVersion()))
            kernel += '.' + QString::number(revision);
#endif
        return describeSystem(QSysInfo::productType(), QSysInfo::productVersion(),
                              QSysInfo::prettyProductName(), QSysInfo::kernelType(), kernel, release);
    }();
    return identity;
}
} // namespace Screen
