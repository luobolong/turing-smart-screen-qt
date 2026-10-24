#include "application.h"
#include <QtWidgets>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
namespace Screen {
ApplicationController::ApplicationController(QString directory, const QString &theme,
                                             std::function<bool()> available)
    : runtime(std::make_shared<DisplayRuntime>(std::move(directory), theme)),
      tray(new QSystemTrayIcon(this)), trayAvailable(std::move(available)) {
    qApp->setQuitOnLastWindowClosed(false);
    countdown.setSingleShot(true);
    countdown.setTimerType(Qt::PreciseTimer);
    connect(&countdown, &QTimer::timeout, this, [this] {
        if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) {
            countdown.start(1000);
            return;
        }
        enterLightweight();
    });
    connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showWindow();
    });
    makeWindow();
    makeTray();
}
ApplicationController::~ApplicationController() {
    editor.reset();
    // Keep the menu alive until the native tray backend has detached it.
    delete tray;
    tray = nullptr;
}
QString ApplicationController::L(const char *zh, const char *en) const {
    const auto language = runtime->config.value("language").toString("auto");
    const bool english = language == "english" || (language == "auto" && QLocale::system().language() != QLocale::Chinese);
    return QString::fromUtf8(english ? en : zh);
}
void ApplicationController::makeWindow() {
    editor = std::make_unique<MainWindow>(runtime);
    editor->trayAvailable = trayAvailable;
    connect(editor.get(), &MainWindow::windowOpened, this, [this] { countdown.stop(); closed = false; });
    connect(editor.get(), &MainWindow::lightweightRequested, this, [this] { enterLightweight(); }, Qt::QueuedConnection);
    connect(editor.get(), &MainWindow::closedToTray, this, [this] {
        closed = true;
        scheduleLightweight();
    });
    connect(editor.get(), &MainWindow::preferencesChanged, this, [this] {
        makeTray();
        scheduleLightweight();
    });
}
void ApplicationController::makeTray() {
    if (!menu) {
        menu = std::make_unique<QMenu>();
        tray->setContextMenu(menu.get());
    }
    menu->clear();
    if (editor) menu->setPalette(editor->palette());
    menu->addAction(L("显示窗口", "Show window"), this, &ApplicationController::showWindow);
    auto light = menu->addAction(L("进入轻量模式", "Enter lightweight mode"), this,
                                [this] { enterLightweight(); }, Qt::QueuedConnection);
    light->setEnabled(!isLightweight() && trayAvailable());
    menu->addAction(L("暂停 / 继续发送", "Pause / resume sending"), runtime.get(), &DisplayRuntime::toggleSending);
    menu->addSeparator();
    menu->addAction(L("退出", "Exit"), this, &ApplicationController::quit);
    tray->setIcon(qApp->style()->standardIcon(QStyle::SP_ComputerIcon));
    tray->setToolTip(isLightweight() ? L("Turing Screen · 轻量模式", "Turing Screen · Lightweight") : "Turing Screen");
    tray->show();
}
void ApplicationController::scheduleLightweight() {
    countdown.stop();
    if (closed && editor && !editor->isVisible() && runtime->config["auto_lightweight"].toBool() && trayAvailable())
        countdown.start(qBound(1, runtime->config.value("lightweight_delay_seconds").toInt(60), 86400) * 1000);
}
QJsonObject ApplicationController::editorState() const {
    const auto history = [](const QList<QJsonObject> &list) {
        QJsonArray result;
        for (const auto &item : list) result.append(item);
        return result;
    };
    QJsonArray drafts;
    for (auto i = editor->drafts.cbegin(); i != editor->drafts.cend(); ++i)
        drafts.append(QJsonObject{{"key", i.key()}, {"theme", i->theme.data}, {"path", i->path},
                                  {"saved", i->saved}, {"undo", history(i->undo)}, {"redo", history(i->redo)}});
    return {{"saved", editor->saved}, {"undo", history(editor->undoHistory)}, {"redo", history(editor->redoHistory)},
            {"drafts", drafts}, {"clipboard", editor->clipboard}, {"selection", QJsonArray::fromStringList(editor->selection())},
            {"editing", editor->editing}, {"page", editor->page}, {"search", editor->search->text()},
            {"geometry", QString::fromLatin1(editor->saveGeometry().toBase64())}};
}
void ApplicationController::restoreEditor(const QJsonObject &state) {
    const auto history = [](const QJsonValue &value) {
        QList<QJsonObject> list;
        for (const auto &item : value.toArray()) list.append(item.toObject());
        return list;
    };
    editor->saved = state["saved"].toObject();
    editor->undoHistory = history(state["undo"]);
    editor->redoHistory = history(state["redo"]);
    for (const auto &value : state["drafts"].toArray()) {
        const auto d = value.toObject();
        editor->drafts.insert(d["key"].toString(), MainWindow::Document{
            Theme::fromJson(d["theme"].toObject()), d["path"].toString(), d["saved"].toObject(),
            history(d["undo"]), history(d["redo"])});
    }
    editor->clipboard = state["clipboard"].toArray();
    editor->dirty = editor->theme.data != editor->saved;
    editor->buildTree();
    QStringList selection;
    for (const auto &value : state["selection"].toArray()) selection.append(value.toString());
    editor->selection(selection);
    editor->setEditing(state["editing"].toBool());
    editor->navigation->setCurrentIndex(qBound(0, state["page"].toInt(), 3));
    editor->search->setText(state["search"].toString());
    editor->restoreGeometry(QByteArray::fromBase64(state["geometry"].toString().toLatin1()));
}
bool ApplicationController::enterLightweight() {
    if (isLightweight()) return true;
    if (!editor || !trayAvailable() || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    // Write histories and inactive drafts before releasing them from memory.
    auto file = std::make_unique<QTemporaryFile>(runtime->dataDir + "/lightweight-session-XXXXXX.json");
    const auto state = editorState();
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    QString failure;
    if (!file->open() || file->write(bytes) != bytes.size() || !file->flush() || !runtime->persist(&failure)) {
        editor->status->setText(L("无法暂存编辑状态，未进入轻量模式。", "Could not preserve the editor session; lightweight mode canceled."));
        return false;
    }
    file->close();
    unsaved = editor->dirty;
    for (const auto &draft : editor->drafts) unsaved |= draft.theme.data != draft.saved;
    suspended = std::move(file);
    countdown.stop();
    runtime->preview = false;
    runtime->setLightweight(true);
    editor.reset(); // Destroy the actual native window and all editor widgets.
    runtime->renderer.clearCaches();
    runtime->frame = {};
    QPixmapCache::clear();
    makeTray();
    // Return free allocator pages once, rather than merely hiding the window.
#if defined(__GLIBC__)
    QTimer::singleShot(0, this, [] { malloc_trim(0); });
#endif
    return true;
}
void ApplicationController::showWindow() {
    countdown.stop();
    closed = false;
    if (!editor) {
        QJsonObject state;
        if (suspended) {
            QFile file(suspended->fileName());
            if (!file.open(QIODevice::ReadOnly)) {
                tray->showMessage("Turing Screen", L("无法恢复编辑状态。", "Could not restore the editor session."));
                return;
            }
            QJsonParseError parse;
            const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
            if (parse.error != QJsonParseError::NoError || !document.isObject()) return;
            state = document.object();
        }
        runtime->setLightweight(false);
        runtime->tick();
        makeWindow();
        if (!state.isEmpty()) restoreEditor(state);
        suspended.reset();
        makeTray();
    }
    editor->showNormal();
    editor->raise();
    editor->activateWindow();
    editor->render();
}
void ApplicationController::quit() {
    if (!editor && !unsaved) { runtime->persist(); qApp->quit(); return; }
    if (!editor) showWindow();
    if (editor) editor->quit();
}
} // namespace Screen
