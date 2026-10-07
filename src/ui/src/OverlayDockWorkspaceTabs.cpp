#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"
#include <QApplication>
#include <QCursor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace imageeditor::ui {
namespace {
// The same routed pointer gesture as panel headers, not a native QDrag/window.
// Reparenting is deferred by the workspace until the release has been delivered.
class PanelTabs final : public QTabBar {
public:
    explicit PanelTabs(QWidget* parent) : QTabBar(parent) {
        setObjectName("WorkspacePanelTabs");
        setExpanding(false);
        setDrawBase(false);
        setUsesScrollButtons(true);
        setElideMode(Qt::ElideRight);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(tr("Drag a tab to reorder, dock or float its panel"));
    }
    std::function<void(int, QPoint)> started;
    std::function<void(QPoint)> moved;
    std::function<void(QPoint, bool)> finished;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate
            || event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::TouchCancel)
            finish(QCursor::pos(), true);
        return QTabBar::event(event);
    }
    void mousePressEvent(QMouseEvent* event) override {
        QTabBar::mousePressEvent(event);
        if (event->button() != Qt::LeftButton) return;
        pressed_ = tabAt(event->position().toPoint());
        press_ = event->globalPosition().toPoint();
        dragging_ = false;
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (pressed_ < 0) { QTabBar::mouseMoveEvent(event); return; }
        const auto global = event->globalPosition().toPoint();
        if (!dragging_ && (global - press_).manhattanLength() >= QApplication::startDragDistance()) {
            dragging_ = true;
            if (started) started(pressed_, press_);
        }
        if (dragging_ && moved) moved(global);
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QTabBar::mouseReleaseEvent(event);
        if (event->button() == Qt::LeftButton) finish(event->globalPosition().toPoint(), false);
    }
private:
    void finish(QPoint global, bool cancel) {
        const bool dragging = dragging_;
        pressed_ = -1; dragging_ = false;
        if (dragging && finished) finished(global, cancel);
    }
    int pressed_ {-1};
    bool dragging_ {};
    QPoint press_;
};
}

const OverlayDockWorkspace::TabGroup* OverlayDockWorkspace::tabGroup(const WorkspacePanel* panel) const
{
    for (const auto& group : tabGroups_)
        if (group.frame == panel || std::find(group.panels.begin(), group.panels.end(), panel) != group.panels.end())
            return &group;
    return nullptr;
}
OverlayDockWorkspace::TabGroup* OverlayDockWorkspace::tabGroup(const WorkspacePanel* panel)
{
    return const_cast<TabGroup*>(std::as_const(*this).tabGroup(panel));
}
WorkspacePanel* OverlayDockWorkspace::panelFrame(const WorkspacePanel* panel) const
{
    if (const auto* group = tabGroup(panel)) return group->frame;
    return const_cast<WorkspacePanel*>(panel);
}
std::vector<WorkspacePanel*> OverlayDockWorkspace::panelTabs(const WorkspacePanel* panel) const
{
    if (const auto* group = tabGroup(panel)) return group->panels;
    return panel ? std::vector{const_cast<WorkspacePanel*>(panel)} : std::vector<WorkspacePanel*>{};
}
QTabBar* OverlayDockWorkspace::panelTabBar(const WorkspacePanel* panel) const
{
    const auto* group = tabGroup(panel);
    return group ? group->tabs : nullptr;
}
void OverlayDockWorkspace::clearTabCallbacks()
{
    for (auto& group : tabGroups_) {
        auto* tabs = static_cast<PanelTabs*>(group.tabs);
        tabs->started = {}; tabs->moved = {}; tabs->finished = {};
    }
}

