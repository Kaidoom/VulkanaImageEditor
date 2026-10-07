#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/core/SelectionRefinement.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QFormLayout>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <cstring>

namespace imageeditor::ui {
namespace {
using namespace core;
class RefinedMaskOutput final : public Command {
public:
  RefinedMaskOutput(std::unique_ptr<Command> operation,
                    LayerSelectionState selection, bool before)
      : operation_(std::move(operation)), selection_(std::move(selection)),
        before_(before) {}
  bool apply(Document &d) override { return operation_->apply(d); }
  bool undo(Document &d) override { return operation_->undo(d); }
  std::string_view label() const noexcept override {
    return "Refine Layer Mask";
  }
  std::size_t memoryCost() const noexcept override {
    return sizeof(*this) + operation_->memoryCost() +
           selection_.ids.capacity() * sizeof(LayerId);
  }
  const LayerSelectionState *
  layerSelectionAfter(bool undo) const noexcept override {
    if (auto *selection = operation_->layerSelectionAfter(undo))
      return selection;
    return &selection_;
  }
  std::optional<bool> maskEditingAfter(bool undo) const noexcept override {
    return undo ? before_ : true;
  }

private:
  std::unique_ptr<Command> operation_;
  LayerSelectionState selection_;
  bool before_;
};
struct Grid {
  RectI rect;
  Extent2u extent;
  double scale{1};
  AffineTransform toDocument() const {
    return {1 / scale, 0, double(rect.x), 0, 1 / scale, double(rect.y)};
  }
  AffineTransform fromDocument() const {
    return {scale, 0, -rect.x * scale, 0, scale, -rect.y * scale};
  }
};
void cancelled(const std::atomic_bool &cancel) {
  if (cancel)
    throw std::runtime_error("Cancelled");
}
RectI mappedBounds(const AffineTransform &t, RectD b) {
  if (!t.validOver(b))
    throw std::runtime_error("The mask mapping crosses a projective horizon.");
  double l = 1e30, r = -1e30, top = 1e30, bottom = -1e30;
  for (auto p : std::array<Vec2d, 4>{{{b.x, b.y},
                                      {b.right(), b.y},
                                      {b.x, b.bottom()},
                                      {b.right(), b.bottom()}}}) {
    p = t.map(p);
    l = std::min(l, p.x);
    r = std::max(r, p.x);
    top = std::min(top, p.y);
    bottom = std::max(bottom, p.y);
  }
  if (!std::isfinite(l) || !std::isfinite(r) || !std::isfinite(top) ||
      !std::isfinite(bottom) || l < -1e8 || top < -1e8 || r > 1e8 ||
      bottom > 1e8)
    throw std::runtime_error("The mask mapping exceeds supported bounds.");
  return {int(std::floor(l)), int(std::floor(top)),
          int(std::ceil(r) - std::floor(l)),
          int(std::ceil(bottom) - std::floor(top))};
}
Grid gridFor(const Document &doc, const Layer *layer, bool mask) {
  Grid g{
      {0, 0, int(doc.canvas().extent.width), int(doc.canvas().extent.height)},
      doc.canvas().extent,
      1};
  if (mask) {
    const auto e = layer->mask->coverage->extent();
    const auto toDoc = composeTransform(layer->localToDocument,
                                        *layer->mask->localToMask.inverted());
    auto maskBounds =
        mappedBounds(toDoc, {0, 0, double(e.width), double(e.height)});
    // Finite support for the largest offset + feather, beyond native storage.
    // Selection-only sessions extend the canvas edge instead, so Select All
    // does not acquire an invented background border.
    constexpr int support = 128;
    maskBounds = {maskBounds.x - support, maskBounds.y - support,
                  maskBounds.width + 2 * support,
                  maskBounds.height + 2 * support};
    g.rect = g.rect.united(maskBounds);
    // A document-aligned solve at no less than the native mask's density.
    // The homography Jacobian is bounded using its denominator extrema.
    const auto inv = *toDoc.inverted();
    double minimum = 1e30, maximumNumerator = 0;
    for (auto p : std::array<Vec2d, 4>{
             {{double(g.rect.x), double(g.rect.y)},
              {double(g.rect.right()), double(g.rect.y)},
              {double(g.rect.x), double(g.rect.bottom())},
              {double(g.rect.right()), double(g.rect.bottom())}}}) {
      const double d = inv.m20 * p.x + inv.m21 * p.y + inv.m22;
      minimum = std::min(minimum, std::abs(d));
      const double nx = inv.m00 * p.x + inv.m01 * p.y + inv.m02,
                   ny = inv.m10 * p.x + inv.m11 * p.y + inv.m12;
      maximumNumerator = std::max(
          maximumNumerator, std::sqrt(std::pow(inv.m00 * d - nx * inv.m20, 2) +
                                      std::pow(inv.m01 * d - nx * inv.m21, 2) +
                                      std::pow(inv.m10 * d - ny * inv.m20, 2) +
                                      std::pow(inv.m11 * d - ny * inv.m21, 2)));
    }
    if (!inv.validOver({double(g.rect.x), double(g.rect.y),
                        double(g.rect.width), double(g.rect.height)}))
      throw std::runtime_error(
          "The mask cannot be refined across this projective horizon.");
    // Spectral norm for affine matrices avoids gratuitous sqrt(2) identity
    // upsampling. The projective Frobenius bound is deliberately conservative.
    if (inv.isAffine()) {
      const double a = inv.m00 * inv.m00 + inv.m10 * inv.m10,
                   b = inv.m01 * inv.m01 + inv.m11 * inv.m11,
                   c = inv.m00 * inv.m01 + inv.m10 * inv.m11;
      g.scale =
          std::max(1., std::sqrt((a + b + std::hypot(a - b, 2 * c)) * .5));
    } else
      g.scale = std::max(1., maximumNumerator / (minimum * minimum));
    g.scale = std::ceil(g.scale * 4) / 4;
  }
  const double w = std::ceil(g.rect.width * g.scale),
               h = std::ceil(g.rect.height * g.scale);
  if (g.scale > 16 || w > 32768 || h > 32768 || w * h > 24. * 1024 * 1024)
    throw std::length_error("Refinement needs more than the supported working "
                            "memory at this mask resolution.");
  g.extent = {std::uint32_t(w), std::uint32_t(h)};
  return g;
}
// Sampling helper with no allocation in the pixel loop.
float sampleCoverage(const LayerMask &coverage, Vec2d p) {
  const auto e = coverage.coverage->extent();
  p = p - Vec2d{.5, .5};
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -1 || p.y < -1 ||
      p.x >= e.width || p.y >= e.height)
    return float(coverage.outside) / 255;
  const int x = int(std::floor(p.x)), y = int(std::floor(p.y));
  const float fx = float(p.x - x), fy = float(p.y - y);
  auto read = [&](int px, int py) {
    return float(px < 0 || py < 0 || px >= int(e.width) || py >= int(e.height)
                     ? coverage.outside
                     : coverage.coverage->coverageAtDocumentPixel(px, py)) /
           255;
  };
  return (read(x, y) * (1 - fx) + read(x + 1, y) * fx) * (1 - fy) +
         (read(x, y + 1) * (1 - fx) + read(x + 1, y + 1) * fx) * fy;
}
SelectionState inputFor(const Document &doc, LayerId id, bool mask,
                        const Grid &g, const std::atomic_bool &cancel) {
  if (!mask)
    return doc.selection();
  const auto &layer = *doc.layer(id);
  auto m = *layer.mask;
  m.enabled = true;
  const auto map = composeTransform(
      m.localToMask,
      composeTransform(*layer.localToDocument.inverted(), g.toDocument()));
  std::vector<std::uint8_t> bytes(std::size_t(g.extent.width) *
                                  g.extent.height);
  for (std::uint32_t y = 0; y < g.extent.height; ++y) {
    cancelled(cancel);
    for (std::uint32_t x = 0; x < g.extent.width; ++x)
      bytes[std::size_t(y) * g.extent.width + x] = std::uint8_t(std::clamp(
          std::lround(sampleCoverage(m, map.map({x + .5, y + .5})) * 255), 0L,
          255L));
  }
  return SelectionMask::fromR8(g.extent, bytes, g.extent.width);
}
SelectionState selectionFor(const SelectionState &coverage, const Grid &g,
                            Extent2u e, const std::atomic_bool &cancel) {
  if (g.scale == 1 && g.rect.x == 0 && g.rect.y == 0 && g.extent == e)
    return coverage;
  LayerMask sampler{coverage, {}, 0, true};
  std::vector<std::uint8_t> bytes(std::size_t(e.width) * e.height);
  const auto map = g.fromDocument();
  for (std::uint32_t y = 0; y < e.height; ++y) {
    cancelled(cancel);
    for (std::uint32_t x = 0; x < e.width; ++x)
      bytes[std::size_t(y) * e.width + x] = std::uint8_t(std::clamp(
          std::lround(sampleCoverage(sampler, map.map({x + .5, y + .5})) * 255),
          0L, 255L));
  }
  return SelectionMask::fromR8(e, bytes, e.width);
}
LayerMaskState outputMask(const Layer &layer, bool refiningMask,
                          const SelectionState &before,
                          const SelectionState &after, const Grid &grid,
                          const std::atomic_bool &cancel) {
  if (!refiningMask)
    return std::make_shared<const LayerMask>(LayerMask{
        after, composeTransform(grid.fromDocument(), layer.localToDocument), 0,
        true});
  if (before == after || before->equivalent(*after))
    return layer.mask;
  // Transfer only the coverage delta. Unedited native samples, hidden RGB,
  // mask origin and retained off-canvas coverage stay byte-exact.
  RectI changed;
  for (std::uint32_t y = 0; y < grid.extent.height; ++y) {
    cancelled(cancel);
    int left = int(grid.extent.width), right = -1;
    for (std::uint32_t x = 0; x < grid.extent.width; ++x)
      if (before->coverageAtDocumentPixel(int(x), int(y)) !=
          after->coverageAtDocumentPixel(int(x), int(y))) {
        left = std::min(left, int(x));
        right = int(x);
      }
    if (right >= left)
      changed = changed.united({left, int(y), right - left + 1, 1});
  }
  if (changed.empty())
    return layer.mask;
  const auto toMask = composeTransform(layer.mask->localToMask,
                                       *layer.localToDocument.inverted());
  auto affected =
      mappedBounds(composeTransform(toMask, grid.toDocument()),
                   {double(changed.x) - 1, double(changed.y) - 1,
                    double(changed.width) + 2, double(changed.height) + 2});
  const auto old = layer.mask->coverage->extent();
  const auto frame =
      RectI{0, 0, int(old.width), int(old.height)}.united(affected);
  if (frame.width > 32768 || frame.height > 32768 ||
      std::uint64_t(frame.width) * std::uint64_t(frame.height) >
          64ULL * 1024 * 1024)
    throw std::length_error("Refined mask exceeds native mask storage limits.");
  const auto maskToGrid =
      composeTransform(grid.fromDocument(), *toMask.inverted());
  LayerMask b{before, {}, layer.mask->outside, true},
      a{after, {}, layer.mask->outside, true};
  std::vector<std::uint8_t> bytes(std::size_t(frame.width) *
                                  std::size_t(frame.height));
  bool differs = false;
  for (int y = 0; y < frame.height; ++y) {
    cancelled(cancel);
    for (int x = 0; x < frame.width; ++x) {
      const int mx = x + frame.x, my = y + frame.y;
      const auto base =
          mx < 0 || my < 0 || mx >= int(old.width) || my >= int(old.height)
              ? layer.mask->outside
              : layer.mask->coverage->coverageAtDocumentPixel(mx, my);
      const auto p = maskToGrid.map({mx + .5, my + .5});
      const auto value = std::uint8_t(
          std::clamp(std::lround(base + 255 * (sampleCoverage(a, p) -
                                               sampleCoverage(b, p))),
                     0L, 255L));
      bytes[std::size_t(y) * std::size_t(frame.width) + std::size_t(x)] = value;
      differs |= base != value;
    }
  }
  if (!differs)
    return layer.mask;
  auto mask = std::make_shared<LayerMask>(*layer.mask);
  if (frame == RectI{0, 0, int(old.width), int(old.height)}) {
    const CoveragePatch patch{frame, bytes, std::size_t(frame.width)};
    mask->coverage = layer.mask->coverage->replacedR8(std::span(&patch, 1));
  } else
    mask->coverage = SelectionMask::fromR8(
        {std::uint32_t(frame.width), std::uint32_t(frame.height)}, bytes,
        std::size_t(frame.width));
  mask->localToMask = composeTransform(
      {1, 0, -double(frame.x), 0, 1, -double(frame.y)}, mask->localToMask);
  return mask;
}
std::shared_ptr<const RasterSurface> surfaceFor(const QImage &image) {
  const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
  std::vector<std::byte> bytes(std::size_t(rgba.width()) *
                               std::size_t(rgba.height()) * 4);
  for (int y = 0; y < rgba.height(); ++y)
    std::memcpy(bytes.data() + std::size_t(y) * std::size_t(rgba.width()) * 4,
                rgba.constScanLine(y), std::size_t(rgba.width()) * 4);
  return std::make_shared<ContiguousRasterSurface>(
      Extent2u{std::uint32_t(rgba.width()), std::uint32_t(rgba.height())},
      std::move(bytes));
}
} // namespace
struct RefinementWorkspace {
  DocumentInstanceId instance{};
  core::Document *owner{};
  core::Revision revision{}, selectionRevision{};
  std::shared_ptr<const core::Document> original;
  core::LayerId target{};
  core::LayerId previewCopyId{core::makeLayerId()};
  std::shared_ptr<const core::RasterSurface> tintSurface =
      std::make_shared<core::ContiguousRasterSurface>(
          core::Extent2u{1, 1}, core::Rgba8{240, 55, 70, 255});
  bool mask{}, oldMaskEditing{}, originalHeld{}, dirty{}, failed{}, closing{},
      cancellingGesture{};
  core::ToolId oldTool{};
  core::BrushSettings brush;
  Grid grid;
  core::RefinementState state;
  std::vector<core::RefinementState> history{{}};
  std::size_t checkpoint{};
  std::optional<core::RefinementState> gesture;
  core::RefinementPath path;
  core::RefinementBrush brushMode{core::RefinementBrush::Edge};
  QFrame *panel{};
  QTimer *timer{};
  QLabel *status{};
  QPushButton *apply{};
  QComboBox *view{};
  QComboBox *source{};
  QComboBox *output{};
  QCheckBox *region{};
  CompactValueControl *overlayOpacity{};
  CompactValueControl *brushSize{};
  std::array<CompactValueControl *, 5> settings{};
  std::vector<std::pair<QPointer<QWidget>, bool>> disabled;
  std::vector<std::pair<QPointer<QAction>, bool>> actions;
  core::Rgba8 background, light, dark;
  std::uint64_t generation{}, runningGeneration{};
  std::shared_ptr<std::atomic_bool> cancel;
  struct Result {
    core::SelectionState input, selection, inverseInput, inverseCoverage;
    core::RefinementResult refined;
    std::shared_ptr<const core::SmartReferenceImage> reference;
    std::shared_ptr<const core::RasterSurface> surface, grayscale,
        originalGrayscale;
    core::LayerMaskState mask;
    std::optional<core::LayerSnapshot> previewLayer;
    std::shared_ptr<const core::LayerRenderCache> typedSource;
    std::shared_ptr<const core::LayerTransfer> duplicate;
    QString error, referenceError;
    double preparationMs{};
  };
  Result result;
  std::future<Result> worker;
  ~RefinementWorkspace() {
    if (cancel)
      *cancel = true;
    if (worker.valid())
      worker.wait();
  }
  bool current(const MainWindow &w) const {
    return w.activeDocumentId() == instance &&
           w.editorSession().document() == owner &&
           owner->revision() == revision &&
           owner->selectionRevision() == selectionRevision &&
           (!target || owner->layer(target));
  }
  void checkpointState() {
    if (history[checkpoint] == state)
      return;
    history.resize(checkpoint + 1);
    history.push_back(state);
    ++checkpoint;
    // Like document history, local gestures have a bounded retention budget.
    // Source pixels/coverage and dab arrays remain shared, never duplicated.
    std::size_t bytes = 0;
    for (const auto &item : history)
      bytes += sizeof(item) +
               item.strokes.capacity() * sizeof(core::RefinementStrokeState);
    while (bytes > 64ULL * 1024 * 1024 && checkpoint > 0) {
      bytes -=
          sizeof(history.front()) + history.front().strokes.capacity() *
                                        sizeof(core::RefinementStrokeState);
      history.erase(history.begin());
      --checkpoint;
    }
  }
};

