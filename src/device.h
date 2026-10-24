#pragma once
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <deque>
namespace Screen {
namespace Protocol {
QByteArray header(int x, int y, int endX, int endY, quint8 command);
QByteArray orientation(int value);
QByteArray mirror(bool value);
QByteArray brightness(int value);
QByteArray bitmap(const QImage &image, QPoint at = {});
QRect changed(const QImage &previous, const QImage &next);
} // namespace Protocol
struct Port {
    QString name, description;
    bool supported = false;
};
struct DeviceStatus {
    bool connected = false, connecting = false;
    QString message;
    quint64 frames = 0, bytes = 0;
};
class Device : public QThread {
    Q_OBJECT
public:
    Device();
    ~Device() override;
    static QList<Port> ports();
    void connectPort(const QString &port, int orientation, int brightness, bool mirror);
    void disconnectPort();
    void command(int kind, int value = 0);
    void submit(const QImage &image);
    DeviceStatus status();
signals:
    void statusChanged();

protected:
    void run() override;

private:
    struct Command {
        int kind = 0, value = 0;
        QString port;
        int direction = 2, light = 75;
        bool mirrored = false;
    };
    QMutex mutex;
    QWaitCondition wake;
    std::deque<Command> queue;
    QImage pending;
    DeviceStatus state;
    bool stopping = false;
    void enqueue(Command c);
};
} // namespace Screen
