#include "uiappearance.h"
#include "model.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QEvent>
#include <QJsonDocument>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleFactory>
#include <QProxyStyle>
#include <QStyleOption>
#include <QTimer>
#include <QTreeWidget>
#include <QSvgRenderer>
#include <QMenu>
#include <algorithm>
namespace Screen {
namespace {
QStyle *systemMenuStyle() { return QStyleFactory::create("Fusion"); }
class FlatStyle : public QProxyStyle {
public:
    FlatStyle() : QProxyStyle(QStyleFactory::create("Fusion")) { setObjectName("turing-flat"); }
    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *data) const override {
        if (hint == SH_ComboBox_Popup) return false;
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override {
        if (metric == PM_IndicatorWidth || metric == PM_IndicatorHeight ||
            metric == PM_ExclusiveIndicatorWidth || metric == PM_ExclusiveIndicatorHeight) return 16;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const override {
        // Keep Qt's check-state hit targets and accessibility; only the visibility column uses eyes.
        if (element == PE_IndicatorItemViewItemCheck && widget) {
            auto tree = qobject_cast<const QTreeWidget *>(widget);
            if (!tree) tree = qobject_cast<const QTreeWidget *>(widget->parentWidget());
            if (tree && tree->objectName() == "layerTree" && tree->columnAt(option->rect.center().x()) == 0) {
                const bool visible = option->state & State_On;
                const auto group = option->state & State_Enabled ? QPalette::Active : QPalette::Disabled;
                const auto role = option->state & State_Selected ? QPalette::HighlightedText
                    : visible ? QPalette::Text : QPalette::PlaceholderText;
                const auto size = qMin(option->rect.width(), option->rect.height());
                painter->save(); painter->setRenderHint(QPainter::Antialiasing);
                painter->translate(option->rect.center().x() - size / 2., option->rect.center().y() - size / 2.);
                painter->scale(size / 20., size / 20.);
                painter->setPen(QPen(option->palette.color(group, role), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter->setBrush(Qt::NoBrush);
                QPainterPath eye; eye.moveTo(1, 10); eye.cubicTo(5, 3, 15, 3, 19, 10);
                eye.cubicTo(15, 17, 5, 17, 1, 10); painter->drawPath(eye);
                painter->drawEllipse(QPointF(10, 10), 2.4, 2.4);
                if (!visible) painter->drawLine(QPointF(3, 3), QPointF(17, 17));
                painter->restore();
                return;
            }
        }
        if (element != PE_IndicatorCheckBox && element != PE_IndicatorRadioButton) {
            QProxyStyle::drawPrimitive(element, option, painter, widget); return;
        }
        const bool checked = option->state & (State_On | State_NoChange);
        const auto group = option->state & State_Enabled ? QPalette::Active : QPalette::Disabled;
        const auto accent = option->palette.color(group, QPalette::Highlight);
        const auto border = option->palette.color(group, option->state & State_MouseOver ? QPalette::Highlight : QPalette::Mid);
        const QRectF rect = QRectF(option->rect).adjusted(1, 1, -1, -1);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(checked ? accent : border, 1.2));
        painter->setBrush(checked ? accent : option->palette.color(group, QPalette::Base));
        if (element == PE_IndicatorRadioButton) {
            painter->setBrush(option->palette.color(group, QPalette::Base));
            painter->drawEllipse(rect);
            if (checked) { painter->setBrush(accent); painter->setPen(Qt::NoPen); painter->drawEllipse(rect.adjusted(3.5, 3.5, -3.5, -3.5)); }
        } else {
            painter->drawRoundedRect(rect, 3, 3);
            if (checked) {
                painter->setPen(QPen(option->palette.color(group, QPalette::HighlightedText), 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                if (option->state & State_NoChange) painter->drawLine(rect.left() + 3, rect.center().y(), rect.right() - 3, rect.center().y());
                else {
                    QPainterPath tick; tick.moveTo(rect.left() + 3, rect.center().y());
                    tick.lineTo(rect.left() + 5.5, rect.bottom() - 3.5); tick.lineTo(rect.right() - 2.5, rect.top() + 3.5);
                    painter->drawPath(tick);
                }
            }
        }
        painter->restore();
    }
};
class LineIcon : public QIconEngine {
    UiIcon icon;
public:
    explicit LineIcon(UiIcon icon) : icon(icon) {}
    QIconEngine *clone() const override { return new LineIcon(icon); }
    bool isNull() override { return false; }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size); result.fill(Qt::transparent);
        QPainter painter(&result); paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        const auto palette = qApp->palette();
        const auto color = mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::Text) :
            palette.color(mode == QIcon::Selected ? QPalette::HighlightedText : QPalette::WindowText);
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        const auto size = qMin(rect.width(), rect.height());
        painter->translate(rect.center().x() - size / 2., rect.center().y() - size / 2.);
        painter->scale(size / 24., size / 24.);
        painter->setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); painter->setBrush(Qt::NoBrush);
        if (icon == UiIcon::Refresh) {
            painter->drawArc(QRectF(4, 4, 16, 16), 30 * 16, 295 * 16);
            QPainterPath arrow; arrow.moveTo(20, 3); arrow.lineTo(20, 9); arrow.lineTo(14, 9);
            painter->drawPath(arrow);
        } else if (icon == UiIcon::Preview) {
            painter->drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2);
            painter->drawLine(QPointF(12, 17), QPointF(12, 21)); painter->drawLine(QPointF(8, 21), QPointF(16, 21));
        } else if (icon == UiIcon::Edit) {
            QPainterPath path; path.moveTo(4, 16); path.lineTo(4, 20); path.lineTo(8, 20); path.lineTo(20, 8);
            path.lineTo(16, 4); path.closeSubpath(); painter->drawPath(path); painter->drawLine(QPointF(13.5, 6.5), QPointF(17.5, 10.5));
        } else if (icon == UiIcon::Sensors) {
            QPainterPath path; path.moveTo(2, 12); path.lineTo(6, 12); path.lineTo(9, 5); path.lineTo(14, 20);
            path.lineTo(17, 12); path.lineTo(22, 12); painter->drawPath(path);
        } else if (icon == UiIcon::Settings) {
            for (int x : {5, 12, 19}) {
                const int y = x == 12 ? 15 : 8;
                painter->drawLine(QPointF(x, 3), QPointF(x, y - 2)); painter->drawLine(QPointF(x, y + 2), QPointF(x, 21));
                painter->drawEllipse(QPointF(x, y), 2, 2);
            }
        } else if (icon == UiIcon::Lightweight) {
            QPainterPath triangle; triangle.moveTo(4, 7); triangle.lineTo(20, 7);
            triangle.lineTo(12, 18); triangle.closeSubpath(); painter->drawPath(triangle);
        } else if (icon == UiIcon::Undo || icon == UiIcon::Redo) {
            const bool back = icon == UiIcon::Undo;
            painter->drawLine(QPointF(4, 12), QPointF(20, 12));
            QPainterPath arrow; arrow.moveTo(back ? 10 : 14, 6); arrow.lineTo(back ? 4 : 20, 12);
            arrow.lineTo(back ? 10 : 14, 18); painter->drawPath(arrow);
        } else if (icon == UiIcon::SidebarExpand || icon == UiIcon::SidebarCollapse) {
            const bool collapse = icon == UiIcon::SidebarCollapse;
            painter->drawLine(QPointF(4, 4), QPointF(4, 20));
            painter->drawLine(QPointF(8, 12), QPointF(21, 12));
            QPainterPath arrow;
            arrow.moveTo(collapse ? 14 : 15, 6);
            arrow.lineTo(collapse ? 8 : 21, 12);
            arrow.lineTo(collapse ? 14 : 15, 18);
            painter->drawPath(arrow);
        } else if (icon == UiIcon::Search) {
            painter->drawEllipse(QRectF(4, 3, 12, 12)); painter->drawLine(QPointF(14, 14), QPointF(21, 21));
        } else if (icon == UiIcon::Add) {
            painter->drawLine(QPointF(12, 4), QPointF(12, 20)); painter->drawLine(QPointF(4, 12), QPointF(20, 12));
        } else if (icon == UiIcon::Locate) {
            painter->drawEllipse(QRectF(5, 5, 14, 14));
            painter->drawLine(QPointF(12, 2), QPointF(12, 8)); painter->drawLine(QPointF(12, 16), QPointF(12, 22));
            painter->drawLine(QPointF(2, 12), QPointF(8, 12)); painter->drawLine(QPointF(16, 12), QPointF(22, 12));
        } else if (icon == UiIcon::Expand || icon == UiIcon::Collapse) {
            const bool expand = icon == UiIcon::Expand;
            QPainterPath arrows;
            arrows.moveTo(7, expand ? 8 : 4); arrows.lineTo(12, expand ? 3 : 9); arrows.lineTo(17, expand ? 8 : 4);
            arrows.moveTo(7, expand ? 16 : 20); arrows.lineTo(12, expand ? 21 : 15); arrows.lineTo(17, expand ? 16 : 20);
            painter->drawPath(arrows);
        } else if (icon == UiIcon::Import) {
            QPainterPath tray; tray.moveTo(3, 14); tray.lineTo(3, 20); tray.lineTo(21, 20); tray.lineTo(21, 14);
            painter->drawPath(tray);
            painter->drawLine(QPointF(12, 3), QPointF(12, 15));
            QPainterPath arrow; arrow.moveTo(7, 10); arrow.lineTo(12, 15); arrow.lineTo(17, 10);
            painter->drawPath(arrow);
        } else if (icon == UiIcon::Hide) {
            painter->drawLine(QPointF(5, 12), QPointF(19, 12));
        }
        painter->restore();
    }
};
const QStringList roles{"window", "base", "button", "alternate_base", "text", "muted", "border",
                        "accent", "accent_text", "link", "tooltip", "tooltip_text", "disabled_text"};
const QStringList statusRoles{"success", "warning", "error"};
bool validId(const QString &id) {
    return QRegularExpression("^[a-z0-9][a-z0-9-]{0,63}$").match(id).hasMatch();
}
UiTheme readTheme(const QString &file, const QString &id) {
    UiTheme theme;
    theme.id = id; theme.file = file; theme.name = QFileInfo(file).completeBaseName();
    const auto fail = [&](const QString &message) {
        theme.error = QFileInfo(file).fileName() + ": " + message;
        return theme;
    };
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) return fail(input.errorString());
    const auto bytes = input.read(65537);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (bytes.size() > 65536 || error.error != QJsonParseError::NoError || !doc.isObject())
        return fail("invalid JSON or file exceeds 64 KiB");
    const auto json = doc.object();
    if (json["format"] != "turing-ui-palette" || json["version"] != 1)
        return fail("expected turing-ui-palette version 1");
    if (!json["extends"].isUndefined()) {
        const auto base = json["extends"].toString();
        // Only custom files may inherit a bundled file, keeping lookup finite
        // and preventing a configuration from traversing arbitrary paths.
        if (!id.startsWith("custom:") || !validId(base)) return fail("extends must name a bundled palette");
        const auto parent = readTheme(assets() + "/ui-themes/" + base + ".json", base);
        if (!parent.error.isEmpty()) return fail("cannot load parent: " + parent.error);
        theme.colors = parent.colors; theme.mode = parent.mode;
    }
    if (json.contains("name")) {
        if (!json["name"].isString() || json["name"].toString().trimmed().isEmpty() || json["name"].toString().size() > 80)
            return fail("name must contain 1 to 80 characters");
        theme.name = json["name"].toString().trimmed();
    }
    if (json.contains("mode")) theme.mode = json["mode"].toString();
    if (theme.mode != "light" && theme.mode != "dark") return fail("mode must be light or dark");
    theme.order = json["order"].toInt(1000);
    if (!json["colors"].isObject()) return fail("colors must be an object");
    const auto colors = json["colors"].toObject();
    for (auto i = colors.begin(); i != colors.end(); ++i) {
        if (!roles.contains(i.key()) && !statusRoles.contains(i.key())) return fail("unknown color role: " + i.key());
        if (!i.value().isString() || !QRegularExpression("^#[0-9a-fA-F]{6}$").match(i.value().toString()).hasMatch())
            return fail("color must be #RRGGBB: " + i.key());
        theme.colors[i.key()] = QColor(i.value().toString());
    }
    for (const auto &role : roles) if (!theme.colors.contains(role)) return fail("missing color role: " + role);
    return theme;
}
}
QIcon uiIcon(UiIcon icon) { return QIcon(new LineIcon(icon)); }
QIcon applicationIcon() {
    QIcon icon;
    QSvgRenderer svg(assets() + "/turing-screen.svg");
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        QPixmap image(size, size); image.fill(Qt::transparent);
        QPainter painter(&image); svg.render(&painter); painter.end();
        icon.addPixmap(image);
    }
    return icon;
}
UiTheme loadUiTheme(const QString &id, const QString &dataDir) {
    const bool custom = id.startsWith("custom:");
    const auto token = custom ? id.mid(7) : id;
    if (!validId(token) || token == "system") {
        UiTheme bad; bad.id = id; bad.error = "Invalid UI palette identifier: " + id; return bad;
    }
    return readTheme((custom ? dataDir : assets()) + "/ui-themes/" + token + ".json", id);
}
QVector<UiTheme> uiThemes(const QString &dataDir) {
    QVector<UiTheme> result;
    for (bool custom : {false, true}) {
        const auto folder = (custom ? dataDir : assets()) + "/ui-themes";
        for (const auto &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
            const auto id = (custom ? QString("custom:") : QString()) + file.completeBaseName();
            result.append(loadUiTheme(id, dataDir));
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const UiTheme &a, const UiTheme &b) {
        if (a.id.startsWith("custom:") != b.id.startsWith("custom:")) return !a.id.startsWith("custom:");
        if (a.order != b.order) return a.order < b.order;
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return result;
}
QPalette UiTheme::palette() const {
    QPalette p;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const auto text = colors.value(group == QPalette::Disabled ? "disabled_text" : "text");
        p.setColor(group, QPalette::Window, colors["window"]);
        p.setColor(group, QPalette::Base, colors["base"]);
        p.setColor(group, QPalette::AlternateBase, colors["alternate_base"]);
        p.setColor(group, QPalette::Button, colors["button"]);
        for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(group, role, text);
        p.setColor(group, QPalette::ToolTipBase, colors["tooltip"]);
        p.setColor(group, QPalette::ToolTipText, colors["tooltip_text"]);
        p.setColor(group, QPalette::PlaceholderText, colors["muted"]);
        p.setColor(group, QPalette::BrightText, colors["accent_text"]);
        p.setColor(group, QPalette::Highlight, colors[group == QPalette::Disabled ? "border" : "accent"]);
        p.setColor(group, QPalette::HighlightedText, colors[group == QPalette::Disabled ? "disabled_text" : "accent_text"]);
        p.setColor(group, QPalette::Link, colors["link"]);
        p.setColor(group, QPalette::LinkVisited, colors["accent"]);
        p.setColor(group, QPalette::Light, colors["button"].lighter(125));
        p.setColor(group, QPalette::Midlight, colors["button"].lighter(110));
        p.setColor(group, QPalette::Mid, colors["border"]);
        p.setColor(group, QPalette::Dark, colors["border"]);
        p.setColor(group, QPalette::Shadow, colors["border"].darker(130));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        p.setColor(group, QPalette::Accent, colors["accent"]);
#endif
    }
    return p;
}
QJsonObject UiTheme::json() const {
    QJsonObject values;
    for (const auto &role : roles) values[role] = colors.value(role).name(QColor::HexRgb);
    for (const auto &role : statusRoles) if (colors.contains(role)) values[role] = colors.value(role).name(QColor::HexRgb);
    return {{"format", "turing-ui-palette"}, {"version", 1}, {"name", name}, {"mode", mode}, {"colors", values}};
}
UiAppearance &UiAppearance::instance() {
    static QPointer<UiAppearance> appearance;
    if (!appearance) appearance = new UiAppearance;
    return *appearance;
}
UiAppearance::UiAppearance() : QObject(qApp), systemPalette(qApp->palette()) {
    auto menuStyle = systemMenuStyle();
    menuPalette = menuStyle->standardPalette();
    delete menuStyle;
    QFile menuColors(assets() + "/ui/system-menu.json");
    const auto values = menuColors.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(menuColors.read(4096)).object() : QJsonObject();
    QFile menuSheet(assets() + "/ui/system-menu.qss");
    if (menuSheet.open(QIODevice::ReadOnly)) menuStyleSheet = QString::fromUtf8(menuSheet.read(8192));
    const QHash<QString, QList<QPalette::ColorRole>> menuRoles{{"background", {QPalette::Window, QPalette::Base, QPalette::Button}},
        {"text", {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}}, {"border", {QPalette::Mid}},
        {"hover", {QPalette::Highlight}}};
    for (auto i = menuRoles.begin(); i != menuRoles.end(); ++i) {
        const QColor value(values[i.key()].toString());
        if (value.isValid()) for (const auto role : i.value()) menuPalette.setColor(role, value);
    }
    const QColor text(values["text"].toString()), disabled(values["disabled_text"].toString());
    if (text.isValid()) menuPalette.setColor(QPalette::HighlightedText, text);
    if (disabled.isValid()) for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText})
        menuPalette.setColor(QPalette::Disabled, role, disabled);
    for (const auto &key : values.keys()) {
        const QColor value(values[key].toString());
        if (value.isValid()) menuStyleSheet.replace('@' + key + '@', value.name());
    }
    applying = true;
    qApp->setStyle(new FlatStyle);
    qApp->setPalette(systemPalette);
    refreshStyle();
    applying = false;
    qApp->installEventFilter(this);
}
QString UiAppearance::refreshStyle(bool resetFonts) {
    QFile input(assets() + "/ui/flat.qss");
    if (!input.open(QIODevice::ReadOnly)) return "Cannot load the flat interface style: " + input.errorString();
    auto sheet = QString::fromUtf8(input.read(128 * 1024));
    const auto p = qApp->palette();
    QFile statusFile(assets() + "/ui/status-colors.json");
    statusDefaults.clear();
    if (statusFile.open(QIODevice::ReadOnly)) {
        const auto mode = p.color(QPalette::Window).lightnessF() < .5 ? "dark" : "light";
        const auto defaults = QJsonDocument::fromJson(statusFile.read(4096)).object()[mode].toObject();
        for (const auto &role : statusRoles) {
            const QColor color(defaults[role].toString());
            if (color.isValid()) statusDefaults[role] = color;
        }
    }
    if (!iconDirectory.isValid()) return "Cannot create the interface icon cache";
    // Qt style sheets do not draw CSS border triangles consistently. Render
    // the small carets once per color and let Qt load them as ordinary icons.
    for (bool disabled : {false, true}) for (bool up : {false, true}) {
        const auto color = p.color(disabled ? QPalette::Disabled : QPalette::Active, QPalette::Text);
        const auto file = iconDirectory.path() + (up ? "/up-" : "/down-") + QString::number(color.rgba(), 16) + ".png";
        if (!QFileInfo::exists(file)) {
            QImage image(24, 24, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
            QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath caret; caret.moveTo(4, up ? 16 : 8); caret.lineTo(12, up ? 8 : 16); caret.lineTo(20, up ? 16 : 8);
            painter.drawPath(caret); painter.end();
            if (!image.save(file)) return "Cannot write the interface icon cache";
        }
        const auto token = QString("@caret_") + (up ? "up" : "down") + (disabled ? "_disabled@" : "@");
        sheet.replace(token, QDir::fromNativeSeparators(file));
    }
    const QHash<QString, QPalette::ColorRole> mapping{{"window", QPalette::Window}, {"base", QPalette::Base},
        {"button", QPalette::Button}, {"alternate_base", QPalette::AlternateBase}, {"text", QPalette::Text},
        {"muted", QPalette::PlaceholderText}, {"border", QPalette::Mid}, {"accent", QPalette::Highlight},
        {"accent_text", QPalette::HighlightedText}, {"tooltip", QPalette::ToolTipBase}, {"tooltip_text", QPalette::ToolTipText}};
    for (auto i = mapping.begin(); i != mapping.end(); ++i) sheet.replace('@' + i.key() + '@', p.color(i.value()).name());
    sheet.replace("@disabled_text@", p.color(QPalette::Disabled, QPalette::Text).name());
    const auto blend = [](QColor a, QColor b, double ratio) {
        return QColor::fromRgbF(a.redF() * (1 - ratio) + b.redF() * ratio,
                               a.greenF() * (1 - ratio) + b.greenF() * ratio,
                               a.blueF() * (1 - ratio) + b.blueF() * ratio).name();
    };
    sheet.replace("@hover@", blend(p.color(QPalette::Button), p.color(QPalette::Text), .08));
    sheet.replace("@accent_hover@", blend(p.color(QPalette::Highlight), p.color(QPalette::Text), .08));
    sheet.replace("@accent_pressed@", blend(p.color(QPalette::Highlight), p.color(QPalette::Base), .12));
    sheet.replace("@selection_tint@", blend(p.color(QPalette::Window), p.color(QPalette::Highlight), .18));
    if (resetFonts) qApp->setStyleSheet({});
    if (qApp->styleSheet() != sheet) qApp->setStyleSheet(sheet);
    return {};
}
bool UiAppearance::eventFilter(QObject *watched, QEvent *event) {
    if (watched == qApp && event->type() == QEvent::ApplicationFontChange && !applying) {
        QTimer::singleShot(0, this, [this] { applying = true; refreshStyle(true); applying = false;
            for (const auto &menu : systemMenus) if (menu) configureSystemMenu(menu);
        });
    }
    if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange && !applying) {
        const auto palette = qApp->palette();
        if (current == "system") {
            systemPalette = palette;
            applying = true; refreshStyle(); applying = false;
        } else if (palette != selectedPalette) {
            systemPalette = palette;
            const auto expected = current;
            QTimer::singleShot(0, this, [this, expected] {
                if (current != expected || current == "system") return;
                applying = true; qApp->setPalette(selectedPalette); refreshStyle(); applying = false;
            });
        }
        for (const auto &menu : systemMenus) if (menu) configureSystemMenu(menu);
    }
    return QObject::eventFilter(watched, event);
}
QString UiAppearance::apply(const QString &id, const QString &dataDir) {
    if (id == "system") {
        applying = true;
        if (current != "system") qApp->setPalette(systemPalette);
        current = "system";
        statusOverrides.clear();
        const auto error = refreshStyle();
        applying = false;
        return error;
    }
    const auto theme = loadUiTheme(id, dataDir);
    if (!theme.error.isEmpty()) return theme.error;
    if (current == "system") {
        systemPalette = qApp->palette();
    }
    applying = true;
    statusOverrides.clear();
    for (const auto &role : statusRoles) if (theme.colors.contains(role)) statusOverrides[role] = theme.colors[role];
    selectedPalette = theme.palette();
    if (qApp->palette() != selectedPalette) qApp->setPalette(selectedPalette);
    current = id;
    const auto error = refreshStyle();
    applying = false;
    return error;
}
QJsonObject UiAppearance::exportCurrent() const {
    const auto p = qApp->palette();
    UiTheme theme;
    theme.name = "My palette";
    theme.mode = p.color(QPalette::Window).lightnessF() < 0.5 ? "dark" : "light";
    const QHash<QString, QPalette::ColorRole> mapping{{"window", QPalette::Window}, {"base", QPalette::Base},
        {"button", QPalette::Button}, {"alternate_base", QPalette::AlternateBase}, {"text", QPalette::Text},
        {"muted", QPalette::PlaceholderText}, {"border", QPalette::Mid}, {"accent", QPalette::Highlight},
        {"accent_text", QPalette::HighlightedText}, {"link", QPalette::Link}, {"tooltip", QPalette::ToolTipBase},
        {"tooltip_text", QPalette::ToolTipText}};
    for (auto i = mapping.begin(); i != mapping.end(); ++i) theme.colors[i.key()] = p.color(QPalette::Active, i.value());
    theme.colors["disabled_text"] = p.color(QPalette::Disabled, QPalette::Text);
    for (const auto &role : statusRoles) theme.colors[role] = statusColor(role);
    return theme.json();
}
QColor UiAppearance::statusColor(const QString &tone) const {
    if (tone == "muted") return qApp->palette().color(QPalette::PlaceholderText);
    return statusOverrides.value(tone, statusDefaults.value(tone, qApp->palette().color(QPalette::Mid)));
}
void UiAppearance::configureSystemMenu(QMenu *menu) {
    if (menu->objectName().isEmpty()) menu->setObjectName("systemTrayMenu");
    if (!menu->property("nativeMenuStyle").toBool()) {
        // Keep the popup independent of Qt 6.8's native Windows animation
        // objects. Its own neutral sheet supplies the modern menu appearance.
        auto style = systemMenuStyle();
        style->setParent(menu);
        menu->setStyle(style);
        menu->setProperty("nativeMenuStyle", true);
        systemMenus.append(menu);
    }
    if (menu->styleSheet() != menuStyleSheet) menu->setStyleSheet(menuStyleSheet);
    auto pinned = menuPalette;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
        for (int role = 0; role < QPalette::NColorRoles; ++role)
            if (role != QPalette::NoRole) pinned.setColor(group, QPalette::ColorRole(role), menuPalette.color(group, QPalette::ColorRole(role)));
    menu->setPalette(pinned);
    menu->setFont(QApplication::font("QMenu"));
}
} // namespace Screen
