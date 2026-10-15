#include "model.h"
#include "nrbf.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <cmath>
#include <stdexcept>
namespace Screen {
static void check(bool ok, const QString &message) {
    if (!ok)
        throw std::runtime_error(message.toUtf8().constData());
}
QString assets() {
    auto env = qEnvironmentVariable("TURING_QT_ASSETS");
    if (!env.isEmpty())
        return env;
    auto dir = QCoreApplication::applicationDirPath() + "/assets";
    return QDir(dir).exists() ? dir : QString::fromUtf8(SCREEN_ASSET_DIR);
}
QString uid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QJsonArray rgba(QColor c) {
    return {c.red(), c.green(), c.blue(), c.alpha()};
}
QColor color(QJsonValue v, QColor fallback) {
    auto a = v.toArray();
    return a.size() >= 3 ? QColor(a[0].toInt(), a[1].toInt(), a[2].toInt(), a.size() > 3 ? a[3].toInt() : 255)
                         : fallback;
}
static QJsonObject base(const QString &kind, const QString &name) {
    return {{"id", uid()}, {"kind", kind}, {"name", name},    {"x", 0},         {"y", 0},
            {"w", 100},    {"h", 30},      {"visible", true}, {"locked", false}};
}
QJsonObject newLayer(const QString &kind, const QString &name, const QString &metric) {
    auto o = base(kind, name);
    o["x"] = 24;
    o["y"] = 24;
    o["metric"] = metric;
    o["fill"] = rgba(QColor("#28c9bf"));
    o["stroke"] = rgba(QColor("#66d9df"));
    o["stroke_width"] = 1;
    if (kind == "text" || kind == "sensor") {
        o["text"] = kind == "text" ? name : QString();
        o["font"] =
            QJsonObject{{"family", "Noto Sans SC"}, {"size", 20}, {"color", rgba(Qt::white)}, {"spacing", 0}};
        o["long_text"] = true;
        o["length"] = 0;
        o["w"] = 120;
        o["h"] = 30;
    }
    if (kind == "group") {
        o["children"] = QJsonArray();
        o["w"] = 0;
        o["h"] = 0;
    }
    if (kind == "bar") {
        o["w"] = 160;
        o["h"] = 10;
        o["step"] = 1;
        o["front"] = rgba(QColor("#28c9bf"));
        o["back"] = rgba(QColor("#253041"));
    }
    if (kind == "arc") {
        o["w"] = 88;
        o["h"] = 88;
        o["radius"] = 42;
        o["width"] = 5;
        o["start_percent"] = -25;
        o["front"] = rgba(QColor("#28c9bf"));
        o["back"] = rgba(QColor("#253041"));
    }
    if (kind == "image")
        o["opacity"] = 255;
    return o;
}
QJsonObject component(const QString &kind) {
    auto group = newLayer("group", kind);
    group["x"] = 16;
    group["y"] = 24;
    auto panel = newLayer("rectangle", "Panel");
    panel["x"] = 0;
    panel["y"] = 0;
    panel["w"] = 208;
    panel["h"] = 128;
    panel["fill"] = rgba(Qt::transparent);
    QJsonArray children{panel};
    const auto add = [&](const QString &name, const QString &metric, int x, int y, int size) {
        auto t = newLayer(metric.isEmpty() ? "text" : "sensor", name, metric);
        t["x"] = x;
        t["y"] = y;
        auto f = t["font"].toObject();
        f["size"] = size;
        t["font"] = f;
        children.append(t);
    };
    add(kind, {}, 12, 8, 18);
    QString metric = kind == "GPU"     ? "GpuLoad"
                     : kind == "RAM"   ? "RamLoad"
                     : kind == "NET"   ? "InternetDown"
                     : kind == "DISK"  ? "HDDLoad"
                     : kind == "CLOCK" ? "Time"
                                       : "CpuLoad";
    add(metric, metric, 12, 40, 26);
    if (kind == "CLOCK")
        add("Date", "Date", 12, 84, 16);
    else if (kind == "NET")
        add("Upload", "InternetUp", 12, 84, 16);
    else {
        auto bar = newLayer("bar", "Progress", metric);
        bar["x"] = 12;
        bar["y"] = 104;
        bar["w"] = 184;
        children.append(bar);
    }
    group["children"] = children;
    return group;
}
Theme Theme::starter() {
    Theme t;
    t.data = {{"format", "turing-screen-qt"},
              {"version", 1},
              {"name", "Qt · System overview"},
              {"landscape", true}};
    QJsonArray a;
    auto cpu = component("CPU");
    cpu["x"] = 16;
    cpu["y"] = 24;
    a.append(cpu);
    auto ram = component("RAM");
    ram["x"] = 252;
    ram["y"] = 24;
    a.append(ram);
    auto net = component("NET");
    net["x"] = 16;
    net["y"] = 168;
    a.append(net);
    auto clock = component("CLOCK");
    clock["x"] = 252;
    clock["y"] = 168;
    a.append(clock);
    t.data["layers"] = a;
    return t;
}
void Theme::validate() const {
    check(data["layers"].isArray(), "Missing layers array");
    int total = 0;
    QSet<QString> ids;
    std::function<void(QJsonArray, int)> visit = [&](QJsonArray a, int depth) {
        check(depth < 32, "Layer hierarchy too deep");
        for (auto v : a) {
            check(v.isObject(), "Invalid layer");
            auto o = v.toObject();
            check(++total <= 2048, "Too many layers");
            auto id = o["id"].toString();
            check(!id.isEmpty() && !ids.contains(id), "Duplicate layer ID");
            ids.insert(id);
            for (auto k : {"x", "y", "w", "h"}) {
                double n = o.value(k).toDouble();
                check(std::isfinite(n) && std::abs(n) <= 8192, "Invalid layer geometry");
            }
            if (o["kind"] == "group")
                visit(o["children"].toArray(), depth + 1);
        }
    };
    visit(layers(), 0);
}
void Theme::save(const QString &path) const {
    validate();
    QSaveFile f(path);
    check(f.open(QIODevice::WriteOnly), f.errorString());
    check(f.write(QJsonDocument(data).toJson()) >= 0, f.errorString());
    check(f.commit(), f.errorString());
}
Theme Theme::load(const QString &path) {
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), f.errorString());
    check(f.size() <= 32 * 1024 * 1024, "Theme file exceeds 32 MiB");
    auto bytes = f.readAll();
    if (path.endsWith(".data", Qt::CaseInsensitive))
        return fromOfficial(bytes);
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    check(error.error == QJsonParseError::NoError && doc.isObject(), error.errorString());
    return fromJson(doc.object());
}
static QJsonValue rawBytes(const QByteArray &b) {
    return QString::fromLatin1(b.toBase64());
}
static QByteArray byteArray(QJsonValue v) {
    if (v.isString())
        return QByteArray::fromBase64(v.toString().toLatin1());
    QByteArray b;
    for (auto x : v.toArray())
        b.append(char(x.toInt()));
    return b;
}
static QJsonObject legacyItem(QJsonObject o) {
    QString name = o["name"].toString("StaticText");
    auto l =
        base(name == "StaticText" || name.startsWith("userDefine") || name == "Model" ? "text" : "sensor",
             o["label"].toString(name));
    l["metric"] = name;
    l["x"] = o["x"];
    l["y"] = o["y"];
    l["visible"] = o.value("enabled").toBool(true);
    l["locked"] = o["locked"].toBool();
    l["text"] = o["text"];
    l["font"] = o["font"];
    l["length"] = o["length"];
    l["long_text"] = o["long_text"];
    auto f = o["font"].toObject();
    l["w"] =
        qMax(12, int(f["size"].toDouble(18) * qMax(1, o["length"].toInt(o["text"].toString().size())) * .7));
    l["h"] = qMax(f["height"].toInt(), int(f["size"].toDouble(18) * 1.3));
    if (o["graphic"].isObject()) {
        auto g = o["graphic"].toObject();
        l["kind"] = g["type"] == "image" ? "image" : g["shape"].toString("rectangle");
        l["w"] = g["width"];
        l["h"] = g["height"];
        for (auto k : {"data", "opacity", "fill", "stroke", "stroke_width"})
            if (g.contains(k))
                l[k] = g[k];
    }
    return l;
}
Theme Theme::fromJson(const QJsonObject &json) {
    if (json["format"] == "turing-screen-qt") {
        Theme t{json};
        t.validate();
        return t;
    }
    check(json.contains("groups"), "Unsupported theme format");
    Theme t;
    t.data = {{"format", "turing-screen-qt"},
              {"version", 1},
              {"name", json["name"]},
              {"landscape", json["landscape"].toBool(true)}};
    for (auto k : {"background", "overlay", "text_background", "use_text_background"})
        t.data[k] = json[k];
    QVector<QJsonObject> groups;
    for (auto v : json["groups"].toArray()) {
        auto o = v.toObject();
        auto g = base("group", o["name"].toString());
        g["x"] = o["x"];
        g["y"] = o["y"];
        g["visible"] = o.value("enabled").toBool(true);
        g["locked"] = o["locked"].toBool();
        g["background"] = o["background"];
        g["root"] = o["root"];
        QJsonArray children;
        for (auto i : o["items"].toArray())
            children.append(legacyItem(i.toObject()));
        g["children"] = children;
        groups.append(g);
    }
    QJsonArray root;
    for (auto category : {"bars", "arcs"})
        for (auto v : json[category].toArray()) {
            auto o = v.toObject();
            auto l =
                base(QString(category) == "bars" ? "bar" : "arc", o["label"].toString(o["name"].toString()));
            l["metric"] = o["name"];
            l["x"] = o["x"];
            l["y"] = o["y"];
            l["visible"] = o.value("enabled").toBool(true);
            l["locked"] = o["locked"].toBool();
            for (auto k : {"radius", "width", "start_percent", "segmented", "front", "back", "step",
                           "offset_x", "offset_y"})
                l[k] = o[k];
            for (auto k : {"front_pixels", "back_pixels", "points", "point_counts"})
                l[k] = QString::fromLatin1(byteArray(o[k]).toBase64());
            if (QString(category) == "bars") {
                l["w"] = o["length"].toInt(160);
                l["h"] = o["height"].toInt(10);
            } else {
                double r = o["radius"].toDouble(42);
                l["w"] = int(2 * r + o["width"].toDouble(5) + 2);
                l["h"] = l["w"];
            }
            int parent = o["parent"].toInt(-1);
            if (parent >= 0 && parent < groups.size()) {
                auto c = groups[parent]["children"].toArray();
                c.prepend(l);
                groups[parent]["children"] = c;
            } else
                root.append(l);
        }
    for (auto g : groups) {
        root.append(g);
    }
    t.data["layers"] = root;
    t.validate();
    return t;
}
Theme Theme::fromOfficial(const QByteArray &bytes) {
    auto d = Nrbf::Document::parse(bytes);
    check(d.root->text == "usb_hid.ThemeConfig", "Not an official ThemeConfig");
    auto f = [&](Nrbf::Value v, const char *k) { return d.field(v, QString::fromLatin1(k)); };
    auto root = d.root;
    QJsonObject old{{"version", 1},
                    {"name", Nrbf::text(f(root, "name"), "Imported")},
                    {"landscape", Nrbf::boolean(f(root, "isLanscape"), false)}};
    auto bitmap = [&](Nrbf::Value v) -> QJsonValue {
        auto b = d.bytes(f(v, "Data"));
        return b.isEmpty() ? QJsonValue() : QJsonValue(QString::fromLatin1(b.toBase64()));
    };
    auto ncolor = [&](Nrbf::Value v) -> QJsonArray {
        auto state = Nrbf::integer(f(v, "state"));
        quint32 argb = quint32(Nrbf::integer(f(v, "value")));
        if (state & 2)
            return rgba(QColor::fromRgba(argb));
        static QJsonObject colors = []() {
            QFile file(assets() + "/known-colors.json");
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("Missing known-colors table");
            auto b = file.readAll();
            if (b.startsWith("\xef\xbb\xbf"))
                b.remove(0, 3);
            return QJsonDocument::fromJson(b).object();
        }();
        return colors.value(QString::number(Nrbf::integer(f(v, "knownColor")))).toArray();
    };
    old["background"] = bitmap(f(root, "backImg"));
    old["overlay"] = bitmap(f(root, "osdImg"));
    old["text_background"] = bitmap(f(root, "osdWithFontBack"));
    old["use_text_background"] = Nrbf::boolean(f(root, "useOsdWithFontBack"), false);
    QJsonArray groups;
    for (auto v : d.list(f(root, "groupList"))) {
        QJsonObject g{{"name", Nrbf::text(f(v, "groupName"))},
                      {"enabled", Nrbf::boolean(f(v, "enabled"))},
                      {"x", int(Nrbf::integer(f(v, "baseX")))},
                      {"y", int(Nrbf::integer(f(v, "baseY")))},
                      {"background", bitmap(f(v, "backImg"))}};
        QJsonArray items;
        for (auto i : d.list(f(v, "itemList"))) {
            auto font = f(i, "fontType");
            QJsonObject fp{{"family", "GeForce"},
                           {"size", Nrbf::number(f(font, "size"), 18)},
                           {"color", ncolor(f(font, "color"))},
                           {"spacing", Nrbf::number(f(font, "interval"))},
                           {"width", int(Nrbf::integer(f(font, "W")))},
                           {"height", int(Nrbf::integer(f(font, "H")))},
                           {"offset_x", int(Nrbf::integer(f(font, "offsetX")))},
                           {"offset_y", int(Nrbf::integer(f(font, "offsetY")))}};
            items.append(QJsonObject{{"name", Nrbf::text(f(i, "itemName"))},
                                     {"label", Nrbf::text(f(i, "cnName"))},
                                     {"enabled", Nrbf::boolean(f(i, "enabled"))},
                                     {"x", int(Nrbf::integer(f(i, "posX")))},
                                     {"y", int(Nrbf::integer(f(i, "posY")))},
                                     {"length", int(Nrbf::integer(f(i, "strLen")))},
                                     {"long_text", Nrbf::boolean(f(i, "isLongStr"), false)},
                                     {"text", Nrbf::text(f(i, "valueStr"))},
                                     {"font", fp}});
        }
        g["items"] = items;
        groups.append(g);
    }
    old["groups"] = groups;
    QJsonArray bars;
    for (auto v : d.list(f(root, "barList"))) {
        QJsonObject b{{"name", Nrbf::text(f(v, "itemName"))},
                      {"enabled", Nrbf::boolean(f(v, "enabled"))},
                      {"x", int(Nrbf::integer(f(v, "posX")))},
                      {"y", int(Nrbf::integer(f(v, "posY")))},
                      {"length", int(Nrbf::integer(f(v, "length"), 100))},
                      {"height", int(Nrbf::integer(f(v, "height"), 13))},
                      {"step", int(Nrbf::integer(f(v, "stepWidth"), 1))},
                      {"front", ncolor(f(v, "frontColor"))},
                      {"back", ncolor(f(v, "backColor"))},
                      {"front_pixels", rawBytes(d.bytes(f(v, "frontData")))}};
        QByteArray back;
        for (auto col : d.list(f(v, "backImgData")))
            back += d.bytes(col);
        if (back.size() != b["length"].toInt() * b["height"].toInt() * 2)
            back = d.bytes(f(v, "backData"));
        b["back_pixels"] = rawBytes(back);
        bars.append(b);
    }
    old["bars"] = bars;
    QJsonArray arcs;
    for (auto v : d.list(f(root, "archList"))) {
        QJsonObject a{{"name", Nrbf::text(f(v, "itemName"))},
                      {"enabled", Nrbf::boolean(f(v, "enabled"))},
                      {"x", int(Nrbf::integer(f(v, "posX")))},
                      {"y", int(Nrbf::integer(f(v, "posY")))},
                      {"radius", Nrbf::number(f(v, "archR"), 84) / 2.},
                      {"width", Nrbf::number(f(v, "archWidth"), 5)},
                      {"start_percent", Nrbf::number(f(v, "startPer"), -25)},
                      {"segmented", Nrbf::boolean(f(v, "useInternal"), false)},
                      {"front", ncolor(f(v, "frontArchColor"))},
                      {"back", ncolor(f(v, "backArchColor"))},
                      {"points", rawBytes(d.bytes(f(v, "archData")))},
                      {"point_counts", rawBytes(d.bytes(f(v, "dataLenArray")))},
                      {"offset_x", int(Nrbf::integer(f(v, "offsetX")))},
                      {"offset_y", int(Nrbf::integer(f(v, "offsetY")))}};
        arcs.append(a);
    }
    old["arcs"] = arcs;
    return fromJson(old);
}
QJsonObject findLayer(const QJsonArray &a, const QString &id) {
    for (auto v : a) {
        auto o = v.toObject();
        if (o["id"] == id)
            return o;
        auto child = findLayer(o["children"].toArray(), id);
        if (!child.isEmpty())
            return child;
    }
    return {};
}
bool updateLayer(QJsonArray &a, const QString &id, const std::function<void(QJsonObject &)> &fn) {
    for (int i = 0; i < a.size(); ++i) {
        auto o = a[i].toObject();
        if (o["id"] == id) {
            fn(o);
            a[i] = o;
            return true;
        }
        auto children = o["children"].toArray();
        if (updateLayer(children, id, fn)) {
            o["children"] = children;
            a[i] = o;
            return true;
        }
    }
    return false;
}
QJsonArray takeLayers(QJsonArray &a, const QStringList &ids) {
    QJsonArray out;
    for (int i = 0; i < a.size();) {
        auto o = a[i].toObject();
        if (ids.contains(o["id"].toString())) {
            out.append(o);
            a.removeAt(i);
        } else {
            auto children = o["children"].toArray();
            auto found = takeLayers(children, ids);
            for (auto v : found)
                out.append(v);
            o["children"] = children;
            a[i] = o;
            ++i;
        }
    }
    return out;
}
bool insertLayers(QJsonArray &a, const QString &parent, const QJsonArray &items, int index) {
    if (parent.isEmpty()) {
        int i = index < 0 ? int(a.size()) : qBound(0, index, int(a.size()));
        for (auto v : items)
            a.insert(i++, v);
        return true;
    }
    return updateLayer(a, parent, [&](QJsonObject &o) {
        auto c = o["children"].toArray();
        insertLayers(c, {}, items, index);
        o["children"] = c;
    });
}
QString parentOf(const QJsonArray &a, const QString &id) {
    for (auto v : a) {
        auto o = v.toObject();
        auto c = o["children"].toArray();
        for (auto child : c)
            if (child.toObject()["id"] == id)
                return o["id"].toString();
        auto p = parentOf(c, id);
        if (!p.isEmpty())
            return p;
    }
    return {};
}
QStringList normalizedSelection(const QJsonArray &a, const QStringList &ids) {
    QStringList out;
    for (auto id : ids) {
        bool skip = false;
        auto p = parentOf(a, id);
        while (!p.isEmpty()) {
            if (ids.contains(p)) {
                skip = true;
                break;
            }
            p = parentOf(a, p);
        }
        if (!skip && !findLayer(a, id).isEmpty())
            out.append(id);
    }
    return out;
}
QPointF absolutePosition(const QJsonArray &a, const QString &id, QPointF offset) {
    for (auto v : a) {
        auto o = v.toObject();
        QPointF p = offset + QPointF(o["x"].toDouble(), o["y"].toDouble());
        if (o["id"] == id)
            return p;
        if (!findLayer(o["children"].toArray(), id).isEmpty())
            return absolutePosition(o["children"].toArray(), id, p);
    }
    return {};
}
QRectF layerBounds(const QJsonObject &o) {
    if (o["kind"] == "group") {
        QRectF r;
        for (auto v : o["children"].toArray()) {
            auto c = v.toObject();
            r = r.united(layerBounds(c).translated(c["x"].toDouble(), c["y"].toDouble()));
        }
        return r.isEmpty() ? QRectF(0, 0, 40, 30) : r;
    }
    return {0, 0, qMax(1., o["w"].toDouble(100)), qMax(1., o["h"].toDouble(30))};
}
bool layerLocked(const QJsonArray &a, const QString &id, bool inherited) {
    for (auto v : a) {
        auto o = v.toObject();
        bool locked = inherited || o["locked"].toBool();
        if (o["id"] == id)
            return locked;
        if (!findLayer(o["children"].toArray(), id).isEmpty())
            return layerLocked(o["children"].toArray(), id, locked);
    }
    return true;
}
void regenerateIds(QJsonObject &o) {
    o["id"] = uid();
    auto a = o["children"].toArray();
    for (int i = 0; i < a.size(); ++i) {
        auto c = a[i].toObject();
        regenerateIds(c);
        a[i] = c;
    }
    if (o["kind"] == "group")
        o["children"] = a;
}
QString canonical(QString k) {
    static const QHash<QString, QString> aliases{
        {"CPUCLOCK", "CpuFreq"},        {"GPUCLOCK", "GpuFreq"},
        {"CPUPWR", "CpuPower"},         {"GPUPWR", "GpuPower"},
        {"DRVLOAD", "HDDLoad"},         {"UPSPEED", "InternetUp"},
        {"DOWNDSPEED", "InternetDown"}, {"DOWNSPEED", "InternetDown"},
        {"VULOMELOAD", "Volume"},       {"RAM", "RamValue"}};
    auto u = k.toUpper();
    if (aliases.contains(u))
        return aliases.value(u);
    for (auto m : metrics())
        if (m.first.toUpper() == u)
            return m.first;
    return k;
}
QVector<QPair<QString, QString>> metrics() {
    return {{"CpuLoad", "CPU 使用率"},    {"CpuFreq", "CPU 频率"},   {"CpuTemp", "CPU 温度"},
            {"CpuModel", "CPU 型号"},     {"GpuLoad", "GPU 使用率"}, {"GpuTemp", "GPU 温度"},
            {"GpuFreq", "GPU 频率"},      {"GpuRam", "显存占用"},    {"RamValue", "内存占用"},
            {"RamLoad", "内存使用率"},    {"RamValid", "可用内存"},  {"RamModel", "内存总量"},
            {"HDDLoad", "分区使用率"},    {"HDDTemp", "硬盘温度"},   {"InternetUp", "上传速率"},
            {"InternetDown", "下载速率"}, {"Time", "时间"},          {"Date", "日期"},
            {"Volume", "音量"},           {"Weather", "天气"}};
}
} // namespace Screen