OverlayDockWorkspace::TabGroup& OverlayDockWorkspace::createTabGroup(WorkspacePanel* target)
{
    const auto placement = panelPlacement(target);
    const auto geometry = target->geometry();
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    auto* tabs = new PanelTabs(body);
    auto* stack = new QStackedWidget(body);
    stack->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(tabs); layout->addWidget(stack, 1);
    auto* frame = new WorkspacePanel(target->title(), body, panelOverlay_);
    frame->setObjectName("WorkspacePanelGroup");
    tabGroups_.push_back({frame, tabs, stack, {target}});
    configurePanel(frame);
    for (auto* list : {&leftPanels_, &rightPanels_, &floatingPanels_})
        std::replace(list->begin(), list->end(), target, frame);
    if (placement != PanelPlacement::Floating) {
        auto* splitter = placement == PanelPlacement::DockedLeft ? leftSplitter_ : rightSplitter_;
        splitter->replaceWidget(splitter->indexOf(target), frame);
    } else frame->setGeometry(geometry);
    frame->setFloatingPresentation(placement == PanelPlacement::Floating);
    target->setFloatingPresentation(false);
    target->setTabbedPresentation(true);
    stack->addWidget(target);
    connect(tabs, &QTabBar::currentChanged, this, [this, frame](int index) {
        if (auto* group = tabGroup(frame); group && index >= 0 && index < int(group->panels.size())) {
            group->stack->setCurrentWidget(group->panels[size_t(index)]);
            frame->setTitle(group->panels[size_t(index)]->title());
            scheduleOverlayUpdate();
        }
    });
    tabs->started = [this, frame](int index, QPoint global) {
        auto* group = tabGroup(frame);
        if (draggedPanel_ || !group || index < 0 || index >= int(group->panels.size())) return;
        draggedTab_ = group->panels[size_t(index)];
        beginPanelDrag(frame, global, frame->mapFromGlobal(global));
    };
    tabs->moved = [this, frame](QPoint global) { movePanelDrag(frame, global); };
    tabs->finished = [this, frame](QPoint global, bool cancel) { finishPanelDrag(frame, global, cancel); };
    refreshTabGroup(tabGroups_.back(), target);
    return tabGroups_.back();
}

void OverlayDockWorkspace::refreshTabGroup(TabGroup& group, WorkspacePanel* active)
{
    if (!active) active = dynamic_cast<WorkspacePanel*>(group.stack->currentWidget());
    const QSignalBlocker blocker(group.tabs);
    while (group.tabs->count()) group.tabs->removeTab(0);
    int minimum = 0, maximum = 0, selected = -1;
    for (auto* panel : group.panels) {
        const int index = group.tabs->addTab(panel->title());
        const bool visible = panelRequestedVisible(panel);
        group.tabs->setTabVisible(index, visible);
        if (!visible) continue;
        if (selected < 0 || panel == active) selected = index;
        minimum = std::max(minimum, panel->configuredMinimumHeight());
        maximum = std::max(maximum, panel->configuredMaximumHeight());
    }
    if (selected >= 0) {
        group.tabs->setCurrentIndex(selected);
        group.stack->setCurrentWidget(group.panels[size_t(selected)]);
        group.frame->setTitle(group.panels[size_t(selected)]->title());
    }
    // A tab strip replaces the members' headers but the group keeps one handle.
    if (selected >= 0) {
        const int tabHeight = group.tabs->sizeHint().height();
        group.frame->setHeightRange(minimum + tabHeight,
            std::min(QWIDGETSIZE_MAX, std::max(minimum + tabHeight + 1, maximum + tabHeight)));
    } // Hiding every tab retains the frame's size for the next activation.
    group.frame->setVisible(selected >= 0);
    scheduleOverlayUpdate();
}

void OverlayDockWorkspace::activatePanel(WorkspacePanel* panel)
{
    setPanelVisible(panel, true);
    if (auto* group = tabGroup(panel)) refreshTabGroup(*group, panel);
    if (panel && panelPlacement(panel) == PanelPlacement::Floating) panelFrame(panel)->raise();
}

