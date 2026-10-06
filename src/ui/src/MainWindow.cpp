#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/BlendModeCombo.hpp"
#include "imageeditor/ui/MemoryStatusLabel.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include "imageeditor/ui/PdfImport.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/EffectsPanel.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/AboutDialog.hpp"
#include "imageeditor/ui/FeedbackDialog.hpp"
#include "imageeditor/ui/FeedbackService.hpp"
#include "imageeditor/ui/ThirdPartyNoticesDialog.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"

#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/ColorPanel.hpp"
#include "imageeditor/ui/AdjustmentsPanel.hpp"
#include "imageeditor/ui/FiltersPanel.hpp"
#include "imageeditor/ui/ColorSelector.hpp"
#include "imageeditor/ui/ClipboardImage.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/BrushOptionsPage.hpp"
#include "imageeditor/ui/CloningOptionsPage.hpp"
#include "imageeditor/ui/LocalBlurOptionsPage.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include <QScopedValueRollback>
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/TransformOptionsPage.hpp"
#include "imageeditor/ui/CropOptionsPage.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/WorkspacePanel.hpp"

#include <QAction>
#include <QActionGroup>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QApplication>
#include <QCloseEvent>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDir>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QInputEvent>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QProgressDialog>
#include <QToolButton>
#include <QRegion>
#include <QSettings>
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include <QScreen>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QTabletEvent>
#include <QTouchEvent>
#include <QToolBar>
#include <QUrl>
#include <QVulkanInstance>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace imageeditor::ui {
namespace {

constexpr auto kToolRailMimeType = "application/x-imageeditor-tool-rail";

// Keep the full text for accessibility/hover, while long contextual names cannot
// force the status bar (and therefore the canvas) to grow wider.
class ElidedStatusLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        style()->drawItemText(&painter, contentsRect(), int(alignment()), palette(), isEnabled(),
            fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()), QPalette::WindowText);
    }
};

// Fixed lanes, measured from the whole status bar rather than the labels'
// changing size hints. Even the size grip/FPS/zoom cannot displace the center.
class StatusReadouts final : public QWidget {
public:
    StatusReadouts(QStatusBar* bar, QWidget* left, QWidget* center, QWidget* right)
        : QWidget(bar), bar_(bar), left_(left), center_(center), right_(right)
    {
        setObjectName(QStringLiteral("StatusReadouts"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        for (auto* child : {left_, center_, right_}) { child->setParent(this); child->show(); }
        bar_->installEventFilter(this);
    }
    QSize sizeHint() const override
    {
        return {0, std::max({left_->sizeHint().height(), center_->sizeHint().height(), right_->sizeHint().height()})};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); arrange(); }
    void moveEvent(QMoveEvent* event) override { QWidget::moveEvent(event); arrange(); }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == bar_ && event->type() == QEvent::Resize) arrange();
        return QWidget::eventFilter(watched,event);
    }
private:
    void arrange()
    {
        const auto offset = mapTo(bar_, QPoint{}).x();
        const int left = std::clamp(bar_->width()/3-offset, 0, width());
        const int right = std::clamp(bar_->width()-bar_->width()/3-offset, left, width());
        left_->setGeometry(0, 0, left, height());
        center_->setGeometry(left, 0, right-left, height());
        right_->setGeometry(right, 0, width()-right, height());
    }
    QStatusBar* bar_;
    QWidget* left_;
    QWidget* center_;
    QWidget* right_;
};

enum class RailDockEdge {
    Left,
    Right,
    Top,
    Bottom,
};

class ToolRailDragHandle final : public QWidget {
public:
    explicit ToolRailDragHandle(
        QToolBar* toolbar, std::function<void(QWidget*)> beginDrag)
        : QWidget(toolbar)
        , toolbar_(toolbar)
        , beginDrag_(std::move(beginDrag))
    {
        setObjectName(QStringLiteral("ToolRailDragHandle"));
        setCursor(Qt::OpenHandCursor);
        setToolTip(QStringLiteral("Drag to dock the tool rail"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        connect(toolbar_, &QToolBar::orientationChanged, this,
            [this](Qt::Orientation) {
                updateGeometry();
                update();
            });
    }

    [[nodiscard]] QSize sizeHint() const override
    {
        return toolbar_ && toolbar_->orientation() == Qt::Horizontal
            ? QSize {22, 36} : QSize {36, 22};
    }

    [[nodiscard]] QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    bool event(QEvent* event) override
    {
        if (event && (event->type() == QEvent::ApplicationDeactivate
                || event->type() == QEvent::WindowDeactivate
                || event->type() == QEvent::FocusOut
                || event->type() == QEvent::Hide
                || event->type() == QEvent::TouchCancel)) {
            pressed_ = false;
            dragStarted_ = false;
            setCursor(Qt::OpenHandCursor);
            update();
        }
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        pressed_ = true;
        dragStarted_ = false;
        pressPosition_ = event->position();
        setCursor(Qt::ClosedHandCursor);
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!pressed_ || dragStarted_) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        if ((event->position() - pressPosition_).manhattanLength()
            < QApplication::startDragDistance()) {
            event->accept();
            return;
        }

        dragStarted_ = true;
        if (beginDrag_) {
            beginDrag_(this);
        }
        pressed_ = false;
        dragStarted_ = false;
        setCursor(Qt::OpenHandCursor);
        update();
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            pressed_ = false;
            dragStarted_ = false;
            setCursor(Qt::OpenHandCursor);
            update();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pressed_
                ? themeTone("#30384D")
                : themeTone("#252A36"));
        painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 5, 5);

        painter.setBrush(themeTone(pressed_ ? "#C3C9FF" : "#858DA1"));
        const bool horizontal = toolbar_
            && toolbar_->orientation() == Qt::Horizontal;
        const QPoint center = rect().center();
        for (int major = -1; major <= 1; ++major) {
            for (int minor = -1; minor <= 1; minor += 2) {
                const QPoint offset = horizontal
                    ? QPoint {minor * 3, major * 4}
                    : QPoint {major * 4, minor * 3};
                painter.drawEllipse(QPointF(center + offset), 1.5, 1.5);
            }
        }
    }

private:
    QToolBar* toolbar_ {nullptr};
    std::function<void(QWidget*)> beginDrag_;
    QPointF pressPosition_;
    bool pressed_ {false};
    bool dragStarted_ {false};
};

class ToolRailDockTarget final : public QWidget {
public:
    ToolRailDockTarget(RailDockEdge edge, QString label,
        std::function<void()> dropped, QWidget* parent)
        : QWidget(parent)
        , edge_(edge)
        , label_(std::move(label))
        , dropped_(std::move(dropped))
    {
        setAcceptDrops(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_StyledBackground, false);
        hide();
    }

    void resetHighlight()
    {
        highlighted_ = false;
        update();
    }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (!event->mimeData()->hasFormat(QLatin1String(kToolRailMimeType))) {
            event->ignore();
            return;
        }
        highlighted_ = true;
        update();
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->mimeData()->hasFormat(QLatin1String(kToolRailMimeType))) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
            return;
        }
        event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        highlighted_ = false;
        update();
        event->accept();
    }

    void dropEvent(QDropEvent* event) override
    {
        highlighted_ = false;
        update();
        if (!event->mimeData()->hasFormat(QLatin1String(kToolRailMimeType))) {
            event->ignore();
            return;
        }
        if (dropped_) {
            dropped_();
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void paintEvent(QPaintEvent* event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(highlighted_
                ? themeTone("#AAB4FF")
                : themeTone("#6577F3"),
            highlighted_ ? 2.0 : 1.25));
        painter.setBrush(themeTone(highlighted_ ? "#3A4665" : "#252B3A"));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 9, 9);

        const QRect iconRect(12, (height() - 25) / 2, 25, 25);
        painter.setPen(QPen(themeTone("#8D9AF8"), 1.25));
        painter.setBrush(themeTone("#171A22"));
        painter.drawRoundedRect(iconRect, 4, 4);
        painter.setPen(Qt::NoPen);
        painter.setBrush(themeTone("#8D9AF8"));
        QRect rail = iconRect.adjusted(4, 4, -4, -4);
        switch (edge_) {
        case RailDockEdge::Left: rail.setWidth(4); break;
        case RailDockEdge::Right: rail.setLeft(rail.right() - 3); break;
        case RailDockEdge::Top: rail.setHeight(4); break;
        case RailDockEdge::Bottom: rail.setTop(rail.bottom() - 3); break;
        }
        painter.drawRoundedRect(rail, 2, 2);

        QFont labelFont = font();
        labelFont.setPointSizeF(8.0);
        labelFont.setWeight(QFont::DemiBold);
        painter.setFont(labelFont);
        painter.setPen(themeTone("#EEF0F7"));
        painter.drawText(QRect(44, 0, width() - 52, height()),
            Qt::AlignVCenter | Qt::AlignLeft, label_);
    }

private:
    RailDockEdge edge_;
    QString label_;
    std::function<void()> dropped_;
    bool highlighted_ {false};
};

class CanvasFpsLabel final : public QLabel {
public:
    explicit CanvasFpsLabel(render::CanvasWindow& canvas, QWidget* parent = nullptr)
        : QLabel(parent)
        , canvas_(canvas)
        , lastFrameCount_(canvas.rendererStats().framesSubmitted)
    {
        setObjectName(QStringLiteral("CanvasFpsStatus"));
        setText(QStringLiteral("0.0 FPS"));
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setMinimumWidth(70);
        setToolTip(QStringLiteral(
            "Actual Vulkan canvas submissions over the last sample window. "
            "The demand-driven canvas should read 0 FPS while idle."));
        setAccessibleName(QStringLiteral("Canvas frame rate"));

        sampleClock_.start();
        sampleTimer_.setInterval(500);
        sampleTimer_.setTimerType(Qt::CoarseTimer);
        connect(&sampleTimer_, &QTimer::timeout, this, [this] { sample(); });
        sampleTimer_.start();
    }

private:
    void sample()
    {
        const qint64 elapsedMilliseconds = sampleClock_.restart();
        const std::uint64_t currentFrameCount =
            canvas_.rendererStats().framesSubmitted;
        const std::uint64_t submittedFrames = currentFrameCount >= lastFrameCount_
            ? currentFrameCount - lastFrameCount_ : 0;
        lastFrameCount_ = currentFrameCount;
        const double framesPerSecond = elapsedMilliseconds > 0
            ? static_cast<double>(submittedFrames) * 1000.0
                / static_cast<double>(elapsedMilliseconds)
            : 0.0;
        setText(QStringLiteral("%1 FPS").arg(framesPerSecond, 0, 'f', 1));
    }

    render::CanvasWindow& canvas_;
    QTimer sampleTimer_;
    QElapsedTimer sampleClock_;
    std::uint64_t lastFrameCount_ {0};
};

QString supportedImageFilter()
{
    QStringList patterns {QStringLiteral("*.pdf"),QStringLiteral("*.PDF"),QStringLiteral("*.psd"),QStringLiteral("*.PSD"),QStringLiteral("*.psb"),QStringLiteral("*.PSB")};
    for (const auto& format : QImageReader::supportedImageFormats()) {
        const auto extension = QString::fromLatin1(format);
        patterns << QStringLiteral("*.") + extension << QStringLiteral("*.") + extension.toUpper();
    }
    patterns.removeDuplicates();
    patterns.sort();
    return QStringLiteral("Images and PDF (%1);;All files (*)").arg(patterns.join(QChar(' ')));
}

QString toolName(core::ToolId tool)
{
    switch (tool) {
    case core::ToolId::Move: return QStringLiteral("Move");
    case core::ToolId::Marquee: return QStringLiteral("Select");
    case core::ToolId::Lasso: return QStringLiteral("Lasso");
    case core::ToolId::Brush: return QStringLiteral("Brush");
    case core::ToolId::Cloning: return QStringLiteral("Cloning");
    case core::ToolId::LocalBlur: return QStringLiteral("Local Blur");
    case core::ToolId::Eraser: return QStringLiteral("Eraser");
    case core::ToolId::Fill: return QStringLiteral("Fill");
    case core::ToolId::Eyedropper: return QStringLiteral("Eyedropper");
    case core::ToolId::Text: return QStringLiteral("Text");
    case core::ToolId::Shape: return QStringLiteral("Shape");
    case core::ToolId::Measure: return QStringLiteral("Measure");
    case core::ToolId::SelectByColor: return QStringLiteral("Select by Color");
    case core::ToolId::SmartSelect: return QStringLiteral("Smart Select");
    case core::ToolId::Transform: return QStringLiteral("Transform");
    case core::ToolId::Crop: return QStringLiteral("Layer Crop");
    }
    return {};
}

std::vector<core::BrushPresetRecord> presentationBrushPresets(
    const BrushAssetLibrary& assets)
{
    const auto builtins = core::builtinBrushPresets();
    std::vector<core::BrushPresetRecord> result {
        builtins.begin(), builtins.end()};
    const auto tipAlreadyRepresented = [&result](std::string_view tipId) {
        return std::any_of(result.begin(), result.end(),
            [tipId](const core::BrushPresetRecord& preset) {
                return preset.settings.tip.assetId == tipId;
            });
    };

    // Every packaged tip remains directly reachable after the old component
    // picker disappears. These are complete immutable presets, not partial
    // tip mutations: selecting one cannot accidentally inherit Dry Ink grain
    // or flow from the previously selected brush.
    for (const auto& component : assets.components(core::BrushAssetType::Tip)) {
        if (tipAlreadyRepresented(component.id)) {
            continue;
        }
        auto settings = core::proceduralBrushPreset(
            core::ProceduralBrushPreset::PressureRound);
        settings.tip.assetId = component.id;
        settings.tip.aspectRatio = 1.0;
        settings.tip.angleDegrees
            = component.defaultRotationDegrees.value_or(0.0);
        settings.tip.rotationMode = core::BrushTipRotationMode::FollowStrokeDirection;
        settings.grain.assetId = core::BrushAssetIds::NoGrain;
        settings.grain.strength = 0.0;
        result.push_back({
            "builtin.preset.asset." + component.id,
            component.displayName.toStdString(), std::move(settings)});
    }
    return result;
}

} // namespace

MainWindow::MainWindow(
    QVulkanInstance* vulkanInstance, bool persistWindowState,
    bool showCanvasFps, UiLayoutConfig uiLayoutConfig, QWidget* parent,
    PanelLayoutMode panelLayoutMode)
    : QMainWindow(parent)
    , brushAssets_(BrushAssetLibrary::createPackaged())
    , canvasWindow_(new render::CanvasWindow)
    , layerModel_(new LayerListModel(this))
    , toolActions_(new QActionGroup(this))
    , persistWindowState_(persistWindowState)
    , persistPanelLayout_(persistWindowState && panelLayoutMode == PanelLayoutMode::Saved)
    , uiLayoutConfig_(std::move(uiLayoutConfig))
{
    setObjectName(QStringLiteral("ImageEditorMainWindow"));
    setWindowTitle(QStringLiteral("Vulkana Image Editor"));
    resize(1500, 940);
    setMinimumSize(980, 640);
    setAcceptDrops(true);
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks
        | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    brushPresets_ = presentationBrushPresets(*brushAssets_);
    QStringList brushDiagnostics = brushAssets_->diagnostics();
    const auto userPresets = brushPresetStore_.loadOnce(brushPresets_,
        &brushDiagnostics);
    brushPresets_.insert(brushPresets_.end(),
        userPresets.begin(), userPresets.end());
    for (const auto& diagnostic : brushDiagnostics) {
        qWarning().noquote() << "Brush runtime:" << diagnostic;
    }

    canvasWindow_->setVulkanInstance(vulkanInstance);
    refreshThemeAppearance();
    auto* content = new QWidget(this);
    content->setObjectName(QStringLiteral("EditorContent"));
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);
    workspace_ = new OverlayDockWorkspace(canvasWindow_, content);
    canvasContainer_ = workspace_->canvasContainer();
    contentLayout->addWidget(workspace_, 1);
    setCentralWidget(content);
    pointerRouter_ = new CrossWindowPointerRouter(
        this, canvasWindow_, canvasContainer_, this);
    pointerRouter_->setObjectName(QStringLiteral("CrossWindowPointerRouter"));

    createActions();
    createToolOptionsBar();
    createToolRail();
    createDocks();
    createMeasureControls();
    createColorSelectionHelp();
    createSmartSelectionHelp();
    createShapeControls();
    createCropControls();
    createMenus();
    createStatusBar(showCanvasFps);
    textController_ = std::make_unique<TextController>(session(), canvasWindow_, workspace_, this);
    textController_->onChanged = [this] { synchronizeUi(true, false); };
    textController_->onActivateText = [this] { setActiveTool(core::ToolId::Text); };
    textController_->onError = [this](QString message) {
        statusBar()->showMessage(message, 6500);
    };
    canvasWindow_->onTextPressed = [this](core::Vec2d p, Qt::KeyboardModifiers m, bool twice) {
        if(!textController_->active()) {
            const auto hit=hitMoveLayer(p);
            const auto* container=hit?session().document()->tree().container(*hit):nullptr;
            if(container && core::isGroup(container->kind)) {
                session().setActiveLayer(*hit);synchronizeUi(false,false);
                statusBar()->showMessage(QStringLiteral("Group selected · Ungroup or explicitly select a child to edit its text."),4000);
                return false;
            }
        }
        return textController_->press(p, m, twice);
    };
    canvasWindow_->onTextDragged = [this](core::Vec2d p) { textController_->drag(p); };
    canvasWindow_->onTextEvent = [this](QEvent* e) { return textController_->inputEvent(e); };
    canvasWindow_->onViewportUpdated = [this] {
        textController_->viewportChanged();
        const double scale=canvasWindow_->zoom()*canvasWindow_->devicePixelRatio();
        if(scale!=shapeViewScale_) {shapeViewScale_=scale;shapeDensityTimer_->start();}
    };
    canvasWindow_->onViewStateChanged = [this] { refreshRulerView(); };
    canvasWindow_->onTemporaryMeasureChanged = [this](bool active) { presentTemporaryMeasure(active); };

    layerModel_->setSession(&session());
    layerModel_->onVisibilityChanged = [this](core::LayerId layerId, bool visible) {
        if (executeDocumentCommand(std::make_unique<core::SetLayerVisibilityCommand>(layerId, visible))) {
            fileState().untouched = false;
            synchronizeUi(false, false);
            return true;
        }
        return false;
    };
    layerModel_->onMoveRequested = [this](core::LayerId layerId,
                                           std::size_t destinationIndex) {
        if (!executeDocumentCommand(std::make_unique<core::MoveLayerCommand>(
                layerId, destinationIndex))) {
            return false;
        }
        fileState().untouched = false;
        synchronizeUi(true, false);
        return true;
    };
    layerModel_->onReparentRequested=[this](std::vector<core::LayerId> ids,core::ItemPlacement destination) {
        if(!session().document() || !settleForFileOperation())return false;
        auto tree=session().document()->tree();
        if(!tree.reparent(ids,destination))return true; // A same-slot drop is a no-op, not an error.
        return commitLayerStructure(QStringLiteral("Organize layers"),std::move(tree),{},{},session().layerSelectionState());
    };
    layerModel_->onExpansionChanged=[this] {synchronizeLayerSelection();};
    layerModel_->onRenameRequested=[this](core::LayerId id,const QString& name) {return setLayerItemName(id,name);};
    canvasWindow_->onZoomChanged = [this](double) { updateStatusText(); };
    canvasWindow_->onStatusMessage = [this](const QString& message) {
        statusBar()->showMessage(message, 4500);
    };
    canvasWindow_->onFilesDropped = [this](const QStringList& paths) {
        handleDroppedImages(paths);
    };
    canvasWindow_->onBrushStrokeBegan = [this](const core::NormalizedPointerSample& sample) {
        return beginBrushStroke(sample);
    };
    canvasWindow_->onBrushStrokeMoved = [this](const core::NormalizedPointerSample& sample) {
        return moveBrushStroke(sample);
    };
    canvasWindow_->onBrushStrokeEnded = [this](const core::NormalizedPointerSample& sample) {
        return endBrushStroke(sample);
    };
    canvasWindow_->onBrushStrokeCancelled = [this] {
        cancelActiveBrushStroke();
    };
    canvasWindow_->onColorSampleRequested = [this](core::Vec2d position) {
        return session().document()
            ? core::sampleDocumentColor(*session().document(), session().activeLayer(),
                position, session().colorSampleSource()) : core::ColorSample {};
    };
    canvasWindow_->onColorPicked = [this](core::Rgba8 color) { setForegroundColor(color); };
    propertiesPanel_->onBrushSettingsChanged = [this](const core::BrushSettings& settings) {
        applyBrushSettings(settings);
    };
    brushOptionsPage_->onBrushSettingsChanged = [this](
        const core::BrushSettings& settings) {
        applyBrushSettings(settings);
    };
    transformOptionsPage_->onValuesChanged = [this](const core::TransformValues& values) {
        if (selectionTransform_) { setSelectionTransformValues(values); return; }
        if (layerTransform_ && layerTransform_->setValues(values)) refreshLayerTransform();
    };
    transformOptionsPage_->onFlip = [this](bool horizontal) {
        if (selectionTransform_) {
            auto values = selectionTransform_->values;
            (horizontal ? values.scaleX : values.scaleY) *= -1;
            setSelectionTransformValues(values);
            completeSelectionTransformAction();
            return;
        }
        if (layerTransform_ && runTransformAction([&] { return layerTransform_->flip(horizontal); })) refreshLayerTransform();
        updateActionState();
    };
    transformOptionsPage_->onNumericActionFinished = [this] {
        if (selectionTransform_) { completeSelectionTransformAction(); return; }
        if (layerTransform_) {
            (void)runTransformAction([&] { return layerTransform_->completeAction(); });
            refreshLayerTransform();
            updateActionState();
        }
    };
    transformOptionsPage_->onApply = [this] {
        if (selectionTransform_) finishSelectionTransform(true); else finishLayerTransform(true);
    };
    transformOptionsPage_->onCancel = [this] {
        if (selectionTransform_) finishSelectionTransform(false); else finishLayerTransform(false);
    };
    moveOptionsPage_->onValuesChanged = [this](const core::TransformValues& values) {
        previewMoveValues(values);
    };
    moveOptionsPage_->onNumericActionFinished = [this] {
        if (!suppressMoveNumeric_ && activeLayerMove_ && !activeLayerMove_->dragging())
            finishLayerMove(true);
    };
    moveOptionsPage_->onFlip = [this](bool horizontal) { flipMoveLayer(horizontal); };
    moveOptionsPage_->onActiveLayerOnlyChanged = [this](bool checked) { moveActiveOnly_ = checked; };
    canvasWindow_->onFinishRequested = [this] { return finishCanvasOperation(); };
    canvasWindow_->onOutsideDocumentPressed = [this] {
        collapseLayerSelectionOnEmptyClick();
        canvasWindow_->dismissLayerOutlines();
    };
    canvasWindow_->onTransformBegan = [this](core::TransformHandle handle, core::Vec2d position) {
        if(layerCrop_) {cropOptionsPage_->finishNumericInput();const bool ok=runTransformAction([&]{return layerCrop_->beginDrag(handle,position,cropOptionsPage_->chamferEnabled());});updateActionState();return ok;}
        if (selectionTransform_) {
            completeSelectionTransformAction();
            auto& edit = *selectionTransform_;
            edit.dragBefore = edit.values;
            edit.drag.emplace(edit.extent(), edit.matrix(), edit.values, handle, position);
            edit.dragPress=position;edit.moving=handle==core::TransformHandle::Move;
            edit.snapMovementStarted=false;
            if(edit.moving || (handle >= core::TransformHandle::TopLeft
                && handle <= core::TransformHandle::Left && int(handle) % 2 == 1)) {
                const auto h=core::transformHandles(edit.matrix(),edit.extent());
                core::DocumentBounds bounds{h[0],h[0]};
                for(auto p:h){bounds.minimum.x=std::min(bounds.minimum.x,p.x);bounds.minimum.y=std::min(bounds.minimum.y,p.y);
                    bounds.maximum.x=std::max(bounds.maximum.x,p.x);bounds.maximum.y=std::max(bounds.maximum.y,p.y);}
                if (edit.pixels) {
                    const auto id=edit.pixels->layerId();edit.snapping.begin(*session().document(),bounds,std::span(&id,1));
                } else edit.snapping.begin(*session().document(),bounds,{});
            }
            updateActionState();
            return true;
        }
        if (!layerTransform_) return runTransformAction([&] { return session().activeTool()==core::ToolId::Shape
            ? beginShapeManipulation(handle,position) : beginLayerMove(position,canvasWindow_->pointerModifiers()); });
        const bool began = runTransformAction([&] { return layerTransform_->beginDrag(handle,position); });
        updateActionState();
        return began;
    };
    canvasWindow_->onTransformMoved = [this](core::Vec2d position, core::TransformModifiers modifiers) {
        if(layerCrop_){const bool ok=runTransformAction([&]{return layerCrop_->dragTo(position,modifiers,cropOptionsPage_->aspectLocked());});if(ok)refreshLayerCrop();return ok;}
        if (shapeResize_) return updateShapeResize(position, modifiers);
        if (selectionTransform_ && selectionTransform_->drag) {
            auto& edit = *selectionTransform_;
            edit.pendingGuides={};
            if(edit.moving && (edit.snapMovementStarted || position != edit.dragPress)){
                edit.snapMovementStarted=true;
                auto options=snappingOptions_;options.logicalScale=canvasWindow_->zoom();options.bypass|=modifiers.control;
                const auto snap=edit.snapping.resolve(*session().document(),position-edit.dragPress,options);
                position=edit.dragPress+snap.translation;edit.pendingGuides=snap.guides;
                if(modifiers.control)canvasWindow_->setSnapGuides({});
            }
            auto values = edit.drag->resolve(position, modifiers, transformOptionsPage_->aspectLocked(), edit.values);
            if (!values) {canvasWindow_->setSnapGuides({});statusBar()->showMessage(tr("Invalid transform — keep corners convex and away from a projective horizon"),2500);return true;}
            if (!edit.moving && position != edit.dragPress) {
                auto options=snappingOptions_;options.logicalScale=canvasWindow_->zoom();
                edit.pendingGuides=edit.drag->snapEdge(*session().document(),edit.snapping,*values,modifiers,
                    transformOptionsPage_->aspectLocked(),options);
            }
            setSelectionTransformValues(*values);
            return true;
        }
        if (duplicateMove_ && !duplicateMove_->command) {
            const auto delta = position - duplicateMove_->press;
            if ((std::abs(delta.x) + std::abs(delta.y)) * canvasWindow_->zoom() < QApplication::startDragDistance())
                return true; // Click/jitter: neither duplicate nor move the originals.
            if (!prepareDuplicateMove()) return false;
        }
        auto* edit = layerTransform_ ? layerTransform_.get() : activeLayerMove_.get();
        const bool ratio=layerTransform_ && transformOptionsPage_->aspectLocked();
        auto snapping = snappingOptions_;
        snapping.logicalScale = canvasWindow_->zoom();
        if (!edit || !edit->dragTo(position,modifiers,ratio,snapping)) {
            canvasWindow_->setSnapGuides({});
            if(layerTransform_&&layerTransform_->targetAvailable()) {
                statusBar()->showMessage(tr("Invalid transform — last valid position retained"),2500);return true;
            }
            return false;
        }
        if (layerTransform_) refreshLayerTransform();
        else {
            canvasWindow_->setDocument(session().document()->snapshot(), false);
            refreshMoveControls();
            refreshShapeOverlay();
        }
        canvasWindow_->setSnapGuides(edit->snapGuides());
        return true;
    };
    canvasWindow_->onTransformEnded = [this] {
        if(layerCrop_){(void)runTransformAction([&]{layerCrop_->endDrag();return true;});refreshLayerCrop();updateActionState();return;}
        if (shapeResize_) { finishShapeResize(true); return; }
        if (selectionTransform_) {
            selectionTransform_->drag.reset();
            completeSelectionTransformAction();
            selectionTransform_->pendingGuides = {};
            canvasWindow_->setSnapGuides({});
            return;
        }
        if (layerTransform_) {
            (void)runTransformAction([&] { layerTransform_->endDrag(); return true; });
            refreshLayerTransform();
            updateActionState();
        } else finishLayerMove(true);
    };
    canvasWindow_->onTransformDragCancelled = [this] {
        if(layerCrop_){layerCrop_->cancelDrag();refreshLayerCrop();updateActionState();return;}
        if (shapeResize_) { finishShapeResize(false); return; }
        if (selectionTransform_) {
            if (selectionTransform_->drag) selectionTransform_->values = selectionTransform_->dragBefore;
            selectionTransform_->drag.reset();
            queuedPixelTransform_.reset();
            selectionTransform_->pendingGuides={};canvasWindow_->setSnapGuides({});
            setSelectionTransformValues(selectionTransform_->values);
            refreshSelectionTransform();
            updateActionState();
            return;
        }
        if (layerTransform_) { layerTransform_->cancelDrag(); refreshLayerTransform(); }
        else finishLayerMove(false);
        updateActionState();
    };
    propertiesPanel_->onSaveBrushCopyRequested = [this](const QString& name,
                                                      const core::BrushSettings& settings) {
        auto result = brushPresetStore_.saveCopy(name, settings, brushPresets_);
        if (result.preset) {
            brushPresets_.push_back(*result.preset);
        }
        return result;
    };
    setForegroundColor(session().foregroundColor());

    createDocumentTabs();
    createInitialDocument();
    setActiveTool(core::ToolId::Move);
    workspace_->setRulerVisible(Qt::Horizontal, true);
    workspace_->setRulerVisible(Qt::Vertical, true);

    if (persistWindowState_) {
        QSettings settings;
        canvasWindow_->setAdvancedMeasurementReadout(settings.value(
            QStringLiteral("preferences/measurement/advancedReadout"), false).toBool());
        layerOutlinesAction_->setChecked(settings.value(
            QStringLiteral("preferences/canvas/selectedLayerOutlines"), true).toBool());
        collapseLayerSelectionOnEmptyClick_ = settings.value(
            QStringLiteral("preferences/canvas/collapseLayerSelectionOnEmptyClick"), true).toBool();
        shiftNudgePixels_ = std::clamp(settings.value(
            QStringLiteral("preferences/canvas/shiftNudgePixels"), 10).toInt(), 1, 1000);
        toolHintPosition_ = std::clamp(settings.value(
            QStringLiteral("preferences/ui/toolHintPosition"), 1).toInt(), 0, 2);
        updateStatusNotification();
        const auto geometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
        if (!geometry.isEmpty()) {
            restoreGeometry(geometry);
        }
        if (persistPanelLayout_) {
            workspace_->setRulerFarEdge(Qt::Horizontal, settings.value(QStringLiteral("view/rulers/horizontalFarEdge"), false).toBool());
            workspace_->setRulerFarEdge(Qt::Vertical, settings.value(QStringLiteral("view/rulers/verticalFarEdge"), false).toBool());
            workspace_->setRulerVisible(Qt::Horizontal, settings.value(QStringLiteral("view/rulers/horizontalVisible"), true).toBool());
            workspace_->setRulerVisible(Qt::Vertical, settings.value(QStringLiteral("view/rulers/verticalVisible"), true).toBool());
            const auto shellState = settings.value(QStringLiteral("window/shell-v4")).toByteArray();
            if (!shellState.isEmpty()) {
                restoreState(shellState, 4);
            }
            if (settings.contains(QStringLiteral("window/overlay-panel-width-v1"))) {
                workspace_->setPanelWidth(settings.value(
                    QStringLiteral("window/overlay-panel-width-v1")).toInt());
            }
            if (settings.contains(QStringLiteral("window/overlay-left-panel-width-v1"))) {
                workspace_->setLeftPanelWidth(settings.value(
                    QStringLiteral("window/overlay-left-panel-width-v1")).toInt());
            }
            const auto savedToolDock = settings.value(
                QStringLiteral("window/tool-rail-dock-v1"),
                QStringLiteral("left")).toString();
            if (savedToolDock == QStringLiteral("right-panel")) {
                setToolRailDockLocation(ToolRailDockLocation::RightPanel);
            } else if (savedToolDock == QStringLiteral("top")) {
                setToolRailDockLocation(ToolRailDockLocation::Top);
            } else if (savedToolDock == QStringLiteral("bottom")) {
                setToolRailDockLocation(ToolRailDockLocation::Bottom);
            } else {
                setToolRailDockLocation(ToolRailDockLocation::Left);
            }
        }

        auto colors = session().colors();
        const auto readColor = [&settings](const QString& key, core::Rgba8 fallback) {
            const auto value = settings.value(key).value<QColor>();
            return value.isValid() ? core::Rgba8 {static_cast<std::uint8_t>(value.red()),
                static_cast<std::uint8_t>(value.green()), static_cast<std::uint8_t>(value.blue()),
                static_cast<std::uint8_t>(value.alpha())} : fallback;
        };
        colors.primary = readColor(QStringLiteral("editor/colors-v2/primary"),
            readColor(QStringLiteral("editor/foreground-color-v1"), colors.primary));
        colors.secondary = readColor(QStringLiteral("editor/colors-v2/secondary"), colors.secondary);
        colors.active = settings.value(QStringLiteral("editor/colors-v2/active"), 0).toInt() == 1
            ? core::ColorSlot::Secondary : core::ColorSlot::Primary;
        setColors(colors);

        if (persistPanelLayout_) {
            struct SavedPanelState {
                WorkspacePanel* panel {nullptr};
                QAction* action {nullptr};
                QString placement;
                QRect geometry;
                bool visible {true};
                int index {0};
                int defaultOrder {0};
            };
            const auto readPanel = [this, &settings](const QString& id,
                                       WorkspacePanel* panel, QAction* action,
                                       int defaultOrder) {
                const auto prefix = QStringLiteral("window/panels-v2/%1-").arg(id);
                return SavedPanelState {
                    .panel = panel,
                    .action = action,
                    .placement = settings.value(prefix + QStringLiteral("placement"),
                        workspace_->panelPlacement(panel) == OverlayDockWorkspace::PanelPlacement::DockedLeft
                            ? QStringLiteral("left") : QStringLiteral("right")).toString(),
                    .geometry = settings.value(
                        prefix + QStringLiteral("geometry")).toRect(),
                    .visible = settings.value(
                        prefix + QStringLiteral("visible"), action->isChecked()).toBool(),
                    .index = settings.value(
                        prefix + QStringLiteral("index"), workspace_->dockedPanelIndex(panel)).toInt(),
                    .defaultOrder = defaultOrder,
                };
            };
            const std::array savedPanels {
                readPanel(QStringLiteral("color"), colorPanelShell_,
                    colorPanelAction_, 0),
                readPanel(QStringLiteral("layers"), layersPanelShell_,
                    layersPanelAction_, 1),
                readPanel(QStringLiteral("properties"), propertiesPanelShell_,
                    propertiesPanelAction_, 2),
                readPanel(QStringLiteral("adjustments"), adjustmentsPanelShell_,
                    adjustmentsPanelAction_, 3),
            };
            const auto savedLeftPanelSizes = settings.value(
                QStringLiteral("window/panels-v2/left-splitter-state"),
                workspace_->saveDockedPanelSizes(OverlayDockWorkspace::PanelDockSide::Left))
                                                   .toByteArray();
            const auto savedRightPanelSizes = settings.value(
                QStringLiteral("window/panels-v2/right-splitter-state"),
                workspace_->saveDockedPanelSizes(OverlayDockWorkspace::PanelDockSide::Right))
                                                    .toByteArray();
            QTimer::singleShot(0, this,
                [this, savedPanels, savedLeftPanelSizes, savedRightPanelSizes] {
                for (const auto& saved : savedPanels) {
                    if (saved.placement == QStringLiteral("floating")) {
                        workspace_->floatPanel(saved.panel, saved.geometry);
                    }
                }
                const auto restoreDockSide = [this, &savedPanels](
                                                 const QString& placement,
                                                 OverlayDockWorkspace::PanelDockSide side) {
                    std::vector<const SavedPanelState*> ordered;
                    for (const auto& saved : savedPanels) {
                        if (saved.placement == placement) {
                            ordered.push_back(&saved);
                        }
                    }
                    std::stable_sort(ordered.begin(), ordered.end(),
                        [](const SavedPanelState* left,
                            const SavedPanelState* right) {
                            return std::tie(left->index, left->defaultOrder)
                                < std::tie(right->index, right->defaultOrder);
                        });
                    for (const auto* saved : ordered) {
                        workspace_->dockPanel(saved->panel, side);
                    }
                };
                restoreDockSide(QStringLiteral("left"),
                    OverlayDockWorkspace::PanelDockSide::Left);
                restoreDockSide(QStringLiteral("right"),
                    OverlayDockWorkspace::PanelDockSide::Right);
                // Placement and order define the splitters' widget sequence. Only
                // then is their opaque size allocation meaningful to restore.
                workspace_->restoreDockedPanelSizes(
                    OverlayDockWorkspace::PanelDockSide::Left,
                    savedLeftPanelSizes);
                workspace_->restoreDockedPanelSizes(
                    OverlayDockWorkspace::PanelDockSide::Right,
                    savedRightPanelSizes);
                for (const auto& saved : savedPanels) {
                    workspace_->setPanelVisible(saved.panel, saved.visible);
                    saved.action->setChecked(saved.visible);
                }
            });
        }
    }
    QTimer::singleShot(0, this, [this] {
        colorPanelAction_->setChecked(
            workspace_->panelVisible(colorPanelShell_));
        layersPanelAction_->setChecked(
            workspace_->panelVisible(layersPanelShell_));
        propertiesPanelAction_->setChecked(
            workspace_->panelVisible(propertiesPanelShell_));
        adjustmentsPanelAction_->setChecked(workspace_->panelVisible(adjustmentsPanelShell_));
        updatePanelAreaVisibility();
    });
    const auto initialDocument = activeDocumentId();
    QTimer::singleShot(0, this, [this, initialDocument] {
        if (activeDocumentId() == initialDocument) canvasWindow_->fitDocumentToView();
    });
    initializeShortcuts();
    qApp->installEventFilter(this);
}

