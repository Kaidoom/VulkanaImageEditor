#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"

namespace imageeditor::core {
class SetAdjustmentScopeCommand final : public Command {
public:
    SetAdjustmentScopeCommand(LayerId id, AdjustmentScope before, AdjustmentScope after)
        : id_(id), before_(before), after_(after) {}
    bool apply(Document& d) override { return change(d, before_, after_); }
    bool undo(Document& d) override { return change(d, after_, before_); }
    std::string_view label() const noexcept override { return "Adjustment scope"; }
    std::size_t memoryCost() const noexcept override { return sizeof(*this); }
private:
    bool change(Document& d, AdjustmentScope from, AdjustmentScope to) {
        const auto* l=d.layer(id_);
        const auto* a=l?std::get_if<AdjustmentLayer>(&l->payload):nullptr;
        return a && a->scope==from && d.setAdjustmentScope(id_,to);
    }
    LayerId id_; AdjustmentScope before_, after_;
};
class SetLayerAdjustmentsCommand final : public Command {
public:
    SetLayerAdjustmentsCommand(LayerId, AdjustmentState before, AdjustmentState after);
    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] bool canAdoptApplied(const Document&) const noexcept override;
    [[nodiscard]] std::string_view label() const noexcept override { return "Layer adjustments"; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
private:
    LayerId id_;
    AdjustmentState before_, after_;
};

// Pins a stable target and guards against unrelated content/history changes.
// Destruction restores only a still-owned live preview, never a newer edit.
class AdjustmentEditTransaction {
public:
    AdjustmentEditTransaction(Document&, LayerId);
    ~AdjustmentEditTransaction();
    AdjustmentEditTransaction(const AdjustmentEditTransaction&) = delete;
    AdjustmentEditTransaction& operator=(const AdjustmentEditTransaction&) = delete;
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] LayerId target() const noexcept { return id_; }
    bool update(AdjustmentState);
    bool commit(History&);
    bool cancel();
private:
    [[nodiscard]] bool ownsPreview() const noexcept;
    Document& document_;
    LayerId id_;
    AdjustmentState before_, after_;
    Revision revision_ {0};
    std::uint64_t contentState_ {0};
    bool active_ {false};
};
} // namespace imageeditor::core
