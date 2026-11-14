#pragma once
#include "canvas.h"
#include "device.h"
#include "renderer.h"
#include "sensors.h"
#include "runtime.h"
#include "startup.h"
#include "panesplitter.h"
#include <memory>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>
class QListWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QTabWidget;
class QToolButton;
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
class QAction;
class QMenu;
namespace Screen {
class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class GuiTest;
    friend class PerformanceProbe;
    friend class ApplicationController;
    std::shared_ptr<DisplayRuntime> runtime;

public:
    explicit MainWindow(QString dataDir, QString themePath = {}, QWidget *parent = nullptr,
                        std::shared_ptr<StartupBackend> startupBackend = {});
    explicit MainWindow(std::shared_ptr<DisplayRuntime> runtime, QWidget *parent = nullptr,
                        std::shared_ptr<StartupBackend> startupBackend = {});
    ~MainWindow() override;
    Theme &theme;
    Snapshot &sample;
    void runSmoke(const QString &dir, bool stress = false, int seconds = 60);
signals:
    void lightweightRequested();
    void preferencesChanged();
    void closedToTray();
    void windowOpened();

protected:
    void closeEvent(QCloseEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

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
    QString dataDir, libraryFolder, fileClipboard, libraryQuery;
    bool fileCut = false;
    QJsonObject &config;
    QJsonObject saved;
    QString &path;
    bool english = false, editing = false, building = false, quitting = false, dirty = false;
    bool &sending;
    std::function<bool()> trayAvailable = QSystemTrayIcon::isSystemTrayAvailable;
    enum Page { LibraryPage, EditorPage, SensorsPage, SettingsPage, PageCount };
    int page = LibraryPage;
    bool sidebarExpanded = false;
    QVector<Entry> entries;
    QHash<QString, Document> drafts;
    QList<QJsonObject> undoHistory, redoHistory;
    QJsonArray clipboard;
    Renderer &renderer;
    QCache<QString, QPixmap> thumbnailCache{2 * 1024 * 1024};
    Collector &collector;
    Device &device;
    QImage &frame;
    QTimer &sampling;
    QTimer renderTimer;
    QWidget *root = nullptr, *titleBar = nullptr, *sidebar = nullptr, *devicePanel = nullptr, *libraryBrowser = nullptr,
            *left = nullptr, *right = nullptr;
    QStackedWidget *pages = nullptr;
    PaneSplitter *librarySplit = nullptr, *editorSplit = nullptr;
    QTabWidget *inspectorTabs = nullptr;
    QToolButton *sidebarToggle = nullptr, *sidebarBrand = nullptr;
    QTreeWidget *library = nullptr;
    QList<QTreeWidgetItem *> libraryItems;
    QTreeWidget *tree = nullptr, *folderTree = nullptr;
    QLineEdit *search = nullptr;
    QWidget *librarySearchPanel = nullptr;
    Canvas *canvas = nullptr;
    Canvas *previewCanvas = nullptr;
    QListWidget *navigation = nullptr;
    QLabel *pageTitle = nullptr, *libraryEmpty = nullptr, *editorThemeTitle = nullptr;
    QLabel *title = nullptr, *resolution = nullptr, *connection = nullptr, *status = nullptr,
           *sendState = nullptr, *dataState = nullptr;
    QPushButton *modeButton = nullptr, *connectButton = nullptr, *sendButton = nullptr,
                *saveButton = nullptr;
    QAction *deleteThemeAction = nullptr, *undoAction = nullptr, *redoAction = nullptr;
    QHash<QString, QAction *> layerActions;
    QComboBox *portBox = nullptr, *directionBox = nullptr, *bindingBox = nullptr;
    QSlider *brightness = nullptr;
    QLineEdit *layerName = nullptr, *layerText = nullptr;
    QSpinBox *xBox = nullptr, *yBox = nullptr, *wBox = nullptr, *hBox = nullptr, *fontBox = nullptr;
    QCheckBox *visibleBox = nullptr, *lockedBox = nullptr, *mirrorBox = nullptr;
    std::unique_ptr<StartupController> startup;
    QCheckBox *startupToggle = nullptr;
    QLabel *startupStatus = nullptr;
    QPushButton *fontColor = nullptr, *fillColor = nullptr;
    QComboBox *uiThemeBox = nullptr;
    QLabel *uiThemeStatus = nullptr;
    QString uiThemeError;
    QTableWidget *sensorTable = nullptr;
    QJsonArray resourceSamples;
    QElapsedTimer resourceClock;
    QString workloadStage = "idle";
    int workflowEdits = 0, workflowSwitches = 0;
    QString L(const char *zh, const char *en) const;
    void build();
    void buildLibraryPage(QWidget *page);
    void buildEditorPage(QWidget *page);
    void buildDevicePanel(QWidget *panel);
    void setSidebarExpanded(bool expanded);
    void updateResponsiveLayout();
    void buildSensors(QWidget *page);
    void updateSensorColumns();
    void buildSettings(QWidget *page);
    QCheckBox *collectionToggle(const QString &name);
    QWidget *zoomControls(Canvas *target, const QString &prefix);
    void reloadUiThemes();
    void selectUiTheme(const QString &id);
    void createUiTheme();
    void updateStartupUi();
    void configureWeather();
    void updateWeatherStatus();
    void buildLibrary();
    void buildFolderTree();
    void selectLibraryFolder(const QString &folder);
    void filterLibrary();
    void searchLibrary();
    void selectOpenedTheme();
    void expandSelectedThemeFolder();
    QString targetLibraryFolder() const;
    void libraryMenu(const QString &file, QPoint global);
    void newLibraryFolder(const QString &parent = {});
    void renameLibraryItem(const QString &file);
    bool renameLibraryPath(const QString &file, const QString &name, QString &error);
    void transferLibraryItem(const QString &file, const QString &directory, bool copy);
    void restoreLibraryItem(const QString &file);
    void rebindLibraryPaths(const QString &from, const QString &to);
    void reloadLibrary();
    void updateThemeActions();
    QString libraryKey(const QString &file) const;
    bool hiddenTheme(const QString &file) const;
    bool managedTheme(const QString &file) const;
    bool builtinTheme(const QString &file) const;
    void includeTheme(const QString &file);
    void deleteTheme(const QString &file, bool permanent = false);
    bool removeTheme(const QString &file, QString &error, bool permanent = false);
    void restoreBuiltinThemes();
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
    void populateLayerMenu(QMenu *menu);
    void updateLayerActions();
    void contextMenu(QPoint global);
    void reparent(const QStringList &ids, const QString &parent, int index = -1);
    void rename();
    void mutate(const std::function<void(Theme &)> &fn, bool rebuildLayers = true);
    void remember(QJsonObject before, bool rebuildLayers = true);
    void undo();
    void redo();
    void render();
    void tick();
    void refreshRuntimeUi();
    void updateStatus();
    void refreshPorts();
    void connectDevice();
    void toggleSend();
    void persist();
    QString orientationKey() const;
    QString orientationKey(const QString &file) const;
    void restoreThemeOrientation(bool migrateLegacy = false);
    void applyThemeOrientation(int orientation);
    void quit();
    bool confirmDiscard();
    QStringList selection() const;
    void selection(const QStringList &ids);
    void recordResource();
    void finishResources(const QString &dir, double seconds);
};
} // namespace Screen