MainWindow::~MainWindow()
{
    if (updateService_) { updateService_->onChanged = {}; updateService_->cancel(); }
    canvasWindow_->onDocumentPresentationChanged={};
    if(pixelPreview_)pixelPreview_->onReady={};
    pixelPreview_.reset();
    cancelSpotHeal(true);
    filterPreparationShuttingDown_=true;
    canvasWindow_->onFilterPreparationRequested={};
    cancelFilterPreparation(true);
    finishEffectEdit(false);
    effectsPanel_->onPreview={};effectsPanel_->onInteractionStarted={};effectsPanel_->onInteractionFinished={};
    effectsPanel_->onComparison={};effectsPanel_->onColorRequested={};
    finishFilterEdit(false);
    filtersPanel_->onPreview={};
    filtersPanel_->onInteractionStarted={};
    filtersPanel_->onInteractionFinished={};
    filtersPanel_->onCaptureSelection={};
    filtersPanel_->onComparison={};
    filtersPanel_->onCapturedRegionChanged={};
    filtersPanel_->onCancelProcessing={};
    adjustmentsPanel_->onFiltersCategoryChanged={};
    finishLayerRename(false);
    static_cast<LayerListView*>(layerList_)->onRenameEditorChanged = {};
    finishAdjustmentEdit(false);
    adjustmentsPanel_->onPreview = {};
    adjustmentsPanel_->onInteractionStarted = {};
    adjustmentsPanel_->onInteractionFinished = {};
    adjustmentsPanel_->onCaptureSelection = {};
    adjustmentsPanel_->onComparison = {};
    adjustmentsPanel_->onHistogramRequested = {};
    qApp->removeEventFilter(this);
    disconnect(qApp, &QApplication::focusChanged, this, nullptr);
    disconnect(qApp, &QGuiApplication::focusWindowChanged, this, nullptr);
    canvasWindow_->onColorSampleRequested = {};
    canvasWindow_->onColorPicked = {};
    canvasWindow_->onFillRequested = {};
    canvasWindow_->cancelColorSampling();
    cancelPendingEdits();
    canvasWindow_->onSelectionBegan = {};
    canvasWindow_->onSelectionSample = {};
    canvasWindow_->onShapePressed = {};
    canvasWindow_->onShapeMoved = {};
    canvasWindow_->onShapeEnded = {};
    canvasWindow_->onShapeCloseRequested = {};
    canvasWindow_->onShapeHit = {};
    canvasWindow_->onPrepareDocumentSnapshot = {};
    shapePreviewTimer_->stop();
    shapeDensityTimer_->stop();
    canvasWindow_->onSelectionMoved = {};
    canvasWindow_->onFinishRequested = {};
    canvasWindow_->onSelectionEnded = {};
    canvasWindow_->onSelectionCloseRequested = {};
    canvasWindow_->onTransformBegan = {};
    canvasWindow_->onTransformMoved = {};
    canvasWindow_->onTransformEnded = {};
    canvasWindow_->onTransformDragCancelled = {};
    canvasWindow_->onBrushStrokeBegan = {};
    canvasWindow_->onBrushStrokeMoved = {};
    canvasWindow_->onBrushStrokeEnded = {};
    canvasWindow_->onBrushStrokeCancelled = {};
    canvasWindow_->onCloneSourcePicked = {};
    canvasWindow_->onTextPressed = {};
    canvasWindow_->onTextDragged = {};
    canvasWindow_->onTextEvent = {};
    canvasWindow_->onViewportUpdated = {};
    canvasWindow_->onViewStateChanged = {};
    canvasWindow_->onTemporaryMeasureChanged = {};
    textController_.reset();
}

namespace {
bool editableWidgetOwnsInput(const QWidget* focus)
{
    const auto* line = qobject_cast<const QLineEdit*>(focus);
    const auto* compact = dynamic_cast<const CompactValueControl*>(focus);
    const auto* combo = qobject_cast<const QComboBox*>(focus);
    return (line && !line->isReadOnly())
        || (combo && combo->isEditable())
        || (compact && compact->isManualEntryActive())
        || (!compact && qobject_cast<const QAbstractSpinBox*>(focus))
        || qobject_cast<const QTextEdit*>(focus) || qobject_cast<const QPlainTextEdit*>(focus);
}
bool nativeFocusBelongsTo(QWindow* host, QWindow* panel)
{
    for (auto* native = QGuiApplication::focusWindow(); native; native = native->parent())
        if (native == host || native == panel) return true;
    return false;
}
}

bool MainWindow::editorTextInputActive() const
{
    return layerRenameEditor_ || editableWidgetOwnsInput(QApplication::focusWidget())
        || editableWidgetOwnsInput(qobject_cast<QWidget*>(QGuiApplication::focusObject()));
}

void MainWindow::focusLayerRenameEditor(QLineEdit* editor)
{
    const auto previous = layerRenameEditor_;
    const bool ownedBridge = previous && canvasContainer_->focusProxy() == previous;
    layerRenameEditor_ = editor;
    if (!editor && !ownedBridge) return;
    auto* nativeFocus = QGuiApplication::focusWindow();
    auto* before = QGuiApplication::focusObject();
    if (editor) {
        // A top-bar control can remain the host QWidgetWindow's cached focus
        // widget even after logical focus moves to the child panel plane.
        // Establish the host's canvas route first, then delegate it to the
        // row editor. Otherwise native keys can still reach that old slider.
        canvasContainer_->setFocusProxy(nullptr);
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(window());
        QT_WARNING_POP
        canvasContainer_->setFocus(Qt::OtherFocusReason);
    }
    canvasContainer_->setFocusProxy(editor);
    if (!editor && previous && previous->hasFocus()) previous->clearFocus();
    // No keyboard grab or child-window activation: delegate the existing
    // native host's keyboard/IME route to the row editor, as for canvas text.
    if (editor || (QGuiApplication::applicationState() == Qt::ApplicationActive
        && nativeFocusBelongsTo(windowHandle(),workspace_->panelOverlay()->windowHandle()))) {
        auto* target = editor ? static_cast<QWidget*>(editor) : canvasContainer_;
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        QApplication::setActiveWindow(target->window());
        QT_WARNING_POP
        target->setFocus(Qt::OtherFocusReason);
    }
    auto* after = QGuiApplication::focusObject();
    if (nativeFocus && nativeFocus == QGuiApplication::focusWindow() && before != after)
        Q_EMIT nativeFocus->focusObjectChanged(after);
}

