#pragma once
#include <QtCore>
#include <atomic>
#include <thread>
#include <stdexcept>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
#ifndef Q_OS_WIN
class PseudoScreen {
public:
    int master = -1;
    QString port;
    QMutex mutex;
    QByteArray received;
    std::atomic<bool> stopping{false};
    std::thread worker;
    PseudoScreen() {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (master < 0 || grantpt(master) || unlockpt(master))
            throw std::runtime_error("Cannot create serial test PTY");
        port = QString::fromLocal8Bit(ptsname(master));
        worker = std::thread([this] {
            bool replied = false;
            while (!stopping) {
                pollfd descriptor{master, POLLIN, 0};
                if (poll(&descriptor, 1, 10) <= 0)
                    continue;
                char buffer[16384];
                auto count = ::read(master, buffer, sizeof(buffer));
                if (count <= 0)
                    continue;
                QMutexLocker lock(&mutex);
                received.append(buffer, int(count));
                if (!replied && received.size() >= 6) {
                    ::write(master, "V2", 2);
                    replied = true;
                }
            }
        });
    }
    ~PseudoScreen() {
        stopping = true;
        worker.join();
        ::close(master);
    }
    QByteArray bytes() {
        QMutexLocker lock(&mutex);
        return received;
    }
};
#endif
