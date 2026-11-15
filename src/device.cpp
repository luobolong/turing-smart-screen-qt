#include "device.h"
#include "framediff.h"
#include <QtEndian>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>
#include <stdexcept>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif
namespace Screen {
namespace Protocol {
QByteArray header(int x, int y, int ex, int ey, quint8 command) {
    for (int n : {x, y, ex, ey})
        if (n < 0 || n > 1023)
            throw std::runtime_error("Coordinate outside 10-bit range");
    quint64 packed = (quint64(x) << 30) | (quint64(y) << 20) | (quint64(ex) << 10) | quint64(ey);
    QByteArray b;
    for (int n = 4; n >= 0; --n)
        b.append(char((packed >> (n * 8)) & 255));
    b.append(char(command));
    return b;
}
QByteArray orientation(int value) {
    if (value < 0 || value > 3)
        throw std::runtime_error("Invalid orientation");
    bool landscape = value >= 2;
    int w = landscape ? 480 : 320, h = landscape ? 320 : 480;
    auto b = header(0, 0, 0, 0, 0x79);
    b.resize(16);
    for (int i = 6; i < 16; ++i)
        b[i] = 0;
    b[6] = char(100 + value);
    b[7] = char(w >> 8);
    b[8] = char(w & 255);
    b[9] = char(h >> 8);
    b[10] = char(h & 255);
    return b;
}
QByteArray mirror(bool value) {
    auto b = header(0, 0, 0, 0, 0x7a);
    b.resize(16);
    for (int i = 6; i < 16; ++i)
        b[i] = 0;
    b[6] = char(value);
    return b;
}
QByteArray brightness(int value) {
    return header((25500 - qBound(0, value, 100) * 255) / 100, 0, 0, 0, 0x6e);
}
QImage rgb565(const QImage &source) {
    if (source.isNull() || source.width() > 480 || source.height() > 480)
        throw std::runtime_error("Invalid bitmap");
    if (source.format() == QImage::Format_RGB16)
        return source;
    const auto rgba = source.convertToFormat(QImage::Format_RGBA8888);
    QImage result(source.size(), QImage::Format_RGB16);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto *p = rgba.constScanLine(y);
        auto *out = reinterpret_cast<quint16 *>(result.scanLine(y));
        for (int x = 0; x < rgba.width(); ++x)
            out[x] = quint16(((p[x * 4] & 0xf8) << 8) | ((p[x * 4 + 1] & 0xfc) << 3) |
                             (p[x * 4 + 2] >> 3));
    }
    return result;
}
QByteArray bitmap(const QImage &source, QPoint at) {
    const auto img = rgb565(source);
    auto b = header(at.x(), at.y(), at.x() + img.width() - 1, at.y() + img.height() - 1, 0xc5);
    b.resize(6 + img.width() * img.height() * 2);
    for (int y = 0; y < img.height(); ++y) {
        const auto *p = reinterpret_cast<const quint16 *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            qToLittleEndian(p[x], b.data() + 6 + (y * img.width() + x) * 2);
    }
    return b;
}
} // namespace Protocol
class NativeSerial {
public:
    ~NativeSerial() {
        close();
    }
    void open(const QString &name) {
        close();
#ifdef Q_OS_WIN
        QString path = "\\\\.\\" + name;
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open serial port");
        DCB d{};
        d.DCBlength = sizeof(d);
        if (!GetCommState(handle, &d))
            throw std::runtime_error("GetCommState failed");
        d.BaudRate = 115200;
        d.ByteSize = 8;
        d.Parity = NOPARITY;
        d.StopBits = ONESTOPBIT;
        d.fBinary = TRUE;
        d.fParity = FALSE;
        d.fOutxCtsFlow = FALSE;
        d.fOutxDsrFlow = FALSE;
        d.fDtrControl = DTR_CONTROL_ENABLE;
        d.fRtsControl = RTS_CONTROL_ENABLE;
        d.fOutX = FALSE;
        d.fInX = FALSE;
        if (!SetCommState(handle, &d))
            throw std::runtime_error("Cannot configure serial port");
        COMMTIMEOUTS t{};
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutConstant = 100;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.WriteTotalTimeoutConstant = 1500;
        SetCommTimeouts(handle, &t);
        PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
        fd = ::open(name.toLocal8Bit().constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
            throw std::runtime_error("Cannot open serial port; check dialout/uucp permissions");
        termios t{};
        if (tcgetattr(fd, &t) != 0)
            throw std::runtime_error("Cannot configure serial port");
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB);
        t.c_cflag = (t.c_cflag & ~CSIZE) | CS8;
        if (tcsetattr(fd, TCSANOW, &t) != 0)
            throw std::runtime_error("Cannot apply serial settings");
        int flags = TIOCM_DTR | TIOCM_RTS;
        ioctl(fd, TIOCMBIS, &flags);
        tcflush(fd, TCIOFLUSH);
#endif
    }
    void close() {
#ifdef Q_OS_WIN
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
    }
    void write(const QByteArray &bytes) {
        qsizetype offset = 0;
        QElapsedTimer timer;
        timer.start();
        while (offset < bytes.size()) {
#ifdef Q_OS_WIN
            DWORD n = 0;
            if (!WriteFile(handle, bytes.constData() + offset,
                           DWORD(qMin(qsizetype(16384), bytes.size() - offset)), &n, nullptr))
                throw std::runtime_error("Serial write failed");
            offset += n;
#else
            auto n = ::write(fd, bytes.constData() + offset, size_t(bytes.size() - offset));
            if (n > 0)
                offset += n;
            else if (errno != EAGAIN && errno != EINTR)
                throw std::runtime_error("Serial write failed");
            else {
                pollfd p{fd, POLLOUT, 0};
                poll(&p, 1, 50);
            }
#endif
            if (timer.elapsed() > 3000)
                throw std::runtime_error("Serial write timed out");
        }
    }
    QByteArray read() {
        char bytes[128];
#ifdef Q_OS_WIN
        DWORD n = 0;
        ReadFile(handle, bytes, sizeof(bytes), &n, nullptr);
        return QByteArray(bytes, int(n));
#else
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 100) > 0) {
            int n = int(::read(fd, bytes, sizeof(bytes)));
            if (n > 0)
                return QByteArray(bytes, n);
        }
        return {};