void MainWindow::restoreLayerRenameFocus()
{
    if (!layerRenameEditor_ || !isVisible() || isMinimized()
        || QGuiApplication::applicationState() != Qt::ApplicationActive
        || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
    if (nativeFocusBelongsTo(windowHandle(),workspace_->panelOverlay()->windowHandle())
        && (QApplication::focusWidget() != layerRenameEditor_
        || canvasContainer_->focusProxy() != layerRenameEditor_))
        focusLayerRenameEditor(layerRenameEditor_);
}

void MainWindow::finishLayerRename(bool commit)
{
    if (layerList_) static_cast<LayerListView*>(layerList_)->finishRename(commit);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if (workspace_ && watched == workspace_->panelOverlay() && event->type() == QEvent::Resize)
        updateStatusNotification();
    if (layerRenameEditor_) {
        auto* editor = layerRenameEditor_.data();
        const auto* popup = QApplication::activePopupWidget();
        const bool editorPopup = popup && popupBelongsTo(popup, editor);
        std::optional<QPointF> press;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            press = static_cast<QMouseEvent*>(event)->globalPosition();
        else if (event->type() == QEvent::TabletPress)
            press = static_cast<QTabletEvent*>(event)->globalPosition();
        else if (event->type() == QEvent::TouchBegin) {
            const auto* touch = static_cast<QTouchEvent*>(event);
            if (!touch->points().empty()) press = touch->points().front().globalPosition();
        }
        // Native event carriers are not QWidget descendants of the editor.
        // Use actual pointer geometry so clicking/selecting within the field
        // never finishes it, but even a NoFocus button or canvas click does.
        if (event->type() == QEvent::ApplicationDeactivate
            || (press && !editorPopup && !QRectF(editor->mapToGlobal(QPoint{}),editor->size()).contains(*press)))
            finishLayerRename();
        else if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true; // Native keys/IME still dispatch to the real QLineEdit.
        }
    }
    if (workspace_ && event->type() == QEvent::ApplicationDeactivate) workspace_->cancelRulerDrag();
    if (workspace_ && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && workspace_->cancelRulerDrag()) {
        pointerRouter_->cancelCapture(); event->accept(); return true;
    }
    // Release momentary access even if focus has moved into a modal card or
    // editable control since the hold began. Never synthesize a tool shortcut.
    if ((measureAccessHeld_ || canvasWindow_->temporaryMeasureActive())
        && (event->type() == QEvent::ApplicationDeactivate
            || ((watched == this || watched == windowHandle())
                && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
            || (event->type() == QEvent::KeyRelease
                && static_cast<QKeyEvent*>(event)->key() == measureAccessKey_
                && !static_cast<QKeyEvent*>(event)->isAutoRepeat()))) {
        measureAccessHeld_ = false;
        measureAccessKey_ = 0;
        canvasWindow_->setTemporaryMeasure(false);
        if (event->type() == QEvent::KeyRelease) { event->accept(); return true; }
    }
    if (panAccessKey_ && (event->type() == QEvent::ApplicationDeactivate
        || ((watched == this || watched == windowHandle()) && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))
        || (event->type() == QEvent::KeyRelease && static_cast<QKeyEvent*>(event)->key() == panAccessKey_
            && !static_cast<QKeyEvent*>(event)->isAutoRepeat()))) {
        panAccessKey_ = 0; canvasWindow_->setSpacePanHeld(false);
        if (event->type() == QEvent::KeyRelease) { event->accept(); return true; }
    }
    // The embedded card owns editor input, including text/IME and shortcuts.
    // Its own barrier allows host-window chrome and nested file dialogs.
    if (event->type() == QEvent::ApplicationPaletteChange && canvasWindow_) refreshThemeAppearance();
    if (workspaceDialog_) return QMainWindow::eventFilter(watched, event);
    if (statusNotification_ && statusNotification_->isVisible()
        && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::TabletMove || event->type() == QEvent::TabletPress)) {
        const auto global = static_cast<QSinglePointEvent*>(event)->globalPosition().toPoint();
        if (statusNotification_->rect().contains(statusNotification_->mapFromGlobal(global)))
            statusBar()->clearMessage(); // Observe, don't consume: the underlying control still receives input.
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    const auto* panelSurface = workspace_ ? workspace_->panelOverlay() : nullptr;
    const bool ourTarget = watched == canvasWindow_ || watched == windowHandle()
        || (widget && (widget == this || isAncestorOf(widget)))
        || (panelSurface && (watched == panelSurface || watched == panelSurface->windowHandle()
            || (widget && panelSurface->isAncestorOf(widget))));
    if (ourTarget && routeDocumentDrag(watched, event)) return true;
    if (spotHealJob_ && ourTarget && routeSpotHealProcessingInput(watched, event)) return true;
    if(localBlurProcessing_){
        if(event->type()==QEvent::ApplicationDeactivate||(watched==canvasWindow_&&event->type()==QEvent::FocusOut))
            localBlurCancelRequested_=true;
        if(ourTarget){
            if(event->type()==QEvent::KeyPress&&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape)
                localBlurCancelRequested_=true;
            if(event->type()==QEvent::MouseMove||event->type()==QEvent::TabletMove
                ||event->type()==QEvent::MouseButtonRelease||event->type()==QEvent::TabletRelease)
                deferLocalBlurInput(watched,event);
            switch(event->type()){
            case QEvent::KeyPress:case QEvent::KeyRelease:case QEvent::ShortcutOverride:
            case QEvent::MouseButtonPress:case QEvent::MouseButtonDblClick:case QEvent::MouseButtonRelease:
            case QEvent::MouseMove:case QEvent::TabletPress:case QEvent::TabletMove:case QEvent::TabletRelease:
            case QEvent::InputMethod:case QEvent::Drop:case QEvent::DragEnter:case QEvent::Wheel:
                event->accept();return true;
            default:break;
            }
        }
    }
    if(cloneProcessing_) {
        if(event->type()==QEvent::ApplicationDeactivate || (watched==canvasWindow_ && event->type()==QEvent::FocusOut))
            cloneCancelRequested_=true;
        if(ourTarget) {
            if(event->type()==QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape)
                cloneCancelRequested_=true;
            switch(event->type()) {
            case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
            case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: case QEvent::MouseButtonRelease:
            case QEvent::MouseMove: case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
            case QEvent::InputMethod: case QEvent::Drop: case QEvent::DragEnter: case QEvent::Wheel:
                event->accept();return true;
            default:break;
            }
        }
    }
    if(widget&&event->type()==QEvent::MouseButtonPress){
        if(adjustmentsPanel_&&(widget==adjustmentsPanel_||adjustmentsPanel_->isAncestorOf(widget))){
            capturedFilterRegionActive_=adjustmentsPanel_->filtersCategoryActive();
            if(capturedFilterRegionActive_){
                if(filtersPanel_->onCapturedRegionChanged)filtersPanel_->onCapturedRegionChanged();
            }else if(adjustmentsPanel_->onCapturedRegionChanged)adjustmentsPanel_->onCapturedRegionChanged();
        }
    }
    if(effectEdit_&&ourTarget&&(event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress)
        &&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){
        if(event->type()==QEvent::KeyPress){effectsPanel_->finishEditing(false);finishEffectEdit(false);pointerRouter_->cancelCapture();}
        event->accept();return true;
    }
    if(event->type()==QEvent::ApplicationDeactivate){if(effectsPanel_->onComparison)effectsPanel_->onComparison(false);finishEffectEdit(false);}
    if(effectEdit_&&watched==canvasWindow_&&(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::TabletPress))finishEffectEdit(true);
    if(filterEdit_&&ourTarget&&(event->type()==QEvent::ShortcutOverride||event->type()==QEvent::KeyPress)
        &&static_cast<QKeyEvent*>(event)->key()==Qt::Key_Escape){
        if(event->type()==QEvent::KeyPress){filtersPanel_->finishEditing(false);finishFilterEdit(false);pointerRouter_->cancelCapture();}
        event->accept();return true;
    }
    if(event->type()==QEvent::ApplicationDeactivate){
        canvasWindow_->setFilterBypassLayer(std::nullopt);
        if(filterEdit_){filtersPanel_->finishEditing(false);finishFilterEdit(false);}
    }
    if(filterEdit_&&watched==canvasWindow_
        &&(event->type()==QEvent::MouseButtonPress||event->type()==QEvent::TabletPress)){
        filtersPanel_->finishEditing();finishFilterEdit(true);
    }
    if (adjustmentEdit_ && ourTarget && (event->type() == QEvent::ShortcutOverride
        || event->type() == QEvent::KeyPress) && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) {
            adjustmentsPanel_->finishEditing(false);
            finishAdjustmentEdit(false);
            pointerRouter_->cancelCapture();
        }
        event->accept(); return true;
    }
    if (adjustmentEdit_ && event->type() == QEvent::ApplicationDeactivate) {
        adjustmentsPanel_->finishEditing(false); finishAdjustmentEdit(false);
    }
    if (event->type() == QEvent::ApplicationDeactivate)
        canvasWindow_->setAdjustmentBypassLayer(std::nullopt);
    if (adjustmentEdit_ && watched == canvasWindow_
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress)) {
        adjustmentsPanel_->finishEditing(); finishAdjustmentEdit(true);
    }
    // File serialization pumps progress/cancel events, but owns an edit
    // exclusion barrier. Paint/expose and the progress dialog remain live.
    if (fileBusy_ && ourTarget && !(fileProgress_ && widget
        && (widget == fileProgress_ || widget->window() == fileProgress_))) {
        switch (event->type()) {
        case QEvent::KeyPress: case QEvent::KeyRelease: case QEvent::ShortcutOverride:
        case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: case QEvent::MouseButtonRelease:
        case QEvent::MouseMove: case QEvent::TabletPress: case QEvent::TabletMove: case QEvent::TabletRelease:
        case QEvent::InputMethod: case QEvent::Drop: case QEvent::DragEnter:
        case QEvent::Wheel: event->accept(); return true;
        default: break;
        }
    }
    if (ourTarget && routePanelKeyboard(watched, event)) return true;
    if (!fileBusy_ && ourTarget && !QApplication::activeModalWidget()
        && !QApplication::activePopupWidget()
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto* key = static_cast<QKeyEvent*>(event);
        const bool saveAs = shortcutMatches(shortcuts_, "SaveDocumentAsAction", *key);
        if (saveAs || shortcutMatches(shortcuts_, "SaveDocumentAction", *key)) {
            event->accept();
            if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
                saveDocument(saveAs);
            return true;
        }
    }
    if (ourTarget && textController_ && (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonDblClick))
        textController_->prepareOverlayInteraction(widget, *static_cast<QMouseEvent*>(event));
    if (ourTarget && textController_ && textController_->active()
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::ShortcutOverride)
        && textController_->keyEvent(static_cast<QKeyEvent*>(event),
            editorTextInputActive() || editableWidgetOwnsInput(widget)))
        return true;
    if (ourTarget && routeEditorShortcut(watched, event)) return true;
    // A modifier may already be held on entering the canvas or a child panel.
    // Observe input snapshots centrally; never synthesize a click or change
    // selectionOperation_ / the operation latched by a live gesture.
    if (ourTarget && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::TabletMove || event->type() == QEvent::TabletPress
            || event->type() == QEvent::TabletRelease))
        updateSelectionUiModifiers(static_cast<QInputEvent*>(event)->modifiers());
    if (event->type() == QEvent::ApplicationDeactivate
        || ((watched == this || watched == windowHandle())
            && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)))
        updateSelectionUiModifiers(Qt::NoModifier);
    if (selectionRasterizer_ || anchoredLassoActive()) {
        const bool canvasCancelled = watched == canvasWindow_
            && (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate
                || event->type() == QEvent::Hide || event->type() == QEvent::Close
                || event->type() == QEvent::TouchCancel
                || (event->type() == QEvent::PlatformSurface
                    && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
                        == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed));
        if (canvasCancelled || ((watched == this || watched == windowHandle())
                && event->type() == QEvent::WindowDeactivate))
            finishSelectionGesture(true);
    }
    if (activeFill_ && watched == canvasWindow_
        && (event->type() == QEvent::TabletPress || (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton
            && !canvasWindow_->spacePanHeld()))) {
        event->accept(); return true;
    }
    // Losing application visibility/focus suspends input, not the persistent
    // Ctrl+T session. Preserve completed geometry and both history branches.
    // An unfinished pointer drag still rolls back to its press baseline;
    // valid numeric text finishes just as it does on an ordinary focus-out.
    // Child/panel focus handoffs must not terminate a numeric hold.
    if (event->type() == QEvent::ApplicationDeactivate
        || ((watched == this || watched == windowHandle())
            && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide))) {
        canvasWindow_->cancelPanInput();
        if(layerCrop_){canvasWindow_->cancelTransformInput();cropOptionsPage_->finishNumericInput();pointerRouter_->cancelCapture();}
        if (layerTransform_ || selectionTransform_) {
            canvasWindow_->cancelTransformInput();
            transformOptionsPage_->finishNumericInput();
            pointerRouter_->cancelCapture();
        }
    }
    if(layerCrop_ && watched==canvasWindow_ && (event->type()==QEvent::MouseButtonPress||event->type()==QEvent::TabletPress))cropOptionsPage_->finishNumericInput();
    if ((layerTransform_ || selectionTransform_) && watched==canvasWindow_
        && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::TabletPress)) {
        transformOptionsPage_->finishNumericInput();
    }
    if (session().activeTool() == core::ToolId::Move && watched == canvasWindow_
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress))
        moveOptionsPage_->finishNumericInput();
    if (core::isSelectionTool(session().activeTool()) && watched == canvasWindow_
        && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress))
        finishSelectionNumericInput();
    if (event->type() == QEvent::ApplicationDeactivate) {
        canvasWindow_->cancelShapeInput();
        finishShapeResize(false);
        canvasWindow_->cancelPanInput();
        cancelFill();
        finishSelectionRotation(false);
        canvasWindow_->cancelSelectionInput();
        finishSelectionGesture(true);
        canvasWindow_->cancelColorSampling();
        finishLayerMove(false);
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease
        && event->type() != QEvent::ShortcutOverride) {
        return QMainWindow::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    if (ourTarget && event->type() != QEvent::ShortcutOverride && !key->isAutoRepeat()) {
        auto modifiers = key->modifiers();
        if (key->key() == Qt::Key_Shift || key->key() == Qt::Key_Alt)
            modifiers.setFlag(key->key() == Qt::Key_Shift ? Qt::ShiftModifier : Qt::AltModifier,
                event->type() == QEvent::KeyPress);
        // Releases must be observed even if focus entered an editable field
        // during the hold. Its keyboard ownership remains unchanged below.
        updateSelectionUiModifiers(modifiers);
        canvasWindow_->updateShapeModifiers(modifiers);
        canvasWindow_->updateMeasureModifiers(modifiers);
    }
    const bool temporaryPickerTool = session().activeTool() == core::ToolId::Brush
        || session().activeTool() == core::ToolId::Eraser || session().activeTool() == core::ToolId::Fill;
    const bool editing = editorTextInputActive() || editableWidgetOwnsInput(widget);
    const bool blocked = QApplication::activeModalWidget() || QApplication::activePopupWidget();
    if(ourTarget && smartInteractionActive() && !blocked && key->key()==Qt::Key_Escape
        && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        if(event->type()==QEvent::KeyPress){cancelSmartSelection(false);updateActionState();
            statusBar()->showMessage(QStringLiteral("Smart selection cancelled"),2500);}
        event->accept();return true;
    }
    if (ourTarget && colorSelectionEditing_ && !blocked && key->key() == Qt::Key_Escape
        && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        if (event->type() == QEvent::KeyPress) {
            cancelColorSelection(false); updateActionState();
            statusBar()->showMessage(QStringLiteral("Color selection cancelled"),2500);
        }
        event->accept(); return true;
    }
    if (ourTarget && !editing && !blocked && key->key() == Qt::Key_Escape
        && canvasWindow_->measureActive()) {
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
            if (canvasWindow_->measureDragging()) canvasWindow_->cancelMeasureInput();
            else canvasWindow_->clearMeasurement();
        }
        event->accept(); return true;
    }
    if (ourTarget && !editing && !blocked && key->key() == Qt::Key_Escape
        && (shapeCreation_ || anchoredLassoActive())) {
        if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
            if (shapeCreation_) finishShapeCreation(true); else finishSelectionGesture(true);
        }
        event->accept(); return true;
    }
    if (event->type() == QEvent::ShortcutOverride) return QMainWindow::eventFilter(watched, event);
    if (key->isAutoRepeat()) return QMainWindow::eventFilter(watched,event);
    if (ourTarget && canvasWindow_->panDragging() && event->type() == QEvent::KeyPress
        && key->key() == Qt::Key_Escape && !editing && !blocked) {
        canvasWindow_->cancelPanInput();
        pointerRouter_->cancelCapture();
        event->accept(); return true;
    }
    if (ourTarget && activeFill_ && !editing && !blocked && event->type()==QEvent::KeyPress && key->key()==Qt::Key_Escape) {
        cancelFill(); event->accept(); return true;
    }
    if (ourTarget && selectionRasterizer_ && !editing && !blocked && event->type() == QEvent::KeyPress
        && key->key() == Qt::Key_Escape) {
        finishSelectionGesture(true); event->accept(); return true;
    }
    if (ourTarget && selectionRotation_ && !editing && !blocked
        && event->type() == QEvent::KeyPress && key->key() == Qt::Key_Escape) {
        finishSelectionRotation(false);
        pointerRouter_->cancelCapture();
        event->accept(); return true;
    }
    if (ourTarget && !editing && !blocked && core::isSelectionTool(session().activeTool())
        && (key->key() == Qt::Key_Shift || key->key() == Qt::Key_Alt
            || (key->key() == Qt::Key_Control && selectionGesture_ && selectionGesture_->moving))) {
        auto modifiers = key->modifiers();
        modifiers.setFlag(key->key() == Qt::Key_Shift ? Qt::ShiftModifier
                : key->key() == Qt::Key_Control ? Qt::ControlModifier : Qt::AltModifier,
            event->type() == QEvent::KeyPress);
        canvasWindow_->updateSelectionModifiers(modifiers);
        if (selectionGesture_ && selectionGesture_->moving)
            moveSelectionGesture(selectionGesture_->pointer);
        event->accept(); return true;
    }
    if (ourTarget && (activeLayerMove_ || shapeResize_) && !editing && !blocked
        && event->type() == QEvent::KeyPress && key->key() == Qt::Key_Escape) {
        if (shapeResize_) finishShapeResize(false); else finishLayerMove(false);
        event->accept(); return true;
    }
    if(ourTarget && (activeLayerMove_ || shapeResize_) && !editing && !blocked
        && (key->key()==Qt::Key_Alt||key->key()==Qt::Key_Shift||key->key()==Qt::Key_Control)) {
        auto modifiers=key->modifiers();
        modifiers.setFlag(key->key()==Qt::Key_Alt?Qt::AltModifier:key->key()==Qt::Key_Control?Qt::ControlModifier:Qt::ShiftModifier,event->type()==QEvent::KeyPress);
        canvasWindow_->updateTransformModifiers(modifiers);
        event->accept();return true;
    }
    if (ourTarget && (layerTransform_ || layerCrop_ || selectionTransform_) && !editing && !blocked) {
        if (key->key()==Qt::Key_Alt || key->key()==Qt::Key_Shift || key->key()==Qt::Key_Control) {
            auto modifiers=key->modifiers();
            const auto flag=key->key()==Qt::Key_Alt ? Qt::AltModifier : key->key()==Qt::Key_Control ? Qt::ControlModifier : Qt::ShiftModifier;
            modifiers.setFlag(flag,event->type()==QEvent::KeyPress);
            canvasWindow_->updateTransformModifiers(modifiers);
            event->accept(); return true;
        }
        if (event->type()==QEvent::KeyPress && key->key()==Qt::Key_Escape) {
            if(layerCrop_)finishLayerCrop(false);
            else if (selectionTransform_) finishSelectionTransform(false);
            else finishLayerTransform(false);
            event->accept(); return true;
        }
    }
    if (key->key() == Qt::Key_Alt && event->type() == QEvent::KeyRelease)
        canvasWindow_->setTemporaryEyedropper(false);
    if (ourTarget && key->key() == Qt::Key_Escape && event->type() == QEvent::KeyPress
        && canvasWindow_->scene().eyedropperActive && !editing
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
        canvasWindow_->cancelColorSampling();
        event->accept();
        return true;
    }
    if (ourTarget && key->key() == Qt::Key_Alt && temporaryPickerTool && canvasWindow_->scene().cursorInside && !editing
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
        canvasWindow_->setTemporaryEyedropper(event->type() == QEvent::KeyPress);
        event->accept();
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // A host close dismisses its embedded card, then guards all documents.
    // Keep the card alive until its nested event loop unwinds normally.
    if (workspaceDialog_) workspaceDialog_->reject();
    const QScopedValueRollback closingOverlay(workspaceDialog_, static_cast<WorkspaceDialog*>(nullptr));
    if(spotHealJob_) cancelSpotHeal(true); // Cooperative cancellation also covers an inactive owner.
    if(cloneProcessing_){cloneCancelRequested_=true;event->ignore();return;}
    if(localBlurProcessing_){localBlurCancelRequested_=true;event->ignore();return;}
    if (fileBusy_) { event->ignore(); return; }
    finishLayerRename();
    // Closing cancels unfinished pointer/fill previews (as before), but never
    // discards completed text or Ctrl+T actions before asking about content.
    finishShapeCreation(true);
    cancelFill();
    cancelActiveBrushStroke();
    if (canvasWindow_->transformDragging()) canvasWindow_->cancelTransformInput();
    if (activeLayerMove_ && activeLayerMove_->dragging()) finishLayerMove(false);
    if (!guardAllDocuments()) { event->ignore(); return; }
    canvasWindow_->cancelColorSampling();
    cancelPendingEdits();
    if (persistWindowState_) {
        QSettings settings;
        settings.setValue(QStringLiteral("preferences/canvas/selectedLayerOutlines"), canvasWindow_->layerOutlinesVisible());
        settings.setValue(QStringLiteral("preferences/canvas/collapseLayerSelectionOnEmptyClick"), collapseLayerSelectionOnEmptyClick_);
        settings.setValue(QStringLiteral("preferences/canvas/shiftNudgePixels"), shiftNudgePixels_);
        settings.setValue(QStringLiteral("preferences/ui/toolHintPosition"), toolHintPosition_);
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        if (persistPanelLayout_) {
            settings.setValue(QStringLiteral("view/rulers/horizontalFarEdge"), workspace_->rulerFarEdge(Qt::Horizontal));
            settings.setValue(QStringLiteral("view/rulers/verticalFarEdge"), workspace_->rulerFarEdge(Qt::Vertical));
            settings.setValue(QStringLiteral("view/rulers/horizontalVisible"), workspace_->rulerVisible(Qt::Horizontal));
            settings.setValue(QStringLiteral("view/rulers/verticalVisible"), workspace_->rulerVisible(Qt::Vertical));
            settings.setValue(QStringLiteral("window/shell-v4"), saveState(4));
            settings.setValue(QStringLiteral("window/overlay-panel-width-v1"),
                workspace_->panelWidth());
            settings.setValue(QStringLiteral("window/overlay-left-panel-width-v1"),
                workspace_->leftPanelWidth());
        }
        const auto saveColor = [&settings](const QString& key, core::Rgba8 color) {
            settings.setValue(key, QColor(color.red, color.green, color.blue, color.alpha));
        };
        saveColor(QStringLiteral("editor/colors-v2/primary"), session().colors().primary);
        saveColor(QStringLiteral("editor/colors-v2/secondary"), session().colors().secondary);
        settings.setValue(QStringLiteral("editor/colors-v2/active"),
            session().colors().active == core::ColorSlot::Secondary ? 1 : 0);
        if (persistPanelLayout_) {
            const auto savePanel = [&settings, this](const QString& id,
                                       WorkspacePanel* panel) {
                QString placement = QStringLiteral("right");
                switch (workspace_->panelPlacement(panel)) {
                case OverlayDockWorkspace::PanelPlacement::DockedLeft:
                    placement = QStringLiteral("left");
                    break;
                case OverlayDockWorkspace::PanelPlacement::DockedRight:
                    break;
                case OverlayDockWorkspace::PanelPlacement::Floating:
                    placement = QStringLiteral("floating");
                    settings.setValue(
                        QStringLiteral("window/panels-v2/%1-geometry").arg(id),
                        workspace_->floatingPanelGeometry(panel));
                    break;
                }
                settings.setValue(
                    QStringLiteral("window/panels-v2/%1-placement").arg(id),
                    placement);
                settings.setValue(
                    QStringLiteral("window/panels-v2/%1-visible").arg(id),
                    workspace_->panelVisible(panel));
                settings.setValue(
                    QStringLiteral("window/panels-v2/%1-index").arg(id),
                    workspace_->dockedPanelIndex(panel));
            };
            savePanel(QStringLiteral("color"), colorPanelShell_);
            savePanel(QStringLiteral("layers"), layersPanelShell_);
            savePanel(QStringLiteral("properties"), propertiesPanelShell_);
            savePanel(QStringLiteral("adjustments"), adjustmentsPanelShell_);
            settings.setValue(
                QStringLiteral("window/panels-v2/left-splitter-state"),
                workspace_->saveDockedPanelSizes(
                    OverlayDockWorkspace::PanelDockSide::Left));
            settings.setValue(
                QStringLiteral("window/panels-v2/right-splitter-state"),
                workspace_->saveDockedPanelSizes(
                    OverlayDockWorkspace::PanelDockSide::Right));
            QString toolDock = QStringLiteral("left");
            switch (toolRailDockLocation_) {
            case ToolRailDockLocation::Left: break;
            case ToolRailDockLocation::RightPanel:
                toolDock = QStringLiteral("right-panel");
                break;
            case ToolRailDockLocation::Top:
                toolDock = QStringLiteral("top");
                break;
            case ToolRailDockLocation::Bottom:
                toolDock = QStringLiteral("bottom");
                break;
            }
            settings.setValue(QStringLiteral("window/tool-rail-dock-v1"), toolDock);
        }
    }
    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        startupNewDocumentPending_ = startupUpdateNoticePending_ = false;
        if (startupUiTimer_) startupUiTimer_->stop();
        if (updateService_) { updateService_->onChanged = {}; updateService_->cancel(); }
        if (workspaceDialog_) workspaceDialog_->reject();
    }
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        const auto urls = event->mimeData()->urls();
        if (std::any_of(urls.begin(), urls.end(), [](const QUrl& url) { return url.isLocalFile(); })) {
            event->acceptProposedAction();
            return;
        }
    }
    QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) {
        if (url.isLocalFile()) {
            paths.push_back(url.toLocalFile());
        }
    }
    if (!paths.isEmpty()) {
        handleDroppedImages(paths);
        event->acceptProposedAction();
        return;
    }
    QMainWindow::dropEvent(event);
}

void MainWindow::createActions()
{
    createLayerOrganizationActions();
    createSelectionActions();
    transformAction_ = new QAction(QStringLiteral("Transform layer"), this);
    transformAction_->setObjectName(QStringLiteral("LayerTransformAction"));
    transformAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    transformAction_->setIcon(toolGlyph(ToolGlyph::Transform));
    transformAction_->setCheckable(true);
    transformAction_->setToolTip(QStringLiteral("Transform · Ctrl+T · Enter applies · Escape cancels"));
    registerEditorWindowAction(transformAction_);
    connect(transformAction_, &QAction::triggered, this, [this] {
        beginLayerTransform(); // idempotent; Apply/Enter and Cancel/Escape own exit
        updateActionState();
    });
    transformSelectionAction_=new QAction(tr("Transform Selection"),this);
    transformSelectionAction_->setObjectName(QStringLiteral("TransformSelectionAction"));
    transformSelectionAction_->setToolTip(tr("Transform the selection boundary without changing layer pixels"));
    registerEditorWindowAction(transformSelectionAction_);
    connect(transformSelectionAction_,&QAction::triggered,this,&MainWindow::beginSelectionTransform);
    transformPixelsAction_=new QAction(tr("Transform Selected Pixels"),this);
    transformPixelsAction_->setObjectName(QStringLiteral("TransformSelectedPixelsAction"));
    transformPixelsAction_->setShortcut(QKeySequence(QStringLiteral("Shift+T")));
    transformPixelsAction_->setToolTip(tr("Cut and transform the selected raw pixels on the primary raster layer"));
    registerEditorWindowAction(transformPixelsAction_);
    connect(transformPixelsAction_,&QAction::triggered,this,&MainWindow::beginSelectedPixelTransform);
    createFillActions();
    undoAction_ = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")), QStringLiteral("Undo"), this);
    undoAction_->setShortcut(QKeySequence::Undo);
    registerEditorWindowAction(undoAction_);
    connect(undoAction_, &QAction::triggered, this, [this] {
        if (fileBusy_) return;
        if(effectEdit_){finishEffectEdit(false);return;}
        if(filterEdit_){filtersPanel_->finishEditing(false);finishFilterEdit(false);return;}
        if (adjustmentEdit_) { adjustmentsPanel_->finishEditing(false); finishAdjustmentEdit(false); return; }
        if (textController_ && textController_->active()) {
            textController_->history(false);
            return;
        }
        if(layerCrop_){stepCropHistory(false);return;}
        if (selectionTransform_) { stepSelectionTransformHistory(false); return; }
        if (layerTransform_) { stepTransformHistory(false); return; }
        if (shapeCreation_ || shapeEdit_ || shapeResize_ || activeFill_ || activeBrushStroke_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument() || activeCloneStroke_ || activeLocalBlurStroke_ || activeLayerMove_ || selectionGesture_ || selectionRotation_ || colorSelectionEditing_ || smartInteractionActive()) {
            cancelPendingEdits();
            return;
        }
        if (session().undo()) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    });

    redoAction_ = new QAction(QIcon::fromTheme(QStringLiteral("edit-redo")), QStringLiteral("Redo"), this);
    auto redoBindings = QKeySequence::keyBindings(QKeySequence::Redo);
    redoBindings.push_back(QKeySequence(QStringLiteral("Ctrl+Shift+Z")));
    QList<QKeySequence> redoKeys;
    for (const auto& key : redoBindings)
        if (!redoKeys.contains(key)) redoKeys.push_back(key);
    redoAction_->setShortcuts(redoKeys);
    registerEditorWindowAction(redoAction_);
    connect(redoAction_, &QAction::triggered, this, [this] {
        if (fileBusy_) return;
        if(effectEdit_){finishEffectEdit(false);return;}
        if(filterEdit_){filtersPanel_->finishEditing(false);finishFilterEdit(false);return;}
        if (adjustmentEdit_) { adjustmentsPanel_->finishEditing(false); finishAdjustmentEdit(false); return; }
        if (textController_ && textController_->active()) {
            textController_->history(true);
            return;
        }
        if(layerCrop_){stepCropHistory(true);return;}
        if (selectionTransform_) { stepSelectionTransformHistory(true); return; }
        if (layerTransform_) { stepTransformHistory(true); return; }
        if (shapeCreation_ || shapeEdit_ || shapeResize_ || activeFill_ || activeBrushStroke_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument() || activeCloneStroke_ || activeLocalBlurStroke_ || activeLayerMove_ || selectionGesture_ || selectionRotation_ || colorSelectionEditing_ || smartInteractionActive()) {
            cancelPendingEdits();
            return;
        }
        if (session().redo()) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    });

    deleteLayerAction_ = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
        QStringLiteral("Delete layer"), this);
    deleteLayerAction_->setText(tr("Delete Selected Layers"));
    deleteLayerAction_->setObjectName(QStringLiteral("DeleteSelectedLayersAction"));
    deleteLayerAction_->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_Delete));
    deleteLayerAction_->setAutoRepeat(false);
    registerEditorWindowAction(deleteLayerAction_);
    connect(deleteLayerAction_, &QAction::triggered, this, &MainWindow::deleteActiveLayer);

    auto* decreaseBrushSize = new QAction(QStringLiteral("Decrease brush size"), this);
    decreaseBrushSize->setObjectName(QStringLiteral("DecreaseBrushSizeAction"));
    decreaseBrushSize->setShortcut(QKeySequence(QStringLiteral("[")));
    registerEditorWindowAction(decreaseBrushSize);
    connect(decreaseBrushSize, &QAction::triggered, this,
        [this] { adjustBrushSize(false); });
    auto* increaseBrushSize = new QAction(QStringLiteral("Increase brush size"), this);
    increaseBrushSize->setObjectName(QStringLiteral("IncreaseBrushSizeAction"));
    increaseBrushSize->setShortcut(QKeySequence(QStringLiteral("]")));
    registerEditorWindowAction(increaseBrushSize);
    connect(increaseBrushSize, &QAction::triggered, this,
        [this] { adjustBrushSize(true); });
    layerOutlinesAction_ = new QAction(tr("Selected layer outlines"), this);
    layerOutlinesAction_->setObjectName(QStringLiteral("ToggleLayerOutlinesAction"));
    layerOutlinesAction_->setToolTip(tr("Show each selected layer's bounds in Move mode · O"));
    layerOutlinesAction_->setShortcut(QKeySequence(QStringLiteral("O")));
    layerOutlinesAction_->setAutoRepeat(false);
    layerOutlinesAction_->setCheckable(true);
    layerOutlinesAction_->setChecked(true);
    registerEditorWindowAction(layerOutlinesAction_);
    connect(layerOutlinesAction_, &QAction::toggled, this,
        [this](bool enabled) { canvasWindow_->setLayerOutlinesVisible(enabled); });
}

void MainWindow::refreshThemeAppearance()
{
    const auto rgba = [](QColor c) { return core::Rgba8 {static_cast<std::uint8_t>(c.red()),
        static_cast<std::uint8_t>(c.green()), static_cast<std::uint8_t>(c.blue()), 255}; };
    canvasWindow_->setOverlayAccent(editorAccent());
    canvasWindow_->setLayerOutlineColor(rgba(themeColor(ThemeColor::LayerSelection)));
    canvasWindow_->setCanvasColors(rgba(themeColor(ThemeColor::Canvas)),
        rgba(themeColor(ThemeColor::CheckerLight)), rgba(themeColor(ThemeColor::CheckerDark)));
    if (layerList_) layerList_->viewport()->update();
}

void MainWindow::showPreferences()
{
    if (fileBusy_ || workspaceDialog_) return;
    // Preferences is not a document edit: preserve text/transform sessions and
    // their history. A live pointer gesture must finish before opening a card.
    if (activeBrushStroke_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument() || activeCloneStroke_ || cloneProcessing_ || activeLocalBlurStroke_ || localBlurProcessing_ || activeFill_ || shapeCreation_ || canvasWindow_->transformDragging()
        || (activeLayerMove_ && activeLayerMove_->dragging())) return;
    WorkspaceDialog presenter(*workspace_, *this);
    PreferencesDialog dialog(PreferencesState {currentThemeSettings(), canvasWindow_->advancedMeasurementReadout(),
        canvasWindow_->layerOutlinesVisible(), collapseLayerSelectionOnEmptyClick_, shiftNudgePixels_, shortcuts_, toolHintPosition_}, &presenter);
    dialog.onPreview = [this](const PreferencesState& value) {
        if (value.theme != currentThemeSettings()) {
            applyEditorTheme(*qApp, value.theme); refreshThemeAppearance();
        }
        canvasWindow_->setAdvancedMeasurementReadout(value.advancedMeasurementReadout);
        layerOutlinesAction_->setChecked(value.layerOutlinesVisible);
        collapseLayerSelectionOnEmptyClick_ = value.collapseLayerSelectionOnEmptyClick;
        shiftNudgePixels_ = value.shiftNudgePixels;
        toolHintPosition_ = value.toolHintPosition;
        updateStatusNotification();
        if (value.shortcuts != shortcuts_) applyShortcutBindings(value.shortcuts);
    };
    dialog.onApply = [](const PreferencesState& value) {
        QSettings settings;
        settings.setValue(QStringLiteral("preferences/measurement/advancedReadout"), value.advancedMeasurementReadout);
        settings.setValue(QStringLiteral("preferences/canvas/selectedLayerOutlines"), value.layerOutlinesVisible);
        settings.setValue(QStringLiteral("preferences/canvas/collapseLayerSelectionOnEmptyClick"), value.collapseLayerSelectionOnEmptyClick);
        settings.setValue(QStringLiteral("preferences/canvas/shiftNudgePixels"), value.shiftNudgePixels);
        settings.setValue(QStringLiteral("preferences/ui/toolHintPosition"), value.toolHintPosition);
        return saveShortcutBindings(settings, value.shortcuts) && saveThemeSettings(settings, value.theme);
    };
    const QScopedValueRollback active(workspaceDialog_, &presenter);
    presenter.exec(dialog);
}

void MainWindow::showAbout()
{
    if (fileBusy_ || workspaceDialog_
        || pointerRouter_->captureDomain() != CrossWindowPointerRouter::CaptureDomain::None
        || activeFill_ || activeBrushStroke_ || activeCloneStroke_ || cloneProcessing_
        || activeLocalBlurStroke_ || localBlurProcessing_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument())
        return;
    WorkspaceDialog presenter(*workspace_, *this);
    startupNewDocumentPending_ = startupUpdateNoticePending_ = false;
    if (startupUiTimer_) startupUiTimer_->stop();
    if (!updateService_) updateService_ = new UpdateService(QCoreApplication::applicationVersion(), this);
    AboutDialog dialog(&presenter, nullptr, UpdateService::buildPackage(), {}, updateService_);
    QString restartPath;
    dialog.onRestartRequested = [&](const QString& path) { restartPath = path; };
    {
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        while (presenter.exec(dialog) == AboutDialog::NoticesRequested) {
            ThirdPartyNoticesDialog notices(&presenter);
            presenter.exec(notices);
        }
    }
    if (!restartPath.isEmpty()) restartAfterUpdate(restartPath);
}

void MainWindow::showFeedback()
{
    if (fileBusy_ || workspaceDialog_
        || pointerRouter_->captureDomain() != CrossWindowPointerRouter::CaptureDomain::None
        || activeFill_ || activeBrushStroke_ || activeCloneStroke_ || cloneProcessing_
        || activeLocalBlurStroke_ || localBlurProcessing_ || activeSpotHealStroke_ || spotHealBusyForActiveDocument())
        return;
    WorkspaceDialog presenter(*workspace_, *this);
    if (!feedbackService_) feedbackService_ = new FeedbackService(this);
    FeedbackDialog dialog(&presenter,
        FeedbackDialog::basicSystemInfo(QString::fromStdString(canvasWindow_->rendererStats().deviceName)), feedbackService_);
    const QScopedValueRollback active(workspaceDialog_, &presenter);
    presenter.exec(dialog);
}

