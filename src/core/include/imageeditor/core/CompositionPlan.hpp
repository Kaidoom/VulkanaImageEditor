#pragma once

#include "imageeditor/core/LayerTree.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/AdjustmentLayer.hpp"
#include <functional>
#include <algorithm>

namespace imageeditor::core {

// Immutable structural program, shared by output sampling and Vulkan dispatch.
// Missing (unselected) bases release clipping in selected-only evaluation;
// hidden bases remain present, and never promote a different child.
struct CompositionNode {
    LayerId id{};
    bool leaf{false}, clipping{false}, included{false}, isolated{false}, adjustment{false};
    std::vector<CompositionNode> children;
};
inline CompositionNode compositionPlan(const LayerTree& tree, std::span<const LayerId> included,
    std::span<const LayerId> selection={},
    std::span<const std::pair<LayerId,AdjustmentScope>> adjustments={})
{
    const auto build = [&](const auto& self, LayerId id) -> CompositionNode {
        CompositionNode n; n.id=id;
        if (const auto* c=tree.container(id)) {
            // Scope is structural, not conditional on visibility/neutrality.
            // Root ThisGroup is equivalent to AllBelow; no boundary is stored
            // against a parent ID that could become stale after reparenting.
            n.isolated=std::ranges::any_of(adjustments,[&](const auto& a){
                return std::ranges::find(c->children,a.first)!=c->children.end()
                    && (c->kind==ContainerKind::ClippingMaskGroup ? c->children.size()<=1 : a.second==AdjustmentScope::ThisGroup);
            });
            n.included=selection.empty() || std::ranges::any_of(selection,[&](auto root){return root==id||tree.isAncestor(root,id);});
            bool basePresent=false;
            for (auto child:c->children) {
                auto node=self(self,child);
                if (!node.included) continue;
                if(child==c->children.front())basePresent=true;
                n.children.push_back(std::move(node));
                n.included=true;
            }
            n.clipping=c->kind==ContainerKind::ClippingMaskGroup && basePresent && n.children.size()>1;
        } else {
            n.included=n.leaf=std::ranges::find(included,id)!=included.end();
            n.adjustment=std::ranges::any_of(adjustments,[&](const auto& a){return a.first==id;});
        }
        return n;
    };
    CompositionNode root;
    for(auto id:tree.roots) {
        auto node=build(build,id);
        if(node.included)root.children.push_back(std::move(node));
    }
    return root;
}
inline bool hasClippingGroups(const LayerTree& tree)
{
    return std::ranges::any_of(tree.containers,[](const auto& c){return c.kind==ContainerKind::ClippingMaskGroup;});
}
inline PremultipliedColor clippingOpaque(PremultipliedColor c)
{
    const auto result=blend_detail::bClippingOpaque({c[0],c[1],c[2],c[3]});
    return {result.x,result.y,result.z,result.w};
}
// A container base has no external opacity/mode. Its subtree is evaluated on
// transparency. Recolor only its content coverage, preserving exterior styles.
inline PremultipliedColor clippingContainerResult(PremultipliedColor styled,
    PremultipliedColor content, PremultipliedColor working)
{
    const auto result=blend_detail::bClippingContainer({styled[0],styled[1],styled[2],styled[3]},
        {content[0],content[1],content[2],content[3]}, {working[0],working[1],working[2],working[3]});
    return {result.x,result.y,result.z,result.w};
}
// leaf(id, backdrop, contentOnly, replacement) evaluates an ordinary leaf.
// raw(id) returns its pre-style content before crop/mask/final opacity.
// Replacing RGB never replaces alpha: fractional base coverage is applied once.
template<class Leaf, class Raw>
PremultipliedColor evaluateComposition(const CompositionNode& node, PremultipliedColor backdrop,
    const Leaf& leaf, const Raw& raw, bool contentOnly=false, bool inDomain=false)
{
    if(node.leaf)return leaf(node.id,backdrop,contentOnly,nullptr);
    if(node.isolated && !inDomain) {
        return compositeLayer(backdrop,evaluateComposition(node,{},leaf,raw,contentOnly,true),1,BlendMode::Normal);
    }
    if(!node.clipping) {
        for(const auto& child:node.children)backdrop=evaluateComposition(child,backdrop,leaf,raw,contentOnly);
        return backdrop;
    }
    const auto& base=node.children.front();
    if(base.adjustment)return backdrop; // An operator supplies no clipping silhouette.
    const auto styled=base.leaf?raw(base.id):evaluateComposition(base,{},leaf,raw,contentOnly);
    const auto content=base.leaf||contentOnly?styled:evaluateComposition(base,{},leaf,raw,true);
    auto working=clippingOpaque(styled);
    for(std::size_t i=1;i<node.children.size();++i)
        working=evaluateComposition(node.children[i],working,leaf,raw,contentOnly);
    if(base.leaf)return leaf(base.id,backdrop,contentOnly,&working);
    return compositeLayer(backdrop,clippingContainerResult(styled,content,working),1,BlendMode::Normal);
}
} // namespace imageeditor::core
