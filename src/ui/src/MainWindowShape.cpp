#include "imageeditor/ui/Localization.hpp"
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
    toolOptionsBar_->registerToolPage(core::ToolId::Shape, i18n::text("main.window.shape.shape"), shapeOptionsPage_);
    toolOptionsBar_->registerToolLeadingWidget(core::ToolId::Shape, shapeOptionsPage_->modeWidget());
    QVBoxLayout* properties = nullptr;
    propertiesPanel_->addToolPage(core::ToolId::Shape, i18n::text("main.window.shape.shape"), i18n::text("main.window.shape.editable.geometry.in.layer.local.pixels.use.the.top"), properties);
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
        if (backgroundRemoval_) { prepareBackgroundRemovalSnapshot(snapshot); return; }
        if (refinement_) { prepareRefinementSnapshot(snapshot); return; }
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
                statusBar()->showMessage(i18n::text("main.window.shape.polygon.vertex.limit.reached.finish.or.cancel.the.shape"), 4000);
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
                queueSelectedLayerFocus();
                statusBar()->showMessage(i18n::text("main.window.shape.group.selected.use.move.1.or.ungroup.to.edit")
                    .arg(shortcutLabel(shortcuts_, "LayerTransformAction")),4000);
                return Action::Ignore;
            }
            if (const auto hit = hitShape(p)) {
                session().setActiveLayer(*hit);
                synchronizeUi(false, false);
                queueSelectedLayerFocus();
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
            statusBar()->showMessage(i18n::text("main.window.shape.cannot.start.this.shape.resize.geometry.is.unchanged"), 5000);
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
        statusBar()->showMessage(i18n::text("main.window.shape.shape.resize.could.not.allocate.memory.restoring.its.starting"), 5000);
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
            statusBar()->showMessage(i18n::text("main.window.shape.shape.resize.could.not.be.recorded.original.geometry.restored"), 5000);
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
    if (shapeCacheWarningDocument_ != activeDocumentId()) {
        shapeCacheWarnings_.clear(); shapeCacheWarningDocument_ = activeDocumentId();
    }
    std::erase_if(shapeCacheWarnings_, [doc](const auto& pair) {
        const auto* layer = doc->layer(pair.first);
        return !layer || !std::holds_alternative<core::ShapeLayer>(layer->payload);
    });
    const auto warn = [&](const core::Layer& layer, const char* key, const QString& reason = {}) {
        auto message = i18n::text(key).arg(i18n::ltrToken(QString::fromStdString(layer.name)))
            .arg(i18n::ltrToken(QString::number(layer.id)));
        if (!reason.isEmpty()) message = message.arg(reason);
        const auto previous = shapeCacheWarnings_.find(layer.id);
        if (previous != shapeCacheWarnings_.end() && previous->second.revision == layer.shapeRevision
            && previous->second.transform == layer.localToDocument && previous->second.message == message) return;
        shapeCacheWarnings_.insert_or_assign(layer.id, ShapeCacheWarning{layer.shapeRevision, layer.localToDocument, message});
        statusBar()->showMessage(message, 5000);
    };
    constexpr std::size_t perCacheLimit = 16ULL * 1024 * 1024;
    // Budget the exact rounded bitmap sizes, rather than dividing a pool by
    // layer count. Effect/filter inputs reserve their native-resolution source
    // first. Only ordinary viewport previews may share a reduced allocation.
    // The resident generation stays within 64M RGBA pixels and the configured
    // document cache budget. Published old snapshots and rasterization scratch
    // may temporarily retain additional storage, as with other derived caches.
    const auto totalBudget = std::min<std::uint64_t>(64ULL * 1024 * 1024, performance_.documentCacheBytes() / 4);
    const auto pixels = [](core::Extent2u size) { return std::uint64_t(size.width) * size.height; };
    const auto cost = [&](const std::shared_ptr<const core::LayerRenderCache>& cache) {
        return cache && cache->surface ? pixels(cache->surface->extent()) : 0;
    };
    struct Plan {
        core::Layer* layer;
        bool filtered {}, documentGrid {}, reduced {};
        double requested {1};
        ShapeRasterSize size {{1, 1}, 1};
        std::size_t allocation {};
    };
    std::vector<Plan> plans;
    QtShapeRenderService service;
    const auto failure = [&](Plan& plan, const char* key, const QString& reason = {}) {
        auto& layer = *plan.layer;
        // Retain a usable previous cache only when it still fits its allocation.
        // Effects must never consume a document-grid or downsampled source.
        if (cost(layer.renderCache) > plan.allocation || (plan.filtered && layer.renderCache
            && (layer.renderCache->rasterizedDocumentTransform || layer.renderCache->density != 1)))
            layer.renderCache.reset();
        warn(layer, key, reason);
    };
    for (const auto& item : doc->layers()) {
        const auto* shape = std::get_if<core::ShapeLayer>(&item.payload);
        if (!shape)
            continue;
        Plan plan {doc->layer(item.id)};
        auto* layer = plan.layer;
        const auto& t = layer->localToDocument;
        const double scale = t.maximumScaleOver({0,0,std::max(1.0,shape->size.width),std::max(1.0,shape->size.height)});
        plan.filtered = core::hasActiveSpatialFilters(layer->filters)||core::hasActiveLayerEffects(layer->effects);
        const auto viewScale = canvasWindow_->zoom() * canvasWindow_->devicePixelRatio();
        plan.requested = plan.filtered ? 1.0 : QtShapeRenderService::densityForScale(scale * viewScale);
        const bool liveEdit = (shapeEdit_ && shapeEdit_->id == layer->id) || (shapeResize_ && shapeResize_->id == layer->id);
        plan.allocation = liveEdit && !plan.filtered ? std::min(perCacheLimit, std::size_t(1'000'000)) : perCacheLimit;
        plan.documentGrid = !plan.filtered && !liveEdit && viewScale <= 1;
        try {
            if (plan.documentGrid) {
                const bool current = layer->renderCache && layer->renderCache->contentRevision == layer->shapeRevision
                    && layer->renderCache->rasterizedDocumentTransform == t;
                const auto bounds = current ? core::RectD{} : service.documentBounds(*shape, t);
                const auto width = current ? double(layer->renderCache->surface->extent().width)
                    : std::max(1.0, std::ceil(bounds.right()) - std::floor(bounds.x));
                const auto height = current ? double(layer->renderCache->surface->extent().height)
                    : std::max(1.0, std::ceil(bounds.bottom()) - std::floor(bounds.y));
                plan.reduced = !std::isfinite(width) || !std::isfinite(height)
                    || width > 8192 || height > 8192 || width * height > double(plan.allocation);
                plan.documentGrid = !plan.reduced;
                if (plan.documentGrid) plan.size = {{std::uint32_t(width), std::uint32_t(height)}, 1};
            }
            if (!plan.documentGrid) {
                const auto& cache = layer->renderCache;
                if (cache && cache->contentRevision == layer->shapeRevision && !cache->rasterizedDocumentTransform
                    && cache->requestedDensity == plan.requested && cache->density == plan.requested
                    && cost(cache) <= plan.allocation)
                    plan.size = {cache->surface->extent(), cache->density};
                else plan.size = service.rasterSize({*shape, plan.requested,
                    plan.filtered ? std::optional(plan.allocation) : std::nullopt,
                    plan.filtered ? std::nullopt : std::optional(plan.allocation)});
                if (plan.size.extent.width > 8192 || plan.size.extent.height > 8192)
                    throw ShapeRasterLimitError("Shape source exceeds the preview dimension limit");
            }
            plans.push_back(plan);
        } catch (const ShapeRasterLimitError&) {
            plan.allocation = 0; failure(plan, "main.window.shape.preview.limit.named");
        } catch (const std::bad_alloc&) {
            plan.allocation = 0; failure(plan, "main.window.shape.preview.memory.named");
        } catch (const std::exception& error) {
            plan.allocation = 0; failure(plan, "main.window.shape.preview.failed.named", QString::fromUtf8(error.what()));
        }
    }
    std::uint64_t remaining = totalBudget, previewDemand = 0;
    for (auto& plan : plans) {
        const auto needed = pixels(plan.size.extent);
        if (!plan.filtered) { previewDemand += needed; continue; }
        plan.allocation = needed <= remaining ? std::size_t(needed) : 0;
        remaining -= plan.allocation;
    }
    for (auto& plan : plans) {
        auto* layer = plan.layer;
        const auto* shape = std::get_if<core::ShapeLayer>(&layer->payload);
        const auto& t = layer->localToDocument;
        if (!plan.filtered) {
            const auto desired = pixels(plan.size.extent);
            plan.allocation = std::size_t(previewDemand <= remaining ? desired : desired * remaining / previewDemand);
            if (plan.allocation < desired && plan.documentGrid) { plan.documentGrid = false; plan.reduced = true; }
        }
        if (!plan.allocation) { failure(plan, "main.window.shape.preview.limit.named"); continue; }
        try {
            if (!plan.filtered && !plan.documentGrid)
                plan.size = service.rasterSize({*shape, plan.requested, {}, plan.allocation});
            const bool contentChanged = !layer->renderCache || layer->renderCache->contentRevision != layer->shapeRevision;
            const bool modeChanged = layer->renderCache
                && layer->renderCache->rasterizedDocumentTransform.has_value() != plan.documentGrid;
            if (plan.documentGrid && !contentChanged && !canvasWindow_->transformDragging())
                if (auto moved = core::translatedDocumentRenderCache(layer->renderCache, t))
                    layer->renderCache = std::move(moved);
            const bool transformChanged = plan.documentGrid && !canvasWindow_->transformDragging() && layer->renderCache
                && layer->renderCache->rasterizedDocumentTransform != t;
            if (contentChanged || modeChanged || transformChanged
                || (layer->renderCache && cost(layer->renderCache) > plan.allocation)
                || (layer->renderCache && (layer->renderCache->requestedDensity != (plan.documentGrid ? 1.0 : plan.requested)
                    || (!plan.documentGrid && layer->renderCache->density != plan.size.density)) && !canvasWindow_->transformDragging())) {
                const auto source = plan.documentGrid ? service.renderDocument(*shape, t, plan.allocation)
                    : service.render({*shape, plan.requested, plan.filtered ? std::optional(plan.allocation) : std::nullopt,
                        plan.filtered ? std::nullopt : std::optional(plan.allocation)});
                auto cache = std::make_shared<core::LayerRenderCache>(*source);
                cache->contentRevision = layer->shapeRevision;
                layer->renderCache = std::move(cache);
            }
            if (plan.reduced) warn(*layer, "main.window.shape.preview.reduced.named");
            else shapeCacheWarnings_.erase(layer->id);
        } catch (const ShapeRasterLimitError&) {
            failure(plan, "main.window.shape.preview.limit.named");
        } catch (const std::bad_alloc&) {
            failure(plan, "main.window.shape.preview.memory.named");
        } catch (const std::exception& error) {
            failure(plan, "main.window.shape.preview.failed.named", QString::fromUtf8(error.what()));
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
            statusBar()->showMessage(i18n::text("main.window.shape.shape.preview.could.not.allocate.memory.creation.cancelled"), 5000);
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
                statusBar()->showMessage(i18n::text("main.window.shape.could.not.create.shape.no.layer.was.added"), 5000);
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
        statusBar()->showMessage(i18n::text("main.window.shape.shape.edit.could.not.be.recorded.original.style.restored"), 5000);
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
    i18n::setWindowTitle(dialog, fill ? i18n::text("main.window.shape.shape.fill.color") : i18n::text("main.window.shape.shape.stroke.color"));
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