void MainWindow::createMenus()
{
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    auto* newAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-new")),
        QStringLiteral("&New Document…"), this, &MainWindow::createNewDocument);
    newAction->setShortcut(QKeySequence::New);
    auto* openAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-open")),
        QStringLiteral("&Open…"), this, &MainWindow::openImage);
    openAction->setShortcut(QKeySequence::Open);
    openAction->setObjectName(QStringLiteral("OpenDocumentAction"));
    newAction->setObjectName(QStringLiteral("NewDocumentAction"));
    auto* closeDocumentAction = fileMenu->addAction(tr("Close Document"), this, [this] { closeDocument(activeDocumentId()); });
    closeDocumentAction->setObjectName("CloseDocumentAction");
    for (const auto& pair : {std::pair{"NextDocumentAction", 1}, std::pair{"PreviousDocumentAction", -1}}) {
        auto* action = new QAction(pair.second > 0 ? tr("Next document") : tr("Previous document"), this);
        action->setObjectName(pair.first);
        connect(action, &QAction::triggered, this, [this, direction=pair.second] {
            if (!documentTabs_ || documents_.size() < 2) return;
            const auto ids = documentIds();
            const auto current = std::ranges::find(ids, activeDocumentId()) - ids.begin();
            const auto next = (current + direction + std::ptrdiff_t(ids.size())) % std::ptrdiff_t(ids.size());
            activateDocument(ids[std::size_t(next)]);
        });
    }
    auto* save = fileMenu->addAction(QStringLiteral("&Save"), this, [this] { saveDocument(); });
    save->setShortcut(QKeySequence::Save); save->setObjectName(QStringLiteral("SaveDocumentAction"));
    registerEditorWindowAction(save);
    auto* saveAs = fileMenu->addAction(QStringLiteral("Save &As…"), this, [this] { saveDocument(true); });
    saveAs->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    saveAs->setObjectName(QStringLiteral("SaveDocumentAsAction")); registerEditorWindowAction(saveAs);
    auto* exportAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-export"),
        toolGlyph(ToolGlyph::ExternalLink)), tr("Export…"), this, [this] { exportImage(); });
    exportAction->setObjectName(QStringLiteral("ExportImageAction"));
    exportAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+E"))); registerEditorWindowAction(exportAction);
    auto* exportAgain = fileMenu->addAction(QStringLiteral("Export Again…"), this, [this] { exportImage(true); });
    exportAgain->setObjectName(QStringLiteral("ExportAgainAction"));
    exportAgain->setToolTip(QStringLiteral("Reuse the last successful export settings; always confirm before replacing an existing image."));
    registerEditorWindowAction(exportAgain);
    recentMenu_ = fileMenu->addMenu(QStringLiteral("Open &Recent"));
    connect(recentMenu_, &QMenu::aboutToShow, this, &MainWindow::refreshRecentMenu);
    auto* importAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-import")),
        QStringLiteral("Import as Layer…"), this, &MainWindow::importImageAsLayer);
    importAction->setObjectName(QStringLiteral("ImportImageAction"));
    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(QStringLiteral("Quit"), this, &QWidget::close);
    quitAction->setObjectName(QStringLiteral("QuitAction"));
    quitAction->setShortcut(QKeySequence::Quit);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(undoAction_);
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    auto* paste = editMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-paste")),
        QStringLiteral("Paste Image as Layer"), this, &MainWindow::pasteImageAsLayer);
    paste->setObjectName(QStringLiteral("PasteImageAsLayerAction"));
    paste->setShortcut(QKeySequence::Paste);
    registerEditorWindowAction(paste);
    editMenu->addSeparator();
    editMenu->addAction(transformAction_);
    editMenu->addAction(transformPixelsAction_);
    for (auto* fill : fillActions_) editMenu->addAction(fill);
    editMenu->addSeparator();
    auto* addLayer = editMenu->addAction(toolGlyph(ToolGlyph::NewLayer),
        QStringLiteral("New Raster Layer"), this, &MainWindow::addRasterLayer);
    addLayer->setObjectName(QStringLiteral("NewRasterLayerAction"));
    addLayer->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
    editMenu->addAction(deleteLayerAction_);
    editMenu->addAction(selectionActions_[3]);
    editMenu->addSeparator();
    auto* preferences = editMenu->addAction(tr("Preferences…"), this, &MainWindow::showPreferences);
    preferences->setObjectName(QStringLiteral("PreferencesAction"));

    auto* layerMenu=menuBar()->addMenu(QStringLiteral("&Layers"));
    newAdjustmentLayerAction_=new QAction(toolGlyph(ToolGlyph::AdjustmentLayer),tr("New Adjustment Layer"),this);
    newAdjustmentLayerAction_->setObjectName(QStringLiteral("NewAdjustmentLayerAction"));
    connect(newAdjustmentLayerAction_,&QAction::triggered,this,&MainWindow::addAdjustmentLayer);
    layerMenu->addAction(newAdjustmentLayerAction_);
    editMenu->addAction(newAdjustmentLayerAction_);
    layerMenu->addAction(newFolderAction_);layerMenu->addAction(renameLayerAction_);
    layerMenu->addSeparator();layerMenu->addAction(groupLayersAction_);layerMenu->addAction(ungroupLayersAction_);
    layerMenu->addSeparator();layerMenu->addAction(mergeLayersAction_);
    layerMenu->addAction(rasterizeLayersAction_);
    auto* maskMenu=layerMenu->addMenu(toolGlyph(ToolGlyph::LayerMask),tr("Layer Mask"));
    for(auto* action:maskActions_)maskMenu->addAction(action);
    layerMenu->addSeparator();
    for (auto* action : layerVisibilityActions_) layerMenu->addAction(action);

    auto* selectionMenu = menuBar()->addMenu(QStringLiteral("&Select"));
    selectionMenu->setObjectName(QStringLiteral("SelectMenu"));
    connect(selectionMenu, &QMenu::aboutToShow, this, &MainWindow::refreshSelectionControls);
    for (std::size_t i = 0; i < 3; ++i) selectionMenu->addAction(selectionActions_[i]);
    selectionMenu->addAction(selectionActions_[4]);
    selectionMenu->addSeparator();
    selectionMenu->addAction(toolActionMap_.at(core::ToolId::SelectByColor));
    selectionMenu->addAction(selectionGrowAction_);
    selectionMenu->addSeparator();
    selectionMenu->addAction(transformSelectionAction_);
    selectionMenu->addAction(transformPixelsAction_);

    auto* imageMenu = menuBar()->addMenu(QStringLiteral("&Image"));
    auto* canvasSizeAction = imageMenu->addAction(
        QStringLiteral("Change Canvas Size…"), this, &MainWindow::changeCanvasSize);
    canvasSizeAction->setObjectName(QStringLiteral("ChangeCanvasSizeAction"));
    canvasSizeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+C")));

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    snappingAction_ = viewMenu->addAction(tr("Snapping"));
    snappingAction_->setObjectName(QStringLiteral("SnappingAction"));
    auto* snapTargets = viewMenu->addMenu(tr("Snap To"));
    snapCanvasAction_ = snapTargets->addAction(tr("Canvas"));
    snapLayersAction_ = snapTargets->addAction(tr("Layers"));
    snapCanvasAction_->setObjectName(QStringLiteral("SnapCanvasAction"));
    snapLayersAction_->setObjectName(QStringLiteral("SnapLayersAction"));
    const std::array snapActions {snappingAction_, snapCanvasAction_, snapLayersAction_};
    const std::array snapKeys {QStringLiteral("view/snapping/enabled"), QStringLiteral("view/snapping/canvas"), QStringLiteral("view/snapping/layers")};
    for (std::size_t i = 0; i < snapActions.size(); ++i) {
        snapActions[i]->setCheckable(true);
        snapActions[i]->setChecked(!persistWindowState_ || QSettings().value(snapKeys[i], true).toBool());
    }
    const auto updateSnapping = [this, snapTargets, snapActions, snapKeys] {
        snappingOptions_.enabled = snappingAction_->isChecked();
        snappingOptions_.canvas = snapCanvasAction_->isChecked();
        snappingOptions_.layers = snapLayersAction_->isChecked();
        snapTargets->setEnabled(snappingOptions_.enabled);
        if (persistWindowState_) {
            QSettings settings;
            for (std::size_t i = 0; i < snapActions.size(); ++i) settings.setValue(snapKeys[i], snapActions[i]->isChecked());
        }
        // Re-evaluate from the stored raw pointer, even without a mouse move.
        canvasWindow_->updateTransformModifiers(canvasWindow_->pointerModifiers());
        if (selectionGesture_ && selectionGesture_->moving)
            moveSelectionGesture(selectionGesture_->pointer);
    };
    for (auto* action : snapActions) connect(action, &QAction::toggled, this, updateSnapping);
    updateSnapping();
    viewMenu->addSeparator();
    pixelPreview_=std::make_unique<PixelPreview>();
    pixelPreviewAction_=viewMenu->addAction(tr("Pixel Preview"));
    pixelPreviewAction_->setObjectName(QStringLiteral("PixelPreviewAction"));
    pixelPreviewAction_->setCheckable(true);
    pixelPreviewAction_->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_P));
    pixelPreviewAction_->setAutoRepeat(false);
    registerEditorWindowAction(pixelPreviewAction_);
    pixelPreviewAction_->setToolTip(tr("Preview the native document pixels used by lossless PNG output; source layers remain editable."));
    auto* previewBadge = new QWidget(workspace_->panelOverlay());
    previewBadge->setObjectName(QStringLiteral("PixelPreviewBadge"));
    previewBadge->setAttribute(Qt::WA_StyledBackground);
    auto* badgeRow = new QHBoxLayout(previewBadge);
    badgeRow->setContentsMargins(10, 4, 4, 4);
    badgeRow->setSpacing(10);
    auto* previewLabel = new QLabel(tr("Pixel Preview · Shift+P"), previewBadge);
    previewLabel->setObjectName("PixelPreviewBadgeLabel");
    badgeRow->addWidget(previewLabel);
    auto* exitPreview = new QPushButton(tr("Exit"), previewBadge);
    exitPreview->setObjectName(QStringLiteral("ExitPixelPreview"));
    exitPreview->setFocusPolicy(Qt::NoFocus);
    exitPreview->setToolTip(tr("Exit Pixel Preview · Shift+P"));
    badgeRow->addWidget(exitPreview);
    connect(exitPreview, &QPushButton::clicked, this, [this] { pixelPreviewAction_->setChecked(false); });
    workspace_->setViewModeOverlay(previewBadge, false);
    canvasWindow_->onDocumentPresentationChanged=[this](const core::DocumentSnapshot& snapshot){
        updateDocumentResources();
        if(canvasWindow_->scene().effectBypassLayer || canvasWindow_->scene().adjustmentBypassLayer){
            auto before=snapshot;
            for(auto& layer:before.layersBottomToTop) {
                if(layer.id==canvasWindow_->scene().effectBypassLayer){layer.effects.reset();layer.effectCache.reset();}
                if(layer.id==canvasWindow_->scene().adjustmentBypassLayer){layer.adjustments.reset();layer.filters.reset();layer.filterCache.reset();layer.effectCache.reset();}
            }
            pixelPreview_->request(before);
        }else pixelPreview_->request(snapshot);
    };
    pixelPreview_->onReady=[this](auto pixels,QString error){
        if(!error.isEmpty()) {
            pixelPreviewAction_->setChecked(false);
            statusBar()->showMessage(tr("Pixel Preview unavailable: %1. Source layers are unchanged.").arg(error),8000);
            return;
        }
        canvasWindow_->setPixelPreview(pixelPreviewAction_->isChecked(),std::move(pixels));
    };
    connect(pixelPreviewAction_,&QAction::toggled,this,[this,previewBadge](bool enabled){
        pixelPreview_->setEnabled(enabled);canvasWindow_->setPixelPreview(enabled);
        workspace_->setViewModeOverlay(previewBadge, enabled);
        if(enabled)pixelPreview_->request(canvasWindow_->scene().document);
        if(persistWindowState_){QSettings settings;settings.setValue(QStringLiteral("view/pixelPreview"),enabled);}
    });
    if(persistWindowState_){QSettings settings;pixelPreviewAction_->setChecked(settings.value(QStringLiteral("view/pixelPreview"),false).toBool());}
    viewMenu->addAction(layerOutlinesAction_);
    auto* rulersMenu = viewMenu->addMenu(QStringLiteral("Rulers"));
    horizontalRulerAction_ = rulersMenu->addAction(QStringLiteral("Horizontal ruler"));
    verticalRulerAction_ = rulersMenu->addAction(QStringLiteral("Vertical ruler"));
    horizontalRulerAction_->setObjectName(QStringLiteral("ViewHorizontalRuler"));
    verticalRulerAction_->setObjectName(QStringLiteral("ViewVerticalRuler"));
    for (auto* action : {horizontalRulerAction_, verticalRulerAction_}) action->setCheckable(true);
    connect(horizontalRulerAction_, &QAction::toggled, this, [this](bool visible) { workspace_->setRulerVisible(Qt::Horizontal, visible); refreshRulerView(); });
    connect(verticalRulerAction_, &QAction::toggled, this, [this](bool visible) { workspace_->setRulerVisible(Qt::Vertical, visible); refreshRulerView(); });
    workspace_->onRulerPlacementChanged = [this] {
        const QSignalBlocker h(horizontalRulerAction_), v(verticalRulerAction_);
        horizontalRulerAction_->setChecked(workspace_->rulerVisible(Qt::Horizontal));
        verticalRulerAction_->setChecked(workspace_->rulerVisible(Qt::Vertical));
        refreshRulerView();
    };
    const auto edgeMenu = [this, rulersMenu](Qt::Orientation axis, const QString& name, const QString& near, const QString& far) {
        auto* menu = rulersMenu->addMenu(name);
        menu->addAction(near, this, [this, axis] { workspace_->setRulerFarEdge(axis, false); });
        menu->addAction(far, this, [this, axis] { workspace_->setRulerFarEdge(axis, true); });
    };
    edgeMenu(Qt::Horizontal, QStringLiteral("Horizontal position"), QStringLiteral("Top"), QStringLiteral("Bottom"));
    edgeMenu(Qt::Vertical, QStringLiteral("Vertical position"), QStringLiteral("Left"), QStringLiteral("Right"));
    viewMenu->addSeparator();
    auto* fitAction = viewMenu->addAction(QStringLiteral("Fit Canvas"), this,
        [this] { canvasWindow_->fitDocumentToView(); });
    fitAction->setObjectName(QStringLiteral("FitCanvasAction"));
    fitAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    auto* actualAction = viewMenu->addAction(QStringLiteral("100%"), this,
        [this] { canvasWindow_->resetTo100Percent(); });
    actualAction->setObjectName(QStringLiteral("ActualSizeAction"));
    actualAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));

    viewMenu->addSeparator();
    auto* panelsMenu = viewMenu->addMenu(QStringLiteral("Panels"));
    colorPanelAction_ = panelsMenu->addAction(QStringLiteral("Color"));
    layersPanelAction_ = panelsMenu->addAction(QStringLiteral("Layers"));
    propertiesPanelAction_ = panelsMenu->addAction(QStringLiteral("Properties"));
    adjustmentsPanelAction_ = panelsMenu->addAction(QStringLiteral("Adjustments"));
    adjustmentsPanelAction_->setObjectName(QStringLiteral("AdjustmentsPanelVisibilityAction"));
    adjustmentsPanelAction_->setCheckable(true);
    adjustmentsPanelAction_->setChecked(true);
    connect(adjustmentsPanelAction_, &QAction::triggered, this, [this](bool visible) {
        workspace_->setPanelVisible(adjustmentsPanelShell_, visible);
        if (visible) {
            capturedFilterRegionActive_=adjustmentsPanel_->filtersCategoryActive();
            refreshAdjustmentPanel();refreshFiltersPanel();
        }
    });
    auto* filtersMenu=menuBar()->addMenu(tr("Filters"));
    for(const auto type:core::allSpatialFilterTypes){
        auto* action=filtersMenu->addAction(QString::fromUtf8(core::spatialFilterName(type).data()));
        action->setObjectName(QStringLiteral("FilterAction_%1").arg(int(type)));
        connect(action,&QAction::triggered,this,[this,type]{
            capturedFilterRegionActive_=true;
            adjustmentsPanelAction_->setChecked(true);workspace_->setPanelVisible(adjustmentsPanelShell_,true);
            refreshAdjustmentPanel();refreshFiltersPanel();adjustmentsPanel_->showFilter(type);
        });
    }
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->setObjectName(QStringLiteral("HelpMenu"));
    // Intentionally no URL/action until the documentation site is available.
    auto* documentation = helpMenu->addAction(tr("Documentation"));
    documentation->setObjectName(QStringLiteral("DocumentationAction"));
    documentation->setToolTip(tr("Documentation will be available here in a future update."));
    auto* feedback = helpMenu->addAction(tr("Submit feedback / bug report…"), this, &MainWindow::showFeedback);
    feedback->setObjectName(QStringLiteral("FeedbackAction"));
    feedback->setEnabled(platform::ServiceConfig::load().enabled());
    auto* about = helpMenu->addAction(tr("About Vulkana"), this, &MainWindow::showAbout);
    about->setObjectName(QStringLiteral("AboutVulkanaAction"));
    about->setMenuRole(QAction::AboutRole);
    colorPanelAction_->setObjectName(QStringLiteral("ColorPanelVisibilityAction"));
    colorPanelAction_->setCheckable(true);
    layersPanelAction_->setCheckable(true);
    propertiesPanelAction_->setCheckable(true);
    colorPanelAction_->setChecked(true);
    layersPanelAction_->setChecked(true);
    propertiesPanelAction_->setChecked(true);
    connect(colorPanelAction_, &QAction::triggered, this, [this](bool visible) {
        workspace_->setPanelVisible(colorPanelShell_, visible);
    });
    connect(layersPanelAction_, &QAction::triggered, this, [this](bool visible) {
        workspace_->setPanelVisible(layersPanelShell_, visible);
    });
    connect(propertiesPanelAction_, &QAction::triggered, this, [this](bool visible) {
        workspace_->setPanelVisible(propertiesPanelShell_, visible);
    });

    viewMenu->addSeparator();
    auto* toolRailMenu = viewMenu->addMenu(QStringLiteral("Tool Rail Position"));
    toolRailDockActions_ = new QActionGroup(this);
    toolRailDockActions_->setExclusive(true);
    const auto addToolDockAction = [this, toolRailMenu](const QString& label,
                                       const QString& objectName,
                                       ToolRailDockLocation location) {
        auto* action = toolRailMenu->addAction(label);
        action->setObjectName(objectName);
        action->setCheckable(true);
        toolRailDockActions_->addAction(action);
        connect(action, &QAction::triggered, this,
            [this, location] { setToolRailDockLocation(location); });
        return action;
    };
    toolRailLeftAction_ = addToolDockAction(QStringLiteral("Left"),
        QStringLiteral("ToolRailDockLeftAction"), ToolRailDockLocation::Left);
    toolRailRightAction_ = addToolDockAction(QStringLiteral("Inside Right Panels"),
        QStringLiteral("ToolRailDockRightAction"), ToolRailDockLocation::RightPanel);
    toolRailTopAction_ = addToolDockAction(QStringLiteral("Top"),
        QStringLiteral("ToolRailDockTopAction"), ToolRailDockLocation::Top);
    toolRailBottomAction_ = addToolDockAction(QStringLiteral("Bottom"),
        QStringLiteral("ToolRailDockBottomAction"), ToolRailDockLocation::Bottom);
    toolRailLeftAction_->setChecked(true);

}

void MainWindow::createToolOptionsBar()
{
    toolOptionsBar_ = new ToolOptionsBar(this);
    brushOptionsPage_ = new BrushOptionsPage;
    transformOptionsPage_ = new TransformOptionsPage;
    moveOptionsPage_ = new TransformOptionsPage(nullptr, TransformOptionsPage::Mode::Move);
    moveOptionsPage_->setTransformAction(transformAction_);
    transformOptionsPage_->setTransformAction(transformAction_);
    toolOptionsBar_->registerToolPage(core::ToolId::Move, QStringLiteral("Move"), moveOptionsPage_);
    toolOptionsBar_->registerToolPage(
        core::ToolId::Transform, QStringLiteral("Transform"), transformOptionsPage_);
    toolOptionsBar_->registerToolPage(
        core::ToolId::Brush, QStringLiteral("Brush"), brushOptionsPage_);
    addToolBar(Qt::TopToolBarArea, toolOptionsBar_);
    brushOptionsPage_->setBrushSettings(brushSettings_);
    toolOptionsBar_->setActiveTool(core::ToolId::Move);
    createSelectionControls();
    createFillControls();
    createCloningControls();
    createLocalBlurControls();
    createEyedropperControls();
    auto* textPage = new QWidget;
    auto* textRow = new QHBoxLayout(textPage);
    textRow->setContentsMargins(0, 0, 0, 0);
    editTextButton_ = new ToolOptionsButton(QStringLiteral("EditText"), QStringLiteral("Edit text"),
        QStringLiteral("Edit the selected text layer, including an empty layer"),
        ToolOptionsButton::Kind::Action, textPage);
    textRow->addWidget(editTextButton_);
    toolOptionsBar_->registerToolPage(core::ToolId::Text, QStringLiteral("Text"), textPage);
    connect(editTextButton_, &QToolButton::clicked, this, [this] {
        if (textController_ && session().activeLayer())
            textController_->editLayer(*session().activeLayer());
    });
}

void MainWindow::createToolRail()
{
    toolRail_ = new QToolBar(
        QStringLiteral("Tools"), workspace_->panelOverlay());
    toolRail_->setObjectName(QStringLiteral("ToolRail"));
    toolRail_->setAllowedAreas(Qt::NoToolBarArea);
    toolRail_->setMovable(false);
    toolRail_->setFloatable(false);
    toolRail_->setOrientation(Qt::Vertical);
    toolRail_->setIconSize({22, 22});

    auto* dragHandle = new ToolRailDragHandle(toolRail_,
        [this](QWidget* source) { beginToolRailDockDrag(source); });
    toolRail_->addWidget(dragHandle);

    toolAction(core::ToolId::Move, QStringLiteral("Move"), QStringLiteral("move"),
        QKeySequence(QStringLiteral("V")));
    toolAction(core::ToolId::Crop,QStringLiteral("Layer Crop"),QStringLiteral("crop"),QKeySequence(QStringLiteral("C")));
    toolAction(core::ToolId::Marquee, QStringLiteral("Select"), QStringLiteral("marquee"),
        QKeySequence(QStringLiteral("M")));
    toolAction(core::ToolId::Lasso, QStringLiteral("Lasso"), QStringLiteral("lasso"),
        QKeySequence(QStringLiteral("L")));
    toolAction(core::ToolId::SelectByColor, QStringLiteral("Select by Color"), QStringLiteral("colorselect"),
        QKeySequence(QStringLiteral("Shift+O")));
    toolAction(core::ToolId::SmartSelect,QStringLiteral("Smart Select"),QStringLiteral("smartselect"),QKeySequence(QStringLiteral("W")));
    toolRail_->addSeparator();
    toolAction(core::ToolId::Brush, QStringLiteral("Brush"), QStringLiteral("brush"),
        QKeySequence(QStringLiteral("B")));
    eraserAction_ = toolAction(core::ToolId::Eraser,
        QStringLiteral("Erase mode"), QStringLiteral("eraser"),
        QKeySequence(QStringLiteral("E")));
    eraserAction_->setAutoRepeat(false);
    brushOptionsPage_->setEraserAction(eraserAction_);
    toolAction(core::ToolId::Cloning, QStringLiteral("Cloning"), QStringLiteral("cloning"),
        QKeySequence(QStringLiteral("S")))->setIcon(toolGlyph(ToolGlyph::Cloning));
    toolAction(core::ToolId::LocalBlur, QStringLiteral("Local Blur"), QStringLiteral("local_blur"),
        QKeySequence(QStringLiteral("K")))->setIcon(toolGlyph(ToolGlyph::LocalBlur));
    toolAction(core::ToolId::Fill, QStringLiteral("Fill"), QStringLiteral("fill"),
        QKeySequence(QStringLiteral("G")));
    toolRail_->addSeparator();
    toolAction(core::ToolId::Text, QStringLiteral("Text"), QStringLiteral("text"),
        QKeySequence(QStringLiteral("T")));
    toolAction(core::ToolId::Shape, QStringLiteral("Shape"), QStringLiteral("shape"),
        QKeySequence(QStringLiteral("U")))->setIcon(toolGlyph(ToolGlyph::ShapeRectangle));
    toolRail_->addSeparator();
    auto* measure = toolAction(core::ToolId::Measure, QStringLiteral("Measure"), QStringLiteral("measure"),
        QKeySequence(QStringLiteral("Shift+R")));
    measure->setAutoRepeat(false);
    measure->setToolTip(QStringLiteral("Measure (Shift+R) · Hold R for temporary measurement"));
    toolAction(core::ToolId::Eyedropper, QStringLiteral("Eyedropper"), QStringLiteral("eyedropper"),
        QKeySequence(QStringLiteral("I")));
    railColors_ = new ColorSelector(ColorSelector::Presentation::Compact, toolRail_);
    railColors_->onColorsChanged = [this](core::EditorColors colors) { setColors(colors); };
    toolRail_->addWidget(railColors_);
    auto* swapColors = new QAction(QStringLiteral("Switch active color"), this);
    swapColors->setObjectName(QStringLiteral("SwapColorsAction"));
    swapColors->setShortcut(QKeySequence(QStringLiteral("X")));
    swapColors->setAutoRepeat(false);
    registerEditorWindowAction(swapColors);
    connect(swapColors, &QAction::triggered, this, [this] {
        auto colors = session().colors();
        colors.switchActive();
        setColors(colors);
    });

    createToolRailDockTargets();
    updateToolRailPresentation();
    workspace_->setToolRail(
        toolRail_, OverlayDockWorkspace::ToolRailPlacement::Left);
}

