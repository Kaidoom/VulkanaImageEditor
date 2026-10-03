#pragma once
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/ShapeResize.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/Measurement.hpp"

#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/LocalBlurStroke.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/SpotHealStroke.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/FreehandSelectionPath.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include "imageeditor/core/AnchoredLassoPath.hpp"
#include "imageeditor/core/LiveWire.hpp"
#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/ColorSelection.hpp"
#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/SelectedPixelTransform.hpp"
#include "imageeditor/core/LayerCropSession.hpp"
#include "imageeditor/core/ShapeRenderService.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"
#include "imageeditor/ui/BrushPresetStore.hpp"
#include "imageeditor/ui/UiLayoutConfig.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/RecentFiles.hpp"
#include "imageeditor/ui/EditorShortcuts.hpp"
#include "imageeditor/ui/DocumentContext.hpp"

#include <QMainWindow>
#include <QPointer>
#include <QStringList>

#include <cstdint>
#include <array>
#include <deque>
#include <future>
#include <map>
#include <optional>
#include <vector>

class QAction;
class QActionGroup;
class QButtonGroup;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QToolBar;
class QVulkanInstance;
class QTimer;
class QMenu;
class QProgressDialog;
class QNetworkAccessManager;
class QTabBar;
class QMimeData;
namespace imageeditor::core { enum class VisibilityOperation; }

namespace imageeditor::render {
class CanvasWindow;
}

