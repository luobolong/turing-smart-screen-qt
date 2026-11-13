#include "themelibrary.h"
#include "model.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryFile>
#include <stdexcept>
namespace Screen {
namespace {
void require(bool ok, const QString &error) { if (!ok) throw std::runtime_error(error.toUtf8().constData()); }
QString absolute(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
bool inside(const QString &path, const QString &directory) {
    if (path.isEmpty() || directory.isEmpty()) return false;
    const auto relative = QDir(absolute(directory)).relativeFilePath(absolute(path));
    if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) return false;
    // Reject links at every level, including links to locations inside the root.
    auto current = absolute(path);
    while (current != absolute(directory)) {
        if (QFileInfo(current).isSymLink()) return false;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current) return false;
        current = parent;
    }
    return !QFileInfo(directory).isSymLink();
}
void copyTree(const QString &source, const QString &target) {
    require(!QFileInfo(source).isSymLink(), "Symbolic links are not supported in the theme library.");
    if (QFileInfo(source).isDir()) {
        require(QDir().mkpath(target), "Cannot create folder: " + target);
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
            copyTree(entry.absoluteFilePath(), target + '/' + entry.fileName());
    } else require(QFile::copy(source, target), "Cannot copy file: " + source);
}
void verifyTree(const QString &source) {
    require(!QFileInfo(source).isSymLink(), "Symbolic links are not supported in the theme library.");
    if (QFileInfo(source).isDir())
        for (const auto &entry : QDir(source).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)) verifyTree(entry.absoluteFilePath());
}
bool moveItem(const QString &source, const QString &target) { return QDir().rename(source, target); }
}
bool ThemeLibrary::validName(const QString &name) {
    return !name.trimmed().isEmpty() && name == name.trimmed() && name != "." && name != ".." &&
        !name.startsWith('.') && !name.endsWith('.') && name.size() <= 120 &&
        !name.contains(QRegularExpression("[\\\\/:*?\"<>|\\x00-\\x1f]")) &&
        !QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption).match(name).hasMatch();
}
bool ThemeLibrary::contains(const QString &path) const { return inside(path, root); }
bool ThemeLibrary::recycled(const QString &path) const { return inside(path, root + "/deleted"); }
bool ThemeLibrary::protectedFolder(const QString &path) const {
    const auto p = absolute(path);
    return p == absolute(root) || p == absolute(root + "/official") || p == absolute(root + "/custom") || p == absolute(root + "/deleted");
}
void ThemeLibrary::requireItem(const QString &path) const {
    require(contains(path) && !protectedFolder(path) && !QFileInfo(path).fileName().startsWith('.') && QFileInfo::exists(path), "Choose a file or subfolder inside the theme library.");
    verifyTree(path);
}
QString ThemeLibrary::unique(const QString &directory, const QString &name) const {
    const QFileInfo info(name);
    auto target = directory + '/' + name;
    for (int n = 2; QFileInfo::exists(target); ++n)
        target = directory + '/' + info.completeBaseName() + QString(" (%1)").arg(n) + (info.suffix().isEmpty() ? QString() : '.' + info.suffix());
    return target;
}
QString ThemeLibrary::resolve(const QString &path, const QString &dataDir) const {
    if (path.isEmpty()) return {};
    const auto clean = QDir::fromNativeSeparators(path);
    QString relative;
    if (clean.contains("/assets/themes/")) relative = clean.mid(clean.indexOf("/assets/themes/") + 15);
    else if (clean.contains("/assets/official/")) relative = "official/" + clean.mid(clean.indexOf("/assets/official/") + 17);
    else if (inside(path, root)) relative = QDir(root).relativeFilePath(path);
    else if (!dataDir.isEmpty() && inside(path, dataDir + "/themes")) relative = QDir(dataDir + "/themes").relativeFilePath(path);
    if (relative == "nightfall.json") relative = "custom/nightfall.json";
    if (relative.startsWith("official/config/")) relative = "official/" + relative.mid(16);
    if (relative == "official/system overview.json") relative = "custom/system overview.json";
    if (!relative.isEmpty()) {
        auto target = root + '/' + relative;
        if (!QFileInfo::exists(target)) target = root + "/custom/" + relative;
        if (contains(target) && QFileInfo::exists(target)) return absolute(target);
    }
    return QFileInfo::exists(path) ? absolute(path) : path;
}
QString ThemeLibrary::defaultThemeFile() const {
    const auto personal = root + "/custom/system overview.json";
    if (!QFileInfo::exists(personal)) {
        if (!QFile::copy(assets() + "/themes/custom/system overview.json", personal)) Theme::starter().save(personal);
    }
    return personal;
}
void ThemeLibrary::initialize(const QString &dataDir, QJsonObject &config) {
    const auto source = assets() + "/themes";
    root = config["theme_library_root"].toString();
    if (root.isEmpty() && qEnvironmentVariableIsSet("TURING_QT_ISOLATED_LIBRARY")) root = dataDir + "/themes";
    if (QDir::fromNativeSeparators(root).contains("/assets/themes")) root = source;
    if (root.isEmpty()) {
        QTemporaryFile probe(source + "/.write-check-XXXXXX");
        root = probe.open() ? source : dataDir + "/themes";
    }
    root = absolute(root);
    require(QDir().mkpath(root + "/custom") && QDir().mkpath(root + "/official") && QDir().mkpath(root + "/deleted"), "Cannot create the theme library.");
    QHash<QString, QString> importedPaths;
    if (config["theme_library_version"].toInt() < 1) {
        if (absolute(source) != root && !config["theme_library_seeded"].toBool()) {
            for (const auto &folder : {QString("official"), QString("custom")})
                for (const auto &entry : QDir(source + '/' + folder).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                    const auto target = root + '/' + folder + '/' + entry.fileName();
                    if (!QFileInfo::exists(target)) copyTree(entry.absoluteFilePath(), target);
                }
        }
        // Preserve old personal themes and the old recovery folder without overwriting files.
        for (const auto &directory : {dataDir + "/themes", dataDir + "/deleted-themes"}) {
            for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
                if (entry.fileName().startsWith('.') || (absolute(directory) == root &&
                    QStringList{"official", "custom", "deleted"}.contains(entry.fileName()))) continue;
                if (!entry.isDir() && entry.suffix() != "json" && entry.suffix() != "data") continue;
                const auto targetFolder = root + (directory.endsWith("deleted-themes") ? "/deleted" : "/custom");
                const auto target = unique(targetFolder, entry.fileName());
                require(moveItem(entry.absoluteFilePath(), target), "Cannot migrate theme: " + entry.absoluteFilePath());
                if (targetFolder.endsWith("/deleted")) { auto index = trashIndex(); index[QDir(root).relativeFilePath(target)] = "custom/" + entry.fileName(); saveTrashIndex(index); }
            }
        }
        QJsonArray external;
        for (const auto &value : config["external_themes"].toArray()) {
            const auto file = value.toString(), mapped = resolve(file, dataDir);
            if (contains(mapped)) continue;
            try {
                const auto name = QFileInfo(file).completeBaseName();
                const auto target = unique(root + "/custom", (validName(name) ? name : uid()) + ".json");
                Theme::load(file).save(target);
                importedPaths[file] = target;
                if (config["selected_theme"].toString() == file) config["selected_theme"] = target;
            } catch (const std::exception &) { external.append(value); }
        }
        config["external_themes"] = external;
        QJsonArray hidden;
        for (const auto &value : config["hidden_themes"].toArray()) {
            const auto key = value.toString();
            if (!key.startsWith("builtin:")) { hidden.append(value); continue; }
            auto relative = key.mid(8);
            const auto old = relative.startsWith("official/") || relative.startsWith("themes/")
                ? assets() + '/' + relative : assets() + "/themes/" + relative;
            const auto target = resolve(old, dataDir);
            if (contains(target) && QFileInfo::exists(target) && !protectedFolder(target)) recycle(target);
        }
        config["hidden_themes"] = hidden;
        config["theme_library_version"] = 1;
    }
    config["theme_library_root"] = root;
    const auto oldOverview = root + "/official/system overview.json";
    if (QFileInfo::exists(oldOverview)) {
        // Preserve both copies if a user already has a custom overview.
        const auto target = unique(root + "/custom", "system overview.json");
        requireItem(oldOverview);
        require(moveItem(oldOverview, target), "Cannot migrate theme: " + oldOverview);
        importedPaths[oldOverview] = target;
        if (config["selected_theme"].toString() == oldOverview) config["selected_theme"] = target;
    }
    if (config["theme_folder"].toString() == "official/config" && !QDir(root + "/official/config").exists())
        config["theme_folder"] = "official";
    config["selected_theme"] = resolve(config["selected_theme"].toString(), dataDir);
    const auto directions = config["theme_orientations"].toObject();
    auto migratedDirections = directions;
    for (auto it = directions.constBegin(); it != directions.constEnd(); ++it) {
        const auto old = it.key();
        auto mapped = importedPaths.value(old, resolve(old, dataDir));
        if (old == "__starter__") mapped = defaultThemeFile();
        if (old.startsWith("builtin:")) {
            auto relative = old.mid(8);
            if (relative.startsWith("themes/")) relative = relative.mid(7);
            mapped = resolve(assets() + "/themes/" + relative, dataDir);
        }
        if (QFileInfo::exists(mapped)) {
            const auto relative = QDir(assets() + "/themes").relativeFilePath(mapped);
            const auto key = relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative) ? "builtin:" + relative : absolute(mapped);
            if (key != old) { migratedDirections[key] = it.value(); migratedDirections.remove(old); }
        }
    }
    config["theme_orientations"] = migratedDirections;
}
QString ThemeLibrary::createFolder(const QString &parent, const QString &name) const {
    require(contains(parent) && !recycled(parent) && QFileInfo(parent).isDir(), "Choose a folder outside the recycle bin.");
    require(validName(name), "The folder name is invalid.");
    const auto target = parent + '/' + name;
    require(!QFileInfo::exists(target) && QDir().mkdir(target), "The folder already exists or cannot be created.");
    return target;
}
QString ThemeLibrary::transfer(const QString &source, const QString &directory, bool copy) const {
    requireItem(source);
    require(!recycled(source) && contains(directory) && !recycled(directory) && QFileInfo(directory).isDir(), "Choose a destination outside the recycle bin.");
    require(!inside(directory, source), "A folder cannot be moved or copied into itself.");
    if (!copy && absolute(directory) == QFileInfo(source).absolutePath()) return source;
    auto target = unique(directory, QFileInfo(source).fileName());
    if (copy) {
        try { copyTree(source, target); } catch (...) { if (QFileInfo(target).isDir()) QDir(target).removeRecursively(); else QFile::remove(target); throw; }
    } else require(moveItem(source, target), "Cannot move the selected item.");
    return target;
}
QJsonObject ThemeLibrary::trashIndex() const {
    QFile file(root + "/.recycle.json");
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
void ThemeLibrary::saveTrashIndex(const QJsonObject &index) const {
    QSaveFile file(root + "/.recycle.json");
    const auto bytes = QJsonDocument(index).toJson();
    const bool saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    require(saved, "Cannot save recycle-bin information: " + file.errorString());
}
QString ThemeLibrary::recycle(const QString &source) const {
    requireItem(source);
    require(!recycled(source), "This item is already in the recycle bin.");
    const auto target = unique(root + "/deleted", QFileInfo(source).fileName());
    require(moveItem(source, target), "Cannot move the item into the recycle bin.");
    auto index = trashIndex(); index[QDir(root).relativeFilePath(target)] = QDir(root).relativeFilePath(source);
    try { saveTrashIndex(index); } catch (...) { moveItem(target, source); throw; }
    return target;
}
QString ThemeLibrary::restore(const QString &source) const {
    requireItem(source);
    require(recycled(source), "Choose an item in the recycle bin.");
    auto index = trashIndex();
    const auto relative = QDir(root).relativeFilePath(source);
    auto original = root + "/custom/" + QFileInfo(source).fileName();
    for (auto i = index.constBegin(); i != index.constEnd(); ++i)
        if (relative == i.key() || relative.startsWith(i.key() + '/')) {
            auto location = i.value().toString() + relative.mid(i.key().size());
            if (location.startsWith("official/config/") && !QDir(root + "/official/config").exists()) location = "official/" + location.mid(16);
            original = root + '/' + location; break;
        }
    require(contains(original) && !recycled(original) && !protectedFolder(original), "The original folder is invalid.");
    require(QDir().mkpath(QFileInfo(original).absolutePath()), "Cannot recreate the original folder.");
    const auto target = unique(QFileInfo(original).absolutePath(), QFileInfo(original).fileName());
    require(moveItem(source, target), "Cannot restore the item.");
    index.remove(relative);
    try { saveTrashIndex(index); } catch (...) { moveItem(target, source); throw; }
    return target;
}
void ThemeLibrary::purge(const QString &source) const {
    requireItem(source);
    auto index = trashIndex();
    const auto relative = QDir(root).relativeFilePath(source);
    for (const auto &key : index.keys()) if (key == relative || key.startsWith(relative + '/')) index.remove(key);
    const auto previous = trashIndex(); saveTrashIndex(index);
    if (!(QFileInfo(source).isDir() ? QDir(source).removeRecursively() : QFile::remove(source))) {
        saveTrashIndex(previous); require(false, "Cannot permanently delete the item.");
    }
}
QString ThemeLibrary::rename(const QString &source, const QString &name) const {
    requireItem(source);
    require(!recycled(source) && validName(name), "The name is invalid or the item is in the recycle bin.");
    const auto target = QFileInfo(source).absolutePath() + '/' + name;
    if (absolute(source) == absolute(target)) return source;
    require(!QFileInfo::exists(target), "An item with that name already exists.");
    require(moveItem(source, target), "Cannot rename the item.");
    return target;
}
QString ThemeLibrary::renameTheme(const QString &source, const QString &name) const {
    requireItem(source);
    require(!recycled(source) && validName(name), "The theme name is invalid or the theme is in the recycle bin.");
    if (source.endsWith(".data", Qt::CaseInsensitive)) return rename(source, name + ".data");
    auto theme = Theme::load(source);
    theme.data["name"] = name;
    const auto target = QFileInfo(source).absolutePath() + '/' + name + ".json";
    const bool same = absolute(target) == absolute(source);
    require(same || !QFileInfo::exists(target), "A theme with that filename already exists.");
    theme.save(target);
    if (!same && !QFile::remove(source)) {
        QFile::remove(target);
        require(false, "Cannot rename the original theme file.");
    }
    return target;
}
}
