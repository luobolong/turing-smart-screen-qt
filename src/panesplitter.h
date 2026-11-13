#pragma once
#include <QHash>
#include <QSplitter>
#include <functional>
namespace Screen {
class PaneSplitter : public QSplitter {
public:
    explicit PaneSplitter(QWidget *parent = nullptr);
    void addCollapsiblePane(int index, const QString &name, bool collapsed = false);
    void setPaneCollapsed(int index, bool collapsed);
    bool paneCollapsed(int index) const;
    void setPaneOrientation(Qt::Orientation orientation, const QList<int> &sizes);
    std::function<void(int, bool)> collapsedChanged;
    QString showPaneText = "Show %1", hidePaneText = "Hide %1";
protected:
    QSplitterHandle *createHandle() override;
private:
    friend class PaneSplitterHandle;
    struct Pane { QString name; bool collapsed = false; int width = 280, height = 200; };
    QHash<int, Pane> panes;
    int controlledPane(QSplitterHandle *handle) const;
    void updateHandles();
    void synchronizePanes();
};
}