void MainWindow::createDocks()
{
    colorPanel_ = new ColorPanel;
    colorPanel_->setColors(session().colors());
    colorPanel_->onColorsChanged = [this](core::EditorColors colors) { setColors(colors); };
    colorPanelShell_ = new WorkspacePanel(
        QStringLiteral("Color"), colorPanel_);
    colorPanelShell_->setObjectName(QStringLiteral("ColorPanelShell"));
    colorPanelShell_->setHeightRange(
        uiLayoutConfig_.color.minimum, uiLayoutConfig_.color.maximum);
    workspace_->addPanel(colorPanelShell_,
        OverlayDockWorkspace::PanelDockSide::Right);

    auto* layersBody = new QWidget;
    auto* layersLayout = new QVBoxLayout(layersBody);
    layersLayout->setContentsMargins(0, 0, 0, 8);
    layersLayout->setSpacing(8);
    auto* layerControls = new QWidget;
    layerControls->setObjectName(QStringLiteral("LayerControls"));
    auto* layerControlsLayout = new QVBoxLayout(layerControls);
    layerControlsLayout->setContentsMargins(10, 9, 10, 2);
    layerControlsLayout->setSpacing(8);

    auto* blendModeRow = new QWidget;
    auto* blendModeLayout = new QHBoxLayout(blendModeRow);
    blendModeLayout->setContentsMargins(0, 0, 0, 0);
    blendModeLayout->setSpacing(8);
    auto* blendModeLabel = new QLabel(QStringLiteral("Blend Mode"));
    blendModeLabel->setObjectName(QStringLiteral("LayerControlLabel"));
    blendModeLabel->setMinimumWidth(78);
    blendModeCombo_ = new QComboBox;
    blendModeCombo_->setObjectName(QStringLiteral("LayerBlendModeCombo"));
    populateBlendModeCombo(*blendModeCombo_);
    blendModeCombo_->setCurrentIndex(0);
    blendModeCombo_->setEnabled(false);
    blendModeCombo_->setToolTip(
        QStringLiteral("How the active layer blends with visible layers underneath"));
    blendModeLabel->setBuddy(blendModeCombo_);
    blendModeLayout->addWidget(blendModeLabel);
    blendModeLayout->addWidget(blendModeCombo_, 1);
    layerControlsLayout->addWidget(blendModeRow);

    opacitySlider_ = new CompactValueControl;
    opacitySlider_->setObjectName(QStringLiteral("LayerOpacitySlider"));
    opacitySlider_->setDecimals(0);
    opacitySlider_->setRange(0, 100);
    opacitySlider_->setSingleStep(1);
    opacitySlider_->setValue(100);
    opacitySlider_->setPrefix(QStringLiteral("Opacity: "));
    opacitySlider_->setSuffix(QStringLiteral("%"));
    opacitySlider_->setAccessibleName(QStringLiteral("Layer opacity"));
    opacitySlider_->setToolTip(QStringLiteral("Layer opacity — drag, type a number, or use the arrows"));
    opacitySlider_->setFixedHeight(30);
    opacitySlider_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layerControlsLayout->addWidget(opacitySlider_);
    layersLayout->addWidget(layerControls);

    layerList_ = new LayerListView;
    layerList_->setObjectName(QStringLiteral("LayerList"));
    layerList_->setModel(layerModel_);
    static_cast<LayerListView*>(layerList_)->onRenameEditorChanged = [this](QLineEdit* editor) {
        focusLayerRenameEditor(editor);
    };
    const auto queueRenameFocus = [this] {
        const auto editor = layerRenameEditor_;
        if (editor) QTimer::singleShot(0,this,[this,editor] {
            if (editor && editor == layerRenameEditor_) restoreLayerRenameFocus();
        });
    };
    connect(qApp,&QApplication::focusChanged,this,[this,queueRenameFocus](QWidget*,QWidget* now) {
        if (layerRenameEditor_ && now != layerRenameEditor_) queueRenameFocus();
    });
    connect(qApp,&QGuiApplication::focusWindowChanged,this,queueRenameFocus);
    layerList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    static_cast<LayerListView*>(layerList_)->onRowSelectionRequested = [this](int row,Qt::KeyboardModifiers modifiers) {
        selectLayerFromRow(row,modifiers);
    };
    static_cast<LayerListView*>(layerList_)->onContextSelectionRequested = [this](int row) {
        selectLayerFromRow(row, Qt::NoModifier, true);
    };
    static_cast<LayerListView*>(layerList_)->onEmptySpacePressed = [this] {
        collapseLayerSelectionOnEmptyClick();
    };
    layerList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layerList_->setDragEnabled(true);
    layerList_->setAcceptDrops(true);
    layerList_->setDropIndicatorShown(true);
    layerList_->setDragDropMode(QAbstractItemView::DragDrop);
    layerList_->setDefaultDropAction(Qt::MoveAction);
    layerList_->setDragDropOverwriteMode(false);
    layerList_->setIconSize({34, 34});
    static_cast<LayerListView*>(layerList_)->onEditingTargetRequested=[this](int row,bool mask){setLayerEditingTarget(row,mask);};
    layerList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(layerList_,&QWidget::customContextMenuRequested,this,&MainWindow::showLayerItemMenu);
    layersLayout->addWidget(layerList_, 1);

    auto* buttons = new QWidget;
    auto* buttonLayout = new QHBoxLayout(buttons);
    buttonLayout->setContentsMargins(10, 0, 10, 0);
    buttonLayout->setSpacing(6);
    auto* addButton = new QPushButton(toolGlyph(ToolGlyph::NewLayer), QString());
    addButton->setObjectName(QStringLiteral("AddLayerButton"));
    addButton->setToolTip(tr("Add raster layer"));
    deleteLayerButton_ = new QPushButton(toolGlyph(ToolGlyph::Trash), QString());
    deleteLayerButton_->setObjectName(QStringLiteral("DeleteLayerButton"));
    deleteLayerButton_->setToolTip(tr("Delete selected layers"));
    buttonLayout->addWidget(addButton);
    auto* folderButton=new QPushButton(toolGlyph(ToolGlyph::Folder),QString());
    folderButton->setObjectName(QStringLiteral("NewFolderButton"));
    folderButton->setToolTip(QStringLiteral("New folder · organizes layers without changing compositing"));
    connect(folderButton,&QPushButton::clicked,newFolderAction_,&QAction::trigger);
    buttonLayout->addWidget(folderButton);
    addMaskButton_=new QPushButton(toolGlyph(ToolGlyph::LayerMask),QString());
    addMaskButton_->setObjectName(QStringLiteral("AddLayerMaskButton"));
    addMaskButton_->setToolTip(tr("Add layer mask"));
    connect(addMaskButton_,&QPushButton::clicked,maskActions_[0],&QAction::trigger);
    buttonLayout->addWidget(addMaskButton_);
    auto* adjustmentButton=new QPushButton(toolGlyph(ToolGlyph::AdjustmentLayer),QString());
    adjustmentButton->setObjectName(QStringLiteral("AddAdjustmentLayerButton"));
    adjustmentButton->setToolTip(tr("New adjustment layer"));
    connect(adjustmentButton,&QPushButton::clicked,newAdjustmentLayerAction_,&QAction::trigger);
    connect(newAdjustmentLayerAction_,&QAction::changed,adjustmentButton,[this,adjustmentButton]{adjustmentButton->setEnabled(newAdjustmentLayerAction_->isEnabled());});
    buttonLayout->addWidget(adjustmentButton);
    for(auto* button:{addButton,deleteLayerButton_,folderButton,addMaskButton_,adjustmentButton}) {
        button->setStyleSheet(QStringLiteral("QPushButton { padding: 0; min-width: 30px; max-width: 30px; min-height: 28px; max-height: 28px; }"));
        button->setFixedSize(32,30);button->setIconSize({18,18});button->setAccessibleName(button->toolTip());
    }
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(deleteLayerButton_);
    layersLayout->addWidget(buttons);
    layersPanelShell_ = new WorkspacePanel(
        QStringLiteral("Layers"), layersBody);
    layersPanelShell_->setObjectName(QStringLiteral("LayersPanel"));
    layersPanelShell_->setHeightRange(
        uiLayoutConfig_.layers.minimum, uiLayoutConfig_.layers.maximum);
    workspace_->addPanel(layersPanelShell_,
        OverlayDockWorkspace::PanelDockSide::Left);

    propertiesPanel_ = new PropertiesPanel(brushAssets_);
    propertiesPanel_->setBrushPresets(brushPresets_);
    propertiesPanelShell_ = new WorkspacePanel(
        QStringLiteral("Properties"), propertiesPanel_);
    propertiesPanelShell_->setObjectName(QStringLiteral("PropertiesPanelShell"));
    propertiesPanelShell_->setHeightRange(
        uiLayoutConfig_.properties.minimum,
        uiLayoutConfig_.properties.maximum);
    workspace_->addPanel(propertiesPanelShell_,
        OverlayDockWorkspace::PanelDockSide::Right);
    createAdjustmentsPanel();
    createFiltersPanel();
    createEffectsPanel();
    // Saved custom layout (2026-09-13): Layers/tools left; Color, Properties,
    // Adjustments right. Existing preferences still override these defaults.
    workspace_->setLeftPanelWidth(305);
    workspace_->setPanelWidth(556);
    // Qt splitter state: vertical, 8px handles, relative heights 120/470/630.
    // Do not copy monitor-specific window geometry or obsolete Filters-panel state.
    workspace_->restoreDockedPanelSizes(OverlayDockWorkspace::PanelDockSide::Right,
        QByteArray::fromHex("000000ff000000010000000300000078000001d6000002760000000008010000000200"));

    connect(layerList_->selectionModel(), &QItemSelectionModel::currentChanged, this,
        [this](const QModelIndex& current) { selectLayerFromRow(current.row()); });
    connect(blendModeCombo_, &QComboBox::activated, this, [this](int index) {
        if (updatingUi_ || fileBusy_ || !session().activeLayer() || index < 0
            || index >= int(core::allBlendModes.size())) return;
        const auto mode = blendModeAt(*blendModeCombo_, index);
        if (!mode) return;
        const auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || layer->blendMode == *mode) return;
        // A blend change must not silently discard completed Ctrl+T gestures
        // when the general document-command boundary settles pending tools.
        if (layerTransform_ || layerCrop_ || selectionTransform_) {
            finishCanvasOperation();
            if (layerTransform_ || layerCrop_ || selectionTransform_) { updateLayerControls(); return; }
        }
        if (executeDocumentCommand(std::make_unique<core::SetLayerBlendModeCommand>(
                *session().activeLayer(), *mode))) {
            fileState().untouched = false;
            synchronizeUi(false, false);
        }
    });
    connect(opacitySlider_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (updatingUi_ || !session().activeLayer()) {
            return;
        }
        const QScopedValueRollback publishing(publishingOpacity_, true);
        if (executeDocumentCommand(std::make_unique<core::SetLayerOpacityCommand>(
                *session().activeLayer(), static_cast<float>(value) / 100.0F,
                activeOpacityMergeKey_))) {
            fileState().untouched = false;
            synchronizeUi(false, false);
        }
    });
    opacitySlider_->onInteractionStarted = [this] {
        activeOpacityMergeKey_ = nextOpacityMergeKey_++;
        if (nextOpacityMergeKey_ == 0) {
            nextOpacityMergeKey_ = 1;
        }
    };
    opacitySlider_->onInteractionFinished = [this] {
        activeOpacityMergeKey_ = 0;
    };
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addRasterLayer);
    connect(deleteLayerButton_, &QPushButton::clicked, this, &MainWindow::deleteActiveLayer);
}

void MainWindow::createStatusBar(bool showCanvasFps)
{
    documentStatus_ = new ElidedStatusLabel;
    documentStatus_->setObjectName(QStringLiteral("DocumentStatus"));
    documentStatus_->setTextFormat(Qt::PlainText);
    documentStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    documentStatus_->setMinimumWidth(0);
    selectionStatus_ = new ElidedStatusLabel;
    selectionStatus_->setObjectName(QStringLiteral("SelectionStatus"));
    selectionStatus_->setTextFormat(Qt::PlainText);
    selectionStatus_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    selectionStatus_->setContentsMargins(12, 0, 12, 0);
    selectionStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    selectionStatus_->setMinimumWidth(0);
    // The right lane already bounds this label. Let it absorb spare width so
    // the trailing zoom readout stays pinned to the edge on wide workspaces.
    toolContextStatus_ = new ElidedStatusLabel;
    toolContextStatus_->setObjectName(QStringLiteral("ToolContextStatus"));
    toolContextStatus_->setTextFormat(Qt::PlainText);
    toolContextStatus_->setAlignment(Qt::AlignCenter);
    toolContextStatus_->setContentsMargins(8, 0, 8, 0);
    toolContextStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolContextStatus_->setMinimumWidth(0);
    zoomStatus_ = new QLabel;
    documentStatus_->setContentsMargins(8, 0, 8, 0);
    zoomStatus_->setContentsMargins(8, 0, 8, 0);
    auto* right = new QWidget;
    auto* rightLayout = new QHBoxLayout(right);
    rightLayout->setContentsMargins(0,0,0,0);
    rightLayout->setSpacing(0);
    rightLayout->addWidget(selectionStatus_,1);
    if (showCanvasFps) {
        auto* canvasFps = new CanvasFpsLabel(*canvasWindow_, right);
        rightLayout->addWidget(canvasFps);
    }
    rightLayout->addWidget(new MemoryStatusLabel([this] {
        const auto stats = canvasWindow_->rendererStats();
        return MemoryStatusSnapshot{platform::processResidentMemoryBytes(),
                                    stats.textureCacheBytes, stats.stagingCapacityBytes};
    }, right));
    rightLayout->addWidget(zoomStatus_);
    statusBar()->addPermanentWidget(new StatusReadouts(statusBar(), documentStatus_, toolContextStatus_, right),1);
    // Keep informational lanes permanent. Existing operation/error messages
    // remain readable just above them, without hiding text or resizing canvas.
    statusNotification_ = new QLabel(workspace_->panelOverlay());
    statusNotification_->setObjectName(QStringLiteral("StatusNotification"));
    statusNotification_->setTextFormat(Qt::PlainText);
    statusNotification_->setWordWrap(true);
    statusNotification_->setAttribute(Qt::WA_TransparentForMouseEvents);
    statusNotification_->setFocusPolicy(Qt::NoFocus);
    statusNotification_->hide();
    statusNotificationTimer_ = new QTimer(this);
    statusNotificationTimer_->setObjectName(QStringLiteral("StatusNotificationTimer"));
    statusNotificationTimer_->setSingleShot(true);
    statusNotificationTimer_->setInterval(4000);
    connect(statusNotificationTimer_, &QTimer::timeout, statusBar(), &QStatusBar::clearMessage);
    connect(statusBar(), &QStatusBar::messageChanged, this, [this](const QString& text) {
        statusNotification_->setText(text);
        if (text.isEmpty()) statusNotificationTimer_->stop();
        else statusNotificationTimer_->start(); // Also bound messages with QStatusBar's default (infinite) timeout.
        updateStatusNotification();
    });
}

void MainWindow::updateStatusNotification()
{
    if (!statusNotification_ || !workspace_) return;
    if (statusNotification_->text().isEmpty() || toolHintPosition_ == 2) {
        workspace_->setPassiveOverlay(statusNotification_, false);
        return;
    }
    const auto area=workspace_->panelOverlay()->rect().adjusted(8,8,-8,-8);
    if (area.isEmpty()) { workspace_->setPassiveOverlay(statusNotification_, false); return; }
    const int width=std::min(area.width(),std::min(640,statusNotification_->sizeHint().width()));
    const int height=std::min(area.height(),statusNotification_->heightForWidth(width));
    const int x = toolHintPosition_ == 1 ? area.left() + (area.width() - width) / 2 : area.left();
    statusNotification_->setGeometry(x,area.bottom()-height+1,width,height);
    workspace_->setPassiveOverlay(statusNotification_, true);
}

void MainWindow::updateToolContextStatus()
{
    if (!toolContextStatus_) return;
    QString text, tooltip;
    // One stable contextual slot, independent of transient status messages and
    // selected-layer information. Future tools can contribute here as needed.
    if (session().activeTool() == core::ToolId::Cloning) {
        const auto* source = session().document() && cloneAnchor_
            ? session().document()->layer(cloneAnchor_->layer) : nullptr;
        text = source ? tr("Source: %1").arg(QString::fromStdString(source->name))
                      : tr("Alt-click to set a clone source");
        tooltip = source ? text + tr("\nAlt-click sets a new source. Changing the destination keeps this anchor.")
                         : tr("Select the source layer, then Alt-click a source point on the canvas.");
        if (cloneSettings_.mode == core::CloneMode::SpotHeal) {
            text = tr("Spot Heal · automatic source");
            tooltip = tr("Mark a blemish or scratch and release to repair. No source anchor needed. Escape cancels.");
        }
    }
    if (toolContextStatus_->text() != text) toolContextStatus_->setText(text);
    if (toolContextStatus_->toolTip() != tooltip) toolContextStatus_->setToolTip(tooltip);
}

void MainWindow::updatePanelAreaVisibility()
{
    if (!workspace_ || !colorPanelShell_ || !layersPanelShell_
        || !propertiesPanelShell_ || !colorPanelAction_
        || !layersPanelAction_ || !propertiesPanelAction_) {
        return;
    }
    workspace_->setPanelVisible(
        colorPanelShell_, colorPanelAction_->isChecked());
    workspace_->setPanelVisible(
        layersPanelShell_, layersPanelAction_->isChecked());
    workspace_->setPanelVisible(
        propertiesPanelShell_, propertiesPanelAction_->isChecked());
    workspace_->setPanelVisible(adjustmentsPanelShell_, adjustmentsPanelAction_->isChecked());
}

void MainWindow::createToolRailDockTargets()
{
    auto* targetHost = workspace_->panelOverlay();
    Q_ASSERT(targetHost);
    const auto createTarget = [this, targetHost](RailDockEdge edge,
                                  const QString& label,
                                  const QString& objectName,
                                  ToolRailDockLocation location) {
        auto* target = new ToolRailDockTarget(edge, label,
            [this, location] { pendingToolRailDockLocation_ = location; },
            targetHost);
        target->setObjectName(objectName);
        return target;
    };
    toolRailLeftTarget_ = createTarget(RailDockEdge::Left,
        QStringLiteral("LEFT"), QStringLiteral("ToolRailDockLeftTarget"),
        ToolRailDockLocation::Left);
    toolRailRightTarget_ = createTarget(RailDockEdge::Right,
        QStringLiteral("RIGHT"), QStringLiteral("ToolRailDockRightTarget"),
        ToolRailDockLocation::RightPanel);
    toolRailTopTarget_ = createTarget(RailDockEdge::Top,
        QStringLiteral("TOP"), QStringLiteral("ToolRailDockTopTarget"),
        ToolRailDockLocation::Top);
    toolRailBottomTarget_ = createTarget(RailDockEdge::Bottom,
        QStringLiteral("BOTTOM"), QStringLiteral("ToolRailDockBottomTarget"),
        ToolRailDockLocation::Bottom);
}

void MainWindow::positionToolRailDockTargets()
{
    auto* host = workspace_->panelOverlay();
    if (!host || host->width() <= 0 || host->height() <= 0) {
        return;
    }

    constexpr QSize targetSize {112, 58};
    constexpr int edgeInset = 18;
    const int centeredX = std::max(edgeInset,
        (host->width() - targetSize.width()) / 2);
    const int centeredY = std::max(edgeInset,
        (host->height() - targetSize.height()) / 2);
    const int rightX = std::max(edgeInset,
        host->width() - edgeInset - targetSize.width());
    const int bottomY = std::max(edgeInset,
        host->height() - edgeInset - targetSize.height());

    toolRailLeftTarget_->setGeometry(
        {edgeInset, centeredY, targetSize.width(), targetSize.height()});
    toolRailRightTarget_->setGeometry(
        {rightX, centeredY, targetSize.width(), targetSize.height()});
    toolRailTopTarget_->setGeometry(
        {centeredX, edgeInset, targetSize.width(), targetSize.height()});
    toolRailBottomTarget_->setGeometry(
        {centeredX, bottomY, targetSize.width(), targetSize.height()});
}

void MainWindow::beginToolRailDockDrag(QWidget* source)
{
    if (!source || !workspace_ || !workspace_->panelOverlay()) {
        return;
    }

    pendingToolRailDockLocation_.reset();
    showToolRailDockTargets();

    QPixmap dragToken(58, 48);
    dragToken.fill(Qt::transparent);
    {
        QPainter painter(&dragToken);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(themeTone("#8D9AF8"), 1.5));
        painter.setBrush(themeTone("#252B3A"));
        painter.drawRoundedRect(QRectF(1.5, 1.5, 55.0, 45.0), 10, 10);
        painter.setPen(Qt::NoPen);
        painter.setBrush(themeTone("#C3C9FF"));
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 2; ++row) {
                painter.drawEllipse(QPointF(22.0 + column * 7.0,
                    21.0 + row * 7.0), 1.8, 1.8);
            }
        }
    }

    QDrag drag(source);
    auto* mimeData = new QMimeData;
    mimeData->setData(QLatin1String(kToolRailMimeType), QByteArrayLiteral("tool-rail"));
    drag.setMimeData(mimeData);
    drag.setPixmap(dragToken);
    drag.setHotSpot(dragToken.rect().center());
    drag.exec(Qt::MoveAction, Qt::MoveAction);

    hideToolRailDockTargets();
    if (pointerRouter_ && pointerRouter_->captureDomain()
        != CrossWindowPointerRouter::CaptureDomain::None) {
        pointerRouter_->cancelCapture();
    }
    if (pendingToolRailDockLocation_) {
        const auto location = *pendingToolRailDockLocation_;
        pendingToolRailDockLocation_.reset();
        setToolRailDockLocation(location);
    }
}

void MainWindow::showToolRailDockTargets()
{
    positionToolRailDockTargets();
    const std::array<QWidget*, 4> targets {
        toolRailLeftTarget_, toolRailRightTarget_,
        toolRailTopTarget_, toolRailBottomTarget_,
    };
    QWidget* currentTarget = toolRailLeftTarget_;
    switch (toolRailDockLocation_) {
    case ToolRailDockLocation::Left: break;
    case ToolRailDockLocation::RightPanel:
        currentTarget = toolRailRightTarget_;
        break;
    case ToolRailDockLocation::Top:
        currentTarget = toolRailTopTarget_;
        break;
    case ToolRailDockLocation::Bottom:
        currentTarget = toolRailBottomTarget_;
        break;
    }

    QRegion targetRegion;
    for (auto* target : targets) {
        static_cast<ToolRailDockTarget*>(target)->resetHighlight();
        if (target == currentTarget) {
            target->hide();
            continue;
        }
        target->show();
        target->raise();
        targetRegion |= target->geometry();
    }
    workspace_->setTransientOverlayInteractionRegion(targetRegion);
}

void MainWindow::hideToolRailDockTargets()
{
    const std::array<QWidget*, 4> targets {
        toolRailLeftTarget_, toolRailRightTarget_,
        toolRailTopTarget_, toolRailBottomTarget_,
    };
    for (auto* target : targets) {
        if (target) {
            target->hide();
        }
    }
    if (workspace_) {
        workspace_->setTransientOverlayInteractionRegion({});
    }
}

void MainWindow::setToolRailDockLocation(ToolRailDockLocation location)
{
    if (!toolRail_ || !workspace_) {
        return;
    }

    workspace_->takeToolRail(toolRail_);
    toolRailDockLocation_ = location;
    updateToolRailPresentation();

    OverlayDockWorkspace::ToolRailPlacement placement =
        OverlayDockWorkspace::ToolRailPlacement::Left;
    switch (location) {
    case ToolRailDockLocation::Left:
        break;
    case ToolRailDockLocation::RightPanel:
        placement = OverlayDockWorkspace::ToolRailPlacement::Right;
        break;
    case ToolRailDockLocation::Top:
        placement = OverlayDockWorkspace::ToolRailPlacement::Top;
        break;
    case ToolRailDockLocation::Bottom:
        placement = OverlayDockWorkspace::ToolRailPlacement::Bottom;
        break;
    }
    workspace_->setToolRail(toolRail_, placement);

    QAction* checkedAction = nullptr;
    switch (location) {
    case ToolRailDockLocation::Left: checkedAction = toolRailLeftAction_; break;
    case ToolRailDockLocation::RightPanel: checkedAction = toolRailRightAction_; break;
    case ToolRailDockLocation::Top: checkedAction = toolRailTopAction_; break;
    case ToolRailDockLocation::Bottom: checkedAction = toolRailBottomAction_; break;
    }
    if (checkedAction) {
        checkedAction->setChecked(true);
    }
}

void MainWindow::updateToolRailPresentation()
{
    if (!toolRail_) {
        return;
    }
    const bool vertical = toolRailDockLocation_ == ToolRailDockLocation::Left
        || toolRailDockLocation_ == ToolRailDockLocation::RightPanel;

    toolRail_->setMinimumWidth(0);
    toolRail_->setMaximumWidth(QWIDGETSIZE_MAX);
    toolRail_->setMinimumHeight(0);
    toolRail_->setMaximumHeight(QWIDGETSIZE_MAX);
    toolRail_->setOrientation(vertical ? Qt::Vertical : Qt::Horizontal);
    if (vertical) {
        toolRail_->setFixedWidth(54);
        toolRail_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        toolRail_->setFixedHeight(54);
        toolRail_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QString dockProperty = QStringLiteral("left");
    switch (toolRailDockLocation_) {
    case ToolRailDockLocation::Left: break;
    case ToolRailDockLocation::RightPanel:
        dockProperty = QStringLiteral("right-panel");
        break;
    case ToolRailDockLocation::Top: dockProperty = QStringLiteral("top"); break;
    case ToolRailDockLocation::Bottom: dockProperty = QStringLiteral("bottom"); break;
    }
    toolRail_->setProperty("railDock", dockProperty);
    toolRail_->style()->unpolish(toolRail_);
    toolRail_->style()->polish(toolRail_);
    toolRail_->updateGeometry();
    toolRail_->update();
}

void MainWindow::createInitialDocument()
{
    auto document = std::make_unique<core::Document>(
        core::CanvasSpec {.extent = {1600, 900}, .dotsPerInch = 96.0});
    auto surface = std::make_shared<core::ContiguousRasterSurface>(
        core::Extent2u {1600, 900}, core::Rgba8 {0, 0, 0, 0});
    document->insertLayer(0, core::Layer::raster("Layer 1", std::move(surface)));
    if (initializeDocument(std::move(document), QStringLiteral("Untitled"))) fileState().untouched = true;
}

void MainWindow::showStartupDocument()
{
    const QScopedValueRollback showing(startupDocumentDialog_, true);
    createNewDocument();
}

void MainWindow::createNewDocument()
{
    if (fileBusy_ || workspaceDialog_) return;
    startupNewDocumentPending_ = false;
    WorkspaceDialog presenter(*workspace_, *this);
    NewDocumentDialog dialog(&presenter);
    dialog.setAcceptDrops(true);
    presenter.setLocalFileDropHandler([&dialog](const QStringList& paths) { dialog.openDroppedFiles(paths); });
    dialog.setRecentFiles(&recentFiles_);
    for (;;) {
        const auto result = [&] {
            const QScopedValueRollback active(workspaceDialog_, &presenter);
            return presenter.exec(dialog);
        }();
        if (result == QDialog::Accepted) break;
        if (result == NewDocumentDialog::OpenDropped) {
            bool opened = false;
            for (const auto& path : dialog.droppedFiles()) opened = openImageFromPath(path) || opened;
            if (opened) return;
            continue;
        }
        if (result != NewDocumentDialog::OpenImage && result != NewDocumentDialog::OpenProject
            && result != NewDocumentDialog::OpenRecent) return;
        const auto filePath = result == NewDocumentDialog::OpenRecent ? dialog.selectedRecentPath()
            : QFileDialog::getOpenFileName(this, QStringLiteral("Open document"), openDocumentDirectory(),
                result == NewDocumentDialog::OpenProject ? QStringLiteral("Vulkana project (*.vulkana)") : supportedImageFilter());
        if (!filePath.isEmpty() && openImageFromPath(filePath)) return;
        dialog.refreshRecentFiles();
        // File-picker cancellation/failure returns to the user's size choices.
    }
    try {
        const auto spec = dialog.canvasSpec();
        auto document = std::make_unique<core::Document>(spec);
        auto surface = std::make_shared<core::ContiguousRasterSurface>(spec.extent, dialog.backgroundColor());
        document->insertLayer(0, core::Layer::raster("Layer 1", std::move(surface)));
        document->markUnsaved();
        if (!initializeDocument(std::move(document), QStringLiteral("Untitled"))) return;
        // Explicit canvas creation is a document-size decision, even with a
        // transparent initial layer. Drops must import, never replace it.
        fileState().untouched = false;
        synchronizeUi(true, true);
        canvasWindow_->dismissLayerOutlines();
        updateDocumentTitle();
    } catch (const std::exception& exception) {
        QMessageBox::critical(this, QStringLiteral("Unable to create document"),
            QString::fromUtf8(exception.what()));
    }
}

void MainWindow::changeCanvasSize()
{
    if (fileBusy_ || workspaceDialog_) return;
    cancelPendingEdits();
    auto* document = session().document();
    if (!document) {
        return;
    }

    WorkspaceDialog presenter(*workspace_, *this);
    NewDocumentDialog dialog(NewDocumentDialog::Mode::ChangeCanvasSize,
        document->canvas(), &presenter);
    const auto result = [&] {
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        return presenter.exec(dialog);
    }();
    if (result != QDialog::Accepted) {
        return;
    }

    try {
        const auto before = document->canvas();
        const auto after = dialog.canvasSpec();
        if (!executeDocumentCommand(
                std::make_unique<core::ChangeCanvasSpecCommand>(after))) {
            statusBar()->showMessage(QStringLiteral("Canvas size unchanged"), 2500);
            return;
        }
        fileState().untouched = false;
        synchronizeUi(false, false);
        statusBar()->showMessage(QStringLiteral("Canvas changed from %1 × %2 to %3 × %4 px")
                .arg(before.extent.width)
                .arg(before.extent.height)
                .arg(after.extent.width)
                .arg(after.extent.height),
            3500);
    } catch (const std::exception& exception) {
        QMessageBox::critical(this, QStringLiteral("Unable to change canvas size"),
            QString::fromUtf8(exception.what()));
    }
}

void MainWindow::openImage()
{
    if (fileBusy_) return;
    const auto paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("Open document"), openDocumentDirectory(), documentOpenFilter());
    for (const auto& path : paths) openImageFromPath(path);
}

void MainWindow::importImageAsLayer()
{
    const auto filePath = QFileDialog::getOpenFileName(this,
        QStringLiteral("Import image as layer"), openDocumentDirectory(), supportedImageFilter());
    if (!filePath.isEmpty()) {
        importImageAsLayerFromPath(filePath);
    }
}

bool MainWindow::openImageFromPath(const QString& filePath)
{
    return openDocumentFromPath(filePath);
}

bool MainWindow::importImageAsLayerFromPath(const QString& filePath)
{
    if (fileBusy_) return false;
    if (isPdfFile(filePath)) return importPdfFromPath(filePath,true);
    if (isPsdFile(filePath)) return importPsdFromPath(filePath,true);
    cancelPendingEdits();
    auto* document = session().document();
    if (!document) {
        return openImageFromPath(filePath);
    }

    auto result = loadRasterLayer(filePath);
    if (!result) {
        QMessageBox::critical(this, QStringLiteral("Unable to import image"), result.error);
        return false;
    }

    const auto layerId = result.layer->id;
    if (!executeDocumentCommand(std::make_unique<core::AddLayerCommand>(
            std::move(*result.layer), document->layers().size(), session().activeLayer()))) {
        QMessageBox::critical(this, QStringLiteral("Unable to import image"),
            QStringLiteral("The image could not be added to the current document."));
        return false;
    }

    session().setActiveLayer(layerId);
    fileState().untouched = false;
    synchronizeUi(true, false);
    statusBar()->showMessage(
        QStringLiteral("Imported %1 as a new layer").arg(QFileInfo(filePath).fileName()), 3500);
    return true;
}