namespace imageeditor::ui {

class LayerListModel;
class BrushOptionsPage;
class CloningOptionsPage;
class LocalBlurOptionsPage;
class ColorPanel;
class AdjustmentsPanel;
class FiltersPanel;
class EffectsPanel;
class ColorSelector;
class CompactValueControl;
class OverlayDockWorkspace;
class PropertiesPanel;
class CrossWindowPointerRouter;
class ToolOptionsBar;
class ToolOptionsNumber;
class ToolOptionsButton;
class TransformOptionsPage;
class CropOptionsPage;
class ShapeOptionsPage;
class WorkspacePanel;
class WorkspaceDialog;
class UpdateService;
class FeedbackService;
class TextController;
class PixelPreview;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QVulkanInstance* vulkanInstance,
        bool persistWindowState = true, bool showCanvasFps = false,
        UiLayoutConfig uiLayoutConfig = {}, QWidget* parent = nullptr);
    ~MainWindow() override;
    [[nodiscard]] const core::EditorSession& editorSession() const noexcept { return session(); }
    [[nodiscard]] std::size_t documentCount() const noexcept { return documents_.size(); }
    [[nodiscard]] DocumentInstanceId activeDocumentId() const noexcept { return activeDocument_ ? activeDocument_->id : 0; }
    [[nodiscard]] std::vector<DocumentInstanceId> documentIds() const;
    struct DocumentMemory { std::uint64_t sourceBytes{}, derivedBytes{}, historyBytes{}; };
    [[nodiscard]] DocumentMemory documentMemory() const;
    [[nodiscard]] const DocumentContext* documentContext(DocumentInstanceId) const;
    bool activateDocument(DocumentInstanceId);
    bool closeDocument(DocumentInstanceId);
    bool openImageFromPath(const QString& filePath);
    // User-scoped single-instance IPC: activate and defer file opening until
    // transactions/dialogs are safe. Relative paths are resolved by the sender.
    void receiveExternalLaunch(const QStringList& files);
    bool saveDocument(bool saveAs = false);
    // Preflight every document before starting the verified replacement. The
    // launch seam is used by tests; production always starts the exact path.
    bool restartAfterUpdate(const QString& path,
        std::function<bool(const QString&, const QStringList&)> launch = {});
    void exportImage(bool again = false);
    [[nodiscard]] const QString& projectPath() const noexcept { return fileState().projectPath; }
    enum class UnsavedChoice { Save, Discard, Cancel };
    // Deterministic automation seams; normal application defaults always prompt.
    struct FileInteractions {
        std::function<UnsavedChoice()> askUnsaved;
        std::function<QString()> chooseSavePath;
        std::function<bool(const QString&)> confirmReplace;
        std::function<void(const QString&)> reportError;
        ProjectProgress progress;
    };
    void setFileInteractions(FileInteractions hooks) { fileInteractions_ = std::move(hooks); }
    void setUnsavedPromptEnabled(bool enabled) { unsavedPromptEnabled_ = enabled; }
    bool importImageAsLayerFromPath(const QString& filePath);
    void showStartupDocument();
    // Called once for the launch's primary window; extra document windows do
    // not start more requests. Transport injection keeps startup tests offline.
    void startStartupFlow(bool showNewDocument, bool forceUpdateCheck = false,
        QNetworkAccessManager* transport = nullptr);
    void logRendererDiagnostics() const;
    void runIntegrationSmokeTest();
    [[nodiscard]] bool integrationSmokePassed() const noexcept;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    enum class ToolRailDockLocation {
        Left,
        RightPanel,
        Top,
        Bottom,
    };

    void createActions();
    void createMenus();
    void createToolOptionsBar();
    void createSelectionActions();
    void createSelectionControls();
    QWidget* createColorSelectionControls(QWidget* parent);
    void createColorSelectionHelp();
    QWidget* createSmartSelectionControls(QWidget* parent);
    void createSmartSelectionHelp();
    bool beginSmartSelection(core::Vec2d, Qt::KeyboardModifiers);
    void moveSmartSelection(core::Vec2d);
    void finishSmartSelection();
    void changeSmartTolerance();
    void advanceSmartSelection();
    void startNextSmartStroke();
    void cancelSmartSelection(bool forget = true);
    void refreshSmartSelectionControls();
    bool smartInteractionActive() const noexcept {return smartSelectionEditing_ || smartQueuedInput_.has_value() || !smartQueuedStrokes_.empty();}
    bool beginColorSelection(core::Vec2d, Qt::KeyboardModifiers);
    void changeColorSelectionFuzziness();
    void finishColorSelection();
    void advanceColorSelection();
    void cancelColorSelection(bool forgetReference = true);
    void refreshColorSelectionControls();
    void createFillActions();
    void createFillControls();
    void createCloningControls();
    void pickCloneSource(core::Vec2d);
    void refreshCloningControls();
    bool beginCloneStroke(const core::NormalizedPointerSample&);
    bool moveCloneStroke(const core::NormalizedPointerSample&);
    bool endCloneStroke(const core::NormalizedPointerSample&);
    void cancelCloneStroke();
    bool beginSpotHealStroke(const core::NormalizedPointerSample&);
    bool moveSpotHealStroke(const core::NormalizedPointerSample&);
    bool endSpotHealStroke(const core::NormalizedPointerSample&);
    void cancelSpotHeal(bool wait = false);
    bool spotHealBusyForActiveDocument() const;
    void refreshSpotHealOwnership();
    void cancelDocumentRepair(DocumentInstanceId id, bool wait = false);
    void pollSpotHeal();
    void refreshSpotHealPreview();
    bool routeSpotHealProcessingInput(QObject*, QEvent*);
    void createLocalBlurControls();
    bool beginLocalBlurStroke(const core::NormalizedPointerSample&);
    bool moveLocalBlurStroke(const core::NormalizedPointerSample&);
    bool endLocalBlurStroke(const core::NormalizedPointerSample&);
    void cancelLocalBlurStroke();
    void deferLocalBlurInput(QObject*, QEvent*);
    void flushDeferredLocalBlurInput();
    void createEyedropperControls();
    void createShapeControls();
    void prepareShapeCaches();
    void refreshShapeControls();
    void refreshShapeOverlay();
    void publishShapePreview();
    void moveShapeCreation(core::Vec2d);
    void finishShapeCreation(bool cancel);
    void closeShapePolygon(core::Vec2d);
    void removeShapeVertex();
    void changeShape(const core::ShapeLayer&);
    void finishShapeEdit(bool commit);
    void chooseShapeColor(bool fill);
    bool beginShapeManipulation(core::TransformHandle, core::Vec2d);
    bool updateShapeResize(core::Vec2d, core::TransformModifiers);
    void publishShapeResize();
    void finishShapeResize(bool commit);
    std::optional<core::LayerId> hitShape(core::Vec2d, bool activeOnly = false) const;
    std::optional<core::LayerId> hitMoveLayer(core::Vec2d) const;
    bool shapeLayerHit(const core::Layer&, core::Vec2d) const;
    void startFill(bool background, bool selectionOnly, core::Vec2d seed = {}, bool eraseSelection = false);
    void nudgeTarget(core::Vec2d delta);
    void advanceFill();
    void cancelFill();
    bool beginSelectionGesture(core::Vec2d, Qt::KeyboardModifiers);
    void moveSelectionGesture(core::Vec2d);
    void finishSelectionGesture(bool cancel);
    void advanceSelectionRasterization();
    void beginAnchoredLasso(core::Vec2d);
    void moveAnchoredLasso(core::Vec2d);
    void anchorLasso(core::Vec2d, bool manual);
    void closeLasso(core::Vec2d);
    void removeLassoAnchor();
    void continueLassoRequests();
    void publishLassoPreview();
    void advanceMagneticLasso();
    void commitAnchoredLasso();
    bool anchoredLassoActive() const;
    void runSelectionAction(int);
    bool canOpenSelectionAdjustments() const;
    void openSelectionAdjustments();
    void growSelection();
    void previewSelectionRotation(double);
    void finishSelectionRotation(bool commit);
    void finishSelectionNumericInput();
    void refreshSelectionControls();
    void updateSelectionUiModifiers(Qt::KeyboardModifiers modifiers);
    void updateEllipseConstraint(Qt::KeyboardModifiers modifiers);
    void refreshSelectionModeHighlight();
    [[nodiscard]] bool editorTextInputActive() const;
    void beginSelectionTransform();
    void beginSelectedPixelTransform();
    void flushSelectedPixelPreview();
    void refreshSelectionTransform();
    void setSelectionTransformValues(core::TransformValues);
    void completeSelectionTransformAction();
    void stepSelectionTransformHistory(bool redo);
    void finishSelectionTransform(bool apply);
    void createToolRail();
    void createDocks();
    void createAdjustmentsPanel();
    bool beginAdjustmentEdit(core::AdjustmentType);
    void previewAdjustmentEdit(core::AdjustmentState);
    void finishAdjustmentEdit(bool commit);
    void captureAdjustmentSelection(core::AdjustmentType);
    void refreshAdjustmentPanel();
    void createFiltersPanel();
    void createEffectsPanel();
    bool beginEffectEdit();
    void previewEffectEdit(core::LayerEffectState);
    void finishEffectEdit(bool commit);
    void chooseEffectColor(core::LayerEffectType,bool second);
    bool beginFilterEdit(core::SpatialFilterType);
    void previewFilterEdit(core::SpatialFilterState);
    void finishFilterEdit(bool commit);
    void captureFilterSelection(core::SpatialFilterType);
    void refreshFiltersPanel();
    void scheduleFilterPreparation();
    void advanceFilterPreparation();
    void cancelFilterPreparation(bool discard = false);
    void createStatusBar(bool showCanvasFps);
    void updateStatusNotification();
    void updateToolContextStatus();
    void createInitialDocument();
    void createNewDocument();
    void changeCanvasSize();
    void openImage();
    bool openDocumentFromPath(const QString&);
    bool importPdfFromPath(const QString&, bool intoCurrent);
    bool importPsdFromPath(const QString&, bool intoCurrent);
    bool settleForFileOperation();
    bool guardUnsavedChanges();
    void updateDocumentTitle();
    void refreshRecentMenu();
    void reportFileError(const QString&);
    [[nodiscard]] static QString documentOpenFilter();
    void importImageAsLayer();
    void pasteImageAsLayer();
    void handleDroppedImages(const QStringList& filePaths);
    void addRasterLayer();
    void deleteActiveLayer();
    void duplicateLayerItems();
    void createLayerOrganizationActions();
    void changeLayerVisibility(core::VisibilityOperation);
    void showLayerItemMenu(const QPoint&);
    void createLayerFolder(std::optional<core::LayerId> contextItem = std::nullopt);
    void addSelectionToNewFolder();
    void groupLayerItems();
    void dissolveLayerItem(core::LayerId);
    void removeLayerItem(core::LayerId, bool contents);
    void renameLayerItem(core::LayerId);
    bool setLayerItemName(core::LayerId, const QString&);
    void setLayerItemLabel(core::LayerId, core::ColorLabel);
    void mergeLayerItems();
    void rasterizeLayerItems();
    void createLayerMaskActions();
    void addLayerMask(bool fromSelection = false);
    void changeLayerMask(bool remove);
    void applyLayerMask();
    void layerMaskToSelection();
    void setLayerEditingTarget(int row, bool mask);
    void refreshLayerMaskActions();
    bool confirmLayerChange(const QString& title,const QString& question);
    bool commitLayerStructure(const QString&, core::LayerTree,
        std::vector<core::LayerId> removed, std::vector<core::Layer> added,
        core::LayerSelectionState afterSelection,
        std::vector<core::ItemVisibilityUpdate> retainedLeafVisibility = {});
    void setActiveTool(core::ToolId tool);
    void selectLayerFromRow(int row, Qt::KeyboardModifiers modifiers = {}, bool contextSelection = false);
    void collapseLayerSelectionOnEmptyClick();
    void focusLayerRenameEditor(QLineEdit* editor);
    void restoreLayerRenameFocus();
    void finishLayerRename(bool commit = true);
    QPointer<QLineEdit> layerRenameEditor_;
    void synchronizeUi(bool resetLayerModel, bool fitCanvas);
    void synchronizeLayerSelection();
    void updateLayerControls();
    void createMeasureControls();
    void refreshMeasureBounds();
    void refreshSelectionStatus();
    void refreshRulerView();
    void presentTemporaryMeasure(bool active);
    bool measureAccessHeld_ {false};
    int measureAccessKey_ {0}, panAccessKey_ {0};
    ShortcutBindings shortcuts_ = defaultShortcutBindings();
    QMap<QString, QAction*> shortcutActions_;
    QPointer<QWidget> keyboardPanelTarget_;
    bool forwardingPanelKey_ {false};
    void initializeShortcuts();
    void applyShortcutBindings(const ShortcutBindings&);
    bool routeEditorShortcut(QObject*, QEvent*);
    bool routePanelKeyboard(QObject*, QEvent*);
    bool shortcutGestureIdle(bool allowMenuCapture = false) const;
    QLabel* measureBoundsLabel_ {nullptr};
    std::optional<core::DocumentBounds> measureBounds_;
    core::Revision measureBoundsRevision_ {0};
    QAction* horizontalRulerAction_ {nullptr};
    QAction* verticalRulerAction_ {nullptr};
    void updateActionState();
    void updatePanelAreaVisibility();
    void beginToolRailDockDrag(QWidget* source);
    void createToolRailDockTargets();
    void showToolRailDockTargets();
    void hideToolRailDockTargets();
    void positionToolRailDockTargets();
    void setToolRailDockLocation(ToolRailDockLocation location);
    void updateToolRailPresentation();
    void updateStatusText();
    void mutateRasterForUploadProbe();
    bool beginBrushStroke(const core::NormalizedPointerSample& sample);
    bool moveBrushStroke(const core::NormalizedPointerSample& sample);
    bool endBrushStroke(const core::NormalizedPointerSample& sample);
    void cancelActiveBrushStroke(bool preserveCompletedRepair = false);
    void cancelPendingEdits();
    bool executeDocumentCommand(std::unique_ptr<core::Command> command);
    void beginLayerTransform();
    void createCropControls();
    void beginLayerCrop();
    void refreshLayerCrop();
    void finishLayerCrop(bool apply);
    void stepCropHistory(bool redo);
    void finishLayerTransform(bool apply);
    void refreshLayerTransform();
    bool beginLayerMove(core::Vec2d position, Qt::KeyboardModifiers modifiers);
    bool prepareDuplicateMove();
    bool finishCanvasOperation();
    bool runTransformAction(const std::function<bool()>& action);
    void finishLayerMove(bool apply);
    void previewMoveValues(const core::TransformValues& values);
    bool beginMoveNumericEdit();
    void flipMoveLayer(bool horizontal);
    void refreshMoveControls();
    void stepTransformHistory(bool redo);
    void adjustBrushSize(bool increase);
    void applyBrushSettings(const core::BrushSettings& settings);
    void setForegroundColor(core::Rgba8 color);
    void setColors(core::EditorColors colors);
    void registerEditorWindowAction(QAction* action);
    void smokeCheck(bool condition, const QString& failure);
    QAction* toolAction(core::ToolId tool, const QString& name,
        const QString& iconName, const QKeySequence& shortcut);

    core::EditorSession& session() noexcept { return fileState().session; }
    const core::EditorSession& session() const noexcept { return fileState().session; }
    DocumentContext& fileState() noexcept { return activeDocument_ ? *activeDocument_ : emptyDocument_; }
    const DocumentContext& fileState() const noexcept { return activeDocument_ ? *activeDocument_ : emptyDocument_; }
    std::vector<std::shared_ptr<DocumentContext>> documents_;
    std::shared_ptr<DocumentContext> activeDocument_;
    DocumentContext emptyDocument_;
    QTabBar* documentTabs_ {nullptr};
    QWidget* welcome_ {nullptr};
    bool switchingDocument_ {false};
    std::uint64_t documentActivationClock_ {0};
    bool initializeDocument(std::unique_ptr<core::Document>, QString name, QString path = {}, QJsonObject metadata = {}, QString source = {});
    bool publishDocuments(std::vector<std::shared_ptr<DocumentContext>>);
    void createDocumentTabs();
    void refreshDocumentTabs();
    bool settleForDocumentSwitch();
    void captureDocumentView();
    bool guardAllDocuments();
    void updateDocumentResources();
    void showEmptyWorkspace();
    QMimeData* captureLayerTransfer(std::span<const core::LayerId>, std::optional<core::Vec2d> = {});
    bool canReceiveLayerTransfer(const QMimeData*) const;
    bool receiveLayerTransfer(const QMimeData*, std::optional<core::Vec2d> = {}, std::optional<core::ItemPlacement> = {});
    bool routeDocumentDrag(QObject*, QEvent*);
    QTimer* tabHoverTimer_ {nullptr};
    DocumentInstanceId tabHoverTarget_ {0};
    QPointer<const QMimeData> tabHoverPayload_;
    core::Vec2d moveTransferGrab_;
    RecentFiles recentFiles_;
    FileInteractions fileInteractions_;
    bool fileBusy_ {false}, unsavedPromptEnabled_ {true};
    WorkspaceDialog* workspaceDialog_ {nullptr};
    UpdateService* updateService_ {nullptr};
    FeedbackService* feedbackService_ {nullptr};
    QTimer* startupUiTimer_ {nullptr};
    bool startupNewDocumentPending_ {false}, startupUpdateNoticePending_ {false};
    bool startupDocumentDialog_ {false}, externalOpenDraining_ {false};
    QStringList externalOpenFiles_;
    QTimer* externalOpenTimer_ {nullptr};
    void drainExternalLaunches();
    void presentStartupUi();
    void showPreferences();
    void showAbout();
    void showFeedback();
    void refreshThemeAppearance();
    QProgressDialog* fileProgress_ {nullptr};
    QMenu* recentMenu_ {nullptr};
    std::unique_ptr<TextController> textController_;
    ShapeOptionsPage* shapeOptionsPage_ {nullptr};
    core::ShapeKind shapeMode_ {core::ShapeKind::Rectangle};
    core::ShapeLayer shapeDefaults_;
    bool shapeDefaultColorsEdited_ {false};
    struct ShapeCreation {
        core::Layer layer;
        core::Vec2d start, pointer;
        std::vector<core::Vec2d> vertices;
        bool ratioLocked {false};
        bool previewDirty {true};
    };
    std::optional<ShapeCreation> shapeCreation_;
    struct ShapeEdit { core::LayerId id; core::ShapeLayer before; };
    std::optional<ShapeEdit> shapeEdit_;
    struct ShapeResizeEdit {
        core::LayerId id;
        core::ShapeResizeGesture gesture;
        core::Revision revision;
        core::AffineTransform currentTransform;
        std::shared_ptr<const core::LayerRenderCache> originalCache;
    };
    std::optional<ShapeResizeEdit> shapeResize_;
    QTimer* shapePreviewTimer_ {nullptr};
    QTimer* shapeDensityTimer_ {nullptr};
    double shapeViewScale_ {0};
    struct ShapeHitRecord {
        core::Revision revision {0};
        std::shared_ptr<const core::ShapeHitGeometry> geometry;
        std::size_t vertices {0};
        std::uint64_t lastUsed {0};
    };
    mutable std::map<core::LayerId,ShapeHitRecord> shapeHitCache_;
    mutable const core::Document* shapeHitDocument_ {nullptr};
    mutable std::uint64_t shapeHitClock_ {0};
    core::FillOptions fillOptions_;
    std::unique_ptr<core::FillOperation> activeFill_;
    std::array<QAction*,2> fillActions_ {};
    QWidget* fillProgress_ {nullptr};
    QTimer* fillTimer_ {nullptr};
    struct SelectionGesture {
        core::SelectionState before;
        core::Vec2d start;
        core::RectI rectangle;
        core::SelectionOperation operation;
        bool moving {false};
        core::Vec2d offset;
        core::BoundsSnapping snapping;
        bool snapMovementStarted {false};
        bool lasso {false};
        bool ellipseMode {false}, circleArmed {false}, circleConstrained {false};
        std::optional<core::EllipseGeometry> ellipse;
        core::FreehandSelectionPath path;
        std::shared_ptr<std::vector<core::SelectionEdge>> pathEdges;
        const core::Document* owner {nullptr};
        core::LassoMode mode {core::LassoMode::Freehand};
        core::AnchoredLassoPath anchored;
        core::FreehandSelectionPath guide;
        std::vector<core::Vec2d> live, closing, previousLive;
        core::Vec2d pointer;
        std::optional<core::Vec2d> deferredPointer;
        struct AnchorRequest { core::Vec2d point; bool manual, close; };
        std::deque<AnchorRequest> queuedAnchors;
        bool anchorRequested {false}, finishing {false}, solvingClosing {false};
        std::uint64_t requestedRevision {0}, searchRevision {0};
        std::size_t previewAnchoredEdges {0};
        std::unique_ptr<core::PinnedDocumentSampler> reference;
        std::unique_ptr<core::MagneticEdgeCache> edges;
        std::unique_ptr<core::LiveWireSearch> search;
    };
    std::optional<SelectionGesture> selectionGesture_;
    std::unique_ptr<core::SelectionCoverageRasterizer> selectionRasterizer_;
    QTimer* selectionRasterTimer_ {nullptr};
    QTimer* magneticTimer_ {nullptr};
    struct ColorSelectionContext {
        const core::Document* owner;
        std::optional<core::LayerId> layer;
        std::unique_ptr<core::ColorSelectionReference> reference;
        core::SelectionState original, expected;
        core::SelectionOperation operation;
        int appliedFuzziness;
        core::Vec2d seed;
        core::Revision revision;
        std::size_t nextLayer {0}, preparedPixels {0};
        std::vector<core::SampleCacheOverride> prepared;
        std::vector<std::pair<std::shared_ptr<const core::RasterSurface>,core::Revision>> pinnedPixels;
    };
    std::optional<ColorSelectionContext> colorSelection_;
    bool colorSelectionEditing_ {false}, colorSelectionFinishRequested_ {false};
    bool updatingColorSelection_ {false};
    int colorSelectionWorkerFuzziness_ {-1}, colorSelectionPreviewFuzziness_ {-1};
    core::ColorSelectionResult colorSelectionPreview_;
    core::ColorSampleSource colorSelectionSource_ {core::ColorSampleSource::MergedVisible};
    std::future<core::ColorSelectionResult> colorSelectionWorker_;
    std::shared_ptr<std::atomic_bool> colorSelectionCancelled_;
    QTimer* colorSelectionTimer_ {nullptr};
    QWidget* colorSelectionControls_ {nullptr};
    CompactValueControl* colorSelectionFuzziness_ {nullptr};
    QLabel* colorSelectionSample_ {nullptr};
    struct SmartSelectionContext {
        const core::Document* owner {nullptr};
        std::optional<core::LayerId> layer;
        core::Revision revision {}, expectedSelectionRevision {};
        std::unique_ptr<core::SmartSelectionReference> reference;
        std::size_t nextLayer {0}, preparedPixels {0};
        std::vector<core::SampleCacheOverride> prepared;
        std::vector<std::pair<std::shared_ptr<const core::RasterSurface>,core::Revision>> pinnedPixels;
        core::QuickSelectionHints hints, startingHints;
        core::SelectionState original;
        core::SelectionOperation operation {core::SelectionOperation::Add};
        core::Vec2d seed;
        core::QuickSelectionPath path;
        double edgeSensitivity {0.4};
        int appliedTolerance {16};
        bool hasClick {false};
    };
    std::optional<SmartSelectionContext> smartSelection_;
    core::SmartSelectMode smartMode_ {core::SmartSelectMode::QuickSelection};
    core::ColorSampleSource smartSource_ {core::ColorSampleSource::MergedVisible};
    bool smartSelectionEditing_ {false}, smartFinishRequested_ {false}, updatingSmart_ {false};
    bool smartEverActivated_ {false};
    std::uint64_t smartGeneration_ {0}, smartVersion_ {0}, smartWorkerGeneration_ {0}, smartWorkerVersion_ {0}, smartPreviewVersion_ {0};
    std::future<core::SmartSelectionResult> smartWorker_;
    std::shared_ptr<std::atomic_bool> smartCancelled_;
    core::SmartSelectionResult smartPreview_;
    core::NormalizedPointerSample smartPointerSample_;
    struct QueuedSmartStroke {
        core::QuickSelectionPath path;
        core::SelectionOperation operation {core::SelectionOperation::Add};
        core::Vec2d seed;
        double edgeSensitivity {0.4};
        int tolerance {16};
    };
    std::optional<QueuedSmartStroke> smartQueuedInput_;
    std::deque<QueuedSmartStroke> smartQueuedStrokes_;
    QTimer* smartTimer_ {nullptr};
    QWidget* smartControls_ {nullptr};
    QWidget* smartModeControls_ {nullptr};
    CompactValueControl* smartSize_ {nullptr};
    CompactValueControl* smartEdgeSensitivity_ {nullptr};
    CompactValueControl* smartTolerance_ {nullptr};
    QLabel* smartSample_ {nullptr};
    core::LassoMode lassoMode_ {core::LassoMode::Freehand};
    bool ellipseSelectMode_ {false};
    double magneticRadius_ {12};
    core::ColorSampleSource magneticSource_ {core::ColorSampleSource::MergedVisible};
    QWidget* lassoModeControls_ {nullptr};
    QWidget* magneticControls_ {nullptr};
    core::SelectionOperation selectionOperation_ {core::SelectionOperation::Replace};
    QButtonGroup* selectionModes_ {nullptr};
    Qt::KeyboardModifiers selectionUiModifiers_ {};
    std::array<QAction*,5> selectionActions_ {};
    QAction* selectionGrowAction_ {nullptr};
    struct SelectionRotation {
        core::SelectionState before, preview;
        core::Vec2d pivot;
    };
    std::optional<SelectionRotation> selectionRotation_;
    struct SelectionTransform {
        std::shared_ptr<DocumentContext> owner;
        std::unique_ptr<core::SelectedPixelTransformSession> pixels;
        core::SelectionState before;
        core::RectI bounds;
        core::TransformValues values, dragBefore;
        std::optional<core::TransformDrag> drag;
        core::BoundsSnapping snapping;
        core::SnapGuides pendingGuides;
        core::Vec2d dragPress;
        bool moving {false};
        bool snapMovementStarted {false};
        std::vector<core::TransformValues> checkpoints;
        std::size_t checkpoint {0};
        [[nodiscard]] core::Extent2u extent() const { return {std::uint32_t(bounds.width), std::uint32_t(bounds.height)}; }
        [[nodiscard]] core::AffineTransform matrix() const { return core::transformFromValues(values, extent()); }
        [[nodiscard]] core::AffineTransform mapping() const {
            return core::composeTransform(matrix(),{1,0,-double(bounds.x),0,1,-double(bounds.y)});
        }
    };
    std::optional<SelectionTransform> selectionTransform_;
    QAction* transformSelectionAction_ {nullptr};
    QAction* transformPixelsAction_ {nullptr};
    QTimer* selectedPixelPreviewTimer_ {nullptr};
    std::optional<core::TransformValues> queuedPixelTransform_;
    ToolOptionsNumber* selectionAngle_ {nullptr};
    std::array<ToolOptionsNumber*,2> selectionGrowth_ {};
    ToolOptionsButton* selectionGrowButton_ {nullptr};
    ToolOptionsButton* selectionAdjustmentsButton_ {nullptr};
    QWidget* selectionAdjustmentControls_ {nullptr};
    bool updatingSelectionControls_ {false};
    core::BrushSettings brushSettings_ {
        core::proceduralBrushPreset(core::ProceduralBrushPreset::PressureRound)};
    std::shared_ptr<BrushAssetLibrary> brushAssets_;
    BrushPresetStore brushPresetStore_;
    std::vector<core::BrushPresetRecord> brushPresets_;
    std::unique_ptr<core::LayerMaskEdit> activeMaskEdit_;
    std::unique_ptr<core::BasicPixelBrushStroke> activeBrushStroke_;
    std::unique_ptr<core::CloneStroke> activeCloneStroke_;
    std::optional<core::CloneAnchor> cloneAnchor_;
    core::CloneSettings cloneSettings_;
    CloningOptionsPage* cloningOptionsPage_ {nullptr};
    bool cloneProcessing_ {false}, cloneCancelRequested_ {false};
    struct SpotHealJob;
    std::shared_ptr<SpotHealJob> spotHealJob_;
    std::future<void> spotHealFuture_;
    std::unique_ptr<core::SpotHealStroke> activeSpotHealStroke_;
    QTimer* spotHealTimer_ {nullptr};
    bool spotHealPreviewDirty_ {false};
    double spotHealReferenceCaptureMilliseconds_ {0}; // Opt-in performance evidence only.
    std::unique_ptr<core::LocalBlurStroke> activeLocalBlurStroke_;
    core::BlurSettings localBlurSettings_;
    LocalBlurOptionsPage* localBlurOptionsPage_ {nullptr};
    bool localBlurProcessing_ {false}, localBlurCancelRequested_ {false};
    std::vector<std::pair<QPointer<QObject>, std::unique_ptr<QEvent>>> deferredLocalBlurInput_;
    std::unique_ptr<core::LayerTransformSession> layerTransform_;
    std::unique_ptr<core::LayerCropSession> layerCrop_;
    bool startingCrop_ {false};
    core::ToolId toolBeforeCrop_ {core::ToolId::Move};
    CropOptionsPage* cropOptionsPage_ {nullptr};
    std::unique_ptr<core::LayerTransformSession> activeLayerMove_;
    core::SnapOptions snappingOptions_;
    QAction* snappingAction_ {nullptr};
    QAction* snapCanvasAction_ {nullptr};
    QAction* snapLayersAction_ {nullptr};
    struct DuplicateMove {
        core::Vec2d press;
        core::LayerSelectionState before;
        std::unique_ptr<core::LayerStructureCommand> command;
        bool preparedForCommit {false};
    };
    std::optional<DuplicateMove> duplicateMove_;
    bool moveActiveOnly_ {false};
    bool collapseLayerSelectionOnEmptyClick_ {true};
    int shiftNudgePixels_ {10};
    bool suppressMoveNumeric_ {false};
    core::ToolId toolBeforeTransform_ {core::ToolId::Move};
    render::CanvasWindow* canvasWindow_ {nullptr};
    OverlayDockWorkspace* workspace_ {nullptr};
    QWidget* canvasContainer_ {nullptr};
    CrossWindowPointerRouter* pointerRouter_ {nullptr};
    ColorPanel* colorPanel_ {nullptr};
    AdjustmentsPanel* adjustmentsPanel_ {nullptr};
    std::unique_ptr<core::AdjustmentEditTransaction> adjustmentEdit_;
    bool finishingAdjustmentEdit_ {false};
    FiltersPanel* filtersPanel_ {nullptr};
    EffectsPanel* effectsPanel_ {nullptr};
    std::unique_ptr<core::EffectEditTransaction> effectEdit_;
    bool finishingEffectEdit_ {false};
    std::unique_ptr<core::FilterEditTransaction> filterEdit_;
    bool finishingFilterEdit_ {false};
    bool capturedFilterRegionActive_ {false};
    struct FilterJob;
    std::shared_ptr<FilterJob> filterJob_;
    std::shared_ptr<FilterJob> frozenFilterInput_;
    std::map<core::LayerId,std::shared_ptr<FilterJob>> failedFilterJobs_;
    std::future<void> filterFuture_;
    QTimer* filterPreparationTimer_ {nullptr};
    bool filterPreparationShuttingDown_ {false};
    ColorSelector* railColors_ {nullptr};
    LayerListModel* layerModel_ {nullptr};
    QListView* layerList_ {nullptr};
    QComboBox* blendModeCombo_ {nullptr};
    CompactValueControl* opacitySlider_ {nullptr};
    bool publishingOpacity_ {false};
    QPushButton* deleteLayerButton_ {nullptr};
    QPushButton* addMaskButton_ {nullptr};
    std::array<QAction*,6> maskActions_ {};
    PropertiesPanel* propertiesPanel_ {nullptr};
    ToolOptionsBar* toolOptionsBar_ {nullptr};
    ToolOptionsButton* editTextButton_ {nullptr};
    BrushOptionsPage* brushOptionsPage_ {nullptr};
    TransformOptionsPage* transformOptionsPage_ {nullptr};
    TransformOptionsPage* moveOptionsPage_ {nullptr};
    QToolBar* toolRail_ {nullptr};
    WorkspacePanel* layersPanelShell_ {nullptr};
    WorkspacePanel* propertiesPanelShell_ {nullptr};
    WorkspacePanel* colorPanelShell_ {nullptr};
    WorkspacePanel* adjustmentsPanelShell_ {nullptr};
    QLabel* documentStatus_ {nullptr};
    QLabel* selectionStatus_ {nullptr};
    QLabel* toolContextStatus_ {nullptr};
    QLabel* statusNotification_ {nullptr};
    QTimer* statusNotificationTimer_ {nullptr};
    int toolHintPosition_ {1};
    QLabel* zoomStatus_ {nullptr};
    QActionGroup* toolActions_ {nullptr};
    QAction* undoAction_ {nullptr};
    QAction* redoAction_ {nullptr};
    QAction* transformAction_ {nullptr};
    QAction* layerOutlinesAction_ {nullptr};
    QAction* deleteLayerAction_ {nullptr};
    QAction* duplicateLayersAction_ {nullptr};
    QAction* newFolderAction_ {nullptr};
    QAction* groupLayersAction_ {nullptr};
    QAction* ungroupLayersAction_ {nullptr};
    QAction* mergeLayersAction_ {nullptr};
    QAction* rasterizeLayersAction_ {nullptr};
    QAction* pixelPreviewAction_ {nullptr};
    std::unique_ptr<PixelPreview> pixelPreview_;
    QAction* renameLayerAction_ {nullptr};
    std::array<QAction*, 4> layerVisibilityActions_ {};
    QAction* layersPanelAction_ {nullptr};
    QAction* propertiesPanelAction_ {nullptr};
    QAction* colorPanelAction_ {nullptr};
    QAction* adjustmentsPanelAction_ {nullptr};
    QAction* eraserAction_ {nullptr};
    QActionGroup* toolRailDockActions_ {nullptr};
    QAction* toolRailLeftAction_ {nullptr};
    QAction* toolRailRightAction_ {nullptr};
    QAction* toolRailTopAction_ {nullptr};
    QAction* toolRailBottomAction_ {nullptr};
    QWidget* toolRailLeftTarget_ {nullptr};
    QWidget* toolRailRightTarget_ {nullptr};
    QWidget* toolRailTopTarget_ {nullptr};
    QWidget* toolRailBottomTarget_ {nullptr};
    std::map<core::ToolId, QAction*> toolActionMap_;
    std::uint64_t nextOpacityMergeKey_ {1};
    std::uint64_t activeOpacityMergeKey_ {0};
    QStringList smokeFailures_;
    core::Revision smokeBrushPaintedRevision_ {0};
    core::Revision smokeEraserRevision_ {0};
    std::optional<BrushAssetRuntimeStats> smokeBrushAssetBaseline_;
    bool persistWindowState_ {true};
    bool smokeCompleted_ {false};
    bool updatingUi_ {false};
    bool colorsSynchronized_ {false};
    ToolRailDockLocation toolRailDockLocation_ {ToolRailDockLocation::Left};
    std::optional<ToolRailDockLocation> pendingToolRailDockLocation_;
    UiLayoutConfig uiLayoutConfig_;
};

} // namespace imageeditor::ui
