#include "imageeditor/core/LayerTree.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace imageeditor::core {
const LayerContainer* LayerTree::container(LayerId id) const noexcept
{
    const auto it = std::ranges::find(containers, id, &LayerContainer::id);
    return it == containers.end() ? nullptr : &*it;
}
LayerContainer* LayerTree::container(LayerId id) noexcept
{
    return const_cast<LayerContainer*>(std::as_const(*this).container(id));
}
const std::vector<LayerId>* LayerTree::children(LayerId parent) const noexcept
{
    const auto* c = container(parent);
    return parent == 0 ? &roots : c ? &c->children
                                    : nullptr;
}
std::vector<LayerId>* LayerTree::children(LayerId parent) noexcept
{
    return const_cast<std::vector<LayerId>*>(std::as_const(*this).children(parent));
}
std::optional<ItemPlacement> LayerTree::placement(LayerId id) const noexcept
{
    const auto find = [id](const auto& ids, LayerId parent) -> std::optional<ItemPlacement> {
        const auto it = std::ranges::find(ids, id);
        return it == ids.end() ? std::nullopt : std::optional<ItemPlacement> { { parent, std::size_t(it - ids.begin()) } };
    };
    if (auto p = find(roots, 0))
        return p;
    for (const auto& c : containers)
        if (auto p = find(c.children, c.id))
            return p;
    return { };
}
bool LayerTree::isAncestor(LayerId ancestor, LayerId child) const noexcept
{
    for (std::size_t depth = 0; depth <= maxDepth; ++depth) {
        const auto p = placement(child);
        if (!p || !p->parent)
            return false;
        if (p->parent == ancestor)
            return true;
        child = p->parent;
    }
    return false;
}
std::vector<LayerId> LayerTree::orderedLeaves(std::span<const LayerId> leaves) const
{
    auto require = [](bool ok) { if (!ok) throw std::invalid_argument("Invalid layer hierarchy (missing, duplicated, cyclic, or excessive nesting)"); };
    require(leaves.size() + containers.size() <= maxItems);
    std::unordered_set<LayerId> known, seen;
    for (auto id : leaves)
        require(id && known.insert(id).second);
    for (const auto& c : containers) {
        require(c.id && !c.name.empty() && c.name.size() <= 4096 && known.insert(c.id).second);
        require(c.kind == ContainerKind::Folder || c.kind == ContainerKind::Group);
        require(c.colorLabel <= ColorLabel::Purple);
    }
    std::vector<LayerId> order;
    order.reserve(leaves.size());
    const auto visit = [&](auto&& self, const std::vector<LayerId>& ids, std::size_t depth) -> void {
        require(depth <= maxDepth);
        for (auto id : ids) {
            require(known.contains(id) && seen.insert(id).second);
            if (const auto* c = container(id))
                self(self, c->children, depth + 1);
            else
                order.push_back(id);
        }
    };
    visit(visit, roots, 0);
    require(seen.size() == known.size());
    return order;
}
std::vector<LayerId> LayerTree::normalize(std::span<const LayerId> input) const
{
    std::vector<LayerId> result;
    const auto visit = [&](auto&& self, const auto& ids) -> void {
        for (auto id : ids) {
            if (std::ranges::find(input, id) != input.end())
                result.push_back(id);
            else if (const auto* c = container(id))
                self(self, c->children);
        }
    };
    visit(visit, roots);
    return result;
}
std::vector<LayerId> LayerTree::descendants(LayerId id, bool immediate) const
{
    std::vector<LayerId> result;
    const auto visit = [&](auto&& self, LayerId item) -> void {
        if (const auto* c = container(item)) {
            for (auto child : c->children) {
                if (!container(child))
                    result.push_back(child);
                else if (!immediate)
                    self(self, child);
            }
        } else if (placement(item))
            result.push_back(item);
    };
    visit(visit, id);
    return result;
}
bool LayerTree::consecutiveSiblings(std::span<const LayerId> input) const
{
    const auto ids = normalize(input);
    if (ids.empty())
        return false;
    const auto first = placement(ids.front());
    if (!first)
        return false;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto p = placement(ids[i]);
        if (!p || p->parent != first->parent || p->index != first->index + i)
            return false;
    }
    return true;
}
bool LayerTree::reparent(std::span<const LayerId> input, ItemPlacement destination)
{
    const auto ids = normalize(input);
    const auto* siblings = children(destination.parent);
    if (ids.empty() || !siblings || destination.index > siblings->size())
        return false;
    for (auto id : ids)
        if (id == destination.parent || isAncestor(id, destination.parent))
            return false;
    auto next = *this;
    auto index = destination.index;
    for (auto id : ids) {
        const auto p = placement(id);
        if (p->parent == destination.parent && p->index < destination.index)
            --index;
        std::erase(*next.children(p->parent), id);
    }
    auto& target = *next.children(destination.parent);
    target.insert(target.begin() + std::ptrdiff_t(index), ids.begin(), ids.end());
    if (next == *this)
        return false;
    *this = std::move(next);
    return true;
}
bool LayerTree::dissolve(LayerId id)
{
    const auto* c = container(id);
    const auto p = placement(id);
    if (!c || !p)
        return false;
    auto next = *this;
    auto& siblings = *next.children(p->parent);
    siblings.erase(siblings.begin() + std::ptrdiff_t(p->index));
    siblings.insert(siblings.begin() + std::ptrdiff_t(p->index), c->children.begin(), c->children.end());
    std::erase_if(next.containers, [id](const auto& entry) { return entry.id == id; });
    *this = std::move(next);
    return true;
}
bool LayerTree::consolidate(std::span<const LayerId> input, LayerId replacement)
{
    if (!replacement || placement(replacement) || container(replacement)) return false;
    for (auto id : input) if (!placement(id)) return false;
    const auto ids = normalize(input); // Document order, not selection/click order.
    if (ids.empty()) return false;
    auto next = *this;
    const auto top = *placement(ids.back());
    // Replace the top slot first. Removing earlier siblings then adjusts its
    // index naturally, including selections spread across different parents.
    next.children(top.parent)->at(top.index) = replacement;
    const auto remove = [&](auto&& self, LayerId id) -> void {
        if (const auto* c = next.container(id)) {
            const auto children = c->children;
            for (auto child : children) self(self, child);
            std::erase_if(next.containers, [=](const auto& item) { return item.id == id; });
        }
        if (auto p = next.placement(id)) std::erase(*next.children(p->parent), id);
    };
    for (auto id : ids) remove(remove, id);
    *this = std::move(next);
    return true;
}
std::size_t LayerTree::memoryCost() const noexcept
{
    std::size_t result = sizeof(*this) + roots.capacity() * sizeof(LayerId) + containers.capacity() * sizeof(LayerContainer);
    for (const auto& c : containers)
        result += c.name.capacity() + c.children.capacity() * sizeof(LayerId);
    return result;
}
} // namespace imageeditor::core
