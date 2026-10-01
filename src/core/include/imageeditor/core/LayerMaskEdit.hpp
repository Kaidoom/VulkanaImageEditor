#pragma once
#include "imageeditor/core/RasterEditTransaction.hpp"

namespace imageeditor::core {
class LayerMaskCommand final : public Command {
public:
  LayerMaskCommand(LayerId, LayerMaskState before, LayerMaskState after,
                   std::string label);
  bool apply(Document &) override;
  bool undo(Document &) override;
  bool canAdoptApplied(const Document &) const noexcept override;
  std::string_view label() const noexcept override { return label_; }
  std::size_t memoryCost() const noexcept override;

private:
  LayerId id_;
  LayerMaskState before_, after_;
  std::string label_;
};

// Adapts the existing regional raster tools to an immutable R8 mask. Only
// touched journal tiles are materialized as RGBA; no full RGBA mask copy.
class LayerMaskEdit final {
public:
  LayerMaskEdit(Document &, LayerId, std::string label);
  ~LayerMaskEdit();
  LayerMaskEdit(const LayerMaskEdit &) = delete;
  LayerMaskEdit &operator=(const LayerMaskEdit &) = delete;
  Document &proxy() noexcept { return proxy_; }
  History &provisionalHistory() noexcept { return provisional_; }
  RasterEditCommitResult commit(History &);
  void cancel() noexcept;
  bool valid() const noexcept;

private:
  bool publish(SelectionState);
  Document &owner_;
  LayerId id_;
  LayerMaskState before_, current_;
  AffineTransform transform_;
  SelectionState selection_;
  std::string label_;
  Document proxy_;
  History provisional_;
  bool finished_{false};
};
} // namespace imageeditor::core
