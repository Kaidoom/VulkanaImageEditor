#pragma once
#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/core/ViewportState.hpp"

#include <QByteArray>
#include <QPointer>
#include <QRegion>
#include <QSet>
#include <QWidget>

#include <vector>
#include <functional>

class QEvent;
class QHideEvent;
class QResizeEvent;
class QShowEvent;
class QSplitter;
class QTabBar;
class QStackedWidget;
class QToolBar;
class QWindow;

namespace imageeditor::render {
class CanvasWindow;
}

namespace imageeditor::ui {

class WorkspacePanel;
class RulerStrip;

// The Vulkan canvas permanently fills this widget. Every product panel lives
// on one stationary, independently alpha-backed native surface above it:
// docked edge columns and internally floating panels are ordinary QWidget
// children, never separate desktop windows.
class OverlayDockWorkspace final : public QWidget {
public:
    enum class PanelDockSide {
        Left,
        Right,
    };

    enum class PanelPlacement {
        DockedLeft,
        DockedRight,
        Floating,
    };

    enum class ToolRailPlacement {
        Left,
        Right,
        Top,
        Bottom,
    };

    explicit OverlayDockWorkspace(render::CanvasWindow* canvasWindow,
        QWidget* parent = nullptr);
    ~OverlayDockWorkspace() override;

    [[nodiscard]] QWidget* canvasContainer() const noexcept
    {
        return canvasContainer_;
    }
    [[nodiscard]] QWidget* panelOverlay() const noexcept { return panelOverlay_; }
    [[nodiscard]] QWidget* panelResizeHandle() const noexcept
    {
        return rightResizeHandle_;
    }
    [[nodiscard]] QWidget* leftPanelResizeHandle() const noexcept
    {
        return leftResizeHandle_;
    }
    [[nodiscard]] QWidget* leftPanelCard() const noexcept { return leftColumn_; }
    [[nodiscard]] QWidget* rightPanelCard() const noexcept { return rightColumn_; }
    [[nodiscard]] QWidget* panelDropIndicator() const noexcept
    {
        return panelDropIndicator_;
    }

    void addPanel(WorkspacePanel* panel,
        PanelDockSide side = PanelDockSide::Right);
    void dockPanel(WorkspacePanel* panel, PanelDockSide side, int index = -1);
    void floatPanel(WorkspacePanel* panel, const QRect& geometry = {});
    void setPanelVisible(WorkspacePanel* panel, bool visible);
    void activatePanel(WorkspacePanel* panel);
    // Temporary presentation only: retain requested visibility, tab membership,
    // current tabs and saved placement. Tools/rulers stay on the panel plane.
    void setPanelsSuppressed(bool suppressed);
    [[nodiscard]] bool panelsSuppressed() const noexcept { return panelsSuppressed_; }
    // Group frames participate in the existing dock/float layout. Individual
    // tabs remain the original panels; no control or native surface is rebuilt.
    void tabifyPanel(WorkspacePanel* panel, WorkspacePanel* target, int index = -1);
    void detachPanel(WorkspacePanel* panel);
    [[nodiscard]] std::vector<WorkspacePanel*> panelTabs(const WorkspacePanel*) const;
    [[nodiscard]] WorkspacePanel* panelFrame(const WorkspacePanel*) const;
    [[nodiscard]] QTabBar* panelTabBar(const WorkspacePanel*) const;
    [[nodiscard]] QByteArray savePanelTabs() const;
    bool restorePanelTabs(const QByteArray&);
    [[nodiscard]] bool panelVisible(const WorkspacePanel* panel) const;
    [[nodiscard]] PanelPlacement panelPlacement(const WorkspacePanel* panel) const;
    [[nodiscard]] int dockedPanelIndex(const WorkspacePanel* panel) const;
    [[nodiscard]] QRect floatingPanelGeometry(const WorkspacePanel* panel) const;
    [[nodiscard]] QByteArray saveDockedPanelSizes(PanelDockSide side) const;
    bool restoreDockedPanelSizes(
        PanelDockSide side, const QByteArray& state);

    void setToolRail(QToolBar* toolRail, ToolRailPlacement placement);
    void takeToolRail(QToolBar* toolRail);
    [[nodiscard]] bool hasToolRail(const QToolBar* toolRail) const noexcept;
    [[nodiscard]] ToolRailPlacement toolRailPlacement() const noexcept
    {
        return toolRailPlacement_;
    }