void MainWindow::pasteImageAsLayer()
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || QApplication::activeModalWidget()
        || editorTextInputActive() || (textController_ && textController_->active())) return;
    const auto* mime = QApplication::clipboard()->mimeData(QClipboard::Clipboard);
    if (!mime) return;
    auto loaded = loadClipboardImage(*mime);
    if (!loaded) {
        statusBar()->showMessage(loaded.error, 3500);
        return; // Invalid clipboard data must not cancel gestures or clear redo.
    }
    const auto id = loaded.layer->id;
    if (!executeDocumentCommand(std::make_unique<core::AddLayerCommand>(std::move(*loaded.layer),
            session().document()->layers().size(), session().activeLayer()))) return;
    session().setActiveLayer(id);
    fileState().untouched = false;
    synchronizeUi(true, false); // Keep document bounds and the current view.
    statusBar()->showMessage(QStringLiteral("Pasted image as a new layer"), 3500);
}

void MainWindow::handleDroppedImages(const QStringList& filePaths)
{
    bool mayOpenFirstSuccessfulImage = fileState().untouched;
    for (const auto& filePath : filePaths) {
        if (isPdfFile(filePath)) { importPdfFromPath(filePath,session().document()!=nullptr); continue; }
        if (isPsdFile(filePath)) { importPsdFromPath(filePath,session().document()!=nullptr); continue; }
        if (QFileInfo(filePath).suffix().compare(QStringLiteral("vulkana"), Qt::CaseInsensitive) == 0) {
            openImageFromPath(filePath); continue;
        }
        if (mayOpenFirstSuccessfulImage) {
            if (openImageFromPath(filePath)) {
                mayOpenFirstSuccessfulImage = false;
            }
            continue;
        }
        importImageAsLayerFromPath(filePath);
    }
}

void MainWindow::logRendererDiagnostics() const
{
    const auto stats = canvasWindow_->rendererStats();
    const auto brushStats = brushAssets_->runtimeStats();
    qInfo() << "Renderer diagnostics: device" << QString::fromStdString(stats.deviceName)
            << "frames" << stats.framesSubmitted
            << "resource generations" << stats.resourceGeneration
            << "swapchain generations" << stats.swapchainGeneration
            << "swapchain extent" << stats.swapchainWidth << 'x' << stats.swapchainHeight
            << "deferred resize events" << stats.deferredResizeEvents
            << "resize commits" << stats.resizeCommits
            << "resize presentation suspends" << stats.resizePresentationSuspends
            << "resize presentation resumes" << stats.resizePresentationResumes
            << "full uploads" << stats.fullUploads
            << "regional batches" << stats.regionalUploadBatches
            << "exact dirty regions" << stats.regionalDirtyRegions
            << "GPU upload regions" << stats.regionalUploads
            << "uploaded bytes" << stats.uploadedBytes
            << "selection geometry uploads" << stats.selectionGeometryUploads
            << "selection geometry bytes" << stats.selectionUploadedBytes
            << "staging allocations" << stats.stagingBufferAllocations
            << "staging capacity" << stats.stagingCapacityBytes
            << "last upload prep us"
            << static_cast<double>(stats.lastUploadPreparationNanoseconds) / 1000.0
            << "max upload prep us"
            << static_cast<double>(stats.maximumUploadPreparationNanoseconds) / 1000.0
            << "brush resource reads" << brushStats.resourceReads
            << "brush decodes" << brushStats.imageDecodes
            << "brush mip builds" << brushStats.mipBuilds
            << "brush resident masks" << brushStats.residentMaskCount
            << "brush resident bytes" << brushStats.residentBytes
            << "brush resolver hits" << brushStats.resolverHits
            << "brush resolver misses" << brushStats.resolverMisses;
}

void MainWindow::runIntegrationSmokeTest()
{
    smokeFailures_.clear();
    smokeCompleted_ = false;
    smokeCheck(QCoreApplication::testAttribute(
            Qt::AA_DontCreateNativeWidgetSiblings),
        QStringLiteral("Native canvas sibling promotion is not disabled"));
    smokeCheck(canvasContainer_->testAttribute(Qt::WA_NativeWindow),
        QStringLiteral("Vulkan canvas container is not a native clipping boundary"));
    smokeCheck(canvasContainer_->windowHandle() != nullptr,
        QStringLiteral("Native Vulkan canvas container has no QWidgetWindow"));
    smokeCheck(canvasContainer_->windowHandle()
            && canvasWindow_->parent() == canvasContainer_->windowHandle(),
        QStringLiteral("Vulkan canvas is not parented to its native container"));
    smokeCheck(workspace_->panelOverlay()->testAttribute(Qt::WA_NativeWindow)
            && workspace_->panelOverlay()->windowHandle(),
        QStringLiteral("Panel overlay is not an independent native surface"));
    smokeCheck(!colorPanelShell_->isWindow()
            && !colorPanelShell_->testAttribute(Qt::WA_NativeWindow)
            && colorPanelShell_->internalWinId() == 0,
        QStringLiteral("Color panel escaped the in-app overlay"));
    smokeCheck(!layersPanelShell_->isWindow()
            && !layersPanelShell_->testAttribute(Qt::WA_NativeWindow)
            && layersPanelShell_->internalWinId() == 0,
        QStringLiteral("Layers panel escaped the in-app overlay"));
    smokeCheck(!propertiesPanelShell_->isWindow()
            && !propertiesPanelShell_->testAttribute(Qt::WA_NativeWindow)
            && propertiesPanelShell_->internalWinId() == 0,
        QStringLiteral("Properties panel escaped the in-app overlay"));
    const auto screens = QGuiApplication::screens();
    const auto createDropProbe =
        [this](const QString& nameTemplate, QSize size, QColor color) {
        const QString temporaryTemplate = QDir(QDir::tempPath()).filePath(nameTemplate);
        auto* file = new QTemporaryFile(temporaryTemplate, this);
        if (!file->open()) {
            smokeCheck(false, QStringLiteral("Could not create a drag-and-drop probe image"));
            return file;
        }
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(color);
        if (!image.save(file, "PNG")) {
            smokeCheck(false, QStringLiteral("Could not encode a drag-and-drop probe image"));
        }
        file->close();
        return file;
    };
    auto* droppedImage = createDropProbe(
        QStringLiteral("imageeditor-wayland-drop-a-XXXXXX.png"), {32, 24}, {72, 96, 220, 192});
    auto* secondDroppedImage = createDropProbe(
        QStringLiteral("imageeditor-wayland-drop-b-XXXXXX.png"), {17, 11}, {220, 112, 72, 255});
    QTimer::singleShot(45, this, [this] {
        auto* panelOverlay = workspace_->panelOverlay();
        auto* panelCard = workspace_->findChild<QWidget*>(
            QStringLiteral("OverlayPanelCard"));
        auto* handle = workspace_->panelResizeHandle();
        smokeCheck(canvasContainer_->geometry() == workspace_->rect(),
            QStringLiteral("Full-size canvas container does not fill the workspace"));
        smokeCheck(panelOverlay->geometry() == workspace_->rect(),
            QStringLiteral("Stationary panel surface does not fill the workspace"));
        smokeCheck(canvasWindow_->geometry() == canvasContainer_->rect(),
            QStringLiteral("Vulkan window does not fill its stable container"));
        smokeCheck(panelOverlay->windowHandle()
                && canvasContainer_->windowHandle()
                && panelOverlay->windowHandle()->parent()
                    == canvasContainer_->windowHandle()->parent(),
            QStringLiteral("Canvas container and panel overlay are not native siblings"));
        smokeCheck(panelCard && panelCard->isVisible()
                && colorPanelShell_->isVisible()
                && layersPanelShell_->isVisible()
                && propertiesPanelShell_->isVisible(),
            QStringLiteral("Default panel stack is not visibly laid out"));
        smokeCheck(panelOverlay->isWindow()
                && panelOverlay->testAttribute(Qt::WA_TranslucentBackground)
                && !panelOverlay->testAttribute(Qt::WA_OpaquePaintEvent)
                && panelOverlay->backingStore() != workspace_->backingStore(),
            QStringLiteral("Panel overlay has no independent alpha backing store"));
        if (handle && panelCard) {
            const auto handleCenter = handle->mapTo(
                panelOverlay, handle->rect().center());
            const int cardLeft = panelCard->mapTo(panelOverlay, QPoint {}).x();
            smokeCheck(std::abs(handleCenter.x() - cardLeft) <= 1,
                QStringLiteral("Panel resize target is not centered on the visible edge"));
            smokeCheck(handle->cursor().shape() == Qt::SizeHorCursor,
                QStringLiteral("Panel resize target has no horizontal resize cursor"));
            smokeCheck(panelOverlay->mask().isEmpty(),
                QStringLiteral("Panel QWidget unexpectedly uses a paint-clipping mask"));
            if (panelOverlay->windowHandle()) {
                const QRegion inputRegion = panelOverlay->windowHandle()->mask();
                smokeCheck(inputRegion.contains(handleCenter)
                        && inputRegion.contains(panelCard->geometry().center())
                        && !inputRegion.contains(
                            QPoint(4, panelOverlay->height() / 2)),
                    QStringLiteral("Panel native input region does not match handle/card"));
            }
        }

        showToolRailDockTargets();
        const std::array<QWidget*, 4> railTargets {
            toolRailLeftTarget_, toolRailRightTarget_,
            toolRailTopTarget_, toolRailBottomTarget_,
        };
        QWidget* currentRailTarget = toolRailLeftTarget_;
        switch (toolRailDockLocation_) {
        case ToolRailDockLocation::Left: break;
        case ToolRailDockLocation::RightPanel:
            currentRailTarget = toolRailRightTarget_;
            break;
        case ToolRailDockLocation::Top:
            currentRailTarget = toolRailTopTarget_;
            break;
        case ToolRailDockLocation::Bottom:
            currentRailTarget = toolRailBottomTarget_;
            break;
        }
        const auto visibleRailTargets = std::count_if(
            railTargets.begin(), railTargets.end(),
            [](const QWidget* target) { return target && target->isVisible(); });
        smokeCheck(visibleRailTargets == 3 && currentRailTarget->isHidden(),
            QStringLiteral("Tool rail drag exposed its redundant current dock target"));
        hideToolRailDockTargets();
    });
    QTimer::singleShot(70, this, [this, droppedImage, secondDroppedImage] {
        if (droppedImage->fileName().isEmpty() || secondDroppedImage->fileName().isEmpty()) {
            return;
        }
        QMimeData mimeData;
        mimeData.setUrls({QUrl::fromLocalFile(droppedImage->fileName()),
            QUrl::fromLocalFile(secondDroppedImage->fileName())});
        QDragEnterEvent dragEnter({240, 140}, Qt::CopyAction, &mimeData,
            Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &dragEnter);
        smokeCheck(dragEnter.isAccepted(),
            QStringLiteral("Native canvas rejected a local-file drag"));
        QDropEvent drop({240.0, 140.0}, Qt::CopyAction, &mimeData,
            Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &drop);
        smokeCheck(drop.isAccepted(),
            QStringLiteral("Native canvas rejected a local-file drop"));
        const auto* document = session().document();
        smokeCheck(document && document->canvas().extent == core::Extent2u({32, 24}),
            QStringLiteral("First dropped raster did not open the untouched document"));
        smokeCheck(document && document->layers().size() == 2,
            QStringLiteral("Multiple dropped rasters did not create multiple layers"));
        if (document && document->layers().size() == 2) {
            const auto& first = std::get<core::RasterLayer>(document->layers()[0].payload);
            const auto& second = std::get<core::RasterLayer>(document->layers()[1].payload);
            smokeCheck(first.surface && first.surface->extent() == core::Extent2u({32, 24}),
                QStringLiteral("First dropped raster is not the bottom layer"));
            smokeCheck(second.surface && second.surface->extent() == core::Extent2u({17, 11}),
                QStringLiteral("Second dropped raster is not the top layer"));
            smokeCheck(session().activeLayer() == document->layers()[1].id,
                QStringLiteral("Last dropped raster is not the active layer"));
        }
    });
    QTimer::singleShot(260, this, [this] { layerList_->setFocus(Qt::OtherFocusReason); });
    QTimer::singleShot(310, this, [this] {
        smokeCheck(layerList_->focusPolicy() != Qt::NoFocus,
            QStringLiteral("Layer list is not keyboard-focusable"));
    });
    QTimer::singleShot(325, this, [this] {
        const auto routesBefore = pointerRouter_->routedEventCount();
        const auto retainedUngrabsBefore = pointerRouter_->retainedUngrabCount();
        const auto repairedButtonsBefore = pointerRouter_->repairedButtonStateCount();
        const auto pressGlobal = opacitySlider_->mapToGlobal(
            opacitySlider_->progressTrackRect().center());
        auto* widgetDispatchWindow = workspace_->panelOverlay()->windowHandle();
        smokeCheck(widgetDispatchWindow != nullptr,
            QStringLiteral("Panel overlay has no QWidget dispatch window"));
        smokeCheck(widgetDispatchWindow != windowHandle(),
            QStringLiteral("Panel controls still dispatch through the outer main window"));
        if (!widgetDispatchWindow) {
            return;
        }
        const auto pressLocal = widgetDispatchWindow->mapFromGlobal(pressGlobal);
        QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressLocal,
            pressGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(widgetDispatchWindow, &press);
        smokeCheck(opacitySlider_->isSliding(),
            QStringLiteral("Opacity slider did not begin its normal Qt drag"));
        smokeCheck(pointerRouter_->captureOwner() == opacitySlider_,
            QStringLiteral("Pointer router did not retain the slider as capture owner"));
        // Compare against the groove-press value. With Layers docked left,
        // crossing into the canvas can legitimately return to the original
        // 100% value, rather than decreasing it as in the old right layout.
        const auto valueAtPress = opacitySlider_->value();

        // KDE Wayland reports UngrabMouse while the pointer crosses between
        // the QWidget surface and the embedded native Vulkan surface. It is a
        // surface handoff, not a physical release, and must not end the gesture.
        QEvent widgetUngrab(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(widgetDispatchWindow, &widgetUngrab);
        QEvent nativeUngrab(QEvent::UngrabMouse);
        QCoreApplication::sendEvent(canvasWindow_, &nativeUngrab);
        smokeCheck(opacitySlider_->isSliding(),
            QStringLiteral("Wayland seam UngrabMouse canceled the opacity drag"));
        smokeCheck(pointerRouter_->retainedUngrabCount() >= retainedUngrabsBefore + 2,
            QStringLiteral("Pointer router did not retain both seam ungrab notifications"));

        const QPointF targetLocal {
            static_cast<double>(canvasWindow_->width()) * 0.5,
            static_cast<double>(canvasWindow_->height()) * 0.5,
        };
        const auto targetGlobal = canvasWindow_->mapToGlobal(targetLocal);
        QMouseEvent move(QEvent::MouseMove, targetLocal, targetLocal,
            targetGlobal, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, targetLocal, targetLocal,
            targetGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);

        smokeCheck(!opacitySlider_->isSliding(),
            QStringLiteral("Opacity slider remained down after cross-window release"));
        smokeCheck(opacitySlider_->value() != valueAtPress,
            QStringLiteral("Opacity slider did not track across the native canvas"));
        smokeCheck(pointerRouter_->routedEventCount() >= routesBefore + 2,
            QStringLiteral("Cross-window slider move/release were not centrally routed"));
        smokeCheck(pointerRouter_->repairedButtonStateCount() >= repairedButtonsBefore + 1,
            QStringLiteral("Wayland seam button state was not repaired"));
        smokeCheck(pointerRouter_->captureDomain()
                == CrossWindowPointerRouter::CaptureDomain::None,
            QStringLiteral("Pointer router retained capture after slider release"));
    });
    QTimer::singleShot(340, this, [this] {
        canvasContainer_->setFocus(Qt::OtherFocusReason);
        canvasWindow_->requestActivate();
    });
    QTimer::singleShot(365, this, [this] {
        // Give this synthetic 160px growth room below the column's maximum;
        // production defaults/preferences may already start near that limit.
        workspace_->setPanelWidth(310);
        auto* handle = workspace_->panelResizeHandle();
        auto* widgetDispatchWindow = workspace_->panelOverlay()->windowHandle();
        smokeCheck(handle && widgetDispatchWindow,
            QStringLiteral("Panel resize handle has no QWidget dispatch surface"));
        if (!handle || !widgetDispatchWindow) {
            return;
        }

        const int widthBefore = workspace_->panelWidth();
        const auto routesBefore = pointerRouter_->routedEventCount();
        const QPoint pressGlobal = handle->mapToGlobal(handle->rect().center());
        const QPointF pressLocal = widgetDispatchWindow->mapFromGlobal(pressGlobal);
        QMouseEvent press(QEvent::MouseButtonPress, pressLocal, pressLocal,
            pressGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(widgetDispatchWindow, &press);
        smokeCheck(pointerRouter_->captureOwner() == handle,
            QStringLiteral("Pointer router did not retain the panel resize handle"));

        const QPoint targetGlobal = pressGlobal - QPoint {160, 0};
        const QPointF targetLocal = canvasWindow_->mapFromGlobal(targetGlobal);
        QMouseEvent move(QEvent::MouseMove, targetLocal, targetLocal,
            targetGlobal, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &move);
        smokeCheck(workspace_->panelWidth() == widthBefore + 160,
            QStringLiteral("A fast cross-window panel drag did not apply its full delta"));
        smokeCheck(std::abs(handle->mapToGlobal(handle->rect().center()).x()
                - targetGlobal.x()) <= 1,
            QStringLiteral("Panel resize edge did not remain glued to the pointer"));

        QMouseEvent release(QEvent::MouseButtonRelease, targetLocal, targetLocal,
            targetGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);
        smokeCheck(pointerRouter_->routedEventCount() >= routesBefore + 2,
            QStringLiteral("Panel move/release did not cross the native canvas seam"));
        smokeCheck(pointerRouter_->captureDomain()
                == CrossWindowPointerRouter::CaptureDomain::None,
            QStringLiteral("Panel resize retained capture after release"));
        smokeCheck(QWidget::mouseGrabber() == nullptr,
            QStringLiteral("Panel resize left an explicit QWidget grab behind"));
    });
    QTimer::singleShot(410, this, [this] {
        const auto before = canvasWindow_->scene().viewport.pan();
        const QPointF start {90.0, 90.0};
        const QPointF finish {124.0, 112.0};
        QMouseEvent press(QEvent::MouseButtonPress, start,
            QPointF(canvasWindow_->mapToGlobal(start.toPoint())),
            Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &press);
        QMouseEvent move(QEvent::MouseMove, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &move);
        const auto afterPan = canvasWindow_->scene().viewport.pan();
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(canvasWindow_, &focusOut);
        const QPointF afterFocusMove {152.0, 138.0};
        QMouseEvent moveAfterFocusOut(QEvent::MouseMove, afterFocusMove,
            QPointF(canvasWindow_->mapToGlobal(afterFocusMove.toPoint())),
            Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &moveAfterFocusOut);
        QMouseEvent release(QEvent::MouseButtonRelease, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);
        smokeCheck(canvasWindow_->lastPanGrabSucceeded(),
            QStringLiteral("Native canvas mouse capture failed"));
        smokeCheck(canvasWindow_->scene().viewport.pan() != before,
            QStringLiteral("Synthetic middle-button pan did not move the viewport"));
        smokeCheck(canvasWindow_->scene().viewport.pan() == afterPan,
            QStringLiteral("Canvas kept panning after focus loss"));
    });
    struct PanelIsolationBaseline {
        QRect canvasContainerGeometry;
        QRect canvasWindowGeometry;
        QRect panelOverlayGeometry;
        QSize workspaceSize;
        render::RendererStats renderer;
    };
    auto panelBaseline = std::make_shared<PanelIsolationBaseline>();
    const auto checkPanelIsolation = [this, panelBaseline](const QString& phase) {
        const auto stats = canvasWindow_->rendererStats();
        smokeCheck(canvasContainer_->geometry() == panelBaseline->canvasContainerGeometry,
            QStringLiteral("Panel operation changed canvas-container geometry: %1").arg(phase));
        smokeCheck(canvasWindow_->geometry() == panelBaseline->canvasWindowGeometry,
            QStringLiteral("Panel operation changed Vulkan-window geometry: %1").arg(phase));
        smokeCheck(workspace_->panelOverlay()->geometry()
                == panelBaseline->panelOverlayGeometry,
            QStringLiteral("Panel operation changed native overlay geometry: %1").arg(phase));
        smokeCheck(workspace_->size() == panelBaseline->workspaceSize,
            QStringLiteral("Panel operation changed workspace size: %1").arg(phase));
        smokeCheck(stats.swapchainGeneration
                == panelBaseline->renderer.swapchainGeneration,
            QStringLiteral("Panel operation recreated the Vulkan swapchain: %1").arg(phase));
        smokeCheck(stats.swapchainWidth == panelBaseline->renderer.swapchainWidth
                && stats.swapchainHeight == panelBaseline->renderer.swapchainHeight,
            QStringLiteral("Panel operation changed the swapchain extent: %1").arg(phase));
        smokeCheck(stats.deferredResizeEvents
                == panelBaseline->renderer.deferredResizeEvents,
            QStringLiteral("Panel operation emitted a canvas resize: %1").arg(phase));
        smokeCheck(stats.resizeCommits == panelBaseline->renderer.resizeCommits,
            QStringLiteral("Panel operation committed a canvas resize: %1").arg(phase));
        smokeCheck(stats.resizePresentationSuspends
                == panelBaseline->renderer.resizePresentationSuspends
                && stats.resizePresentationResumes
                    == panelBaseline->renderer.resizePresentationResumes,
            QStringLiteral("Panel operation suspended canvas presentation: %1").arg(phase));
    };
    QTimer::singleShot(430, this, [this, panelBaseline] {
        panelBaseline->canvasContainerGeometry = canvasContainer_->geometry();
        panelBaseline->canvasWindowGeometry = canvasWindow_->geometry();
        panelBaseline->panelOverlayGeometry = workspace_->panelOverlay()->geometry();
        panelBaseline->workspaceSize = workspace_->size();
        panelBaseline->renderer = canvasWindow_->rendererStats();
        workspace_->setPanelWidth(workspace_->panelWidth() + 120);
    });
    QTimer::singleShot(520, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("panel width"));
        layersPanelAction_->trigger();
    });
    QTimer::singleShot(600, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("Layers hidden"));
        propertiesPanelAction_->trigger();
        colorPanelAction_->trigger();
        adjustmentsPanelAction_->trigger();
    });
    QTimer::singleShot(680, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("all panels hidden"));
        smokeCheck(workspace_->panelOverlay()->isVisible()
                && workspace_->hasToolRail(toolRail_),
            QStringLiteral("Overlay-hosted tool rail disappeared with all panels hidden"));
        layersPanelAction_->trigger();
        propertiesPanelAction_->trigger();
        colorPanelAction_->trigger();
        adjustmentsPanelAction_->trigger();
    });
    QTimer::singleShot(760, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("panels restored"));
        smokeCheck(workspace_->panelOverlay()->isVisible(),
            QStringLiteral("Panel overlay did not return with its panels"));
        workspace_->floatPanel(layersPanelShell_,
            QRect {70, 80, 300, 310});
        workspace_->floatPanel(propertiesPanelShell_,
            QRect {420, 120, 340, 300});
        workspace_->floatPanel(colorPanelShell_,
            QRect {780, 80, 300, 180});
        workspace_->floatPanel(adjustmentsPanelShell_,
            QRect {780, 300, 360, 420});
    });
    QTimer::singleShot(850, this, [this] {
        smokeCheck(propertiesPanelShell_->parentWidget()
                == workspace_->panelOverlay(),
            QStringLiteral("Floating panel is not owned by the overlay"));
    });
    QTimer::singleShot(940, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("all four panels floating"));
        for (auto* panel : {layersPanelShell_, propertiesPanelShell_,
                 colorPanelShell_, adjustmentsPanelShell_}) {
            smokeCheck(workspace_->panelPlacement(panel)
                    == OverlayDockWorkspace::PanelPlacement::Floating
                    && panel->isVisible() && panel->parentWidget() == workspace_->panelOverlay()
                    && !panel->isWindow() && panel->internalWinId() == 0,
                QStringLiteral("Floating panel lost internal workspace ownership: %1").arg(panel->objectName()));
        }
        smokeCheck(workspace_->panelPlacement(propertiesPanelShell_)
                == OverlayDockWorkspace::PanelPlacement::Floating,
            QStringLiteral("Properties panel did not enter internal floating state"));
        smokeCheck(propertiesPanelShell_->isVisible(),
            QStringLiteral("Internally floating Properties panel was hidden"));
        smokeCheck(workspace_->panelOverlay()->isVisible(),
            QStringLiteral("Panel overlay disappeared with an internal floating panel"));
        smokeCheck(!propertiesPanelShell_->isWindow()
                && propertiesPanelShell_->internalWinId() == 0
                && propertiesPanelShell_->windowHandle() == nullptr,
            QStringLiteral("Internal floating panel became a desktop window"));
        const bool leftHostsRail = workspace_->toolRailPlacement()
            == OverlayDockWorkspace::ToolRailPlacement::Left;
        const bool rightHostsRail = workspace_->toolRailPlacement()
            == OverlayDockWorkspace::ToolRailPlacement::Right;
        smokeCheck(workspace_->leftPanelCard()->isVisible() == leftHostsRail
                && workspace_->rightPanelCard()->isVisible() == rightHostsRail,
            QStringLiteral("Idle empty panel adoption shelf remained visible"));
    });
    QTimer::singleShot(1000, this, [this] {
        workspace_->dockPanel(propertiesPanelShell_,
            OverlayDockWorkspace::PanelDockSide::Right);
    });
    QTimer::singleShot(1100, this, [this, checkPanelIsolation] {
        checkPanelIsolation(QStringLiteral("Properties redocked"));
        smokeCheck(workspace_->panelPlacement(propertiesPanelShell_)
                == OverlayDockWorkspace::PanelPlacement::DockedRight,
            QStringLiteral("Properties panel did not redock"));
        smokeCheck(workspace_->panelOverlay()->isVisible(),
            QStringLiteral("Panel overlay did not return when Properties redocked"));
        smokeCheck(!propertiesPanelShell_->testAttribute(Qt::WA_NativeWindow)
                && propertiesPanelShell_->internalWinId() == 0
                && propertiesPanelShell_->windowHandle() == nullptr,
            QStringLiteral("Redocked Properties panel acquired a native window"));
        workspace_->dockPanel(layersPanelShell_,
            OverlayDockWorkspace::PanelDockSide::Left);
        workspace_->dockPanel(colorPanelShell_,
            OverlayDockWorkspace::PanelDockSide::Right, 0);
        workspace_->dockPanel(adjustmentsPanelShell_,
            OverlayDockWorkspace::PanelDockSide::Right, 2);
    });
    QTimer::singleShot(1130, this, [this, checkPanelIsolation] {
        const std::array transitions {
            ToolRailDockLocation::Top,
            ToolRailDockLocation::Left,
            ToolRailDockLocation::Bottom,
            ToolRailDockLocation::RightPanel,
            ToolRailDockLocation::Top,
            ToolRailDockLocation::Bottom,
            ToolRailDockLocation::Left,
        };
        for (const auto location : transitions) {
            setToolRailDockLocation(location);
            checkPanelIsolation(QStringLiteral("tool rail edge transition"));
        }
        smokeCheck(workspace_->hasToolRail(toolRail_)
                && !toolRail_->isWindow()
                && workspace_->panelOverlay()->isAncestorOf(toolRail_),
            QStringLiteral("Tool rail escaped the stable panel overlay"));
    });
    QTimer::singleShot(1180, this, [this] { showMaximized(); });
    QTimer::singleShot(1350, this, [this] {
        smokeCheck(isMaximized(), QStringLiteral("Main window did not maximize"));
    });
    QTimer::singleShot(1460, this, [this] { showNormal(); });
    QTimer::singleShot(1660, this, [this] { showMinimized(); });
    QTimer::singleShot(1810, this, [this] {
        const bool minimizedOrUnexposed = isMinimized()
            || windowHandle()->windowStates().testFlag(Qt::WindowMinimized)
            || !windowHandle()->isExposed();
        smokeCheck(minimizedOrUnexposed,
            QStringLiteral("Main window remained exposed after minimize request"));
    });
    QTimer::singleShot(1910, this, [this] { showNormal(); });
    if (screens.size() >= 2) {
        QTimer::singleShot(2150, this, [this, screens] {
            windowHandle()->setScreen(screens.front());
            showFullScreen();
        });
        QTimer::singleShot(2320, this, [this, screens] {
            smokeCheck(windowHandle()->screen() == screens.front(),
                QStringLiteral("Main window did not transition to the first screen"));
        });
        QTimer::singleShot(2450, this, [this, screens] {
            windowHandle()->setScreen(screens.at(1));
            showFullScreen();
        });
        QTimer::singleShot(2620, this, [this, screens] {
            smokeCheck(windowHandle()->screen() == screens.at(1),
                QStringLiteral("Main window did not transition to the second screen"));
        });
        QTimer::singleShot(2700, this, [this] { showNormal(); });
    }
    // Test-only continuation: compositor readiness must precede every raster
    // probe. Independent wall-clock timers let the fragmented mutation land
    // between Brush's saved revision and its Undo/Redo under a delayed resize.
    using RasterProbeStages = std::vector<std::pair<int, std::function<void()>>>;
    const auto rasterProbeStages = std::make_shared<RasterProbeStages>();
    const auto afterRasterReady = [rasterProbeStages](int delay, std::function<void()> callback) {
        rasterProbeStages->emplace_back(delay, std::move(callback));
    };
    QTimer::singleShot(2880, Qt::PreciseTimer, this, [this, rasterProbeStages] {
        const auto fullUploadsBefore = canvasWindow_->rendererStats().fullUploads;
        resize(1420, 900);
        // The earlier drop checks intentionally replace the starter document
        // with tiny fixtures. Restore a representative raster before the
        // fragmented-upload and brush history probes.
        createInitialDocument();
        // A compositor resize can defer this document's initial frame beyond
        // a fixed timer. Mutating before it uploads tests a full upload, not
        // the regional path. Wait for the baseline to reach the GPU first.
        // This bounded timer exists only during the integration smoke.
        auto* uploadReady = new QTimer(this);
        uploadReady->setInterval(16);
        uploadReady->setTimerType(Qt::PreciseTimer);
        // Diagnostic regression hook, read once and only in --smoke-test.
        const int readinessDelay = std::clamp(
            qEnvironmentVariableIntValue("IMAGEEDITOR_SMOKE_RASTER_READY_DELAY_MS"), 0, 600);
        QElapsedTimer elapsed;
        elapsed.start();
        connect(uploadReady, &QTimer::timeout, this,
            [this, uploadReady, fullUploadsBefore, elapsed, rasterProbeStages, readinessDelay,
                regionalBefore = std::optional<std::uint64_t>{}]() mutable {
                const auto stats = canvasWindow_->rendererStats();
                if (!regionalBefore && stats.fullUploads > fullUploadsBefore
                    && elapsed.elapsed() >= readinessDelay) {
                    regionalBefore = stats.regionalUploadBatches;
                    mutateRasterForUploadProbe();
                } else if (regionalBefore && stats.regionalUploadBatches > *regionalBefore) {
                    uploadReady->stop();
                    uploadReady->deleteLater();
                    for (const auto& [delay, callback] : *rasterProbeStages)
                        QTimer::singleShot(delay, Qt::PreciseTimer, this, callback);
                } else if (elapsed.hasExpired(750)) {
                    uploadReady->stop();
                    uploadReady->deleteLater();
                    smokeCheck(false, QStringLiteral(
                        "Raster upload prerequisites timed out before tool probes"));
                    smokeCompleted_ = true;
                    qCritical() << "Wayland/Vulkan smoke assertions failed:" << smokeFailures_;
                    close();
                }
            });
        uploadReady->start();
    });
    afterRasterReady(280, [this] {
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("No active raster for brush integration smoke"));
            return;
        }
        auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Active brush smoke target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        const auto surfaceRevisionBefore = raster.surface->revision();
        const auto historyDepthBefore = session().history().undoDepth();
        setActiveTool(core::ToolId::Brush);
        // Exercise the complete advanced path (bitmap mask, rotated chisel
        // geometry, and document-anchored grain) in the native Vulkan smoke.
        brushSettings_ = core::proceduralBrushPreset(
            core::ProceduralBrushPreset::DryInk);
        applyBrushSettings(brushSettings_);
        QString brushAssetError;
        const auto tipPrepared = propertiesPanel_->selectBrushComponent(
            core::BrushAssetType::Tip,
            "builtin.tip.bitmap.tapered-claw.v1", &brushAssetError);
        smokeCheck(tipPrepared,
            QStringLiteral("Packaged smoke tip failed to prepare: %1")
                .arg(brushAssetError));
        const auto grainPrepared = propertiesPanel_->selectBrushComponent(
            core::BrushAssetType::Grain,
            "builtin.grain.paper-breakup-fine.v1", &brushAssetError);
        smokeCheck(grainPrepared,
            QStringLiteral("Packaged smoke grain failed to prepare: %1")
                .arg(brushAssetError));
        brushSettings_.pressureToSize = false;
        brushSettings_.pressureToFlow = false;
        brushSettings_.sizePixels = 28.0;
        brushSettings_.tip.rotationMode =
            core::BrushTipRotationMode::FollowStrokeDirection;
        applyBrushSettings(brushSettings_);
        smokeBrushAssetBaseline_ = brushAssets_->runtimeStats();

        const QPointF start {
            static_cast<double>(canvasWindow_->width()) * 0.42,
            static_cast<double>(canvasWindow_->height()) * 0.48,
        };
        const QPointF finish {
            static_cast<double>(canvasWindow_->width()) * 0.58,
            static_cast<double>(canvasWindow_->height()) * 0.54,
        };
        QMouseEvent press(QEvent::MouseButtonPress, start,
            QPointF(canvasWindow_->mapToGlobal(start.toPoint())),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &press);
        smokeCheck(activeBrushStroke_ != nullptr
                && activeBrushStroke_->lastResolvedTipAngleDegrees().has_value()
                && std::abs(std::remainder(
                    canvasWindow_->scene().brushTipAngleDegrees
                        - *activeBrushStroke_->lastResolvedTipAngleDegrees(),
                    360.0)) < 1.0e-9,
            QStringLiteral("Stationary brush cursor did not match the resolved base-angle dab"));
        QMouseEvent move(QEvent::MouseMove, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &move);
        smokeCheck(activeBrushStroke_ != nullptr
                && activeBrushStroke_->lastResolvedTipAngleDegrees().has_value()
                && std::abs(std::remainder(
                    canvasWindow_->scene().brushTipAngleDegrees
                        - *activeBrushStroke_->lastResolvedTipAngleDegrees(),
                    360.0)) < 1.0e-9,
            QStringLiteral("Vulkan brush cursor diverged from the painted direction angle"));
        QMouseEvent release(QEvent::MouseButtonRelease, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);
        smokeCheck(std::abs(std::remainder(
                canvasWindow_->scene().brushTipAngleDegrees
                    - canvasWindow_->scene().brushTipBaseAngleDegrees,
                360.0)) < 1.0e-9,
            QStringLiteral("Brush hover cursor did not return to its base angle"));

        smokeCheck(session().history().undoDepth() == historyDepthBefore + 1,
            QStringLiteral("Brush stroke did not create exactly one history entry"));
        smokeCheck(session().history().undoLabel() == std::string_view("Brush stroke"),
            QStringLiteral("Brush history entry has the wrong label"));
        smokeCheck(raster.surface->revision() > surfaceRevisionBefore,
            QStringLiteral("Brush input did not mutate the CPU-authoritative surface"));
        smokeBrushPaintedRevision_ = raster.surface->revision();
        synchronizeUi(false, false);
    });
    // Give paint, undo, and redo separate event-loop turns. This ensures the
    // integration gate actually exercises each incremental Vulkan upload
    // instead of observing only the final pixels after three synchronous
    // history operations.
    afterRasterReady(410, [this] {
        undoAction_->trigger();
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("Brush smoke target disappeared before undo"));
            return;
        }
        const auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Brush smoke undo target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        smokeCheck(raster.surface->revision() == smokeBrushPaintedRevision_ + 1,
            QStringLiteral("Brush undo did not produce one surface revision"));
    });
    afterRasterReady(540, [this] {
        redoAction_->trigger();
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("Brush smoke target disappeared before redo"));
            return;
        }
        const auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Brush smoke redo target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        smokeCheck(raster.surface->revision() == smokeBrushPaintedRevision_ + 2,
            QStringLiteral("Brush redo did not produce one surface revision"));
        if (smokeBrushAssetBaseline_) {
            const auto assetStats = brushAssets_->runtimeStats();
            smokeCheck(assetStats.resourceReads
                    == smokeBrushAssetBaseline_->resourceReads
                    && assetStats.imageDecodes
                        == smokeBrushAssetBaseline_->imageDecodes
                    && assetStats.mipBuilds
                        == smokeBrushAssetBaseline_->mipBuilds,
                QStringLiteral("Brush paint/undo/redo performed hot-path asset IO or decoding"));
            smokeCheck(assetStats.residentMaskCount
                    == smokeBrushAssetBaseline_->residentMaskCount
                    && assetStats.residentBytes
                        == smokeBrushAssetBaseline_->residentBytes,
                QStringLiteral("Brush paint/undo/redo changed the resident mask cache"));
            smokeCheck(assetStats.resolverHits
                    == smokeBrushAssetBaseline_->resolverHits + 2
                    && assetStats.resolverMisses
                        == smokeBrushAssetBaseline_->resolverMisses,
                QStringLiteral("Direction brush took an unexpected asset resolver path"));
        }
    });
    afterRasterReady(640, [this] {
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("No active raster for eraser integration smoke"));
            return;
        }
        auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Active eraser smoke target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        const auto revisionBefore = raster.surface->revision();
        const auto historyDepthBefore = session().history().undoDepth();

        // An alpha-zero foreground proves Erase is driven by resolved brush
        // coverage rather than paint color. The same cached bitmap tip, grain,
        // direction, cursor, and transaction path remain active.
        setForegroundColor({17, 42, 99, 0});
        setActiveTool(core::ToolId::Eraser);
        smokeCheck(eraserAction_ && eraserAction_->isChecked(),
            QStringLiteral("Eraser mode action did not reflect the active mode"));
        const QPointF start {
            static_cast<double>(canvasWindow_->width()) * 0.42,
            static_cast<double>(canvasWindow_->height()) * 0.48,
        };
        const QPointF finish {
            static_cast<double>(canvasWindow_->width()) * 0.58,
            static_cast<double>(canvasWindow_->height()) * 0.54,
        };
        QMouseEvent press(QEvent::MouseButtonPress, start,
            QPointF(canvasWindow_->mapToGlobal(start.toPoint())),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &press);
        smokeCheck(activeBrushStroke_
                && activeBrushStroke_->compositeMode()
                    == core::BrushCompositeMode::Erase,
            QStringLiteral("Eraser input did not enter the shared erase compositor"));
        QMouseEvent move(QEvent::MouseMove, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &move);
        smokeCheck(activeBrushStroke_
                && activeBrushStroke_->lastResolvedTipAngleDegrees().has_value()
                && std::abs(std::remainder(
                    canvasWindow_->scene().brushTipAngleDegrees
                        - *activeBrushStroke_->lastResolvedTipAngleDegrees(),
                    360.0)) < 1.0e-9,
            QStringLiteral("Vulkan eraser cursor diverged from the resolved tip angle"));
        QMouseEvent release(QEvent::MouseButtonRelease, finish,
            QPointF(canvasWindow_->mapToGlobal(finish.toPoint())),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);

        smokeCheck(session().history().undoDepth() == historyDepthBefore + 1,
            QStringLiteral("Eraser stroke did not create exactly one history entry"));
        smokeCheck(session().history().undoLabel()
                == std::string_view("Eraser stroke"),
            QStringLiteral("Eraser history entry has the wrong label"));
        smokeCheck(raster.surface->revision() > revisionBefore,
            QStringLiteral("Eraser did not mutate the CPU-authoritative surface"));
        smokeEraserRevision_ = raster.surface->revision();
        synchronizeUi(false, false);
    });
    afterRasterReady(770, [this] {
        undoAction_->trigger();
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("Eraser smoke target disappeared before undo"));
            return;
        }
        const auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Eraser smoke undo target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        smokeCheck(raster.surface->revision() == smokeEraserRevision_ + 1,
            QStringLiteral("Eraser undo did not produce one surface revision"));
    });
    afterRasterReady(860, [this] {
        redoAction_->trigger();
        if (!session().document() || !session().activeLayer()) {
            smokeCheck(false, QStringLiteral("Eraser smoke target disappeared before redo"));
            return;
        }
        const auto* layer = session().document()->layer(*session().activeLayer());
        if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
            smokeCheck(false, QStringLiteral("Eraser smoke redo target is not raster"));
            return;
        }
        const auto& raster = std::get<core::RasterLayer>(layer->payload);
        smokeCheck(raster.surface->revision() == smokeEraserRevision_ + 2,
            QStringLiteral("Eraser redo did not produce one surface revision"));
        if (smokeBrushAssetBaseline_) {
            const auto assetStats = brushAssets_->runtimeStats();
            smokeCheck(assetStats.resourceReads
                    == smokeBrushAssetBaseline_->resourceReads
                    && assetStats.imageDecodes
                        == smokeBrushAssetBaseline_->imageDecodes
                    && assetStats.mipBuilds
                        == smokeBrushAssetBaseline_->mipBuilds,
                QStringLiteral("Eraser paint/undo/redo performed hot-path asset IO"));
            smokeCheck(assetStats.resolverHits
                    == smokeBrushAssetBaseline_->resolverHits + 4
                    && assetStats.resolverMisses
                        == smokeBrushAssetBaseline_->resolverMisses,
                QStringLiteral("Eraser took a separate or unexpected asset resolver path"));
        }
        setActiveTool(core::ToolId::Move);
    });
    afterRasterReady(980, [this] {
        if (!session().document()) {
            smokeCheck(false, QStringLiteral("No document for eyedropper smoke"));
            return;
        }
        const auto revision = session().document()->revision();
        const auto depth = session().history().undoDepth();
        const auto memory = session().history().memoryUsed();
        const auto assetStats = brushAssets_->runtimeStats();
        setActiveTool(core::ToolId::Eraser);
        const QPointF point {canvasWindow_->width() * 0.5, canvasWindow_->height() * 0.4};
        const auto expected = core::sampleDocumentColor(*session().document(), session().activeLayer(),
            canvasWindow_->documentPositionForLogical(point), core::ColorSampleSource::MergedVisible);
        QMouseEvent press(QEvent::MouseButtonPress, point,
            QPointF(canvasWindow_->mapToGlobal(point.toPoint())),
            Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
        QCoreApplication::sendEvent(canvasWindow_, &press);
        smokeCheck(expected.available() && session().foregroundColor() == expected.color
                && canvasWindow_->scene().eyedropperActive && !activeBrushStroke_,
            QStringLiteral("Alt Eraser did not sample the CPU document"));
        QMouseEvent release(QEvent::MouseButtonRelease, point,
            QPointF(canvasWindow_->mapToGlobal(point.toPoint())),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvasWindow_, &release);
        smokeCheck(session().activeTool() == core::ToolId::Eraser
                && !canvasWindow_->scene().eyedropperActive,
            QStringLiteral("Alt sampling changed the selected brush/erase mode"));
        setActiveTool(core::ToolId::Eyedropper);
        setForegroundColor({231, 80, 103, 180});
        canvasWindow_->refreshColorSample();
        smokeCheck(canvasWindow_->scene().eyedropperSampleValid
                && canvasWindow_->scene().eyedropperCandidate == expected.color,
            QStringLiteral("Vulkan picker preview diverged from the CPU sample"));
        smokeCheck(session().document()->revision() == revision
                && session().history().undoDepth() == depth
                && session().history().memoryUsed() == memory,
            QStringLiteral("Eyedropper/color state modified document history"));
        const auto after = brushAssets_->runtimeStats();
        smokeCheck(after.resourceReads == assetStats.resourceReads
                && after.imageDecodes == assetStats.imageDecodes
                && after.mipBuilds == assetStats.mipBuilds,
            QStringLiteral("Sampling performed brush asset IO"));
    });
    struct TransformSmokeState {
        render::RendererStats baseline;
        core::AffineTransform before, after;
        core::Revision rasterRevision {0};
        std::size_t historyDepth {0};
        QRect canvasGeometry;
    };
    const auto transformSmoke=std::make_shared<TransformSmokeState>();
    afterRasterReady(1220, [this,transformSmoke] {
        transformSmoke->baseline=canvasWindow_->rendererStats();
        transformSmoke->canvasGeometry=canvasWindow_->geometry();
        transformSmoke->historyDepth=session().history().undoDepth();
        setActiveTool(core::ToolId::Move);
        auto* x = moveOptionsPage_->findChild<QDoubleSpinBox*>(QStringLiteral("MoveXControl"));
        auto* angle = moveOptionsPage_->findChild<QDoubleSpinBox*>(QStringLiteral("MoveAngleControl"));
        auto* flip = moveOptionsPage_->findChild<QToolButton*>(QStringLiteral("MoveFlipHorizontal"));
        smokeCheck(x && angle && flip, QStringLiteral("Move top-bar controls missing"));
        if (!x || !angle || !flip || !session().activeLayer()) return;
        transformSmoke->before = session().document()->layer(*session().activeLayer())->localToDocument;
        x->setValue(x->value() + 13.25);
        angle->setValue(angle->value() + 17.0);
        flip->click();
        smokeCheck(!layerTransform_ && !canvasWindow_->scene().transformOverlay,
            QStringLiteral("Move numeric edits entered a Ctrl+T session"));
        smokeCheck(session().history().undoDepth() == transformSmoke->historyDepth + 3,
            QStringLiteral("Move controls did not create three independent undo actions"));
    });
    afterRasterReady(1400, [this,transformSmoke] {
        smokeCheck(canvasWindow_->rendererStats().framesSubmitted > transformSmoke->baseline.framesSubmitted,
            QStringLiteral("Move control edits were not rendered"));
        undoAction_->trigger();
        undoAction_->trigger();
        undoAction_->trigger();
        smokeCheck(session().activeLayer() && session().document()->layer(*session().activeLayer())->localToDocument
                == transformSmoke->before,
            QStringLiteral("Move controls undo lost original geometry"));
        beginLayerTransform();
        smokeCheck(layerTransform_!=nullptr,QStringLiteral("Could not enter transform session"));
        if (!layerTransform_) return;
        transformSmoke->before=layerTransform_->transform();
        const auto* layer=session().document()->layer(layerTransform_->layerId());
        transformSmoke->rasterRevision=std::get<core::RasterLayer>(layer->payload).surface->revision();
        auto values=layerTransform_->values();
        values.scaleX=-0.65; values.scaleY=0.72; values.rotationDegrees=28.0;
        values.center=values.center+core::Vec2d {43,25};
        smokeCheck(layerTransform_->setValues(values),QStringLiteral("Transform preview rejected valid affine"));
        smokeCheck(layerTransform_->completeAction(),QStringLiteral("Transform action was not recorded"));
        const auto firstAction = layerTransform_->transform();
        smokeCheck(layerTransform_->flip(true),QStringLiteral("Transform Flip action failed"));
        smokeCheck(layerTransform_->pendingHistory().undoDepth()==2,
            QStringLiteral("Transform did not retain separate numeric and Flip actions"));
        stepTransformHistory(false);
        smokeCheck(layerTransform_ && layerTransform_->transform()==firstAction,
            QStringLiteral("Local transform undo lost mode or geometry"));
        stepTransformHistory(true);
        refreshLayerTransform();
        transformSmoke->after=layerTransform_->transform();
    });
    afterRasterReady(1570, [this,transformSmoke] {
        smokeCheck(canvasWindow_->rendererStats().framesSubmitted>transformSmoke->baseline.framesSubmitted,
            QStringLiteral("Transform overlay was never rendered"));
        finishLayerTransform(true);
        smokeCheck(session().history().undoDepth()==transformSmoke->historyDepth+2,
            QStringLiteral("Apply collapsed individual transform actions"));
        undoAction_->trigger();
        undoAction_->trigger();
        if (session().activeLayer()) smokeCheck(session().document()->layer(*session().activeLayer())->localToDocument==transformSmoke->before,
            QStringLiteral("Transform undo did not restore exact geometry"));
    });
    afterRasterReady(1800, [this,transformSmoke] {
        redoAction_->trigger();
        redoAction_->trigger();
        if (session().activeLayer()) smokeCheck(session().document()->layer(*session().activeLayer())->localToDocument==transformSmoke->after,
            QStringLiteral("Transform redo did not restore exact geometry"));
    });
    afterRasterReady(2030, [this,transformSmoke] {
        beginLayerTransform();
        if (layerTransform_) {
            (void)layerTransform_->flip(true);
            refreshLayerTransform();
            finishLayerTransform(false);
        }
        const auto* layer=session().activeLayer() ? session().document()->layer(*session().activeLayer()) : nullptr;
        smokeCheck(layer && layer->localToDocument==transformSmoke->after,
            QStringLiteral("Transform cancellation lost original geometry"));
        smokeCheck(layer && std::get<core::RasterLayer>(layer->payload).surface->revision()==transformSmoke->rasterRevision,
            QStringLiteral("Transform rewrote raster pixels"));
        smokeCheck(canvasWindow_->geometry()==transformSmoke->canvasGeometry,
            QStringLiteral("Changing tool options resized the canvas"));
        const auto stats=canvasWindow_->rendererStats();
        smokeCheck(stats.fullUploads==transformSmoke->baseline.fullUploads
                && stats.regionalUploads==transformSmoke->baseline.regionalUploads
                && stats.uploadedBytes==transformSmoke->baseline.uploadedBytes,
            QStringLiteral("Transform-only updates uploaded raster textures"));
        smokeCheck(stats.resourceGeneration==transformSmoke->baseline.resourceGeneration
                && stats.swapchainGeneration==transformSmoke->baseline.swapchainGeneration,
            QStringLiteral("Transform recreated Vulkan resources"));
    });
    afterRasterReady(2320, [this] {
        const auto stats = canvasWindow_->rendererStats();
        smokeCheck(canvasWindow_->geometry() == canvasContainer_->rect(),
            QStringLiteral("Vulkan canvas/container geometry diverged after lifecycle exercise"));
        smokeCheck(canvasContainer_->geometry() == workspace_->rect()
                && workspace_->panelOverlay()->geometry() == workspace_->rect(),
            QStringLiteral("Stationary canvas/panel topology diverged after lifecycle exercise"));
        smokeCheck(stats.framesSubmitted > 0, QStringLiteral("No Vulkan frames were submitted"));
        smokeCheck(stats.resourceGeneration > 0,
            QStringLiteral("Vulkan device resources were never initialized"));
        smokeCheck(stats.swapchainGeneration > 1,
            QStringLiteral("Vulkan swapchain was not recreated during lifecycle exercise"));
        smokeCheck(stats.fullUploads > 0, QStringLiteral("No full raster upload was observed"));
        smokeCheck(stats.regionalUploads > 0,
            QStringLiteral("No dirty-region raster upload was observed"));
        smokeCheck(stats.regionalUploads <= stats.regionalDirtyRegions,
            QStringLiteral("GPU upload coalescing expanded the dirty-region count"));
        smokeCheck(stats.regionalDirtyRegions >= 4096,
            QStringLiteral("Fragmented raster upload probe was not observed"));
        smokeCheck(stats.regionalUploads * 8 < stats.regionalDirtyRegions,
            QStringLiteral("Fragmented dirty regions were not materially coalesced"));
        smokeCheck(stats.stagingBufferAllocations > 0,
            QStringLiteral("No reusable Vulkan staging buffer was allocated"));
        smokeCheck(stats.stagingBufferAllocations
                <= stats.fullUploads + stats.regionalUploadBatches,
            QStringLiteral("Vulkan staging allocation occurred per dirty rectangle"));
        smokeCheck(stats.maximumUploadPreparationNanoseconds < 50'000'000,
            QStringLiteral("Raster upload preparation exceeded 50 ms"));
        smokeCheck(!stats.deviceName.empty(), QStringLiteral("No Vulkan device was selected"));
        smokeCheck(!canvasWindow_->presentationSuppressedForResize(),
            QStringLiteral("Canvas presentation remained suppressed after lifecycle exercise"));
        smokeCheck(stats.resizePresentationSuspends
                == stats.resizePresentationResumes,
            QStringLiteral("Canvas resize presentation suspend/resume counts diverged"));
        smokeCompleted_ = true;
        logRendererDiagnostics();
        if (smokeFailures_.isEmpty()) {
            qInfo() << "Wayland/Vulkan smoke assertions passed";
        } else {
            qCritical() << "Wayland/Vulkan smoke assertions failed:" << smokeFailures_;
        }
        close();
    });
}

