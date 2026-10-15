#include "nrbf.h"
#include <QtEndian>
#include <cstring>
#include <stdexcept>
namespace Nrbf {
static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
static Value make(Node::Kind kind) {
    auto n = std::make_shared<Node>();
    n->kind = kind;
    return n;
}
struct Metadata {
    QString name;
    QStringList names;
    QVector<QPair<int, int>> types;
};
class Reader {
public:
    explicit Reader(const QByteArray &data) : data(data) {}
    Document read() {
        require(data.size() <= 32 * 1024 * 1024, "Theme file exceeds 32 MiB");
        require(byte() == 0, "Invalid NRBF header");
        const int rootId = i32();
        i32();
        require(i32() == 1 && i32() == 0, "Unsupported NRBF version");
        while (true) {
            require(pos < data.size(), "Truncated NRBF stream");
            if (quint8(data[pos]) == 11) {
                ++pos;
                break;
            }
            value();
        }
        require(doc.objects.contains(rootId), "Missing NRBF root");
        doc.root = doc.objects.value(rootId);
        return doc;
    }

private:
    const QByteArray &data;
    qsizetype pos = 0;
    int depth = 0;
    Document doc;
    QHash<int, Metadata> metadata;
    QByteArray take(qsizetype n) {
        require(n >= 0 && n <= data.size() - pos, "Truncated NRBF payload");
        auto out = data.mid(pos, n);
        pos += n;
        return out;
    }
    quint8 byte() {
        require(pos < data.size(), "Unexpected end of NRBF");
        return quint8(data[pos++]);
    }
    qint32 i32() {
        auto b = take(4);
        return qFromLittleEndian<qint32>(b.constData());
    }
    int count() {
        int n = i32();
        require(n >= 0 && n <= 16000000, "Invalid NRBF count");
        return n;
    }
    QString string() {
        quint32 n = 0;
        int shift = 0;
        while (true) {
            quint8 b = byte();
            require(shift < 35, "Invalid NRBF string length");
            n |= quint32(b & 127) << shift;
            if (!(b & 128))
                break;
            shift += 7;
        }
        require(n <= 32 * 1024 * 1024, "NRBF string too large");
        return QString::fromUtf8(take(n));
    }
    Value primitive(int type) {
        Value v;
        switch (type) {
        case 1:
            v = make(Node::Boolean);
            v->integer = byte() != 0;
            return v;
        case 2:
            v = make(Node::Integer);
            v->integer = byte();
            return v;
        case 3: {
            QByteArray b;
            const auto c = byte();
            b.append(char(c));
            int n = c < 128 ? 1 : (c < 224 ? 2 : (c < 240 ? 3 : 4));
            b += take(n - 1);
            v = make(Node::Text);
            v->text = QString::fromUtf8(b);
            return v;
        }
        case 5:
        case 18:
            v = make(Node::Text);
            v->text = string();
            return v;
        case 6: {
            auto b = take(8);
            quint64 bits = qFromLittleEndian<quint64>(b.constData());
            double d;
            std::memcpy(&d, &bits, 8);
            v = make(Node::Number);
            v->number = d;
            return v;
        }
        case 7: {
            v = make(Node::Integer);
            auto b = take(2);
            v->integer = qFromLittleEndian<qint16>(b.constData());
            return v;
        }
        case 8:
            v = make(Node::Integer);
            v->integer = i32();
            return v;
        case 9:
        case 12:
        case 13:
        case 16: {
            auto b = take(8);
            v = make(Node::Integer);
            v->integer = qFromLittleEndian<qint64>(b.constData());
            return v;
        }
        case 10:
            v = make(Node::Integer);
            v->integer = qint8(byte());
            return v;
        case 11: {
            auto b = take(4);
            quint32 bits = qFromLittleEndian<quint32>(b.constData());
            float f;
            std::memcpy(&f, &bits, 4);
            v = make(Node::Number);
            v->number = f;
            return v;
        }
        case 14: {
            auto b = take(2);
            v = make(Node::Integer);
            v->integer = qFromLittleEndian<quint16>(b.constData());
            return v;
        }
        case 15: {
            auto b = take(4);
            v = make(Node::Integer);
            v->integer = qFromLittleEndian<quint32>(b.constData());
            return v;
        }
        case 17:
            return make(Node::Null);
        default:
            throw std::runtime_error("Unsupported NRBF primitive");
        }
    }
    QPair<int, int> typeInfo(int type) {
        int p = 0;
        switch (type) {
        case 0:
        case 7:
            p = byte();
            break;
        case 3:
            string();
            break;
        case 4:
            string();
            i32();
            break;
        case 1:
        case 2:
        case 5:
        case 6:
            break;
        default:
            throw std::runtime_error("Invalid NRBF member type");
        }
        return {type, p};
    }
    Value members(int id, const Metadata &m) {
        auto v = make(Node::Object);
        v->text = m.name;
        doc.objects.insert(id, v);
        for (int i = 0; i < m.names.size(); ++i) {
            auto [t, p] = m.types[i];
            v->fields.insert(m.names[i], t == 0 ? primitive(p) : value());
        }
        return v;
    }
    Value array(int id, int n, int type, int p) {
        require(n >= 0 && n <= 16000000, "Invalid NRBF array length");
        auto v = make(type == 0 && p == 2 ? Node::Bytes : Node::Array);
        doc.objects.insert(id, v);
        if (v->kind == Node::Bytes) {
            v->bytes = take(n);
            return v;
        }
        while (v->array.size() < n) {
            auto item = type == 0 ? primitive(p) : value();
            if (item->kind == Node::Nulls) {
                require(item->integer >= 0 && item->integer <= n - v->array.size(), "Invalid NRBF null run");
                for (int i = 0; i < item->integer; ++i)
                    v->array.append(make(Node::Null));
            } else
                v->array.append(item);
        }
        return v;
    }
    Value value() {
        require(depth++ < 128, "NRBF nesting too deep");
        struct Guard {
            int &n;
            ~Guard() {
                --n;
            }
        } guard{depth};
        while (true) {
            const int record = byte();
            switch (record) {
            case 12:
                i32();
                string();
                continue;
            case 1: {
                int id = i32();
                int mid = i32();
                require(metadata.contains(mid), "Invalid NRBF metadata reference");
                return members(id, metadata.value(mid));
            }
            case 4:
            case 5: {
                int id = i32();
                Metadata m;
                m.name = string();
                int n = count();
                require(n < 4096, "Too many NRBF members");
                for (int i = 0; i < n; ++i)
                    m.names.append(string());
                QByteArray kinds = take(n);
                for (char t : kinds)
                    m.types.append(typeInfo(quint8(t)));
                if (record == 5) {
                    i32();
                }
                metadata.insert(id, m);
                return members(id, m);
            }
            case 6: {
                int id = i32();
                auto v = make(Node::Text);
                v->text = string();
                doc.objects.insert(id, v);
                return v;
            }
            case 7: {
                int id = i32();
                int shape = byte();
                int rank = count();
                require(rank >= 1 && rank <= 8, "Invalid NRBF rank");
                qint64 n = 1;
                for (int i = 0; i < rank; ++i) {
                    n *= count();
                    require(n <= 16000000, "NRBF array too large");
                }
                if (shape >= 3) {
                    for (int i = 0; i < rank; ++i)
                        i32();
                }
                auto [t, p] = typeInfo(byte());
                return array(id, int(n), t, p);
            }
            case 8:
                return primitive(byte());
            case 9: {
                auto v = make(Node::Reference);
                v->integer = i32();
                return v;
            }
            case 10:
                return make(Node::Null);
            case 13:
            case 14: {
                auto v = make(Node::Nulls);
                v->integer = record == 13 ? byte() : count();
                return v;
            }
            case 15: {
                int id = i32();
                int n = count();
                int p = byte();
                return array(id, n, 0, p);
            }
            case 16:
            case 17: {
                int id = i32();
                int n = count();
                return array(id, n, 2, 0);
            }
            default:
                throw std::runtime_error("Unsupported NRBF record");
            }
        }
    }
};
Document Document::parse(const QByteArray &data) {
    return Reader(data).read();
}
Value Document::resolve(Value v) const {
    for (int i = 0; i < 64 && v && v->kind == Node::Reference; ++i)
        v = objects.value(int(v->integer));
    return v;
}
Value Document::field(Value v, const QString &key) const {
    v = resolve(v);
    if (!v || v->kind != Node::Object)
        return {};
    if (v->fields.contains(key))
        return resolve(v->fields.value(key));
    const auto backing = QStringLiteral("<%1>k__BackingField").arg(key);
    for (auto i = v->fields.cbegin(); i != v->fields.cend(); ++i)
        if (i.key() == backing || i.key().endsWith(QLatin1Char('+') + backing))
            return resolve(i.value());
    return {};
}
QVector<Value> Document::list(Value v) const {
    v = resolve(v);
    if (!v)
        return {};
    if (v->kind == Node::Object) {
        int n = int(integer(field(v, QStringLiteral("_size"))));
        auto a = list(field(v, QStringLiteral("_items")));
        a.resize(qMin(qMax(n, 0), int(a.size())));
        return a;
    }
    if (v->kind != Node::Array) {
        return {};
    }
    QVector<Value> out;
    for (auto i : v->array)
        out.append(resolve(i));
    return out;
}
QByteArray Document::bytes(Value v) const {
    v = resolve(v);
    return v && v->kind == Node::Bytes ? v->bytes : QByteArray();
}
QString text(Value v, const QString &f) {
    return v && v->kind == Node::Text ? v->text : f;
}
qint64 integer(Value v, qint64 f) {
    return !v ? f
              : (v->kind == Node::Integer || v->kind == Node::Boolean
                     ? v->integer
                     : (v->kind == Node::Number ? qint64(v->number) : f));
}
double number(Value v, double f) {
    return !v ? f
              : (v->kind == Node::Number ? v->number : (v->kind == Node::Integer ? double(v->integer) : f));
}
bool boolean(Value v, bool f) {
    return v && v->kind == Node::Boolean ? v->integer != 0 : f;
}
} // namespace Nrbf