void OverlayDockWorkspace::collapseTabGroup(WorkspacePanel* frame)
{
    auto* group = tabGroup(frame);
    if (!group || group->panels.size() > 1) return;
    if (!group->panels.empty()) {
        auto* remaining = group->panels.front();
        const auto placement = panelPlacement(frame);
        const auto geometry = frame->geometry();
        group->stack->removeWidget(remaining);
        remaining->setParent(panelOverlay_);
        remaining->setTabbedPresentation(false);
        remaining->setFloatingPresentation(placement == PanelPlacement::Floating);
        for (auto* list : {&leftPanels_, &rightPanels_, &floatingPanels_})
            std::replace(list->begin(), list->end(), frame, remaining);
        if (placement != PanelPlacement::Floating) {
            auto* splitter = placement == PanelPlacement::DockedLeft ? leftSplitter_ : rightSplitter_;
            splitter->replaceWidget(splitter->indexOf(frame), remaining);
        } else remaining->setGeometry(clampFloatingGeometry(remaining, geometry));
        remaining->setVisible(panelRequestedVisible(remaining));
    } else removePanelFromPlacements(frame);
    auto* tabs = static_cast<PanelTabs*>(group->tabs);
    tabs->started = {}; tabs->moved = {}; tabs->finished = {};
    std::erase_if(tabGroups_, [frame](const TabGroup& g) { return g.frame == frame; });
    frame->hide();
    frame->setParent(panelOverlay_); // Remove the retired frame from its splitter now.
    frame->deleteLater();
}

void OverlayDockWorkspace::detachPanel(WorkspacePanel* panel)
{
    auto* group = tabGroup(panel);
    if (!group || group->frame == panel) return;
    auto* frame = group->frame;
    const QRect geometry(frame->mapTo(panelOverlay_, QPoint{}), frame->size());
    const QSignalBlocker blocker(group->tabs);
    group->stack->removeWidget(panel);
    std::erase(group->panels, panel);
    panel->setParent(panelOverlay_);
    panel->setTabbedPresentation(false);
    panel->setFloatingPresentation(true);
    floatingPanels_.push_back(panel);
    panel->setGeometry(clampFloatingGeometry(panel, geometry));
    panel->setVisible(panelRequestedVisible(panel));
    refreshTabGroup(*group);
    collapseTabGroup(frame);
    updatePanelGeometry();
}

void OverlayDockWorkspace::tabifyPanel(WorkspacePanel* panel, WorkspacePanel* target, int index)
{
    if (!panel || !target || panel == target) return;
    auto* sourceGroup = tabGroup(panel);
    auto* targetGroup = tabGroup(target);
    // Moving within one frame reorders tabs. The insertion index is a gap in
    // the pre-drop sequence, so removing a preceding tab shifts it left once.
    if (sourceGroup && sourceGroup == targetGroup) {
        if (panel == sourceGroup->frame) return;
        auto& members = sourceGroup->panels;
        const auto old = int(std::find(members.begin(), members.end(), panel) - members.begin());
        int next = index < 0 ? int(members.size()) : std::clamp(index, 0, int(members.size()));
        if (old < next) --next;
        members.erase(members.begin() + old);
        members.insert(members.begin() + next, panel);
        refreshTabGroup(*sourceGroup, panel);
        return;
    }
    const auto incoming = sourceGroup && sourceGroup->frame == panel
        ? sourceGroup->panels : std::vector{panel};
    // Keep the target frame stable while removing any source group.
    auto* targetFrame = targetGroup ? targetGroup->frame : createTabGroup(target).frame;
    int insertion = index < 0 ? int(tabGroup(targetFrame)->panels.size())
        : std::clamp(index, 0, int(tabGroup(targetFrame)->panels.size()));
    for (auto* member : incoming) {
        detachPanel(member);
        removePanelFromPlacements(member);
        member->setFloatingPresentation(false);
        member->setTabbedPresentation(true);
        auto* group = tabGroup(targetFrame);
        group->panels.insert(group->panels.begin() + insertion++, member);
        group->stack->addWidget(member);
    }
    refreshTabGroup(*tabGroup(targetFrame), incoming.front());
    updatePanelGeometry();
}