bool MainWindow::integrationSmokePassed() const noexcept
{
    return smokeCompleted_ && smokeFailures_.isEmpty();
}

void MainWindow::smokeCheck(bool condition, const QString& failure)
{
    if (!condition) {
        smokeFailures_.push_back(failure);
    }
}

void MainWindow::addRasterLayer()
{
    auto* document = session().document();
    if (!document) {
        return;
    }
    try {
        auto surface = std::make_shared<core::ContiguousRasterSurface>(document->canvas().extent, core::Rgba8 {});
        auto layer = core::Layer::raster(
            "Layer " + std::to_string(document->layers().size() + 1U), std::move(surface));
        const auto id = layer.id;
        if (executeDocumentCommand(std::make_unique<core::AddLayerCommand>(
                std::move(layer), document->layers().size(), session().activeLayer()))) {
            session().setActiveLayer(id);
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& exception) {
        QMessageBox::critical(this, QStringLiteral("Unable to add layer"),
            QString::fromUtf8(exception.what()));
    }
}

void MainWindow::setActiveTool(core::ToolId tool)
{
    if(tool==core::ToolId::Crop && !layerCrop_ && !startingCrop_){
        beginLayerCrop();
        if(!layerCrop_){
            const auto active=session().activeTool()==core::ToolId::Transform?core::ToolId::Move:session().activeTool();
            if(const auto action=toolActionMap_.find(active);action!=toolActionMap_.end())action->second->setChecked(true);
        }
        return;
    }
    if (tool != session().activeTool()) {
        if(session().activeTool()==core::ToolId::Shape) {
            shapeOptionsPage_->finishNumericInput();
            canvasWindow_->setTransformOverlay({});
        }
        if (core::isSelectionTool(session().activeTool())) finishSelectionNumericInput();
        // Rail buttons do not take QWidget focus. Finish numeric text while
        // Move still owns it, before hiding the page changes its callback target.
        if (session().activeTool() == core::ToolId::Move
            && (!activeLayerMove_ || !activeLayerMove_->dragging()))
            moveOptionsPage_->finishNumericInput();
        cancelPendingEdits();
        if (textController_ && textController_->active())
            return;
    }
    session().setActiveTool(tool);
    if(tool==core::ToolId::SmartSelect && !smartEverActivated_){
        smartEverActivated_=true;selectionOperation_=core::SelectionOperation::Add;
        canvasWindow_->setSelectionOperation(selectionOperation_);
    }
    if (tool == core::ToolId::Measure) refreshMeasureBounds();
    // Eraser is a compositing mode of Brush. The session/canvas retain the
    // exclusive Brush/Eraser selection for coherent rail highlighting, while
    // both modes present the exact same settings UI.
    const auto presentedTool = tool == core::ToolId::Eraser
        ? core::ToolId::Brush : tool;
    propertiesPanel_->setActiveTool(tool == core::ToolId::Transform ? core::ToolId::Move : presentedTool);
    toolOptionsBar_->setActiveTool(presentedTool);
    canvasWindow_->setLayerOutlineTargets(session().document()
        ? session().document()->expandedLayers(session().selectedLayers()) : std::vector<core::LayerId>{});
    canvasWindow_->setActiveTool(tool);
    const auto found = toolActionMap_.find(tool == core::ToolId::Transform ? core::ToolId::Move : tool);
    if (found != toolActionMap_.end()) {
        found->second->setChecked(true);
    }
    statusBar()->showMessage(QStringLiteral("%1 tool").arg(toolName(tool)), 1600);
    updateActionState();
}

void MainWindow::collapseLayerSelectionOnEmptyClick()
{
    // A blank click is session selection only, never an implicit Apply/Cancel
    // for a transform, path construction or captured pointer gesture.
    if (!collapseLayerSelectionOnEmptyClick_ || updatingUi_ || fileBusy_ || workspaceDialog_
        || layerTransform_ || layerCrop_ || selectionTransform_ || activeFill_
        || canvasWindow_->pointerGestureActive() || session().selectedLayers().size() <= 1)
        return;
    if (session().activeTool() == core::ToolId::Move)
        moveOptionsPage_->finishNumericInput();
    const auto primary = session().activeLayer();
    if (!primary || !session().isLayerSelected(*primary)) return;
    session().setActiveLayer(primary); // Stable ID, not displayed row or stack order.
    synchronizeUi(false, false);
    canvasWindow_->dismissLayerOutlines();
}

void MainWindow::selectLayerFromRow(int row, Qt::KeyboardModifiers modifiers, bool contextSelection)
{
    if (updatingUi_ || fileBusy_) {
        return;
    }
    if (core::isSelectionTool(session().activeTool())) finishSelectionNumericInput();
    if (session().activeTool() == core::ToolId::Move
        && (!activeLayerMove_ || !activeLayerMove_->dragging()))
        moveOptionsPage_->finishNumericInput();
    cancelPendingEdits();
    const auto id=layerModel_->layerIdAt(row);
    if (contextSelection && id && session().isLayerSelected(*id)) {
        session().setLayerSelection(session().selectedLayers(), id, session().selectionAnchor());
    } else if(id && modifiers.testFlag(Qt::ShiftModifier)) {
        std::vector<core::LayerId> order;
        for(int i=0;i<layerModel_->rowCount();++i) order.push_back(*layerModel_->layerIdAt(i));
        session().selectLayerRange(*id,order,modifiers.testFlag(Qt::ControlModifier));
    } else if(id && modifiers.testFlag(Qt::ControlModifier)) session().toggleSelectedLayer(*id);
    else session().setActiveLayer(id);
    if (session().document())
        canvasWindow_->setLayerOutlineTargets(session().document()->expandedLayers(session().selectedLayers()), true);
    // Completing a numeric edit may have synchronized the old selection.
    synchronizeLayerSelection();
    updateLayerControls();
    updateActionState();
    canvasWindow_->refreshColorSample();
    refreshShapeControls();
    refreshShapeOverlay();
}

void MainWindow::synchronizeUi(bool resetLayerModel, bool fitCanvas)
{
    prepareShapeCaches();
    if (textController_)
        textController_->prepareCaches();
    if (resetLayerModel) {
        const QSignalBlocker guard(layerList_->selectionModel());
        layerModel_->refresh();
    }
    synchronizeLayerSelection();
    updateLayerControls();
    updateActionState();
    updateStatusText();
    if (session().document()) {
        canvasWindow_->setDocument(session().document()->snapshot(), fitCanvas);
    }
    refreshShapeControls();
    refreshShapeOverlay();
}

void MainWindow::synchronizeLayerSelection()
{
    updatingUi_ = true;
    const QSignalBlocker guard(layerList_->selectionModel());
    layerModel_->refreshSelectionIndicators();
    QItemSelection selection;
    for(const auto id:session().selectedLayers()) {
        const auto row=layerModel_->rowForLayer(id);
        if(row>=0) selection.select(layerModel_->index(row),layerModel_->index(row));
    }
    layerList_->selectionModel()->select(selection,QItemSelectionModel::ClearAndSelect);
    const auto row=session().activeLayer()?layerModel_->rowForLayer(*session().activeLayer()):-1;
    layerList_->selectionModel()->setCurrentIndex(row>=0?layerModel_->index(row):QModelIndex{},QItemSelectionModel::NoUpdate);
    layerList_->viewport()->update();
    updatingUi_ = false;
}

void MainWindow::updateLayerControls()
{
    canvasWindow_->setLayerOutlineTargets(session().document()
        ? session().document()->expandedLayers(session().selectedLayers()) : std::vector<core::LayerId>{});
    refreshMeasureBounds();
    updatingUi_ = true;
    const core::Layer* active = nullptr;
    if (session().document() && session().activeLayer()) {
        active = session().document()->layer(*session().activeLayer());
    }
    opacitySlider_->setEnabled(active != nullptr);
    if (!publishingOpacity_ || !opacitySlider_->isManualEntryActive())
        opacitySlider_->setValue(active ? static_cast<int>(std::lround(active->opacity * 100.0F)) : 100);
    const auto* container = session().document() && session().activeLayer()
        ? session().document()->tree().container(*session().activeLayer()) : nullptr;
    // Containers organize pass-through content. They never gain an isolated
    // compositor or their own blend properties merely by being selected.
    if (container && blendModeCombo_->count() == int(core::allBlendModes.size()))
        blendModeCombo_->addItem(QStringLiteral("Pass Through"));
    else if (!container && blendModeCombo_->count() > int(core::allBlendModes.size()))
        blendModeCombo_->removeItem(int(core::allBlendModes.size()));
    blendModeCombo_->setEnabled(active != nullptr && !fileBusy_);
    opacitySlider_->setPrefix(active && std::holds_alternative<core::AdjustmentLayer>(active->payload)?tr("Strength: "):tr("Opacity: "));
    blendModeCombo_->setCurrentIndex(container ? int(core::allBlendModes.size())
        : blendModeCombo_->findData(int(active ? active->blendMode : core::BlendMode::Normal)));
    propertiesPanel_->setSelectedLayer(active, container);
    refreshAdjustmentPanel();
    refreshFiltersPanel();
    updatingUi_ = false;
    refreshMoveControls();
}

void MainWindow::updateActionState()
{
    refreshLayerMaskActions();
    refreshCloningControls();
    refreshShapeControls();
    refreshShapeOverlay();
    updateDocumentTitle();
    refreshSelectionControls();
    const bool selectionCommandsAvailable = session().document() && !layerTransform_ && !selectionTransform_;
    for (auto* action : selectionActions_) action->setEnabled(selectionCommandsAvailable);
    const bool hasActiveSelection = session().document() && session().document()->selection();
    selectionActions_[1]->setEnabled(selectionCommandsAvailable && hasActiveSelection);
    selectionActions_[2]->setEnabled(selectionCommandsAvailable && hasActiveSelection);
    selectionActions_[4]->setEnabled(selectionCommandsAvailable && session().document()->lastSelection());
    selectionActions_[3]->setEnabled(session().document() && session().activeLayer() && !layerTransform_ && !selectionTransform_);
    const auto& history = layerCrop_ ? layerCrop_->pendingHistory() : layerTransform_ ? layerTransform_->pendingHistory() : session().history();
    undoAction_->setEnabled(history.canUndo() || activeLayerMove_ != nullptr || selectionGesture_.has_value() || selectionRotation_.has_value()
        || (layerTransform_ && layerTransform_->dragging()));
    redoAction_->setEnabled(history.canRedo());
    const auto* layer = session().document() && session().activeLayer()
        ? session().document()->layer(*session().activeLayer()) : nullptr;
    transformAction_->setEnabled(layer && !core::layerGeometryExtent(*layer).empty()
        && layer->localToDocument.inverted().has_value());
    if(session().document()) {
        const auto items=session().document()->tree().normalize(session().selectedLayers());
        const bool containsFolder=std::ranges::any_of(items,[&](auto id){const auto* c=session().document()->tree().container(id);return c&&c->kind==core::ContainerKind::Folder;});
        if(containsFolder)transformAction_->setEnabled(false);
        else if(session().activeLayer() && !layer)transformAction_->setEnabled(!session().document()->expandedLayers(items).empty());
    }
    editTextButton_->setEnabled(layer && std::holds_alternative<core::TextLayer>(layer->payload));
    undoAction_->setText(history.canUndo()
        ? QStringLiteral("Undo %1").arg(QString::fromUtf8(history.undoLabel()))
        : QStringLiteral("Undo"));
    redoAction_->setText(history.canRedo()
        ? QStringLiteral("Redo %1").arg(QString::fromUtf8(history.redoLabel()))
        : QStringLiteral("Redo"));
    if (selectionTransform_) {
        const auto& edit = *selectionTransform_;
        undoAction_->setEnabled(edit.checkpoint > 0 || edit.drag.has_value());
        redoAction_->setEnabled(edit.checkpoint + 1 < edit.checkpoints.size());
        undoAction_->setText(edit.pixels?tr("Undo selected-pixel transform"):tr("Undo selection transform"));
        redoAction_->setText(edit.pixels?tr("Redo selected-pixel transform"):tr("Redo selection transform"));
    }
    const bool selectionTool = core::isSelectionTool(session().activeTool());
    if (selectionTool) {
        const auto mask = session().document() ? session().document()->selection() : core::SelectionState{};
        transformAction_->setEnabled(mask && !mask->bounds().empty());
    }
    transformAction_->setText(selectionTool || selectionTransform_ ? QStringLiteral("Transform selection") : QStringLiteral("Transform layer"));
    transformAction_->setChecked(layerTransform_ || selectionTransform_.has_value());
    transformSelectionAction_->setEnabled(hasActiveSelection && !session().document()->selection()->bounds().empty()
        && !fileBusy_ && !layerTransform_ && !selectionTransform_ && !layerCrop_);
    transformPixelsAction_->setEnabled(transformSelectionAction_->isEnabled()&&!session().editingLayerMask());
    const bool canFill = layer && (session().editingLayerMask()||std::holds_alternative<core::RasterLayer>(layer->payload))
        && layer->localToDocument.inverted() && !layerTransform_ && !selectionTransform_ && !activeFill_;
    for (auto* fill : fillActions_) fill->setEnabled(canFill);
    if (activeFill_) { undoAction_->setEnabled(true); redoAction_->setEnabled(false); transformAction_->setEnabled(false); }
    if (adjustmentEdit_||filterEdit_) { undoAction_->setEnabled(true); redoAction_->setEnabled(false); }
    if(activeLocalBlurStroke_){undoAction_->setEnabled(true);redoAction_->setEnabled(false);transformAction_->setEnabled(false);}
    if(activeSpotHealStroke_ || spotHealBusyForActiveDocument()){undoAction_->setEnabled(true);redoAction_->setEnabled(false);transformAction_->setEnabled(false);}
    if(layerCrop_){
        undoAction_->setEnabled(history.canUndo()||layerCrop_->dragging());
        transformAction_->setEnabled(false);
        for(auto* action:selectionActions_)action->setEnabled(false);
        for(auto* action:fillActions_)action->setEnabled(false);
    }
    if (selectionRasterizer_) { undoAction_->setEnabled(true); redoAction_->setEnabled(false); transformAction_->setEnabled(false); }
    if (colorSelectionEditing_) { undoAction_->setEnabled(true); redoAction_->setEnabled(false); transformAction_->setEnabled(false); }
    if (smartInteractionActive()) { undoAction_->setEnabled(true); redoAction_->setEnabled(false); transformAction_->setEnabled(false); }
    if (anchoredLassoActive()) { redoAction_->setEnabled(false); transformAction_->setEnabled(false); }
    if(shapeCreation_ || shapeResize_) {undoAction_->setEnabled(true);redoAction_->setEnabled(false);transformAction_->setEnabled(false);}
    const bool canDeleteLayer = session().document() && !fileBusy_ && !session().selectedLayers().empty()
        && session().document()->expandedLayers(session().selectedLayers()).size() < session().document()->layers().size();
    const auto* activeContainer=session().document() && session().activeLayer()?session().document()->tree().container(*session().activeLayer()):nullptr;
    newFolderAction_->setEnabled(session().document() && !fileBusy_);
    newAdjustmentLayerAction_->setEnabled(session().document() && !fileBusy_);
    renameLayerAction_->setEnabled(session().activeLayer().has_value() && !fileBusy_);
    groupLayersAction_->setEnabled(session().document() && !session().selectedLayers().empty() && !fileBusy_);
    mergeLayersAction_->setEnabled(groupLayersAction_->isEnabled());
    clippingGroupAction_->setEnabled(groupLayersAction_->isEnabled());
    clippingGroupAction_->setText(activeContainer && session().selectedLayers().size()==1 && core::isGroup(activeContainer->kind)
        ? activeContainer->kind==core::ContainerKind::ClippingMaskGroup ? tr("Convert to Ordinary Group") : tr("Convert to Clipping Mask Group")
        : tr("Add to Clipping Mask Group"));
    rasterizeLayersAction_->setEnabled(groupLayersAction_->isEnabled());
    duplicateLayersAction_->setEnabled(groupLayersAction_->isEnabled());
    ungroupLayersAction_->setEnabled(activeContainer && core::isGroup(activeContainer->kind) && !fileBusy_);
    for (std::size_t i = 0; i < layerVisibilityActions_.size(); ++i)
        layerVisibilityActions_[i]->setEnabled(session().document() && !fileBusy_
            && (i == 3 || !session().selectedLayers().empty()));
    deleteLayerAction_->setEnabled(canDeleteLayer);
    if (deleteLayerButton_) {
        deleteLayerButton_->setEnabled(canDeleteLayer);
        deleteLayerButton_->setToolTip(canDeleteLayer
                ? QStringLiteral("Delete selected layers · %1").arg(shortcutLabel(shortcuts_, "DeleteSelectedLayersAction"))
                : QStringLiteral("A document must keep at least one layer"));
    }
}

void MainWindow::updateStatusText()
{
    if (!session().document()) {
        documentStatus_->setText(QStringLiteral("No document"));
        documentStatus_->setToolTip(documentStatus_->text());
        zoomStatus_->clear();
        return;
    }
    const auto& canvas = session().document()->canvas();
    const auto layerCount = session().document()->layers().size();
    documentStatus_->setText(QStringLiteral("%1 × %2 px  ·  %3 layer%4  ·  sRGB RGBA8")
        .arg(canvas.extent.width)
        .arg(canvas.extent.height)
        .arg(layerCount)
        .arg(layerCount == 1 ? QString() : QStringLiteral("s")));
    if (const auto& selection = session().document()->selection()) {
        const auto bounds = selection->bounds();
        documentStatus_->setText(documentStatus_->text() + (bounds.empty()
            ? QStringLiteral("  ·  Empty selection — editing blocked")
            : QStringLiteral("  ·  Selection %1 × %2 px").arg(bounds.width).arg(bounds.height)));
    }
    if(session().editingLayerMask())documentStatus_->setText(documentStatus_->text()+tr("  ·  Editing MASK — white reveals / black hides"));
    zoomStatus_->setText(QStringLiteral("%1%")
        .arg(static_cast<int>(std::lround(canvasWindow_->zoom() * 100.0))));
    documentStatus_->setToolTip(documentStatus_->text());
}

void MainWindow::mutateRasterForUploadProbe()
{
    if (!session().document() || !session().activeLayer()) {
        return;
    }
    auto* layer = session().document()->layer(*session().activeLayer());
    if (!layer || !std::holds_alternative<core::RasterLayer>(layer->payload)) {
        return;
    }
    auto& raster = std::get<core::RasterLayer>(layer->payload);
    if (!raster.surface) {
        return;
    }
    const auto extent = raster.surface->extent();
    const auto columns = std::min<std::uint32_t>(128, extent.width);
    const auto rows = std::min<std::uint32_t>(32, extent.height);
    const auto patchCount = static_cast<std::size_t>(columns)
        * static_cast<std::size_t>(rows);
    std::vector<std::array<std::byte, 4>> pixels(patchCount,
        {std::byte {101}, std::byte {119}, std::byte {243}, std::byte {255}});
    std::vector<core::RasterPatch> patches;
    patches.reserve(patchCount);
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto index = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(columns)
                + static_cast<std::size_t>(column);
            const auto x = std::min(static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(column) * extent.width / columns),
                extent.width - 1U);
            const auto y = std::min(static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(row) * extent.height / rows),
                extent.height - 1U);
            patches.push_back({
                {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 1, 1},
                pixels[index], 4});
        }
    }
    raster.surface->replaceRgba8Batch(patches);
    canvasWindow_->setDocument(session().document()->snapshot(), false);
    qInfo() << "Applied fragmented dirty-region upload probe with"
            << patches.size() << "one-pixel regions";
}

