#pragma once
#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QString>
#include <QVector>
#include <memory>
namespace Nrbf {
struct Node;
using Value = std::shared_ptr<Node>;
struct Node {
    enum Kind { Null, Boolean, Integer, Number, Text, Bytes, Array, Object, Reference, Nulls } kind = Null;
    qint64 integer = 0;
    double number = 0;
    QString text;
    QByteArray bytes;
    QVector<Value> array;
    QMap<QString, Value> fields;
};
class Document {
public:
    static Document parse(const QByteArray &data);
    Value root;
    Value resolve(Value value) const;
    Value field(Value value, const QString &key) const;
    QVector<Value> list(Value value) const;
    QByteArray bytes(Value value) const;

private:
    QHash<int, Value> objects;
    friend class Reader;
};
QString text(Value v, const QString &fallback = {});
qint64 integer(Value v, qint64 fallback = 0);
double number(Value v, double fallback = 0);
bool boolean(Value v, bool fallback = true);
} // namespace Nrbf
