#include "imageeditor/core/EffectCommands.hpp"
namespace imageeditor::core {
bool Document::setLayerEffects(LayerId id, LayerEffectState state) {
  auto *target = layer(id);
  if(target && std::holds_alternative<AdjustmentLayer>(target->payload))return false;
  if (!target || (state && !validLayerEffects(*state)) ||
      equivalentLayerEffects(target->effects, state))
    return false;
  target->effects = std::move(state);
  ++target->effectRevision;
  // Zero-strength bevel bypasses drawing, but a material scrub back from zero
  // can reuse its unchanged relief. Aggregate cache eviction still owns it.
  if (!hasActiveLayerEffects(target->effects) &&
      (!target->effects || !target->effects->items[7].enabled))
    target->effectCache.reset();
  touch();
  return true;
}
bool SetLayerEffectsCommand::apply(Document &d) {
  const auto *l = d.layer(id_);
  return l && equivalentLayerEffects(l->effects, before_) &&
         d.setLayerEffects(id_, after_);
}
bool SetLayerEffectsCommand::undo(Document &d) {
  const auto *l = d.layer(id_);
  return l && equivalentLayerEffects(l->effects, after_) &&
         d.setLayerEffects(id_, before_);
}
bool SetLayerEffectsCommand::canAdoptApplied(const Document &d) const noexcept {
  const auto *l = d.layer(id_);
  return l && !equivalentLayerEffects(before_, after_) &&
         equivalentLayerEffects(l->effects, after_);
}
EffectEditTransaction::EffectEditTransaction(Document &d, LayerId id)
    : document_(d), id_(id), contentState_(d.contentState()) {
  if (const auto *l = d.layer(id)) {
    before_ = after_ = l->effects;
    revision_ = l->effectRevision;
    active_ = true;
  }
}
EffectEditTransaction::~EffectEditTransaction() { (void)cancel(); }
bool EffectEditTransaction::ownsPreview() const noexcept {
  const auto *l = document_.layer(id_);
  return active_ && l && l->effectRevision == revision_ &&
         equivalentLayerEffects(l->effects, after_);
}
bool EffectEditTransaction::update(LayerEffectState state) {
  if (!ownsPreview() || document_.contentState() != contentState_) {
    (void)cancel();
    return false;
  }
  if (!document_.setLayerEffects(id_, state))
    return false;
  after_ = std::move(state);
  revision_ = document_.layer(id_)->effectRevision;
  return true;
}
bool EffectEditTransaction::commit(History &history) {
  if (!ownsPreview() || document_.contentState() != contentState_) {
    (void)cancel();
    return false;
  }
  if (equivalentLayerEffects(before_, after_)) {
    active_ = false;
    return false;
  }
  try {
    std::unique_ptr<Command> command =
        std::make_unique<SetLayerEffectsCommand>(id_, before_, after_);
    if (!history.adoptApplied(document_, command)) {
      (void)cancel();
      return false;
    }
  } catch (...) {
    (void)cancel();
    throw;
  }
  active_ = false;
  return true;
}
bool EffectEditTransaction::cancel() {
  const bool owns = ownsPreview();
  active_ = false;
  return owns && document_.setLayerEffects(id_, before_);
}
} // namespace imageeditor::core
