#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
namespace imageeditor::core {
// Derive effect-only inheritance changes for a mirrored layer. The caller can
// resolve a detached candidate before atomic publication of a multi-layer edit.
[[nodiscard]] LayerMirror mirrorEffectOverrides(const Layer&, const Layer& source,
                                                const LayerEffectState&, bool overrideAll = false);
class SetLayerEffectsCommand final : public Command {
public:
  SetLayerEffectsCommand(LayerId id, LayerEffectState before,
                         LayerEffectState after, bool overrideAll = false)
      : id_(id), before_(std::move(before)), after_(std::move(after)), overrideAll_(overrideAll) {}
  bool apply(Document &) override;
  bool undo(Document &) override;
  bool canAdoptApplied(const Document &) const noexcept override;
  std::string_view label() const noexcept override { return "Layer effects"; }
  std::size_t memoryCost() const noexcept override {
    return sizeof(*this) + layerEffectMemoryCost(before_) + layerEffectMemoryCost(after_);
  }

private:
  friend class EffectEditTransaction;
  LayerId id_;
  LayerEffectState before_, after_;
  std::optional<LayerMirror> beforeMirror_, afterMirror_;
  bool overrideAll_{}, captured_{};
};
class EffectEditTransaction {
public:
  EffectEditTransaction(Document &, LayerId);
  ~EffectEditTransaction();
  EffectEditTransaction(const EffectEditTransaction &) = delete;
  EffectEditTransaction &operator=(const EffectEditTransaction &) = delete;
  bool active() const noexcept { return active_; }
  LayerId target() const noexcept { return id_; }
  bool update(LayerEffectState);
  bool commit(History &);
  bool cancel();

private:
  bool ownsPreview() const noexcept;
  Document &document_;
  LayerId id_;
  LayerEffectState before_, after_;
  std::optional<LayerMirror> beforeMirror_, afterMirror_;
  Revision revision_{};
  std::uint64_t contentState_{};
  bool active_{};
};
} // namespace imageeditor::core
