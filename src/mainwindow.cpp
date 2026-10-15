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
static QLabel *label(const QString &text, QLayout *layout, const char *object = {}) {
    auto l = new QLabel(text);
    if (object)
        l->setObjectName(object);
    layout->addWidget(l);
    return l;
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
MainWindow::MainWindow(QString dir, QString initial, QWidget *parent)
    : QMainWindow(parent), dataDir(std::move(dir)) {
    resourceTrace("constructor body");
    QDir().mkpath(dataDir + "/themes");
    QFile f(dataDir + "/config.json");
    if (f.open(QIODevice::ReadOnly))
        config = QJsonDocument::fromJson(f.readAll()).object();
    english = config.value("language").toString("auto") == "english" ||
              (config.value("language").toString("auto") == "auto" &&
               QLocale::system().language() != QLocale::Chinese);
    paletteName = config.value("palette").toString("graphite");
    theme = Theme::starter();
    saved = theme.data;
    for (auto catalogRoot : {assets() + "/official/config", dataDir + "/themes"}) {
        QDirIterator it(catalogRoot, {"*.data", "*.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            auto file = it.next();
            if (file.endsWith("AppConfig.data"))
                continue;
            try {
                auto t = Theme::load(file);
                entries.append({file, t.name(), t.size()});
            } catch (const std::exception &) {
            }
        }
    }
    resourceTrace("catalog loaded");
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.name.localeAwareCompare(b.name) < 0; });
    initial = initial.isEmpty() ? config["selected_theme"].toString() : initial;
    if (!initial.isEmpty())
        try {
            theme = Theme::load(initial);
            path = initial;
            saved = theme.data;
        } catch (const std::exception &) {
        }
    connect(&sampling, &QTimer::timeout, this, &MainWindow::tick);
    sampling.setTimerType(Qt::CoarseTimer);
    sampling.start(qBound(1000, config.value("interval_ms").toInt(2000), 10000));
    renderTimer.setSingleShot(true);
    connect(&renderTimer, &QTimer::timeout, this, &MainWindow::render);
    build();
    resourceTrace("UI built");
    makeTray();
    sample = collector.collect(config["disk"].toString());
    render();
    resourceTrace("rendered");
    resize(1360, 900);
    setMinimumSize(1000, 660);
    resourceClock.start();
}
MainWindow::~MainWindow() {
    persist();
}
void MainWindow::applyPalette() {
    bool light = paletteName == "light";
    QString bg = light                       ? "#eef1f5"
                 : paletteName == "midnight" ? "#151321"
                 : paletteName == "forest"   ? "#101b18"
                                             : "#11161e";
    QString card = light                       ? "#ffffff"
                   : paletteName == "midnight" ? "#211e30"
                   : paletteName == "forest"   ? "#1b2b25"
                                               : "#1c242e";
    QString text = light ? "#172337" : "#e7edf5", muted = light ? "#526073" : "#a9b8ca",
            line = light ? "#c9d2de" : "#344150", control = light ? "#f3f6fa" : "#25313d";
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(bg));
    pal.setColor(QPalette::Base, QColor(card));
    pal.setColor(QPalette::Text, QColor(text));
    pal.setColor(QPalette::WindowText, QColor(text));
    pal.setColor(QPalette::Button, QColor(control));
    pal.setColor(QPalette::ButtonText, QColor(text));
    pal.setColor(QPalette::Highlight, QColor("#2e68b1"));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor(card));
    pal.setColor(QPalette::ToolTipText, QColor(text));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(muted));
    qApp->setPalette(pal);
    qApp->setStyleSheet(
        QString(
            "QWidget{font-size:14px;color:%1;}QMainWindow,QWidget#root{background:%2;}QFrame#panel{"
            "background:%3;border:1px solid "
            "%4;border-radius:8px;}QPushButton,QToolButton,QComboBox,QLineEdit,QSpinBox{min-height:28px;"
            "padding:3px 8px;border:1px solid "
            "%4;border-radius:4px;background:%5;}QPushButton:hover,QToolButton:hover{border-color:#5598ff;}"
            "QPushButton:disabled{color:%6;}QPushButton#primary{background:#2878f0;color:white;border-color:#"
            "2878f0;}QPushButton#primary:disabled{background:#293c54;color:#8995a6;border-color:#344150;}"
            "QLineEdit:focus{border-color:#438af4;}QLabel#heading{font-size:23px;font-weight:600;}QLabel#"
            "muted{color:%6;}QTabBar::tab{padding:12px 24px;border-bottom:2px solid "
            "transparent;}QTabBar::tab:selected{color:#5598ff;border-bottom:2px solid "
            "#438af4;}QTreeWidget,QListWidget,QTableWidget,QPlainTextEdit{border:1px solid "
            "%4;border-radius:4px;background:%3;selection-background-color:#294e7b;}QTreeWidget::item{height:"
            "30px;}QListWidget::item{padding:8px;}QListWidget::item:selected{background:#294e7b;border-left:"
            "3px solid "
            "#438af4;}QHeaderView::section{background:%5;padding:6px;border:0;}QGroupBox{border:1px solid "
            "%4;border-radius:6px;margin-top:16px;padding:16px;}QGroupBox::title{subcontrol-origin:margin;"
            "left:12px;}QScrollBar:vertical{width:10px;background:%2;}QScrollBar::handle:vertical{background:"
            "%4;min-height:24px;border-radius:4px;}QStatusBar{border-top:1px solid "
            "%4;}QMenu{background:%3;border:1px solid %4;}QMenu::item{padding:7px "
            "24px;}QMenu::item:selected{background:#294e7b;}QToolTip{background:%3;color:%1;border:1px solid "
            "%4;}")
            .arg(text, bg, card, line, control, muted));
}
void MainWindow::build() {
    building = true;
    auto old = takeCentralWidget();
    if (old)
        old->deleteLater();
    root = new QWidget;
    root->setObjectName("root");
    auto outer = new QVBoxLayout(root);
    outer->setContentsMargins(12, 8, 12, 0);
    outer->setSpacing(8);
    auto header = new QHBoxLayout;
    label("Turing Screen · Qt", header, "heading");
    header->addSpacing(24);
    navigation = new QTabBar;
    for (auto t :
         {L("主题", "Themes"), L("传感器", "Sensors"), L("设置", "Settings"), L("诊断", "Diagnostics")})
        navigation->addTab(t);
    navigation->setExpanding(false);
    header->addWidget(navigation);
    header->addStretch();
    button(L("保存主题", "Save"), header, [this] { saveTheme(); });
    outer->addLayout(header);
    pages = new QStackedWidget;
    outer->addWidget(pages, 1);
    auto t = new QWidget;
    pages->addWidget(t);
    buildThemePage(t);
    auto sensors = new QWidget;
    pages->addWidget(sensors);
    buildSensors(sensors);
    auto settings = new QWidget;
    pages->addWidget(settings);
    buildSettings(settings);
    auto diag = new QWidget;
    pages->addWidget(diag);
    buildDiagnostics(diag);
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
    statusBar()->addWidget(dataState);
    statusBar()->addWidget(status, 1);
    statusBar()->addPermanentWidget(connection);
    statusBar()->addPermanentWidget(sendState);
    auto quitButton = new QPushButton(L("退出", "Exit"));
    quitButton->setObjectName("statusPart");
    statusBar()->addPermanentWidget(quitButton);
    connect(quitButton, &QPushButton::clicked, this, &MainWindow::quit);
    connect(navigation, &QTabBar::currentChanged, this, &MainWindow::setPage);
    navigation->setCurrentIndex(page);
    pages->setCurrentIndex(page);
    setWindowTitle(L("Turing Screen Qt · USB 副屏", "Turing Screen Qt · USB Display"));
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
    shortcut(QKeySequence::Undo, [this] { undo(); });
    shortcut(QKeySequence::Redo, [this] { redo(); });
    shortcut(QKeySequence(Qt::Key_F2), [this] {
        if (editing && page == 0)
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
            if (page == 0 && editing && !qobject_cast<QLineEdit *>(QApplication::focusWidget()))
                layerAction(pair.second);
        });
    shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] {
        if (page == 0)
            createLayer("text");
    });
    applyPalette();
    buildLibrary();
    buildTree();
    if (tray)
        makeTray();
    building = false;
    setEditing(editing);
    updateStatus();
    properties();
}
void MainWindow::buildThemePage(QWidget *w) {
    auto layout = new QHBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto splitter = new QSplitter;
    layout->addWidget(splitter);
    left = new QFrame;
    left->setObjectName("panel");
    auto lv = new QVBoxLayout(left);
    lv->setContentsMargins(12, 16, 12, 12);
    auto lt = new QHBoxLayout;
    button(L("主题库", "Library"), lt, [this] {
        leftPages->setCurrentIndex(0);
        thumbnails();
    });
    button(L("图层", "Layers"), lt, [this] {
        setEditing(true);
        leftPages->setCurrentIndex(1);
    });
    lv->addLayout(lt);
    leftPages = new QStackedWidget;
    lv->addWidget(leftPages, 1);
    auto lib = new QWidget;
    auto ll = new QVBoxLayout(lib);
    ll->setContentsMargins(0, 0, 0, 0);
    search = new QLineEdit;
    search->setPlaceholderText(L("搜索主题", "Search themes"));
    ll->addWidget(search);
    auto libraryButtons = new QHBoxLayout;
    button(L("＋ 新建", "+ New"), libraryButtons, [this] { newTheme(); });
    button(L("导入", "Import"), libraryButtons, [this] { importTheme(); });
    ll->addLayout(libraryButtons);
    library = new QListWidget;
    library->setIconSize(QSize(88, 60));
    library->setUniformItemSizes(true);
    ll->addWidget(library, 1);
    leftPages->addWidget(lib);
    connect(search, &QLineEdit::textChanged, this, [this](const QString &s) {
        for (int n = 0; n < library->count(); ++n)
            library->item(n)->setHidden(!library->item(n)->text().contains(s, Qt::CaseInsensitive));
        thumbnails();
    });
    connect(library, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item && !building)
            choose(item->data(Qt::UserRole).toString());
    });
    connect(library->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { thumbnails(); });
    auto layers = new QWidget;
    auto llayers = new QVBoxLayout(layers);
    llayers->setContentsMargins(0, 0, 0, 0);
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
    leftPages->addWidget(layers);
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
    splitter->addWidget(left);
    auto center = new QWidget;
    auto cv = new QVBoxLayout(center);
    cv->setContentsMargins(16, 16, 16, 8);
    auto heading = new QHBoxLayout;
    auto text = new QVBoxLayout;
    title = label(theme.name(), text, "heading");
    title->setMinimumWidth(120);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    resolution = label({}, text, "muted");
    heading->addLayout(text, 1);
    modeButton = button({}, heading, [this] { setEditing(!editing); });
    auto themeOptions = button(L("主题选项", "Theme options"), heading, [] {});
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
    sendButton = button({}, heading, [this] { toggleSend(); });
    sendButton->setObjectName("primary");
    cv->addLayout(heading);
    auto tools = new QHBoxLayout;
    undoButton = button(L("撤销", "Undo"), tools, [this] { undo(); });
    redoButton = button(L("重做", "Redo"), tools, [this] { redo(); });
    auto toolBox = new QComboBox;
    for (auto p : QList<QPair<QString, QString>>{{"move", L("移动", "Move")},
                                                 {"text", L("文字", "Text")},
                                                 {"rectangle", L("矩形", "Rectangle")},
                                                 {"ellipse", L("椭圆", "Ellipse")},
                                                 {"line", L("直线", "Line")}})
        toolBox->addItem(p.second, p.first);
    tools->addWidget(toolBox);
    button(L("适应窗口", "Fit"), tools, [this] { canvas->setFit(true); });
    button("1:1", tools, [this] { canvas->setZoom(1); });
    button("−", tools,
           [this] { canvas->setZoom(canvas->screenRect().width() / theme.size().width() / 1.2); });
    button("+", tools,
           [this] { canvas->setZoom(canvas->screenRect().width() / theme.size().width() * 1.2); });
    cv->addLayout(tools);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    canvas = new Canvas;
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
    label(L("预览与副屏使用相同像素", "Preview and device share the same pixels"), options, "muted");
    cv->addLayout(options);
    splitter->addWidget(center);
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
    connect(canvas, &Canvas::committed, this, &MainWindow::remember);
    connect(canvas, &Canvas::createRequested, this, &MainWindow::createLayer);
    connect(canvas, &Canvas::contextRequested, this, &MainWindow::contextMenu);
    right = new QFrame;
    right->setObjectName("panel");
    auto rv = new QVBoxLayout(right);
    rv->setContentsMargins(16, 16, 16, 16);
    label(L("设备", "Device"), rv, "heading");
    portBox = new QComboBox;
    rv->addWidget(portBox);
    auto connectionButtons = new QHBoxLayout;
    connectButton = button({}, connectionButtons, [this] { connectDevice(); });
    connectButton->setObjectName("primary");
    button(L("刷新", "Refresh"), connectionButtons, [this] { refreshPorts(); });
    rv->addLayout(connectionButtons);
    auto form = new QFormLayout;
    directionBox = new QComboBox;
    for (auto directionText : {L("竖屏", "Portrait"), L("反向竖屏", "Reverse portrait"),
                               L("横屏", "Landscape"), L("反向横屏", "Reverse landscape")})
        directionBox->addItem(directionText);
    directionBox->setCurrentIndex(config.value("orientation").toInt(theme.size().width() > 320 ? 2 : 0));
    form->addRow(L("方向", "Orientation"), directionBox);
    brightness = new QSlider(Qt::Horizontal);
    brightness->setRange(0, 100);
    brightness->setValue(config.value("brightness").toInt(75));
    form->addRow(L("亮度", "Brightness"), brightness);
    mirrorBox = new QCheckBox(L("镜像显示", "Mirror display"));
    mirrorBox->setChecked(config["mirror"].toBool());
    form->addRow(mirrorBox);
    rv->addLayout(form);
    connect(directionBox, &QComboBox::currentIndexChanged, this, [this](int v) {
        config["orientation"] = v;
        device.command(4, v);
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
    auto separator = new QFrame;
    separator->setFrameShape(QFrame::HLine);
    rv->addWidget(separator);
    label(L("图层属性", "Layer properties"), rv, "heading");
    auto props = new QFormLayout;
    layerName = new QLineEdit;
    layerText = new QLineEdit;
    bindingBox = new QComboBox;
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
    splitter->addWidget(right);
    splitter->setSizes({260, 740, 312});
    splitter->setStretchFactor(1, 1);
    left->setMinimumWidth(224);
    right->setMinimumWidth(280);
    refreshPorts();
}
void MainWindow::buildSensors(QWidget *w) {
    auto l = new QVBoxLayout(w);
    label(L("本机传感器", "Local sensors"), l, "heading");
    label(L("默认每 2 秒采集。GPU、天气、外部硬件助手在 Qt 精简版中移除；无法获取的数据显示 --。",
            "Samples every 2 seconds by default. GPU, weather and external hardware helpers are omitted; "
            "unavailable values show --."),
          l, "muted");
    sensorTable = new QTableWidget(0, 3);
    sensorTable->setHorizontalHeaderLabels({L("项目", "Metric"), L("数值", "Value"), L("来源", "Source")});
    sensorTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    sensorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    l->addWidget(sensorTable, 1);
}
void MainWindow::buildSettings(QWidget *w) {
    auto outer = new QVBoxLayout(w);
    label(L("设置", "Settings"), outer, "heading");
    auto group = new QGroupBox(L("界面与运行", "Appearance and behavior"));
    auto form = new QFormLayout(group);
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
    auto palette = new QComboBox;
    for (auto p : QList<QPair<QString, QString>>{{"graphite", L("石墨深色", "Graphite")},
                                                 {"light", L("浅色", "Light")},
                                                 {"midnight", L("午夜紫", "Midnight")},
                                                 {"forest", L("森林绿", "Forest")}})
        palette->addItem(p.second, p.first);
    palette->setCurrentIndex(qMax(0, palette->findData(paletteName)));
    form->addRow(L("界面配色", "Palette"), palette);
    connect(palette, &QComboBox::currentIndexChanged, this, [this, palette] {
        paletteName = palette->currentData().toString();
        config["palette"] = paletteName;
        applyPalette();
        persist();
    });
    auto interval = new QSpinBox;
    interval->setRange(1000, 10000);
    interval->setSingleStep(500);
    interval->setSuffix(" ms");
    interval->setValue(config.value("interval_ms").toInt(2000));
    form->addRow(L("采样间隔", "Sample interval"), interval);
    connect(interval, &QSpinBox::valueChanged, this, [this](int n) {
        config["interval_ms"] = n;
        sampling.setInterval(n);
        persist();
    });
    auto closeTray = new QCheckBox(L("关闭窗口时最小化到托盘", "Close window to tray"));
    closeTray->setChecked(config["minimize_to_tray"].toBool());
    form->addRow(closeTray);
    connect(closeTray, &QCheckBox::clicked, this, [this](bool v) {
        config["minimize_to_tray"] = v;
        persist();
    });
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
    outer->addWidget(group);
    label(L("Qt Widgets 原生界面，按需绘制；无浏览器、无动画循环。默认关闭在线服务和重型采集组件。",
            "Native Qt Widgets with on-demand painting, no browser or animation loop. Online services and "
            "heavy collectors are omitted."),
          outer, "muted");
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
void MainWindow::buildDiagnostics(QWidget *w) {
    auto l = new QVBoxLayout(w);
    label(L("诊断", "Diagnostics"), l, "heading");
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
    page = n;
    pages->setCurrentIndex(n);
    if (n == 1)
        tick();
    if (n == 3)
        diagnosticText->setPlainText(QString::fromUtf8(QJsonDocument(diagnostics()).toJson()));
    if (n == 0) {
        render();
        thumbnails();
    }
}
void MainWindow::setEditing(bool value) {
    editing = value;
    canvas->editing = value;
    modeButton->setText(value ? L("切换到预览", "Switch to preview") : L("切换到编辑", "Switch to edit"));
    leftPages->setCurrentIndex(value ? 1 : 0);
    canvas->update();
    properties();
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
        i->setSizeHint(QSize(224, 76));
        if (path == this->path)
            i->setSelected(true);
    };
    add({}, L("Qt 系统概览", "Qt overview"), QSize(480, 320));
    for (auto e : entries)
        add(e.path, e.name, e.size);
    QTimer::singleShot(0, this, &MainWindow::thumbnails);
}
void MainWindow::thumbnails() {
    if (!library || page != 0 || !isVisible() || leftPages->currentIndex() != 0)
        return;
    int count = 0;
    for (int n = 0; n < library->count() && count < 10; ++n) {
        auto i = library->item(n);
        if (i->isHidden() || !library->visualItemRect(i).intersects(library->viewport()->rect()) ||
            !i->icon().isNull())
            continue;
        try {
            auto file = i->data(Qt::UserRole).toString();
            auto t = file.isEmpty() ? Theme::starter() : Theme::load(file);
            i->setIcon(QPixmap::fromImage(
                renderer.render(t, sample).scaled(88, 60, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            ++count;
        } catch (const std::exception &) {
        }
    }
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
        config["orientation"] = theme.size().width() > 320 ? 2 : 0;
        {
            QSignalBlocker b(directionBox);
            directionBox->setCurrentIndex(config["orientation"].toInt());
        }
        device.command(4, config["orientation"].toInt());
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
        if (!std::any_of(entries.begin(), entries.end(), [&](const Entry &e) { return e.path == file; }))
            entries.prepend({file, theme.name(), theme.size()});
        buildLibrary();
    }
}
void MainWindow::saveTheme(bool as) {
    auto target = path;
    if (as || target.isEmpty() || !theme.data.contains("format") || target.endsWith(".data"))
        target =
            QFileDialog::getSaveFileName(this, L("保存主题", "Save theme"),
                                         dataDir + "/themes/" + theme.name() + ".json", "Qt theme (*.json)");
    if (target.isEmpty())
        return;
    try {
        theme.save(target);
        path = target;
        saved = theme.data;
        dirty = false;
        config["selected_theme"] = path;
        bool found = false;
        for (auto &entry : entries)
            if (entry.path == path) {
                entry.name = theme.name();
                entry.size = theme.size();
                found = true;
            }
        if (!found)
            entries.prepend({path, theme.name(), theme.size()});
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
    if (!canvas)
        return;
    try {
        frame = renderer.render(theme, sample);
        canvas->mirrored = config["mirror"].toBool();
        canvas->setFrame(frame);
        title->setText(theme.name() + (dirty ? " *" : ""));
        resolution->setText(
            QString("%1 × %2 · %3")
                .arg(theme.size().width())
                .arg(theme.size().height())
                .arg(theme.size().width() > 320 ? L("横屏", "Landscape") : L("竖屏", "Portrait")));
        if (sending && device.status().connected) {
            QImage image = frame;
            int dir = config.value("orientation").toInt(2);
            if ((dir >= 2) != (image.width() > image.height()))
                image = image.transformed(QTransform().rotate(90));
            device.submit(image);
        }
        undoButton->setEnabled(!undoHistory.isEmpty());
        redoButton->setEnabled(!redoHistory.isEmpty());
        updateStatus();
    } catch (const std::exception &e) {
        if (status)
            status->setText(QString::fromUtf8(e.what()));
    }
}
void MainWindow::tick() {
    sample = collector.collect(config["disk"].toString());
    if ((isVisible() && page == 0) || (sending && device.status().connected))
        render();
    else
        updateStatus();
    if (page == 1 && isVisible()) {
        auto m = metrics();
        sensorTable->setRowCount(int(m.size()));
        for (int n = 0; n < m.size(); ++n) {
            sensorTable->setItem(n, 0, new QTableWidgetItem(english ? m[n].first : m[n].second));
            auto value = new QTableWidgetItem(sample.text(m[n].first));
            value->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sensorTable->setItem(n, 1, value);
            sensorTable->setItem(
                n, 2,
                new QTableWidgetItem(sample.values.contains(m[n].first) || sample.strings.contains(m[n].first)
                                         ? L("系统 API", "System API")
                                         : "--"));
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
    for (auto widget : {static_cast<QWidget *>(brightness), static_cast<QWidget *>(directionBox),
                        static_cast<QWidget *>(mirrorBox)}) {
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
    config["selected_theme"] = path;
    QSaveFile f(dataDir + "/config.json");
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(config).toJson());
        f.commit();
    }
}
void MainWindow::makeTray() {
    if (tray) {
        auto oldMenu = tray->contextMenu();
        delete tray;
        delete oldMenu;
    }
    tray = new QSystemTrayIcon(style()->standardIcon(QStyle::SP_ComputerIcon), this);
    tray->setToolTip("Turing Screen Qt");
    auto menu = new QMenu(this);
    menu->addAction(L("显示窗口", "Show window"), this, [this] {
        showNormal();
        raise();
        activateWindow();
        render();
    });
    menu->addAction(L("暂停 / 继续发送", "Pause / resume sending"), this, &MainWindow::toggleSend);
    menu->addSeparator();
    menu->addAction(L("退出", "Exit"), this, &MainWindow::quit);
    tray->setContextMenu(menu);
    connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::DoubleClick) {
            showNormal();
            raise();
            activateWindow();
            render();
        }
    });
    tray->show();
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
    if (!quitting && config["minimize_to_tray"].toBool() && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
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
    return {{"application", "Turing Screen Qt"},
            {"version", SCREEN_VERSION},
            {"qt", qVersion()},
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
    timer->setInterval(stress ? 250 : 200);
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
                setPage(n % 4);
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
                setPage(0);
                workloadStage = "steady";
            }
        } else {
            static const char *names[]{"preview",     "editor", "sensors", "settings",
                                       "diagnostics", "light",  "english"};
            if (n < 7) {
                if (n == 1)
                    setEditing(true);
                if (n >= 2 && n <= 4) {
                    navigation->setCurrentIndex(n - 1);
                }
                if (n == 5) {
                    paletteName = "light";
                    applyPalette();
                    navigation->setCurrentIndex(0);
                }
                if (n == 6) {
                    english = true;
                    build();
                    render();
                }
                QTimer::singleShot(80, this, [this, dir, n] { grab().save(dir + "/" + names[n] + ".png"); });
            }
        }
        if (elapsed >= seconds) {
            finishResources(dir, elapsed);
            QFile marker(dir + "/smoke-ok.txt");
            if (marker.open(QIODevice::WriteOnly))
                marker.write("Qt C++ workload completed");
            dirty = false;
            timer->stop();
            quit();
        }
    });
    timer->start();
}
} // namespace Screen
