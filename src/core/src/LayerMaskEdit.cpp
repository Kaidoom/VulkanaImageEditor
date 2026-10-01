#include "imageeditor/core/LayerMaskEdit.hpp"
#include <functional>
#include <stdexcept>

namespace imageeditor::core {
namespace {
class MaskSurface final : public RasterSurface {
public:
  MaskSurface(SelectionState state, std::function<bool(SelectionState)> publish)
      : state_(std::move(state)), publish_(std::move(publish)) {}
  SurfaceId id() const noexcept override { return id_; }
  Extent2u extent() const noexcept override { return state_->extent(); }
  Revision revision() const noexcept override { return revision_; }
  DirtySet dirtySince(Revision previous) const override {
    return {revision_, previous != revision_, {}};
  }
  void copyRgba8(RectI r, std::span<std::byte> bytes,
                 std::size_t stride) const override {
    validate(r, bytes.size(), stride);
    for (int y = 0; y < r.height; ++y)
      for (int x = 0; x < r.width; ++x) {
        const auto value =
            std::byte(state_->coverageAtDocumentPixel(r.x + x, r.y + y));
        auto *p = bytes.data() + std::size_t(y) * stride + std::size_t(x) * 4;
        p[0] = p[1] = p[2] = value;
        p[3] = std::byte{255};
      }
  }
  DirtySet replaceRgba8Batch(std::span<const RasterPatch> patches) override {
    std::vector<std::vector<std::uint8_t>> owned;
    owned.reserve(patches.size());
    std::vector<CoveragePatch> views;
    views.reserve(patches.size());
    DirtySet dirty{revision_, false, {}};
    for (const auto &p : patches) {
      if (p.region.empty())
        continue;
      validate(p.region, p.rgbaBytes.size(), p.stride);
      auto &bytes = owned.emplace_back(std::size_t(p.region.width) *
                                       std::size_t(p.region.height));
      for (int y = 0; y < p.region.height; ++y)
        for (int x = 0; x < p.region.width; ++x)
          bytes[std::size_t(y) * std::size_t(p.region.width) + std::size_t(x)] =
              std::to_integer<std::uint8_t>(
                  p.rgbaBytes[std::size_t(y) * p.stride + std::size_t(x) * 4]);
      views.push_back({p.region, bytes, std::size_t(p.region.width)});
      dirty.regions.push_back(p.region);
    }
    auto next = state_->replacedR8(views);
    if (!next)
      return {revision_, false, {}};
    if (!publish_(next))
      throw std::runtime_error("Layer mask target changed during editing");
    state_ = std::move(next);
    dirty.revision = ++revision_;
    return dirty;
  }
  DirtySet swapRgba8Batch(std::span<MutableRasterPatch> patches) override {
    std::vector<std::vector<std::byte>> originals;
    originals.reserve(patches.size());
    std::vector<RasterPatch> views;
    for (const auto &p : patches) {
      auto &bytes = originals.emplace_back(p.rgbaBytes.size());
      copyRgba8(p.region, bytes, p.stride);
      views.push_back({p.region, p.rgbaBytes, p.stride});
    }
    auto dirty = replaceRgba8Batch(views);
    for (std::size_t i = 0; i < patches.size(); ++i)
      std::copy(originals[i].begin(), originals[i].end(),
                patches[i].rgbaBytes.begin());
    return dirty;
  }

private:
  void validate(RectI r, std::size_t size, std::size_t stride) const {
    const auto e = extent();
    if (r.empty() || r.x < 0 || r.y < 0 || r.right() > int(e.width) ||
        r.bottom() > int(e.height) || stride < std::size_t(r.width) * 4 ||
        size < stride * std::size_t(r.height - 1) + std::size_t(r.width) * 4)
      throw std::invalid_argument("Invalid layer mask region");
  }
  SurfaceId id_{makeSurfaceId()};
  Revision revision_{1};
  SelectionState state_;
  std::function<bool(SelectionState)> publish_;
};
} // namespace
LayerMaskCommand::LayerMaskCommand(LayerId id, LayerMaskState before,
                                   LayerMaskState after, std::string label)
    : id_(id), before_(std::move(before)), after_(std::move(after)),
      label_(std::move(label)) {}
bool LayerMaskCommand::apply(Document &d) {
  const auto *l = d.layer(id_);
  return l && l->mask == before_ && !equivalentLayerMasks(before_, after_) &&
         d.setLayerMask(id_, after_);
}
bool LayerMaskCommand::undo(Document &d) {
  const auto *l = d.layer(id_);
  return l && l->mask == after_ && d.setLayerMask(id_, before_);
}
bool LayerMaskCommand::canAdoptApplied(const Document &d) const noexcept {
  const auto *l = d.layer(id_);
  return l && l->mask == after_ && !equivalentLayerMasks(before_, after_);
}
std::size_t LayerMaskCommand::memoryCost() const noexcept {
  return sizeof(*this) + label_.capacity() +
         (before_ ? sizeof(LayerMask) + before_->coverage->memoryCost() : 0) +
         (after_ && after_ != before_
              ? sizeof(LayerMask) + after_->coverage->memoryCost()
              : 0);
}
LayerMaskEdit::LayerMaskEdit(Document &owner, LayerId id, std::string label)
    : owner_(owner), id_(id), selection_(owner.selection()),
      label_(std::move(label)), proxy_(owner.canvas()) {
  const auto *layer = owner.layer(id);
  if (!layer || !layer->mask || !validLayerMask(layer->mask))
    throw std::invalid_argument("Layer has no editable mask");
  before_ = current_ = layer->mask;
  transform_ = layer->localToDocument;
  auto surface = std::make_shared<MaskSurface>(
      before_->coverage,
      [this](SelectionState state) { return publish(std::move(state)); });
  auto proxy = Layer::raster(layer->name, std::move(surface));
  proxy.id = id;
  proxy.localToDocument =
      composeTransform(transform_, *before_->localToMask.inverted());
  if (!proxy_.insertLayer(0, std::move(proxy)))
    throw std::runtime_error("Cannot prepare layer mask edit");
  proxy_.setSelection(selection_);
}
LayerMaskEdit::~LayerMaskEdit() { cancel(); }
bool LayerMaskEdit::valid() const noexcept {
  const auto *layer = owner_.layer(id_);
  return !finished_ && layer && layer->mask == current_ &&
         layer->localToDocument == transform_ &&
         owner_.selection() == selection_;
}
bool LayerMaskEdit::publish(SelectionState coverage) {
  if (!valid())
    return false;
  auto next = std::make_shared<LayerMask>(*current_);
  next->coverage = std::move(coverage);
  if (!owner_.setLayerMask(id_, next))
    return false;
  current_ = std::move(next);
  return true;
}
RasterEditCommitResult LayerMaskEdit::commit(History &history) {
  if (!valid()) {
    cancel();
    return RasterEditCommitResult::TargetUnavailable;
  }
  if (equivalentLayerMasks(before_, current_)) {
    cancel();
    return RasterEditCommitResult::NoChanges;
  }
  std::unique_ptr<Command> command =
      std::make_unique<LayerMaskCommand>(id_, before_, current_, label_);
  if (!history.adoptApplied(owner_, command)) {
    cancel();
    return RasterEditCommitResult::HistoryRejected;
  }
  finished_ = true;
  return RasterEditCommitResult::Committed;
}
void LayerMaskEdit::cancel() noexcept {
  if (finished_)
    return;
  if (const auto *layer = owner_.layer(id_); layer && layer->mask == current_)
    owner_.setLayerMask(id_, before_);
  finished_ = true;
}
} // namespace imageeditor::core
