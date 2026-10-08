#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SmartSelectionEvidence.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/SampledColorLabel.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include <QButtonGroup>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <utility>

namespace imageeditor::ui {
QWidget* MainWindow::createSmartSelectionControls(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("SmartSelectionControls"));
    auto* row = new QHBoxLayout(page);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    smartModeControls_ = new QWidget(parent);
    auto* leading = new QHBoxLayout(smartModeControls_);
    leading->setContentsMargins(0, 0, 0, 0);
    leading->setSpacing(6);
    auto* modes = new QButtonGroup(page);
    for (int i = 0; i < 3; ++i) {
        auto* button
            = new ToolOptionsButton(i==2 ? QStringLiteral("SmartModeObject") : i ? QStringLiteral("SmartModeWand") : QStringLiteral("SmartModeQuick"),
                i==2 ? tr("Object Selection") : i ? QStringLiteral("Magic Wand") : QStringLiteral("Quick Selection"),
                i==2 ? tr("Object Selection · draw a rectangle; click to include, Alt-click to exclude. Runs locally and offline.") : i ? QStringLiteral("Magic Wand · click a contiguous matching color region")
                  : QStringLiteral("Quick Selection · brush foreground or exclusion evidence; grow along "
                                   "appearance and edges"),
                ToolOptionsButton::Kind::Toggle, smartModeControls_, ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(i==2 ? ToolGlyph::ObjectSelection : i ? ToolGlyph::MagicWand : ToolGlyph::QuickSelection));
        button->setChecked(i == 0);
        modes->addButton(button, i);
        leading->addWidget(button);
    }
    connect(modes, &QButtonGroup::idClicked, this, [this](int mode) {
        cancelSmartSelection();
        smartMode_ = core::SmartSelectMode(mode);
        if(mode==2 && !objectSelectionEngine_)
            objectSelectionEngine_=std::make_shared<ObjectSelectionEngine>();
        refreshSelectionControls();
        updateActionState();
    });
    auto* sources = new QButtonGroup(page);
    for (int i = 0; i < 2; ++i) {
        auto* button = new ToolOptionsButton(
            i ? QStringLiteral("SmartSourceActive") : QStringLiteral("SmartSourceMerged"),
            i ? QStringLiteral("Active Layer") : QStringLiteral("Merged Visible"),
            i ? QStringLiteral("Active Layer · visible adjusted/cropped layer appearance, including opacity, "
                               "against transparency")
              : QStringLiteral("Merged Visible · rendered document content including blending and hierarchy"),
            ToolOptionsButton::Kind::Toggle, page, ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(i ? ToolGlyph::ActiveLayer : ToolGlyph::MergedVisible));
        button->setChecked(i == 0);
        sources->addButton(button, i);
        row->addWidget(button);
    }
    connect(sources, &QButtonGroup::idClicked, this, [this](int source) {
        cancelSmartSelection();
        smartSource_ = core::ColorSampleSource(source);
        refreshSmartSelectionControls();
        updateActionState();
    });
    auto field
        = [&](const QString& name, const QString& label, double min, double max, double value, int width) {
              auto* control = new CompactValueControl(page);
              control->setObjectName(name);
              control->setAccessibleName(label);
              control->setPrefix(label + QStringLiteral(": "));
              control->setDecimals(0);
              control->setRange(min, max);
              control->setSingleStep(1);
              control->setValue(value);
              control->setFixedSize(width, 30);
              row->addWidget(control);
              return control;
          };
    smartSize_ = field(QStringLiteral("SmartSize"), QStringLiteral("Size"), 2, 512, 24, 132);
    smartSize_->setSuffix(QStringLiteral(" px"));
    smartSize_->setToolTip(QStringLiteral(
        "Brush diameter in document pixels. Growth stays near your path; larger brushes allow a wider local region."));
    connect(smartSize_, &QDoubleSpinBox::valueChanged, this, [this] { refreshSmartSelectionControls(); });
    smartEdgeSensitivity_
        = field(QStringLiteral("SmartEdgeSensitivity"), QStringLiteral("Edges"), 0, 100, 40, 132);
    smartEdgeSensitivity_->setSuffix(QStringLiteral("%"));
    smartEdgeSensitivity_->setToolTip(QStringLiteral(
        "Edge sensitivity · higher strengthens soft, monotone boundaries; lower prioritizes fine pixel "
        "contrast. Applies to the next Quick Selection stroke; does not change brush reach or Magic Wand "
        "fuzziness."));
    smartTolerance_ = field(QStringLiteral("SmartTolerance"), QStringLiteral("Fuzziness"), 0, 255, 16, 152);
    smartTolerance_->setToolTip(
        QStringLiteral("Premultiplied RGB/alpha distance · 0 closest, 255 all eligible connected colors"));
    connect(smartTolerance_, &QDoubleSpinBox::valueChanged, this, &MainWindow::changeSmartTolerance);
    smartTolerance_->onInteractionFinished = [this] {
        if (!updatingSmart_)
            finishSmartSelection();
    };
    smartSample_ = new SampledColorLabel(page);
    smartSample_->setObjectName(QStringLiteral("SmartSample"));
    smartSample_->setFixedSize(174, 30);
    smartSample_->setToolTip(QStringLiteral("Sampled reference; does not change painting colors"));
    row->addWidget(smartSample_);
    smartTimer_ = new QTimer(this);
    smartTimer_->setObjectName(QStringLiteral("SmartSelectionTimer"));
    smartTimer_->setSingleShot(true);
    connect(smartTimer_, &QTimer::timeout, this, &MainWindow::advanceSmartSelection);
    page->hide();
    return page;
}
void MainWindow::createSmartSelectionHelp()
{
    QVBoxLayout* content { };
    propertiesPanel_->addToolPage(core::ToolId::SmartSelect, QStringLiteral("Smart Select"),
        QStringLiteral("{{ToolAction_smartselect}} · Quick Selection, Magic Wand and Object Selection create ordinary selections."), content);
    auto* help = new QLabel(QStringLiteral(
        "Quick Selection · Brush along the region to grow nearby edges. Continue brushing to extend it; "
        "Add grows, Subtract supplies correction hints. "
        "New strokes override conflicting hints. Edges: higher favors soft boundaries, lower favors fine contrast. "
        "Weak edges may need correction strokes.\n"
        "Magic Wand · Click a connected color region; Fuzziness refines that click.\n\n"
        "Object Selection · Draw a rough rectangle around an object. Click to include, Alt-click to exclude; "
        "drag a new rectangle to start another object. Runs locally and offline using the bundled model. "
        "Use Refine Selection for soft edges; the model does not recover transparency.\n\n"
        "{{DecreaseBrushSizeAction}} / {{IncreaseBrushSizeAction}} · Quick Selection brush size\n"
        "Shift · Add; Alt · Subtract; Shift+Alt · Intersect (latched at press)\n"
        "Escape · Cancel pending work\n{{SelectAllAction}} / {{DeselectAction}} · Select all / deselect\n"
        "{{InvertSelectionAction}} · Invert; {{LayerTransformAction}} · Transform selection\n{{LayerViaCopyAction}} · Copy selected raster content\n"
        "{{UndoAction}} / {{RedoAction}} · Undo / redo, including correction hints\n\n"
        "Merged Visible samples the visible document. Active Layer samples its adjusted/cropped appearance without other layers. "
        "Click within its valid extent. Changed source content resets hints.\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan; wheel · Zoom"));
    help->setWordWrap(true);
    help->setObjectName(QStringLiteral("MutedLabel"));
    content->addWidget(help);
}
bool MainWindow::beginSmartSelection(core::Vec2d point, Qt::KeyboardModifiers modifiers)
{
    if (fileBusy_ || !session().document() || layerTransform_ || selectionTransform_)
        return false;
    refreshSmartSelectionControls();
    if(smartMode_==core::SmartSelectMode::ObjectSelection&&!objectSelectionEngine_)
        objectSelectionEngine_=std::make_shared<ObjectSelectionEngine>();
    if (smartSelectionEditing_ && smartFinishRequested_) {
        if(smartMode_==core::SmartSelectMode::ObjectSelection) {
            statusBar()->showMessage(tr("Finishing Object Selection — Escape cancels"),2500);return false;
        }
        if (smartQueuedStrokes_.size() >= 8) {
            statusBar()->showMessage(
                QStringLiteral("Finishing queued selections — please wait before the next stroke"), 2500);
            return false;
        }
        try {
            smartQueuedInput_.emplace();
            auto& queued = *smartQueuedInput_;
            queued.operation = selectionOperation_;
            const bool shift = modifiers.testFlag(Qt::ShiftModifier),
                       alt = modifiers.testFlag(Qt::AltModifier);
            if (shift && alt)
                queued.operation = core::SelectionOperation::Intersect;
            else if (shift)
                queued.operation = core::SelectionOperation::Add;
            else if (alt)
                queued.operation = core::SelectionOperation::Subtract;
            queued.seed = point;
            queued.edgeSensitivity = smartEdgeSensitivity_->value() / 100.0;
            queued.tolerance = int(smartTolerance_->value());
            auto sample = smartPointerSample_;
            sample.documentPosition = point;
            if (smartMode_ == core::SmartSelectMode::QuickSelection)
                queued.path.begin(smartSize_->value(), sample);
            return true;
        } catch (const std::exception& error) {
            smartQueuedInput_.reset();
            statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
            return false;
        }
    }
    cancelSmartSelection(false);
    auto* doc = session().document();
    try {
        if (doc->layers().size() > 1024)
            throw std::length_error("Smart Select supports up to 1024 source layers");
        if (!smartSelection_) {
            smartSelection_.emplace();
            auto& state = *smartSelection_;
            state.owner = doc;
            state.documentId = activeDocumentId();
            state.layer = session().activeLayer();
            state.revision = doc->revision();
            state.expectedSelectionRevision = doc->selectionRevision();
            if (const auto evidence
                = std::dynamic_pointer_cast<const core::SmartSelectionEvidence>(doc->selectionEvidence());
                evidence && evidence->matches(*doc, state.layer, smartSource_, state.documentId))
                state.hints = evidence->hints();
            for (const auto& layer : doc->layers())
                if (const auto* raster = std::get_if<core::RasterLayer>(&layer.payload))
                    if (raster->surface)
                        state.pinnedPixels.emplace_back(raster->surface, raster->surface->revision());
        }
        auto& state = *smartSelection_;
        state.original = doc->selection();
        state.startingHints = state.hints;
        state.operation = selectionOperation_;
        const bool shift = modifiers.testFlag(Qt::ShiftModifier), alt = modifiers.testFlag(Qt::AltModifier);
        if (shift && alt)
            state.operation = core::SelectionOperation::Intersect;
        else if (shift)
            state.operation = core::SelectionOperation::Add;
        else if (alt)
            state.operation = core::SelectionOperation::Subtract;
        state.seed = point;
        if(smartMode_==core::SmartSelectMode::ObjectSelection) {
            state.objectEnd=point;state.objectCorrection=false;state.objectPrompt={};state.objectOriginal={};
            if(const auto evidence=std::dynamic_pointer_cast<const ObjectSelectionEvidence>(doc->selectionEvidence());
                evidence&&evidence->identity.matches(*doc,state.layer,smartSource_,state.documentId)) {
                state.objectPrompt=evidence->prompt;state.objectOriginal=evidence->original;state.objectOperation=evidence->operation;
            }
        }
        state.edgeSensitivity = smartEdgeSensitivity_->value() / 100.0;
        state.hasClick = true;
        state.appliedTolerance = int(smartTolerance_->value());
        auto sample = smartPointerSample_;
        sample.documentPosition = point;
        if (smartMode_ == core::SmartSelectMode::QuickSelection)
            state.path.begin(smartSize_->value(), sample);
        smartSelectionEditing_ = true;
        smartFinishRequested_ = false;
        ++smartVersion_;
        // This argument controls preview visibility only. Actual combination is
        // latched above. Even Replace retains committed ants alongside evidence.
        canvasWindow_->setSelectionPathPreview(
            std::make_shared<const std::vector<core::SelectionEdge>>(), core::SelectionOperation::Add, 0, 0);
        smartTimer_->start(0);
        updateActionState();
        statusBar()->showMessage(
            QStringLiteral("Smart Select · preparing reference / refining edges… · Escape cancels"));
        return true;
    } catch (const std::exception& error) {
        cancelSmartSelection();
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
        return false;
    }
}
void MainWindow::moveSmartSelection(core::Vec2d point)
{
    if(smartMode_==core::SmartSelectMode::ObjectSelection&&smartSelectionEditing_&&smartSelection_&&!smartFinishRequested_) {
        auto& state=*smartSelection_;state.objectEnd=point;
        const core::Vec2d a=state.seed,b=point;
        auto edges=std::make_shared<std::vector<core::SelectionEdge>>(std::initializer_list<core::SelectionEdge>{
            {a,{b.x,a.y}},{{b.x,a.y},b},{b,{a.x,b.y}},{{a.x,b.y},a}});
        canvasWindow_->setSelectionPathPreview(edges,state.operation,0,0);return;
    }
    if (smartQueuedInput_) {
        auto sample = smartPointerSample_;
        sample.documentPosition = point;
        try {
            if (smartMode_ == core::SmartSelectMode::QuickSelection)
                smartQueuedInput_->path.append(sample);
        } catch (const std::exception& error) {
            smartQueuedInput_.reset();
            statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
        }
        return;
    }
    if (!smartSelectionEditing_ || !smartSelection_ || smartFinishRequested_
        || smartMode_ != core::SmartSelectMode::QuickSelection)
        return;
    try {
        auto sample = smartPointerSample_;
        sample.documentPosition = point;
        const auto before = smartSelection_->path.dabs().size();
        smartSelection_->path.append(sample);
        if (before != smartSelection_->path.dabs().size()) {
            ++smartVersion_;
            // Lightweight evidence centerline while an immutable solve is in
            // flight; no mask rasterization/composition in the pointer callback.
            auto edges = std::make_shared<std::vector<core::SelectionEdge>>();
            const auto& dabs = smartSelection_->path.dabs();
            edges->reserve(dabs.size());
            for (std::size_t i = 1; i < dabs.size(); ++i)
                edges->push_back({ dabs[i - 1].center, dabs[i].center });
            if (!smartPreview_.combined)
                canvasWindow_->setSelectionPathPreview(edges, smartSelection_->operation, 0, 0);
            if (!smartTimer_->isActive())
                smartTimer_->start(16);
        }
    } catch (const std::exception& error) {
        cancelSmartSelection(false);
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}
void MainWindow::finishSmartSelection()
{
    if (smartQueuedInput_) {
        try {
            if (smartMode_ == core::SmartSelectMode::QuickSelection)
                smartQueuedInput_->path.end(smartPointerSample_);
            smartQueuedStrokes_.push_back(std::move(*smartQueuedInput_));
            smartQueuedInput_.reset();
            startNextSmartStroke();
        } catch (const std::exception& error) {
            smartQueuedInput_.reset();
            statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
        }
        return;
    }
    if (!smartSelectionEditing_ || !smartSelection_ || updatingSmart_ || smartFinishRequested_)
        return;
    try {
        if(smartMode_==core::SmartSelectMode::ObjectSelection) {
            auto& state=*smartSelection_;
            const double dx=state.objectEnd.x-state.seed.x,dy=state.objectEnd.y-state.seed.y;
            state.objectCorrection=std::hypot(dx,dy)<3;
            if(state.objectCorrection) {
                if(state.objectPrompt.box.width<=1||state.objectPrompt.box.height<=1) {
                    cancelSmartSelection(false);statusBar()->showMessage(tr("Draw a rectangle around the intended object first"),4000);return;
                }
                std::erase_if(state.objectPrompt.corrections,[&](const auto& p){return std::hypot(p.position.x-state.seed.x,p.position.y-state.seed.y)<1;});
                state.objectPrompt.corrections.push_back({state.seed,state.operation!=core::SelectionOperation::Subtract});
                state.original=state.objectOriginal;state.operation=state.objectOperation;
            } else {
                state.objectPrompt={core::RectD{std::min(state.seed.x,state.objectEnd.x),std::min(state.seed.y,state.objectEnd.y),std::abs(dx),std::abs(dy)}, {}};
                state.objectOriginal=state.original;state.objectOperation=state.operation;
            }
            ++smartVersion_;
        }
        if (smartMode_ == core::SmartSelectMode::QuickSelection) {
            const auto before = smartSelection_->path.dabs().size();
            smartSelection_->path.end(smartPointerSample_);
            if (before != smartSelection_->path.dabs().size())
                ++smartVersion_;
        }
        smartFinishRequested_ = true;
        smartTimer_->start(0);
    } catch (const std::exception& error) {
        cancelSmartSelection(false);
        statusBar()->showMessage(QString::fromUtf8(error.what()), 6000);
    }
}
void MainWindow::changeSmartTolerance()
{
    if (updatingSmart_ || smartMode_ != core::SmartSelectMode::MagicWand)
        return;
    refreshSmartSelectionControls();
    if (!smartSelection_ || !smartSelection_->hasClick || smartQueuedInput_ || !smartQueuedStrokes_.empty())
        return;
    smartSelectionEditing_ = true;
    smartFinishRequested_ = !smartTolerance_->interactionActive();
    ++smartVersion_;
    smartTimer_->start(0);
    updateActionState();
}
void MainWindow::advanceSmartSelection()
{
    refreshSmartSelectionControls();
    try {
        if (smartWorker_.valid()) {
            if (smartWorker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                smartTimer_->start(8);
                return;
            }
            core::SmartSelectionResult result;
            try {
                result = smartWorker_.get();
            } catch (...) {
                if (!*smartCancelled_)
                    throw;
            }
            if (smartSelectionEditing_ && smartSelection_ && !*smartCancelled_
                && smartWorkerGeneration_ == smartGeneration_
                && smartWorkerVersion_ <= smartVersion_
                && (!smartFinishRequested_ || smartWorkerVersion_ == smartVersion_)) {
                smartPreview_ = std::move(result);
                smartPreviewVersion_ = smartWorkerVersion_;
                if (smartPreview_.combined)
                    canvasWindow_->setSelectionMaskPreview(
                        smartPreview_.combined, {}, core::SelectionOperation::Replace);
            }
            smartCancelled_.reset();
        }
        if (!smartSelectionEditing_ || !smartSelection_)
            return;
        auto& state = *smartSelection_;
        if(smartMode_==core::SmartSelectMode::ObjectSelection&&!smartFinishRequested_)return;
        if (!state.reference) {
            QElapsedTimer slice;
            slice.start();
            const auto& layers = session().document()->layers();
            while (state.nextLayer < layers.size()) {
                const auto& layer = layers[state.nextLayer++];
                const bool included
                    = (smartSource_ == core::ColorSampleSource::MergedVisible || layer.id == state.layer)
                    && session().document()->isEffectivelyVisible(layer.id) && layer.opacity > 0;
                if (included && (std::holds_alternative<core::TextLayer>(layer.payload)
                    || std::holds_alternative<core::ShapeLayer>(layer.payload))) {
                    constexpr std::size_t budget = 64 * 1024 * 1024;
                    auto cache = prepareDocumentSampleCache(layer, budget - state.preparedPixels);
                    if (!cache || !cache->surface)
                        throw std::runtime_error("Could not prepare the Smart Select reference layer");
                    const auto e = cache->surface->extent();
                    state.preparedPixels += std::size_t(e.width) * e.height;
                    if (state.preparedPixels > budget)
                        throw std::length_error("Smart Select reference caches exceed 256 MiB");
                    state.prepared.push_back({ layer.id, std::move(cache) });
                }
                if (slice.elapsed() >= 4 && state.nextLayer < layers.size()) {
                    smartTimer_->start(0);
                    return;
                }
            }
            state.reference = std::make_unique<core::SmartSelectionReference>(
                *session().document(), state.layer, smartSource_, state.prepared);
            state.prepared.clear();
            state.pinnedPixels.clear();
        }
        if (!state.reference->image()) {
            QElapsedTimer slice;
            slice.start();
            do {
                if (state.reference->step(std::max(std::size_t(1),
                        std::size_t(2048) / std::max(std::size_t(1), state.reference->sourcesPerPixel()))))
                    break;
            } while (slice.elapsed() < 4);
            if (!state.reference->image()) {
                smartTimer_->start(0);
                return;
            }
        }
        if (smartPreviewVersion_ != smartVersion_) {
            const auto image = state.reference->image();
            const auto original = state.original;
            const auto hints = state.startingHints;
            const auto dabs = state.path.dabs();
            const auto seed = state.seed;
            const auto operation = state.operation;
            const auto mode = smartMode_;
            const auto edgeSensitivity = state.edgeSensitivity;
            const auto tolerance = int(smartTolerance_->value());
            const auto objectPrompt=state.objectPrompt;const auto objectEngine=objectSelectionEngine_;
            smartCancelled_ = std::make_shared<std::atomic_bool>(false);
            const auto cancelled = smartCancelled_;
            smartWorkerGeneration_ = smartGeneration_;
            smartWorkerVersion_ = smartVersion_;
            smartWorker_ = std::async(std::launch::async,
                [image, original, hints, dabs, seed, operation, mode, edgeSensitivity, tolerance, cancelled,objectPrompt,objectEngine] {
                    if(mode==core::SmartSelectMode::ObjectSelection) {
                        core::SmartSelectionResult result;
                        result.incoming=objectEngine->evaluate(image,objectPrompt,*cancelled);
                        if(result.incoming) {
                            result.combined=core::combineSelection(original,result.incoming,operation);
                            if(!core::prepareSelectionBoundary(result.combined,*cancelled))return core::SmartSelectionResult{};
                        }
                        return result;
                    }
                    return mode == core::SmartSelectMode::MagicWand
                        ? core::buildMagicWand(*image, seed, tolerance, original, operation, *cancelled)
                        : core::buildQuickSelection(*image, dabs, hints, original, operation, *cancelled,
                              core::QuickSelectionSettings { .edgeSensitivity = edgeSensitivity });
                });
            smartTimer_->start(8);
            return;
        }
        if (!smartFinishRequested_)
            return;
        const bool quick = smartMode_ == core::SmartSelectMode::QuickSelection;
        core::SelectionEvidenceState evidence = quick && smartPreview_.combined
            ? std::make_shared<const core::SmartSelectionEvidence>(
                  *session().document(), state.layer, smartSource_, smartPreview_.hints,state.documentId)
            : core::SelectionEvidenceState { };
        if(smartMode_==core::SmartSelectMode::ObjectSelection&&smartPreview_.combined)
            evidence=std::make_shared<const ObjectSelectionEvidence>(*session().document(),state.layer,smartSource_,state.objectPrompt,state.objectOriginal,state.objectOperation,state.documentId);
        const bool changed = smartPreview_.combined
            && session().execute(std::make_unique<core::SetSelectionCommand>(
                smartPreview_.combined, quick ? "Quick selection" : smartMode_==core::SmartSelectMode::ObjectSelection ? "Object selection" : "Magic wand", evidence));
        // Deliberate corrections can change evidence without changing visible
        // R8 coverage. Both are one atomic, nonpersistent history action; truly
        // equivalent masks AND hints are discarded without clearing redo.
        if (changed && quick)
            state.hints = smartPreview_.hints;
        state.expectedSelectionRevision = session().document()->selectionRevision();
        state.appliedTolerance = int(smartTolerance_->value());
        smartSelectionEditing_ = false;
        const bool limited=quick&&smartPreview_.stats.limitedGrowth;
        smartPreview_ = { };
        smartPreviewVersion_ = 0;
        canvasWindow_->setSelectionPreview({ });
        synchronizeUi(false, false);
        statusBar()->showMessage(
            limited ? tr("Only the brushed area could be resolved. Add evidence across the region or supply an exclusion stroke.")
                    : changed ? QStringLiteral("Smart selection applied") : QStringLiteral("Selection unchanged"),
            limited?6000:2500);
        startNextSmartStroke();
    } catch (const std::exception& error) {
        const auto message = QString::fromUtf8(error.what());
        cancelSmartSelection(false);
        statusBar()->showMessage(QStringLiteral("Smart Select: %1").arg(message), 7000);
        updateActionState();
    }
}
void MainWindow::startNextSmartStroke()
{
    if (smartSelectionEditing_ || !smartSelection_ || (smartQueuedStrokes_.empty() && !smartQueuedInput_))
        return;
    // Promote a still-held stroke as soon as preceding work finishes, so it can
    // receive live refinement instead of waiting for its eventual release.
    const bool completed = !smartQueuedStrokes_.empty();
    auto queued = completed ? std::move(smartQueuedStrokes_.front()) : std::move(*smartQueuedInput_);
    if (completed)
        smartQueuedStrokes_.pop_front();
    else
        smartQueuedInput_.reset();
    auto& state = *smartSelection_;
    state.original = session().document()->selection();
    state.startingHints = state.hints;
    state.operation = queued.operation;
    state.seed = queued.seed;
    state.edgeSensitivity = queued.edgeSensitivity;
    state.path = std::move(queued.path);
    state.hasClick = true;
    state.appliedTolerance = queued.tolerance;
    {
        const QSignalBlocker blocker(smartTolerance_);
        smartTolerance_->setValue(queued.tolerance);
    }
    smartSelectionEditing_ = true;
    smartFinishRequested_ = completed;
    smartPreviewVersion_ = 0;
    ++smartVersion_;
    ++smartGeneration_;
    smartTimer_->start(0);
    updateActionState();
}
void MainWindow::cancelSmartSelection(bool forget)
{
    if (!smartTimer_)
        return;
    const QScopedValueRollback guard(updatingSmart_, true);
    if (smartCancelled_)
        *smartCancelled_ = true;
    ++smartGeneration_;
    const bool editing = std::exchange(smartSelectionEditing_, false) || smartQueuedInput_.has_value();
    smartQueuedInput_.reset();
    smartQueuedStrokes_.clear();
    smartFinishRequested_ = false;
    smartPreview_ = { };
    smartPreviewVersion_ = 0;
    if (editing) {
        if (smartSelection_) {
            const QSignalBlocker blocker(smartTolerance_);
            smartTolerance_->setValue(smartSelection_->appliedTolerance);
        }
        canvasWindow_->setSelectionPreview({ });
        canvasWindow_->cancelSelectionInput();
        pointerRouter_->cancelCapture();
        smartTolerance_->clearFocus();
    }
    if (forget)
        smartSelection_.reset();
    if (smartWorker_.valid())
        smartTimer_->start(8);
    else
        smartTimer_->stop();
}
void MainWindow::refreshSmartSelectionControls()
{
    if (!smartControls_ || updatingSmart_)
        return;
    const auto* doc = session().document();
    if (smartSelection_) {
        auto& state = *smartSelection_;
        if (!doc || activeDocumentId()!=state.documentId || doc != state.owner || doc->revision() != state.revision
            || (state.reference && !state.reference->matches(*doc))
            || std::ranges::any_of(
                state.pinnedPixels, [](const auto& p) { return p.first->revision() != p.second; })
            || session().activeLayer() != state.layer)
            cancelSmartSelection();
        else if (doc->selectionRevision() != state.expectedSelectionRevision) {
            cancelSmartSelection(false);
            state.hints = { };
            if (const auto evidence
                = std::dynamic_pointer_cast<const core::SmartSelectionEvidence>(doc->selectionEvidence());
                evidence && evidence->matches(*doc, state.layer, smartSource_,state.documentId))
                state.hints = evidence->hints();
            state.startingHints = { };
            state.hasClick = false;
            state.original = { };
            state.expectedSelectionRevision = doc->selectionRevision();
        }
    }
    const bool quick = smartMode_ == core::SmartSelectMode::QuickSelection;
    smartControls_->setVisible(session().activeTool() == core::ToolId::SmartSelect);
    smartSize_->setVisible(quick);
    smartEdgeSensitivity_->setVisible(quick);
    smartTolerance_->setVisible(smartMode_==core::SmartSelectMode::MagicWand);
    smartSample_->setVisible(smartMode_==core::SmartSelectMode::MagicWand);
    smartSize_->setEnabled(!smartSelectionEditing_);
    smartEdgeSensitivity_->setEnabled(!smartSelectionEditing_);
    std::optional<core::Rgba8> sampled;
    if (!quick && smartSelection_ && smartSelection_->reference && smartSelection_->hasClick) {
        const auto image = smartSelection_->reference->image();
        const auto p = smartSelection_->seed;
        if (image && p.x >= 0 && p.y >= 0 && p.x < image->extent.width && p.y < image->extent.height) {
            const auto c = image->pixels[std::size_t(p.y) * image->extent.width + std::size_t(p.x)];
            sampled = c;
        }
    }
    static_cast<SampledColorLabel*>(smartSample_)->setSample(sampled);
    canvasWindow_->setQuickSelectionCursor(quick, smartSize_->value());
}
}
