#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/ShapeCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/QtShapeRenderService.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolOptionsBar.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include <QLabel>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
    QString shapeName(core::ShapeKind kind)
    {
        constexpr std::array names { "Rectangle", "Rounded rectangle", "Ellipse", "Triangle", "Line", "Polygon" };
        return QString::fromLatin1(names[static_cast<std::size_t>(kind)]);
    }
    core::AffineTransform toScreen(const render::CanvasWindow& canvas, const core::AffineTransform& local)
    {
        const auto& scene = canvas.scene();
        const auto origin = scene.viewport.documentToViewport({ },
            { static_cast<double>(scene.document.canvas.extent.width), static_cast<double>(scene.document.canvas.extent.height) },
            { static_cast<double>(canvas.width()), static_cast<double>(canvas.height()) });
        core::AffineTransform view;
        view.m00 = view.m11 = canvas.zoom();
        view.m02 = origin.x;
        view.m12 = origin.y;
        return core::composeAffine(view, local);
    }
}
bool MainWindow::shapeLayerHit(const core::Layer& layer, core::Vec2d p) const
{
    if(const auto inverse=layer.localToDocument.inverted();inverse&&core::layerMaskCoverage(layer.mask,inverse->map(p))<=0)return false;
    if(!core::hitLayerCrop(layer,p))return false;
    if (!session().document() || !session().document()->isEffectivelyVisible(layer.id) || layer.opacity <= 0)
        return false;
    const auto* shape = std::get_if<core::ShapeLayer>(&layer.payload);
    if (!shape)
        return false;
    // Reject distant layers before allocating any Qt paths. Round caps/joins
    // fit inside this conservative frame expanded by half the stroke width.
    const auto mapping = toScreen(*canvasWindow_, layer.localToDocument);
    const auto point = toScreen(*canvasWindow_, { }).map(p);
    const double halfStroke = shape->strokeEnabled ? shape->strokeWidth * .5 : 0;
    const std::array corners {
        mapping.map({ -halfStroke, -halfStroke }),
        mapping.map({ shape->size.width + halfStroke, -halfStroke }),
        mapping.map({ shape->size.width + halfStroke, shape->size.height + halfStroke }),
        mapping.map({ -halfStroke, shape->size.height + halfStroke })
    };
    auto lower = corners.front(), upper = lower;
    for (auto q : corners) {
        lower.x = std::min(lower.x, q.x);
        lower.y = std::min(lower.y, q.y);
        upper.x = std::max(upper.x, q.x);
        upper.y = std::max(upper.y, q.y);
    }
    if (point.x < lower.x - 6 || point.y < lower.y - 6 || point.x > upper.x + 6 || point.y > upper.y + 6)
        return false;
    if (shapeHitDocument_ != session().document()) {
        shapeHitCache_.clear();
        shapeHitDocument_ = session().document();
    }
    try {
        const auto found = shapeHitCache_.find(layer.id);
        if (found != shapeHitCache_.end() && found->second.revision != layer.shapeRevision)
            shapeHitCache_.erase(found);
        if (!shapeHitCache_.contains(layer.id)) {
            // Bound path metadata separately from the raster-cache budget.
            const auto vertices = std::max(std::size_t { 8 }, shape->points.size());
            const auto totalVertices = [this] {std::size_t n=0;for(const auto& pair:shapeHitCache_)n+=pair.second.vertices;return n; };
            while (!shapeHitCache_.empty() && (shapeHitCache_.size() >= 32 || totalVertices() + vertices > 250'000)) {
                auto oldest = std::min_element(shapeHitCache_.begin(), shapeHitCache_.end(), [](const auto& a, const auto& b) { return a.second.lastUsed < b.second.lastUsed; });
                shapeHitCache_.erase(oldest);
            }
        }
        auto& hit = shapeHitCache_[layer.id];
        if (!hit.geometry || hit.revision != layer.shapeRevision) {
            hit.geometry = QtShapeRenderService { }.prepareHit(*shape);
            hit.revision = layer.shapeRevision;
            hit.vertices = std::max(std::size_t { 8 }, shape->points.size());
        }
        hit.lastUsed = ++shapeHitClock_;
        return hit.geometry->hit(mapping, point, 6.0);
    } catch (const std::exception&) {
        return false;
    }
}

void MainWindow::createShapeControls()
{
    shapeOptionsPage_ = new ShapeOptionsPage;
    toolOptionsBar_->registerToolPage(core::ToolId::Shape, QStringLiteral("Shape"), shapeOptionsPage_);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::Shape, shapeOptionsPage_->modeWidget());
    QVBoxLayout* properties = nullptr;
    propertiesPanel_->addToolPage(core::ToolId::Shape, QStringLiteral("Shape"), QStringLiteral("Editable geometry in layer-local pixels. Use the top bar for shape modes, fill and stroke."), properties);
    shapeOptionsPage_->populateProperties(properties);
    auto* helpTitle = new QLabel(QStringLiteral("INTERACTION"));
    helpTitle->setObjectName(QStringLiteral("SectionLabel"));
    properties->addWidget(helpTitle);
    auto* help = new QLabel(QStringLiteral(
        "{{ToolAction_shape}} · Shape; drag empty canvas · Create\n"
        "Shift at press · Force new shape (except on handles)\n"
        "Shift during creation · Square / circle / 45° line\n"
        "Drag inside bounds · Move; handles · Resize; outside corners · Rotate\n"
        "Shift · Constrain resizing / snap rotation to 15°; Alt · Resize from center\n\n"
        "Polygon: click · Add vertex; {{RemovePointAction}} · Remove vertex\n"
        "{{FinishOperationAction}} / right-click / double-click / first vertex · Finish polygon\n"
        "Escape · Cancel construction or drag\n{{UndoAction}} / {{RedoAction}} · Undo / redo\n\n"
        "{{LayerTransformAction}} · Whole-layer transform; {{FinishOperationAction}} / right-click · Apply; Escape · Cancel session\n"
        "Ordinary handles edit geometry and keep stroke width; {{LayerTransformAction}} scales the whole layer, including stroke. "
        "Shapes ignore raster selections."));
    help->setObjectName(QStringLiteral("MutedLabel"));
    help->setWordWrap(true);
    properties->addWidget(help);
    shapeOptionsPage_->onModeChanged = [this](core::ShapeKind kind) {
        if (shapeCreation_)
            return;
        shapeMode_ = kind;
        if (kind == core::ShapeKind::Line)
            shapeDefaults_.strokeEnabled = true;
        // A mode chooses future geometry; selecting it never rewrites a layer.
        const auto* layer = session().document() && session().activeLayer() ? session().document()->layer(*session().activeLayer()) : nullptr;
        if (!layer || !std::holds_alternative<core::ShapeLayer>(layer->payload))
            refreshShapeControls();
    };
    shapeOptionsPage_->onShapeChanged = [this](const core::ShapeLayer& shape) { changeShape(shape); };
    shapeOptionsPage_->onNumericFinished = [this](bool commit) { finishShapeEdit(commit); };
    shapeOptionsPage_->onColorRequested = [this](bool fill) { chooseShapeColor(fill); };
    shapeOptionsPage_->onTransformRequested = [this] { beginLayerTransform(); };
    shapePreviewTimer_ = new QTimer(this);
    shapePreviewTimer_->setSingleShot(true);
    shapePreviewTimer_->setInterval(16);
    connect(shapePreviewTimer_, &QTimer::timeout, this, [this] {
        if (shapeResize_)
            publishShapeResize();
        else
            publishShapePreview();
    });
    shapeDensityTimer_ = new QTimer(this);
    shapeDensityTimer_->setSingleShot(true);
    shapeDensityTimer_->setInterval(70);
    connect(shapeDensityTimer_, &QTimer::timeout, this, [this] {
        if (!session().document())
            return;
        if (canvasWindow_->transformDragging()) {
            shapeDensityTimer_->start();
            return;
        }
        prepareShapeCaches();
        if (shapeCreation_)
            publishShapePreview();
        else if (!(textController_ && textController_->active()))
            canvasWindow_->setDocument(session().document()->snapshot(), false);
    });
    canvasWindow_->onShapeHit = [this](core::Vec2d p, bool active) { return hitShape(p, active).has_value(); };
    canvasWindow_->onPrepareDocumentSnapshot = [this](core::DocumentSnapshot& snapshot) {
        if (shapeResize_) {
            // Another publisher (for example text-cache density refresh) may
            // arrive before the coalesced shape rebuild. Retain the last
            // coherent presentation of this layer, not a null cache or an old
            // stroked raster under the new geometry/anchor. Core stays live.
            auto next = std::find_if(snapshot.layersBottomToTop.begin(), snapshot.layersBottomToTop.end(),
                [this](const auto& layer) { return layer.id == shapeResize_->id; });
            const auto& displayed = canvasWindow_->scene().document.layersBottomToTop;
            const auto previous = std::find_if(displayed.begin(), displayed.end(),
                [this](const auto& layer) { return layer.id == shapeResize_->id; });
            if (next != snapshot.layersBottomToTop.end() && !next->renderCache
                && previous != displayed.end() && previous->renderCache)
                *next = *previous;
        }
        if (!shapeCreation_ || !shapeCreation_->layer.renderCache)
            return;
        const auto& layer = shapeCreation_->layer;
        core::LayerSnapshot preview;
        preview.id = layer.id;
        preview.name = layer.name;
        preview.payload = std::get<core::ShapeLayer>(layer.payload);
        preview.localToDocument = layer.localToDocument;
        preview.renderCache = layer.renderCache;
        snapshot.layersBottomToTop.push_back(std::move(preview));
    };
    canvasWindow_->onShapePressed = [this](core::Vec2d p, Qt::KeyboardModifiers mods) {
        using Action = render::CanvasWindow::ShapePress;
        if (!session().document() || fileBusy_)
            return Action::Ignore;
        shapeOptionsPage_->finishNumericInput();
        if (shapeCreation_) {
            auto& draft = *shapeCreation_;
            if (std::get<core::ShapeLayer>(draft.layer.payload).kind != core::ShapeKind::Polygon)
                return Action::Ignore;
            if (draft.vertices.size() >= 3 && std::hypot(p.x - draft.vertices.front().x, p.y - draft.vertices.front().y) * canvasWindow_->zoom() <= 8) {
                finishShapeCreation(false);
                return Action::Ignore;
            }
            if (draft.vertices.size() >= core::maximumShapePoints) {
                statusBar()->showMessage(QStringLiteral("Polygon vertex limit reached; finish or cancel the shape."), 4000);
                return Action::Ignore;
            }
            if (draft.vertices.empty() || std::hypot(p.x - draft.vertices.back().x, p.y - draft.vertices.back().y) > 1e-8)
                draft.vertices.push_back(p);
            moveShapeCreation(p);
            return Action::Create;
        }
        if (!mods.testFlag(Qt::ShiftModifier)) {
            if(const auto hit=hitMoveLayer(p); hit && session().document()->tree().container(*hit)) {
                session().setActiveLayer(*hit);synchronizeUi(false,false);
                statusBar()->showMessage(QStringLiteral("Group selected · Use Move/%1, or Ungroup to edit shapes. Shift creates a new shape.")
                    .arg(shortcutLabel(shortcuts_, "LayerTransformAction")),4000);
                return Action::Ignore;
            }
            if (const auto hit = hitShape(p)) {
                session().setActiveLayer(*hit);
                synchronizeUi(false, false);
                return Action::Move;
            }
        }
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return Action::Ignore;
        auto shape = shapeDefaults_;
        shape.kind = shapeMode_;
        shape.points.clear();
        shape.size = { 0, 0 };
        if (!shapeDefaultColorsEdited_)
            shape.fillColor = shape.strokeColor = session().foregroundColor();
        if (shape.kind == core::ShapeKind::Line) {
            shape.fillEnabled = false;
            shape.points = { { 0, 0 }, { 0, 0 } };
        }
        if (shape.kind == core::ShapeKind::Polygon)
            shape.points = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
        shapeCreation_.emplace(ShapeCreation { core::Layer::shape(shapeName(shape.kind).toStdString(), shape), p, p, { }, mods.testFlag(Qt::ShiftModifier), true });
        if (shape.kind == core::ShapeKind::Polygon)
            shapeCreation_->vertices.push_back(p);
        canvasWindow_->setTransformOverlay({ });
        canvasWindow_->setShapeConstructionActive(true);
        shapeOptionsPage_->setConstructionActive(true);
        updateActionState();
        moveShapeCreation(p);
        return Action::Create;
    };
    canvasWindow_->onShapeMoved = [this](core::Vec2d p, Qt::KeyboardModifiers modifiers) {
        if (shapeCreation_)
            shapeCreation_->ratioLocked = modifiers.testFlag(Qt::ShiftModifier);
        moveShapeCreation(p);
    };
    canvasWindow_->onShapeEnded = [this](bool cancel) {
        if (!shapeCreation_)
            return;
        if (cancel || std::get<core::ShapeLayer>(shapeCreation_->layer.payload).kind != core::ShapeKind::Polygon)
            finishShapeCreation(cancel);
    };
    canvasWindow_->onShapeCloseRequested = [this](core::Vec2d p) { closeShapePolygon(p); };
}

