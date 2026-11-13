#pragma once
#include <QHash>
#include <QJsonObject>
#include <QIcon>
#include <QObject>
#include <QPalette>
#include <QVector>
#include <QTemporaryDir>
#include <QPointer>
class QMenu;
namespace Screen {
struct UiTheme {
    QString id, name, mode, file, error;
    int order = 0;
    QHash<QString, QColor> colors;
    QPalette palette() const;
    QJsonObject json() const;
};
QVector<UiTheme> uiThemes(const QString &dataDir);
UiTheme loadUiTheme(const QString &id, const QString &dataDir);
enum class UiIcon { Preview, Edit, Sensors, Settings, Lightweight, Undo, Redo, Search, Add, Locate, Expand, Collapse, Hide, SidebarExpand, SidebarCollapse, Import, Refresh };
QIcon uiIcon(UiIcon icon);
QIcon applicationIcon();
class UiAppearance : public QObject {
public:
    static UiAppearance &instance();
    QString apply(const QString &id, const QString &dataDir);
    QJsonObject exportCurrent() const;
    QColor statusColor(const QString &tone) const;
    void configureSystemMenu(QMenu *menu);
    QPalette systemMenuPalette() const { return menuPalette; }
    QString currentId() const { return current; }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    UiAppearance();
    QPalette systemPalette, selectedPalette, menuPalette;
    QTemporaryDir iconDirectory;
    QHash<QString, QColor> statusOverrides, statusDefaults;
    QString current = "system";
    QList<QPointer<QMenu>> systemMenus;
    QString menuStyleSheet;
    bool applying = false;
    QString refreshStyle(bool resetFonts = false);
};
} // namespace Screen
