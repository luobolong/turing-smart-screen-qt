#include "mainwindow.h"
#include <QBuffer>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStorageInfo>
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
class PaneSplitterHandle : public QSplitterHandle {
    QFrame *separator;
    void updateSeparator() {
        separator->setFrameShape(orientation() == Qt::Horizontal ? QFrame::VLine : QFrame::HLine);
        separator->setGeometry(rect());
    }
public:
    PaneSplitterHandle(Qt::Orientation orientation, QSplitter *parent)
        : QSplitterHandle(orientation, parent), separator(new QFrame(this)) {
        separator->setObjectName("paneSeparator");
        separator->setFrameShadow(QFrame::Plain);
        separator->setLineWidth(1);
        separator->setAttribute(Qt::WA_TransparentForMouseEvents);
        updateSeparator();
    }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QSplitterHandle::resizeEvent(event);
        updateSeparator();
    }
};
class PaneSplitter : public QSplitter {
public:
    explicit PaneSplitter(QWidget *parent = nullptr) : QSplitter(parent) { setHandleWidth(7); }
protected:
    QSplitterHandle *createHandle() override { return new PaneSplitterHandle(orientation(), this); }
};
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
    connect(startup.get(), &StartupController::changed, this, &MainWindow::updateStartupUi);
    saved = theme.data;
    reloadLibrary();
    if (!path.isEmpty()) includeTheme(path);
    resourceTrace("catalog loaded");
    renderTimer.setSingleShot(true);
    connect(&renderTimer, &QTimer::timeout, this, &MainWindow::render);
    connect(runtime.get(), &DisplayRuntime::updated, this, &MainWindow::refreshRuntimeUi);
    build();
    resourceTrace("UI built");
    render();
    resourceTrace("rendered");
    setMinimumSize(640, 520);
    resize(1280, 820);
    resourceClock.start();
}
MainWindow::~MainWindow() {
    runtime->preview = false;
    persist();
}
void MainWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    runtime->preview = page == LibraryPage || page == EditorPage;
    updateResponsiveLayout();
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
    emit windowOpened();
}
void MainWindow::hideEvent(QHideEvent *event) {
    QMainWindow::hideEvent(event);
    runtime->preview = false;
}
void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    if ((event->oldSize().width() < 900) != (width() < 900))
        setSidebarExpanded(width() >= 900 && config.value("sidebar_expanded").toBool(false));
    updateResponsiveLayout();
}
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == pages || (library && watched == library->viewport())) && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &MainWindow::updateResponsiveLayout);
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::build() {
    building = true;
    const auto filter = search ? search->text() : QString();
    const auto selected = selection();
    const int inspectorPage = inspectorTabs ? inspectorTabs->currentIndex() : 0;
    auto old = takeCentralWidget();
    if (old)
        old->deleteLater();
    root = new QWidget;
    root->setObjectName("root");
    auto windowLayout = new QVBoxLayout(root);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);
    titleBar = new QWidget;
    titleBar->setObjectName("titleBar");
    auto header = new QHBoxLayout(titleBar);
    header->setContentsMargins(12, 8, 16, 8);
    header->setSpacing(12);
    sidebarToggle = new QToolButton;
    sidebarToggle->setObjectName("sidebarToggle");
    sidebarToggle->setAutoRaise(true);
    sidebarToggle->setMinimumSize(32, 32);
    header->addWidget(sidebarToggle);
    pageTitle = label({}, header);
    pageTitle->setObjectName("pageTitle");
    title = label({}, header);
    title->setMinimumWidth(0);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    header->setStretch(header->count() - 1, 1);
    resolution = label({}, header);
    modeButton = button({}, header, [this] { setEditing(page != EditorPage); });
    modeButton->setObjectName("editThemeButton");
    saveButton = button(L("保存主题", "Save theme"), header, [this] { saveTheme(); });
    saveButton->setObjectName("saveThemeButton");
    windowLayout->addWidget(titleBar);
    auto divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Plain);
    divider->setLineWidth(1);
    windowLayout->addWidget(divider);
    connect(sidebarToggle, &QToolButton::clicked, this, [this] {
        setSidebarExpanded(!sidebarExpanded);
        config["sidebar_expanded"] = sidebarExpanded;
        persist();
        updateResponsiveLayout();
    });
    auto body = new QWidget;
    auto shell = new QHBoxLayout(body);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    windowLayout->addWidget(body, 1);
    auto sidebarFrame = new QFrame;
    sidebarFrame->setFrameShape(QFrame::NoFrame);
    sidebar = sidebarFrame;
    sidebar->setObjectName("sidebar");
    auto sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(8, 12, 8, 12);
    const auto sideAction = [&](const QString &text, QStyle::StandardPixmap icon, const char *name,
                                const std::function<void()> &action) {
        auto tool = new QToolButton;
        tool->setObjectName(name);
        tool->setText(text);
        tool->setToolTip(text);
        tool->setAccessibleName(text);
        tool->setIcon(style()->standardIcon(icon));
        tool->setAutoRaise(true);
        tool->setMinimumHeight(40);
        tool->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sideLayout->addWidget(tool);
        connect(tool, &QToolButton::clicked, this, action);
    };
    navigation = new QListWidget;
    navigation->setObjectName("navigation");
    navigation->setAccessibleName(L("功能导航", "Navigation"));
    navigation->setFrameShape(QFrame::NoFrame);
    navigation->setBackgroundRole(QPalette::Window);
    navigation->viewport()->setBackgroundRole(QPalette::Window);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setIconSize(QSize(22, 22));
    navigation->setSpacing(3);
    const QStringList names{L("主题预览", "Theme preview"), L("主题编辑器", "Theme editor"),
                            L("传感器", "Sensors"), L("设置", "Settings"),
                            L("诊断", "Diagnostics")};
    const QStyle::StandardPixmap icons[]{QStyle::SP_DirIcon, QStyle::SP_FileDialogContentsView,
        QStyle::SP_DriveHDIcon, QStyle::SP_FileDialogDetailedView,
        QStyle::SP_MessageBoxInformation};
    for (int n = 0; n < PageCount; ++n) {
        auto item = new QListWidgetItem(style()->standardIcon(icons[n]), names[n], navigation);
        item->setData(Qt::UserRole, names[n]);
        item->setData(Qt::AccessibleTextRole, names[n]);
        item->setToolTip(names[n]);
        item->setSizeHint(QSize(40, 44));
    }
    sideLayout->addWidget(navigation, 1);
    sideAction(L("轻量模式", "Lightweight mode"), QStyle::SP_TitleBarMinButton, "sidebarLightweight",
               [this] { emit lightweightRequested(); });
    shell->addWidget(sidebar);
    auto content = new QWidget;
    content->setObjectName("mainContent");
    auto outer = new QVBoxLayout(content);
    outer->setContentsMargins(20, 16, 20, 12);
    outer->setSpacing(12);
    pages = new QStackedWidget;
    pages->setObjectName("pages");
    pages->installEventFilter(this);
    outer->addWidget(pages, 1);
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
    auto diag = new QWidget;
    pages->addWidget(diag);
    buildDiagnostics(diag);
    shell->addWidget(content, 1);
    setCentralWidget(root);
    statusBar()->clearMessage();
    for (auto l : findChildren<QWidget *>("statusPart")) {
        statusBar()->removeWidget(l);
        l->deleteLater();
    }
    dataState = new QLabel;
    dataState->setObjectName("statusPart");
    connection = new QLabel;
    connection->setObjectName("statusPart");
    sendState = new QLabel;
    sendState->setObjectName("statusPart");
    status = new QLabel;
    status->setObjectName("statusPart");
    for (auto part : {dataState, status, connection, sendState}) {
        part->setMinimumWidth(0);
        part->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    }
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusBar()->addWidget(dataState);
    statusBar()->addWidget(status, 1);
    statusBar()->addPermanentWidget(connection);
    statusBar()->addPermanentWidget(sendState);
    auto quitButton = new QPushButton(L("退出", "Exit"));
    quitButton->setObjectName("statusPart");
    statusBar()->addPermanentWidget(quitButton);
    connect(quitButton, &QPushButton::clicked, this, &MainWindow::quit);
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
        if (editing && page == EditorPage)
            rename();
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
            if (page == EditorPage && editing && !qobject_cast<QLineEdit *>(QApplication::focusWidget()))
                layerAction(pair.second);
        });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] {
        if (page == EditorPage)
            createLayer("text");
    });
    buildLibrary();
    buildTree();
    search->setText(filter);
    selection(selected);
    inspectorTabs->setCurrentIndex(inspectorPage);
    emit preferencesChanged();
    building = false;
    setSidebarExpanded(width() >= 900 && config.value("sidebar_expanded").toBool(false));
    setPage(page);
    updateResponsiveLayout();
    updateStatus();
    properties();
}
void MainWindow::buildLibraryPage(QWidget *w) {
    auto layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto bar = new QHBoxLayout;
    search = new QLineEdit;
    search->setObjectName("themeSearch");
    search->setClearButtonEnabled(true);
    search->setPlaceholderText(L("搜索主题", "Search themes"));
    search->setMinimumWidth(100);
    bar->addWidget(search, 1);
    button(L("新建", "New"), bar, [this] { newTheme(); });
    button(L("导入", "Import"), bar, [this] { importTheme(); });
    deleteThemeButton = button(L("删除", "Delete"), bar, [this] { deleteTheme(path); });
    deleteThemeButton->setObjectName("deleteThemeButton");
    layout->addLayout(bar);
    libraryEmpty = new QLabel(L("没有找到匹配的主题，请尝试其他关键词。", "No matching themes. Try another search."));
    libraryEmpty->setWordWrap(true);
    libraryEmpty->hide();
    layout->addWidget(libraryEmpty);
    librarySplit = new PaneSplitter;
    librarySplit->setObjectName("librarySplit");
    librarySplit->setChildrenCollapsible(false);
    library = new QListWidget;
    library->setObjectName("themeLibrary");
    library->viewport()->installEventFilter(this);
    library->setViewMode(QListView::ListMode);
    library->setFlow(QListView::TopToBottom);
    library->setResizeMode(QListView::Adjust);
    library->setMovement(QListView::Static);
    library->setWrapping(false);
    library->setWordWrap(true);
    library->setIconSize(QSize(88, 60));
    library->setSpacing(4);
    library->setUniformItemSizes(true);
    library->setFrameShape(QFrame::NoFrame);
    library->setMinimumSize(200, 80);
    library->setToolTip(L("选择主题可预览；双击主题进入编辑器。", "Select a theme to preview. Double-click to edit."));
    library->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    library->setContextMenuPolicy(Qt::CustomContextMenu);
    librarySplit->addWidget(library);
    auto preview = new QWidget;
    auto previewLayout = new QVBoxLayout(preview);
    previewLayout->setContentsMargins(12, 0, 0, 0);
    label(L("当前主题", "Current theme"), previewLayout);
    previewCanvas = new Canvas;
    previewCanvas->setObjectName("previewCanvas");
    previewCanvas->theme = &theme;
    previewCanvas->setMinimumSize(200, 160);
    previewLayout->addWidget(previewCanvas, 1);
    preview->setMinimumWidth(220);
    librarySplit->addWidget(preview);
    devicePanel = new QWidget;
    devicePanel->setObjectName("devicePanel");
    devicePanel->setMinimumSize(240, 80);
    buildDevicePanel(devicePanel);
    librarySplit->addWidget(devicePanel);
    librarySplit->setStretchFactor(0, 0);
    librarySplit->setStretchFactor(1, 1);
    librarySplit->setStretchFactor(2, 0);
    librarySplit->setSizes({250, 650, 280});
    layout->addWidget(librarySplit, 1);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &s) {
        int matches = 0;
        for (int n = 0; n < library->count(); ++n)
            if (auto item = library->item(n)) {
                const bool match = item->text().contains(s, Qt::CaseInsensitive);
                item->setHidden(!match);
                matches += match;
            }
        libraryEmpty->setVisible(matches == 0);
        thumbnails();
    });
    connect(library, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item && !building)
            choose(item->data(Qt::UserRole).toString());
    });
    connect(library->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { thumbnails(); });
    connect(library, &QListWidget::customContextMenuRequested, this, [this](QPoint position) {
        QMenu menu(library);
        if (const auto item = library->itemAt(position)) {
            const auto file = item->data(Qt::UserRole).toString();
            auto remove = menu.addAction(L("删除主题", "Delete theme"), this, [this, file] { deleteTheme(file); });
            remove->setEnabled(!file.isEmpty());
            menu.addSeparator();
        }
        menu.addAction(L("恢复内置主题", "Restore built-in themes"), this, &MainWindow::restoreBuiltinThemes);
        menu.exec(library->viewport()->mapToGlobal(position));
    });
    connect(library, &QListWidget::itemDoubleClicked, this, [this] { setEditing(true); });
}
void MainWindow::buildEditorPage(QWidget *w) {
    auto layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto tools = new QToolBar;
    tools->setObjectName("editorTools");
    tools->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    tools->setIconSize(QSize(18, 18));
    tools->setMinimumWidth(0);
    auto toolBox = new QComboBox;
    for (auto p : QList<QPair<QString, QString>>{{"move", L("移动", "Move")},
                                                 {"text", L("文字", "Text")},
                                                 {"rectangle", L("矩形", "Rectangle")},
                                                 {"ellipse", L("椭圆", "Ellipse")},
                                                 {"line", L("直线", "Line")}})
        toolBox->addItem(p.second, p.first);
    tools->addWidget(toolBox);
    undoAction = tools->addAction(style()->standardIcon(QStyle::SP_ArrowBack), L("撤销", "Undo"), this, &MainWindow::undo);
    redoAction = tools->addAction(style()->standardIcon(QStyle::SP_ArrowForward), L("重做", "Redo"), this, &MainWindow::redo);
    tools->addSeparator();
    tools->addAction(L("适应窗口", "Fit"), this, [this] { canvas->setFit(true); });
    tools->addAction("1:1", this, [this] { canvas->setZoom(1); });
    tools->addAction("−", this, [this] { canvas->setZoom(canvas->zoom() / 1.2); });
    tools->addAction("+", this, [this] { canvas->setZoom(canvas->zoom() * 1.2); });
    tools->addSeparator();
    auto themeOptions = new QToolButton;
    themeOptions->setText(L("主题选项", "Theme options"));
    themeOptions->setPopupMode(QToolButton::InstantPopup);
    tools->addWidget(themeOptions);
    auto themeMenu = new QMenu(themeOptions);
    themeOptions->setMenu(themeMenu);
    themeMenu->addAction(L("重命名主题", "Rename theme"), this, [this] {
        bool ok = false;
        auto name = QInputDialog::getText(this, L("重命名主题", "Rename theme"), L("名称", "Name"),
                                          QLineEdit::Normal, theme.name(), &ok)
                        .trimmed();
        if (ok && !name.isEmpty())
            mutate([&](Theme &t) { t.data["name"] = name; });
    });
    themeMenu->addAction(L("横屏主题", "Landscape theme"), this,
                         [this] { mutate([](Theme &t) { t.data["landscape"] = true; }); });
    themeMenu->addAction(L("竖屏主题", "Portrait theme"), this,
                         [this] { mutate([](Theme &t) { t.data["landscape"] = false; }); });
    themeMenu->addAction(L("切换文字背景", "Toggle text background"), this, [this] {
        mutate([](Theme &t) { t.data["use_text_background"] = !t.data["use_text_background"].toBool(); });
    });
    themeMenu->addAction(L("另存为", "Save as"), this, [this] { saveTheme(true); });
    themeMenu->addSeparator();
    deleteThemeAction = themeMenu->addAction(L("删除主题", "Delete theme"), this, [this] { deleteTheme(path); });
    themeMenu->addAction(L("恢复内置主题", "Restore built-in themes"), this, &MainWindow::restoreBuiltinThemes);
    layout->addWidget(tools);
    editorSplit = new PaneSplitter;
    editorSplit->setObjectName("editorSplit");
    editorSplit->setChildrenCollapsible(false);
    layout->addWidget(editorSplit, 1);
    auto center = new QWidget;
    auto cv = new QVBoxLayout(center);
    cv->setContentsMargins(0, 0, 0, 0);
    auto layers = new QWidget;
    auto llayers = new QVBoxLayout(layers);
    llayers->setContentsMargins(8, 8, 8, 8);
    auto creation = new QHBoxLayout;
    auto add = button(L("＋ 图层", "+ Layer"), creation, [] {});
    auto menu = new QMenu(add);
    for (auto pair : QList<QPair<QString, QString>>{{"text", L("文字", "Text")},
                                                    {"sensor", L("传感器", "Sensor")},
                                                    {"image", L("图片", "Image")},
                                                    {"rectangle", L("矩形", "Rectangle")},
                                                    {"ellipse", L("椭圆", "Ellipse")},
                                                    {"line", L("直线", "Line")},
                                                    {"bar", L("状态条", "Progress bar")},
                                                    {"arc", L("弧形条", "Arc")},
                                                    {"group", L("分组", "Group")}})
        menu->addAction(pair.second, this, [this, pair] { createLayer(pair.first); });
    add->setMenu(menu);
    auto components = button(L("组件", "Components"), creation, [] {});
    auto cm = new QMenu(components);
    for (auto kind : {"CPU", "GPU", "RAM", "NET", "CLOCK", "DISK"})
        cm->addAction(QString::fromLatin1(kind), this,
                      [this, kind] { addComponent(QString::fromLatin1(kind)); });
    components->setMenu(cm);
    llayers->addLayout(creation);
    auto layerButtons = new QHBoxLayout;
    button(L("重命名", "Rename"), layerButtons, [this] { rename(); });
    button(L("编组", "Group"), layerButtons, [this] { layerAction("group"); });
    button(L("解组", "Ungroup"), layerButtons, [this] { layerAction("ungroup"); });
    llayers->addLayout(layerButtons);
    auto copyButtons = new QHBoxLayout;
    button(L("复制", "Copy"), copyButtons, [this] { layerAction("copy"); });
    button(L("粘贴", "Paste"), copyButtons, [this] { layerAction("paste"); });
    button(L("删除", "Delete"), copyButtons, [this] { layerAction("delete"); });
    llayers->addLayout(copyButtons);
    auto tr = new LayerTree;
    tree = tr;
    tr->dropped = [this](QStringList ids, QString parent, int index) { reparent(ids, parent, index); };
    tree->setHeaderLabels({L("图层 / 双击重命名", "Layers / double-click to rename"), L("锁定", "Lock")});
    tree->setColumnWidth(0, 190);
    tree->setColumnWidth(1, 44);
    tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree->setIndentation(20);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    llayers->addWidget(tree, 1);
    connect(tree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::selectTree);
    connect(tree, &QTreeWidget::customContextMenuRequested, this,
            [this](QPoint p) { contextMenu(tree->viewport()->mapToGlobal(p)); });
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
        });
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
    connect(toolBox, &QComboBox::currentIndexChanged, this, [this, toolBox] {
        canvas->tool = toolBox->currentData().toString();
        setEditing(true);
    });
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
        if (!canvas->selected.isEmpty())
            inspectorTabs->setCurrentIndex(1);
    });
    connect(canvas, &Canvas::transientEdited, this, [this] {
        if (!renderTimer.isActive())
            renderTimer.start(33);
    });
    connect(canvas, &Canvas::committed, this, &MainWindow::remember);
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
    label(L("图层属性", "Layer properties"), rv);
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
}
void MainWindow::buildDevicePanel(QWidget *w) {
    auto layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto scroll = new QScrollArea;
    scroll->setMinimumSize(220, 70);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    auto content = new QWidget;
    auto rv = new QVBoxLayout(content);
    rv->setContentsMargins(12, 0, 12, 12);
    label(L("设备", "Device"), rv);
    auto hint = new QLabel(L("连接 USB 副屏后，可应用当前主题并调整显示参数。", "Connect a USB display to send the current theme and adjust display settings."));
    hint->setWordWrap(true);
    rv->addWidget(hint);
    portBox = new QComboBox;
    rv->addWidget(portBox);
    auto connectionButtons = new QHBoxLayout;
    connectButton = button({}, connectionButtons, [this] { connectDevice(); });
    button(L("刷新", "Refresh"), connectionButtons, [this] { refreshPorts(); });
    rv->addLayout(connectionButtons);
    auto form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    directionBox = new QComboBox;
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
    advanced->setText(L("高级设备操作", "Advanced device operations"));
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
    sendButton = button({}, rv, [this] { toggleSend(); });
    sendButton->setObjectName("sendButton");
    rv->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll);
    refreshPorts();
}
void MainWindow::buildSensors(QWidget *w) {
    auto l = new QVBoxLayout(w);
    label(L("本机传感器", "Local sensors"), l);
    label(L("默认每秒采集并更新副屏。内存已用、可用、总量以 GB 显示；无法读取的项目会在来源栏说明原因。",
            "Samples and updates the display every second by default. Used, available and total RAM are shown in GB. "
            "Unavailable sensors show a reason in the source column."),
          l)->setWordWrap(true);
    sensorTable = new QTableWidget(0, 3);
    sensorTable->setHorizontalHeaderLabels({L("项目", "Metric"), L("数值", "Value"), L("来源 / 状态", "Source / status")});
    sensorTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    sensorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    l->addWidget(sensorTable, 1);
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
    label(L("设置", "Settings"), outer);
    auto group = new QGroupBox(L("运行设置", "Behavior"));
    auto form = new QFormLayout(group);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
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
    disk->addItem(L("系统分区", "System drive"), QString());
    for (auto d : QStorageInfo::mountedVolumes())
        if (d.isReady() && !d.isReadOnly())
            disk->addItem(d.rootPath(), d.rootPath());
    disk->setCurrentIndex(qMax(0, disk->findData(config["disk"].toString())));
    form->addRow(L("硬盘分区", "Disk partition"), disk);
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
    auto weatherCredit = new QLabel(L("天气：<a href=\"https://open-meteo.com/\">Open-Meteo</a>（CC BY 4.0）。留空城市时使用 ipwho.is。",
                                     "Weather: <a href=\"https://open-meteo.com/\">Open-Meteo</a> (CC BY 4.0). Empty city uses ipwho.is."));
    weatherCredit->setOpenExternalLinks(true);
    weatherCredit->setWordWrap(true);
    form->addRow(weatherCredit);
    outer->addWidget(group);
    outer->addStretch();
    auto buttons = new QHBoxLayout;
    button(L("打开数据目录", "Open data folder"), buttons,
           [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dataDir)); });
    button(L("导出诊断", "Export diagnostics"), buttons, [this] {
        auto file = QFileDialog::getSaveFileName(this, L("导出诊断", "Export diagnostics"),
                                                 dataDir + "/diagnostics.json", "JSON (*.json)");
        if (!file.isEmpty()) {
            QSaveFile f(file);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(diagnostics()).toJson());
                f.commit();
            }
        }
    });
    outer->addLayout(buttons);
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
            ? L("已启用，下次登录后直接进入轻量模式，点击托盘图标打开窗口。",
                "Enabled. Starts in lightweight mode at your next sign-in. Click the tray icon to open the window.")
            : L("已关闭，不影响当前运行和副屏发送。", "Disabled. The current session and display updates continue."));
}
void MainWindow::configureWeather() {
    const auto options = theme.data.value("weather").toObject();
    const bool enabled = config["weather_enabled"].toBool(options["enabled"].toBool());
    const auto city = config["weather_city"].toString(options["city"].toString());
    collector.setWeather(enabled, city);
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
}
void MainWindow::buildDiagnostics(QWidget *w) {
    auto l = new QVBoxLayout(w);
    label(L("诊断", "Diagnostics"), l);
    diagnosticText = new QPlainTextEdit;
    diagnosticText->setReadOnly(true);
    l->addWidget(diagnosticText, 1);
    auto row = new QHBoxLayout;
    button(L("刷新诊断", "Refresh"), row, [this] {
        diagnosticText->setPlainText(QString::fromUtf8(QJsonDocument(diagnostics()).toJson()));
    });
    button(L("复制诊断", "Copy"), row,
           [this] { qApp->clipboard()->setText(QString::fromUtf8(QJsonDocument(diagnostics()).toJson())); });
    l->addLayout(row);
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
    pageTitle->setText(navigation->item(n)->data(Qt::UserRole).toString());
    setWindowTitle(pageTitle->text() + L(" · 图灵智显", " · Turing Screen"));
    title->setVisible(themePage);
    resolution->setVisible(themePage && pages->width() >= 850);
    modeButton->setVisible(themePage);
    modeButton->setText(n == EditorPage ? L("返回预览", "Back to preview") : L("编辑主题", "Edit theme"));
    saveButton->setVisible(n == EditorPage);
    if (building)
        return;
    if (n == SettingsPage) startup->refresh();
    if (n == SensorsPage)
        tick();
    if (n == DiagnosticsPage)
        diagnosticText->setPlainText(QString::fromUtf8(QJsonDocument(diagnostics()).toJson()));
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
    sidebarToggle->setArrowType(expanded ? Qt::LeftArrow : Qt::RightArrow);
    const auto description = expanded ? L("收起功能栏", "Collapse sidebar") : L("展开功能栏", "Expand sidebar");
    sidebarToggle->setToolTip(description);
    sidebarToggle->setAccessibleName(description);
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
    const auto reflow = [](QSplitter *splitter, bool vertical, const QList<int> &wideSizes,
                           const QList<int> &narrowSizes) {
        const auto orientation = vertical ? Qt::Vertical : Qt::Horizontal;
        if (splitter->orientation() != orientation) {
            splitter->setOrientation(orientation);
            splitter->setSizes(vertical ? narrowSizes : wideSizes);
        }
    };
    const bool compactPreview = pages->width() < 900;
    library->setMaximumWidth(compactPreview ? QWIDGETSIZE_MAX : 320);
    devicePanel->setMaximumWidth(compactPreview ? QWIDGETSIZE_MAX : 340);
    const auto mode = compactPreview ? QListView::IconMode : QListView::ListMode;
    const auto flow = compactPreview ? QListView::LeftToRight : QListView::TopToBottom;
    const auto grid = compactPreview
        ? QSize(qMax(1, (library->viewport()->width() - 4 * library->spacing()) / 2),
                qMax(104, library->iconSize().height() + 2 * library->fontMetrics().height() + 12))
        : QSize();
    const bool relayout = library->viewMode() != mode || library->flow() != flow || library->isWrapping() != compactPreview ||
                          library->gridSize() != grid;
    if (library->viewMode() != mode) library->setViewMode(mode);
    library->setMovement(QListView::Static);
    if (library->flow() != flow) library->setFlow(flow);
    if (library->isWrapping() != compactPreview) library->setWrapping(compactPreview);
    if (library->gridSize() != grid) library->setGridSize(grid);
    if (relayout)
        for (int i = 0; i < library->count(); ++i)
            library->item(i)->setSizeHint(compactPreview ? grid : QSize(220, 80));
    reflow(librarySplit, compactPreview, {250, 650, 280}, {180, 260, 180});
    reflow(editorSplit, pages->width() < 820, {720, 300}, {360, 260});
    if (relayout && library->isVisible()) library->doItemsLayout();
    resolution->setVisible((page == LibraryPage || page == EditorPage) && pages->width() >= 850);
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
void MainWindow::buildLibrary() {
    QSignalBlocker b(library);
    library->clear();
    auto add = [&](QString path, QString name, QSize size) {
        auto i = new QListWidgetItem(
            QString("%1\n%2 × %3 · %4")
                .arg(name)
                .arg(size.width())
                .arg(size.height())
                .arg(size.width() > size.height() ? L("横屏", "Landscape") : L("竖屏", "Portrait")),
            library);
        i->setData(Qt::UserRole, path);
        i->setSizeHint(library->viewMode() == QListView::IconMode ? library->gridSize() : QSize(220, 80));
        i->setToolTip(i->text());
        i->setHidden(search && !i->text().contains(search->text(), Qt::CaseInsensitive));
        if (path == this->path)
            library->setCurrentItem(i);
    };
    add({}, L("系统概览", "System overview"), QSize(480, 320));
    for (auto e : entries)
        add(e.path, e.name, e.size);
    bool matches = false;
    for (int n = 0; n < library->count(); ++n)
        matches |= !library->item(n)->isHidden();
    libraryEmpty->setVisible(!matches);
    updateThemeActions();
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
QString MainWindow::libraryKey(const QString &file) const {
    if (file.isEmpty())
        return "__starter__";
    if (builtinTheme(file))
        return "builtin:" + QDir(fileIdentity(assets())).relativeFilePath(fileIdentity(file));
    return fileIdentity(file);
}
bool MainWindow::builtinTheme(const QString &file) const {
    return withinDirectory(file, assets());
}
bool MainWindow::managedTheme(const QString &file) const {
    return !QFileInfo(file).isSymLink() && !builtinTheme(file) && withinDirectory(file, dataDir + "/themes");
}
bool MainWindow::hiddenTheme(const QString &file) const {
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
            entries.append({file, cached["name"].toString(), QSize(cached["width"].toInt(), cached["height"].toInt())});
            seen.insert(key);
        } catch (const std::exception &) {
        }
    };
    for (const auto &directory : {assets() + "/themes", assets() + "/official/config", dataDir + "/themes"}) {
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
    if (!builtinTheme(file) && !managedTheme(file)) {
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
    const bool available = !path.isEmpty();
    if (deleteThemeButton) {
        deleteThemeButton->setEnabled(available);
        deleteThemeButton->setToolTip(available ? L("删除当前主题", "Delete current theme")
                                                : L("系统概览保留为默认主题", "System overview is the default theme"));
    }
    if (deleteThemeAction)
        deleteThemeAction->setEnabled(available);
}
void MainWindow::deleteTheme(const QString &file) {
    if (file.isEmpty())
        return;
    const auto key = libraryKey(file);
    QString name = QFileInfo(file).completeBaseName();
    bool unsaved = libraryKey(path) == key && dirty;
    for (const auto &entry : entries)
        if (libraryKey(entry.path) == key)
            name = entry.name;
    for (auto i = drafts.cbegin(); i != drafts.cend(); ++i)
        if (libraryKey(i->path) == key)
            unsaved |= i->theme.data != i->saved;
    auto effect = builtinTheme(file)
        ? L("此内置主题将从主题库隐藏，可使用“恢复内置主题”重新显示。",
            "This built-in theme will be hidden. Use Restore built-in themes to show it again.")
        : managedTheme(file)
        ? L("主题文件将移至数据目录的 deleted-themes 文件夹，可从该目录找回。",
            "The theme file will be moved to deleted-themes in the data folder for recovery.")
        : L("此主题将从主题库移除，原始文件保留在原位置。",
            "This theme will be removed from the library; the original file is kept in its location.");
    if (unsaved)
        effect += L("\n此主题还有未保存的修改，删除将一并丢弃。",
                    "\nThis theme has unsaved changes, which will also be discarded.");
    QMessageBox confirmation(QMessageBox::Warning, L("删除主题", "Delete theme"),
                             L("删除主题“%1”？\n\n", "Delete theme “%1”?\n\n").arg(name) + effect,
                             QMessageBox::NoButton, this);
    confirmation.setObjectName("deleteThemeConfirmation");
    confirmation.addButton(L("删除", "Delete"), QMessageBox::DestructiveRole);
    const auto cancel = confirmation.addButton(L("取消", "Cancel"), QMessageBox::RejectRole);
    confirmation.setDefaultButton(cancel);
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (!confirmation.clickedButton() || confirmation.buttonRole(confirmation.clickedButton()) != QMessageBox::DestructiveRole)
        return;
    QString error;
    if (!removeTheme(file, error))
        QMessageBox::warning(this, L("删除失败", "Delete failed"), error);
}
bool MainWindow::removeTheme(const QString &file, QString &error) {
    error.clear();
    if (file.isEmpty()) {
        error = L("系统概览保留为默认主题。", "System overview is the default theme.");
        return false;
    }
    const auto key = libraryKey(file);
    const bool active = libraryKey(path) == key;
    QStringList files, draftKeys, directionKeys;
    for (const auto &entry : entries)
        if (libraryKey(entry.path) == key)
            files.append(entry.path);
    if (active)
        files.append(path);
    if (files.isEmpty()) {
        error = L("主题不在主题库中。", "The theme is not in the library.");
        return false;
    }
    for (const auto &related : files)
        directionKeys.append(orientationKey(related));
    for (auto i = drafts.cbegin(); i != drafts.cend(); ++i)
        if (libraryKey(i->path) == key)
            draftKeys.append(i.key());
    const bool personal = managedTheme(file);
    QJsonArray external;
    for (const auto &value : config["external_themes"].toArray())
        if (libraryKey(value.toString()) != key)
            external.append(value);
    QString backup;
    if (personal && QFileInfo::exists(file)) {
        const auto directory = dataDir + "/deleted-themes";
        if (!QDir().mkpath(directory)) {
            error = L("无法创建已删除主题目录。", "Cannot create the deleted-themes folder.");
            return false;
        }
        const auto info = QFileInfo(file);
        backup = directory + '/' + uid() + '-' + info.completeBaseName().left(40) + '.' + info.suffix();
        QFile source(file);
        if (!source.rename(backup)) {
            error = L("无法移动主题文件：", "Cannot move the theme file: ") + source.errorString();
            return false;
        }
    }
    auto nextConfig = config;
    if (!personal) {
        auto hidden = nextConfig["hidden_themes"].toArray();
        if (!hidden.contains(key))
            hidden.append(key);
        nextConfig["hidden_themes"] = hidden;
    }
    nextConfig["external_themes"] = external;
    auto directions = nextConfig["theme_orientations"].toObject();
    for (const auto &direction : directionKeys)
        directions.remove(direction);
    nextConfig["theme_orientations"] = directions;
    if (active) {
        nextConfig["selected_theme"] = QString();
        nextConfig["orientation"] = qBound(0, directions.value("__starter__").toInt(2), 3);
    }
    if (!storeConfig(dataDir, nextConfig, error)) {
        error = L("无法保存主题库设置：", "Cannot save the theme library settings: ") + error;
        if (!backup.isEmpty() && !QFile::rename(backup, file))
            error += L("\n主题副本保留于：", "\nA copy of the theme is kept at: ") + backup;
        return false;
    }
    config = nextConfig;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry &e) { return files.contains(e.path); }), entries.end());
    if (active) {
        sending = false;
        choose({});
    }
    // Switching away stores a draft, so remove the deleted document afterwards.
    for (const auto &related : files)
        drafts.remove(related);
    for (const auto &draft : draftKeys)
        drafts.remove(draft);
    buildLibrary();
    return true;
}
void MainWindow::restoreBuiltinThemes() {
    QJsonArray hidden;
    for (const auto &value : config["hidden_themes"].toArray())
        if (!value.toString().startsWith("builtin:"))
            hidden.append(value);
    auto restored = config;
    restored["hidden_themes"] = hidden;
    QString error;
    if (!storeConfig(dataDir, restored, error)) {
        QMessageBox::warning(this, L("恢复失败", "Restore failed"), error);
        return;
    }
    config = restored;
    reloadLibrary();
    buildLibrary();
}
void MainWindow::thumbnails() {
    if (!library || page != LibraryPage || !isVisible())
        return;
    int count = 0;
    std::unique_ptr<Renderer> thumbnailRenderer;
    for (int n = 0; n < library->count() && count < 10; ++n) {
        auto i = library->item(n);
        if (i->isHidden() || !library->visualItemRect(i).intersects(library->viewport()->rect()) ||
            !i->icon().isNull())
            continue;
        try {
            auto file = i->data(Qt::UserRole).toString();
            const QFileInfo info(file);
            const auto key = file + ':' + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size());
            if (const auto cached = thumbnailCache.object(key)) {
                i->setIcon(*cached);
                ++count;
                continue;
            }
            if (!thumbnailRenderer) thumbnailRenderer = std::make_unique<Renderer>();
            auto t = file.isEmpty() ? Theme::starter() : Theme::load(file);
            const auto pixmap = QPixmap::fromImage(
                thumbnailRenderer->render(t, sample).scaled(library->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            i->setIcon(pixmap);
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
            i->setToolTip(0, o["kind"].toString());
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
void MainWindow::mutate(const std::function<void(Theme &)> &fn) {
    auto before = theme.data;
    try {
        fn(theme);
        theme.validate();
        remember(before);
    } catch (const std::exception &e) {
        theme.data = before;
        QMessageBox::warning(this, L("操作失败", "Operation failed"), QString::fromUtf8(e.what()));
    }
}
void MainWindow::remember(QJsonObject before) {
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
    buildTree();
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
void MainWindow::choose(const QString &file) {
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
            theme = file.isEmpty() ? Theme::starter() : Theme::load(file);
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
            for (int n = 0; n < library->count(); ++n)
                if (libraryKey(library->item(n)->data(Qt::UserRole).toString()) == libraryKey(path)) {
                    library->setCurrentItem(library->item(n));
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
    auto file = dataDir + "/themes/" + uid() + ".json";
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
        QFileDialog::getOpenFileName(this, L("导入主题", "Import theme"), {}, "Themes (*.data *.json)");
    if (!file.isEmpty()) {
        choose(file);
        buildLibrary();
    }
}
void MainWindow::saveTheme(bool as) {
    auto target = path;
    const bool builtin = QFileInfo(target).absoluteFilePath().startsWith(
        QDir(assets() + "/themes").absolutePath() + '/');
    if (as || builtin || target.isEmpty() || !theme.data.contains("format") || target.endsWith(".data"))
        target =
            QFileDialog::getSaveFileName(this, L("保存主题", "Save theme"),
                                         dataDir + "/themes/" + theme.name() + ".json", "Theme JSON (*.json)");
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
void MainWindow::contextMenu(QPoint global) {
    QMenu m;
    auto create = m.addMenu(L("新建图层", "New layer"));
    for (auto k : {"text", "sensor", "image", "rectangle", "ellipse", "line", "bar", "arc", "group"})
        create->addAction(QString::fromLatin1(k), this, [this, k] { createLayer(QString::fromLatin1(k)); });
    m.addAction(L("重命名 F2", "Rename F2"), this, &MainWindow::rename);
    for (auto p : QList<QPair<QString, QString>>{{"copy", L("复制", "Copy")},
                                                 {"paste", L("粘贴", "Paste")},
                                                 {"duplicate", L("复制图层", "Duplicate")},
                                                 {"group", L("编组", "Group")},
                                                 {"ungroup", L("取消编组", "Ungroup")},
                                                 {"front", L("置顶", "Bring to front")},
                                                 {"back", L("置底", "Send to back")},
                                                 {"delete", L("删除", "Delete")}})
        m.addAction(p.second, this, [this, p] { layerAction(p.first); });
    m.exec(global);
}
void MainWindow::render() {
    runtime->render();
}
void MainWindow::tick() {
    runtime->tick();
}
void MainWindow::refreshRuntimeUi() {
    if (!frame.isNull()) {
        canvas->mirrored = config["mirror"].toBool();
        canvas->setFrame(frame);
        previewCanvas->mirrored = canvas->mirrored;
        previewCanvas->setFrame(frame);
    }
    title->setText(theme.name() + (dirty ? " *" : ""));
    title->setToolTip(title->text());
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
    dataState->setText("● " + L("本机数据采集中", "Local collection active"));
    connection->setText(
        "● " + (s.connected ? L("设备已连接", "Device connected") : L("设备未连接", "Device disconnected")));
    sendState->setText(sending && s.connected ? L("画面发送中", "Sending frames")
                                              : L("画面发送已暂停", "Sending paused"));
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
QJsonObject MainWindow::diagnostics() const {
    auto &d = const_cast<Device &>(device);
    auto s = d.status();
    return {{"application", "Turing Screen"},
            {"version", SCREEN_VERSION},
            {"ui_runtime_version", qVersion()},
            {"platform", QSysInfo::prettyProductName()},
            {"configuration", config},
            {"snapshot", sample.json()},
            {"process", processStats().json()},
            {"device", QJsonObject{{"connected", s.connected},
                                   {"message", s.message},
                                   {"frames", double(s.frames)},
                                   {"bytes", double(s.bytes)}}}};
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
            static const char *names[]{"library", "editor", "sensors", "settings", "diagnostics",
                                       "compact-library", "compact-editor", "compact-properties", "english",
                                       "expanded-library", "expanded-editor", "settings-auto-lightweight"};
            if (n < 12) {
                if (n < PageCount)
                    setPage(n);
                if (n == 5) {
                    resize(760, 680);
                    setPage(LibraryPage);
                }
                if (n == 6)
                    setPage(EditorPage);
                if (n == 7) {
                    inspectorTabs->setCurrentIndex(1);
                    if (!theme.layers().isEmpty())
                        selection({theme.layers().first().toObject()["id"].toString()});
                }
                if (n == 8) {
                    resize(1280, 820);
                    english = true;
                    build();
                    render();
                }
                if (n == 9) {
                    english = false;
                    build();
                    setSidebarExpanded(true);
                    setPage(LibraryPage);
                }
                if (n == 10)
                    setPage(EditorPage);
                if (n == 11) {
                    setPage(SettingsPage);
                    findChild<QCheckBox *>("autoLightweight")->setChecked(true);
                    findChild<QSpinBox *>("lightweightDelay")->setValue(0);
                }
                timer->stop();
                QTimer::singleShot(100, this, [this, dir, n, timer] {
                    grab().save(dir + "/" + names[n] + ".png");
                    QJsonArray positions;
                    for (int i = 0; i < qMin(6, library->count()); ++i) {
                        const auto rect = library->visualItemRect(library->item(i));
                        positions.append(QJsonObject{{"x", rect.x()}, {"y", rect.y()},
                                                     {"width", rect.width()}, {"height", rect.height()}});
                    }
                    QFile layoutFile(dir + "/" + names[n] + "-layout.json");
                    if (layoutFile.open(QIODevice::WriteOnly))
                        layoutFile.write(QJsonDocument(QJsonObject{
                            {"viewport_width", library->viewport()->width()},
                            {"grid_width", library->gridSize().width()},
                            {"flow", int(library->flow())}, {"wrapping", library->isWrapping()},
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