std::optional<core::LayerId> MainWindow::hitShape(core::Vec2d p, bool activeOnly) const
{
    if (!session().document())
        return { };
    const auto hit = [this, p](const core::Layer& layer) {
        if(!core::hitLayerCrop(layer,p))return false;
        const auto* shape = std::get_if<core::ShapeLayer>(&layer.payload);
        if (!shape || !session().document()->isEffectivelyVisible(layer.id) || layer.opacity <= 0)
            return false;
        // Shape mode manipulates the full, exact transform frame, including
        // empty ellipse/polygon corners. Inverse mapping preserves rotation
        // and flips; normal Move still calls the visual shapeLayerHit directly.
        if (const auto inverse = layer.localToDocument.inverted()) {
            const auto local = inverse->map(p);
            if (local.x >= 0 && local.y >= 0 && local.x <= shape->size.width
                && local.y <= shape->size.height)
                return true;
        }
        // Retain comfortable line/stroke targeting outside the logical frame.
        return shapeLayerHit(layer, p);
    };
    if (activeOnly) {
        const auto* layer = session().activeLayer() ? session().document()->layer(*session().activeLayer()) : nullptr;
        return layer && hit(*layer) ? std::optional(layer->id) : std::nullopt;
    }
    const auto& layers = session().document()->layers();
    const auto content = core::hitTestRasterLayer(*session().document(), p);
    for (auto i = layers.rbegin(); i != layers.rend(); ++i) {
        if (hit(*i) && core::clippingVisibility(*session().document(),i->id,p)>0)
            return i->id;
        if (content == i->id && !std::holds_alternative<core::ShapeLayer>(i->payload))
            return { };
    }
    return { };
}
std::optional<core::LayerId> MainWindow::hitMoveLayer(core::Vec2d p) const
{
    if (!session().document())
        return { };
    const auto ordinary = core::hitTestRasterLayer(*session().document(), p);
    // Respect stacking across ALL layer kinds while giving shapes a logical-
    // screen tolerance (not alpha/bounding-box-only targeting).
    const auto& layers = session().document()->layers();
    for (auto i = layers.rbegin(); i != layers.rend(); ++i) {
        if (std::holds_alternative<core::ShapeLayer>(i->payload)) {
            if (shapeLayerHit(*i, p) && core::clippingVisibility(*session().document(),i->id,p)>0)
                return session().document()->canvasTarget(i->id);
        } else if (ordinary == i->id)
            return session().document()->canvasTarget(i->id);
    }
    return { };
}
bool MainWindow::beginShapeManipulation(core::TransformHandle handle, core::Vec2d p)
{
    if (shapeCreation_ || !session().activeLayer() || !session().document())
        return false;
    shapeOptionsPage_->finishNumericInput();
    auto* layer = session().document()->layer(*session().activeLayer());
    if (!layer || !std::holds_alternative<core::ShapeLayer>(layer->payload) || !session().document()->isEffectivelyVisible(layer->id) || layer->opacity <= 0)
        return false;
    if (handle >= core::TransformHandle::TopLeft && handle <= core::TransformHandle::Left) {
        try {
            shapeResize_.emplace(ShapeResizeEdit { layer->id,
                core::ShapeResizeGesture(std::get<core::ShapeLayer>(layer->payload), layer->localToDocument, handle, p),
                layer->shapeRevision, layer->localToDocument, layer->renderCache });
        } catch (const std::exception&) {
            statusBar()->showMessage(QStringLiteral("Cannot start this shape resize; geometry is unchanged."), 5000);
            return false;
        }
        updateActionState();
        return true;
    }
    auto edit = std::make_unique<core::LayerTransformSession>(*session().document(), layer->id);
    if (!edit->active() || !edit->beginDrag(handle, p))
        return false;
    activeLayerMove_ = std::move(edit);
    updateActionState();
    return true;
}
bool MainWindow::updateShapeResize(core::Vec2d position, core::TransformModifiers modifiers)
{
    if (!shapeResize_ || !session().document())
        return false;
    auto& edit = *shapeResize_;
    auto* layer = session().document()->layer(edit.id);
    if (!layer || layer->shapeRevision != edit.revision || layer->localToDocument != edit.currentTransform
        || !std::holds_alternative<core::ShapeLayer>(layer->payload))
        return false;
    try {
        auto resolved = edit.gesture.resolve(position, modifiers);
        if (!resolved)
            return true; // Out-of-budget point: keep the last valid geometry.
        if (session().document()->setLayerShapeGeometry(edit.id, std::move(resolved->shape), resolved->transform)) {
            edit.revision = layer->shapeRevision;
            edit.currentTransform = layer->localToDocument;
            // Properties and handles follow input immediately. Raster work is
            // coalesced; an old outline is never stretched into a new aspect.
            refreshShapeControls();
            refreshShapeOverlay();
            if (!shapePreviewTimer_->isActive())
                shapePreviewTimer_->start();
        }
        return true;
    } catch (const std::exception&) {
        statusBar()->showMessage(QStringLiteral("Shape resize could not allocate memory; restoring its starting geometry."), 5000);
        return false;
    }
}
void MainWindow::publishShapeResize()
{
    if (!shapeResize_ || !session().document())
        return;
    prepareShapeCaches();
    canvasWindow_->setDocument(session().document()->snapshot(), false);
}
void MainWindow::finishShapeResize(bool commit)
{
    if (!shapeResize_)
        return;
    auto edit = std::move(*shapeResize_);
    shapeResize_.reset();
    shapePreviewTimer_->stop();
    if (!commit) {
        pointerRouter_->cancelCapture();
        canvasWindow_->cancelTransformInput();
    }
    auto* doc = session().document();
    auto* layer = doc ? doc->layer(edit.id) : nullptr;
    if (layer && layer->shapeRevision == edit.revision && layer->localToDocument == edit.currentTransform
        && std::holds_alternative<core::ShapeLayer>(layer->payload)) {
        auto before = std::move(edit.gesture).releaseBefore();
        bool accepted = false;
        try {
            core::ShapeGeometryState after { std::get<core::ShapeLayer>(layer->payload), layer->localToDocument };
            if (commit && before != after) {
                accepted = session().adoptApplied(std::make_unique<core::ResizeShapeCommand>(edit.id, before, std::move(after)));
                if (accepted)
                    fileState().untouched = false;
            }
        } catch (const std::exception&) {
            statusBar()->showMessage(QStringLiteral("Shape resize could not be recorded; original geometry restored."), 5000);
        }
        if (!accepted) {
            doc->setLayerShapeGeometry(edit.id, std::move(before.shape), before.transform);
            if (edit.originalCache) {
                try {
                    auto restored = std::make_shared<core::LayerRenderCache>(*edit.originalCache);
                    restored->contentRevision = layer->shapeRevision;
                    layer->renderCache = std::move(restored);
                } catch (const std::bad_alloc&) {
                    // Restoring authoritative geometry never needs allocation;
                    // disposable cache metadata can be regenerated later.
                }
            }
        }
    }
    synchronizeUi(false, false);
}
void MainWindow::prepareShapeCaches()
{
    auto* doc = session().document();
    if (!doc)
        return;
    std::erase_if(shapeHitCache_, [doc](const auto& pair) { return !doc->containsLayer(pair.first); });
    const auto shapeCount = std::count_if(doc->layers().begin(), doc->layers().end(), [](const auto& layer) {
        return std::holds_alternative<core::ShapeLayer>(layer.payload);
    });
    // A hostile or simply large multilayer file must not allocate 16M pixels
    // per layer without limit. Small shapes still get their normal density;
    // large ones share at most 64M resident cache pixels in total. The service
    // enforces its own 16M/8192 limits as well. Old published snapshots may
    // retain the previous bounded cache generation until the next frame.
    const double pixelBudget = std::min(16.0 * 1024 * 1024, 64.0 * 1024 * 1024 / static_cast<double>(std::max(std::ptrdiff_t { 1 }, shapeCount)));
    for (const auto& item : doc->layers()) {
        const auto* shape = std::get_if<core::ShapeLayer>(&item.payload);
        if (!shape)
            continue;
        auto* layer = doc->layer(item.id);
        const auto& t = layer->localToDocument;
        const double scale = t.maximumScaleOver({0,0,std::max(1.0,shape->size.width),std::max(1.0,shape->size.height)});
        const bool filtered = core::hasActiveSpatialFilters(layer->filters)||core::hasActiveLayerEffects(layer->effects);
        const auto viewScale = canvasWindow_->zoom() * canvasWindow_->devicePixelRatio();
        const auto requested = filtered ? 1.0 : QtShapeRenderService::densityForScale(scale * viewScale);
        const double stroke = shape->strokeEnabled ? shape->strokeWidth : 0;
        const double w = shape->size.width + stroke, h = shape->size.height + stroke;
        // Conservative box plus 8 physical pixels covers AA fringe and ceil.
        const bool liveEdit = (shapeEdit_ && shapeEdit_->id == layer->id) || (shapeResize_ && shapeResize_->id == layer->id);
        const double liveBudget = liveEdit ? std::min(pixelBudget, 1'000'000.0) : pixelBudget;
        const double area = w * h, perimeter = 8 * (w + h), available = std::max(1.0, liveBudget - 64);
        const double denominator = perimeter + std::sqrt(perimeter * perimeter + 4 * area * available);
        const double budgetDensity = denominator > 0 ? 2 * available / denominator : requested;
        // Spatial filters run in layer-local units, on a stable source grid.
        // Changing their input resolution with zoom changes the filtered image.
        const auto density = filtered ? 1.0 : std::min(requested, budgetDensity);
        bool documentGrid = !filtered && !liveEdit && viewScale <= 1;
        bool reducedPreview = false;
        if (documentGrid) {
            const bool current = layer->renderCache && layer->renderCache->contentRevision == layer->shapeRevision
                && layer->renderCache->rasterizedDocumentTransform == t;
            core::RectD bounds;
            try {
                if(!current) bounds=QtShapeRenderService{}.documentBounds(*shape,t);
            } catch(const std::exception& error) {
                statusBar()->showMessage(QString::fromUtf8(error.what()),5000);
                continue;
            }
            const auto width = current ? layer->renderCache->surface->extent().width
                : std::ceil(bounds.right()) - std::floor(bounds.x);
            const auto height = current ? layer->renderCache->surface->extent().height
                : std::ceil(bounds.bottom()) - std::floor(bounds.y);
            // Presentation keeps its existing independent memory/dimension
            // limits; output admission never inherits this preview fallback.
            reducedPreview = width > 8192 || height > 8192 || width * height > liveBudget;
            documentGrid = !reducedPreview;
        }
        const bool contentChanged = !layer->renderCache || layer->renderCache->contentRevision != layer->shapeRevision;
        const bool modeChanged = layer->renderCache
            && layer->renderCache->rasterizedDocumentTransform.has_value() != documentGrid;
        if (documentGrid && !contentChanged && !canvasWindow_->transformDragging())
            if (auto moved = core::translatedDocumentRenderCache(layer->renderCache, t))
                layer->renderCache = std::move(moved);
        const bool transformChanged = documentGrid && !canvasWindow_->transformDragging() && layer->renderCache
            && layer->renderCache->rasterizedDocumentTransform != t;
        if (contentChanged || modeChanged || transformChanged
            || (layer->renderCache->requestedDensity != (documentGrid ? 1.0 : density) && !canvasWindow_->transformDragging())) {
            try {
                const auto source = documentGrid
                    ? QtShapeRenderService {}.renderDocument(*shape, t, std::size_t(liveBudget))
                    : QtShapeRenderService {}.render({ *shape, density,
                        filtered ? std::optional<std::size_t>(std::size_t(pixelBudget)) : std::nullopt });
                if (filtered && (source->surface->extent().width > 8192 || source->surface->extent().height > 8192))
                    throw std::runtime_error("Filtered shape source exceeds the display dimension limit");
                auto cache = std::make_shared<core::LayerRenderCache>(*source);
                cache->contentRevision = layer->shapeRevision;
                layer->renderCache = std::move(cache);
                if (reducedPreview)
                    statusBar()->showMessage(QStringLiteral("This shape exceeds the full-resolution preview budget. Its editable data is retained."), 5000);
            } catch (const std::exception&) {
                if (filtered && layer->renderCache && layer->renderCache->rasterizedDocumentTransform)
                    layer->renderCache.reset();
                statusBar()->showMessage(QStringLiteral("Not enough memory to render this shape. Its editable data is retained."), 5000);
            }
        }
    }
}
void MainWindow::refreshShapeControls()
{
    if (!shapeOptionsPage_)
        return;
    const auto* layer = session().document() && session().activeLayer() ? session().document()->layer(*session().activeLayer()) : nullptr;
    const auto* shape = layer ? std::get_if<core::ShapeLayer>(&layer->payload) : nullptr;
    // Creation mode is editor state. Selecting a layer (including through undo)
    // updates editable properties, never the user's next-creation choice.
    auto defaults = shapeDefaults_;
    defaults.kind = shapeMode_;
    defaults.points.clear();
    if (!shapeDefaultColorsEdited_)
        defaults.fillColor = defaults.strokeColor = session().foregroundColor();
    if (defaults.kind == core::ShapeKind::Line) {
        defaults.fillEnabled = false;
        defaults.points = { { 0, 0 }, { defaults.size.width, defaults.size.height } };
    }
    if (defaults.kind == core::ShapeKind::Polygon)
        defaults.points = { { 0, 0 }, { defaults.size.width, 0 }, { 0, defaults.size.height } };
    shapeOptionsPage_->setShape(shape ? *shape : defaults, shape != nullptr);
    shapeOptionsPage_->setConstructionActive(shapeCreation_.has_value());
}
void MainWindow::refreshShapeOverlay()
{
    if (session().activeTool() != core::ToolId::Shape)
        return;
    const auto* layer = session().document() && session().activeLayer() ? session().document()->layer(*session().activeLayer()) : nullptr;
    if (shapeCreation_ || !layer || !session().document()->isEffectivelyVisible(layer->id) || layer->opacity <= 0 || !std::holds_alternative<core::ShapeLayer>(layer->payload)) {
        canvasWindow_->setTransformOverlay({ });
        return;
    }
    const auto extent = core::layerGeometryExtent(*layer);
    const auto values = core::valuesFromTransform(layer->localToDocument, { 1, 1 });
    if (values)
        canvasWindow_->setTransformOverlay(render::TransformOverlay { layer->localToDocument, extent, values->rotationDegrees,
            std::get<core::ShapeLayer>(layer->payload).size });
    else
        canvasWindow_->setTransformOverlay({ });
}
void MainWindow::moveShapeCreation(core::Vec2d p)
{
    if (!shapeCreation_ || !std::isfinite(p.x) || !std::isfinite(p.y))
        return;
    auto& draft = *shapeCreation_;
    // Bound admission, not canvas-clamp: off-canvas geometry remains intact.
    p.x = std::clamp(p.x, draft.start.x - core::maximumShapeDimension, draft.start.x + core::maximumShapeDimension);
    p.y = std::clamp(p.y, draft.start.y - core::maximumShapeDimension, draft.start.y + core::maximumShapeDimension);
    draft.pointer = p;
    draft.previewDirty = true;
    if (!shapePreviewTimer_->isActive())
        shapePreviewTimer_->start();
}
void MainWindow::publishShapePreview()
{
    if (!shapeCreation_ || !session().document())
        return;
    auto& draft = *shapeCreation_;
    auto shape = std::get<core::ShapeLayer>(draft.layer.payload);
    auto p = draft.pointer;
    if (shape.kind != core::ShapeKind::Polygon && draft.ratioLocked) {
        const auto d = p - draft.start;
        if (shape.kind == core::ShapeKind::Line) {
            p = core::constrainLineEndpoint(draft.start, p);
        } else {
            const auto side = std::max(std::abs(d.x), std::abs(d.y));
            p = draft.start + core::Vec2d { std::copysign(side, d.x), std::copysign(side, d.y) };
        }
    }
    auto points = shape.kind == core::ShapeKind::Polygon ? draft.vertices : std::vector<core::Vec2d> { draft.start, p };
    if (shape.kind == core::ShapeKind::Polygon && (points.empty() || points.back() != p))
        points.push_back(p);
    if (points.empty())
        return;
    core::Vec2d minimum = points[0], maximum = points[0];
    for (auto q : points) {
        minimum.x = std::min(minimum.x, q.x);
        minimum.y = std::min(minimum.y, q.y);
        maximum.x = std::max(maximum.x, q.x);
        maximum.y = std::max(maximum.y, q.y);
    }
    shape.size = { maximum.x - minimum.x, maximum.y - minimum.y };
    if (shape.kind == core::ShapeKind::Polygon || shape.kind == core::ShapeKind::Line) {
        shape.points = points;
        for (auto& q : shape.points)
            q = q - minimum;
    }
    draft.layer.localToDocument.m02 = minimum.x;
    draft.layer.localToDocument.m12 = minimum.y;
    if (shape.kind == core::ShapeKind::Polygon) {
        auto edges = std::make_shared<std::vector<core::SelectionEdge>>();
        for (std::size_t i = 1; i < points.size(); ++i)
            edges->push_back({ points[i - 1], points[i] });
        if (points.size() > 1)
            edges->push_back({ points.back(), points.front() });
        canvasWindow_->setSelectionPathPreview(edges, core::SelectionOperation::Add, 1, { }, draft.vertices.size());
    }
    if (!core::validShape(shape) || !core::hasShapeGeometry(shape)) {
        // Returning a drag to zero must erase its old preview, not commit the
        // most recent nonzero rectangle. Polygon construction can temporarily
        // contain fewer than three points, but remains provisional only.
        draft.layer.payload = shape;
        draft.layer.renderCache.reset();
        canvasWindow_->setDocument(session().document()->snapshot(), false);
        canvasWindow_->setPointerTooltip({ });
        return;
    }
    if (draft.previewDirty || !draft.layer.renderCache) {
        const auto fullDensity = QtShapeRenderService::densityForScale(canvasWindow_->zoom() * canvasWindow_->devicePixelRatio());
        const auto outset = shape.strokeEnabled ? shape.strokeWidth : 0;
        const auto area = std::max(1.0, (shape.size.width + outset + 4) * (shape.size.height + outset + 4));
        const auto previewDensity = std::min(fullDensity, std::sqrt(1'000'000.0 / area));
        try {
            draft.layer.renderCache = QtShapeRenderService { }.render({ shape, previewDensity });
        } catch (const std::exception&) {
            finishShapeCreation(true);
            statusBar()->showMessage(QStringLiteral("Shape preview could not allocate memory; creation cancelled."), 5000);
            return;
        }
        draft.layer.payload = shape;
        draft.previewDirty = false;
    }
    canvasWindow_->setDocument(session().document()->snapshot(), false);
    if (shape.kind != core::ShapeKind::Polygon)
        canvasWindow_->setPointerSizeTooltip(shape.size);
}
void MainWindow::finishShapeCreation(bool cancel)
{
    if (!shapeCreation_)
        return;
    shapePreviewTimer_->stop();
    if (!cancel) {
        if (std::get<core::ShapeLayer>(shapeCreation_->layer.payload).kind == core::ShapeKind::Polygon) {
            if (shapeCreation_->vertices.size() < 3)
                cancel = true;
            else
                shapeCreation_->pointer = shapeCreation_->vertices.back();
        }
        if (!cancel) {
            shapeCreation_->previewDirty = true;
            publishShapePreview();
            if (!shapeCreation_)
                return;
        }
    }
    auto draft = std::move(*shapeCreation_);
    shapeCreation_.reset();
    canvasWindow_->setShapeConstructionActive(false);
    canvasWindow_->cancelShapeInput();
    canvasWindow_->setSelectionPathPreview({ });
    canvasWindow_->setPointerTooltip({ });
    if (!cancel && session().document()) {
        auto& shape = std::get<core::ShapeLayer>(draft.layer.payload);
        if (core::validShape(shape) && core::hasShapeGeometry(shape)) {
            // Mark the draft cache valid for content, but its preview density
            // deliberately differs so publication upgrades it once on release.
            const auto id = draft.layer.id;
            try {
                const auto index = session().document()->layers().size();
                if (session().execute(std::make_unique<core::AddLayerCommand>(std::move(draft.layer), index, session().activeLayer()))) {
                    session().setActiveLayer(id);
                    fileState().untouched = false;
                }
            } catch (const std::exception&) {
                statusBar()->showMessage(QStringLiteral("Could not create shape; no layer was added."), 5000);
            }
        }
    }
    synchronizeUi(true, false);
    refreshShapeOverlay();
}
void MainWindow::closeShapePolygon(core::Vec2d)
{
    if (shapeCreation_)
        finishShapeCreation(false);
}
void MainWindow::removeShapeVertex()
{
    if (!shapeCreation_)
        return;
    auto& vertices = shapeCreation_->vertices;
    if (vertices.size() <= 1) {
        finishShapeCreation(true);
        return;
    }
    vertices.pop_back();
    shapeCreation_->previewDirty = true;
    publishShapePreview();
}
void MainWindow::changeShape(const core::ShapeLayer& shape)
{
    if (shapeCreation_ || !core::validShape(shape))
        return;
    auto* doc = session().document();
    auto* layer = doc && session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
    const auto* current = layer ? std::get_if<core::ShapeLayer>(&layer->payload) : nullptr;
    if (!current) {
        shapeDefaults_ = shape;
        return;
    }
    if (*current == shape)
        return;
    if(!layer->localToDocument.isAffine()) {
        try {
            (void)QtShapeRenderService{}.documentBounds(shape,layer->localToDocument);
        } catch(const std::exception& error) {
            statusBar()->showMessage(QString::fromUtf8(error.what()),5000);
            refreshShapeControls();
            return;
        }
    }
    if (!shapeEdit_)
        shapeEdit_ = ShapeEdit { layer->id, *current };
    if (shapeEdit_->id != layer->id)
        return;
    doc->setLayerShape(layer->id, shape);
    prepareShapeCaches();
    canvasWindow_->setDocument(doc->snapshot(), false);
    refreshShapeOverlay();
}
void MainWindow::finishShapeEdit(bool commit)
{
    if (!shapeEdit_)
        return;
    auto edit = std::move(*shapeEdit_);
    shapeEdit_.reset();
    auto* doc = session().document();
    auto* layer = doc ? doc->layer(edit.id) : nullptr;
    auto* shape = layer ? std::get_if<core::ShapeLayer>(&layer->payload) : nullptr;
    if (!shape)
        return;
    try {
        if (commit && *shape != edit.before) {
            if (session().adoptApplied(std::make_unique<core::SetShapeCommand>(edit.id, edit.before, *shape)))
                fileState().untouched = false;
            else
                doc->setLayerShape(edit.id, edit.before);
        } else if (!commit)
            doc->setLayerShape(edit.id, edit.before);
    } catch (const std::exception&) {
        doc->setLayerShape(edit.id, edit.before);
        statusBar()->showMessage(QStringLiteral("Shape edit could not be recorded; original style restored."), 5000);
    }
    synchronizeUi(false, false);
}
void MainWindow::chooseShapeColor(bool fill)
{
    finishShapeEdit(true);
    const auto target = session().activeLayer();
    const auto* layer = session().document() && target ? session().document()->layer(*target) : nullptr;
    const auto* data = layer ? std::get_if<core::ShapeLayer>(&layer->payload) : nullptr;
    auto shape = data ? *data : shapeDefaults_;
    if (!data) {
        shape.kind = shapeMode_;
        shape.points.clear();
        if (shapeMode_ == core::ShapeKind::Line)
            shape.points = { { 0, 0 }, { shape.size.width, shape.size.height } };
        if (shapeMode_ == core::ShapeKind::Polygon)
            shape.points = { { 0, 0 }, { shape.size.width, 0 }, { 0, shape.size.height } };
        if (!shapeDefaultColorsEdited_)
            shape.fillColor = shape.strokeColor = session().foregroundColor();
    }
    const auto color = fill ? shape.fillColor : shape.strokeColor;
    auto* owner = workspace_->panelOverlay()->window();
    auto* dialog = new ColorDialog(QColor(color.red, color.green, color.blue, color.alpha), owner);
    dialog->setObjectName(QStringLiteral("ShapeColorDialog"));
    dialog->setWindowTitle(fill ? QStringLiteral("Shape fill color") : QStringLiteral("Shape stroke color"));
    dialog->setOptions(ColorDialog::DontUseNativeDialog | ColorDialog::ShowAlphaChannel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &ColorDialog::colorSelected, this, [this, fill, target, shape](QColor c) {
        if (session().activeLayer() != target)
            return;
        const auto* layer = session().document() && target ? session().document()->layer(*target) : nullptr;
        const auto* current = layer ? std::get_if<core::ShapeLayer>(&layer->payload) : nullptr;
        auto changed = current ? *current : shape;
        (fill ? changed.fillColor : changed.strokeColor) = core::Rgba8 { static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()), static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha()) };
        if (!current)
            shapeDefaultColorsEdited_ = true;
        changeShape(changed);
        finishShapeEdit(true);
        refreshShapeControls();
    });
    dialog->show();
}
}
