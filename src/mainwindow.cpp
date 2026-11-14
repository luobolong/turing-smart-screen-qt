#include "mainwindow.h"
#include "uiappearance.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
#include <QMimeData>
#include <QDrag>
#include <QStandardItemModel>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace Screen {
namespace {
constexpr int sidebarIconCenter = 22;
constexpr int pageHeaderHeight = 64;
// Keep the icon column fixed while only the space for labels changes.
class SidebarButton : public QToolButton {
    const int rowHeight;
public:
    explicit SidebarButton(int height) : rowHeight(height) { setFixedHeight(height); }
    QSize sizeHint() const override { return QSize(44, rowHeight); }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        const auto caption = option.text;
        const auto image = option.icon;
        option.text.clear();
        option.icon = {};
        QPainter painter(this);
        painter.setFont(font());
        style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);
        const QRect iconRect(sidebarIconCenter - iconSize().width() / 2, (height() - iconSize().height()) / 2,
                             iconSize().width(), iconSize().height());
        image.paint(&painter, iconRect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
        if (toolButtonStyle() == Qt::ToolButtonTextBesideIcon)
            style()->drawItemText(&painter, QRect(44, 0, qMax(0, width() - 48), height()),
                                  Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                                  option.palette, isEnabled(), caption, QPalette::ButtonText);
    }
};
class NavigationDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const auto caption = item.text;
        const auto image = item.icon;
        item.text.clear();
        item.icon = {};
        const auto style = option.widget ? option.widget->style() : QApplication::style();
        painter->save();
        painter->setFont(item.font);
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, option.widget);
        image.paint(painter, QRect(sidebarIconCenter - 11, item.rect.center().y() - 11, 22, 22),
                    Qt::AlignCenter, item.state & QStyle::State_Selected ? QIcon::Selected : QIcon::Normal);
        style->drawItemText(painter, QRect(44, item.rect.top(), qMax(0, item.rect.right() - 48), item.rect.height()),
                            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, item.palette,
                            item.state & QStyle::State_Enabled, caption,
                            item.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
        painter->restore();
    }
};
}
class StatusLabel : public QLabel {
    QString caption, tone;
    void refreshStatus() {
        const auto color = UiAppearance::instance().statusColor(tone);
        setText("<span style=\"color:" + color.name() + "\">●</span> " + caption.toHtmlEscaped());
    }
public:
    void setState(const QString &text, const QString &state) {
        caption = text; tone = state;
        setProperty("statusTone", state);
        setAccessibleName(text);
        refreshStatus();
    }
protected:
    void changeEvent(QEvent *event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange) refreshStatus();
    }
};
class PaletteLinkLabel : public QLabel {
    QString original;
    void refreshLinks() {
        auto colored = original;
        colored.replace("<a href=", "<a style=\"color:" + palette().color(QPalette::Link).name() + "\" href=");
        QLabel::setText(colored);
    }
public:
    explicit PaletteLinkLabel(const QString &text = {}) { setText(text); }
    void setText(const QString &text) { original = text; refreshLinks(); }
protected:
    void changeEvent(QEvent *event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) refreshLinks();
    }
};
QString MainWindow::L(const char *zh, const char *en) const {
    return QString::fromUtf8(english ? en : zh);
}
static QPushButton *button(const QString &text, QLayout *layout, const std::function<void()> &fn) {
    auto b = new QPushButton(text);
    layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, fn);
    return b;
}
static QLabel *label(const QString &text, QLayout *layout) {
    auto l = new QLabel(text);
    layout->addWidget(l);
    return l;
}
static QLabel *heading(const QString &text, QLayout *layout) {
    auto result = label(text, layout);
    result->setProperty("uiRole", "heading");
    return result;
}
static void uiRole(QWidget *widget, const QString &role) {
    if (widget->property("uiRole").toString() == role) return;
    widget->setProperty("uiRole", role);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
static QString fileIdentity(const QString &file) {
    const auto canonical = QFileInfo(file).canonicalFilePath();
    return canonical.isEmpty() ? QFileInfo(file).absoluteFilePath() : canonical;
}
static bool withinDirectory(const QString &file, const QString &directory) {
    if (file.isEmpty())
        return false;
    const auto relative = QDir(fileIdentity(directory)).relativeFilePath(fileIdentity(file));
    return relative != "." && relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}
static bool storeConfig(const QString &directory, const QJsonObject &config, QString &error) {
    QSaveFile file(directory + "/config.json");
    const auto bytes = QJsonDocument(config).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
class LayerTree : public QTreeWidget {
public:
    std::function<void(QStringList, QString, int)> dropped;
    explicit LayerTree(QWidget *p = nullptr) : QTreeWidget(p) {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::DragDrop);
    }

protected:
    void dropEvent(QDropEvent *e) override {
        auto target = itemAt(e->position().toPoint());
        QString parent;
        int index = -1;
        if (target) {
            if (target->data(0, Qt::UserRole + 1).toString() == "group")
                parent = target->data(0, Qt::UserRole).toString();
            else if (target->parent())
                parent = target->parent()->data(0, Qt::UserRole).toString();
            if (target->data(0, Qt::UserRole + 1).toString() != "group") {
                auto p = target->parent();
                index = (p ? p->childCount() - 1 - p->indexOfChild(target)
                           : topLevelItemCount() - 1 - indexOfTopLevelItem(target));
            }
        }
        QStringList ids;
        for (auto i : selectedItems())
            ids.append(i->data(0, Qt::UserRole).toString());
        if (dropped)
            dropped(ids, parent, index);
        e->acceptProposedAction();
    }
};
static void dragLibraryPath(QWidget *widget, const QString &path) {
    if (path.isEmpty()) return;
    auto data = new QMimeData;
    data->setUrls({QUrl::fromLocalFile(path)});
    auto drag = new QDrag(widget);
    drag->setMimeData(data);
    drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
}
class ThemeFolders : public QTreeWidget {
public:
    std::function<void(QString, QString, bool)> dropped;
protected:
    void startDrag(Qt::DropActions) override {
        if (currentItem()) dragLibraryPath(this, currentItem()->data(0, Qt::UserRole).toString());
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (event->mimeData()->hasUrls()) event->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (event->mimeData()->hasUrls() && itemAt(event->position().toPoint())) event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override {
        const auto item = itemAt(event->position().toPoint());
        if (!item || !event->mimeData()->hasUrls()) return;
        auto target = item->data(0, Qt::UserRole).toString();
        if (item->data(0, Qt::UserRole + 1).toString() != "folder") target = item->parent() ? item->parent()->data(0, Qt::UserRole).toString() : QString();
        for (const auto &url : event->mimeData()->urls())
            if (url.isLocalFile() && dropped) dropped(url.toLocalFile(), target, event->modifiers() & Qt::ControlModifier);
        event->acceptProposedAction();
    }
};
class ThemeCatalogDelegate : public QStyledItemDelegate {
public:
    explicit ThemeCatalogDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return QSize(220, index.data(Qt::UserRole + 1).toString() == "folder" ? 32 : 80);
    }
protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(Qt::UserRole + 1).toString() == "folder") option->decorationSize = QSize(20, 20);
    }
};
MainWindow::MainWindow(QString dir, QString initial, QWidget *parent, std::shared_ptr<StartupBackend> startupBackend)
    : MainWindow(std::make_shared<DisplayRuntime>(std::move(dir), initial), parent, std::move(startupBackend)) {}
