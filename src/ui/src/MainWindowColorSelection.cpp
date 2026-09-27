#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/SampledColorLabel.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include <QButtonGroup>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace imageeditor::ui {
QWidget* MainWindow::createColorSelectionControls(QWidget* parent)
{
    auto* page = new QWidget(parent); page->setObjectName(QStringLiteral("ColorSelectionControls"));
    auto* row = new QHBoxLayout(page); row->setContentsMargins(0,0,0,0); row->setSpacing(8);
    auto* sources = new QButtonGroup(page);
    for (int i=0;i<2;++i) {
        auto* button = new ToolOptionsButton(i ? QStringLiteral("ColorSelectionActiveLayer") : QStringLiteral("ColorSelectionMergedVisible"),
            i ? QStringLiteral("Active Layer") : QStringLiteral("Merged Visible"),
            i ? QStringLiteral("Match the active raster, text or shape image, ignoring layer visibility/opacity")
                : QStringLiteral("Match the visible composited document, including hierarchy and layer opacity"),
            ToolOptionsButton::Kind::Toggle,page,ToolOptionsButton::Presentation::Icon);
        button->setIcon(toolGlyph(i ? ToolGlyph::ActiveLayer : ToolGlyph::MergedVisible));
        button->setChecked(i==0); sources->addButton(button,i); row->addWidget(button);
    }
    connect(sources,&QButtonGroup::idClicked,this,[this](int source) {
        cancelColorSelection(); colorSelectionSource_ = core::ColorSampleSource(source);
        refreshColorSelectionControls(); updateActionState();
    });
    colorSelectionFuzziness_ = new CompactValueControl(page);
    auto* field = colorSelectionFuzziness_;
    field->setObjectName(QStringLiteral("ColorSelectionFuzziness"));
    field->setAccessibleName(QStringLiteral("Fuzziness"));
    field->setPrefix(QStringLiteral("Fuzziness: ")); field->setDecimals(0); field->setRange(0,255);
    field->setSingleStep(1); field->setValue(16); field->setFixedSize(160,30);
    field->setToolTip(QStringLiteral("Color/alpha distance · 0 closest match, 255 all eligible colors. Transparent/nontransparent pixels never cross-match."));
    connect(field,&QDoubleSpinBox::valueChanged,this,[this] { changeColorSelectionFuzziness(); });
    field->onInteractionFinished = [this] { if (!updatingColorSelection_) finishColorSelection(); };
    row->addWidget(field);
    auto* sample = new SampledColorLabel(page); colorSelectionSample_ = sample;
    sample->setObjectName(QStringLiteral("ColorSelectionSample")); sample->setFixedSize(174,30);
    sample->setToolTip(QStringLiteral("Sampled reference only — does not change either painting color"));
    sample->setSample({}); row->addWidget(sample);
    colorSelectionTimer_ = new QTimer(this);
    colorSelectionTimer_->setObjectName(QStringLiteral("ColorSelectionTimer"));
    colorSelectionTimer_->setSingleShot(true);
    connect(colorSelectionTimer_,&QTimer::timeout,this,&MainWindow::advanceColorSelection);
    page->hide(); return page;
}
void MainWindow::createColorSelectionHelp()
{
    QVBoxLayout* content {};
    propertiesPanel_->addToolPage(core::ToolId::SelectByColor,QStringLiteral("Select by Color"),
        QStringLiteral("Click a color to select matching document pixels everywhere, including disconnected areas."),content);
    auto* help = new QLabel(QStringLiteral(
        "{{ToolAction_colorselect}} · Select by Color; click · Sample (also inside selections)\n"
        "Shift · Add; Alt · Subtract; Shift+Alt · Intersect (latched on click)\n"
        "Escape · Cancel pending work\n{{SelectAllAction}} / {{DeselectAction}} · Select all / deselect\n"
        "{{InvertSelectionAction}} · Invert; {{LayerTransformAction}} · Transform selection\n{{LayerViaCopyAction}} · Copy selected raster content to a layer\n"
        "{{UndoAction}} / {{RedoAction}} · Undo / redo\n\n"
        "Merged Visible uses the visible document. Active Layer ignores its opacity and visibility; sample within its valid extent. "
        "Transparent samples match transparent pixels only. Higher Fuzziness includes less-similar colors, refining the last sample. "
        "After source or selection changes, click again.\n\n"
        "{{PanCanvasAction}} + left-drag / middle-drag · Pan; wheel · Zoom"));
    help->setWordWrap(true); help->setObjectName(QStringLiteral("MutedLabel")); content->addWidget(help);
}
bool MainWindow::beginColorSelection(core::Vec2d seed, Qt::KeyboardModifiers modifiers)
{
    if (fileBusy_ || !session().document() || layerTransform_ || selectionTransform_) return false;
    cancelColorSelection();
    auto* doc = session().document();
    auto operation = selectionOperation_;
    const bool shift = modifiers.testFlag(Qt::ShiftModifier), alt = modifiers.testFlag(Qt::AltModifier);
    if (shift && alt) operation = core::SelectionOperation::Intersect;
    else if (shift) operation = core::SelectionOperation::Add;
    else if (alt) operation = core::SelectionOperation::Subtract;
    try {
        const auto extent = doc->canvas().extent;
        if (std::uint64_t(extent.width)*extent.height > core::ColorSelectionReference::maximumPixels
            || doc->layers().size() > 1024)
            throw std::length_error("Select by Color supports up to 64 megapixels and 1024 layers");
        colorSelection_.emplace(ColorSelectionContext {doc,session().activeLayer(),{},
            doc->selection(),doc->selection(),operation,int(colorSelectionFuzziness_->value()),seed,doc->revision(),0,0,{},{}});
        for (const auto& layer : doc->layers()) if (const auto* raster = std::get_if<core::RasterLayer>(&layer.payload))
            if (raster->surface) colorSelection_->pinnedPixels.emplace_back(raster->surface,raster->surface->revision());
        colorSelectionEditing_ = true; colorSelectionFinishRequested_ = false;
        colorSelectionPreview_ = {}; colorSelectionPreviewFuzziness_ = -1;
        // Empty incoming path still retains baseline ants in combination modes.
        canvasWindow_->setSelectionPathPreview(std::make_shared<const std::vector<core::SelectionEdge>>(),operation,0,0);
        colorSelectionTimer_->start(0); refreshColorSelectionControls(); updateActionState();
        statusBar()->showMessage(QStringLiteral("Sampling document colors… · Escape cancels"));
        return true;
    } catch (const std::exception& error) {
        cancelColorSelection();
        statusBar()->showMessage(QString::fromUtf8(error.what()),6000); return false;
    }
}
void MainWindow::changeColorSelectionFuzziness()
{
    if (updatingColorSelection_) return;
    refreshColorSelectionControls();
    if (!colorSelection_) return; // Can set the next click's fuzziness before sampling.
    colorSelectionEditing_ = true;
    colorSelectionFinishRequested_ = !colorSelectionFuzziness_->interactionActive();
    if (colorSelectionCancelled_) *colorSelectionCancelled_ = true;
    colorSelectionPreview_ = {}; colorSelectionPreviewFuzziness_ = -1;
    colorSelectionTimer_->start(0); updateActionState();
    statusBar()->showMessage(QStringLiteral("Refining color selection… · Escape cancels"));
}
void MainWindow::finishColorSelection()
{
    if (!colorSelectionEditing_ || updatingColorSelection_) return;
    colorSelectionFinishRequested_ = true;
    colorSelectionTimer_->start(0);
}
void MainWindow::advanceColorSelection()
{
    refreshColorSelectionControls(); // Checks document, content, target and selection ownership.
    try {
        if (colorSelectionWorker_.valid()) {
            if (colorSelectionWorker_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                colorSelectionTimer_->start(8); return;
            }
            core::ColorSelectionResult result;
            try { result = colorSelectionWorker_.get(); }
            catch (...) { if (!*colorSelectionCancelled_) throw; } // A stale failure cannot cancel a new click.
            if (colorSelectionEditing_ && colorSelection_ && !*colorSelectionCancelled_
                && colorSelectionWorkerFuzziness_ == int(colorSelectionFuzziness_->value())) {
                colorSelectionPreview_ = std::move(result);
                colorSelectionPreviewFuzziness_ = colorSelectionWorkerFuzziness_;
                if (colorSelectionPreview_.incoming)
                    canvasWindow_->setSelectionMaskPreview(colorSelectionPreview_.incoming,
                        colorSelection_->original,colorSelection_->operation);
            }
            colorSelectionCancelled_.reset();
        }
        if (!colorSelectionEditing_ || !colorSelection_) return;
        auto& context = *colorSelection_;
        if (!context.reference) {
            QElapsedTimer preparation; preparation.start();
            const auto& layers = session().document()->layers();
            while (context.nextLayer < layers.size()) {
                const auto& layer = layers[context.nextLayer++];
                const bool included = colorSelectionSource_ == core::ColorSampleSource::ActiveLayer
                    ? layer.id == context.layer : session().document()->isEffectivelyVisible(layer.id) && layer.opacity > 0;
                if (included && !std::holds_alternative<core::RasterLayer>(layer.payload)) {
                    // Same document-resolution preparation as export. Never
                    // install these caches into viewport/layer state or Vulkan.
                    constexpr std::size_t budget = 64 * 1024 * 1024;
                    auto cache = prepareDocumentSampleCache(layer,budget-context.preparedPixels);
                    if (!cache || !cache->surface) throw std::runtime_error("Could not prepare the reference layer");
                    const auto e = cache->surface->extent();
                    context.preparedPixels += std::size_t(e.width)*e.height;
                    if (context.preparedPixels > budget) throw std::length_error("Reference images exceed the 256 MiB cache budget");
                    context.prepared.push_back({layer.id,std::move(cache)});
                }
                if (preparation.elapsed() >= 4 && context.nextLayer < layers.size()) {
                    colorSelectionTimer_->start(0); return;
                }
            }
            context.reference = std::make_unique<core::ColorSelectionReference>(*session().document(),context.layer,
                colorSelectionSource_,context.seed,context.prepared);
            context.prepared.clear(); context.pinnedPixels.clear();
            refreshColorSelectionControls();
        }
        if (!context.reference->field()) {
            QElapsedTimer slice; slice.start();
            do {
                const auto budget = std::max(std::size_t(1), std::size_t(2048)
                    / std::max(std::size_t(1),context.reference->sourcesPerPixel()));
                if (context.reference->step(budget)) break;
            } while (slice.elapsed() < 4);
            if (!context.reference->field()) { colorSelectionTimer_->start(0); return; }
        }
        const auto fuzziness = int(colorSelectionFuzziness_->value());
        if (colorSelectionPreviewFuzziness_ != fuzziness || !colorSelectionPreview_.combined) {
            const auto field = context.reference->field();
            const auto original = context.original;
            const auto operation = context.operation;
            colorSelectionCancelled_ = std::make_shared<std::atomic_bool>(false);
            const auto cancelled = colorSelectionCancelled_;
            colorSelectionWorkerFuzziness_ = fuzziness;
            colorSelectionWorker_ = std::async(std::launch::async,[field,original,operation,fuzziness,cancelled] {
                return core::buildColorSelection(*field,fuzziness,original,operation,*cancelled);
            });
            colorSelectionTimer_->start(8); return;
        }
        if (!colorSelectionFinishRequested_) return;
        const bool changed = session().execute(std::make_unique<core::SetSelectionCommand>(
            colorSelectionPreview_.combined,"Select by color"));
        context.expected = session().document()->selection();
        context.appliedFuzziness = fuzziness;
        colorSelectionEditing_ = false; colorSelectionPreview_ = {}; colorSelectionPreviewFuzziness_ = -1;
        canvasWindow_->setSelectionPreview({});
        synchronizeUi(false,false);
        statusBar()->showMessage(changed ? QStringLiteral("Color selection applied") : QStringLiteral("Selection unchanged"),2500);
    } catch (const std::exception& error) {
        const auto message = QString::fromUtf8(error.what());
        cancelColorSelection(!colorSelection_ || !colorSelection_->reference);
        statusBar()->showMessage(QStringLiteral("Select by Color: %1").arg(message),6000);
    }
}
void MainWindow::cancelColorSelection(bool forgetReference)
{
    if (!colorSelectionTimer_) return;
    const QScopedValueRollback guard(updatingColorSelection_,true);
    if (colorSelectionCancelled_) *colorSelectionCancelled_ = true;
    const bool editing = std::exchange(colorSelectionEditing_,false);
    colorSelectionFinishRequested_ = false; colorSelectionPreview_ = {}; colorSelectionPreviewFuzziness_ = -1;
    if (editing) {
        if (colorSelection_) {
            const QSignalBlocker blocker(colorSelectionFuzziness_);
            colorSelectionFuzziness_->setValue(colorSelection_->appliedFuzziness);
        }
        canvasWindow_->setSelectionPreview({});
        canvasWindow_->cancelSelectionInput(); pointerRouter_->cancelCapture();
        colorSelectionFuzziness_->clearFocus();
    }
    if (forgetReference) colorSelection_.reset();
    if (colorSelectionWorker_.valid()) colorSelectionTimer_->start(8);
    else colorSelectionTimer_->stop();
}
void MainWindow::refreshColorSelectionControls()
{
    if (!colorSelectionControls_ || updatingColorSelection_) return;
    const auto* doc = session().document();
    if (colorSelection_ && (!doc || doc != colorSelection_->owner
        || doc->revision() != colorSelection_->revision || doc->selection() != colorSelection_->expected
        || (colorSelection_->reference && !colorSelection_->reference->matches(*doc))
        || std::ranges::any_of(colorSelection_->pinnedPixels,[](const auto& source) { return source.first->revision() != source.second; })
        || (colorSelectionSource_ == core::ColorSampleSource::ActiveLayer && session().activeLayer() != colorSelection_->layer)))
        cancelColorSelection();
    colorSelectionControls_->setVisible(session().activeTool() == core::ToolId::SelectByColor);
    static_cast<SampledColorLabel*>(colorSelectionSample_)->setSample(colorSelection_ && colorSelection_->reference
        ? std::optional(colorSelection_->reference->sampledColor()) : std::nullopt);
}
}