    [[nodiscard]] int panelWidth() const noexcept;
    void setPanelWidth(int width);
    [[nodiscard]] int leftPanelWidth() const noexcept;
    void setLeftPanelWidth(int width);
    void setTransientOverlayInteractionRegion(const QRegion& region);
    void setContextOverlayInteractionRegion(const QRegion& region);
    // One passive notification slot on the existing panel plane. It can keep
    // the plane visible without contributing any input footprint; modal cards
    // hide it temporarily and restore the still-requested notification later.
    // The caller owns content/geometry; overlay must be a child of panelOverlay().
    void setPassiveOverlay(QWidget* overlay, bool visible);
    // Interactive view-state badge below the top ruler, without canvas reflow.
    void setViewModeOverlay(QWidget* overlay, bool visible);
    void setWelcomeOverlay(QWidget*, bool);
    void setModalOverlay(QWidget* overlay);
    [[nodiscard]] QWidget* modalOverlay() const noexcept { return modalOverlay_.data(); }
    [[nodiscard]] bool hasModalOverlay() const noexcept { return !modalOverlay_.isNull(); }
    void setRulerVisible(Qt::Orientation axis, bool visible);
    [[nodiscard]] bool rulerVisible(Qt::Orientation axis) const;
    void setRulerFarEdge(Qt::Orientation axis, bool farEdge);
    [[nodiscard]] bool rulerFarEdge(Qt::Orientation axis) const;
    [[nodiscard]] QWidget* rulerStrip(Qt::Orientation axis) const;
    [[nodiscard]] QRect rulerContentRect() const;
    bool cancelRulerDrag();
    void setRulerView(core::ViewportState, core::Extent2d document, core::Extent2d viewport,
        std::optional<core::DocumentBounds>, std::optional<core::Vec2d> pointer);
    std::function<void()> onRulerPlacementChanged;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    struct TabGroup {
        WorkspacePanel* frame {};
        QTabBar* tabs {};
        QStackedWidget* stack {};
        std::vector<WorkspacePanel*> panels;
    };
    std::vector<TabGroup> tabGroups_;
    std::vector<WorkspacePanel*> registeredPanels_;
    [[nodiscard]] TabGroup* tabGroup(const WorkspacePanel*);
    [[nodiscard]] const TabGroup* tabGroup(const WorkspacePanel*) const;
    TabGroup& createTabGroup(WorkspacePanel*);
    void refreshTabGroup(TabGroup&, WorkspacePanel* active = nullptr);
    void collapseTabGroup(WorkspacePanel* frame);
    void clearTabCallbacks();
    struct TabDrop { WorkspacePanel* target {}; int index {-1}; int lineX {-1}; QRect area; };
    [[nodiscard]] TabDrop tabDropAt(QPoint overlayPosition) const;
    WorkspacePanel* draggedTab_ {};
    QRegion contextInteractionRegion_;
    QPointer<QWidget> passiveOverlay_;
    bool passiveOverlayRequestedVisible_ {false};
    QPointer<QWidget> viewModeOverlay_;
    bool viewModeOverlayRequestedVisible_ {false};
    QPointer<QWidget> modalOverlay_;
    QPointer<QWidget> welcomeOverlay_;
    RulerStrip* rulers_[2] {};
    QWidget* rulerDockHints_[2] {};
    bool rulerVisible_[2] {false, false}, rulerFarEdge_[2] {false, false};
    int draggingRuler_ {-1};
    void updateRulerGeometry();
    void updateRulerDrag(QPoint global);
    void finishRulerDrag(QPoint global, bool cancel);
    static constexpr int kDefaultPanelWidth = 310;
    static constexpr int kMinimumPanelWidth = 228;
    static constexpr int kMaximumPanelWidth = 640;
    static constexpr int kEmptyShelfWidth = 38;
    static constexpr int kEmptyDockDropWidth = 112;
    static constexpr int kToolRailWidth = 54;
    static constexpr int kResizeHandleWidth = 12;
    static constexpr int kResizeHandleHalfWidth = kResizeHandleWidth / 2;
    static constexpr int kPanelInset = 10;
    static constexpr int kMinimumCanvasExposure = 100;

