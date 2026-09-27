#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace imageeditor::core {

class Document;
struct TextEditHint {
    std::uint64_t layer {0};
    std::size_t anchor {0}, position {0}; // UTF-8 bytes, not Qt UTF-16 positions.
};
struct LayerSelectionState {
    std::vector<std::uint64_t> ids;
    std::optional<std::uint64_t> primary, anchor;
    friend bool operator==(const LayerSelectionState&, const LayerSelectionState&) = default;
};

class Command {
    friend class History;
public:
    virtual ~Command() = default;

    virtual bool apply(Document& document) = 0;
    virtual bool undo(Document& document) = 0;
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;
    [[nodiscard]] virtual std::size_t memoryCost() const noexcept = 0;
    virtual bool mergeWith(const Command&) { return false; }
    [[nodiscard]] virtual bool affectsPersistentContent() const noexcept { return true; }
    // Optional editor-session consequence of a content command, not a new
    // selection-only history entry. Layer creation can restore its active ID.
    [[nodiscard]] virtual std::optional<std::uint64_t> activeLayerAfter(bool) const noexcept { return {}; }
    [[nodiscard]] virtual std::optional<TextEditHint> textEditAfter(bool) const noexcept { return {}; }
    [[nodiscard]] virtual const LayerSelectionState* layerSelectionAfter(bool) const noexcept { return nullptr; }

    // Live raster transactions mutate the authoritative surface before the
    // user releases the pointer. Only commands that can validate that already-
    // applied state may enter history without apply() being called first.
    [[nodiscard]] virtual bool canAdoptApplied(const Document&) const noexcept
    {
        return false;
    }
private:
    // Included in derived sizeof-based history accounting. Tokens survive
    // eviction/branch publication; cache revisions are deliberately unrelated.
    std::uint64_t contentBefore_ {0}, contentAfter_ {0};
};

} // namespace imageeditor::core
