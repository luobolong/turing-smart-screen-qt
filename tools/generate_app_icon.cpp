// Rasterize the application SVG into a multi-resolution Windows icon.
#include <QCoreApplication>
#include <QSvgRenderer>
#include <QImage>
#include <QPainter>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3) return 1;
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) return 2;
    QList<QByteArray> images;
    const QList<int> sizes{16, 24, 32, 48, 64, 128, 256};
    for (int size : sizes) {
        QImage image(size, size, QImage::Format_ARGB32); image.fill(Qt::transparent);
        QPainter painter(&image); svg.render(&painter); painter.end();
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) return 3;
        images.append(bytes);
    }
    QFile output(QString::fromLocal8Bit(argv[2]));
    if (!output.open(QIODevice::WriteOnly)) return 4;
    QDataStream stream(&output); stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (int i = 0; i < sizes.size(); ++i) {
        stream << quint8(sizes[i] == 256 ? 0 : sizes[i]) << quint8(sizes[i] == 256 ? 0 : sizes[i]);
        stream << quint8(0) << quint8(0) << quint16(1) << quint16(32) << quint32(images[i].size()) << offset;
        offset += quint32(images[i].size());
    }
    for (const auto &bytes : images) stream.writeRawData(bytes.constData(), bytes.size());
    return stream.status() == QDataStream::Ok ? 0 : 5;
}