OverlayDockWorkspace::TabDrop OverlayDockWorkspace::tabDropAt(QPoint position) const
{
    std::vector<WorkspacePanel*> candidates = floatingPanels_;
    std::reverse(candidates.begin(), candidates.end());
    candidates.insert(candidates.end(), leftPanels_.begin(), leftPanels_.end());
    candidates.insert(candidates.end(), rightPanels_.begin(), rightPanels_.end());
    for (auto* frame : candidates) {
        if ((frame == draggedPanel_ && !draggedTab_) || !panelRequestedVisible(frame)) continue;
        const QRect area(frame->mapTo(panelOverlay_, QPoint{}), frame->size());
        if (!area.contains(position)) continue;
        if (const auto* group = tabGroup(frame)) {
            const auto local = group->tabs->mapFrom(panelOverlay_, position);
            if (group->tabs->rect().contains(local)) {
                int index = group->tabs->count(), line = 0;
                for (int i = 0; i < group->tabs->count(); ++i) {
                    if (!group->tabs->isTabVisible(i)) continue;
                    const auto tab = group->tabs->tabRect(i);
                    if (local.x() < tab.center().x()) { index = i; line = tab.left(); break; }
                    line = tab.right() + 1;
                }
                const QRect tabArea(group->tabs->mapTo(panelOverlay_, QPoint{}), group->tabs->size());
                return {frame, index, std::clamp(line, 2, std::max(2, tabArea.width() - 3)), tabArea};
            }
        }
        const int edge = std::clamp(area.height() / 4, 28, 64);
        if (position.y() >= area.top() + edge && position.y() <= area.bottom() - edge)
            return {frame, -1, -1, area};
        // A topmost floating panel occludes any panel behind it.
        return {};
    }
    return {};
}

QByteArray OverlayDockWorkspace::savePanelTabs() const
{
    QJsonArray groups;
    for (const auto& group : tabGroups_) {
        QJsonArray members;
        for (const auto* panel : group.panels) members.append(panel->objectName());
        auto* active = group.stack->currentWidget();
        const auto r = floatingPanelGeometry(group.frame);
        groups.append(QJsonObject{{"members", members}, {"active", active ? active->objectName() : QString()},
            {"geometry", QJsonArray{r.x(), r.y(), r.width(), r.height()}}});
    }
    return QJsonDocument(QJsonObject{{"version", 1}, {"groups", groups}}).toJson(QJsonDocument::Compact);
}

bool OverlayDockWorkspace::restorePanelTabs(const QByteArray& bytes)
{
    if (bytes.isEmpty() || !tabGroups_.empty()) return false;
    const auto root = QJsonDocument::fromJson(bytes).object();
    if (root.value("version").toInt() != 1 || !root.value("groups").isArray()) return false;
    struct Saved { std::vector<WorkspacePanel*> panels; WorkspacePanel* active {}; QRect geometry; };
    std::vector<Saved> saved;
    QSet<WorkspacePanel*> used;
    // Validate the entire layout before changing any ownership.
    for (const auto value : root.value("groups").toArray()) {
        const auto object = value.toObject();
        if (!object.value("members").isArray()) return false;
        Saved group;
        for (const auto name : object.value("members").toArray()) {
            const auto found = std::find_if(registeredPanels_.begin(), registeredPanels_.end(),
                [&](const WorkspacePanel* p) { return p->objectName() == name.toString(); });
            if (found == registeredPanels_.end() || used.contains(*found)) return false;
            used.insert(*found); group.panels.push_back(*found);
            if ((*found)->objectName() == object.value("active").toString()) group.active = *found;
        }
        if (group.panels.size() < 2 || !group.active) return false;
        const auto r = object.value("geometry").toArray();
        if (r.size() == 4) group.geometry = QRect(r[0].toInt(), r[1].toInt(), r[2].toInt(), r[3].toInt());
        saved.push_back(std::move(group));
    }
    for (auto& group : saved) {
        for (size_t i = 1; i < group.panels.size(); ++i) tabifyPanel(group.panels[i], group.panels.front());
        refreshTabGroup(*tabGroup(group.panels.front()), group.active);
        if (panelPlacement(group.panels.front()) == PanelPlacement::Floating && group.geometry.isValid())
            floatPanel(group.panels.front(), group.geometry);
    }
    return true;
}
} // namespace imageeditor::ui
