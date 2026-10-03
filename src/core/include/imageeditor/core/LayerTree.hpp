#pragma once

#include "imageeditor/core/Layer.hpp"

#include <optional>
#include <span>
#include <vector>

namespace imageeditor::core {

// Containers have an inherited visibility gate, never their own transform or
// opacity. Clipping groups evaluate their structural stack explicitly.
// Every child retains its absolute document-space matrix. Orders are bottom first.
enum class ContainerKind { Folder,
    Group, ClippingMaskGroup };
[[nodiscard]] constexpr bool isGroup(ContainerKind kind) { return kind != ContainerKind::Folder; }
[[nodiscard]] constexpr bool isExpandable(ContainerKind kind) { return kind != ContainerKind::Group; }
enum class ColorLabel : std::uint8_t { None,
    Red,
    Orange,
    Yellow,
    Green,
    Blue,
    Purple };
struct LayerContainer {
    LayerId id { 0 };
    std::string name;
    ContainerKind kind { ContainerKind::Folder };
    ColorLabel colorLabel { ColorLabel::None };
    std::vector<LayerId> children;
    bool visible { true };
    friend bool operator==(const LayerContainer&, const LayerContainer&) = default;
};
struct ItemPlacement {
    LayerId parent { 0 }; // Zero is the document root, never an item.
    std::size_t index { 0 };
    friend bool operator==(const ItemPlacement&, const ItemPlacement&) = default;
};
struct LayerTree {
    static constexpr std::size_t maxItems = 4096, maxDepth = 64;
    std::vector<LayerId> roots;
    std::vector<LayerContainer> containers;
    [[nodiscard]] const LayerContainer* container(LayerId) const noexcept;
    [[nodiscard]] LayerContainer* container(LayerId) noexcept;
    [[nodiscard]] const std::vector<LayerId>* children(LayerId parent) const noexcept;
    [[nodiscard]] std::vector<LayerId>* children(LayerId parent) noexcept;
    [[nodiscard]] std::optional<ItemPlacement> placement(LayerId) const noexcept;
    [[nodiscard]] ItemPlacement insertionAbove(LayerId) const noexcept;
    [[nodiscard]] bool isAncestor(LayerId ancestor, LayerId child) const noexcept;
    // Throws on duplicate membership, missing/orphan items, cycles, or excessive depth.
    [[nodiscard]] std::vector<LayerId> orderedLeaves(std::span<const LayerId> leafIds) const;
    [[nodiscard]] std::vector<LayerId> normalize(std::span<const LayerId>) const;
    [[nodiscard]] std::vector<LayerId> descendants(LayerId, bool immediateOnly = false) const;
    [[nodiscard]] bool consecutiveSiblings(std::span<const LayerId>) const;
    // Destination is an insertion slot in the original sibling order. Strong guarantee.
    bool reparent(std::span<const LayerId>, ItemPlacement);
    bool dissolve(LayerId);
    // Replace normalized selected subtrees at the topmost selected position.
    // Other items keep their relative order and parents; strong guarantee.
    bool consolidate(std::span<const LayerId>, LayerId replacement);
    [[nodiscard]] std::size_t memoryCost() const noexcept;
    friend bool operator==(const LayerTree&, const LayerTree&) = default;
};

} // namespace imageeditor::core
