#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <cmath>

namespace imageeditor::ui {
void MainWindow::createSelectionActions()
{
    const std::array names { "SelectAllAction", "DeselectAction", "InvertSelectionAction",
        "LayerViaCopyAction", "ReselectAction" };
    const std::array labels { "Select All", "Deselect", "Invert Selection", "Layer via Copy", "Reselect Last Selection" };
    const std::array shortcuts { "Ctrl+A", "Ctrl+D", "Ctrl+Shift+I", "Ctrl+J", "Ctrl+Shift+D" };
    for (std::size_t i = 0; i < selectionActions_.size(); ++i) {
        auto* action = new QAction(QString::fromLatin1(labels[i]), this);
        action->setObjectName(QString::fromLatin1(names[i]));
        action->setShortcut(QKeySequence(QString::fromLatin1(shortcuts[i])));
        action->setToolTip(QStringLiteral("%1 · %2").arg(action->text(), action->shortcut().toString()));
        action->setAutoRepeat(false);
        selectionActions_[i] = action;
        registerEditorWindowAction(action);
        connect(action, &QAction::triggered, this, [this, i] { runSelectionAction(int(i)); });
    }
    selectionGrowAction_ = new QAction(tr("Grow / Shrink"), this);
    selectionGrowAction_->setObjectName(QStringLiteral("GrowShrinkSelectionAction"));
    selectionGrowAction_->setToolTip(tr("Show selection adjustments in the top toolbar"));
    selectionGrowAction_->setAutoRepeat(false);
    registerEditorWindowAction(selectionGrowAction_);
    connect(selectionGrowAction_, &QAction::triggered, this, &MainWindow::openSelectionAdjustments);
}
void MainWindow::createSelectionControls()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("RectangleSelectionOptionsPage"));
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addStretch();
    auto* shapeControls=new QWidget(page);
    shapeControls->setObjectName(QStringLiteral("SelectModeControls"));
    auto* shapeRow=new QHBoxLayout(shapeControls);
    shapeRow->setContentsMargins(0,0,0,0); shapeRow->setSpacing(6);
    auto* shapes=new QButtonGroup(page);
    for(int i=0;i<2;++i) {
        auto* button=new ToolOptionsButton(i?QStringLiteral("SelectModeEllipse"):QStringLiteral("SelectModeRectangle"),
            i?QStringLiteral("Ellipse"):QStringLiteral("Rectangle"),
            i?QStringLiteral("Ellipse selection · Shift constrains a circle; see Properties for adding to an existing selection")
                :QStringLiteral("Rectangle selection · Shift constrains a square; see Properties for adding to an existing selection"),
            ToolOptionsButton::Kind::Toggle,shapeControls,ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(i?ToolGlyph::SelectEllipse:ToolGlyph::SelectRectangle));
        button->setChecked(i==0); shapes->addButton(button,i); shapeRow->addWidget(button);
    }
    connect(shapes,&QButtonGroup::idClicked,this,[this](int mode) {
        cancelPendingEdits(); ellipseSelectMode_=mode==1; refreshSelectionControls();
    });
    lassoModeControls_=new QWidget(page);
    auto* lassoRow=new QHBoxLayout(lassoModeControls_);
    lassoRow->setContentsMargins(0,0,0,0); lassoRow->setSpacing(6);
    auto* lassoModes=new QButtonGroup(page);
    const std::array lassoNames {"Freehand","Polygonal","Magnetic"};
    const std::array lassoHints {"Freehand lasso · drag to trace; release closes",
        "Polygonal lasso · click vertices; Enter/double-click closes; Backspace removes",
        "Magnetic lasso · follow image edges; click anchors; Ctrl+click forces a straight segment"};
    for (std::size_t i=0;i<3;++i) {
        auto* button=new ToolOptionsButton(QStringLiteral("LassoMode%1").arg(QString::fromLatin1(lassoNames[i])),
            QString::fromLatin1(lassoNames[i]),QString::fromLatin1(lassoHints[i]),ToolOptionsButton::Kind::Toggle,
            lassoModeControls_,ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(ToolGlyph(int(ToolGlyph::LassoFreehand)+int(i))));
        button->setChecked(i==0); lassoModes->addButton(button,int(i)); lassoRow->addWidget(button);
    }
    connect(lassoModes,&QButtonGroup::idClicked,this,[this](int mode) {
        cancelPendingEdits(); lassoMode_=core::LassoMode(mode); refreshSelectionControls();
    });
    auto* modes = selectionModes_ = new QButtonGroup(page);
    const std::array names { "Replace", "Add", "Subtract", "Intersect" };
    const std::array hints { "Replace selection", "Add to selection · Shift", "Subtract from selection · Alt",
        "Intersect selection · Shift+Alt" };
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto* button
            = new ToolOptionsButton(QStringLiteral("SelectionMode%1").arg(QString::fromLatin1(names[i])),
                QString::fromLatin1(names[i]), QString::fromLatin1(hints[i]), ToolOptionsButton::Kind::Toggle,
                page, ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(static_cast<ToolGlyph>(i)));
        modes->addButton(button, int(i));
        button->setChecked(i == 0);
        row->addWidget(button);
    }
    connect(modes, &QButtonGroup::idClicked, this, [this](int mode) {
        finishSelectionNumericInput();
        selectionOperation_ = core::SelectionOperation(mode);
        canvasWindow_->setSelectionOperation(selectionOperation_);
        refreshSelectionModeHighlight();
    });
    connect(qApp, &QApplication::focusChanged, this,
        [this] { refreshSelectionModeHighlight(); });
    magneticControls_=new QWidget(page);
    auto* magneticRow=new QHBoxLayout(magneticControls_);
    magneticRow->setContentsMargins(0,0,0,0); magneticRow->setSpacing(6);
    auto* radius=new ToolOptionsNumber(magneticControls_);
    radius->setObjectName(QStringLiteral("MagneticRadiusControl"));
    radius->setPrefix(QStringLiteral("Radius: ")); radius->setSuffix(QStringLiteral(" px"));
    radius->setDecimals(0); radius->setRange(2,32); radius->setValue(magneticRadius_); radius->setFixedWidth(120);
    radius->setToolTip(QStringLiteral("Maximum edge search distance from your trace, in document pixels"));
    connect(radius,&QDoubleSpinBox::valueChanged,this,[this](double value) {
        cancelPendingEdits(); magneticRadius_=value;
    });
    magneticRow->addWidget(radius);
    auto* references=new QButtonGroup(page);
    for (int i=0;i<2;++i) {
        auto* button=new ToolOptionsButton(i==0?QStringLiteral("MagneticSourceMergedVisible"):QStringLiteral("MagneticSourceActiveLayer"),
            i==0?QStringLiteral("Merged Visible"):QStringLiteral("Active Layer"),
            i==0?QStringLiteral("Follow edges in the visible document (pinned when construction begins)")
                :QStringLiteral("Follow the active raster layer, ignoring its opacity/visibility"),
            ToolOptionsButton::Kind::Toggle,magneticControls_,ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(i==0?ToolGlyph::MergedVisible:ToolGlyph::ActiveLayer));
        button->setChecked(i==0); references->addButton(button,i); magneticRow->addWidget(button);
    }
    connect(references,&QButtonGroup::idClicked,this,[this](int source) {
        cancelPendingEdits(); magneticSource_=core::ColorSampleSource(source);
    });
    row->addWidget(magneticControls_);
    colorSelectionControls_ = createColorSelectionControls(page);
    row->addWidget(colorSelectionControls_);
    smartControls_=createSmartSelectionControls(page);
    row->addWidget(smartControls_);
    for (std::size_t i = 0; i < 3; ++i) {
        auto* button
            = new ToolOptionsButton(QStringLiteral("SelectionCommand%1").arg(i), selectionActions_[i]->text(),
                selectionActions_[i]->shortcut().toString(), ToolOptionsButton::Kind::Action, page);
        button->setDefaultAction(selectionActions_[i]);
        row->addWidget(button);
    }
    selectionAdjustmentsButton_ = new ToolOptionsButton(QStringLiteral("SelectionTransform"),
        QStringLiteral("Selection adjustments"),
        QStringLiteral("Selection adjustments · Show/hide Grow / Shrink and Rotate by · {{LayerTransformAction}} opens transform handles"),
        ToolOptionsButton::Kind::Toggle, page, ToolOptionsButton::Presentation::Icon);
    selectionAdjustmentsButton_->setIcon(toolGlyph(ToolGlyph::SelectionAdjustments));
    row->addWidget(selectionAdjustmentsButton_);
    selectionAdjustmentControls_ = new QWidget(page);
    selectionAdjustmentControls_->setObjectName(QStringLiteral("SelectionAdjustmentControls"));
    auto* adjustments = new QHBoxLayout(selectionAdjustmentControls_);
    adjustments->setContentsMargins(0, 0, 0, 0);
    adjustments->setSpacing(8);
    selectionAdjustmentControls_->hide();
    row->addWidget(selectionAdjustmentControls_);
    connect(selectionAdjustmentsButton_, &QToolButton::toggled, this, [this](bool expanded) {
        if (!expanded) finishSelectionNumericInput();
        selectionAdjustmentControls_->setVisible(expanded);
    });
    auto* growthPair = new QWidget(selectionAdjustmentControls_);
    growthPair->setObjectName(QStringLiteral("SelectionGrowthPair"));
    auto* pair = new QHBoxLayout(growthPair);
    pair->setContentsMargins(0, 0, 0, 0);
    pair->setSpacing(0);
    for (std::size_t i = 0; i < 2; ++i) {
        auto* control = new ToolOptionsNumber(growthPair);
        selectionGrowth_[i] = control;
        control->setObjectName(
            i == 0 ? QStringLiteral("SelectionGrowHorizontal") : QStringLiteral("SelectionGrowVertical"));
        control->setPrefix(i == 0 ? QStringLiteral("X: ") : QStringLiteral("Y: "));
        control->setSuffix(QStringLiteral(" px"));
        control->setDecimals(0);
        control->setRange(-32768, 32768);
        control->setValue(1);
        control->setFixedWidth(108);
        control->setToolTip(i == 0
                ? QStringLiteral("Grow/shrink per left and right side, in document pixels")
                : QStringLiteral("Grow/shrink per top and bottom side, in document pixels"));
        control->setAccessibleName(control->toolTip());
        pair->addWidget(control);
    }
    adjustments->addWidget(growthPair);
    selectionGrowButton_
        = new ToolOptionsButton(QStringLiteral("SelectionGrowApply"), QStringLiteral("Grow / Shrink"),
            QStringLiteral("Apply signed X/Y amounts per side to the actual mask"),
            ToolOptionsButton::Kind::Action, selectionAdjustmentControls_);
    adjustments->addWidget(selectionGrowButton_);
    connect(selectionGrowButton_, &QToolButton::clicked, this, &MainWindow::growSelection);
    selectionAngle_ = new ToolOptionsNumber(selectionAdjustmentControls_);
    selectionAngle_->setObjectName(QStringLiteral("SelectionAngleControl"));
    selectionAngle_->setPrefix(QStringLiteral("Rotate by: "));
    selectionAngle_->setSuffix(QStringLiteral("°"));
    selectionAngle_->setRange(-36000, 36000);
    selectionAngle_->setDecimals(2);
    selectionAngle_->setFixedWidth(154);
    selectionAngle_->setToolTip(QStringLiteral(
        "Clockwise around selection bounds center. One action per hold/edit; resets after applying."));
    selectionAngle_->setAccessibleName(selectionAngle_->toolTip());
    selectionAngle_->onInteractionFinished = [this] {
        if (!updatingSelectionControls_)
            finishSelectionRotation(true);
    };
    selectionAngle_->onInteractionCancelled = [this] { finishSelectionRotation(false); };
    connect(selectionAngle_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        previewSelectionRotation(value);
        if (!selectionAngle_->interactionActive())
            finishSelectionRotation(true);
    });
    adjustments->addWidget(selectionAngle_);
    row->addStretch();
    toolOptionsBar_->registerToolPage(core::ToolId::Marquee, QStringLiteral("Select"), page);
    toolOptionsBar_->registerToolPage(core::ToolId::Lasso, QStringLiteral("Lasso"), page);
    toolOptionsBar_->registerToolPage(core::ToolId::SelectByColor, QStringLiteral("Select by Color"), page);
    toolOptionsBar_->registerToolPage(core::ToolId::SmartSelect, QStringLiteral("Smart Select"), page);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::SmartSelect,smartModeControls_);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::Marquee,shapeControls);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::Lasso, lassoModeControls_);
    selectionRasterTimer_ = new QTimer(this);
    selectionRasterTimer_->setObjectName(QStringLiteral("SelectionRasterizationTimer"));
    selectionRasterTimer_->setSingleShot(true);
    selectionRasterTimer_->setInterval(1);
    connect(selectionRasterTimer_, &QTimer::timeout, this, &MainWindow::advanceSelectionRasterization);
    magneticTimer_=new QTimer(this); magneticTimer_->setSingleShot(true);
    magneticTimer_->setObjectName(QStringLiteral("MagneticLassoTimer"));
    magneticTimer_->setInterval(0);
    connect(magneticTimer_,&QTimer::timeout,this,&MainWindow::advanceMagneticLasso);
    canvasWindow_->onSelectionBegan = [this](core::Vec2d point, Qt::KeyboardModifiers modifiers) {
        return beginSelectionGesture(point, modifiers);
    };
    canvasWindow_->onSelectionSample=[this](const core::NormalizedPointerSample& sample){smartPointerSample_=sample;};
    canvasWindow_->onSelectionMoved = [this](core::Vec2d point) { moveSelectionGesture(point); };
    canvasWindow_->onSelectionEnded = [this](bool cancel) { finishSelectionGesture(cancel); };
    canvasWindow_->onSelectionCloseRequested=[this](core::Vec2d point) { closeLasso(point); };
}
bool MainWindow::beginSelectionGesture(core::Vec2d point, Qt::KeyboardModifiers modifiers)
{
    if(session().activeTool()==core::ToolId::SmartSelect)return beginSmartSelection(point,modifiers);
    if (session().activeTool() == core::ToolId::SelectByColor) return beginColorSelection(point, modifiers);
    auto* document = session().document();
    if (anchoredLassoActive()) {
        anchorLasso(point,modifiers.testFlag(Qt::ControlModifier));
        return anchoredLassoActive();
    }
    if (!document || !core::isSelectionTool(session().activeTool()) || selectionGesture_)
        return false;
    auto operation = selectionOperation_;
    const bool shift = modifiers.testFlag(Qt::ShiftModifier), alt = modifiers.testFlag(Qt::AltModifier);
    if (shift && alt)
        operation = core::SelectionOperation::Intersect;
    else if (shift)
        operation = core::SelectionOperation::Add;
    else if (alt)
        operation = core::SelectionOperation::Subtract;
    const auto before = document->selection();
    const bool lasso = session().activeTool() == core::ToolId::Lasso;
    const bool moving = core::selectionMoveHit(before, point, selectionOperation_, shift, alt);
    selectionGesture_.emplace();
    selectionGesture_->before=before;
    selectionGesture_->start=point; selectionGesture_->operation=operation; selectionGesture_->moving=moving;
    selectionGesture_->lasso = lasso && !moving;
    selectionGesture_->ellipseMode = !lasso && !moving && ellipseSelectMode_;
    // Initial Shift is reserved for Add when a selection exists. Releasing
    // it arms the next press for a square/circle, without changing the latched op.
    selectionGesture_->circleArmed = !before || !shift;
    selectionGesture_->circleConstrained = shift && selectionGesture_->circleArmed;
    selectionGesture_->owner = document;
    if (moving) {
        const auto b = before->bounds();
        // Moving the mask changes no layer geometry: every visible layer may
        // be a target, including the active layer beneath this selection.
        selectionGesture_->snapping.begin(*document,
            {{double(b.x), double(b.y)}, {double(b.right()), double(b.bottom())}}, {});
    }
    selectionGesture_->mode=lassoMode_;
    if (selectionGesture_->lasso)
        selectionGesture_->pathEdges = std::make_shared<std::vector<core::SelectionEdge>>();
    // Force an initial preview even for a stationary press (active-empty on release).
    selectionGesture_->rectangle = { -1, -1, 0, 0 };
    if (selectionGesture_->lasso && lassoMode_!=core::LassoMode::Freehand) {
        try { beginAnchoredLasso(point); }
        catch (const std::exception& error) {
            finishSelectionGesture(true);
            statusBar()->showMessage(QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())),6000);
        }
        updateActionState(); return selectionGesture_.has_value();
    }
    moveSelectionGesture(point);
    updateActionState();
    return selectionGesture_.has_value();
}
void MainWindow::moveSelectionGesture(core::Vec2d point)
{
    if(session().activeTool()==core::ToolId::SmartSelect){moveSmartSelection(point);return;}
    if (session().activeTool() == core::ToolId::SelectByColor) return;
    if (!selectionGesture_ || !session().document() || selectionRasterizer_)
        return;
    if (session().document() != selectionGesture_->owner
        || session().document()->selection() != selectionGesture_->before) {
        finishSelectionGesture(true);
        return;
    }
    if (anchoredLassoActive()) { moveAnchoredLasso(point); return; }
    auto& gesture = *selectionGesture_;
    gesture.pointer=point;
    const auto extent = session().document()->canvas().extent;
    if (gesture.lasso) {
        try {
            if (!gesture.path.append(point)) return;
            const auto points = gesture.path.points();
            auto& edges = *gesture.pathEdges;
            if (points.size() > 1) {
                const bool closing = points.back() != points.front();
                edges.resize(points.size() - 1 + std::size_t(closing));
                edges[points.size() - 2] = {points[points.size() - 2], points.back()};
                if (closing) edges.back() = {points.back(), points.front()};
            }
            canvasWindow_->setSelectionPathPreview(gesture.pathEdges,gesture.operation);
        } catch (const std::exception& error) {
            finishSelectionGesture(true);
            statusBar()->showMessage(QStringLiteral("Lasso cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
        }
        return;
    }
    if (gesture.moving) {
        const auto delta = point - gesture.start;
        const core::Vec2d raw {std::clamp(delta.x, -double(extent.width), double(extent.width)),
            std::clamp(delta.y, -double(extent.height), double(extent.height))};
        core::Vec2d offset {std::round(raw.x), std::round(raw.y)};
        core::SnapGuides guides;
        if (gesture.snapMovementStarted || point != gesture.start) {
            gesture.snapMovementStarted = true;
            auto options = snappingOptions_;
            options.logicalScale = canvasWindow_->zoom();
            options.bypass |= canvasWindow_->pointerModifiers().testFlag(Qt::ControlModifier);
            const auto snap = gesture.snapping.resolve(*session().document(), raw, options);
            guides = snap.guides;
            // Free movement remains pixel-aligned, but actual alignments retain
            // fractional coordinates (e.g. centering an odd-sized selection).
            if (guides[0]) offset.x = snap.translation.x;
            if (guides[1]) offset.y = snap.translation.y;
        }
        canvasWindow_->setSnapGuides(guides);
        if (offset == gesture.offset && canvasWindow_->selectionDragging())
            return;
        gesture.offset = offset;
        auto edges = core::translatedSelectionPreviewEdges(gesture.before, offset.x, offset.y);
        canvasWindow_->setSelectionPreview(
            std::make_shared<const std::vector<core::SelectionEdge>>(std::move(edges)));
        statusBar()->showMessage(
            QStringLiteral("Move selection · X %1 px · Y %2 px").arg(offset.x).arg(offset.y));
        return;
    }
    if (gesture.ellipseMode) {
        try {
            const core::EllipseGeometry ellipse(gesture.start,point,gesture.circleConstrained);
            if(gesture.ellipse && *gesture.ellipse==ellipse) return;
            gesture.ellipse=ellipse;
            auto edges=std::make_shared<const std::vector<core::SelectionEdge>>(ellipse.previewEdges(extent));
            canvasWindow_->setSelectionPathPreview(std::move(edges),gesture.operation,0,0);
            const auto size=ellipse.maximum()-ellipse.minimum();
            canvasWindow_->setPointerSizeTooltip({size.x,size.y});
            statusBar()->showMessage(QStringLiteral("%1 selection · %2 × %3 px")
                .arg(gesture.circleConstrained?QStringLiteral("Circle"):QStringLiteral("Ellipse"))
                .arg(size.x,0,'f',1).arg(size.y,0,'f',1));
        } catch(const std::exception& error) {
            finishSelectionGesture(true);
            statusBar()->showMessage(QStringLiteral("Ellipse cancelled: %1").arg(QString::fromUtf8(error.what())),6000);
        }
        return;
    }
    auto rectangleStart = gesture.start;
    if (gesture.circleConstrained) {
        // Rectangle and ellipse share the same Shift rearming state. Rectangle
        // endpoints remain pixel-aligned: constrain the snapped endpoints before
        // canvas clipping so fractional press coordinates cannot yield a 1 px
        // difference between the square's sides, or distort off-canvas geometry.
        rectangleStart = {std::round(rectangleStart.x), std::round(rectangleStart.y)};
        const core::Vec2d delta {std::round(point.x) - rectangleStart.x,
            std::round(point.y) - rectangleStart.y};
        const auto side = std::max(std::abs(delta.x), std::abs(delta.y));
        point = {rectangleStart.x + std::copysign(side, delta.x),
            rectangleStart.y + std::copysign(side, delta.y)};
    }
    const auto rectangle = core::alignedSelectionRectangle(rectangleStart, point, extent);
    if (rectangle == gesture.rectangle)
        return;
    gesture.rectangle = rectangle;
    canvasWindow_->setPointerSizeTooltip({double(rectangle.width),double(rectangle.height)});
    // Construction previews keep the baseline separate. Do not erase holes or
    // replace the original ants with a provisional combination.
    auto edges=std::make_shared<std::vector<core::SelectionEdge>>();
    if (!rectangle.empty()) {
        const core::Vec2d a{double(rectangle.x),double(rectangle.y)}, b{double(rectangle.right()),double(rectangle.y)},
            c{double(rectangle.right()),double(rectangle.bottom())}, d{double(rectangle.x),double(rectangle.bottom())};
        *edges={{a,b},{b,c},{c,d},{d,a}};
    }
    canvasWindow_->setSelectionPathPreview(edges,gesture.operation,0,0);
    statusBar()->showMessage(
        QStringLiteral("Selection · %1 × %2 px").arg(rectangle.width).arg(rectangle.height));
}
void MainWindow::finishSelectionGesture(bool cancel)
{
    if(smartSelectionEditing_ || smartQueuedInput_){if(cancel)cancelSmartSelection(false);else finishSmartSelection();return;}
    if (colorSelectionEditing_) {
        if (cancel) cancelColorSelection(false); else finishColorSelection();
        return;
    }
    canvasWindow_->setPointerTooltip({});
    if (!selectionGesture_)
        return;
    if (!cancel && anchoredLassoActive()) return; // Button release only drops capture, not the multi-click path.
    if (!cancel && selectionRasterizer_) return;
    if (!cancel && (selectionGesture_->lasso || selectionGesture_->ellipseMode) && !selectionRasterizer_ && session().document()) {
        try {
            if (selectionGesture_->ellipseMode)
                selectionRasterizer_=std::make_unique<core::EllipseCoverageRasterizer>(
                    session().document()->canvas().extent,*selectionGesture_->ellipse);
            else selectionRasterizer_ = std::make_unique<core::PolygonCoverageRasterizer>(
                    session().document()->canvas().extent, selectionGesture_->path.points());
            statusBar()->showMessage(QStringLiteral("Creating selection… · Escape cancels"));
            selectionRasterTimer_->start();
            updateActionState();
            return;
        } catch (const std::exception& error) {
            finishSelectionGesture(true);
            statusBar()->showMessage(QStringLiteral("Selection cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
            return;
        }
    }
    selectionRasterTimer_->stop();
    magneticTimer_->stop();
    canvasWindow_->setSelectionConstructionActive(false);
    selectionRasterizer_.reset();
    const auto gesture = std::move(*selectionGesture_);
    selectionGesture_.reset();
    canvasWindow_->setSnapGuides({});
    if (!cancel && session().document() && session().document() == gesture.owner
        && session().document()->selection() == gesture.before) {
        try {
            core::AffineTransform translation;
            translation.m02 = gesture.offset.x;
            translation.m12 = gesture.offset.y;
            auto mask = gesture.moving
                ? gesture.before->transformed(translation)
                : core::combineSelection(gesture.before,
                      core::SelectionMask::rectangle(session().document()->canvas().extent, gesture.rectangle),
                      gesture.operation);
            session().execute(std::make_unique<core::SetSelectionCommand>(
                std::move(mask), gesture.moving ? "Move selection" : "Rectangle selection"));
        } catch (const std::exception& error) {
            QMessageBox::warning(this, QStringLiteral("Unable to select"), QString::fromUtf8(error.what()));
        }
    }
    canvasWindow_->setSelectionPreview({ });
    if (cancel)
        pointerRouter_->cancelCapture();
    statusBar()->clearMessage();
    synchronizeUi(false, false);
}
void MainWindow::advanceSelectionRasterization()
{
    if (!selectionGesture_ || !selectionRasterizer_) return;
    auto* document = session().document();
    if (document != selectionGesture_->owner || !document || document->selection() != selectionGesture_->before) {
        finishSelectionGesture(true);
        return;
    }
    try {
        // Analytic ellipses fill interior row spans in bulk. Their larger
        // pixel budget avoids hundreds of timer round-trips at 4K/5K while
        // keeping each chunk small; polygon edge/subrow budgets are unchanged.
        const std::size_t budget=selectionGesture_->ellipseMode ? 524288 : 65536;
        if (!selectionRasterizer_->step(budget)) { selectionRasterTimer_->start(); return; }
        // The incoming mask is constructed independently; combination and
        // command adoption happen exactly once after every subrow completes.
        const auto incoming = core::SelectionMask::fromR8Region(document->canvas().extent,
            selectionRasterizer_->region(), selectionRasterizer_->coverage(), selectionRasterizer_->stride());
        auto after = core::combineSelection(selectionGesture_->before, incoming, selectionGesture_->operation);
        const auto mode=selectionGesture_->mode;
        session().execute(std::make_unique<core::SetSelectionCommand>(std::move(after),
            selectionGesture_->ellipseMode?"Ellipse selection":mode==core::LassoMode::Polygonal?"Polygonal lasso selection"
                :mode==core::LassoMode::Magnetic?"Magnetic lasso selection":"Lasso selection"));
        selectionGesture_.reset();
        selectionRasterizer_.reset();
        canvasWindow_->setSelectionPreview({});
        statusBar()->clearMessage();
        synchronizeUi(false, false);
    } catch (const std::exception& error) {
        finishSelectionGesture(true);
        statusBar()->showMessage(QStringLiteral("Selection cancelled: %1").arg(QString::fromUtf8(error.what())), 6000);
    }
}
void MainWindow::runSelectionAction(int action)
{
    if (!session().document() || layerTransform_ || selectionTransform_)
        return;
    finishSelectionNumericInput();
    cancelPendingEdits();
    auto* document = session().document();
    try {
        if (action == 3) {
            if(session().editingLayerMask()){statusBar()->showMessage(tr("Click the content thumbnail before copying pixels to a new layer."),3500);return;}
            if (!session().activeLayer())
                return;
            if (session().execute(std::make_unique<core::LayerViaCopyCommand>(
                    *session().activeLayer(), session().activeLayer()))) {
                fileState().untouched = false;
                synchronizeUi(true, false);
            } else
                statusBar()->showMessage(QStringLiteral("Nothing selected to copy from this layer"), 2500);
            return;
        }
        core::SelectionState after;
        if (action == 4) {
            after = document->lastSelection();
            if (!after) return;
        }
        if (action == 0)
            after = core::SelectionMask::filled(document->canvas().extent, 255);
        if (action == 2)
            after = document->selection() ? document->selection()->inverted()
                                          : core::SelectionMask::filled(document->canvas().extent, 255);
        session().execute(std::make_unique<core::SetSelectionCommand>(
            std::move(after), selectionActions_[std::size_t(action)]->text().toStdString()));
        synchronizeUi(false, false);
    } catch (const std::exception& error) {
        QMessageBox::warning(
            this, QStringLiteral("Unable to complete selection action"), QString::fromUtf8(error.what()));
    }
}

void MainWindow::finishSelectionNumericInput()
{
    if (!selectionAngle_ || updatingSelectionControls_)
        return;
    selectionAngle_->interpretText();
    selectionAngle_->finishInteraction();
    selectionAngle_->clearFocus();
    finishSelectionRotation(true);
}
void MainWindow::refreshSelectionControls()
{
    refreshSelectionModeHighlight();
    refreshColorSelectionControls();
    refreshSmartSelectionControls();
    selectionGrowAction_->setEnabled(canOpenSelectionAdjustments());
    if (!selectionAngle_ || updatingSelectionControls_)
        return;
    magneticControls_->setVisible(session().activeTool()==core::ToolId::Lasso && lassoMode_==core::LassoMode::Magnetic);
    const auto* document = session().document();
    const bool hasCoverage = document && document->selection() && !document->selection()->bounds().empty();
    const bool enabled = hasCoverage
        && !selectionGesture_ && !colorSelectionEditing_ && !smartInteractionActive() && !layerTransform_ && !selectionTransform_;
    selectionAdjustmentsButton_->setEnabled(enabled);
    if (!hasCoverage) selectionAdjustmentsButton_->setChecked(false);
    selectionAngle_->setEnabled(enabled);
    selectionGrowButton_->setEnabled(enabled && !selectionRotation_);
    for (auto* field : selectionGrowth_)
        field->setEnabled(enabled && !selectionRotation_);
}
bool MainWindow::canOpenSelectionAdjustments() const
{
    const auto* document = session().document();
    return document && document->selection() && !document->selection()->bounds().empty()
        && !fileBusy_ && !workspaceDialog_ && !layerTransform_ && !selectionTransform_
        && shortcutGestureIdle(true);
}
void MainWindow::openSelectionAdjustments()
{
    // Recheck at invocation too: a gesture/job may have started since the last
    // UI refresh. Opening controls must never cancel or apply another operation.
    if (!canOpenSelectionAdjustments()) return;
    if (!core::isSelectionTool(session().activeTool())) setActiveTool(core::ToolId::Marquee);
    if (core::isSelectionTool(session().activeTool())) selectionAdjustmentsButton_->setChecked(true);
}
void MainWindow::updateSelectionUiModifiers(Qt::KeyboardModifiers modifiers)
{
    modifiers &= Qt::ShiftModifier | Qt::AltModifier;
    if (selectionUiModifiers_ == modifiers) return;
    selectionUiModifiers_ = modifiers;
    if (!editorTextInputActive() && !QApplication::activeModalWidget() && !QApplication::activePopupWidget())
        updateEllipseConstraint(modifiers);
    refreshSelectionModeHighlight();
}
void MainWindow::updateEllipseConstraint(Qt::KeyboardModifiers modifiers)
{
    if (!selectionGesture_ || selectionGesture_->lasso || selectionGesture_->moving || selectionRasterizer_) return;
    auto& gesture=*selectionGesture_;
    const bool shift=modifiers.testFlag(Qt::ShiftModifier);
    if(!shift) gesture.circleArmed=true;
    const bool constrained=shift && gesture.circleArmed;
    if(gesture.circleConstrained==constrained) return;
    gesture.circleConstrained=constrained;
    // Rebuild from the original press/current pointer, never a constrained
    // previous preview. Modifier-only changes update immediately while held.
    moveSelectionGesture(gesture.pointer);
}
void MainWindow::refreshSelectionModeHighlight()
{
    if (!selectionModes_) return;
    auto highlighted = selectionOperation_;
    // Presentation only: neither the chosen default nor an in-progress
    // gesture's latched operation is changed by these temporary checks.
    if (core::isSelectionTool(session().activeTool()) && !editorTextInputActive()
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget()) {
        const bool shift = selectionUiModifiers_.testFlag(Qt::ShiftModifier);
        const bool alt = selectionUiModifiers_.testFlag(Qt::AltModifier);
        if (shift && alt) highlighted = core::SelectionOperation::Intersect;
        else if (shift) highlighted = core::SelectionOperation::Add;
        else if (alt) highlighted = core::SelectionOperation::Subtract;
    }
    // QButtonGroup is exclusive. Checking the resolved mode unchecks the old
    // one without invoking idClicked or publishing an editing/history action.
    if (auto* button = selectionModes_->button(int(highlighted)); button && !button->isChecked())
        button->setChecked(true);
}
void MainWindow::growSelection()
{
    finishSelectionNumericInput();
    cancelPendingEdits();
    auto* document = session().document();
    if (!document || !document->selection() || document->selection()->bounds().empty())
        return;
    for (auto* control : selectionGrowth_) {
        control->interpretText();
        control->clearFocus();
    }
    try {
        auto mask = document->selection()->adjusted(
            int(selectionGrowth_[0]->value()), int(selectionGrowth_[1]->value()));
        session().execute(
            std::make_unique<core::SetSelectionCommand>(std::move(mask), "Grow/shrink selection"));
        synchronizeUi(false, false);
    } catch (const std::exception& error) {
        QMessageBox::warning(
            this, QStringLiteral("Unable to adjust selection"), QString::fromUtf8(error.what()));
    }
}
void MainWindow::previewSelectionRotation(double angle)
{
    if (updatingSelectionControls_ || selectionGesture_ || !core::isSelectionTool(session().activeTool()))
        return;
    const auto* document = session().document();
    if (!document || !document->selection() || document->selection()->bounds().empty())
        return;
    try {
        if (!selectionRotation_) {
            const auto before = document->selection();
            const auto b = before->bounds();
            selectionRotation_
                = SelectionRotation { before, before, { b.x + b.width * 0.5, b.y + b.height * 0.5 } };
        }
        auto& edit = *selectionRotation_;
        edit.preview = edit.before->rotated(angle, edit.pivot);
        canvasWindow_->setSelectionPreview(std::shared_ptr<const std::vector<core::SelectionEdge>>(
            edit.preview, &edit.preview->boundaryEdges()));
        updateActionState();
    } catch (const std::exception& error) {
        finishSelectionRotation(false);
        QMessageBox::warning(
            this, QStringLiteral("Unable to rotate selection"), QString::fromUtf8(error.what()));
    }
}
void MainWindow::finishSelectionRotation(bool commit)
{
    if (updatingSelectionControls_ || !selectionAngle_)
        return;
    auto edit = std::move(selectionRotation_);
    selectionRotation_.reset();
    // End numeric mode without interpreting cancelled/stale text again.
    {
        const QScopedValueRollback guard(updatingSelectionControls_, true);
        selectionAngle_->finishInteraction();
        {
            const QSignalBlocker block(selectionAngle_);
            selectionAngle_->setValue(0);
        }
        if (!commit)
            selectionAngle_->clearFocus();
    }
    if (!edit)
        return;
    if (commit && session().document() && session().document()->selection() == edit->before) {
        try {
            session().execute(std::make_unique<core::SetSelectionCommand>(edit->preview, "Rotate selection"));
        } catch (const std::exception& error) {
            QMessageBox::warning(
                this, QStringLiteral("Unable to rotate selection"), QString::fromUtf8(error.what()));
        }
    }
    canvasWindow_->setSelectionPreview({ });
    synchronizeUi(false, false);
}
}
