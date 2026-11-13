#include "mainwindow.h"
#include <QtWidgets>
#include <QDesktopServices>
namespace Screen {
namespace {
bool related(const QString &file, const QString &parent) {
    if (file.isEmpty() || parent.isEmpty()) return false;
    const auto relative = QDir(parent).relativeFilePath(QFileInfo(file).absoluteFilePath());
    return relative == "." || (!relative.startsWith("../") && relative != ".." && !QDir::isAbsolutePath(relative));
}
}
void MainWindow::buildFolderTree() {
    if (!library) return;
    QSignalBlocker blocker(library);
    QSet<QString> expanded;
    const bool previous = library->topLevelItemCount() > 0;
    const bool folderSelected = library->currentItem() && library->currentItem()->data(0, Qt::UserRole + 1).toString() == "folder";
    QTreeWidgetItemIterator old(library);
    while (*old) { if ((*old)->isExpanded()) expanded.insert((*old)->data(0, Qt::UserRole).toString()); ++old; }
    libraryItems.clear();
    library->clear();
    const auto catalogRoot = runtime->library.root;
    if (libraryFolder.isEmpty()) libraryFolder = QDir::cleanPath(catalogRoot + '/' + config["theme_folder"].toString("."));
    if (!runtime->library.contains(libraryFolder) || !QFileInfo(libraryFolder).isDir()) libraryFolder = catalogRoot;
    QTreeWidgetItem *active = nullptr, *selectedFolder = nullptr;
    QFileIconProvider icons;
    const auto addTheme = [&](const Entry &entry, QTreeWidgetItem *parent) {
        auto item = new QTreeWidgetItem(parent);
        item->setText(0, QString("%1\n%2 × %3 · %4").arg(QFileInfo(entry.path).fileName()).arg(entry.size.width()).arg(entry.size.height())
            .arg(entry.size.width() > entry.size.height() ? L("横屏", "Landscape") : L("竖屏", "Portrait")));
        item->setData(0, Qt::UserRole, entry.path);
        item->setData(0, Qt::UserRole + 1, "theme");
        item->setToolTip(0, item->text(0) + (entry.path.isEmpty() ? QString() : "\n" + QDir::toNativeSeparators(entry.path)));
        const QFileInfo info(entry.path);
        const auto cacheKey = entry.path + ':' + QString::number(info.lastModified().toMSecsSinceEpoch()) + ':' + QString::number(info.size());
        if (const auto cached = thumbnailCache.object(cacheKey)) {
            QIcon icon(*cached); icon.addPixmap(*cached, QIcon::Selected); icon.addPixmap(*cached, QIcon::Active);
            item->setIcon(0, icon);
        }
        libraryItems.append(item);
        if (libraryKey(entry.path) == libraryKey(path)) active = item;
    };
    std::function<void(QString, QTreeWidgetItem *)> add = [&](QString directory, QTreeWidgetItem *parent) {
        auto item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(library);
        item->setText(0, QFileInfo(directory).fileName());
        item->setData(0, Qt::UserRole, directory);
        item->setData(0, Qt::UserRole + 1, "folder");
        item->setToolTip(0, QDir::toNativeSeparators(directory));
        item->setIcon(0, directory == catalogRoot + "/deleted" ? style()->standardIcon(QStyle::SP_TrashIcon) : icons.icon(QFileInfo(directory)));
        for (const auto &sub : QDir(directory).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) add(sub.absoluteFilePath(), item);
        for (const auto &entry : entries)
            if (QFileInfo(entry.path).absolutePath() == directory ||
                (directory == catalogRoot + "/custom" && !runtime->library.contains(entry.path))) addTheme(entry, item);
        const bool showActive = !path.isEmpty() && related(path, directory);
        item->setExpanded(expanded.contains(directory) || showActive || (!previous && directory == catalogRoot + "/custom"));
        if (directory == libraryFolder) selectedFolder = item;
    };
    QStringList directories{"official", "custom", "deleted"};
    for (const auto &name : QDir(catalogRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (!directories.contains(name)) directories.append(name);
    for (const auto &name : directories) add(catalogRoot + '/' + name, nullptr);
    auto current = folderSelected && selectedFolder ? selectedFolder : active ? active : selectedFolder;
    library->setCurrentItem(current);
    if (current) { for (auto parent = current->parent(); parent; parent = parent->parent()) parent->setExpanded(true); library->scrollToItem(current); }
    QTimer::singleShot(0, this, [this] { library->doItemsLayout(); thumbnails(); });
}
void MainWindow::selectLibraryFolder(const QString &folder) {
    if (!runtime->library.contains(folder) || !QFileInfo(folder).isDir()) return;
    libraryFolder = folder;
    config["theme_folder"] = QDir(runtime->library.root).relativeFilePath(folder);
    QTreeWidgetItemIterator it(library);
    while (*it) {
        auto item = *it;
        if (item->data(0, Qt::UserRole).toString() == folder && item->data(0, Qt::UserRole + 1).toString() == "folder") {
            QSignalBlocker blocker(library); library->setCurrentItem(item); item->setExpanded(true);
            for (auto parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
            library->scrollToItem(item); break;
        }
        ++it;
    }
    filterLibrary(); persist();
}
QString MainWindow::targetLibraryFolder() const {
    return runtime->library.contains(libraryFolder) && !runtime->library.recycled(libraryFolder)
        && QFileInfo(libraryFolder).isDir() && libraryFolder != runtime->library.root
        ? libraryFolder : runtime->library.root + "/custom";
}
void MainWindow::filterLibrary() {
    if (!library) return;
    const auto query = libraryQuery;
    int matches = 0;
    std::function<bool(QTreeWidgetItem *, bool)> visit = [&](QTreeWidgetItem *item, bool folderMatches) {
        const bool folder = item->data(0, Qt::UserRole + 1).toString() == "folder";
        const bool ownMatch = query.isEmpty() || folderMatches || item->text(0).contains(query, Qt::CaseInsensitive);
        bool visible = folder ? ownMatch && item->childCount() == 0 : ownMatch;
        for (int n = 0; n < item->childCount(); ++n) visible |= visit(item->child(n), ownMatch);
        if (folder && query.isEmpty()) visible = true;
        item->setHidden(!visible);
        if (!folder && visible) ++matches;
        if (folder && visible && !query.isEmpty()) item->setExpanded(true);
        return visible;
    };
    for (int i = 0; i < library->topLevelItemCount(); ++i) visit(library->topLevelItem(i), false);
    libraryEmpty->setVisible(matches == 0 && !query.isEmpty());
    thumbnails();
}
void MainWindow::searchLibrary() {
    libraryQuery = search->text().trimmed();
    filterLibrary();
}
void MainWindow::selectOpenedTheme() {
    librarySplit->setPaneCollapsed(0, false);
    libraryQuery.clear();
    search->clear();
    filterLibrary();
    QSignalBlocker blocker(library);
    for (auto item : libraryItems) if (libraryKey(item->data(0, Qt::UserRole).toString()) == libraryKey(path)) {
        for (auto parent = item->parent(); parent; parent = parent->parent()) parent->setExpanded(true);
        library->setCurrentItem(item);
        library->scrollToItem(item);
        library->setFocus();
        break;
    }
    thumbnails();
}
void MainWindow::expandSelectedThemeFolder() {
    const auto selected = library->selectedItems();
    for (auto item : selected) {
        if (item->data(0, Qt::UserRole + 1).toString() != "folder") item = item->parent();
        if (item) library->expandRecursively(library->indexFromItem(item));
    }
    thumbnails();
}
void MainWindow::newLibraryFolder(const QString &parent) {
    bool ok = false;
    const auto directory = parent.isEmpty() ? (libraryFolder.isEmpty() ? runtime->library.root : libraryFolder) : parent;
    const auto name = QInputDialog::getText(this, L("新建文件夹", "New folder"), L("文件夹名称", "Folder name"),
        QLineEdit::Normal, L("新建文件夹", "New folder"), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    try {
        libraryFolder = runtime->library.createFolder(directory, name);
        config["theme_folder"] = QDir(runtime->library.root).relativeFilePath(libraryFolder);
        buildFolderTree(); selectLibraryFolder(libraryFolder); persist();
    } catch (const std::exception &e) { QMessageBox::warning(this, L("新建失败", "Cannot create folder"), QString::fromUtf8(e.what())); }
}
void MainWindow::rebindLibraryPaths(const QString &source, const QString &destination) {
    const auto from = source, to = destination; // These arguments can alias the active document path.
    const auto remap = [&](QString value) {
        if (value == from) return to;
        return related(value, from) ? to + '/' + QDir(from).relativeFilePath(value) : value;
    };
    auto directions = config["theme_orientations"].toObject();
    for (const auto &entry : entries) if (entry.path == from || related(entry.path, from)) {
        const auto old = orientationKey(entry.path), next = orientationKey(remap(entry.path));
        if (directions.contains(old)) { const auto value = directions.take(old); directions[next] = value; }
    }
    config["theme_orientations"] = directions;
    path = remap(path);
    config["selected_theme"] = path;
    QHash<QString, Document> rebound;
    for (auto it = drafts.begin(); it != drafts.end(); ++it) { auto draft = it.value(); draft.path = remap(draft.path); rebound.insert(remap(it.key()), draft); }
    drafts = rebound;
    if (!libraryFolder.isEmpty()) libraryFolder = remap(libraryFolder);
    config["theme_folder"] = QDir(runtime->library.root).relativeFilePath(libraryFolder);
}
bool MainWindow::renameLibraryPath(const QString &file, const QString &name, QString &error) {
    try {
        if (file.isEmpty()) { mutate([&](Theme &t) { t.data["name"] = name; }); return true; }
        const bool folder = QFileInfo(file).isDir();
        const auto next = folder ? runtime->library.rename(file, name) : runtime->library.renameTheme(file, name);
        rebindLibraryPaths(file, next);
        if (!folder) {
            if (path == next) { theme.data["name"] = name; saved["name"] = name; dirty = theme.data != saved; }
            if (drafts.contains(next)) { drafts[next].theme.data["name"] = name; drafts[next].saved["name"] = name; }
        }
        reloadLibrary(); buildLibrary(); persist(); render();
        return true;
    } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); return false; }
}
void MainWindow::renameLibraryItem(const QString &file) {
    if (!file.isEmpty() && (!runtime->library.contains(file) || runtime->library.protectedFolder(file) || runtime->library.recycled(file))) return;
    const bool folder = !file.isEmpty() && QFileInfo(file).isDir();
    auto name = folder ? QFileInfo(file).fileName() : theme.name();
    if (!folder && file != path) for (const auto &entry : entries) if (entry.path == file) name = entry.name;
    bool ok = false;
    name = QInputDialog::getText(this, folder ? L("重命名文件夹", "Rename folder") : L("重命名主题", "Rename theme"),
        L("名称", "Name"), QLineEdit::Normal, name, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    QString error;
    if (!renameLibraryPath(file, name, error)) QMessageBox::warning(this, L("重命名失败", "Rename failed"), error);
}
void MainWindow::transferLibraryItem(const QString &source, const QString &directory, bool copy) {
    const auto file = source;
    if (directory == runtime->library.root + "/deleted" && !copy) { deleteTheme(file); return; }
    try {
        const auto target = runtime->library.transfer(file, directory, copy);
        if (!copy) {
            if (runtime->library.recycled(target)) {
                if (path == file || related(path, file)) { sending = false; choose({}); }
                for (const auto &key : drafts.keys()) if (key == file || related(key, file)) drafts.remove(key);
            } else rebindLibraryPaths(file, target);
        }
        reloadLibrary(); buildLibrary(); persist(); render();
    } catch (const std::exception &e) { QMessageBox::warning(this, L("操作失败", "Operation failed"), QString::fromUtf8(e.what())); }
}
void MainWindow::restoreLibraryItem(const QString &file) {
    try {
        const auto target = runtime->library.restore(file);
        rebindLibraryPaths(file, target);
        reloadLibrary(); buildLibrary(); persist(); render();
    } catch (const std::exception &e) { QMessageBox::warning(this, L("还原失败", "Restore failed"), QString::fromUtf8(e.what())); }
}
void MainWindow::libraryMenu(const QString &file, QPoint global) {
    QMenu menu(this);
    menu.setObjectName("themeLibraryMenu");
    const bool exists = !file.isEmpty() && QFileInfo::exists(file);
    const bool folder = exists && QFileInfo(file).isDir();
    const bool owned = exists && runtime->library.contains(file) && !runtime->library.protectedFolder(file);
    const bool trash = exists && runtime->library.recycled(file);
    const auto directory = folder ? file : exists ? QFileInfo(file).absolutePath() : runtime->library.root;
    if (exists) {
        auto open = menu.addAction(folder ? L("打开文件夹", "Open folder") : L("编辑主题", "Edit theme"), this, [this, file, folder] {
            if (folder) selectLibraryFolder(file); else { choose(file); setEditing(true); }
        });
        open->setObjectName("libraryOpen");
        if (trash && owned) menu.addAction(L("还原", "Restore"), this, [this, file] { restoreLibraryItem(file); })->setObjectName("libraryRestore");
        auto rename = menu.addAction(L("重命名", "Rename"), this, [this, file] { renameLibraryItem(file); });
        rename->setEnabled(owned && !trash); rename->setObjectName("libraryRename");
        menu.addSeparator();
        auto copy = menu.addAction(L("复制", "Copy"), this, [this, file] { fileClipboard = file; fileCut = false; });
        copy->setEnabled(owned && !trash); copy->setObjectName("libraryCopy");
        auto cut = menu.addAction(L("剪切", "Cut"), this, [this, file] { fileClipboard = file; fileCut = true; });
        cut->setEnabled(owned && !trash); cut->setObjectName("libraryCut");
        auto duplicate = menu.addAction(L("创建副本", "Duplicate"), this, [this, file] { transferLibraryItem(file, QFileInfo(file).absolutePath(), true); });
        duplicate->setEnabled(owned && !trash);
        auto move = menu.addAction(L("移动到…", "Move to…"), this, [this, file] {
            const auto destination = QFileDialog::getExistingDirectory(this, L("移动到文件夹", "Move to folder"), runtime->library.root);
            if (!destination.isEmpty()) transferLibraryItem(file, destination, false);
        });
        move->setEnabled(owned && !trash);
        menu.addSeparator();
        auto remove = menu.addAction(L("删除", "Delete"), this, [this, file] { deleteTheme(file); });
        remove->setEnabled(!trash && (!folder || owned)); remove->setObjectName("libraryDelete");
        auto purge = menu.addAction(L("彻底删除", "Delete permanently"), this, [this, file] { deleteTheme(file, true); });
        purge->setEnabled(owned); purge->setObjectName("libraryPurge");
        menu.addSeparator();
        menu.addAction(L("在文件管理器中显示", "Show in file manager"), this, [file] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).isDir() ? file : QFileInfo(file).absolutePath()));
        });
    }
    if (!runtime->library.recycled(directory)) {
        menu.addAction(L("新建主题", "New theme"), this, [this, directory] { libraryFolder = directory; newTheme(); });
        menu.addAction(L("新建文件夹", "New folder"), this, [this, directory] { newLibraryFolder(directory); })->setObjectName("libraryNewFolder");
        menu.addAction(L("导入主题…", "Import theme…"), this, [this, directory] { libraryFolder = directory; importTheme(); });
        auto paste = menu.addAction(L("粘贴", "Paste"), this, [this, directory] {
            transferLibraryItem(fileClipboard, directory, !fileCut);
            if (fileCut && !QFileInfo::exists(fileClipboard)) fileClipboard.clear();
        });
        paste->setEnabled(!fileClipboard.isEmpty() && QFileInfo::exists(fileClipboard)); paste->setObjectName("libraryPaste");
    }
    menu.addSeparator();
    menu.addAction(L("刷新", "Refresh"), this, [this] { reloadLibrary(); buildLibrary(); });
    menu.addAction(L("打开回收站", "Open recycle bin"), this, &MainWindow::restoreBuiltinThemes);
    menu.exec(global);
}
}
