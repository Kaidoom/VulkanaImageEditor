#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
namespace imageeditor::core {
class SetLayerEffectsCommand final : public Command {
public:
  SetLayerEffectsCommand(LayerId id, LayerEffectState before,
                         LayerEffectState after)
      : id_(id), before_(std::move(before)), after_(std::move(after)) {}
  bool apply(Document &) override;
  bool undo(Document &) override;
  bool canAdoptApplied(const Document &) const noexcept override;
  std::string_view label() const noexcept override { return "Layer effects"; }
  std::size_t memoryCost() const noexcept override {
    return sizeof(*this) + 2 * sizeof(LayerEffectStack);
  }

private:
  LayerId id_;
  LayerEffectState before_, after_;
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
  Revision revision_{};
  std::uint64_t contentState_{};
  bool active_{};
};
} // namespace imageeditor::core
