#include "imageeditor/core/EffectCommands.hpp"
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
namespace imageeditor::core {
namespace {
bool restoreEffects(Document& document, LayerId id, const LayerEffectState& effects,
                    const std::optional<LayerMirror>& mirror) {
  const auto* current = document.layer(id);
  if (!current || current->mirror.has_value() != mirror.has_value()) return false;
  if (!mirror) return document.setLayerEffects(id, effects);
  // Keep unrelated appearance values intact while restoring effect inheritance.
  Layer state;
  state.id = id;
  state.opacity = current->opacity;
  state.fillOpacity = current->fillOpacity;
  state.blendMode = current->blendMode;
  state.effects = effects;
  state.mirror = mirror;
  return document.restoreMirrorAppearance(id, state);
}
}
LayerMirror mirrorEffectOverrides(const Layer& layer, const Layer& source,
                                 const LayerEffectState& state, bool overrideAll) {
  auto mirror = layer.mirror.value();
  if (overrideAll) {
    mirror.effects.fill(MirrorEffectMode::Override);
    mirror.pixelAlphaShapesEffectsOverride = true;
    mirror.maskEffectsAfterRenderingOverride = true;
    return mirror;
  }
  const LayerEffectStack defaults;
  const auto& before = layer.effects ? *layer.effects : defaults;
  const auto& after = state ? *state : defaults;
  const auto& inherited = source.effects ? *source.effects : defaults;
  for (std::size_t i = 0; i < layerEffectCount; ++i) {
    const auto& old = before.items[i];
    const auto& next = after.items[i];
    if (old == next) continue;
    auto& mode = mirror.effects[i];
    if (old.enabled && !next.enabled)
      mode = mode == MirrorEffectMode::Override ? MirrorEffectMode::Inherit : MirrorEffectMode::Disabled;
    else if (mode == MirrorEffectMode::Disabled && !old.enabled && next.enabled && inherited.items[i].enabled) {
      // Re-enabling an inherited effect removes the disabling override,
      // unless the same edit also changes its parameters.
      auto enabled = old; enabled.enabled = true;
      mode = enabled == next ? MirrorEffectMode::Inherit : MirrorEffectMode::Override;
    } else mode = MirrorEffectMode::Override;
  }
  if (before.pixelAlphaShapesEffects != after.pixelAlphaShapesEffects)
    mirror.pixelAlphaShapesEffectsOverride = true;
  if (before.maskEffectsAfterRendering != after.maskEffectsAfterRendering)
    mirror.maskEffectsAfterRenderingOverride = true;
  return mirror;
}
bool Document::setLayerEffects(LayerId id, LayerEffectState state, bool overrideAll) {
  auto *target = layer(id);
  if(target && (std::holds_alternative<AdjustmentLayer>(target->payload) || isScalarLayer(*target) || isPackedDataLayer(*target)))return false;
  if (!target || (state && !validLayerEffects(*state))) return false;
  auto mirror = target->mirror;
  if (mirror) {
    const auto* source = layer(mirror->source);
    if (!source) return false;
    mirror = mirrorEffectOverrides(*target, *source, state, overrideAll);
    auto candidate = *target;
    candidate.mirror = mirror;
    candidate.effects = state;
    // Prepare all potentially allocating resolution before publishing either
    // the effect values or the inheritance flags.
    state = resolvedMirroredLayer(candidate, *source).effects;
  }
  if (equivalentLayerEffects(target->effects, state) && target->mirror == mirror) return false;
  target->effects = std::move(state);
  target->mirror = mirror;
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
  if (!l || !equivalentLayerEffects(l->effects, before_) || (captured_ && l->mirror != beforeMirror_)) return false;
  if (captured_) return restoreEffects(d, id_, after_, afterMirror_);
  const auto originalMirror = l->mirror;
  if (!d.setLayerEffects(id_, after_, overrideAll_)) return false;
  // Setters retain layer storage. Read the already-resolved published fields
  // through this stable pointer, without another allocating graph resolution.
  beforeMirror_ = originalMirror;
  afterMirror_ = l->mirror;
  after_ = l->effects;
  captured_ = true;
  return true;
}
bool SetLayerEffectsCommand::undo(Document &d) {
  const auto *l = d.layer(id_);
  return l && equivalentLayerEffects(l->effects, after_) && l->mirror == afterMirror_ &&
         restoreEffects(d, id_, before_, beforeMirror_);
}
bool SetLayerEffectsCommand::canAdoptApplied(const Document &d) const noexcept {
  const auto *l = d.layer(id_);
  return l && l->mirror == afterMirror_ &&
         (!equivalentLayerEffects(before_, after_) || beforeMirror_ != afterMirror_) &&
         equivalentLayerEffects(l->effects, after_);
}
EffectEditTransaction::EffectEditTransaction(Document &d, LayerId id)
    : document_(d), id_(id), contentState_(d.contentState()) {
  if (const auto *l = d.layer(id)) {
    before_ = after_ = l->effects;
    beforeMirror_ = afterMirror_ = l->mirror;
    revision_ = l->effectRevision;
    active_ = true;
  }
}
EffectEditTransaction::~EffectEditTransaction() { (void)cancel(); }
bool EffectEditTransaction::ownsPreview() const noexcept {
  const auto *l = document_.layer(id_);
  return active_ && l && l->effectRevision == revision_ &&
         l->mirror == afterMirror_ &&
         equivalentLayerEffects(l->effects, after_);
}
bool EffectEditTransaction::update(LayerEffectState state) {
  if (!ownsPreview() || document_.contentState() != contentState_) {
    (void)cancel();
    return false;
  }
  const auto* current = document_.layer(id_);
  if (!current || !document_.setLayerEffects(id_, state))
    return false;
  after_ = current->effects;
  afterMirror_ = current->mirror;
  revision_ = current->effectRevision;
  return true;
}
bool EffectEditTransaction::commit(History &history) {
  if (!ownsPreview() || document_.contentState() != contentState_) {
    (void)cancel();
    return false;
  }
  if (equivalentLayerEffects(before_, after_) && beforeMirror_ == afterMirror_) {
    active_ = false;
    return false;
  }
  try {
    auto edit = std::make_unique<SetLayerEffectsCommand>(id_, before_, after_);
    edit->beforeMirror_ = beforeMirror_;
    edit->afterMirror_ = afterMirror_;
    edit->captured_ = true;
    std::unique_ptr<Command> command = std::move(edit);
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
  return owns && restoreEffects(document_, id_, before_, beforeMirror_);
}
} // namespace imageeditor::core