void MainWindow::refreshRefinementActions() {
  if (!refineSelectionAction_)
    return;
  const auto *doc = session().document();
  const auto *layer = doc && session().activeLayer()
                          ? doc->layer(*session().activeLayer())
                          : nullptr;
  const bool ready = doc && !refinement_ && !fileBusy_ && !workspaceDialog_ &&
                     !layerTransform_ && !selectionTransform_ && !layerCrop_ &&
                     shortcutGestureIdle(true);
  refineSelectionAction_->setEnabled(ready && doc->selection() &&
                                     !doc->selection()->bounds().empty());
  refineMaskAction_->setEnabled(ready && layer && layer->mask);
}
void MainWindow::beginRefinement(bool mask) {
  if (refinement_ || fileBusy_ || workspaceDialog_ || !session().document() ||
      !settleForFileOperation())
    return;
  auto *doc = session().document();
  const auto *layer =
      session().activeLayer() ? doc->layer(*session().activeLayer()) : nullptr;
  if (mask ? (!layer || !layer->mask)
           : (!doc->selection() || doc->selection()->bounds().empty()))
    return;
  try {
    auto r = std::make_shared<RefinementWorkspace>();
    r->grid = gridFor(*doc, layer, mask);
    r->instance = activeDocumentId();
    r->owner = doc;
    r->revision = doc->revision();
    r->selectionRevision = doc->selectionRevision();
    r->target = layer ? layer->id : 0;
    r->mask = mask;
    r->oldTool = session().activeTool();
    r->oldMaskEditing = session().editingLayerMask();
    r->brush = brushSettings_;
    r->brush.tip = {};
    r->brush.grain = {};
    r->brush.flow = 1; // Strength is the visible ceiling; tablet pressure still
                       // modulates it.
    r->original = std::make_shared<const core::Document>(*doc);
    r->background = canvasWindow_->scene().canvasBackground;
    r->light = canvasWindow_->scene().checkerLight;
    r->dark = canvasWindow_->scene().checkerDark;
    setActiveTool(core::ToolId::Brush);
    refinement_ = r;
    // Keep the native canvas interactive. Only the existing panel plane's
    // other controls are gated; no second window or modal canvas blocker.
    for (auto *child : workspace_->panelOverlay()->findChildren<QWidget *>(
             QString{}, Qt::FindDirectChildrenOnly)) {
      r->disabled.push_back({child, child->isEnabled()});
      child->setEnabled(false);
    }
    for (auto *action : findChildren<QAction *>()) {
      if (action == undoAction_ || action == redoAction_)
        continue;
      r->actions.push_back({action, action->isEnabled()});
      action->setEnabled(false);
    }
    r->panel = new QFrame(workspace_->panelOverlay());
    r->panel->setObjectName("RefineSelectionWorkspace");
    r->panel->setAttribute(Qt::WA_StyledBackground);
    r->panel->setStyleSheet(
        QStringLiteral("QFrame#RefineSelectionWorkspace { background: %1; "
                       "border: 1px solid %2; border-radius: 6px; }")
            .arg(themeColor(ThemeColor::Surface).name(),
                 themeColor(ThemeColor::Border).name()));
    auto *outer = new QVBoxLayout(r->panel);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(6);
    auto *title =
        new QLabel(mask ? tr("Refine Mask") : tr("Refine Selection"), r->panel);
    outer->addWidget(title);
    auto *target = new QLabel(
        mask ? tr("Mask: %1").arg(QString::fromStdString(layer->name))
             : tr("Selection"),
        r->panel);
    target->setTextFormat(Qt::PlainText);
    target->setWordWrap(true);
    outer->addWidget(target);
    auto *scroll = new QScrollArea(r->panel);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *form = new QFormLayout(content);
    form->setContentsMargins(0, 0, 4, 0);
    form->setSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto combo = [&](const QString &label, const QStringList &items,
                     const char *name) {
      auto *c = new QComboBox(content);
      c->setObjectName(name);
      c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
      c->setMinimumContentsLength(8);
      c->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
      c->addItems(items);
      form->addRow(label, c);
      return c;
    };
    r->view = combo(tr("View"),
                    {tr("Overlay"), tr("On Black"), tr("On White"),
                     tr("Transparency"), tr("Grayscale Mask")},
                    "RefineView");
    auto number = [&](const QString &label, double low, double high,
                      double value, int decimals, const char *name) {
      auto *c = new CompactValueControl(content);
      c->setObjectName(name);
      c->setRange(low, high);
      c->setDecimals(decimals);
      c->setValue(value);
      form->addRow(label, c);
      return c;
    };
    r->overlayOpacity =
        number(tr("Overlay opacity"), 0, 100, 50, 0, "RefineOverlayOpacity");
    auto *original = new QPushButton(tr("Hold for Original"), content);
    form->addRow(original);
    connect(original, &QPushButton::pressed, this, [this] {
      if (refinement_) {
        refinement_->originalHeld = true;
        publishRefinementPreview();
      }
    });
    connect(original, &QPushButton::released, this, [this] {
      if (refinement_) {
        refinement_->originalHeld = false;
        publishRefinementPreview();
      }
    });
    r->region = new QCheckBox(tr("Show Refinement Region"), content);
    form->addRow(r->region);
    r->source =
        combo(tr("Reference"), {tr("Merged Visible"), tr("Active Layer")},
              "RefineReference");
    r->source->setToolTip(
        tr("Merged Visible uses the visible composition. Active Layer uses the "
           "layer in isolation, without its visibility, opacity or blend mode. "
           "Refine Mask bypasses only the target mask in this reference."));
    if (layer &&
        !std::holds_alternative<core::AdjustmentLayer>(layer->payload) && mask)
      r->source->setCurrentIndex(1);
    if (!layer || std::holds_alternative<core::AdjustmentLayer>(layer->payload))
      r->source->setItemData(1, 0, Qt::UserRole - 1);
    form->addRow(new QLabel(tr("Edge Refinement"), content));
    r->settings[0] = number(tr("Radius (px)"), 0, 64, 0, 1, "RefineRadius");
    form->addRow(new QLabel(tr("Global Refinement"), content));
    r->settings[1] = number(tr("Smooth"), 0, 12, 0, 1, "RefineSmooth");
    r->settings[2] = number(tr("Feather (px)"), 0, 64, 0, 1, "RefineFeather");
    r->settings[3] = number(tr("Contrast (%)"), 0, 100, 0, 1, "RefineContrast");
    r->settings[4] =
        number(tr("Shift Edge (px)"), -64, 64, 0, 1, "RefineShift");
    auto *brush =
        combo(tr("Brush"),
              {tr("Refine Edge"), tr("Add Coverage"), tr("Subtract Coverage")},
              "RefineBrush");
    auto *size = number(tr("Size (px)"), 1, 2000, r->brush.sizePixels, 1,
                        "RefineBrushSize");
    r->brushSize = size;
    auto *hardness = number(tr("Hardness (%)"), 0, 100, r->brush.hardness * 100,
                            0, "RefineBrushHardness");
    auto *strength = number(tr("Strength (%)"), 1, 100, r->brush.opacity * 100,
                            0, "RefineBrushStrength");
    r->output = new QComboBox(r->panel);
    r->output->setObjectName("RefineOutput");
    r->output->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    r->output->setMinimumContentsLength(8);
    r->output->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    r->output->addItem(tr("Selection"));
    if (layer) {
      r->output->addItem(
          layer->mask
              ? tr("Replace Layer Mask — %1")
                    .arg(QString::fromStdString(layer->name))
              : tr("Layer Mask — %1").arg(QString::fromStdString(layer->name)));
      r->output->addItem(tr("New Layer with Mask — original kept"));
    }
    r->output->setCurrentIndex(mask ? 1 : 0);
    auto *hint = new QLabel(
        tr("Refine Edge analyzes the brushed region. Add/Subtract corrections "
           "are applied last. Source colors stay unchanged."),
        content);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto *outputRow = new QHBoxLayout;
    outputRow->addWidget(new QLabel(tr("Output"), r->panel));
    outputRow->addWidget(r->output, 1);
    outer->addLayout(outputRow);
    r->status = new QLabel(tr("Preparing reference…"), r->panel);
    r->status->setTextFormat(Qt::PlainText);
    r->status->setObjectName("RefineStatus");
    r->status->setWordWrap(true);
    r->status->setMinimumHeight(38);
    outer->addWidget(r->status);
    auto *row = new QHBoxLayout;
    auto *reset = new QPushButton(tr("Reset"), r->panel);
    auto *cancel = new QPushButton(tr("Cancel"), r->panel);
    r->apply = new QPushButton(tr("Apply"), r->panel);
    r->apply->setObjectName("RefineApply");
    cancel->setObjectName("RefineCancel");
    reset->setObjectName("RefineReset");
    row->addWidget(reset);
    row->addStretch();
    row->addWidget(cancel);
    row->addWidget(r->apply);
    outer->addLayout(row);
    r->timer = new QTimer(r->panel);
    r->timer->setSingleShot(true);
    connect(r->timer, &QTimer::timeout, this, &MainWindow::advanceRefinement);
    for (auto *control : r->settings) {
      control->onInteractionStarted = [this] {
        if (refinement_)
          refinement_->gesture = refinement_->state;
      };
      control->onInteractionFinished = [this] {
        if (refinement_ && !refinement_->cancellingGesture) {
          refinement_->checkpointState();
          refinement_->gesture.reset();
          requestRefinement();
        }
      };
      connect(control, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
              [this] {
                if (!refinement_)
                  return;
                auto &r = *refinement_;
                r.state.settings = {
                    r.settings[0]->value(), r.settings[1]->value(),
                    r.settings[2]->value(), r.settings[3]->value() / 100,
                    r.settings[4]->value()};
                if (!r.gesture)
                  r.checkpointState();
                requestRefinement();
              });
    }
    connect(size, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
              if (refinement_) {
                refinement_->brush.sizePixels = v;
                canvasWindow_->setBrushCursor(refinement_->brush);
              }
            });
    connect(hardness, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
              if (refinement_)
                refinement_->brush.hardness = v / 100;
            });
    connect(strength, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double v) {
              if (refinement_)
                refinement_->brush.opacity = v / 100;
            });
    connect(brush, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int i) {
              if (refinement_)
                refinement_->brushMode = core::RefinementBrush(i);
            });
    connect(r->source, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] {
              if (!refinement_)
                return;
              refinement_->result.reference.reset();
              refinement_->result.surface.reset();
              refinement_->result.referenceError.clear();
              requestRefinement();
            });
    connect(r->view, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::publishRefinementPreview);
    connect(r->overlayOpacity, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::publishRefinementPreview);
    connect(r->region, &QCheckBox::toggled, this,
            &MainWindow::publishRefinementPreview);
    connect(r->output, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::requestRefinement);
    connect(cancel, &QPushButton::clicked, this,
            [this] { finishRefinement(false); });
    connect(r->apply, &QPushButton::clicked, this,
            [this] { finishRefinement(true); });
    connect(reset, &QPushButton::clicked, this, [this] {
      if (!refinement_)
        return;
      auto &r = *refinement_;
      r.state = {};
      r.checkpointState();
      for (auto *c : r.settings) {
        QSignalBlocker guard(c);
        c->setValue(0);
      }
      requestRefinement();
    });
    r->panel->setGeometry(
        std::max(8, workspace_->panelOverlay()->width() - 328), 12, 316,
        std::max(240,
                 std::min(740, workspace_->panelOverlay()->height() - 24)));
    r->panel->show();
    r->panel->raise();
    workspace_->setContextOverlayInteractionRegion(r->panel->geometry());
    canvasWindow_->setBrushCursor(r->brush);
    canvasWindow_->setTransformOverlay({});
    canvasWindow_->setPixelPreview(false);
    pixelPreview_->setEnabled(false);
    requestRefinement();
    publishRefinementPreview();
  } catch (const std::exception &e) {
    finishRefinement(false);
    statusBar()->showMessage(
        tr("Cannot refine: %1").arg(QString::fromUtf8(e.what())), 6000);
  }
}
void MainWindow::requestRefinement() {
  if (!refinement_ || refinement_->closing)
    return;
  auto &r = *refinement_;
  ++r.generation;
  r.dirty = true;
  r.failed = false;
  if (r.cancel)
    *r.cancel = true;
  r.apply->setEnabled(false);
  r.status->setText(tr("Refining…"));
  r.timer->start(30);
  undoAction_->setEnabled(r.checkpoint > 0);
  redoAction_->setEnabled(r.checkpoint + 1 < r.history.size());
  undoAction_->setText(tr("Undo refinement"));
  redoAction_->setText(tr("Redo refinement"));
}
void MainWindow::advanceRefinement() {
  if (!refinement_)
    return;
  auto &r = *refinement_;
  if (r.closing) {
    finishRefinement(false);
    return;
  }
  if (!r.current(*this)) {
    finishRefinement(false);
    statusBar()->showMessage(
        tr("Refinement cancelled because its target changed."), 5000);
    return;
  }
  if (r.worker.valid()) {
    if (r.worker.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
      r.timer->start(12);
      return;
    }
    auto result = r.worker.get();
    if (r.runningGeneration == r.generation && !result.refined.cancelled) {
      if (!result.error.isEmpty()) {
        r.failed = true;
        r.status->setText(result.error);
      } else {
        r.result = std::move(result);
        r.apply->setEnabled(bool(r.result.refined.coverage) &&
                            !r.path.active());
        const auto &stats = r.result.refined;
        r.status->setText(
            !r.result.referenceError.isEmpty()
                ? tr("Global refinements available. Reference: %1")
                      .arg(r.result.referenceError)
            : stats.unresolvedPixels
                ? tr("Ready · limited color evidence in %1 boundary pixels; "
                     "original coverage retained there.")
                      .arg(qulonglong(stats.unresolvedPixels))
                : tr("Ready"));
        r.status->setToolTip(
            tr("Reference %1 ms · analysis %2 ms · global %3 ms · corrections "
               "%4 ms · working limit %5 MiB")
                .arg(r.result.preparationMs, 0, 'f', 1)
                .arg(stats.analysisMs, 0, 'f', 1)
                .arg(stats.globalMs, 0, 'f', 1)
                .arg(stats.correctionMs, 0, 'f', 1)
                .arg(double(stats.workingBytes) / (1024 * 1024), 0, 'f', 1));
        publishRefinementPreview();
      }
    }
  }
  if (!r.dirty)
    return;
  r.dirty = false;
  r.runningGeneration = r.generation;
  r.cancel = std::make_shared<std::atomic_bool>(false);
  auto state = r.state;
  if (r.path.active())
    state.strokes.push_back(r.path.stroke());
  const auto doc = r.original;
  const auto target = r.target;
  const bool mask = r.mask;
  const auto grid = r.grid;
  const auto cancel = r.cancel;
  const int source = r.source->currentIndex();
  const int output = r.output->currentIndex();
  const auto retained = r.result;
  r.worker = std::async(std::launch::async, [doc, target, mask, grid, cancel,
                                             state = std::move(state), source,
                                             output, retained]() mutable {
    RefinementWorkspace::Result result;
    try {
      const auto start = std::chrono::steady_clock::now();
      result.input = retained.input
                         ? retained.input
                         : inputFor(*doc, target, mask, grid, *cancel);
      result.inverseInput = retained.inverseInput ? retained.inverseInput
                                                  : result.input->inverted();
      result.reference = retained.reference;
      result.surface = retained.surface;
      result.referenceError = retained.referenceError;
      if (!result.reference && result.referenceError.isEmpty()) {
        auto reference = *doc;
        if (mask) {
          auto *l = reference.layer(target);
          l->mask.reset();
          l->effectCache.reset();
        }
        if (source == 1) {
          core::Document single(reference.canvas());
          auto l = *reference.layer(target);
          l.visible = true;
          l.opacity = 1;
          l.blendMode = core::BlendMode::Normal;
          single.insertLayer(0, std::move(l));
          reference = std::move(single);
        }
        auto image =
            flattenDocumentRegion(reference, grid.rect, grid.extent,
                                  [&](auto, auto) { return !*cancel; });
        cancelled(*cancel);
        if (image) {
          auto rgba = image.image.convertToFormat(QImage::Format_RGBA8888);
          auto pixels = std::make_shared<core::SmartReferenceImage>();
          pixels->extent = grid.extent;
          const auto n = std::size_t(grid.extent.width) * grid.extent.height;
          pixels->pixels.resize(n);
          pixels->valid.resize(n);
          for (std::uint32_t y = 0; y < grid.extent.height; ++y) {
            cancelled(*cancel);
            std::memcpy(
                pixels->pixels.data() + std::size_t(y) * grid.extent.width,
                rgba.constScanLine(int(y)), std::size_t(grid.extent.width) * 4);
          }
          for (std::size_t i = 0; i < n; ++i) {
            pixels->valid[i] = pixels->pixels[i].alpha > 0;
            if (!pixels->valid[i])
              pixels->pixels[i] = {};
          }
          result.reference = std::move(pixels);
          result.surface = surfaceFor(rgba);
        } else
          result.referenceError = image.error;
      }
      result.preparationMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
      core::RefinementOptions options;
      options.origin = {double(grid.rect.x), double(grid.rect.y)};
      options.pixelsPerDocumentPixel = grid.scale;
      options.cancelled = [cancel] { return bool(*cancel); };
      options.referenceOwner = result.reference;
      options.analysis = retained.refined.analysis;
      result.refined = core::refineSelection(
          result.input, result.reference.get(), state, options);
      cancelled(*cancel);
      if (!result.refined.coverage)
        throw std::runtime_error("Refinement did not complete.");
      result.inverseCoverage = result.refined.coverage == result.input
                                   ? result.inverseInput
                                   : result.refined.coverage->inverted();
      result.selection = selectionFor(result.refined.coverage, grid,
                                      doc->canvas().extent, *cancel);
      if (target && output != 0) {
        result.mask = outputMask(*doc->layer(target), mask, result.input,
                                 result.refined.coverage, grid, *cancel);
        auto layer = *doc->layer(target);
        layer.mask = result.mask;
        if (!std::holds_alternative<core::RasterLayer>(layer.payload) &&
            !std::holds_alternative<core::AdjustmentLayer>(layer.payload)) {
          result.typedSource =
              retained.typedSource
                  ? retained.typedSource
                  : prepareDocumentSampleCache(layer, 64ULL * 1024 * 1024);
          layer.renderCache = result.typedSource;
        }
        core::FilterPreparationOptions preparation;
        preparation.cancelled = [cancel] { return bool(*cancel); };
        layer = core::prepareSpatialFilterLayer(layer, preparation);
        core::Document temporary(doc->canvas());
        temporary.insertLayer(0, std::move(layer));
        result.previewLayer = temporary.snapshot().layersBottomToTop.front();
      }
      result.duplicate = retained.duplicate;
      if (output == 2 && !result.duplicate) {
        cancelled(*cancel);
        const std::array ids{target};
        result.duplicate = std::make_shared<const core::LayerTransfer>(
            core::captureLayerTransfer(*doc, ids));
        cancelled(*cancel);
      }
      // Display-only grayscale raster; the color reference remains cached
      // and is never uploaded again when only coverage/settings change.
      std::vector<std::byte> gray(std::size_t(grid.extent.width) *
                                  grid.extent.height * 4);
      for (std::uint32_t y = 0; y < grid.extent.height; ++y) {
        cancelled(*cancel);
        for (std::uint32_t x = 0; x < grid.extent.width; ++x) {
          const auto i = (std::size_t(y) * grid.extent.width + x) * 4;
          const auto v =
              result.refined.coverage->coverageAtDocumentPixel(int(x), int(y));
          gray[i] = gray[i + 1] = gray[i + 2] = std::byte(v);
          gray[i + 3] = std::byte{255};
        }
      }
      result.grayscale = std::make_shared<core::ContiguousRasterSurface>(
          grid.extent, std::move(gray));
      result.originalGrayscale = retained.originalGrayscale;
      if (!result.originalGrayscale) {
        if (result.refined.coverage == result.input)
          result.originalGrayscale = result.grayscale;
        else {
          std::vector<std::byte> originalGray(std::size_t(grid.extent.width) *
                                              grid.extent.height * 4);
          for (std::uint32_t y = 0; y < grid.extent.height; ++y) {
            cancelled(*cancel);
            for (std::uint32_t x = 0; x < grid.extent.width; ++x) {
              const auto i = (std::size_t(y) * grid.extent.width + x) * 4;
              const auto v =
                  result.input->coverageAtDocumentPixel(int(x), int(y));
              originalGray[i] = originalGray[i + 1] = originalGray[i + 2] =
                  std::byte(v);
              originalGray[i + 3] = std::byte{255};
            }
          }
          result.originalGrayscale =
              std::make_shared<core::ContiguousRasterSurface>(
                  grid.extent, std::move(originalGray));
        }
      }
    } catch (const std::exception &e) {
      if (*cancel)
        result.refined.cancelled = true;
      else
        result.error = QString::fromUtf8(e.what());
    }
    return result;
  });
  r.timer->start(12);
}
void MainWindow::prepareRefinementSnapshot(core::DocumentSnapshot &snapshot) {
  if (!refinement_)
    return;
  const auto &r = *refinement_;
  if (!r.current(*this))
    return;
  snapshot.selection.reset();
  snapshot.selectionRevision = 0;
  if (!r.result.input)
    return;
  const auto coverage =
      r.originalHeld ? r.result.input : r.result.refined.coverage;
  if (!coverage)
    return;
  const int view = r.view->currentIndex();
  // Existing mask pipeline, replacing rather than multiplying the old mask.
  if (r.output->currentIndex() != 0 && view != 4) {
    if (!r.originalHeld && r.result.previewLayer) {
      if (r.output->currentIndex() == 2) {
        auto copy = *r.result.previewLayer;
        copy.id = r.previewCopyId;
        auto placement = *snapshot.tree.placement(r.target);
        auto *siblings = snapshot.tree.children(placement.parent);
        siblings->insert(
            siblings->begin() + std::ptrdiff_t(placement.index + 1), copy.id);
        auto where =
            std::find_if(snapshot.layersBottomToTop.begin(),
                         snapshot.layersBottomToTop.end(),
                         [&](const auto &l) { return l.id == r.target; });
        snapshot.layersBottomToTop.insert(where + 1, std::move(copy));
      } else
        for (auto &layer : snapshot.layersBottomToTop)
          if (layer.id == r.target)
            layer = *r.result.previewLayer;
    }
  } else {
    core::LayerSnapshot layer;
    layer.id = r.target ? r.target : 1;
    layer.localToDocument = r.grid.toDocument();
    layer.payload = core::RasterLayerSnapshot{
        view == 4
            ? (r.originalHeld ? r.result.originalGrayscale : r.result.grayscale)
            : r.result.surface};
    if (!std::get<core::RasterLayerSnapshot>(layer.payload).surface)
      return;
    if (view != 0 && view != 4)
      layer.mask = std::make_shared<const core::LayerMask>(
          core::LayerMask{coverage, {}, 0, true});
    snapshot.layersBottomToTop = {layer};
    snapshot.tree = {};
    snapshot.tree.roots = {layer.id};
  }
  if (view == 0) {
    core::LayerSnapshot tint;
    tint.id = std::numeric_limits<core::LayerId>::max();
    tint.opacity = float(r.overlayOpacity->value() / 100);
    tint.localToDocument = core::composeTransform(
        r.grid.toDocument(), {double(r.grid.extent.width), 0, 0, 0,
                              double(r.grid.extent.height), 0});
    tint.payload = core::RasterLayerSnapshot{r.tintSurface};
    tint.mask = std::make_shared<const core::LayerMask>(core::LayerMask{
        r.originalHeld ? r.result.inverseInput : r.result.inverseCoverage,
        {double(r.grid.extent.width), 0, 0, 0, double(r.grid.extent.height), 0},
        255,
        true});
    snapshot.layersBottomToTop.push_back(std::move(tint));
    snapshot.tree.roots.push_back(snapshot.layersBottomToTop.back().id);
  }
}
void MainWindow::publishRefinementPreview() {
  if (!refinement_)
    return;
  auto &r = *refinement_;
  const int view = r.view->currentIndex();
  r.overlayOpacity->setEnabled(view == 0);
  const auto c = view == 1 || view == 4 ? core::Rgba8{0, 0, 0, 255}
                                        : core::Rgba8{255, 255, 255, 255};
  canvasWindow_->setCanvasColors(
      r.background, view == 1 || view == 2 || view == 4 ? c : r.light,
      view == 1 || view == 2 || view == 4 ? c : r.dark);
  canvasWindow_->setDocument(session().document()->snapshot(), false);
  if (r.region->isChecked() && r.result.refined.region) {
    auto edges = std::make_shared<std::vector<core::SelectionEdge>>(
        r.result.refined.region->nonzeroBoundaryEdges());
    const auto map = r.grid.toDocument();
    for (auto &edge : *edges) {
      edge.from = map.map(edge.from);
      edge.to = map.map(edge.to);
    }
    canvasWindow_->setRepairRegion(edges);
  } else
    canvasWindow_->setRepairRegion({});
}
bool MainWindow::refinementBrush(const core::NormalizedPointerSample &sample,
                                 int phase) {
  if (!refinement_)
    return false;
  auto &r = *refinement_;
  if (phase == 3) {
    r.path.cancel();
    requestRefinement();
    return true;
  }
  try {
    if (phase == 0) {
      for (auto *c : r.settings)
        c->finishEditing();
      r.path.begin(r.brushMode, r.brush, sample);
    } else if (!r.path.active())
      return false;
    else if (phase == 1)
      r.path.append(sample);
    else {
      r.path.end(sample);
      std::size_t count = r.path.stroke()->dabs.size();
      for (const auto &stroke : r.state.strokes)
        count += stroke->dabs.size();
      if (r.state.strokes.size() >= 4096 || count > 262144)
        throw std::length_error(
            "Refinement stroke history exceeds its memory limit.");
      r.state.strokes.push_back(r.path.stroke());
      r.checkpointState();
    }
  } catch (const std::exception &e) {
    r.path.cancel();
    statusBar()->showMessage(
        tr("Stroke cancelled: %1").arg(QString::fromUtf8(e.what())), 6000);
  }
  requestRefinement();
  return true;
}
void MainWindow::stepRefinementHistory(bool redo) {
  if (!refinement_)
    return;
  auto &r = *refinement_;
  if (r.path.active()) {
    r.path.cancel();
    requestRefinement();
    return;
  }
  for (auto *c : r.settings)
    c->finishEditing();
  if (redo ? r.checkpoint + 1 >= r.history.size() : r.checkpoint == 0)
    return;
  if (redo)
    ++r.checkpoint;
  else
    --r.checkpoint;
  r.state = r.history[r.checkpoint];
  const auto &s = r.state.settings;
  const std::array values{s.radius, s.smooth, s.feather, s.contrast * 100,
                          s.shift};
  for (std::size_t i = 0; i < values.size(); ++i) {
    QSignalBlocker guard(r.settings[i]);
    r.settings[i]->setValue(values[i]);
  }
  requestRefinement();
}
void MainWindow::finishRefinement(bool apply, bool wait) {
  if (!refinement_)
    return;
  auto r = refinement_;
  if (!apply && r->worker.valid()) {
    *r->cancel = true;
    if (wait)
      r->worker.wait();
    else if (r->worker.wait_for(std::chrono::milliseconds(0)) !=
             std::future_status::ready) {
      // Keep exactly one owned job until it cooperatively drains. UI and pan/
      // zoom keep servicing events; no destructor waits on the UI cancel path.
      r->closing = true;
      r->dirty = false;
      r->apply->setEnabled(false);
      r->status->setText(tr("Cancelling…"));
      r->timer->start(12);
      return;
    }
  }
  if (apply && r->closing)
    return;
  if (apply) {
    for (auto *c : r->settings)
      c->finishEditing();
    if (!r->current(*this) || r->dirty || r->worker.valid() || r->failed ||
        r->path.active() || !r->result.refined.coverage) {
      r->status->setText(tr("Wait for refinement to finish before applying."));
      return;
    }
    try {
      bool changed = false;
      if (r->output->currentIndex() == 0)
        changed = session().execute(std::make_unique<core::SetSelectionCommand>(
            r->result.selection, "Refine Selection"));
      else if (r->output->currentIndex() == 1) {
        changed = session().execute(std::make_unique<RefinedMaskOutput>(
            std::make_unique<core::LayerMaskCommand>(
                r->target, r->owner->layer(r->target)->mask, r->result.mask,
                "Refine Layer Mask"),
            session().layerSelectionState(), r->oldMaskEditing));
      } else {
        if (!r->result.duplicate)
          throw std::runtime_error("The new layer is not ready.");
        auto transfer = *r->result.duplicate;
        transfer.layers.front().name += tr(" refined").toStdString();
        transfer.layers.front().mask = r->result.mask;
        auto placement = *r->owner->tree().placement(r->target);
        ++placement.index;
        changed = session().execute(std::make_unique<RefinedMaskOutput>(
            core::insertLayerTransfer(*r->owner, std::move(transfer), placement,
                                      session().layerSelectionState()),
            session().layerSelectionState(), r->oldMaskEditing));
      }
      if (changed && r->output->currentIndex() != 0)
        fileState().untouched = false;
    } catch (const std::exception &e) {
      r->status->setText(
          tr("Could not apply: %1").arg(QString::fromUtf8(e.what())));
      return;
    }
  }
  r->closing = true;
  if (r->cancel)
    *r->cancel = true;
  r->timer->stop();
  r->path.cancel();
  refinement_.reset();
  pointerRouter_->cancelCapture();
  canvasWindow_->cancelSelectionInput();
  r->panel->hide();
  workspace_->setContextOverlayInteractionRegion({});
  for (auto &[widget, enabled] : r->disabled)
    if (widget)
      widget->setEnabled(enabled);
  for (auto &[action, enabled] : r->actions)
    if (action)
      action->setEnabled(enabled);
  canvasWindow_->setRepairRegion({});
  canvasWindow_->setCanvasColors(r->background, r->light, r->dark);
  canvasWindow_->setBrushCursor(brushSettings_);
  setActiveTool(r->oldTool);
  pixelPreview_->setEnabled(pixelPreviewAction_->isChecked());
  canvasWindow_->setPixelPreview(pixelPreviewAction_->isChecked());
  if (!apply && r->current(*this))
    session().setEditingLayerMask(r->oldMaskEditing);
  r->panel->deleteLater();
  synchronizeUi(true, false);
}
bool MainWindow::refinementEvent(QObject *watched, QEvent *event) {
  if (!workspace_ || !canvasWindow_)
    return false;
  if (!refinement_) {
    if (watched == canvasWindow_ && event->type() == QEvent::ContextMenu &&
        !(static_cast<QContextMenuEvent *>(event)->reason() ==
              QContextMenuEvent::Mouse &&
          canvasWindow_->finishContextMenuSuppressed()) &&
        core::isSelectionTool(session().activeTool()) &&
        !canvasWindow_->selectionConstructionActive() &&
        !canvasWindow_->selectionDragging() && !layerTransform_ &&
        !selectionTransform_ && shortcutGestureIdle(true) &&
        refineSelectionAction_) {
      refreshRefinementActions();
      QMenu menu(this);
      menu.addAction(refineSelectionAction_);
      menu.addAction(selectionGrowAction_);
      menu.addAction(transformSelectionAction_);
      menu.addAction(transformPixelsAction_);
      menu.exec(static_cast<QContextMenuEvent *>(event)->globalPos());
      event->accept();
      return true;
    }
    return false;
  }
  if (watched == workspace_->panelOverlay() &&
      event->type() == QEvent::Resize) {
    auto &r = *refinement_;
    r.panel->setGeometry(
        std::max(8, workspace_->panelOverlay()->width() - 328), 12, 316,
        std::max(240,
                 std::min(740, workspace_->panelOverlay()->height() - 24)));
    workspace_->setContextOverlayInteractionRegion(r.panel->geometry());
  }
  if (event->type() == QEvent::ApplicationDeactivate) {
    refinement_->originalHeld = false;
    publishRefinementPreview();
  }
  if (event->type() != QEvent::KeyPress &&
      event->type() != QEvent::ShortcutOverride)
    return false;
  const auto *widget = qobject_cast<QWidget *>(watched);
  const auto *overlay = workspace_->panelOverlay();
  const bool ours = watched == canvasWindow_ || watched == windowHandle() ||
                    watched == overlay || watched == overlay->windowHandle() ||
                    (widget && (widget == this || isAncestorOf(widget) ||
                                overlay->isAncestorOf(widget)));
  if (!ours || QApplication::activePopupWidget() ||
      QApplication::activeModalWidget())
    return false;
  auto *key = static_cast<QKeyEvent *>(event);
  if (key->key() == Qt::Key_Escape) {
    if (event->type() == QEvent::KeyPress) {
      if (refinement_->gesture) {
        auto &r = *refinement_;
        const auto before = *r.gesture;
        r.cancellingGesture = true;
        for (auto *c : r.settings)
          c->finishEditing(false);
        r.state = before;
        r.gesture.reset();
        r.cancellingGesture = false;
        const auto s = before.settings;
        const std::array values{s.radius, s.smooth, s.feather, s.contrast * 100,
                                s.shift};
        for (std::size_t i = 0; i < values.size(); ++i) {
          QSignalBlocker guard(r.settings[i]);
          r.settings[i]->setValue(values[i]);
        }
        requestRefinement();
      } else if (refinement_->path.active())
        refinementBrush({}, 3);
      else
        finishRefinement(false);
    }
    event->accept();
    return true;
  }
  const auto typing = [](const QWidget *owner) {
    if (const auto *line = qobject_cast<const QLineEdit *>(owner))
      return !line->isReadOnly();
    if (const auto *control = dynamic_cast<const CompactValueControl *>(owner))
      return control->isManualEntryActive();
    return qobject_cast<const QAbstractSpinBox *>(owner) != nullptr;
  };
  if (typing(widget) || typing(QApplication::focusWidget()) ||
      typing(keyboardPanelTarget_))
    return false;
  const bool smaller =
                 shortcutMatches(shortcuts_, "DecreaseBrushSizeAction", *key),
             larger =
                 shortcutMatches(shortcuts_, "IncreaseBrushSizeAction", *key);
  if (smaller || larger) {
    if (event->type() == QEvent::KeyPress && !refinement_->path.active()) {
      auto *size = refinement_->brushSize;
      const double current = size->value();
      const double next = std::round(current * (larger ? 1.1 : 1. / 1.1));
      size->setValue(larger ? std::max(current + 1, next)
                            : std::min(current - 1, next));
    }
    event->accept();
    return true;
  }
  const bool undo = shortcutMatches(shortcuts_, "UndoAction", *key),
             redo = shortcutMatches(shortcuts_, "RedoAction", *key);
  if (undo || redo || key->key() == Qt::Key_Return ||
      key->key() == Qt::Key_Enter) {
    if (event->type() == QEvent::KeyPress) {
      if (undo || redo)
        stepRefinementHistory(redo);
      else
        finishRefinement(true);
    }
    event->accept();
    return true;
  }
  return false;
}
} // namespace imageeditor::ui