bool MainWindow::beginBrushStroke(const core::NormalizedPointerSample& sample)
{
    if(session().editingLayerMask()&&session().activeTool()!=core::ToolId::Brush&&session().activeTool()!=core::ToolId::Eraser) {
        statusBar()->showMessage(tr("Use Brush, Eraser or Fill on a layer mask. Click the content thumbnail for other pixel tools."),4000);return false;
    }
    if(session().activeTool()==core::ToolId::LocalBlur)return beginLocalBlurStroke(sample);
    if(session().activeTool()==core::ToolId::Cloning)return beginCloneStroke(sample);
    const auto activeTool = session().activeTool();
    if ((activeTool != core::ToolId::Brush
            && activeTool != core::ToolId::Eraser)
        || !session().document()
        || !session().activeLayer()) {
        return false;
    }
    auto* layer = session().document()->layer(*session().activeLayer());
    if (!layer || (!session().editingLayerMask()&&!std::holds_alternative<core::RasterLayer>(layer->payload))) {
        statusBar()->showMessage(
            QStringLiteral("Select a raster layer before painting"), 2500);
        return false;
    }
    cancelActiveBrushStroke();
    auto strokeSettings = brushSettings_;
    strokeSettings.foreground = session().foregroundColor();
    const bool mask=session().editingLayerMask();
    if(mask&&activeTool==core::ToolId::Eraser)strokeSettings.foreground={0,0,0,255};
    const auto unavailable = brushAssets_->unavailableMessage(strokeSettings);
    if (!unavailable.isEmpty()) {
        statusBar()->showMessage(unavailable, 4500);
        return false;
    }
    if(mask) {
        try {activeMaskEdit_=std::make_unique<core::LayerMaskEdit>(*session().document(),layer->id,"Paint layer mask");}
        catch(const std::exception& e){statusBar()->showMessage(QString::fromUtf8(e.what()),4500);return false;}
    }
    core::RasterEditTransactionOptions transactionOptions;transactionOptions.coverageValues=mask;
    activeBrushStroke_ = std::make_unique<core::BasicPixelBrushStroke>(
        mask?activeMaskEdit_->proxy():*session().document(), *session().activeLayer(), std::move(strokeSettings),
        mask?core::BrushCompositeMode::MaskCoverage:activeTool == core::ToolId::Eraser
            ? core::BrushCompositeMode::Erase
            : core::BrushCompositeMode::Paint,
        std::make_unique<core::BasicPixelBrushEngine>(),
        std::unique_ptr<core::IBrushTip> {},
        std::unique_ptr<core::IBrushGrain> {}, brushAssets_.get(),transactionOptions);
    const auto rasterGeometryRevision=session().document()->revision();
    if (!activeBrushStroke_->valid() || !activeBrushStroke_->begin(sample)) {
        const bool limited=activeBrushStroke_->failure()==core::BrushStrokeFailure::RasterWorkLimitExceeded;
        activeBrushStroke_.reset();
        activeMaskEdit_.reset();
        statusBar()->showMessage(limited
            ? QStringLiteral("Stroke cancelled: too many source pixels at this layer scale. Reduce brush size or enlarge the layer.")
            : QStringLiteral("Unable to begin brush stroke"), limited ? 6500 : 2500);
        return false;
    }
    if (const auto angle = activeBrushStroke_->lastResolvedTipAngleDegrees()) {
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    }
    canvasWindow_->setConstrainedBrushPosition(activeBrushStroke_->constrainedPosition());
    if(activeMaskEdit_ || session().document()->revision()!=rasterGeometryRevision)
        canvasWindow_->setDocument(session().document()->snapshot(),false);
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::moveBrushStroke(const core::NormalizedPointerSample& sample)
{
    if(activeSpotHealStroke_)return moveSpotHealStroke(sample);
    if(activeCloneStroke_)return moveCloneStroke(sample);
    if(activeLocalBlurStroke_)return moveLocalBlurStroke(sample);
    const auto rasterGeometryRevision=session().document()?session().document()->revision():0;
    if (!activeBrushStroke_ || !activeBrushStroke_->append(sample)) {
        cancelActiveBrushStroke();
        return false;
    }
    if (const auto angle = activeBrushStroke_->lastResolvedTipAngleDegrees()) {
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    }
    canvasWindow_->setConstrainedBrushPosition(activeBrushStroke_->constrainedPosition());
    if(activeMaskEdit_ || session().document()->revision()!=rasterGeometryRevision)
        canvasWindow_->setDocument(session().document()->snapshot(),false);
    canvasWindow_->scheduleFrame();
    return true;
}

bool MainWindow::endBrushStroke(const core::NormalizedPointerSample& sample)
{
    if(activeSpotHealStroke_)return endSpotHealStroke(sample);
    if(activeCloneStroke_)return endCloneStroke(sample);
    if(activeLocalBlurStroke_)return endLocalBlurStroke(sample);
    if (!activeBrushStroke_) {
        return false;
    }
    auto result = activeBrushStroke_->end(sample, activeMaskEdit_?activeMaskEdit_->provisionalHistory():session().history());
    if (const auto angle = activeBrushStroke_->lastResolvedTipAngleDegrees()) {
        canvasWindow_->setResolvedBrushCursorAngle(*angle);
    }
    const auto compositeMode = activeBrushStroke_->compositeMode();
    const auto stats = activeBrushStroke_->stats();
    const bool limited=activeBrushStroke_->failure()==core::BrushStrokeFailure::RasterWorkLimitExceeded;
    activeBrushStroke_.reset();
    if(activeMaskEdit_) {
        if(result==core::RasterEditCommitResult::Committed||result==core::RasterEditCommitResult::NoChanges)
            result=activeMaskEdit_->commit(session().history());
        activeMaskEdit_.reset();
        canvasWindow_->setDocument(session().document()->snapshot(),false);
    }
    if (result == core::RasterEditCommitResult::Committed) {
        fileState().untouched = false;
        synchronizeUi(false, false);
        const auto erasing = compositeMode == core::BrushCompositeMode::Erase;
        statusBar()->showMessage(
            QStringLiteral("%1 stroke · %2 dabs · %3 dirty batches")
                .arg(erasing ? QStringLiteral("Eraser") : QStringLiteral("Brush"))
                .arg(stats.emittedDabs)
                .arg(stats.surfaceWriteBatches),
            1800);
        return true;
    }
    canvasWindow_->scheduleFrame();
    updateActionState();
    if (limited) statusBar()->showMessage(QStringLiteral("Stroke cancelled: too many source pixels at this layer scale. Reduce brush size or enlarge the layer."),6500);
    return result == core::RasterEditCommitResult::NoChanges;
}

void MainWindow::cancelActiveBrushStroke(bool preserveCompletedRepair)
{
    if (!preserveCompletedRepair || activeSpotHealStroke_) cancelSpotHeal();
    cancelLocalBlurStroke();
    cancelCloneStroke();
    canvasWindow_->setConstrainedBrushPosition({});
    if (!activeBrushStroke_) {
        return;
    }
    const auto erasing = activeBrushStroke_->compositeMode()
        == core::BrushCompositeMode::Erase;
    const bool limited=activeBrushStroke_->failure()==core::BrushStrokeFailure::RasterWorkLimitExceeded;
    activeBrushStroke_->cancel();
    activeBrushStroke_.reset();
    activeMaskEdit_.reset();
    if (session().document()) {
        canvasWindow_->setDocument(session().document()->snapshot(), false);
    }
    updateActionState();
    statusBar()->showMessage(
        limited ? QStringLiteral("Stroke cancelled: too many source pixels at this layer scale. Reduce brush size or enlarge the layer.")
                : erasing ? QStringLiteral("Eraser stroke cancelled")
                : QStringLiteral("Brush stroke cancelled"),
        limited ? 6500 : 1500);
}

void MainWindow::adjustBrushSize(bool increase)
{
    if(session().activeTool()==core::ToolId::SmartSelect && smartMode_==core::SmartSelectMode::QuickSelection) {
        if(!smartInteractionActive())smartSize_->setValue(std::clamp(smartSize_->value()
            +(increase?1:-1)*std::max(1.0,std::round(smartSize_->value()*.1)),2.0,512.0));
        return;
    }
    if (session().activeTool() != core::ToolId::Brush
        && session().activeTool() != core::ToolId::Eraser && session().activeTool()!=core::ToolId::Cloning
        && session().activeTool()!=core::ToolId::LocalBlur) {
        return;
    }
    const auto factor = increase ? 1.1 : 1.0 / 1.1;
    brushSettings_.sizePixels = std::clamp(
        std::round(brushSettings_.sizePixels * factor), 1.0, 1000.0);
    applyBrushSettings(brushSettings_);
    statusBar()->showMessage(QStringLiteral("Brush size: %1 px")
            .arg(brushSettings_.sizePixels, 0, 'f', 0),
        900);
}

void MainWindow::applyBrushSettings(const core::BrushSettings& settings)
{
    brushSettings_ = settings;
    brushSettings_.foreground = session().foregroundColor();
    if(cloningOptionsPage_)cloningOptionsPage_->setBrushSettings(brushSettings_);
    if(localBlurOptionsPage_)localBlurOptionsPage_->setBrushSettings(brushSettings_);
    if (brushOptionsPage_) {
        brushOptionsPage_->setBrushSettings(brushSettings_);
    }
    if (propertiesPanel_) {
        propertiesPanel_->setBrushSettings(brushSettings_);
    }
    if (canvasWindow_) {
        canvasWindow_->setBrushCursor(brushSettings_);
    }
}

void MainWindow::setForegroundColor(core::Rgba8 color)
{
    auto colors = session().colors();
    colors.setColor(colors.active, color);
    setColors(colors);
}

void MainWindow::setColors(core::EditorColors colors)
{
    if (colorsSynchronized_ && colors == session().colors()) {
        return;
    }
    colorsSynchronized_ = true;
    session().setColors(colors);
    brushSettings_.foreground = session().foregroundColor();
    applyBrushSettings(brushSettings_);
    if (colorPanel_) {
        colorPanel_->setColors(colors);
    }
    if (railColors_) {
        railColors_->setColors(colors);
    }
    if (canvasWindow_) {
        canvasWindow_->setWorkingColor(session().foregroundColor());
    }
}

void MainWindow::registerEditorWindowAction(QAction* action)
{
    if (!action) {
        return;
    }
    action->setShortcutContext(Qt::WindowShortcut);
    if (!actions().contains(action)) {
        addAction(action);
    }
    auto* overlay = workspace_ ? workspace_->panelOverlay() : nullptr;
    if (overlay && !overlay->actions().contains(action)) {
        overlay->addAction(action);
    }
}

QAction* MainWindow::toolAction(core::ToolId tool, const QString& name,
    const QString& iconName, const QKeySequence& shortcut)
{
    auto* action = new QAction(tool==core::ToolId::Measure ? toolGlyph(ToolGlyph::Measure)
        : tool==core::ToolId::Crop ? toolGlyph(ToolGlyph::Crop)
        : tool==core::ToolId::SelectByColor ? toolGlyph(ToolGlyph::SelectByColor)
        : tool==core::ToolId::SmartSelect ? toolGlyph(ToolGlyph::QuickSelection)
        : tool==core::ToolId::Shape ? toolGlyph(ToolGlyph::ShapeRectangle)
        : tool==core::ToolId::Cloning ? toolGlyph(ToolGlyph::Cloning)
        : tool==core::ToolId::LocalBlur ? toolGlyph(ToolGlyph::LocalBlur)
        : themedIcon(QStringLiteral(":/icons/%1.svg").arg(iconName)), name, this);
    action->setObjectName(QStringLiteral("ToolAction_%1").arg(iconName));
    action->setCheckable(true);
    action->setShortcut(shortcut);
    action->setToolTip(QStringLiteral("%1 (%2)").arg(name, shortcut.toString()));
    toolActions_->addAction(action);
    // The toolbar and canvas occupy separate native shortcut windows. Register
    // the same editor action with both while retaining normal WindowShortcut
    // and ShortcutOverride semantics for editable controls and modal dialogs.
    registerEditorWindowAction(action);
    toolRail_->addAction(action);
    toolActionMap_[tool] = action;
    connect(action, &QAction::triggered, this, [this, tool] {
        // The checked Eraser action is a mode toggle in both its rail and top-
        // bar presentations. Activating it again returns to paint mode.
        if (tool == core::ToolId::Eraser
            && session().activeTool() == core::ToolId::Eraser) {
            setActiveTool(core::ToolId::Brush);
            return;
        }
        setActiveTool(tool);
    });
    return action;
}

} // namespace imageeditor::ui