MainWindow::MainWindow(std::shared_ptr<DisplayRuntime> session, QWidget *parent, std::shared_ptr<StartupBackend> startupBackend)
    : QMainWindow(parent), runtime(std::move(session)), theme(runtime->theme), sample(runtime->sample),
      dataDir(runtime->dataDir), config(runtime->config), path(runtime->path), sending(runtime->sending),
      renderer(runtime->renderer), collector(runtime->collector), device(runtime->device),
      frame(runtime->frame), sampling(runtime->sampling) {
    resourceTrace("constructor body");
    english = config.value("language").toString("auto") == "english" ||
              (config.value("language").toString("auto") == "auto" &&
               QLocale::system().language() != QLocale::Chinese);
    startup = std::make_unique<StartupController>(startupBackend ? std::move(startupBackend) : nativeStartupBackend(dataDir));
    setWindowIcon(applicationIcon());
    connect(startup.get(), &StartupController::changed, this, &MainWindow::updateStartupUi);
    saved = theme.data;
    auto &appearance = UiAppearance::instance();
    uiThemeError = appearance.apply(config["ui_theme"].toString("system"), dataDir);
    if (!uiThemeError.isEmpty()) appearance.apply("system", dataDir);
    reloadLibrary();
    if (!path.isEmpty()) includeTheme(path);
    resourceTrace("catalog loaded");
    renderTimer.setSingleShot(true);
    connect(&renderTimer, &QTimer::timeout, this, &MainWindow::render);
    connect(runtime.get(), &DisplayRuntime::updated, this, &MainWindow::refreshRuntimeUi);
    connect(runtime.get(), &DisplayRuntime::weatherStatusChanged, this, &MainWindow::updateWeatherStatus);
    build();
    connect(&device, &Device::statusChanged, this, &MainWindow::updateStatus, Qt::QueuedConnection);
    resourceTrace("UI built");
    render();
    resourceTrace("rendered");
    setMinimumSize(640, 520);
    resize(1280, 820);
    resourceClock.start();
}
MainWindow::~MainWindow() {
    runtime->preview = false;
    runtime->allMetrics = false;
    runtime->weatherSettingsVisible = false;
    runtime->configureWeather();
    persist();
}
void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    runtime->preview = page == LibraryPage || page == EditorPage;
    runtime->allMetrics = page == SensorsPage;
    runtime->weatherSettingsVisible = page == SettingsPage;
    runtime->configureWeather();
    updateResponsiveLayout();
    updateSensorColumns();
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
    emit windowOpened();
}
void MainWindow::hideEvent(QHideEvent *event) {
    QMainWindow::hideEvent(event);
    runtime->preview = false;
    runtime->allMetrics = false;
    runtime->weatherSettingsVisible = false;
    runtime->configureWeather();
}
void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if ((event->oldSize().width() < 800) != (width() < 800))
        setSidebarExpanded(width() >= 800 && config.value("sidebar_expanded").toBool(false));
    updateResponsiveLayout();
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == pages || (library && watched == library->viewport())) && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &MainWindow::updateResponsiveLayout);
    if (sensorTable && ((watched == sensorTable->viewport() && event->type() == QEvent::Resize) ||
                        (watched == sensorTable && event->type() == QEvent::Show))) {
        auto table = sensorTable;
        QTimer::singleShot(0, table, [this, table] { if (table == sensorTable) updateSensorColumns(); });
    }
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::build() {
    building = true;
    const auto filter = search ? search->text() : QString();
    const bool searchShown = librarySearchPanel && !librarySearchPanel->isHidden();
    const auto selected = selection();
    const int inspectorPage = inspectorTabs ? inspectorTabs->currentIndex() : 0;
    auto old = takeCentralWidget();
    if (old)
        old->deleteLater();
    root = new QWidget;
    root->setObjectName("root");
    auto shell = new QHBoxLayout(root);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    auto sidebarFrame = new QFrame;
    sidebarFrame->setFrameShape(QFrame::NoFrame);
    sidebar = sidebarFrame;
    sidebar->setObjectName("sidebar");
    auto sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(8, 0, 8, 12);
    sideLayout->setSpacing(6);
    sidebarBrand = new SidebarButton(pageHeaderHeight);
    sidebarBrand->setObjectName("sidebarBrand");
    sidebarBrand->setAutoRaise(true);
    sidebarBrand->setFixedHeight(pageHeaderHeight);
    sidebarBrand->setIcon(applicationIcon());
    sidebarBrand->setIconSize(QSize(32, 32));
    sidebarBrand->setText(L("图灵智显", "Turing Screen"));
    sidebarBrand->setToolTip(sidebarBrand->text());
    sidebarBrand->setAccessibleName(sidebarBrand->text());
    sidebarBrand->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sideLayout->addWidget(sidebarBrand);
    navigation = new QListWidget;
    navigation->setObjectName("navigation");
    navigation->setAccessibleName(L("功能导航", "Navigation"));
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setBackgroundRole(QPalette::Window);
    navigation->viewport()->setBackgroundRole(QPalette::Window);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setIconSize(QSize(22, 22));
    navigation->setSpacing(3);
    navigation->setItemDelegate(new NavigationDelegate(navigation));
    const QStringList names{L("主题预览", "Theme preview"), L("主题编辑器", "Theme editor"),
                            L("传感器", "Sensors"), L("设置", "Settings")};
    const UiIcon icons[]{UiIcon::Preview, UiIcon::Edit, UiIcon::Sensors, UiIcon::Settings};
    for (int n = 0; n < PageCount; ++n) {
        auto item = new QListWidgetItem(uiIcon(icons[n]), names[n], navigation);
        item->setData(Qt::UserRole, names[n]);
        item->setData(Qt::AccessibleTextRole, names[n]);
        item->setToolTip(names[n]);
        item->setSizeHint(QSize(40, 44));
    }
    sideLayout->addWidget(navigation, 1);
    auto lightweight = new SidebarButton(44);
    lightweight->setObjectName("sidebarLightweight");
    lightweight->setText(L("轻量模式", "Lightweight Mode"));
    lightweight->setIcon(uiIcon(UiIcon::Lightweight));
    lightweight->setIconSize(QSize(22, 22));
    lightweight->setToolTip(L("进入轻量模式", "Enter lightweight mode"));
    lightweight->setAccessibleName(lightweight->toolTip());
    lightweight->setAutoRaise(true);
    lightweight->setFixedHeight(44);
    lightweight->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sideLayout->addWidget(lightweight);
    connect(lightweight, &QToolButton::clicked, this, &MainWindow::lightweightRequested);
    sidebarToggle = new SidebarButton(36);
    sidebarToggle->setObjectName("sidebarToggle");
    sidebarToggle->setAutoRaise(true);
    sidebarToggle->setFixedHeight(36);
    sidebarToggle->setIconSize(QSize(22, 22));
    sidebarToggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    sideLayout->addWidget(sidebarToggle);
    const auto toggleSidebar = [this] {
        setSidebarExpanded(!sidebarExpanded);
        config["sidebar_expanded"] = sidebarExpanded;
        persist();
        updateResponsiveLayout();
    };
    connect(sidebarToggle, &QToolButton::clicked, this, toggleSidebar);
    connect(sidebarBrand, &QToolButton::clicked, this, toggleSidebar);
    shell->addWidget(sidebar);
    auto sidebarSeparator = new QFrame;
    sidebarSeparator->setObjectName("sidebarSeparator");
    sidebarSeparator->setFrameShape(QFrame::VLine);
    sidebarSeparator->setFrameShadow(QFrame::Plain);
    sidebarSeparator->setLineWidth(1);
    sidebarSeparator->setForegroundRole(QPalette::Mid);
    sidebarSeparator->setFixedWidth(1);
    shell->addWidget(sidebarSeparator);
    auto content = new QWidget;
    content->setObjectName("mainContent");
    auto contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    titleBar = new QWidget;
    titleBar->setObjectName("titleBar");
    titleBar->setFixedHeight(pageHeaderHeight);
    auto header = new QHBoxLayout(titleBar);
    header->setContentsMargins(20, 12, 20, 12);
    pageTitle = label({}, header);
    pageTitle->setObjectName("pageTitle");
    header->addStretch();
    resolution = label({}, header);
    resolution->setProperty("uiRole", "caption");
    modeButton = button({}, header, [this] { setEditing(page != EditorPage); });
    modeButton->setObjectName("editThemeButton");
    saveButton = button(L("保存主题", "Save theme"), header, [this] { saveTheme(); });
    saveButton->setObjectName("saveThemeButton");
    saveButton->setProperty("uiRole", "primary");
    contentLayout->addWidget(titleBar);
    auto divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setLineWidth(1);
    divider->setForegroundRole(QPalette::Mid);
    contentLayout->addWidget(divider);
    auto pageContainer = new QWidget;
    auto outer = new QVBoxLayout(pageContainer);
    outer->setContentsMargins(16, 16, 16, 12);
    pages = new QStackedWidget;
    pages->setObjectName("pages");
    pages->installEventFilter(this);
    outer->addWidget(pages, 1);
    contentLayout->addWidget(pageContainer, 1);
    auto t = new QWidget;
    pages->addWidget(t);
    buildLibraryPage(t);
    auto editor = new QWidget;
    pages->addWidget(editor);
    buildEditorPage(editor);
    auto sensors = new QWidget;
    pages->addWidget(sensors);
    buildSensors(sensors);
    auto settings = new QWidget;
    pages->addWidget(settings);
    buildSettings(settings);
    shell->addWidget(content, 1);
    setCentralWidget(root);
    statusBar()->clearMessage();
    for (auto l : findChildren<QWidget *>("statusPart")) {
        statusBar()->removeWidget(l);
        l->deleteLater();
    }
    dataState = new StatusLabel;
    dataState->setObjectName("statusPart");
    connection = new StatusLabel;
    connection->setObjectName("statusPart");
    sendState = new StatusLabel;
    sendState->setObjectName("statusPart");
    status = new QLabel;
    status->setObjectName("statusPart");
    for (auto part : {dataState, status, connection, sendState}) {
        part->setMinimumWidth(0);
        part->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addPermanentWidget(dataState);
    statusBar()->addWidget(status, 1);
    statusBar()->addPermanentWidget(connection);
    statusBar()->addPermanentWidget(sendState);
    connect(navigation, &QListWidget::currentRowChanged, this, &MainWindow::setPage);
    navigation->setCurrentRow(page);
    pages->setCurrentIndex(page);
    setWindowTitle(L("图灵智显 · USB 副屏", "Turing Screen · USB Display"));
    for (auto a : findChildren<QAction *>("shortcut"))
        delete a;
    auto shortcut = [&](QKeySequence key, std::function<void()> fn) {
        auto a = new QAction(this);
        a->setObjectName("shortcut");
        a->setShortcut(key);
        addAction(a);
        connect(a, &QAction::triggered, this, fn);
    };
    shortcut(QKeySequence::Save, [this] { saveTheme(); });
    shortcut(QKeySequence::SaveAs, [this] { saveTheme(true); });
    shortcut(QKeySequence::Open, [this] { importTheme(); });
    shortcut(QKeySequence::New, [this] { newTheme(); });
    shortcut(QKeySequence::Undo, [this] { if (page == EditorPage) undo(); });
    shortcut(QKeySequence::Redo, [this] { if (page == EditorPage) redo(); });
    shortcut(QKeySequence(Qt::Key_F2), [this] {
        if (page == LibraryPage) {
            const auto file = folderTree->hasFocus() && folderTree->currentItem() ? folderTree->currentItem()->data(0, Qt::UserRole).toString()
                : library->currentItem() ? library->currentItem()->data(0, Qt::UserRole).toString() : QString();
            renameLibraryItem(file);
        } else if (editing && page == EditorPage) rename();
    });
    const QList<QPair<QKeySequence, QString>> shortcuts{
        {QKeySequence::Copy, "copy"},
        {QKeySequence::Paste, "paste"},
        {QKeySequence::Cut, "cut"},
        {QKeySequence(Qt::CTRL | Qt::Key_J), "duplicate"},
        {QKeySequence(Qt::CTRL | Qt::Key_G), "group"},
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G), "ungroup"},
        {QKeySequence(Qt::Key_Delete), "delete"}};
    for (auto pair : shortcuts)
        shortcut(pair.first, [this, pair] {
            if (page == LibraryPage && (library->hasFocus() || folderTree->hasFocus())) {
                const auto file = folderTree->hasFocus() && folderTree->currentItem() ? folderTree->currentItem()->data(0, Qt::UserRole).toString()
                    : library->currentItem() ? library->currentItem()->data(0, Qt::UserRole).toString() : QString();
                if (pair.second == "delete") deleteTheme(file, runtime->library.recycled(file));
                else if (pair.second == "copy" || pair.second == "cut") { fileClipboard = file; fileCut = pair.second == "cut"; }
                else if (pair.second == "paste" && !fileClipboard.isEmpty()) transferLibraryItem(fileClipboard, targetLibraryFolder(), !fileCut);
                return;
            }
            if (page == EditorPage && editing && !qobject_cast<QLineEdit *>(QApplication::focusWidget()))
                layerAction(pair.second);
        });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] {
        if (page == EditorPage) createLayer("text");
        else if (page == LibraryPage) newLibraryFolder();
    });
    shortcut(QKeySequence(Qt::Key_F5), [this] { if (page == LibraryPage) { reloadLibrary(); buildLibrary(); } });
    buildLibrary();
    buildTree();
    search->setText(filter);
    findChild<QToolButton *>("libraryToggleSearch")->setChecked(searchShown);
    selection(selected);
    inspectorTabs->setCurrentIndex(inspectorPage);
    emit preferencesChanged();
    building = false;
    setSidebarExpanded(width() >= 800 && config.value("sidebar_expanded").toBool(false));
    setPage(page);
    updateResponsiveLayout();
    updateStatus();
    properties();
}
void MainWindow::buildLibraryPage(QWidget *w) {
    auto layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    librarySplit = new PaneSplitter;
    librarySplit->setObjectName("librarySplit");
    librarySplit->showPaneText = L("展开%1", "Show %1");
    librarySplit->hidePaneText = L("收起%1", "Hide %1");
    auto browserPanel = new QWidget;
    libraryBrowser = browserPanel;
    browserPanel->setObjectName("libraryBrowser");
    auto browserLayout = new QVBoxLayout(browserPanel);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->setSpacing(6);
    auto tools = new QWidget;
    tools->setObjectName("libraryTools");
    auto bar = new QHBoxLayout(tools);
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    const auto tool = [&](UiIcon icon, const char *name, const QString &caption) {
        auto result = new QToolButton;
        result->setObjectName(name);
        result->setIcon(uiIcon(icon));
        result->setIconSize(QSize(18, 18));
        result->setToolTip(caption);
        result->setAccessibleName(caption);
        bar->addWidget(result);
        return result;
    };
    auto toggleSearch = tool(UiIcon::Search, "libraryToggleSearch", L("搜索主题", "Search themes"));
    toggleSearch->setCheckable(true);
    auto create = tool(UiIcon::Add, "libraryNew", L("新建", "New"));
    create->setPopupMode(QToolButton::InstantPopup);
    auto createMenu = new QMenu(create);
    createMenu->setObjectName("libraryNewMenu");
    createMenu->addAction(L("新建主题", "New theme"), this, &MainWindow::newTheme)->setObjectName("newTheme");
    createMenu->addAction(L("新建文件夹", "New folder"), this, [this] { newLibraryFolder(); })->setObjectName("newThemeFolder");
    create->setMenu(createMenu);
    auto import = tool(UiIcon::Import, "libraryImport", L("导入主题", "Import theme"));
    connect(import, &QToolButton::clicked, this, &MainWindow::importTheme);
    auto locate = tool(UiIcon::Locate, "libraryLocate", L("定位当前主题", "Locate current theme"));
    connect(locate, &QToolButton::clicked, this, &MainWindow::selectOpenedTheme);
    auto expand = tool(UiIcon::Expand, "libraryExpandSelected", L("展开所选文件夹", "Expand selected folder"));
    connect(expand, &QToolButton::clicked, this, &MainWindow::expandSelectedThemeFolder);
    auto collapse = tool(UiIcon::Collapse, "libraryCollapseAll", L("收起所有文件夹", "Collapse all folders"));
    auto hide = tool(UiIcon::Hide, "libraryHide", L("隐藏主题库", "Hide theme library"));
    connect(hide, &QToolButton::clicked, this, [this] { librarySplit->setPaneCollapsed(0, true); });
    bar->addStretch();
    browserLayout->addWidget(tools);
    librarySearchPanel = new QWidget;
    librarySearchPanel->setObjectName("librarySearchPanel");
    auto searchRow = new QHBoxLayout(librarySearchPanel);
    searchRow->setContentsMargins(0, 0, 0, 0);
    searchRow->setSpacing(4);
    search = new QLineEdit;
    search->setObjectName("themeSearch");
    search->setClearButtonEnabled(true);
    search->setPlaceholderText(L("搜索主题", "Search themes"));
    search->setMinimumWidth(0);
    searchRow->addWidget(search, 1);
    connect(search, &QLineEdit::textChanged, this, &MainWindow::searchLibrary);
    connect(search, &QLineEdit::returnPressed, this, &MainWindow::searchLibrary);
    browserLayout->addWidget(librarySearchPanel);
    librarySearchPanel->hide();
    connect(toggleSearch, &QToolButton::toggled, this, [this](bool shown) {
        librarySearchPanel->setVisible(shown);
        if (shown) search->setFocus();
    });
    libraryEmpty = new QLabel(L("没有找到匹配的主题，请尝试其他关键词。", "No matching themes. Try another search."));
    libraryEmpty->setWordWrap(true);
    libraryEmpty->hide();
    browserLayout->addWidget(libraryEmpty);
    auto browser = new ThemeFolders;
    library = browser;
    folderTree = browser;
    browser->setObjectName("themeLibrary");
    browser->setItemDelegate(new ThemeCatalogDelegate(browser));
    browser->setHeaderHidden(true);
    browser->setMinimumSize(240, 80);
    browser->setIndentation(16);
    browser->setIconSize(QSize(88, 60));
    browser->setWordWrap(false);
    browser->setTextElideMode(Qt::ElideRight);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setContextMenuPolicy(Qt::CustomContextMenu);
    browser->setDragEnabled(true);
    browser->setAcceptDrops(true);
    browser->setDragDropMode(QAbstractItemView::DragDrop);
    browser->viewport()->installEventFilter(this);
    browser->dropped = [this](QString file, QString folder, bool copy) { transferLibraryItem(file, folder, copy); };
    browserLayout->addWidget(browser, 1);
    librarySplit->addWidget(browserPanel);
    connect(collapse, &QToolButton::clicked, library, &QTreeWidget::collapseAll);
    auto preview = new QWidget;
    auto previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(12, 0, 0, 0);
    auto themeHeader = new QHBoxLayout;
    title = heading({}, themeHeader);
    title->setObjectName("previewThemeTitle");
    title->setMinimumWidth(0);
    title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto renameTheme = new QToolButton;
    renameTheme->setObjectName("renamePreviewTheme");
    renameTheme->setIcon(uiIcon(UiIcon::Edit));
    renameTheme->setToolTip(L("重命名主题", "Rename theme"));
    renameTheme->setAccessibleName(renameTheme->toolTip());
    themeHeader->addWidget(renameTheme);
    themeHeader->addStretch();
    connect(renameTheme, &QToolButton::clicked, this, [this] { renameLibraryItem(path); });
    previewLayout->addLayout(themeHeader);
    previewCanvas = new Canvas;
    previewCanvas->setObjectName("previewCanvas");
    previewCanvas->theme = &theme;
    previewCanvas->setMinimumSize(200, 160);
    previewLayout->addWidget(previewCanvas, 1);
    auto previewBottom = new QHBoxLayout;
    previewBottom->addStretch();
    previewBottom->addWidget(zoomControls(previewCanvas, "preview"));
    previewLayout->addLayout(previewBottom);
    preview->setMinimumWidth(200);
    librarySplit->addWidget(preview);
    devicePanel = new QWidget;
    devicePanel->setObjectName("devicePanel");
    devicePanel->setMinimumSize(200, 80);
    buildDevicePanel(devicePanel);
    librarySplit->addWidget(devicePanel);
    librarySplit->setStretchFactor(0, 0);
    librarySplit->setStretchFactor(1, 1);
    librarySplit->setStretchFactor(2, 0);
    librarySplit->setSizes({300, 540, 240});
    librarySplit->addCollapsiblePane(0, L("主题库", "theme library"), config["library_pane_hidden"].toBool());
    librarySplit->addCollapsiblePane(2, L("设备面板", "device panel"), config["device_pane_hidden"].toBool());
    librarySplit->collapsedChanged = [this](int index, bool collapsed) {
        config[index == 0 ? "library_pane_hidden" : "device_pane_hidden"] = collapsed;
        persist();
    };
    layout->addWidget(librarySplit, 1);
    connect(library, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item || building) return;
        const auto file = item->data(0, Qt::UserRole).toString();
        if (item->data(0, Qt::UserRole + 1).toString() == "folder") {
            libraryFolder = file;
            config["theme_folder"] = QDir(runtime->library.root).relativeFilePath(file);
            persist();
        } else {
            libraryFolder = item->parent() ? item->parent()->data(0, Qt::UserRole).toString() : runtime->library.root;
            choose(file);
        }
    });
    connect(library->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { thumbnails(); });
    connect(library, &QTreeWidget::itemExpanded, this, [this] { QTimer::singleShot(0, this, &MainWindow::thumbnails); });
    connect(library, &QTreeWidget::customContextMenuRequested, this, [this](QPoint position) {
        const auto item = library->itemAt(position);
        libraryMenu(item ? item->data(0, Qt::UserRole).toString() : QString(), library->viewport()->mapToGlobal(position));
    });
    connect(library, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, Qt::UserRole + 1).toString() != "folder" && !runtime->library.recycled(item->data(0, Qt::UserRole).toString())) setEditing(true);
    });
}
void MainWindow::buildEditorPage(QWidget *w) {
    auto layout = new QHBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto tools = new QToolBar;
    tools->setObjectName("editorTools");
    tools->setAccessibleName(L("编辑工具", "Editor tools"));
    tools->setOrientation(Qt::Vertical);
    tools->setMovable(false);
    tools->setToolButtonStyle(Qt::ToolButtonIconOnly);
    tools->setIconSize(QSize(22, 22));
    tools->setFixedWidth(44);
    auto toolGroup = new QActionGroup(tools);
    const auto addTool = [&](const QString &kind, const QString &label, UiIcon icon) {
        auto action = tools->addAction(uiIcon(icon), label);
        action->setObjectName("editorTool_" + kind);
        action->setCheckable(true);
        action->setChecked(kind == "move");
        toolGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, kind] { canvas->tool = kind; canvas->setFocus(); });
    };
    addTool("move", L("移动工具", "Move tool"), UiIcon::Move);
    addTool("text", L("文字工具", "Text tool"), UiIcon::Text);
    addTool("rectangle", L("矩形工具", "Rectangle tool"), UiIcon::Rectangle);
    addTool("ellipse", L("椭圆工具", "Ellipse tool"), UiIcon::Ellipse);
    addTool("line", L("直线工具", "Line tool"), UiIcon::Line);
    tools->addSeparator();
    const auto menuButton = [&](const QString &name, const QString &label, UiIcon icon) {
        auto button = new QToolButton;
        button->setObjectName(name);
        button->setText(label);
        button->setToolTip(label);
        button->setAccessibleName(label);
        button->setIcon(uiIcon(icon));
        button->setIconSize(tools->iconSize());
        button->setPopupMode(QToolButton::InstantPopup);
        auto menu = new QMenu(button);
        button->setMenu(menu);
        tools->addWidget(button);
        return menu;
    };
    auto creationMenu = menuButton("editorNewLayer", L("新建图层", "New layer"), UiIcon::Layers);
    creationMenu->setObjectName("editorNewLayerMenu");
    populateLayerMenu(creationMenu);
    auto componentsMenu = menuButton("editorComponents", L("添加组件", "Add component"), UiIcon::Components);
    for (auto kind : {"CPU", "GPU", "RAM", "NET", "CLOCK", "DISK"})
        componentsMenu->addAction(QString::fromLatin1(kind), this,
                                  [this, kind] { addComponent(QString::fromLatin1(kind)); });
    tools->addSeparator();
    undoAction = tools->addAction(uiIcon(UiIcon::Undo), L("撤销", "Undo"), this, &MainWindow::undo);
    redoAction = tools->addAction(uiIcon(UiIcon::Redo), L("重做", "Redo"), this, &MainWindow::redo);
    tools->addSeparator();
    auto themeMenu = menuButton("themeOptions", L("主题选项", "Theme options"), UiIcon::Settings);
    themeMenu->setObjectName("themeOptionsMenu");
    themeMenu->addAction(L("重命名主题", "Rename theme"), this, [this] {
        renameLibraryItem(path);
    });
    auto themeType = themeMenu->addMenu(L("主题类型", "Theme type"));
    themeType->setObjectName("themeTypeMenu");
    auto typeGroup = new QActionGroup(themeType);
    for (bool landscape : {true, false}) {
        auto action = themeType->addAction(landscape ? L("横屏主题", "Landscape theme") : L("竖屏主题", "Portrait theme"));
        action->setObjectName(landscape ? "landscapeTheme" : "portraitTheme");
        action->setCheckable(true);
        action->setData(landscape);
        typeGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, landscape] {
            if (theme.data["landscape"].toBool(true) != landscape)
                mutate([landscape](Theme &t) { t.data["landscape"] = landscape; });
        });
    }
    const auto updateType = [this, typeGroup] {
        for (auto action : typeGroup->actions()) action->setChecked(action->data().toBool() == theme.data["landscape"].toBool(true));
    };
    connect(themeType, &QMenu::aboutToShow, this, updateType);
    connect(themeMenu, &QMenu::aboutToShow, this, updateType);
    updateType();
    themeMenu->addAction(L("另存为", "Save as"), this, [this] { saveTheme(true); });
    themeMenu->addSeparator();
    deleteThemeAction = themeMenu->addAction(L("删除主题", "Delete theme"), this, [this] { deleteTheme(path); });
    layout->addWidget(tools);
    editorSplit = new PaneSplitter;
    editorSplit->setObjectName("editorSplit");
    editorSplit->showPaneText = L("展开%1", "Show %1");
    editorSplit->hidePaneText = L("收起%1", "Hide %1");
    editorSplit->setChildrenCollapsible(false);
    layout->addWidget(editorSplit, 1);
    auto center = new QWidget;
    auto cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    auto themeHeader = new QHBoxLayout;
    editorThemeTitle = heading({}, themeHeader);
    editorThemeTitle->setObjectName("editorThemeTitle");
    editorThemeTitle->setMinimumWidth(0);
    editorThemeTitle->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto renameTheme = new QToolButton;
    renameTheme->setObjectName("renameEditorTheme");
    renameTheme->setIcon(uiIcon(UiIcon::Edit));
    renameTheme->setToolTip(L("重命名主题", "Rename theme"));
    renameTheme->setAccessibleName(renameTheme->toolTip());
    themeHeader->addWidget(renameTheme);
    themeHeader->addStretch();
    connect(renameTheme, &QToolButton::clicked, this, [this] { renameLibraryItem(path); });
    cv->addLayout(themeHeader);
    auto layers = new QWidget;
    layers->setObjectName("layersPanel");
    auto llayers = new QVBoxLayout(layers);
    llayers->setContentsMargins(0, 0, 0, 0);
    llayers->setSpacing(0);
    auto tr = new LayerTree;
    tree = tr;
    tree->setObjectName("layerTree");
    tr->dropped = [this](QStringList ids, QString parent, int index) { reparent(ids, parent, index); };
    tree->setHeaderLabels({L("图层", "Layers"), L("锁定", "Lock")});
    tree->setColumnWidth(0, 190);
    tree->setColumnWidth(1, 44);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setIndentation(20);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    llayers->addWidget(tree, 1);
    auto layerTools = new QToolBar;
    layerTools->setObjectName("layerTools");
    layerTools->setAccessibleName(L("图层操作", "Layer actions"));
    layerTools->setMovable(false);
    layerTools->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layerTools->setIconSize(QSize(18, 18));
    layerActions.clear();
    const auto layerCommand = [&](const QString &key, const QString &label, UiIcon icon, const QString &shortcut = {}) {
        auto action = new QAction(uiIcon(icon), label + (shortcut.isEmpty() ? QString() : "\t" + shortcut), layers);
        action->setObjectName("layer_" + key);
        action->setToolTip(label + (shortcut.isEmpty() ? QString() : " (" + shortcut + ")"));
        layerActions.insert(key, action);
        connect(action, &QAction::triggered, this, [this, key] {
            if (key == "rename") rename();
            else if (key == "properties") inspectorTabs->setCurrentIndex(1);
            else layerAction(key);
        });
    };
    layerCommand("properties", L("图层属性…", "Layer properties…"), UiIcon::Settings);
    layerCommand("rename", L("重命名图层…", "Rename layer…"), UiIcon::Edit, "F2");
    layerCommand("duplicate", L("复制图层", "Duplicate layer"), UiIcon::Duplicate, "Ctrl+J");
    layerCommand("delete", L("删除图层", "Delete layer"), UiIcon::Delete, "Del");
    layerCommand("copy", L("复制", "Copy"), UiIcon::Copy, "Ctrl+C");
    layerCommand("paste", L("粘贴", "Paste"), UiIcon::Paste, "Ctrl+V");
    layerCommand("group", L("编组", "Group layers"), UiIcon::Group, "Ctrl+G");
    layerCommand("ungroup", L("取消编组", "Ungroup layers"), UiIcon::Ungroup, "Ctrl+Shift+G");
    layerCommand("front", L("置于顶层", "Bring to front"), UiIcon::Expand);
    layerCommand("back", L("置于底层", "Send to back"), UiIcon::Collapse);
    for (const auto key : {"rename", "copy", "paste", "group", "ungroup", "duplicate", "delete"}) {
        layerTools->addAction(layerActions.value(key));
        layerTools->widgetForAction(layerActions.value(key))->setAccessibleName(layerActions.value(key)->toolTip());
    }
    llayers->addWidget(layerTools);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::selectTree);
    connect(tree, &QTreeWidget::customContextMenuRequested, this,
            [this](QPoint p) {
                const auto item = tree->itemAt(p);
                if (!item) tree->clearSelection();
                else if (!item->isSelected()) { tree->clearSelection(); item->setSelected(true); tree->setCurrentItem(item); }
                contextMenu(tree->viewport()->mapToGlobal(p));
            });
    connect(tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *i, int column) {
        if (building)
            return;
        auto id = i->data(0, Qt::UserRole).toString();
        auto original = findLayer(theme.layers(), id);
        if (column == 0 && layerLocked(theme.layers(), id)) {
            QSignalBlocker blocker(tree);
            i->setText(0, original["name"].toString());
            i->setCheckState(0, original["visible"].toBool(true) ? Qt::Checked : Qt::Unchecked);
            return;
        }
        mutate([&](Theme &t) {
            auto a = t.layers();
            updateLayer(a, id, [&](QJsonObject &l) {
                if (column == 0) {
                    l["name"] = i->text(0);
                    l["visible"] = i->checkState(0) == Qt::Checked;
                } else
                    l["locked"] = i->checkState(1) == Qt::Checked;
            });
            t.data["layers"] = a;
        }, false); // The delegate is still using this item while it commits the check state or name.
        QSignalBlocker blocker(tree);
        i->setToolTip(0, original["kind"].toString() + "\n" + (i->checkState(0) == Qt::Checked
            ? L("可见 · 点击眼睛隐藏", "Visible · Click the eye to hide")
            : L("隐藏 · 点击眼睛显示", "Hidden · Click the eye to show")));
    });
    left = layers;
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    canvas = new Canvas;
    canvas->setObjectName("editorCanvas");
    canvas->setMinimumSize(200, 160);
    canvas->editing = true;
    canvas->setToolTip(L("拖动组合可整体移动；Alt+单击或双击可选择内部图层。也可在图层树中直接选择。",
                         "Drag a group to move it together. Alt-click or double-click selects a child; "
                         "individual layers can also be selected in the layer tree."));
    canvas->theme = &theme;
    scroll->setWidget(canvas);
    cv->addWidget(scroll, 1);
    auto options = new QHBoxLayout;
    auto grid = new QCheckBox(L("网格", "Grid"));
    options->addWidget(grid);
    connect(grid, &QCheckBox::toggled, this, [this](bool v) {
        canvas->grid = v;
        canvas->update();
    });
    auto snap = new QCheckBox(L("吸附", "Snap"));
    snap->setChecked(true);
    options->addWidget(snap);
    connect(snap, &QCheckBox::toggled, this, [this](bool v) { canvas->snap = v; });
    options->addStretch();
    options->addWidget(zoomControls(canvas, "editor"));
    cv->addLayout(options);
    editorSplit->addWidget(center);
    connect(canvas, &Canvas::selectionChanged, this, [this] {
        building = true;
        QTreeWidgetItemIterator it(tree);
        while (*it) {
            (*it)->setSelected(canvas->selected.contains((*it)->data(0, Qt::UserRole).toString()));
            ++it;
        }
        building = false;
        properties();
    });
    connect(canvas, &Canvas::transientEdited, this, [this] {
        if (!renderTimer.isActive())
            renderTimer.start(33);
    });
    connect(canvas, &Canvas::committed, this, [this](QJsonObject before) { remember(std::move(before)); });
    connect(canvas, &Canvas::createRequested, this, &MainWindow::createLayer);
    connect(canvas, &Canvas::contextRequested, this, &MainWindow::contextMenu);
    inspectorTabs = new QTabWidget;
    inspectorTabs->setObjectName("inspectorTabs");
    inspectorTabs->setMinimumSize(260, 140);
    inspectorTabs->addTab(layers, L("图层", "Layers"));
    auto propertyScroll = new QScrollArea;
    propertyScroll->setWidgetResizable(true);
    propertyScroll->setFrameShape(QFrame::NoFrame);
    right = new QWidget;
    auto rv = new QVBoxLayout(right);
    rv->setContentsMargins(12, 12, 12, 12);
    heading(L("图层属性", "Layer properties"), rv);
    auto props = new QFormLayout;
    layerName = new QLineEdit;
    layerText = new QLineEdit;
    bindingBox = new QComboBox;
    bindingBox->setMinimumContentsLength(12);
    bindingBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    bindingBox->addItem("--", QString());
    for (auto m : metrics())
        bindingBox->addItem(english ? m.first : m.second, m.first);
    props->addRow(L("名称", "Name"), layerName);
    props->addRow(L("内容", "Text"), layerText);
    props->addRow(L("数据", "Metric"), bindingBox);
    xBox = new QSpinBox;
    yBox = new QSpinBox;
    wBox = new QSpinBox;
    hBox = new QSpinBox;
    for (auto box : {xBox, yBox})
        box->setRange(-2048, 2048);
    for (auto box : {wBox, hBox})
        box->setRange(1, 1536);
    auto position = new QHBoxLayout;
    position->addWidget(xBox);
    position->addWidget(yBox);
    props->addRow("X / Y", position);
    auto dimensions = new QHBoxLayout;
    dimensions->addWidget(wBox);
    dimensions->addWidget(hBox);
    props->addRow(L("宽 / 高", "W / H"), dimensions);
    fontBox = new QSpinBox;
    fontBox->setRange(6, 128);
    props->addRow(L("字号", "Font size"), fontBox);
    rv->addLayout(props);
    auto visibility = new QHBoxLayout;
    visibleBox = new QCheckBox(L("可见", "Visible"));
    lockedBox = new QCheckBox(L("锁定", "Locked"));
    visibility->addWidget(visibleBox);
    visibility->addWidget(lockedBox);
    rv->addLayout(visibility);
    auto colors = new QHBoxLayout;
    fontColor = button(L("文字颜色", "Text color"), colors, [this] {
        auto ids = selection();
        if (ids.size() != 1)
            return;
        auto l = findLayer(theme.layers(), ids.first());
        auto c = QColorDialog::getColor(color(l["font"].toObject()["color"]), this,
                                        L("文字颜色", "Text color"), QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            mutate([&](Theme &t) {
                auto a = t.layers();
                updateLayer(a, ids.first(), [&](QJsonObject &o) {
                    auto f = o["font"].toObject();
                    f["color"] = rgba(c);
                    o["font"] = f;
                });
                t.data["layers"] = a;
            });
    });
    fillColor = button(L("填充颜色", "Fill color"), colors, [this] {
        auto ids = selection();
        if (ids.size() != 1)
            return;
        auto l = findLayer(theme.layers(), ids.first());
        auto c = QColorDialog::getColor(color(l["fill"], QColor("#28c9bf")), this,
                                        L("填充颜色", "Fill color"), QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            mutate([&](Theme &t) {
                auto a = t.layers();
                updateLayer(a, ids.first(), [&](QJsonObject &o) {
                    o["fill"] = rgba(c);
                    o["front"] = rgba(c);
                });
                t.data["layers"] = a;
            });
    });
    rv->addLayout(colors);
    for (auto edit : {layerName, layerText})
        connect(edit, &QLineEdit::editingFinished, this, &MainWindow::propertyEdited);
    for (auto box : {xBox, yBox, wBox, hBox, fontBox})
        connect(box, &QSpinBox::editingFinished, this, &MainWindow::propertyEdited);
    connect(bindingBox, &QComboBox::currentIndexChanged, this, [this] {
        if (!building)
            propertyEdited();
    });
    for (auto box : {visibleBox, lockedBox})
        connect(box, &QCheckBox::clicked, this, &MainWindow::propertyEdited);
    rv->addStretch();
    auto files = new QHBoxLayout;
    button(L("背景图", "Background"), files, [this] { backgroundImage(); });
    button(L("导出 PNG", "Export PNG"), files, [this] { exportImage(); });
    rv->addLayout(files);
    propertyScroll->setWidget(right);
    inspectorTabs->addTab(propertyScroll, L("属性", "Properties"));
    editorSplit->addWidget(inspectorTabs);
    editorSplit->setStretchFactor(0, 1);
    editorSplit->setStretchFactor(1, 0);
    editorSplit->setSizes({720, 280});
    editorSplit->addCollapsiblePane(1, L("图层和属性", "layers and properties"), config["editor_pane_hidden"].toBool());
    editorSplit->collapsedChanged = [this](int, bool collapsed) { config["editor_pane_hidden"] = collapsed; persist(); };
}
void MainWindow::buildDevicePanel(QWidget *w) {
    auto layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto scroll = new QScrollArea;
    scroll->setObjectName("deviceScroll");
    scroll->setMinimumSize(0, 70);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto content = new QWidget;
    content->setObjectName("deviceContent");
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto rv = new QVBoxLayout(content);
    rv->setContentsMargins(12, 0, 12, 12);
    heading(L("设备", "Device"), rv);
    auto hint = new QLabel(L("连接 USB 副屏后，可应用当前主题并调整显示参数。", "Connect a USB display to send the current theme and adjust display settings."));
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rv->addWidget(hint);
    rv->addWidget(collectionToggle("deviceCollectData"));
    portBox = new QComboBox;
    portBox->setObjectName("devicePort");
    portBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    portBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    portBox->setMinimumContentsLength(3);
    auto portRow = new QHBoxLayout;
    portRow->setSpacing(6);
    portRow->addWidget(portBox, 1);
    auto refresh = new QToolButton;
    refresh->setObjectName("refreshDevicePorts");
    refresh->setIcon(uiIcon(UiIcon::Refresh));
    refresh->setToolTip(L("刷新串口", "Refresh ports"));
    refresh->setAccessibleName(refresh->toolTip());
    connect(refresh, &QToolButton::clicked, this, &MainWindow::refreshPorts);
    portRow->addWidget(refresh);
    rv->addLayout(portRow);
    auto form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    directionBox = new QComboBox;
    directionBox->setObjectName("themeDirection");
    directionBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    directionBox->setMinimumContentsLength(1);
    directionBox->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    for (auto directionText : {L("竖屏", "Portrait"), L("反向竖屏", "Reverse portrait"),
                               L("横屏", "Landscape"), L("反向横屏", "Reverse landscape")})
        directionBox->addItem(directionText);
    directionBox->setCurrentIndex(theme.orientation());
    directionBox->setToolTip(L("方向按主题自动保存，切换主题或重启后会恢复。保存主题 JSON 时也会携带方向。",
                               "Orientation is remembered per theme across switches and restarts, "
                               "and is included when saving the theme JSON."));
    form->addRow(L("主题方向", "Theme orientation"), directionBox);
    brightness = new QSlider(Qt::Horizontal);
    brightness->setRange(0, 100);
    brightness->setValue(config.value("brightness").toInt(75));
    form->addRow(L("亮度", "Brightness"), brightness);
    mirrorBox = new QCheckBox(L("镜像显示", "Mirror display"));
    mirrorBox->setChecked(config["mirror"].toBool());
    form->addRow(mirrorBox);
    rv->addLayout(form);
    connect(directionBox, &QComboBox::currentIndexChanged, this, [this](int v) {
        applyThemeOrientation(v);
        persist();
        render();
    });
    connect(brightness, &QSlider::sliderReleased, this, [this] {
        config["brightness"] = brightness->value();
        device.command(3, brightness->value());
        persist();
    });
    connect(mirrorBox, &QCheckBox::toggled, this, [this](bool v) {
        config["mirror"] = v;
        canvas->mirrored = v;
        canvas->update();
        previewCanvas->mirrored = v;
        previewCanvas->update();
        device.command(5, int(v));
        persist();
    });
    auto advanced = new QToolButton;
    advanced->setText(L("高级设备操作", "Advanced operations"));
    advanced->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    auto advancedMenu = new QMenu(advanced);
    advancedMenu->addAction(L("清屏", "Clear"), this, [this] { device.command(6); });
    advancedMenu->addAction(L("休眠", "Sleep"), this, [this] {
        sending = false;
        device.command(8, 0);
        updateStatus();
    });
    advancedMenu->addAction(L("唤醒", "Wake"), this, [this] { device.command(8, 1); });
    advancedMenu->addAction(L("复位", "Reset"), this, [this] {
        sending = false;
        device.command(7);
        updateStatus();
    });
    advanced->setMenu(advancedMenu);
    advanced->setPopupMode(QToolButton::InstantPopup);
    rv->addWidget(advanced);
    advanced->setProperty("deviceOperation", true);
    connectButton = button({}, rv, [this] { connectDevice(); });
    connectButton->setObjectName("connectDevice");
    sendButton = button({}, rv, [this] { toggleSend(); });
    sendButton->setObjectName("sendButton");
    sendButton->setProperty("uiRole", "primary");
    rv->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll);
    refreshPorts();
}
void MainWindow::buildSensors(QWidget *w) {
    auto l = new QVBoxLayout(w);
    heading(L("本机传感器", "Local sensors"), l);
    label(L("默认每秒采集并更新副屏。内存已用、可用、总量以 GB 显示；无法读取的项目会在来源栏说明原因。",
            "Samples and updates the display every second by default. Used, available and total RAM are shown in GB. "
            "Unavailable sensors show a reason in the source column."),
          l)->setWordWrap(true);
    sensorTable = new QTableWidget(0, 3);
    sensorTable->setObjectName("sensorTable");
    sensorTable->setHorizontalHeaderLabels({L("项目", "Metric"), L("数值", "Value"), L("来源 / 状态", "Source / status")});
    auto header = sensorTable->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setMinimumSectionSize(80);
    header->setStretchLastSection(false);
    sensorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sensorTable->setShowGrid(true);
    sensorTable->setAlternatingRowColors(true);
    sensorTable->verticalHeader()->hide();
    sensorTable->verticalHeader()->setDefaultSectionSize(36);
    sensorTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    sensorTable->viewport()->installEventFilter(this);
    sensorTable->installEventFilter(this);
    auto table = sensorTable;
    connect(header, &QHeaderView::sectionResized, this, [this, table] {
        if (building || table != sensorTable || !table->property("columnsInitialized").toBool()) return;
        QJsonArray widths;
        for (int column = 0; column < table->columnCount(); ++column) widths.append(table->columnWidth(column));
        config["sensor_column_widths"] = widths;
        persist();
    });
    l->addWidget(sensorTable, 1);
    updateSensorColumns();
}
void MainWindow::updateSensorColumns() {
    if (!sensorTable) return;
    const auto savedWidths = config["sensor_column_widths"].toArray();
    const bool hasSavedWidths = savedWidths.size() == 3 && std::all_of(savedWidths.begin(), savedWidths.end(),
        [](const QJsonValue &value) { return value.isDouble() && value.toInt() >= 80 && value.toInt() <= 4096; });
    if (!hasSavedWidths && !sensorTable->isVisible()) return;
    if (hasSavedWidths && sensorTable->property("columnsInitialized").toBool()) return;
    const int available = sensorTable->viewport()->width();
    const int widths[]{qMax(80, available * 24 / 100), qMax(80, available * 28 / 100),
                       qMax(80, available - available * 24 / 100 - available * 28 / 100)};
    const QSignalBlocker blocker(sensorTable->horizontalHeader());
    for (int column = 0; column < 3; ++column)
        sensorTable->setColumnWidth(column, hasSavedWidths ? savedWidths[column].toInt() : widths[column]);
    sensorTable->setProperty("columnsInitialized", true);
}
QWidget *MainWindow::zoomControls(Canvas *target, const QString &prefix) {
    auto controls = new QWidget;
    controls->setObjectName(prefix + "ZoomControls");
    auto row = new QHBoxLayout(controls);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(4);
    const auto action = [&](const QString &name, const QString &caption, const QString &tooltip, auto callback) {
        auto button = new QToolButton;
        button->setObjectName(prefix + name);
        button->setText(caption);
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        connect(button, &QToolButton::clicked, controls, callback);
        row->addWidget(button);
        return button;
    };
    action("Fit", L("适应窗口", "Fit"), L("适应窗口", "Fit to window"), [target] { target->setFit(true); });
    action("ActualSize", "1:1", L("实际大小", "Actual size"), [target] { target->setZoom(1); });
    auto slider = new QSlider(Qt::Horizontal);
    slider->setObjectName(prefix + "ZoomSlider");
    slider->setRange(25, 400);
    slider->setMinimumWidth(40);
    slider->setMaximumWidth(96);
    slider->setToolTip(L("缩放", "Zoom"));
    slider->setAccessibleName(slider->toolTip());
    row->addWidget(slider);
    auto input = new QDoubleSpinBox;
    input->setObjectName(prefix + "ZoomValue");
    input->setProperty("zoomInput", true);
    input->setRange(10, 10000);
    input->setDecimals(2);
    input->setSingleStep(5);
    input->setSuffix(QStringLiteral("%"));
    input->setButtonSymbols(QAbstractSpinBox::NoButtons);
    input->setKeyboardTracking(false);
    input->setFixedWidth(92);
    input->setToolTip(L("缩放百分比（手动范围 25%–400%）", "Zoom percentage (manual range 25%–400%)"));
    input->setAccessibleName(input->toolTip());
    row->addWidget(input);
    action("ZoomOut", "−", L("缩小", "Zoom out"), [target] { target->setZoom(target->zoom() / 1.2); });
    action("ZoomIn", "+", L("放大", "Zoom in"), [target] { target->setZoom(target->zoom() * 1.2); });
    const auto sync = [target, slider, input] {
        const QSignalBlocker blockSlider(slider), blockInput(input);
        slider->setValue(qRound(target->zoom() * 100));
        input->setValue(target->zoom() * 100.);
    };
    connect(target, &Canvas::zoomChanged, controls, sync);
    connect(slider, &QSlider::valueChanged, controls, [target](int value) { target->setZoom(value / 100.); });
    connect(input, &QDoubleSpinBox::valueChanged, controls, [target](double value) { target->setZoom(value / 100.); });
    sync();
    return controls;
}
QCheckBox *MainWindow::collectionToggle(const QString &name) {
    auto toggle = new QCheckBox(L("采集数据", "Collect data"));
    toggle->setObjectName(name);
    toggle->setProperty("collectionToggle", true);
    toggle->setChecked(runtime->collectionEnabled());
    toggle->setToolTip(L("关闭后停止采集，保留最后一帧；重新开启时立即刷新。",
                        "Pause collection and keep the last frame. Resume to refresh immediately."));
    connect(toggle, &QCheckBox::toggled, this, [this](bool enabled) { runtime->setCollectionEnabled(enabled); });
    return toggle;
}
void MainWindow::buildSettings(QWidget *w) {
    auto pageLayout = new QVBoxLayout(w);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    auto content = new QWidget;
    content->setMaximumWidth(820);
    content->setObjectName("settingsContent");
    auto outer = new QVBoxLayout(content);
    scroll->setWidget(content);
    pageLayout->addWidget(scroll);
    outer->setSpacing(16);
    auto appearanceGroup = new QGroupBox(L("界面配色", "Interface colors"));
    auto appearanceForm = new QFormLayout(appearanceGroup);
    appearanceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    appearanceForm->setVerticalSpacing(10);
    appearanceForm->setHorizontalSpacing(16);
    uiThemeBox = new QComboBox;
    uiThemeBox->setObjectName("uiTheme");
    appearanceForm->addRow(L("配色主题", "Color theme"), uiThemeBox);
    uiThemeStatus = new QLabel;
    uiThemeStatus->setObjectName("uiThemeStatus");
    uiThemeStatus->setProperty("uiRole", "caption");
    uiThemeStatus->setWordWrap(true);
    uiThemeStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    appearanceForm->addRow(uiThemeStatus);
    auto appearanceButtons = new QHBoxLayout;
    appearanceButtons->setSpacing(8);
    auto reloadColors = button(L("重载配色", "Reload palettes"), appearanceButtons, [this] {
        selectUiTheme(config["ui_theme"].toString("system"));
        reloadUiThemes();
    });
    reloadColors->setObjectName("reloadUiThemes");
    auto copyColors = button(L("复制为自定义", "Copy to custom"), appearanceButtons, [this] { createUiTheme(); });
    copyColors->setObjectName("copyUiTheme");
    button(L("打开配色目录", "Open palette folder"), appearanceButtons, [this] {
        QDir().mkpath(dataDir + "/ui-themes");
        QDesktopServices::openUrl(QUrl::fromLocalFile(dataDir + "/ui-themes"));
    });
    appearanceForm->addRow(appearanceButtons);
    auto appearanceHint = new QLabel(L("配色由 JSON 文件定义；编辑后点击“重载配色”即可应用。",
        "Palettes are JSON files. After editing a file, click Reload palettes to apply it."));
    appearanceHint->setWordWrap(true);
    appearanceForm->addRow(appearanceHint);
    outer->addWidget(appearanceGroup);
    reloadUiThemes();
    connect(uiThemeBox, &QComboBox::currentIndexChanged, this, [this] {
        selectUiTheme(uiThemeBox->currentData().toString());
        reloadUiThemes();
    });
    auto group = new QGroupBox(L("运行设置", "Behavior"));
    auto form = new QFormLayout(group);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setVerticalSpacing(10);
    form->setHorizontalSpacing(16);
    form->addRow(collectionToggle("collectData"));
    auto language = new QComboBox;
    language->addItem("自动 / System", "auto");
    language->addItem("简体中文", "chinese");
    language->addItem("English", "english");
    language->setCurrentIndex(qMax(0, language->findData(config.value("language").toString("auto"))));
    form->addRow(L("语言", "Language"), language);
    connect(language, &QComboBox::currentIndexChanged, this, [this, language] {
        config["language"] = language->currentData().toString();
        english = language->currentData() == "english" ||
                  (language->currentData() == "auto" && QLocale::system().language() != QLocale::Chinese);
        persist();
        QTimer::singleShot(0, this, [this] {
            build();
            render();
        });
    });
    auto interval = new QSpinBox;
    interval->setRange(1000, 10000);
    interval->setSingleStep(500);
    interval->setSuffix(" ms");
    interval->setValue(config.value("interval_ms").toInt(1000));
    form->addRow(L("采集 / 发送间隔", "Sample / send interval"), interval);
    connect(interval, &QSpinBox::valueChanged, this, [this](int n) {
        config["interval_ms"] = n;
        sampling.setInterval(n);
        persist();
    });
    startupToggle = new QCheckBox(L("开机启动（轻量模式）", "Start at sign-in in lightweight mode"));
    startupToggle->setObjectName("startupEnabled");
    form->addRow(startupToggle);
    startupStatus = new QLabel;
    startupStatus->setObjectName("startupStatus");
    startupStatus->setWordWrap(true);
    form->addRow(startupStatus);
    updateStartupUi();
    connect(startupToggle, &QCheckBox::clicked, this, [this](bool enabled) { startup->setEnabled(enabled); });
    auto closeTray = new QCheckBox(L("关闭窗口时最小化到托盘", "Close window to tray"));
    closeTray->setChecked(config["minimize_to_tray"].toBool());
    form->addRow(closeTray);
    connect(closeTray, &QCheckBox::clicked, this, [this](bool v) {
        config["minimize_to_tray"] = v;
        persist();
    });
    auto autoLightweight = new QCheckBox(L("关闭窗口后自动进入轻量模式", "Enter lightweight mode after closing the window"));
    autoLightweight->setObjectName("autoLightweight");
    autoLightweight->setChecked(config["auto_lightweight"].toBool());
    form->addRow(autoLightweight);
    auto delay = new QSpinBox;
    delay->setObjectName("lightweightDelay");
    delay->setRange(0, 86400);
    delay->setSuffix(" s");
    delay->setSpecialValueText(L("立即（0 秒）", "Immediately (0 s)"));
    delay->setValue(config.value("lightweight_delay_seconds").toInt(60));
    form->addRow(L("进入轻量模式倒计时", "Lightweight countdown"), delay);
    form->labelForField(delay)->setObjectName("lightweightDelayLabel");
    auto enterLightweight = new QPushButton(L("立即进入轻量模式", "Enter lightweight mode now"));
    enterLightweight->setObjectName("enterLightweight");
    enterLightweight->setEnabled(QSystemTrayIcon::isSystemTrayAvailable());
    connect(enterLightweight, &QPushButton::clicked, this, &MainWindow::lightweightRequested);
    form->addRow(enterLightweight);
    auto lightweightHint = new QLabel(L("关闭窗口后开始计时，0 秒立即进入；重新打开取消。释放界面并继续推送，可从系统托盘恢复。",
        "Closing the window starts the countdown; 0 seconds enters immediately. Reopening cancels it. Display updates continue; restore from the system tray."));
    lightweightHint->setWordWrap(true);
    form->addRow(lightweightHint);
    const auto showCountdown = [form, delay, lightweightHint](bool enabled) {
        form->setRowVisible(delay, enabled);
        form->setRowVisible(lightweightHint, enabled);
    };
    showCountdown(autoLightweight->isChecked());
    const auto updateLightweight = [this, autoLightweight, delay, showCountdown] {
        config["auto_lightweight"] = autoLightweight->isChecked();
        config["lightweight_delay_seconds"] = delay->value();
        showCountdown(autoLightweight->isChecked());
        persist();
        emit preferencesChanged();
    };
    connect(autoLightweight, &QCheckBox::toggled, this, updateLightweight);
    connect(delay, &QSpinBox::valueChanged, this, updateLightweight);
    auto disk = new QComboBox;
    disk->setObjectName("diskPartition");
    disk->addItem(L("系统分区（%1，默认）", "System drive (%1, default)")
                      .arg(QDir::toNativeSeparators(QDir::rootPath())), QString());
    for (auto d : QStorageInfo::mountedVolumes())
        if (d.isReady() && !d.isReadOnly())
            disk->addItem(d.rootPath(), d.rootPath());
    disk->setCurrentIndex(qMax(0, disk->findData(config["disk"].toString())));
    form->addRow(L("磁盘空间监控", "Disk space monitoring"), disk);
    connect(disk, &QComboBox::currentIndexChanged, this, [this, disk] {
        config["disk"] = disk->currentData().toString();
        persist();
    });
    auto weatherEnabled = new QCheckBox(L("启用天气（每 10 分钟更新）", "Enable weather (updates every 10 minutes)"));
    weatherEnabled->setObjectName("weatherEnabled");
    const auto weatherOptions = theme.data.value("weather").toObject();
    weatherEnabled->setChecked(config["weather_enabled"].toBool(weatherOptions["enabled"].toBool()));
    form->addRow(weatherEnabled);
    auto weatherCity = new QLineEdit(config["weather_city"].toString(weatherOptions["city"].toString()));
    weatherCity->setObjectName("weatherCity");
    weatherCity->setPlaceholderText(L("城市；留空按 IP 粗略定位", "City; leave empty for approximate IP location"));
    form->addRow(L("天气城市", "Weather city"), weatherCity);
    const auto applyWeather = [this, weatherEnabled, weatherCity] {
        config["weather_enabled"] = weatherEnabled->isChecked();
        config["weather_city"] = weatherCity->text().trimmed();
        configureWeather();
        persist();
    };
    connect(weatherEnabled, &QCheckBox::toggled, this, applyWeather);
    connect(weatherCity, &QLineEdit::editingFinished, this, applyWeather);
    auto weatherActions = new QWidget;
    auto weatherRow = new QHBoxLayout(weatherActions);
    weatherRow->setContentsMargins(0, 0, 0, 0);
    weatherRow->setSpacing(12);
    auto fetchWeather = new QPushButton(L("立即获取天气信息", "Fetch weather now"));
    fetchWeather->setObjectName("fetchWeatherNow");
    weatherRow->addWidget(fetchWeather);
    auto weatherStatus = new StatusLabel;
    weatherStatus->setObjectName("weatherStatus");
    weatherStatus->setWordWrap(true);
    weatherStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    weatherStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    weatherRow->addWidget(weatherStatus, 1);
    form->addRow(weatherActions);
    connect(fetchWeather, &QPushButton::clicked, this, [this, applyWeather] {
        applyWeather();
        runtime->requestWeather();
        updateWeatherStatus();
    });
    updateWeatherStatus();
    auto weatherCredit = new PaletteLinkLabel(L("天气：<a href=\"https://open-meteo.com/\">Open-Meteo</a>（CC BY 4.0）。留空城市时使用 ipwho.is。",
                                     "Weather: <a href=\"https://open-meteo.com/\">Open-Meteo</a> (CC BY 4.0). Empty city uses ipwho.is."));
    weatherCredit->setOpenExternalLinks(true);
    weatherCredit->setObjectName("weatherCredit");
    weatherCredit->setWordWrap(true);
    form->addRow(weatherCredit);
    outer->addWidget(group);
#ifdef Q_OS_WIN
    auto sensorsGroup = new QGroupBox(L("Windows 传感器", "Windows sensors"));
    auto sensorsForm = new QFormLayout(sensorsGroup);
    sensorsForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    sensorsForm->setVerticalSpacing(10);
    sensorsForm->setHorizontalSpacing(16);
    auto source = new QComboBox;
    source->setObjectName("windowsSensorSource");
    source->addItem(L("仅系统数据", "System data only"), "system");
    source->addItem("HWiNFO", "hwinfo");
    source->addItem("AIDA64", "aida64");
    source->addItem("LibreHardwareMonitor", "librehardwaremonitor");
    source->setCurrentIndex(qMax(0, source->findData(windowsSensorSource(config["windows_sensor_source"].toString()))));
    sensorsForm->addRow(L("采集数据来源", "Sensor data source"), source);
    auto gpuIndex = new QSpinBox;
    gpuIndex->setObjectName("windowsGpuIndex");
    gpuIndex->setRange(1, 32);
    gpuIndex->setValue(qBound(1, config["windows_gpu_index"].toInt(0) + 1, 32));
    sensorsForm->addRow(L("显卡序号", "GPU number"), gpuIndex);
    auto lhmPort = new QSpinBox;
    lhmPort->setObjectName("lhmPort");
    lhmPort->setRange(1, 65535);
    lhmPort->setValue(qBound(1, config["lhm_port"].toInt(8085), 65535));
    sensorsForm->addRow(L("LHM 本机端口", "LHM local port"), lhmPort);
    auto sourceHint = new PaletteLinkLabel;
    sourceHint->setObjectName("windowsSensorHint");
    sourceHint->setWordWrap(true);
    sourceHint->setOpenExternalLinks(true);
    sensorsForm->addRow(sourceHint);
    const auto updateSourceUi = [this, source, gpuIndex, lhmPort, sensorsForm, sourceHint] {
        const auto value = source->currentData().toString();
        sensorsForm->setRowVisible(lhmPort, value == "librehardwaremonitor");
        sensorsForm->setRowVisible(gpuIndex, value != "system");
        if (value == "hwinfo") sourceHint->setText(L(
            "运行 <a href=\"https://www.hwinfo.com/download/\">HWiNFO</a>，开启 Sensors 和 Shared Memory Support。免费版共享内存连续运行限 12 小时，长期常开请使用 Pro。",
            "Run <a href=\"https://www.hwinfo.com/download/\">HWiNFO</a> Sensors and enable Shared Memory Support. Free shared memory is limited to 12 continuous hours; use Pro for continuous operation."));
        else if (value == "aida64") sourceHint->setText(L(
            "运行 <a href=\"https://www.aida64.com/downloads\">AIDA64</a>，在设置 → 硬件监控 → 外部程序中启用共享内存，并勾选所需指标。试用版可能限制部分数据。",
            "Run <a href=\"https://www.aida64.com/downloads\">AIDA64</a>. In Preferences → Hardware Monitoring → External Applications, enable shared memory and select the required sensors. Trial editions may restrict values."));
        else if (value == "librehardwaremonitor") sourceHint->setText(L(
            "运行 <a href=\"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases\">LibreHardwareMonitor</a>，启用 Options → Remote Web Server → Run，并将端口设为上方数值。温度等指标可能需要管理员权限及对应驱动。本程序只读取 127.0.0.1。",
            "Run <a href=\"https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/releases\">LibreHardwareMonitor</a>. Enable Options → Remote Web Server → Run with the port above. Hardware sensors may require administrator privileges and the appropriate driver. Only 127.0.0.1 is queried."));
        else sourceHint->setText(L("系统来源提供 CPU 使用率、内存和网络等数据。温度、GPU 和实时 CPU 频率需选择第三方来源。",
            "The system source provides CPU usage, memory and networking. Select a third-party source for temperature, GPU and live CPU clocks."));
    };
    const auto applySource = [this, source, gpuIndex, lhmPort, updateSourceUi] {
        config["windows_sensor_source"] = source->currentData().toString();
        config["windows_gpu_index"] = gpuIndex->value() - 1;
        config["lhm_port"] = lhmPort->value();
        runtime->configureSensors();
        updateSourceUi();
        persist();
        tick();
    };
    updateSourceUi();
    connect(source, &QComboBox::currentIndexChanged, this, applySource);
    connect(gpuIndex, &QSpinBox::valueChanged, this, applySource);
    connect(lhmPort, &QSpinBox::valueChanged, this, applySource);
    outer->addWidget(sensorsGroup);
#endif
    outer->addStretch();
    auto buttons = new QHBoxLayout;
    button(L("打开数据目录", "Open data folder"), buttons,
           [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dataDir)); });
    buttons->addStretch();
    outer->addLayout(buttons);
}
void MainWindow::reloadUiThemes() {
    const QSignalBlocker blocker(uiThemeBox);
    const auto selected = config["ui_theme"].toString("system");
    uiThemeBox->clear();
    uiThemeBox->addItem(L("跟随系统", "Follow system"), "system");
    const auto themes = uiThemes(dataDir);
    for (const auto &palette : themes) {
        auto name = palette.name.isEmpty() ? palette.id : palette.name;
        if (palette.id.startsWith("custom:")) name = L("自定义 · ", "Custom · ") + name;
        name += !palette.error.isEmpty() ? L("（无效）", " (Invalid)") : palette.mode == "light" ? L("（浅色）", " (Light)") : L("（深色）", " (Dark)");
        uiThemeBox->addItem(name, palette.id);
        const int row = uiThemeBox->count() - 1;
        uiThemeBox->setItemData(row, palette.error.isEmpty() ? palette.file : palette.error, Qt::ToolTipRole);
        if (!palette.error.isEmpty()) {
            auto model = qobject_cast<QStandardItemModel *>(uiThemeBox->model());
            if (model) model->item(row)->setEnabled(false);
        }
    }
    int index = uiThemeBox->findData(selected);
    if (index < 0) {
        uiThemeBox->addItem(L("找不到配色：", "Palette not found: ") + selected, selected);
        index = uiThemeBox->count() - 1;
    }
    uiThemeBox->setCurrentIndex(index);
    uiThemeStatus->setTextFormat(Qt::PlainText);
    if (!uiThemeError.isEmpty()) uiThemeStatus->setText(L("配色加载失败：", "Could not load palette: ") + uiThemeError);
    else if (selected == "system") uiThemeStatus->setText(L("跟随系统的 Qt 外观和配色。", "Use the system's Qt style and colors."));
    else uiThemeStatus->setText(uiThemeBox->itemData(index, Qt::ToolTipRole).toString());
}
void MainWindow::selectUiTheme(const QString &id) {
    uiThemeError = UiAppearance::instance().apply(id, dataDir);
    if (uiThemeError.isEmpty()) {
        config["ui_theme"] = id;
        persist();
        updateStatus();
        emit preferencesChanged();
    }
}
void MainWindow::createUiTheme() {
    const auto folder = dataDir + "/ui-themes";
    if (!QDir().mkpath(folder)) {
        uiThemeError = L("无法创建配色目录", "Cannot create palette folder");
        reloadUiThemes();
        return;
    }
    QString token = "my-palette";
    int count = 1;
    while (QFileInfo::exists(folder + '/' + token + ".json")) token = "my-palette-" + QString::number(++count);
    auto json = UiAppearance::instance().exportCurrent();
    json["name"] = L("自定义配色", "My palette") + (count > 1 ? ' ' + QString::number(count) : QString());
    QSaveFile file(folder + '/' + token + ".json");
    const auto bytes = QJsonDocument(json).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        uiThemeError = file.errorString();
    else selectUiTheme("custom:" + token);
    reloadUiThemes();
}
void MainWindow::updateStartupUi() {
    if (!startupToggle || !startupStatus) return;
    const QSignalBlocker blocker(startupToggle);
    const auto &state = startup->state();
    startupToggle->setChecked(state.enabled);
    startupToggle->setEnabled(startup->ready() && !startup->busy() && state.available);
    if (startup->busy() || !startup->ready())
        startupStatus->setText(L("正在检查或更新启动设置…", "Checking or updating startup settings…"));
    else if (!state.error.isEmpty())
        startupStatus->setText(L("启动设置：", "Startup: ") + state.error);
    else
        startupStatus->setText(state.enabled
            ? L("已启用，下次登录后自动连接副屏、开始发送并进入轻量模式，点击托盘图标打开窗口。",
                "Enabled. Automatically connects to the display, starts sending and enters lightweight mode at your next sign-in. Click the tray icon to open the window.")
            : L("已关闭，不影响当前运行和副屏发送。", "Disabled. The current session and display updates continue."));
}
void MainWindow::configureWeather() {
    const auto options = theme.data.value("weather").toObject();
    const bool enabled = config["weather_enabled"].toBool(options["enabled"].toBool());
    const auto city = config["weather_city"].toString(options["city"].toString());
    runtime->configureWeather();
    if (root) {
        if (const auto checkbox = root->findChild<QCheckBox *>("weatherEnabled")) {
            QSignalBlocker blocker(checkbox);
            checkbox->setChecked(enabled);
        }
        if (const auto field = root->findChild<QLineEdit *>("weatherCity")) {
            QSignalBlocker blocker(field);
            field->setText(city);
        }
    }
    updateWeatherStatus();
}
void MainWindow::updateWeatherStatus() {
    if (!root) return;
    auto button = root->findChild<QPushButton *>("fetchWeatherNow");
    auto label = root->findChild<QLabel *>("weatherStatus");
    if (!button || !label) return;
    const auto options = theme.data.value("weather").toObject();
    const bool enabled = config.value("weather_enabled").toBool(options["enabled"].toBool());
    const auto state = collector.weatherStatus();
    button->setEnabled(enabled && runtime->collectionEnabled() && !state.busy);
    QString text, tone = "muted";
    if (!runtime->collectionEnabled()) text = L("采集已暂停，请先开启“采集数据”。", "Collection is paused. Enable Collect data first.");
    else if (!enabled) text = L("请先启用天气。", "Enable weather first.");
    else if (state.busy) text = L("正在获取天气信息…", "Fetching weather…");
    else if (!state.error.isEmpty()) {
        text = L("获取失败：", "Failed: ") + state.error;
        tone = "error";
    } else if (state.updated > 0) {
        text = L("已获取：%1（%2）", "Updated: %1 (%2)")
            .arg(state.summary, QDateTime::fromMSecsSinceEpoch(state.updated).toString("HH:mm:ss"));
        tone = "success";
    } else text = L("点击按钮获取最新天气信息。", "Click to fetch the latest weather.");
    static_cast<StatusLabel *>(label)->setState(text, tone);
    label->setToolTip(text);
}
void MainWindow::setPage(int n) {
    if (n < 0 || n >= PageCount)
        return;
    page = n;
    const QSignalBlocker blocker(navigation);
    navigation->setCurrentRow(n);
    pages->setCurrentIndex(n);
    editing = n == EditorPage;
    canvas->editing = editing;
    canvas->update();
    const bool themePage = n == LibraryPage || n == EditorPage;
    runtime->preview = isVisible() && themePage;
    runtime->allMetrics = isVisible() && n == SensorsPage;
    runtime->weatherSettingsVisible = isVisible() && n == SettingsPage;
    runtime->configureWeather();
    if (runtime->allMetrics) runtime->tick();
    pageTitle->setText(navigation->item(n)->data(Qt::UserRole).toString());
    setWindowTitle(pageTitle->text() + L(" · 图灵智显", " · Turing Screen"));

    resolution->setVisible(themePage && pages->width() >= 850);
    modeButton->setVisible(themePage);
    uiRole(modeButton, n == LibraryPage ? "primary" : "");
    modeButton->setText(n == EditorPage ? L("返回预览", "Back to preview") : L("编辑主题", "Edit theme"));
    saveButton->setVisible(n == EditorPage);
    if (building)
        return;
    if (n == SettingsPage) startup->refresh();
    if (n == SensorsPage) {
        updateSensorColumns();
        tick();
    }
    if (themePage) {
        render();
        QTimer::singleShot(0, this, [this] {
            if (page == LibraryPage) {
                library->doItemsLayout();
                thumbnails();
            }
        });
    }
}
void MainWindow::setEditing(bool value) {
    setPage(value ? EditorPage : LibraryPage);
    properties();
}
void MainWindow::setSidebarExpanded(bool expanded) {
    if (!sidebar || !navigation)
        return;
    sidebarExpanded = expanded;
    sidebar->setFixedWidth(expanded ? 208 : 60);
    sidebarBrand->setToolButtonStyle(expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
    sidebarToggle->setToolButtonStyle(Qt::ToolButtonIconOnly);
    sidebarToggle->setIcon(uiIcon(expanded ? UiIcon::SidebarCollapse : UiIcon::SidebarExpand));
    const auto description = expanded ? L("收起功能栏", "Collapse sidebar") : L("展开功能栏", "Expand sidebar");
    sidebarToggle->setText({});
    sidebarToggle->setToolTip(description);
    sidebarToggle->setAccessibleName(description);
    sidebarBrand->setToolTip(sidebarBrand->text() + " · " + description);
    for (int n = 0; n < navigation->count(); ++n) {
        auto item = navigation->item(n);
        item->setText(expanded ? item->data(Qt::UserRole).toString() : QString());
    }
    sidebar->findChild<QToolButton *>("sidebarLightweight")->setToolButtonStyle(
        expanded ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
}
void MainWindow::updateResponsiveLayout() {
    if (!pages || !librarySplit || !editorSplit)
        return;
    const auto reflow = [](PaneSplitter *splitter, bool vertical, const QList<int> &wideSizes,
                           const QList<int> &narrowSizes) {
        const auto orientation = vertical ? Qt::Vertical : Qt::Horizontal;
        if (splitter->orientation() != orientation) {
            splitter->setPaneOrientation(orientation, vertical ? narrowSizes : wideSizes);
        }
    };
    const bool compactPreview = pages->width() < 760;
    libraryBrowser->setMaximumWidth(compactPreview ? QWIDGETSIZE_MAX : 380);
    devicePanel->setMaximumWidth(compactPreview ? QWIDGETSIZE_MAX : 340);
    reflow(librarySplit, compactPreview, {300, 540, 240}, {220, 260, 180});
    // Use the page width: on a rebuild the splitter has not been laid out yet.
    // Reserve space for the vertical toolbar and its layout spacing.
    reflow(editorSplit, pages->width() < 730, {720, 300}, {360, 260});
    resolution->setVisible((page == LibraryPage || page == EditorPage) && pages->width() >= 850);
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
void MainWindow::buildLibrary() {
    buildFolderTree();
    filterLibrary();
    updateThemeActions();
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
QString MainWindow::libraryKey(const QString &file) const {
    if (file.isEmpty())
        return "__starter__";
    return fileIdentity(file);
}
bool MainWindow::builtinTheme(const QString &file) const {
    return withinDirectory(file, runtime->library.root + "/official");
}
bool MainWindow::managedTheme(const QString &file) const {
    return runtime->library.contains(file) && !runtime->library.protectedFolder(file);
}
bool MainWindow::hiddenTheme(const QString &file) const {
    if (runtime->library.contains(file)) return false;
    const auto key = libraryKey(file);
    for (const auto &value : config["hidden_themes"].toArray())
        if (value.toString() == key)
            return true;
    return false;
}
void MainWindow::reloadLibrary() {
    entries.clear();
    QFile indexFile(dataDir + "/catalog-index.json");
    QJsonObject previous;
    if (indexFile.open(QIODevice::ReadOnly)) {
        const auto document = QJsonDocument::fromJson(indexFile.readAll()).object();
        if (document["version"].toInt() == 1) previous = document["files"].toObject();
    }
    QJsonObject index;
    QSet<QString> seen;
    const auto add = [&](const QString &file) {
        if (QFileInfo(file).fileName().startsWith('.')) return;
        const auto key = libraryKey(file);
        if (seen.contains(key) || hiddenTheme(file) || file.endsWith("AppConfig.data"))
            return;
        try {
            const QFileInfo info(file);
            auto cached = previous.value(file).toObject();
            const auto modified = info.lastModified().toMSecsSinceEpoch();
            if (cached["modified"].toDouble(-1) != modified || cached["bytes"].toDouble(-1) != info.size() ||
                cached["name"].toString().isEmpty() || cached["width"].toInt() <= 0 || cached["height"].toInt() <= 0) {
                const auto item = Theme::load(file);
                cached = {{"modified", double(modified)}, {"bytes", double(info.size())},
                          {"name", item.name()}, {"width", item.size().width()}, {"height", item.size().height()}};
            }
            index.insert(file, cached);
            if (file.endsWith(".data", Qt::CaseInsensitive)) cached["name"] = info.completeBaseName();
            entries.append({file, cached["name"].toString(), QSize(cached["width"].toInt(), cached["height"].toInt())});
            seen.insert(key);
        } catch (const std::exception &) {
        }
    };
    for (const auto &directory : {runtime->library.root}) {
        QDirIterator files(directory, {"*.data", "*.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext())
            add(files.next());
    }
    for (const auto &value : config["external_themes"].toArray())
        add(value.toString());
    if (index != previous) {
        QSaveFile output(dataDir + "/catalog-index.json");
        const auto bytes = QJsonDocument(QJsonObject{{"version", 1}, {"files", index}}).toJson(QJsonDocument::Compact);
        if (output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size()) output.commit();
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.name.localeAwareCompare(b.name) < 0; });
}
void MainWindow::includeTheme(const QString &file) {
    if (file.isEmpty())
        return;
    const auto key = libraryKey(file);
    QJsonArray hidden;
    for (const auto &value : config["hidden_themes"].toArray())
        if (value.toString() != key)
            hidden.append(value);
    config["hidden_themes"] = hidden;
    if (!runtime->library.contains(file)) {
        auto external = config["external_themes"].toArray();
        bool found = false;
        for (const auto &value : external)
            found |= libraryKey(value.toString()) == key;
        if (!found)
            external.append(QFileInfo(file).absoluteFilePath());
        config["external_themes"] = external;
    }
    auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return libraryKey(e.path) == key; });
    if (found == entries.end())
        entries.prepend({file, theme.name(), theme.size()});
    else {
        found->name = theme.name();
        found->size = theme.size();
    }
}
void MainWindow::updateThemeActions() {
    const bool available = !path.isEmpty() && !runtime->library.recycled(path);
    if (deleteThemeAction)
        deleteThemeAction->setEnabled(available);
    for (const auto name : {"renamePreviewTheme", "renameEditorTheme"})
        if (auto button = findChild<QToolButton *>(name)) button->setEnabled(path.isEmpty() || (managedTheme(path) && available));
    if (modeButton) modeButton->setEnabled(!runtime->library.recycled(path));
    if (saveButton) saveButton->setEnabled(!runtime->library.recycled(path));
}
void MainWindow::deleteTheme(const QString &file, bool permanent) {
    if (file.isEmpty() || runtime->library.protectedFolder(file)) return;
    QString name = QFileInfo(file).fileName();
    for (const auto &entry : entries) if (entry.path == file) name = entry.name;
    const auto affected = [selected = file](const QString &value) { return value == selected || withinDirectory(value, selected); };
    bool unsaved = affected(path) && dirty;
    for (const auto &draft : drafts) if (affected(draft.path)) unsaved |= draft.theme.data != draft.saved;
    auto effect = permanent ? L("文件将被永久移除，无法从回收站还原。", "Files will be permanently removed and cannot be restored from the recycle bin.")
        : managedTheme(file) ? L("文件将移入 themes/deleted 回收站，可以还原。", "Files will be moved to themes/deleted and can be restored.")
        : L("仅移除主题库中的引用，保留外部原始文件。", "Only the library reference will be removed; the external original is kept.");
    if (unsaved) effect += L("\n此主题还有未保存的修改，删除将一并丢弃。", "\nThis theme has unsaved changes, which will also be discarded.");
    QMessageBox confirmation(QMessageBox::Warning, permanent ? L("彻底删除", "Delete permanently") : L("删除", "Delete"),
        L("删除“%1”？\n\n", "Delete “%1”?\n\n").arg(name) + effect, QMessageBox::NoButton, this);
    confirmation.setObjectName("deleteThemeConfirmation");
    confirmation.addButton(permanent ? L("彻底删除", "Delete permanently") : L("删除", "Delete"), QMessageBox::DestructiveRole);
    const auto cancel = confirmation.addButton(L("取消", "Cancel"), QMessageBox::RejectRole);
    confirmation.setDefaultButton(cancel); confirmation.setEscapeButton(cancel); confirmation.exec();
    if (!confirmation.clickedButton() || confirmation.buttonRole(confirmation.clickedButton()) != QMessageBox::DestructiveRole) return;
    QString error;
    if (!removeTheme(file, error, permanent)) QMessageBox::warning(this, L("删除失败", "Delete failed"), error);
}
bool MainWindow::removeTheme(const QString &file, QString &error, bool permanent) {
    error.clear();
    const bool owned = managedTheme(file);
    if (file.isEmpty() || runtime->library.protectedFolder(file) || (permanent && !owned) || (!owned && !QFileInfo(file).isFile())) {
        error = L("请选择主题库中的主题或子文件夹。", "Choose a theme or subfolder in the library."); return false;
    }
    const auto affected = [selected = file](const QString &value) { return value == selected || withinDirectory(value, selected); };
    const bool active = affected(path);
    auto next = config;
    QJsonArray external;
    for (const auto &value : config["external_themes"].toArray()) if (!affected(value.toString())) external.append(value);
    next["external_themes"] = external;
    if (!owned) { auto hidden = next["hidden_themes"].toArray(); hidden.append(libraryKey(file)); next["hidden_themes"] = hidden; }
    auto directions = next["theme_orientations"].toObject();
    for (const auto &entry : entries) if (affected(entry.path)) directions.remove(orientationKey(entry.path));
    next["theme_orientations"] = directions;
    if (active) { next["selected_theme"] = QString(); next["orientation"] = directions.value("__starter__").toInt(2); }
    QString backup;
    try {
        if (owned && !permanent) backup = runtime->library.recycle(file);
        if (!storeConfig(dataDir, next, error)) {
            if (!backup.isEmpty()) runtime->library.restore(backup);
            return false;
        }
        if (owned && permanent) {
            try { runtime->library.purge(file); }
            catch (...) { QString ignored; storeConfig(dataDir, config, ignored); throw; }
        }
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
    config = next;
    if (active) { sending = false; choose({}); }
    for (const auto &key : drafts.keys()) if (affected(drafts[key].path)) drafts.remove(key);
    if (!libraryFolder.isEmpty() && !QFileInfo(libraryFolder).isDir()) libraryFolder = runtime->library.root;
    reloadLibrary(); buildLibrary(); persist();
    return true;
}
void MainWindow::restoreBuiltinThemes() {
    selectLibraryFolder(runtime->library.root + "/deleted");
}
void MainWindow::thumbnails() {
    if (!library || page != LibraryPage || !isVisible())
        return;
    int count = 0;
    std::unique_ptr<Renderer> thumbnailRenderer;
    const auto thumbnailIcon = [](const QPixmap &pixmap) {
        QIcon icon(pixmap);
        icon.addPixmap(pixmap, QIcon::Selected);
        icon.addPixmap(pixmap, QIcon::Active);
        return icon;
    };
    for (auto i : libraryItems) {
        if (count >= 10) break;
        if (i->isHidden() || !library->visualItemRect(i).intersects(library->viewport()->rect()) ||
            !i->icon(0).isNull())
            continue;
        try {
            auto file = i->data(0, Qt::UserRole).toString();
            const QFileInfo info(file);
            const auto key = file + ':' + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size());
            if (const auto cached = thumbnailCache.object(key)) {
                i->setIcon(0, thumbnailIcon(*cached));
                ++count;
                continue;
            }
            if (!thumbnailRenderer) thumbnailRenderer = std::make_unique<Renderer>();
            auto t = Theme::load(file);
            const auto pixmap = QPixmap::fromImage(
                thumbnailRenderer->render(t, sample).scaled(library->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            i->setIcon(0, thumbnailIcon(pixmap));
            thumbnailCache.insert(key, new QPixmap(pixmap), int(pixmap.width() * pixmap.height() * 4));
            ++count;
        } catch (const std::exception &) {
        }
    }
    if (count == 10)
        QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
void MainWindow::buildTree() {
    if (!tree)
        return;
    bool was = building;
    building = true;
    QSet<QString> expanded;
    QTreeWidgetItemIterator old(tree);
    while (*old) {
        if ((*old)->isExpanded())
            expanded.insert((*old)->data(0, Qt::UserRole).toString());
        ++old;
    }
    auto ids = canvas ? canvas->selected : QStringList();
    tree->clear();
    std::function<void(QJsonArray, QTreeWidgetItem *)> add = [&](QJsonArray a, QTreeWidgetItem *parent) {
        for (int n = int(a.size()) - 1; n >= 0; --n) {
            auto o = a[n].toObject();
            auto i = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
            i->setText(0, o["name"].toString(o["kind"].toString()));
            i->setData(0, Qt::UserRole, o["id"].toString());
            i->setData(0, Qt::UserRole + 1, o["kind"].toString());
            i->setToolTip(0, o["kind"].toString() + "\n" + (o["visible"].toBool(true)
                ? L("可见 · 点击眼睛隐藏", "Visible · Click the eye to hide")
                : L("隐藏 · 点击眼睛显示", "Hidden · Click the eye to show")));
            i->setFlags(i->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled |
                        Qt::ItemIsDropEnabled);
            i->setCheckState(0, o["visible"].toBool(true) ? Qt::Checked : Qt::Unchecked);
            i->setCheckState(1, o["locked"].toBool() ? Qt::Checked : Qt::Unchecked);
            i->setSelected(ids.contains(o["id"].toString()));
            if (o["kind"] == "group") {
                i->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
                add(o["children"].toArray(), i);
                i->setExpanded(expanded.isEmpty() || expanded.contains(o["id"].toString()));
            }
        }
    };
    add(theme.layers(), nullptr);
    building = was;
}
QStringList MainWindow::selection() const {
    return canvas ? canvas->selected : QStringList();
}
void MainWindow::selection(const QStringList &ids) {
    canvas->selected = ids;
    buildTree();
    properties();
    canvas->update();
}
void MainWindow::selectTree() {
    if (building)
        return;
    canvas->selected.clear();
    for (auto i : tree->selectedItems())
        canvas->selected.append(i->data(0, Qt::UserRole).toString());
    canvas->update();
    properties();
}
void MainWindow::properties() {
    updateLayerActions();
    if (!layerName)
        return;
    bool was = building;
    building = true;
    auto ids = selection();
    auto l = ids.size() == 1 ? findLayer(theme.layers(), ids.first()) : QJsonObject();
    bool has = !l.isEmpty(), locked = has && layerLocked(theme.layers(), ids.first());
    for (auto w : QList<QWidget *>{layerName, layerText, bindingBox, xBox, yBox, wBox, hBox, fontBox,
                                   visibleBox, lockedBox, fontColor, fillColor})
        w->setEnabled(has && (w == lockedBox || !locked));
    layerName->setText(l["name"].toString());
    layerText->setText(l["text"].toString());
    xBox->setValue(l["x"].toInt());
    yBox->setValue(l["y"].toInt());
    wBox->setValue(l["w"].toInt(100));
    hBox->setValue(l["h"].toInt(30));
    fontBox->setValue(l["font"].toObject()["size"].toInt(20));
    bindingBox->setCurrentIndex(qMax(0, bindingBox->findData(canonical(l["metric"].toString()))));
    visibleBox->setChecked(l.value("visible").toBool(true));
    lockedBox->setChecked(l["locked"].toBool());
    bool text = l["kind"] == "text" || l["kind"] == "sensor";
    layerText->setEnabled(has && !locked && l["kind"] == "text");
    fontBox->setEnabled(has && !locked && text);
    bindingBox->setEnabled(has && !locked && l["kind"] != "text" && l["kind"] != "group");
    wBox->setEnabled(has && !locked && l["kind"] != "group");
    hBox->setEnabled(wBox->isEnabled());
    fontColor->setEnabled(has && !locked && text);
    building = was;
}
void MainWindow::propertyEdited() {
    if (building || selection().size() != 1)
        return;
    auto id = selection().first();
    bool locked = layerLocked(theme.layers(), id);
    mutate([&](Theme &t) {
        auto a = t.layers();
        updateLayer(a, id, [&](QJsonObject &l) {
            if (locked) {
                l["locked"] = lockedBox->isChecked();
                return;
            }
            l["name"] = layerName->text().trimmed();
            l["text"] = layerText->text();
            l["metric"] = bindingBox->currentData().toString();
            l["x"] = xBox->value();
            l["y"] = yBox->value();
            if (l["kind"] != "group") {
                l["w"] = wBox->value();
                l["h"] = hBox->value();
            }
            l["visible"] = visibleBox->isChecked();
            l["locked"] = lockedBox->isChecked();
            auto f = l["font"].toObject();
            if (f["size"].toInt(20) != fontBox->value()) {
                f["size"] = fontBox->value();
                if (f["family"] != "Turing Screen Symbols" && f["family"] != "Turing Screen Latin" &&
                    f["family"] != "Turing Screen Mono")
                    f["family"] = "Noto Sans SC";
                f["height"] = 0;
                f["width"] = 0;
                f["spacing"] = 0;
            }
            l["font"] = f;
        });
        t.data["layers"] = a;
    });
}
void MainWindow::mutate(const std::function<void(Theme &)> &fn, bool rebuildLayers) {
    auto before = theme.data;
    try {
        fn(theme);
        theme.validate();
        remember(before, rebuildLayers);
    } catch (const std::exception &e) {
        theme.data = before;
        QMessageBox::warning(this, L("操作失败", "Operation failed"), QString::fromUtf8(e.what()));
    }
}
void MainWindow::remember(QJsonObject before, bool rebuildLayers) {
    if (before == theme.data)
        return;
    undoHistory.append(before);
    redoHistory.clear();
    while (undoHistory.size() > 30)
        undoHistory.removeFirst();
    qsizetype total = 0;
    for (auto i = undoHistory.crbegin(); i != undoHistory.crend(); ++i)
        total += QJsonDocument(*i).toJson(QJsonDocument::Compact).size();
    while (total > 8 * 1024 * 1024 && !undoHistory.isEmpty()) {
        total -= QJsonDocument(undoHistory.first()).toJson(QJsonDocument::Compact).size();
        undoHistory.removeFirst();
    }
    dirty = theme.data != saved;
    if (rebuildLayers) buildTree();
    properties();
    render();
}
void MainWindow::undo() {
    if (undoHistory.isEmpty())
        return;
    redoHistory.append(theme.data);
    theme.data = undoHistory.takeLast();
    selection({});
    dirty = theme.data != saved;
    render();
}
void MainWindow::redo() {
    if (redoHistory.isEmpty())
        return;
    undoHistory.append(theme.data);
    theme.data = redoHistory.takeLast();
    selection({});
    dirty = theme.data != saved;
    render();
}
void MainWindow::choose(const QString &requested) {
    const auto file = requested.isEmpty() ? runtime->library.defaultThemeFile() : runtime->library.resolve(requested, dataDir);
    if (file == path && !file.isEmpty())
        return;
    QString old = path.isEmpty() ? QString("__starter__") : path;
    if (theme.data != saved || !undoHistory.isEmpty())
        drafts.insert(old, Document{theme, path, saved, undoHistory, redoHistory});
    else
        drafts.remove(old);
    try {
        QString key = file.isEmpty() ? QString("__starter__") : file;
        if (drafts.contains(key)) {
            auto d = drafts.take(key);
            theme = d.theme;
            path = d.path;
            saved = d.saved;
            undoHistory = d.undo;
            redoHistory = d.redo;
        } else {
            theme = Theme::load(file);
            if (file.endsWith(".data", Qt::CaseInsensitive)) theme.data["name"] = QFileInfo(file).completeBaseName();
            path = file;
            saved = theme.data;
            undoHistory.clear();
            redoHistory.clear();
        }
        dirty = theme.data != saved;
        canvas->theme = &theme;
        selection({});
        config["selected_theme"] = path;
        restoreThemeOrientation();
        configureWeather();
        includeTheme(file);
        updateThemeActions();
        {
            const QSignalBlocker blocker(library);
            for (auto item : libraryItems)
                if (libraryKey(item->data(0, Qt::UserRole).toString()) == libraryKey(path)) {
                    library->setCurrentItem(item);
                    for (auto parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
                    library->scrollToItem(item);
                    break;
                }
        }
        persist();
        render();
    } catch (const std::exception &e) {
        QMessageBox::warning(this, L("导入失败", "Import failed"), QString::fromUtf8(e.what()));
    }
}
void MainWindow::newTheme() {
    auto file = runtime->library.unique(targetLibraryFolder(), L("新主题", "New theme") + ".json");
    Theme t;
    t.data = {{"format", "turing-screen-qt"},
              {"version", 1},
              {"name", L("新主题", "New theme")},
              {"landscape", true},
              {"layers", QJsonArray()}};
    t.save(file);
    entries.prepend({file, t.name(), t.size()});
    choose(file);
    buildLibrary();
    setEditing(true);
}
void MainWindow::importTheme() {
    auto file =
        QFileDialog::getOpenFileName(this, L("导入主题", "Import theme"), {}, L("主题文件 (*.data *.json)", "Theme files (*.data *.json)"));
    if (!file.isEmpty()) {
        try {
            auto imported = Theme::load(file);
            auto target = runtime->library.unique(targetLibraryFolder(), QFileInfo(file).completeBaseName() + ".json");
            imported.save(target);
            choose(target); reloadLibrary(); buildLibrary();
        } catch (const std::exception &e) { QMessageBox::warning(this, L("导入失败", "Import failed"), QString::fromUtf8(e.what())); }
    }
}
void MainWindow::saveTheme(bool as) {
    auto target = path;
    if (target.isEmpty() || !managedTheme(target) || builtinTheme(target) || target.endsWith(".data"))
        target = runtime->library.unique(runtime->library.root + "/custom", (ThemeLibrary::validName(theme.name()) ? theme.name() : uid()) + ".json");
    if (as) target = QFileDialog::getSaveFileName(this, L("保存主题", "Save theme"),
        targetLibraryFolder() + '/' + theme.name() + ".json", "Theme JSON (*.json)");
    if (target.isEmpty())
        return;
    try {
        theme.save(target);
        path = target;
        saved = theme.data;
        dirty = false;
        config["selected_theme"] = path;
        includeTheme(path);
        persist();
        buildLibrary();
        render();
    } catch (const std::exception &e) {
        QMessageBox::warning(this, L("保存失败", "Save failed"), QString::fromUtf8(e.what()));
    }
}
void MainWindow::exportImage() {
    auto target = QFileDialog::getSaveFileName(this, L("导出图片", "Export image"), theme.name() + ".png",
                                               "PNG (*.png)");
    if (!target.isEmpty())
        frame.save(target);
}
static QString importImage(const QString &file) {
    QImageReader r(file);
    auto size = r.size();
    if (size.width() > 1536 || size.height() > 1536)
        throw std::runtime_error("Please resize the image to 1536 x 1536 or smaller");
    auto img = r.read();
    if (img.isNull())
        throw std::runtime_error("Cannot read image");
    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    return QString::fromLatin1(data.toBase64());
}
void MainWindow::backgroundImage() {
    auto file = QFileDialog::getOpenFileName(this, L("背景图", "Background"), {},
                                             "Images (*.png *.jpg *.jpeg *.bmp)");
    if (file.isEmpty())
        return;
    try {
        auto encoded = importImage(file);
        mutate([&](Theme &t) { t.data["background"] = encoded; });
    } catch (const std::exception &e) {
        QMessageBox::warning(this, L("图像错误", "Image error"), QString::fromUtf8(e.what()));
    }
}
void MainWindow::createLayer(const QString &kind, QPointF point, QSizeF size) {
    setEditing(true);
    QString metric = kind == "text" ? QString() : QString("CpuLoad");
    QString name = kind;
    if (kind == "text") {
        bool ok = false;
        name = QInputDialog::getText(this, L("新建文字图层", "New text layer"), L("文字内容", "Text"),
                                     QLineEdit::Normal, L("新文字", "New text"), &ok);
        if (!ok)
            return;
    }
    auto l = newLayer(kind, name, metric);
    l["x"] = point.x();
    l["y"] = point.y();
    if (kind == "rectangle" || kind == "ellipse" || kind == "line") {
        l["w"] = size.width();
        l["h"] = size.height();
    }
    if (kind == "image") {
        auto file = QFileDialog::getOpenFileName(this, L("选择图片", "Choose image"), {},
                                                 "Images (*.png *.jpg *.jpeg *.bmp)");
        if (file.isEmpty())
            return;
        try {
            l["data"] = importImage(file);
            QImageReader reader(file);
            auto fit = reader.size().scaled(theme.size(), Qt::KeepAspectRatio);
            l["w"] = fit.width();
            l["h"] = fit.height();
            l["name"] = QFileInfo(file).baseName();
        } catch (const std::exception &e) {
            QMessageBox::warning(this, L("图像错误", "Image error"), QString::fromUtf8(e.what()));
            return;
        }
    }
    QString parent;
    if (selection().size() == 1) {
        auto id = selection().first();
        auto p = findLayer(theme.layers(), id);
        if (p["kind"] == "group" && !layerLocked(theme.layers(), id))
            parent = id;
    }
    if (!parent.isEmpty()) {
        auto at = absolutePosition(theme.layers(), parent);
        l["x"] = l["x"].toDouble() - at.x();
        l["y"] = l["y"].toDouble() - at.y();
    }
    auto id = l["id"].toString();
    mutate([&](Theme &t) {
        auto a = t.layers();
        insertLayers(a, parent, QJsonArray{l});
        t.data["layers"] = a;
    });
    selection({id});
}
void MainWindow::addComponent(const QString &kind) {
    setEditing(true);
    auto c = component(kind);
    auto id = c["id"].toString();
    mutate([&](Theme &t) {
        auto a = t.layers();
        a.append(c);
        t.data["layers"] = a;
    });
    selection({id});
}
void MainWindow::rename() {
    auto ids = selection();
    if (ids.size() != 1 || layerLocked(theme.layers(), ids.first()))
        return;
    bool ok = false;
    auto name =
        QInputDialog::getText(this, L("重命名图层", "Rename layer"), L("名称", "Name"), QLineEdit::Normal,
                              findLayer(theme.layers(), ids.first())["name"].toString(), &ok)
            .trimmed();
    if (ok && !name.isEmpty())
        mutate([&](Theme &t) {
            auto a = t.layers();
            updateLayer(a, ids.first(), [&](QJsonObject &o) { o["name"] = name; });
            t.data["layers"] = a;
        });
}
void MainWindow::reparent(const QStringList &raw, const QString &parent, int index) {
    auto ids = normalizedSelection(theme.layers(), raw);
    if (ids.isEmpty() || (!parent.isEmpty() && layerLocked(theme.layers(), parent)))
        return;
    for (auto id : ids)
        if (layerLocked(theme.layers(), id) || parent == id ||
            !findLayer(QJsonArray{findLayer(theme.layers(), id)}, parent).isEmpty())
            return;
    mutate([&](Theme &t) {
        auto a = t.layers();
        QHash<QString, QPointF> locations;
        for (auto id : ids)
            locations[id] = absolutePosition(a, id);
        auto target = parent.isEmpty() ? QPointF() : absolutePosition(a, parent);
        auto moved = takeLayers(a, ids);
        for (int i = 0; i < moved.size(); ++i) {
            auto o = moved[i].toObject();
            auto at = locations.value(o["id"].toString()) - target;
            o["x"] = at.x();
            o["y"] = at.y();
            moved[i] = o;
        }
        insertLayers(a, parent, moved, index);
        t.data["layers"] = a;
    });
    selection(ids);
}
void MainWindow::layerAction(const QString &action) {
    auto ids = normalizedSelection(theme.layers(), selection());
    if (action == "copy" || action == "cut" || action == "duplicate") {
        clipboard = {};
        for (auto id : ids) {
            auto l = findLayer(theme.layers(), id);
            auto p = absolutePosition(theme.layers(), id);
            l["x"] = p.x();
            l["y"] = p.y();
            clipboard.append(l);
        }
        updateLayerActions();
        if (action == "copy")
            return;
    }
    if (action == "paste" || action == "duplicate") {
        QStringList added;
        mutate([&](Theme &t) {
            auto a = t.layers();
            for (auto v : clipboard) {
                auto o = v.toObject();
                regenerateIds(o);
                o["x"] = o["x"].toDouble() + 8;
                o["y"] = o["y"].toDouble() + 8;
                o["name"] = o["name"].toString() + L(" 副本", " copy");
                added.append(o["id"].toString());
                a.append(o);
            }
            t.data["layers"] = a;
        });
        selection(added);
        return;
    }
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const QString &id) { return layerLocked(theme.layers(), id); }),
              ids.end());
    if (ids.isEmpty())
        return;
    if (action == "delete" || action == "cut") {
        mutate([&](Theme &t) {
            auto a = t.layers();
            takeLayers(a, ids);
            t.data["layers"] = a;
        });
        selection({});
    } else if (action == "group") {
        QString name = L("分组", "Group");
        auto group = newLayer("group", name);
        auto groupId = group["id"].toString();
        mutate([&](Theme &t) {
            auto a = t.layers();
            QHash<QString, QPointF> coords;
            for (auto id : ids)
                coords[id] = absolutePosition(a, id);
            auto children = takeLayers(a, ids);
            for (int i = 0; i < children.size(); ++i) {
                auto o = children[i].toObject();
                auto at = coords[o["id"].toString()];
                o["x"] = at.x();
                o["y"] = at.y();
                children[i] = o;
            }
            group["x"] = 0;
            group["y"] = 0;
            group["children"] = children;
            a.append(group);
            t.data["layers"] = a;
        });
        selection({groupId});
    } else if (action == "ungroup") {
        mutate([&](Theme &t) {
            auto a = t.layers();
            for (auto id : ids) {
                auto l = findLayer(a, id);
                if (l["kind"] != "group")
                    continue;
                auto parent = parentOf(a, id);
                auto siblings = parent.isEmpty() ? a : findLayer(a, parent)["children"].toArray();
                int index = 0;
                while (index < siblings.size() && siblings[index].toObject()["id"] != id)
                    ++index;
                auto children = l["children"].toArray();
                if (!l["background"].toString().isEmpty()) {
                    auto bg = newLayer("image", L("分组背景", "Group background"));
                    auto pixels = renderer.decode(l["background"].toString());
                    bg["x"] = 0;
                    bg["y"] = 0;
                    bg["w"] = pixels.width();
                    bg["h"] = pixels.height();
                    bg["data"] = l["background"];
                    children.prepend(bg);
                }
                QJsonArray flattened;
                for (auto v : children) {
                    auto o = v.toObject();
                    o["x"] = o["x"].toDouble() + l["x"].toDouble();
                    o["y"] = o["y"].toDouble() + l["y"].toDouble();
                    o["visible"] = o["visible"].toBool(true) && l["visible"].toBool(true);
                    flattened.append(o);
                }
                takeLayers(a, {id});
                insertLayers(a, parent, flattened, index);
            }
            t.data["layers"] = a;
        });
        selection({});
    } else if (action == "front" || action == "back") {
        for (auto id : ids)
            reparent({id}, parentOf(theme.layers(), id), action == "front" ? -1 : 0);
    }
}
void MainWindow::populateLayerMenu(QMenu *menu) {
    for (auto pair : QList<QPair<QString, QString>>{{"text", L("文字", "Text")},
            {"sensor", L("传感器", "Sensor")}, {"image", L("图片", "Image")},
            {"rectangle", L("矩形", "Rectangle")}, {"ellipse", L("椭圆", "Ellipse")},
            {"line", L("直线", "Line")}, {"bar", L("状态条", "Progress bar")},
            {"arc", L("弧形条", "Arc")}, {"group", L("分组", "Group")}}) {
        auto action = menu->addAction(pair.second, this, [this, pair] { createLayer(pair.first); });
        action->setData(pair.first);
    }
}
void MainWindow::updateLayerActions() {
    const auto ids = normalizedSelection(theme.layers(), selection());
    bool editable = false, group = false;
    for (const auto &id : ids) {
        if (layerLocked(theme.layers(), id)) continue;
        editable = true;
        group |= findLayer(theme.layers(), id)["kind"] == "group";
    }
    for (auto i = layerActions.cbegin(); i != layerActions.cend(); ++i) {
        bool enabled = editable;
        if (i.key() == "paste") enabled = !clipboard.isEmpty();
        else if (i.key() == "copy" || i.key() == "duplicate") enabled = !ids.isEmpty();
        else if (i.key() == "properties") enabled = ids.size() == 1;
        else if (i.key() == "rename") enabled = ids.size() == 1 && editable;
        else if (i.key() == "ungroup") enabled = group;
        i.value()->setEnabled(enabled);
    }
}
void MainWindow::contextMenu(QPoint global) {
    updateLayerActions();
    QMenu m(this);
    m.setObjectName("layerContextMenu");
    const auto add = [&](const char *key) { m.addAction(layerActions.value(key)); };
    add("properties"); add("rename");
    m.addSeparator();
    add("duplicate"); add("delete");
    m.addSeparator();
    add("copy"); add("paste");
    m.addSeparator();
    add("group"); add("ungroup");
    auto arrange = m.addMenu(L("排列", "Arrange"));
    arrange->setObjectName("layerArrangeMenu");
    arrange->addAction(layerActions.value("front"));
    arrange->addAction(layerActions.value("back"));
    arrange->setEnabled(layerActions.value("front")->isEnabled());
    m.addSeparator();
    populateLayerMenu(m.addMenu(L("新建图层", "New layer")));
    m.exec(global);
}
void MainWindow::render() {
    runtime->render();
}
void MainWindow::tick() {
    runtime->tick();
}
void MainWindow::refreshRuntimeUi() {
    updateWeatherStatus();
    for (auto toggle : findChildren<QCheckBox *>()) {
        if (!toggle->property("collectionToggle").toBool()) continue;
        const QSignalBlocker blocker(toggle);
        toggle->setChecked(runtime->collectionEnabled());
    }
    if (!frame.isNull()) {
        canvas->mirrored = config["mirror"].toBool();
        canvas->setFrame(frame);
        previewCanvas->mirrored = canvas->mirrored;
        previewCanvas->setFrame(frame);
    }
    title->setText(theme.name() + (dirty ? " *" : ""));
    title->setToolTip(title->text());
    editorThemeTitle->setText(title->text());
    editorThemeTitle->setToolTip(title->text());
    resolution->setText(QString("%1 × %2 · %3").arg(theme.size().width()).arg(theme.size().height())
                           .arg(theme.size().width() > 320 ? L("横屏", "Landscape") : L("竖屏", "Portrait")));
    undoAction->setEnabled(!undoHistory.isEmpty());
    redoAction->setEnabled(!redoHistory.isEmpty());
    updateStatus();
    if (!runtime->error.isEmpty()) status->setText(runtime->error);
    if (page == SensorsPage && isVisible()) {
        const auto &m = metrics();
        sensorTable->setRowCount(int(m.size()));
        for (int n = 0; n < m.size(); ++n) {
            const auto item = [&](int column) {
                auto result = sensorTable->item(n, column);
                if (!result) { result = new QTableWidgetItem; sensorTable->setItem(n, column, result); }
                return result;
            };
            item(0)->setText(english ? m[n].first : m[n].second);
            auto value = item(1);
            value->setText(sample.text(m[n].first));
            value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            auto source = sample.sources.value(m[n].first);
            if (source.isEmpty()) {
                const auto reason = sample.unavailable.value(m[n].first);
                if (reason.contains("permission"))
                    source = L("接口不可读（权限或驱动限制）", "Counter not readable (permission or driver restriction)");
                else if (reason.startsWith("Waiting for"))
                    source = L("等待第二次采样", "Waiting for a second sample");
                else if (reason.startsWith("Audio service"))
                    source = L("音频服务连接中，或无默认输出", "Audio service connecting or no default output");
                else if (reason.startsWith("Weather disabled"))
                    source = L("天气未启用，可在设置中开启", "Weather disabled; enable in Settings");
                else if (reason == "Weather loading")
                    source = L("正在获取天气", "Loading weather");
                else if (reason.startsWith("Driver does"))
                    source = L("驱动未提供此传感器", "Driver does not expose this sensor");
                else
                    source = reason;
            }
            auto sourceItem = item(2);
            sourceItem->setText(source);
            sourceItem->setToolTip(sample.unavailable.value(m[n].first, source));
        }
    }
}
void MainWindow::refreshPorts() {
    if (!portBox)
        return;
    auto previous = portBox->currentText();
    portBox->clear();
    for (auto p : Device::ports())
        portBox->addItem(p.name, p.name);
    if (portBox->count() == 0)
        portBox->addItem(L("没有可用串口", "No serial ports"), QString());
    int n = portBox->findData(config["port"].toString());
    if (n >= 0)
        portBox->setCurrentIndex(n);
    else if (!previous.isEmpty())
        portBox->setCurrentText(previous);
}
void MainWindow::connectDevice() {
    auto s = device.status();
    if (s.connected || s.connecting) {
        sending = false;
        device.disconnectPort();
    } else {
        auto port = portBox->currentData().toString();
        if (port.isEmpty())
            return;
        config["port"] = port;
        device.connectPort(port, directionBox->currentIndex(), brightness->value(), mirrorBox->isChecked());
        persist();
    }
    updateStatus();
    QTimer::singleShot(300, this, [this] { updateStatus(); });
}
void MainWindow::toggleSend() {
    if (!device.status().connected) {
        connectDevice();
        return;
    }
    sending = !sending;
    render();
}
void MainWindow::updateStatus() {
    if (!connection)
        return;
    auto s = device.status();
    connectButton->setText(s.connected    ? L("断开设备", "Disconnect")
                           : s.connecting ? L("正在连接…", "Connecting…")
                                          : L("连接设备", "Connect device"));
    sendButton->setText(sending ? L("暂停发送", "Pause sending") : L("应用到副屏", "Send to display"));
    sendButton->setEnabled(s.connected);
    directionBox->setEnabled(true);
    for (auto widget : {static_cast<QWidget *>(brightness), static_cast<QWidget *>(mirrorBox)}) {
        widget->setEnabled(s.connected);
        widget->setToolTip(s.connected ? QString()
                                       : L("连接设备后可调整", "Available after connecting a device"));
    }
    for (auto w : findChildren<QToolButton *>())
        if (w->property("deviceOperation").toBool())
            w->setEnabled(s.connected);
    const bool collecting = sampling.isActive();
    static_cast<StatusLabel *>(dataState)->setState(collecting ? L("本机数据采集中", "Local collection active")
        : L("本机采集已暂停", "Local collection paused"), collecting ? "success" : "muted");
    static_cast<StatusLabel *>(connection)->setState(s.connected ? L("设备已连接", "Device connected")
        : s.connecting ? L("设备连接中", "Connecting device") : L("设备未连接", "Device disconnected"),
        s.connected ? "success" : s.connecting ? "warning" : "muted");
    static_cast<StatusLabel *>(sendState)->setState(sending && s.connected ? L("画面发送中", "Sending frames")
        : L("画面发送已暂停", "Sending paused"), sending && s.connected ? "success" : "muted");
    status->setText(s.message);
}
void MainWindow::persist() {
    runtime->persist();
}
QString MainWindow::orientationKey() const {
    return orientationKey(path);
}
QString MainWindow::orientationKey(const QString &file) const {
    return themeOrientationKey(file);
}
void MainWindow::restoreThemeOrientation(bool migrateLegacy) {
    auto preference = config["theme_orientations"].toObject().value(orientationKey());
    const auto valid = [](QJsonValue v) {
        return v.isDouble() && v.toInt(-1) >= 0 && v.toInt(-1) <= 3 && v.toDouble() == v.toInt();
    };
    if (!valid(preference) && migrateLegacy && !theme.data.contains("orientation"))
        preference = config["orientation"];
    applyThemeOrientation(valid(preference) ? preference.toInt() : theme.orientation());
}
void MainWindow::applyThemeOrientation(int direction) {
    if (direction < 0 || direction > 3)
        return;
    // Orientation is an automatically persisted theme setting. Keep document
    // history consistent so undoing an unrelated layer edit cannot revert it.
    theme.data["orientation"] = direction;
    saved["orientation"] = direction;
    for (auto &state : undoHistory)
        state["orientation"] = direction;
    for (auto &state : redoHistory)
        state["orientation"] = direction;
    config["orientation"] = direction;
    auto directions = config["theme_orientations"].toObject();
    directions[orientationKey()] = direction;
    config["theme_orientations"] = directions;
    if (directionBox) {
        QSignalBlocker blocker(directionBox);
        directionBox->setCurrentIndex(direction);
    }
    if (device.status().connected)
        device.command(4, direction);
}
void MainWindow::quit() {
    if (!confirmDiscard())
        return;
    quitting = true;
    persist();
    qApp->quit();
}
bool MainWindow::confirmDiscard() {
    bool unsaved = dirty;
    for (auto i = drafts.cbegin(); i != drafts.cend(); ++i)
        unsaved = unsaved || i->theme.data != i->saved;
    return !unsaved || QMessageBox::question(this, L("未保存的主题", "Unsaved themes"),
                                             L("退出将丢弃尚未保存的修改，是否继续？",
                                               "Exit and discard all unsaved changes?")) == QMessageBox::Yes;
}
void MainWindow::closeEvent(QCloseEvent *e) {
    if (!quitting && (config["minimize_to_tray"].toBool() || config["auto_lightweight"].toBool()) &&
        trayAvailable()) {
        hide();
        emit closedToTray();
        e->ignore();
        return;
    }
    if (!quitting && !confirmDiscard()) {
        e->ignore();
        return;
    }
    persist();
    quitting = true;
    e->accept();
    qApp->quit();
}
void MainWindow::recordResource() {
    auto row = processStats().json();
    row["seconds"] = resourceClock.elapsed() / 1000.;
    row["stage"] = workloadStage;
    resourceSamples.append(row);
}
void MainWindow::finishResources(const QString &dir, double seconds) {
    quint64 peak = 0, privateBytes = 0;
    double cpuStart = -1, cpuEnd = 0, timeStart = 0, timeEnd = 0;
    for (auto v : resourceSamples) {
        auto o = v.toObject();
        peak = qMax(peak, quint64(o["peak_resident_bytes"].toDouble()));
        peak = qMax(peak, quint64(o["resident_bytes"].toDouble()));
        privateBytes = qMax(privateBytes, quint64(o["private_bytes"].toDouble()));
        if (o["seconds"].toDouble() >= 5 && o["stage"] == "idle") {
            if (cpuStart < 0) {
                cpuStart = o["cpu_seconds"].toDouble();
                timeStart = o["seconds"].toDouble();
            }
            cpuEnd = o["cpu_seconds"].toDouble();
            timeEnd = o["seconds"].toDouble();
        }
    }
    double core = timeEnd > timeStart ? 100 * (cpuEnd - cpuStart) / (timeEnd - timeStart) : 0;
    QJsonObject report{{"seconds", seconds},
                       {"peak_resident_bytes", double(peak)},
                       {"peak_private_bytes", double(privateBytes)},
                       {"idle_cpu_percent_one_core", core},
                       {"idle_cpu_percent_total", core / qMax(1, QThread::idealThreadCount())},
                       {"logical_processors", QThread::idealThreadCount()},
                       {"memory_limit_bytes", 100000000},
                       {"passed", peak < 100000000},
                       {"theme_switches", workflowSwitches},
                       {"edits", workflowEdits},
                       {"samples", resourceSamples}};
    QFile f(dir + "/resources.json");
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(report).toJson());
}
void MainWindow::runSmoke(const QString &dir, bool stress, int seconds) {
    QDir().mkpath(dir);
    resourceClock.restart();
    resourceSamples = {};
    auto timer = new QTimer(this);
    timer->setInterval(stress ? 250 : 500);
    auto step = std::make_shared<int>(0);
    connect(timer, &QTimer::timeout, this, [this, dir, stress, seconds, step, timer] {
        recordResource();
        int n = (*step)++;
        double elapsed = resourceClock.elapsed() / 1000.;
        if (stress) {
            if (elapsed < 8)
                workloadStage = "idle";
            else if (elapsed < 26 && !entries.isEmpty()) {
                workloadStage = "themes";
                choose(entries[n % entries.size()].path);
                setEditing(n % 2);
                ++workflowSwitches;
            } else if (elapsed < 44) {
                if (workloadStage != "editing") {
                    theme = Theme::starter();
                    canvas->theme = &theme;
                    setEditing(true);
                    workloadStage = "editing";
                }
                auto before = theme.data;
                auto a = theme.layers();
                auto l = newLayer("rectangle", "Test layer");
                l["x"] = 20 + n % 400;
                l["y"] = 20 + n % 220;
                a.append(l);
                theme.data["layers"] = a;
                remember(before);
                ++workflowEdits;
            } else if (elapsed < 49) {
                workloadStage = "history";
                if (n % 2)
                    undo();
                else
                    redo();
            } else if (elapsed < 54) {
                workloadStage = "pages";
                setPage(n % PageCount);
            } else if (elapsed < 58) {
                if (workloadStage != "tray") {
                    workloadStage = "tray";
                    if (config["minimize_to_tray"].toBool() && QSystemTrayIcon::isSystemTrayAvailable())
                        close();
                    else
                        hide();
                }
            } else {
                if (!isVisible())
                    show();
                setPage(LibraryPage);
                workloadStage = "steady";
            }
        } else {
            static const char *names[]{"library", "editor", "sensors", "settings",
                                       "compact-library", "compact-editor", "compact-properties", "english",
                                       "expanded-library", "expanded-editor", "settings-auto-lightweight"};
            if (n < 11) {
                if (n < PageCount)
                    setPage(n);
                if (n == 4) {
                    resize(760, 680);
                    setPage(LibraryPage);
                }
                if (n == 5)
                    setPage(EditorPage);
                if (n == 6) {
                    inspectorTabs->setCurrentIndex(1);
                    if (!theme.layers().isEmpty())
                        selection({theme.layers().first().toObject()["id"].toString()});
                }
                if (n == 7) {
                    resize(1280, 820);
                    english = true;
                    build();
                    render();
                }
                if (n == 8) {
                    english = false;
                    build();
                    setSidebarExpanded(true);
                    setPage(LibraryPage);
                }
                if (n == 9)
                    setPage(EditorPage);
                if (n == 10) {
                    setPage(SettingsPage);
                    findChild<QCheckBox *>("autoLightweight")->setChecked(true);
                    findChild<QSpinBox *>("lightweightDelay")->setValue(0);
                }
                timer->stop();
                QTimer::singleShot(100, this, [this, dir, n, timer] {
                    grab().save(dir + "/" + names[n] + ".png");
                    QJsonArray positions;
                    for (int i = 0; i < qMin(6, libraryItems.size()); ++i) {
                        const auto rect = library->visualItemRect(libraryItems.at(i));
                        positions.append(QJsonObject{{"x", rect.x()}, {"y", rect.y()},
                                                     {"width", rect.width()}, {"height", rect.height()}});
                    }
                    QFile layoutFile(dir + "/" + names[n] + "-layout.json");
                    if (layoutFile.open(QIODevice::WriteOnly))
                        layoutFile.write(QJsonDocument(QJsonObject{
                            {"viewport_width", library->viewport()->width()},
                            {"columns", library->columnCount()}, {"wrapping", false},
                            {"items", positions}}).toJson());
                    timer->start();
                });
            }
        }
        if (elapsed >= seconds) {
            grab().save(dir + "/final.png");
            finishResources(dir, elapsed);
            QFile marker(dir + "/smoke-ok.txt");
            if (marker.open(QIODevice::WriteOnly))
                marker.write("Turing Screen workload completed");
            dirty = false;
            timer->stop();
            quit();
        }
    });
    timer->start();
}
} // namespace Screen