    void configurePanel(WorkspacePanel* panel);
    void removePanelFromPlacements(WorkspacePanel* panel);
    void insertDockedPanel(WorkspacePanel* panel, PanelDockSide side, int index);
    [[nodiscard]] int panelIndex(
        const WorkspacePanel* panel, PanelPlacement placement) const;
    [[nodiscard]] bool hasVisibleDockedPanel(PanelDockSide side) const;
    [[nodiscard]] bool panelRequestedVisible(const WorkspacePanel* panel) const;
    [[nodiscard]] QRect clampFloatingGeometry(
        const WorkspacePanel* panel, const QRect& geometry) const;
    void clampFloatingPanels();

    void beginPanelDrag(WorkspacePanel* panel,
        QPoint pressGlobal, QPoint pressOffset);
    void movePanelDrag(WorkspacePanel* panel, QPoint globalPosition);
    void finishPanelDrag(WorkspacePanel* panel,
        QPoint globalPosition, bool canceled);
    void beginPanelResize(WorkspacePanel* panel, QPoint pressGlobal);
    void movePanelResize(WorkspacePanel* panel, QPoint globalPosition);
    void finishPanelResize(WorkspacePanel* panel, bool canceled);

    void ensurePanelSurfaceParent();
    void scheduleOverlayUpdate();
    void updateOverlayLayoutAndStacking();
    void updatePanelGeometry();
    void updateColumnContents();
    void updateDropHighlights(QPoint overlayPosition);
    [[nodiscard]] QRect dockDropZone(PanelDockSide side) const;
    [[nodiscard]] bool panelDragTouchesDropZone(
        PanelDockSide side, QPoint overlayPosition) const;
    [[nodiscard]] bool hasDockedDropPeers(PanelDockSide side) const;
    [[nodiscard]] bool isCurrentDockPosition(
        PanelDockSide side, int insertionIndex) const;
    [[nodiscard]] std::vector<int> dockInsertionGapYs(
        PanelDockSide side, const QRect& dropZone) const;
    [[nodiscard]] int closestDockInsertionSlot(
        PanelDockSide side, QPoint overlayPosition,
        const QRect& dropZone) const;
    [[nodiscard]] int dockInsertionIndex(
        PanelDockSide side, QPoint overlayPosition) const;
    [[nodiscard]] int dockInsertionLineY(
        PanelDockSide side, QPoint overlayPosition, const QRect& dropZone) const;
    [[nodiscard]] QRegion calculateInteractionFootprint() const;

    render::CanvasWindow* canvasWindow_ {nullptr};
    QWidget* canvasContainer_ {nullptr};
    QWidget* panelOverlay_ {nullptr};
    QWidget* leftColumn_ {nullptr};
    QWidget* rightColumn_ {nullptr};
    QWidget* leftResizeHandle_ {nullptr};
    QWidget* rightResizeHandle_ {nullptr};
    QWidget* panelDragProxy_ {nullptr};
    QWidget* panelDropIndicator_ {nullptr};
    QSplitter* leftSplitter_ {nullptr};
    QSplitter* rightSplitter_ {nullptr};
    QToolBar* toolRail_ {nullptr};
    ToolRailPlacement toolRailPlacement_ {ToolRailPlacement::Left};
    QPointer<QWindow> hostWindow_;
    std::vector<WorkspacePanel*> leftPanels_;
    std::vector<WorkspacePanel*> rightPanels_;
    std::vector<WorkspacePanel*> floatingPanels_;
    QSet<const WorkspacePanel*> hiddenPanels_;
    bool panelsSuppressed_ {false};
    QRegion transientInteractionRegion_;
    QRegion lastInteractionFootprint_;
    int requestedRightWidth_ {kDefaultPanelWidth};
    int requestedLeftWidth_ {kDefaultPanelWidth};
    int effectiveRightWidth_ {kDefaultPanelWidth};
    int effectiveLeftWidth_ {kDefaultPanelWidth};

    WorkspacePanel* draggedPanel_ {nullptr};
    PanelPlacement dragOrigin_ {PanelPlacement::Floating};
    int dragOriginIndex_ {-1};
    QRect dragOriginGeometry_;
    QPoint dragOffset_;
    WorkspacePanel* resizedPanel_ {nullptr};
    QPoint resizePressGlobal_;
    QSize resizeInitialSize_;
    bool overlayUpdatePending_ {false};
    bool panelGeometryUpdateInProgress_ {false};
    bool workspaceShown_ {false};
};

} // namespace imageeditor::ui
