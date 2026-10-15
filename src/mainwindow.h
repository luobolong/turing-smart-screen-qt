#pragma once
#include "canvas.h"
#include "device.h"
#include "renderer.h"
#include "sensors.h"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTabBar;
class QLabel;
class QPushButton;
class QComboBox;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QStackedWidget;
class QSplitter;
class QToolBar;
namespace Screen {
class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class GuiTest;

public:
    explicit MainWindow(QString dataDir, QString themePath = {}, QWidget *parent = nullptr);
    ~MainWindow() override;
    Theme theme;
    Snapshot sample;
    void runSmoke(const QString &dir, bool stress = false, int seconds = 60);
    QJsonObject diagnostics() const;

protected:
    void closeEvent(QCloseEvent *) override;

private:
    struct Entry {
        QString path, name;
        QSize size;
    };
    struct Document {
        Theme theme;
        QString path;
        QJsonObject saved;
        QList<QJsonObject> undo, redo;
    };
    QString dataDir;
    QJsonObject config, saved;
    QString path;
    bool english = false, editing = false, building = false, sending = false, quitting = false, dirty = false;
    int page = 0;
    QString paletteName = "graphite";
    QVector<Entry> entries;
    QHash<QString, Document> drafts;
    QList<QJsonObject> undoHistory, redoHistory;
    QJsonArray clipboard;
    Renderer renderer;
    Collector collector;
    Device device;
    QImage frame;
    QTimer sampling, renderTimer;
    QSystemTrayIcon *tray = nullptr;
    QWidget *root = nullptr, *left = nullptr, *right = nullptr;
    QStackedWidget *pages = nullptr, *leftPages = nullptr;
    QListWidget *library = nullptr;
    QTreeWidget *tree = nullptr;
    QLineEdit *search = nullptr;
    Canvas *canvas = nullptr;
    QTabBar *navigation = nullptr;
    QLabel *title = nullptr, *resolution = nullptr, *connection = nullptr, *status = nullptr,
           *sendState = nullptr, *dataState = nullptr;
    QPushButton *modeButton = nullptr, *connectButton = nullptr, *sendButton = nullptr, *undoButton = nullptr,
                *redoButton = nullptr;
    QComboBox *portBox = nullptr, *directionBox = nullptr, *bindingBox = nullptr;
    QSlider *brightness = nullptr;
    QLineEdit *layerName = nullptr, *layerText = nullptr;
    QSpinBox *xBox = nullptr, *yBox = nullptr, *wBox = nullptr, *hBox = nullptr, *fontBox = nullptr;
    QCheckBox *visibleBox = nullptr, *lockedBox = nullptr, *mirrorBox = nullptr;
    QPushButton *fontColor = nullptr, *fillColor = nullptr;
    QTableWidget *sensorTable = nullptr;
    QPlainTextEdit *diagnosticText = nullptr;
    QJsonArray resourceSamples;
    QElapsedTimer resourceClock;
    QString workloadStage = "idle";
    int workflowEdits = 0, workflowSwitches = 0;
    QString L(const char *zh, const char *en) const;
    void build();
    void applyPalette();
    void buildThemePage(QWidget *page);
    void buildSensors(QWidget *page);
    void buildSettings(QWidget *page);
    void buildDiagnostics(QWidget *page);
    void buildLibrary();
    void buildTree();
    void selectTree();
    void properties();
    void propertyEdited();
    void thumbnails();
    void setPage(int index);
    void setEditing(bool value);
    void choose(const QString &file);
    void newTheme();
    void importTheme();
    void saveTheme(bool as = false);
    void exportImage();
    void backgroundImage();
    void createLayer(const QString &kind, QPointF point = {24, 24}, QSizeF size = {100, 30});
    void addComponent(const QString &kind);
    void layerAction(const QString &action);
    void contextMenu(QPoint global);
    void reparent(const QStringList &ids, const QString &parent, int index = -1);
    void rename();
    void mutate(const std::function<void(Theme &)> &fn);
    void remember(QJsonObject before);
    void undo();
    void redo();
    void render();
    void tick();
    void updateStatus();
    void refreshPorts();
    void connectDevice();
    void toggleSend();
    void persist();
    void makeTray();
    void quit();
    bool confirmDiscard();
    QStringList selection() const;
    void selection(const QStringList &ids);
    void recordResource();
    void finishResources(const QString &dir, double seconds);
};
} // namespace Screen
