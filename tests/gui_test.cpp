#include "mainwindow.h"
#include <QtTest>
#include <QtWidgets>
namespace Screen {
class GuiTest : public QObject {
    Q_OBJECT
private slots:
    void editingWorkflow() {
        QTemporaryDir dir;
        MainWindow w(dir.path());
        w.show();
        QTest::qWait(100);
        w.setEditing(true);
        w.addComponent("CPU");
        auto id = w.selection().first();
        w.layerName->setText("Renamed CPU");
        w.propertyEdited();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("Renamed CPU"));
        w.undo();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("CPU"));
        w.redo();
        QCOMPARE(findLayer(w.theme.layers(), id)["name"].toString(), QString("Renamed CPU"));
        w.selection({id});
        w.layerAction("duplicate");
        auto copy = w.selection().first();
        QVERIFY(copy != id);
        QVERIFY(findLayer(w.theme.layers(), copy)["kind"] == "group");
        w.selection({id, copy});
        w.layerAction("group");
        auto group = w.selection().first();
        QCOMPARE(findLayer(w.theme.layers(), group)["children"].toArray().size(), 2);
        auto at = absolutePosition(w.theme.layers(), copy);
        w.reparent({copy}, {}, -1);
        QCOMPARE(absolutePosition(w.theme.layers(), copy), at);
        w.selection({copy});
        w.lockedBox->setChecked(true);
        w.propertyEdited();
        auto before = w.theme.data;
        w.layerAction("delete");
        QCOMPARE(w.theme.data, before);
        w.lockedBox->setChecked(false);
        w.propertyEdited();
        w.layerAction("delete");
        QVERIFY(findLayer(w.theme.layers(), copy).isEmpty());
        w.undo();
        QVERIFY(!findLayer(w.theme.layers(), copy).isEmpty());
        const QString file = dir.path() + "/edited.json";
        w.theme.save(file);
        QCOMPARE(Theme::load(file).data, w.theme.data);
        w.dirty = false;
        w.quitting = true;
    }
    void nestedFoldersAndCycleProtection() {
        QTemporaryDir dir;
        MainWindow w(dir.path());
        w.setEditing(true);
        w.addComponent("CPU");
        auto a = w.selection().first();
        w.addComponent("RAM");
        auto b = w.selection().first();
        w.reparent({b}, a);
        QCOMPARE(parentOf(w.theme.layers(), b), a);
        auto before = w.theme.data;
        w.reparent({a}, b);
        QCOMPARE(w.theme.data, before);
        w.selection({a});
        w.layerAction("ungroup");
        QVERIFY(findLayer(w.theme.layers(), a).isEmpty());
        QVERIFY(!findLayer(w.theme.layers(), b).isEmpty());
        w.undo();
        QVERIFY(!findLayer(w.theme.layers(), a).isEmpty());
        w.dirty = false;
        w.quitting = true;
    }
    void pageIsolationPreferencesAndGeometry() {
        QTemporaryDir dir;
        {
            MainWindow w(dir.path());
            w.show();
            QTest::qWait(50);
            w.setPage(2);
            QCOMPARE(w.pages->currentIndex(), 2);
            QVERIFY(!w.left->isVisible());
            w.resize(1100, 720);
            QTest::qWait(50);
            QCOMPARE(w.size(), QSize(1100, 720));
            w.setPage(0);
            QVERIFY(w.left->isVisible());
            w.config["minimize_to_tray"] = true;
            w.config["interval_ms"] = 3000;
            w.config["language"] = "english";
            w.config["palette"] = "forest";
            w.persist();
            w.dirty = false;
            w.quitting = true;
        }
        MainWindow copy(dir.path());
        QVERIFY(copy.config["minimize_to_tray"].toBool());
        QCOMPARE(copy.config["interval_ms"].toInt(), 3000);
        QVERIFY(copy.english);
        QCOMPARE(copy.paletteName, QString("forest"));
        copy.quitting = true;
    }
    void repeatedSamplingDoesNotMutateTheme() {
        QTemporaryDir dir;
        MainWindow w(dir.path());
        auto before = w.theme.data;
        for (int i = 0; i < 3; ++i)
            w.tick();
        QCOMPARE(before, w.theme.data);
        QVERIFY(!w.sample.timestamp.isEmpty());
        w.quitting = true;
    }
    void ungroupPreservesOfficialPixels() {
        QTemporaryDir dir;
        MainWindow w(dir.path());
        w.choose(assets() + "/official/config/微星.data");
        auto before = w.renderer.render(w.theme, w.sample, true);
        QStringList groups;
        for (auto value : w.theme.layers()) {
            auto layer = value.toObject();
            if (layer["kind"] == "group")
                groups.append(layer["id"].toString());
        }
        QVERIFY(!groups.isEmpty());
        w.selection(groups);
        w.layerAction("ungroup");
        QCOMPARE(w.renderer.render(w.theme, w.sample, true), before);
        w.dirty = false;
        w.quitting = true;
    }
};
} // namespace Screen
QTEST_MAIN(Screen::GuiTest)
#include "gui_test.moc"