#endif
    }

private:
#ifdef Q_OS_WIN
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
};
QList<Port> Device::ports() {
    QList<Port> out;
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t name[512], value[512];
            DWORD nameSize = 512, valueSize = sizeof(value), type = 0;
            auto r = RegEnumValueW(key, i, name, &nameSize, nullptr, &type, reinterpret_cast<BYTE *>(value),
                                   &valueSize);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r == ERROR_SUCCESS && type == REG_SZ) {
                QString port = QString::fromWCharArray(value);
                out.append({port, QString::fromWCharArray(name), false});
            }
        }
        RegCloseKey(key);
    }
#else
    QDir dev("/dev");
    for (auto name : dev.entryList({"ttyACM*", "ttyUSB*"}, QDir::System | QDir::Files))
        out.append({"/dev/" + name, "USB Serial", true});
#endif
    std::sort(out.begin(), out.end(), [](const Port &a, const Port &b) { return a.name < b.name; });
    return out;
}
Device::Device() {
    start();
}
Device::~Device() {
    {
        QMutexLocker lock(&mutex);
        stopping = true;
        wake.wakeOne();
    }
    wait();
}
void Device::enqueue(Command c) {
    QMutexLocker lock(&mutex);
    if (queue.size() < 32)
        queue.push_back(c);
    wake.wakeOne();
}
void Device::connectPort(const QString &p, int direction, int light, bool mirrored) {
    {
        QMutexLocker lock(&mutex);
        state.connecting = true;
        state.connected = false;
        state.port.clear();
    }
    Command c;
    c.kind = 1;
    c.port = p;
    c.direction = direction;
    c.light = light;
    c.mirrored = mirrored;
    enqueue(c);
    emit statusChanged();
}
void Device::disconnectPort() {
    Command c;
    c.kind = 2;
    enqueue(c);
}
void Device::command(int k, int value) {
    Command c;
    c.kind = k;
    c.value = value;
    enqueue(c);
}
void Device::submit(const QImage &img) {
    QMutexLocker lock(&mutex);
    if (state.connected) {
        pending = img;
        wake.wakeOne();
    }
}
DeviceStatus Device::status() {
    QMutexLocker lock(&mutex);
    return state;
}
void Device::run() {
    NativeSerial serial;
    FrameDiff diff;
    QImage target;
    bool refreshPending = false;
    QElapsedTimer clock;
    clock.start();
    qint64 nextSend = 0;
    bool connected = false, v2 = false;
    int direction = 2;
    for (;;) {
        std::deque<Command> commands;
        QImage frame;
        {
            QMutexLocker lock(&mutex);
            while (!stopping && queue.empty() && pending.isNull()) {
                if (!refreshPending) {
                    wake.wait(&mutex);
                } else {
                    const auto delay = nextSend - clock.elapsed();
                    if (delay <= 0) break;
                    wake.wait(&mutex, static_cast<unsigned long>(delay));
                }
            }
            if (stopping)
                break;
            commands.swap(queue);
            frame = std::move(pending);
            pending = QImage();
        }
        try {
            for (const auto &c : commands) {
                if (c.kind == 1) {
                    serial.open(c.port);
                    serial.write(Protocol::header(0, 0, 0, 0, 0xff));
                    v2 = serial.read().contains("V2");
                    direction = c.direction;
                    serial.write(Protocol::orientation(direction));
                    msleep(10);
                    serial.write(Protocol::mirror(c.mirrored));
                    msleep(10);
                    serial.write(Protocol::brightness(c.light));
                    connected = true;
                    diff.invalidate();
                    target = {};
                    refreshPending = false;
                    QMutexLocker lock(&mutex);
                    state.connected = true;
                    state.connecting = false;
                    state.message = c.port;
                    state.port = c.port;
                    lock.unlock();
                    emit statusChanged();
                } else if (c.kind == 2) {
                    serial.close();
                    connected = false;
                    diff.invalidate();
                    target = {};
                    refreshPending = false;
                    QMutexLocker lock(&mutex);
                    state.connected = false;
                    state.connecting = false;
                    state.message = "Disconnected";
                    state.port.clear();
                    lock.unlock();
                    emit statusChanged();
                } else if (connected) {
                    if (c.kind == 3)
                        serial.write(Protocol::brightness(c.value));
                    else if (c.kind == 4) {
                        direction = c.value;
                        serial.write(Protocol::orientation(direction));
                        msleep(10);
                        diff.invalidate();
                        refreshPending = !target.isNull();
                    } else if (c.kind == 5) {
                        serial.write(Protocol::mirror(c.value != 0));
                        msleep(10);
                        diff.invalidate();
                        refreshPending = !target.isNull();
                    } else if (c.kind == 6) {
                        serial.write(Protocol::orientation(0));
                        msleep(10);
                        serial.write(Protocol::header(0, 0, 0, 0, 0x66));
                        serial.write(Protocol::orientation(direction));
                        msleep(10);
                        diff.invalidate();
                        target = {};
                        refreshPending = false;
                        frame = {};
                    } else if (c.kind == 7) {
                        serial.write(Protocol::header(0, 0, 0, 0, 0x65));
                        serial.close();
                        connected = false;
                        diff.invalidate();
                        target = {};
                        refreshPending = false;
                        QMutexLocker lock(&mutex);
                        state.connected = false;
                        state.port.clear();
                        lock.unlock();
                        emit statusChanged();
                    } else if (c.kind == 8) {
                        serial.write(Protocol::header(0, 0, 0, 0, c.value ? 0x6d : 0x6c));
                        diff.invalidate();
                        target = {};
                        refreshPending = false;
                        frame = {};
                    }
                }
            }
            if (connected && !frame.isNull()) {
                const auto expected = direction >= 2 ? QSize(480, 320) : QSize(320, 480);
                if (frame.size() == expected) {
                    target = Protocol::rgb565(frame);
                    refreshPending = true;
                }
            }
            const auto expected = direction >= 2 ? QSize(480, 320) : QSize(320, 480);
            if (connected && refreshPending && target.size() == expected && clock.elapsed() >= nextSend) {
                const auto started = clock.elapsed();
                const auto result = diff.send(target, started, v2, [&](const QRect &, const QByteArray &packet) {
                    {
                        QMutexLocker lock(&mutex);
                        if (stopping || !queue.empty() || !pending.isNull()) return false;
                    }
                    serial.write(packet.left(6));
                    if (!v2) msleep(10);
                    serial.write(packet.mid(6));
                    if (!v2) msleep(10);
                    return true;
                });
                refreshPending = result.pending;
                // Account for USB buffering without sleeping twice when native writes already blocked.
                nextSend = started + (result.cost * 1000 + FrameDiff::Bandwidth - 1) / FrameDiff::Bandwidth;
                if (result.rectangles > 0) {
                    QMutexLocker lock(&mutex);
                    ++state.frames;
                    state.bytes += quint64(result.bytes);
                }
            } else if (target.size() != expected) {
                target = {};
                refreshPending = false;
            }
        } catch (const std::exception &e) {
            serial.close();
            connected = false;
            diff.invalidate();
            target = {};
            refreshPending = false;
            QMutexLocker lock(&mutex);
            state.connected = false;
            state.connecting = false;
            state.message = QString::fromUtf8(e.what());
            state.port.clear();
            lock.unlock();
            emit statusChanged();
        }
    }
}
} // namespace Screen
