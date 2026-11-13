#pragma once
#include <QJsonObject>
#include <QString>
namespace Screen {
// All filesystem changes are bounded to this library; imported originals stay outside it.
class ThemeLibrary {
public:
    QString root;
    void initialize(const QString &dataDir, QJsonObject &config);
    QString defaultThemeFile() const;
    bool contains(const QString &path) const;
    bool recycled(const QString &path) const;
    bool protectedFolder(const QString &path) const;
    QString resolve(const QString &path, const QString &dataDir = {}) const;
    QString unique(const QString &directory, const QString &name) const;
    QString createFolder(const QString &parent, const QString &name) const;
    QString transfer(const QString &source, const QString &directory, bool copy) const;
    QString recycle(const QString &source) const;
    QString restore(const QString &source) const;
    void purge(const QString &source) const;
    QString rename(const QString &source, const QString &name) const;
    QString renameTheme(const QString &source, const QString &name) const;
    static bool validName(const QString &name);
private:
    QJsonObject trashIndex() const;
    void saveTrashIndex(const QJsonObject &index) const;
    void requireItem(const QString &path) const;
};
}
