#include "imageeditor/core/ColorSampler.hpp"
#include "imageeditor/core/ColorMath.hpp"
#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCrop.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
namespace {

using LinearColor = PremultipliedColor;

std::shared_ptr<const CompiledAdjustmentStack> preparedAdjustments(const AdjustmentState& state)
{
    if (!state) return {};
    auto program = std::make_shared<CompiledAdjustmentStack>(compileAdjustmentStack(state));
    return program->active ? program : nullptr;
}

LinearColor sampleRaster(const AffineTransform& inverse, const RasterSurface& surface,
    Vec2d point, std::size_t& texelsRead, SampleFiltering = SampleFiltering::AlphaAware)
{
    const auto extent = surface.extent();
    if (extent.empty()) {
        return {};
    }
    const auto local = inverse.map(point);
    if (!std::isfinite(local.x) || !std::isfinite(local.y))return {};
    float edgeCoverage=1;
    if(!inverse.isAffine()){
        const auto d=inverse.derivatives(point);
        edgeCoverage=layerCropCoverage(LayerCrop{{0,0,double(extent.width),double(extent.height)}},local,d[0],d[1]);
    }else if(local.x < 0.0 || local.y < 0.0 || local.x >= extent.width || local.y >= extent.height)edgeCoverage=0;
    if(edgeCoverage<=0) {
        return {};
    }
    // Vulkan normalized linear filtering: texel centers are i+0.5, with
    // CLAMP_TO_EDGE only inside the actual transformed layer quad.
    const double x = std::clamp(local.x - 0.5, 0.0, double(extent.width - 1));
    const double y = std::clamp(local.y - 0.5, 0.0, double(extent.height - 1));
    const auto x0 = static_cast<std::int32_t>(std::floor(x));
    const auto y0 = static_cast<std::int32_t>(std::floor(y));
    const auto x1 = static_cast<std::int32_t>(std::ceil(x));
    const auto y1 = static_cast<std::int32_t>(std::ceil(y));
    const RectI region {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    std::array<std::byte, 16> bytes {};
    const auto stride = static_cast<std::size_t>(region.width) * 4;
    surface.copyRgba8(region, bytes, stride);
    texelsRead += static_cast<std::size_t>(region.width * region.height);
    std::array<double,4> result {};
    for (int row = 0; row < region.height; ++row) {
        const auto wy = region.height == 1 ? 1.0 : row == 0 ? 1.0 - (y - y0) : y - y0;
        for (int col = 0; col < region.width; ++col) {
            const auto wx = region.width == 1 ? 1.0 : col == 0 ? 1.0 - (x - x0) : x - x0;
            const auto offset = static_cast<std::size_t>(row) * stride + std::size_t(col) * 4;
            const double alpha = double(std::to_integer<std::uint8_t>(bytes[offset + 3])) / 255.0;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto value = std::to_integer<std::uint8_t>(bytes[offset + channel]);
                result[channel] += wx * wy * (channel == 3 ? alpha : srgbToLinear(value) * alpha);
            }
        }
    }
    return {float(result[0])*edgeCoverage,float(result[1])*edgeCoverage,float(result[2])*edgeCoverage,float(result[3])*edgeCoverage};
}

LinearColor sampleLayer(const Layer& layer, const RasterSurface& surface,
    Vec2d point, std::size_t& texelsRead)
{
    const auto inverse = renderTransform(layer).inverted();
    return inverse ? sampleRaster(*inverse, surface, point, texelsRead) : LinearColor {};
}

Rgba8 encode(LinearColor color)
{
    return encodeColor(color);
}

} // namespace

PreparedLayerSampler::PreparedLayerSampler(const Layer& original, bool adjusted)
{
    auto layer=original;
    if(adjusted)layer=prepareSpatialFilterLayer(original);
    else {layer.filters.reset();layer.filterCache.reset();layer.effects.reset();layer.effectCache.reset();}
    const auto inverse = renderTransform(layer).inverted();
    const auto local = layer.localToDocument.inverted();
    if (!inverse || !local) return;
    surface_ = renderedSurface(layer);
    if (!surface_) return;
    revision_ = surface_->revision(); inverse_ = *inverse; documentToLocal_ = *local;
    crop_=layer.crop;
    mask_=layer.mask;
    if (adjusted && !layerSpatialFilterCacheValid(layer)) adjustments_ = preparedAdjustments(layer.adjustments);
    if(adjusted&&hasActiveLayerEffects(layer.effects)){
        effects_=compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer));effectCache_=layer.effectCache;
    }
}
PremultipliedColor PreparedLayerSampler::sample(Vec2d point, std::size_t stopBefore) const
{
    if (!surface_) return {};
    if (surface_->revision() != revision_)
        throw std::runtime_error("Layer pixels changed during prepared sampling");
    std::size_t reads = 0;
    auto color = sampleRaster(inverse_, *surface_, point, reads);
    if(adjustments_)color=evaluateAdjustments(*adjustments_,color,documentToLocal_.map(point),stopBefore);
    if(effects_)color=compositeLayerEffects({},color,*effects_,effectCache_.get(),documentToLocal_.map(point),1,BlendMode::Normal);
    color=applyLayerCrop(color,crop_,documentToLocal_,point);
    const float coverage=layerMaskCoverage(mask_,documentToLocal_.map(point));
    for(auto& channel:color)channel*=coverage;
    return color;
}

PinnedDocumentSampler::PinnedDocumentSampler(const Document& document,
    std::optional<LayerId> active, ColorSampleSource source, SampleFiltering filtering,
    std::span<const SampleCacheOverride> prepared, ActiveReferenceAppearance appearance, LayerId adjustmentProbe)
    : owner_(&document), snapshot_(document.snapshot()),
      activeOnly_(source == ColorSampleSource::ActiveLayer), filtering_(filtering)
{
    for (auto& layer : snapshot_.layersBottomToTop) {
        if(adjustmentProbe && sourceById_.contains(adjustmentProbe))break;
        if (source==ColorSampleSource::ActiveLayer && layer.id!=active) continue;
        if (layer.id!=adjustmentProbe && (source==ColorSampleSource::MergedVisible || appearance==ActiveReferenceAppearance::Rendered)
            && (!layer.visible || !std::isfinite(layer.opacity) || layer.opacity<=0)) continue;
        if (std::holds_alternative<AdjustmentLayer>(layer.payload)) {
            if (source==ColorSampleSource::ActiveLayer) continue;
            const auto inverse=layer.localToDocument.inverted();
            if (inverse) sources_.push_back({{},0,{},layer.opacity,BlendMode::Normal,*inverse,
                preparedAdjustments(layer.adjustments),{},{},{},layer.mask,layer.id,true});
            if(layer.id==adjustmentProbe)sourceById_[layer.id]=sources_.size()-1;
            continue;
        }
        const auto replacement = std::find_if(prepared.begin(),prepared.end(),
            [&](const auto& entry) { return entry.id == layer.id; });
        if (replacement != prepared.end()) layer.renderCache = replacement->cache;
        const bool intrinsic=source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic;
        if(intrinsic) {layer.filters.reset();layer.filterCache.reset();layer.effects.reset();layer.effectCache.reset();}
        else {
            if(adjustmentProbe && ((hasActiveSpatialFilters(layer.filters)&&!layerSpatialFilterCacheValid(layer)) || !layerEffectCacheValid(layer)))
                throw std::invalid_argument("Preparing lower-stack input");
            layer=prepareSpatialFilterLayer(layer);
        }
        const auto surface=renderedSurface(layer);
        if (!surface) throw std::invalid_argument("Reference layer has no prepared image");
        const auto inverse=renderTransform(layer).inverted();
        const auto local=layer.localToDocument.inverted();
        if (!inverse || !local) continue;
        sources_.push_back({surface,surface->revision(),*inverse,
            source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic
                ? 1.0 : std::clamp(double(layer.opacity),0.0,1.0),
            source==ColorSampleSource::ActiveLayer ? BlendMode::Normal : layer.blendMode,
            *local, source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Intrinsic
                ? nullptr : layerSpatialFilterCacheValid(layer)?nullptr:preparedAdjustments(layer.adjustments),layer.crop,
            hasActiveLayerEffects(layer.effects)?std::optional(compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer))):std::nullopt,layer.effectCache,
            intrinsic?LayerMaskState{}:layer.mask,layer.id});
    }
    if(source==ColorSampleSource::MergedVisible) {
        std::vector<LayerId> ids;
        std::vector<std::pair<LayerId,AdjustmentScope>> adjustments;
        for(const auto& l:document.layers()) {
            ids.push_back(l.id);
            if(const auto* a=std::get_if<AdjustmentLayer>(&l.payload))adjustments.emplace_back(l.id,a->scope);
        }
        if(hasClippingGroups(document.tree()) || !adjustments.empty())
            composition_=compositionPlan(document.tree(),ids,{},adjustments);
        for(std::size_t i=0;i<sources_.size();++i)sourceById_[sources_[i].id]=i;
    }
    if (source==ColorSampleSource::ActiveLayer && sources_.empty())
        throw std::invalid_argument("Reference requires an active layer with a prepared image and valid transform");
    if(active && source==ColorSampleSource::ActiveLayer && appearance==ActiveReferenceAppearance::Rendered) {
        const auto& tree=document.tree();auto id=*active;
        for(auto p=tree.placement(id);p&&p->parent;p=tree.placement(id)) {
            const auto* parent=tree.container(p->parent);if(!parent)break;
            if(parent->kind==ContainerKind::ClippingMaskGroup && parent->children.size()>1 && parent->children.front()!=id) {
                const auto base=parent->children.front();std::vector<Layer> layers;std::vector<const Layer*> refs;
                for(auto leaf:document.expandedLayers(std::array{base})) {
                    layers.push_back(*document.layer(leaf));auto& l=layers.back();l.effects.reset();l.effectCache.reset();l.visible=document.isEffectivelyVisible(leaf);
                }
                for(const auto& l:layers)refs.push_back(&l);
                clippingGates_.push_back(std::make_shared<PinnedDocumentSampler>(refs,document.canvas().extent,AffineTransform{},Vec2d{},&tree,std::array{base}));
            }
            id=parent->id;
        }
    }
}
bool PinnedDocumentSampler::validSample(Vec2d point) const noexcept
{
    const auto extent = snapshot_.canvas.extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= extent.width || point.y >= extent.height) return false;
    if (!activeOnly_) return true;
    try {for(const auto& gate:clippingGates_)if(gate->sampleLinear(point)[3]<=0)return false;}
    catch(...) {return false;}
    if (sources_.empty()) return false;
    point=point+sampleOrigin_;
    const auto& source = sources_.front();
    const auto derivatives=source.documentToLocal.derivatives(point);
    if(source.crop && layerCropCoverage(*source.crop,source.documentToLocal.map({std::floor(point.x)+.5,std::floor(point.y)+.5}),
        derivatives[0],derivatives[1])<=0)return false;
    if(source.effects&&source.effectCache){
        const auto p=source.documentToLocal.map(point);const auto& r=source.effectCache->visualBounds;
        return p.x>=r.x&&p.y>=r.y&&p.x<r.right()&&p.y<r.bottom();
    }
    const auto local = source.inverse.map({std::floor(point.x) + .5, std::floor(point.y) + .5});
    const auto size = source.surface->extent();
    return std::isfinite(local.x) && std::isfinite(local.y) && local.x >= 0 && local.y >= 0
        && local.x < size.width && local.y < size.height;
}
bool PinnedDocumentSampler::matches(const Document& document) const noexcept
{
    if (&document!=owner_ || document.revision()!=snapshot_.documentRevision) return false;
    return std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return !s.surface || s.surface->revision()==s.revision; });
}
std::vector<std::uint64_t> adjustmentInputKey(const Document& document,LayerId target)
{
    std::vector<LayerId> ids;
    std::vector<std::pair<LayerId,AdjustmentScope>> adjustments;
    for(const auto& l:document.layers()) {
        ids.push_back(l.id);
        if(const auto* a=std::get_if<AdjustmentLayer>(&l.payload))adjustments.emplace_back(l.id,a->scope);
    }
    const auto plan=compositionPlan(document.tree(),ids,{},adjustments);
    const CompositionNode* domain=&plan;
    const auto locate=[&](const auto& self,const CompositionNode& node)->void {
        if(node.id && node.id!=target && !document.tree().isAncestor(node.id,target))return;
        if(node.isolated || node.clipping)domain=&node;
        for(const auto& child:node.children)self(self,child);
    };
    locate(locate,plan);
    std::vector<std::uint64_t> key{document.canvas().extent.width,document.canvas().extent.height,domain->id};
    const auto number=[&](double x){key.push_back(std::bit_cast<std::uint64_t>(x));};
    const auto matrix=[&](const AffineTransform& m){for(auto x:{m.m00,m.m01,m.m02,m.m10,m.m11,m.m12,m.m20,m.m21,m.m22})number(x);};
    const auto walk=[&](const auto& self,const CompositionNode& node)->bool {
        key.insert(key.end(),{node.id,node.leaf,node.clipping,node.isolated,node.adjustment});
        if(node.leaf) {
            const auto* l=document.layer(node.id);if(!l)return false;
            matrix(l->localToDocument);
            if(node.id==target)return true;
            key.push_back(document.isEffectivelyVisible(l->id));number(l->opacity);
            key.insert(key.end(),{std::uint64_t(l->blendMode),l->payload.index(),l->textRevision,l->shapeRevision,
                l->adjustmentRevision,l->filterRevision,l->effectRevision,
                std::uint64_t(reinterpret_cast<std::uintptr_t>(l->mask.get()))});
            if(l->mask) {
                key.insert(key.end(),{l->mask->coverage->revision(),l->mask->enabled,l->mask->outside});
                matrix(l->mask->localToMask);
            }
            if(const auto* r=std::get_if<RasterLayer>(&l->payload);r&&r->surface) {
                key.insert(key.end(),{r->surface->id(),r->surface->revision()});
            }
            if(l->renderCache) {
                const auto& c=*l->renderCache;
                key.insert(key.end(),{c.surface?c.surface->id():0,c.surface?c.surface->revision():0,c.contentRevision});
                matrix(c.pixelsToLocal);
            }
            number(l->rasterOrigin.x);number(l->rasterOrigin.y);
            key.push_back(l->rasterEffectFrame.has_value());
            if(l->rasterEffectFrame)for(auto x:{l->rasterEffectFrame->x,l->rasterEffectFrame->y,l->rasterEffectFrame->width,l->rasterEffectFrame->height})number(x);
            key.push_back(l->crop.has_value());
            if(l->crop) {for(auto x:{l->crop->x,l->crop->y,l->crop->width,l->crop->height})number(x);for(auto x:l->crop->corners)number(x);}
        } else for(const auto& child:node.children)if(self(self,child))return true;
        key.push_back(0); // End of this node; IDs are nonzero.
        return false;
    };
    walk(walk,*domain);return key;
}
PinnedDocumentSampler::PinnedDocumentSampler(std::span<const Layer* const> layers,
    Extent2u extent,const AffineTransform& documentToSample,Vec2d sampleOrigin,const LayerTree* tree,std::span<const LayerId> selectedItems)
    : sampleOrigin_(sampleOrigin)
{
    snapshot_.canvas={extent,96};
    sources_.reserve(layers.size());
    for(const auto* original:layers) {
        if(!original || !original->visible || !std::isfinite(original->opacity) || original->opacity<=0)continue;
        if (std::holds_alternative<AdjustmentLayer>(original->payload)) {
            const auto inverse=composeAffine(documentToSample,original->localToDocument).inverted();
            if (inverse) sources_.push_back({{},0,{},original->opacity,BlendMode::Normal,*inverse,
                preparedAdjustments(original->adjustments),{},{},{},original->mask,original->id,true});
            continue;
        }
        std::optional<Layer> prepared;
        const auto* layer=original;
        if((hasActiveSpatialFilters(original->filters) && !layerSpatialFilterCacheValid(*original))||!layerEffectCacheValid(*original)) {
            prepared=prepareSpatialFilterLayer(*original); layer=&*prepared;
        }
        const auto surface=renderedSurface(*layer);
        if(!surface)throw std::invalid_argument("Reference layer has no prepared image");
        const auto inverse=composeAffine(documentToSample,renderTransform(*layer)).inverted();
        const auto local=composeAffine(documentToSample,layer->localToDocument).inverted();
        if(inverse && local)sources_.push_back({surface,surface->revision(),*inverse,std::clamp(double(layer->opacity),0.0,1.0),layer->blendMode,
            *local,layerSpatialFilterCacheValid(*layer)?nullptr:preparedAdjustments(layer->adjustments),layer->crop,
            hasActiveLayerEffects(layer->effects)?std::optional(compileLayerEffects(layer->effects,layerEffectReferenceFrame(*layer))):std::nullopt,layer->effectCache,layer->mask,layer->id});
    }
    {
        std::vector<LayerId> ids;
        std::vector<std::pair<LayerId,AdjustmentScope>> adjustments;
        for(const auto* layer:layers)if(layer) {
            ids.push_back(layer->id);
            if(const auto* a=std::get_if<AdjustmentLayer>(&layer->payload))adjustments.emplace_back(layer->id,a->scope);
        }
        LayerTree flat;flat.roots=ids;
        if((tree && hasClippingGroups(*tree)) || !adjustments.empty())
            composition_=compositionPlan(tree?*tree:flat,ids,selectedItems,adjustments);
        for(std::size_t i=0;i<sources_.size();++i)sourceById_[sources_[i].id]=i;
    }
}
PremultipliedColor PinnedDocumentSampler::sampleComposition(Vec2d point, std::span<const PremultipliedColor> samples,
    LayerId probe, std::size_t stopBefore) const
{
    PremultipliedColor captured{};
    bool reachedProbe=false;
    const auto raw=[&](LayerId id) -> PremultipliedColor {
        if(reachedProbe)return {};
        const auto it=sourceById_.find(id);if(it==sourceById_.end())return {};
        if(!samples.empty())return samples[it->second];
        const auto& s=sources_[it->second];std::size_t reads=0;
        if(s.adjustment)return {};
        auto c=sampleRaster(s.inverse,*s.surface,point,reads,filtering_);
        if(s.adjustments)c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(point));
        return c;
    };
    const auto leaf=[&](LayerId id,PremultipliedColor backdrop,bool contentOnly,const PremultipliedColor* replacement) {
        if(reachedProbe)return backdrop;
        const auto it=sourceById_.find(id);if(it==sourceById_.end())return backdrop;
        const auto& s=sources_[it->second];
        if(s.adjustment) {
            const auto local=s.documentToLocal.map(point);
            if(id==probe){captured=s.adjustments?evaluateAdjustments(*s.adjustments,backdrop,local,stopBefore):backdrop;reachedProbe=true;}
            return compositeAdjustment(s.adjustments.get(),backdrop,local,float(s.opacity)*layerMaskCoverage(s.mask,local));
        }
        auto c=raw(id);
        if(replacement)for(std::size_t i=0;i<3;++i)c[i]=(*replacement)[i]*c[3];
        const float coverage=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,point)[3]
            *layerMaskCoverage(s.mask,s.documentToLocal.map(point));
        if(s.effects&&!contentOnly)return compositeLayerEffects(backdrop,c,*s.effects,s.effectCache.get(),
            s.documentToLocal.map(point),float(s.opacity),s.blendMode,coverage);
        for(auto& channel:c)channel*=coverage;
        return compositeLayer(backdrop,c,float(s.opacity),s.blendMode);
    };
    const auto result=evaluateComposition(*composition_,{},leaf,raw);
    return probe?captured:result;
}
PremultipliedColor PinnedDocumentSampler::sampleAdjustmentInput(Vec2d point,LayerId id,std::size_t stopBefore) const
{
    return composition_?sampleComposition(point,{},id,stopBefore):PremultipliedColor{};
}
Rgba8 PinnedDocumentSampler::sample(Vec2d point) const
{
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
    return encode(sampleLinear({std::floor(point.x)+.5,std::floor(point.y)+.5}));
}
void PinnedDocumentSampler::sampleRow(std::int32_t x, std::int32_t y, std::span<PremultipliedColor> output,
    std::span<std::byte> scratch) const
{
    std::fill(output.begin(), output.end(), PremultipliedColor{});
    if (!std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return !s.surface || s.surface->revision()==s.revision; }))
        throw std::runtime_error("Image reference changed while rendering output");
    const auto extent=snapshot_.canvas.extent;
    if (y<0 || y>=std::int64_t(extent.height) || output.empty()) return;
    const auto first=std::max<std::int64_t>(0,-std::int64_t(x));
    const auto last=std::min<std::int64_t>(std::int64_t(output.size()),std::int64_t(extent.width)-x);
    const double documentX=double(x)+sampleOrigin_.x, documentY=double(y)+sampleOrigin_.y+.5;
    if(composition_) {
        constexpr std::size_t rowBudget=64ULL*1024*1024;
        const auto maxWidth=std::max<std::size_t>(1,rowBudget/(std::max<std::size_t>(sources_.size(),1)*sizeof(PremultipliedColor)));
        if(output.size()>maxWidth) {
            for(std::size_t offset=0;offset<output.size();offset+=maxWidth)
                sampleRow(x+std::int32_t(offset),y,output.subspan(offset,std::min(maxWidth,output.size()-offset)),scratch);
            return;
        }
        // Row-local source reads, not per-pixel virtual copies. Native aligned
        // sources retain exact samples and sparse rows are skipped outright.
        std::vector<PremultipliedColor> rows(output.size()*sources_.size());
        std::vector<std::pair<std::int64_t,std::int64_t>> ranges(sources_.size(),{0,0});
        std::vector<std::byte> bytes(output.size()*4);
        for(std::size_t si=0;si<sources_.size();++si) {
            if(sources_[si].adjustment){ranges[si]={first,last};continue;}
            const auto& s=sources_[si];const auto e=s.surface->extent();
            if(s.effects)ranges[si]={first,last};
            const bool aligned=s.inverse.isAffine() && s.inverse.m00==1 && s.inverse.m11==1 && s.inverse.m01==0 && s.inverse.m10==0
                && s.inverse.m02==std::floor(s.inverse.m02) && s.inverse.m12==std::floor(s.inverse.m12)
                && sampleOrigin_.x==std::floor(sampleOrigin_.x) && sampleOrigin_.y==std::floor(sampleOrigin_.y);
            auto begin=first,end=last;
            if(aligned) {
                const auto sy=double(y)+sampleOrigin_.y+s.inverse.m12;
                if(sy<0||sy>=e.height)continue;
                begin=std::max(begin,std::int64_t(-s.inverse.m02-documentX));
                end=std::min(end,std::int64_t(e.width-s.inverse.m02-documentX));
                if(end<=begin)continue;
                const auto span=std::span(bytes).first(std::size_t(end-begin)*4);
                s.surface->copyRgba8({int(documentX+double(begin)+s.inverse.m02),int(sy),int(end-begin),1},span,span.size());
            }
            if(!s.effects)ranges[si]={begin,end};
            for(auto i=begin;i<end;++i) {
                const Vec2d p{documentX+double(i)+.5,documentY};PremultipliedColor c;
                if(aligned) {const auto* v=bytes.data()+std::size_t(i-begin)*4;c=decodeColor({std::to_integer<std::uint8_t>(v[0]),std::to_integer<std::uint8_t>(v[1]),std::to_integer<std::uint8_t>(v[2]),std::to_integer<std::uint8_t>(v[3])});}
                else {std::size_t reads=0;c=sampleRaster(s.inverse,*s.surface,p,reads,filtering_);}
                if(s.adjustments)c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(p));
                rows[si*output.size()+std::size_t(i)]=c;
            }
        }
        const auto applyLeaf=[&](LayerId id,std::span<PremultipliedColor> dst,bool contentOnly,
            std::span<const PremultipliedColor> replacement,std::int64_t start,std::int64_t finish) {
            const auto found=sourceById_.find(id);if(found==sourceById_.end())return;
            const auto si=found->second;const auto& s=sources_[si];
            start=std::max(start,ranges[si].first);finish=std::min(finish,ranges[si].second);
            for(auto i=start;i<finish;++i) {
                const auto index=std::size_t(i);const Vec2d p{documentX+double(i)+.5,documentY};
                if(s.adjustment) {
                    const auto local=s.documentToLocal.map(p);
                    dst[index]=compositeAdjustment(s.adjustments.get(),dst[index],local,float(s.opacity)*layerMaskCoverage(s.mask,local));
                    continue;
                }
                auto c=rows[si*output.size()+index];
                if(!replacement.empty())for(std::size_t j=0;j<3;++j)c[j]=replacement[index][j]*c[3];
                const auto local=s.documentToLocal.map(p);
                const float coverage=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,p)[3]*layerMaskCoverage(s.mask,local);
                if(s.effects&&!contentOnly)dst[index]=compositeLayerEffects(dst[index],c,*s.effects,s.effectCache.get(),local,float(s.opacity),s.blendMode,coverage);
                else {for(auto& channel:c)channel*=coverage;dst[index]=compositeLayer(dst[index],c,float(s.opacity),s.blendMode);}
            }
        };
        const auto renderRow=[&](const auto& self,const CompositionNode& node,std::span<PremultipliedColor> dst,
            bool contentOnly,std::int64_t start,std::int64_t finish,bool inDomain=false)->void {
            if(node.leaf){applyLeaf(node.id,dst,contentOnly,{},start,finish);return;}
            if(node.isolated && !inDomain) {
                std::vector<PremultipliedColor> local(output.size());
                self(self,node,local,contentOnly,start,finish,true);
                for(auto i=start;i<finish;++i)dst[std::size_t(i)]=compositeLayer(dst[std::size_t(i)],local[std::size_t(i)],1,BlendMode::Normal);
                return;
            }
            if(!node.clipping){for(const auto& child:node.children)self(self,child,dst,contentOnly,start,finish);return;}
            const auto& base=node.children.front();
            if(base.adjustment)return;
            std::vector<PremultipliedColor> working(output.size()),styled,content;
            if(base.leaf) {
                const auto found=sourceById_.find(base.id);if(found==sourceById_.end())return;
                const auto si=found->second;
                start=std::max(start,ranges[si].first);finish=std::min(finish,ranges[si].second);
                for(auto i=start;i<finish;++i)working[std::size_t(i)]=clippingOpaque(rows[si*output.size()+std::size_t(i)]);
            }else {
                styled.resize(output.size());content.resize(output.size());
                self(self,base,styled,contentOnly,start,finish);
                if(contentOnly)content=styled;else self(self,base,content,true,start,finish);
                for(auto i=start;i<finish;++i)working[std::size_t(i)]=clippingOpaque(styled[std::size_t(i)]);
            }
            for(std::size_t i=1;i<node.children.size();++i)self(self,node.children[i],working,contentOnly,start,finish);
            if(base.leaf)applyLeaf(base.id,dst,contentOnly,working,start,finish);
            else for(auto i=start;i<finish;++i) {
                const auto j=std::size_t(i);dst[j]=compositeLayer(dst[j],clippingContainerResult(styled[j],content[j],working[j]),1,BlendMode::Normal);
            }
        };
        renderRow(renderRow,*composition_,output,false,first,last);
        return;
    }
    std::vector<std::byte> owned;
    if(scratch.size()<output.size()*4) {owned.resize(output.size()*4);scratch=owned;}
    for (const auto& s : sources_) {
        const auto e=s.surface->extent();
        // Intersect this output row with the inverse-mapped source quad before
        // touching pixels. Sparse separated layers do not scan the union N times.
        double lo=documentX+double(first)+.5, hi=documentX+double(last)-.5;
        const auto clipAxis=[&](double a,double b,double size) {
            if (a==0) return b>=0 && b<size;
            const auto p=-b/a,q=(size-b)/a;
            lo=std::max(lo,std::min(p,q)); hi=std::min(hi,std::max(p,q));
            return hi>=lo-1; // Conservative admission; exact coverage is sampled below.
        };
        const auto r=s.effects&&s.effectCache?s.effectCache->visualBounds:RectD{0,0,double(e.width),double(e.height)};
        const auto& admission=s.effects?s.documentToLocal:s.inverse;
        if (first>=last || (admission.isAffine() && (!clipAxis(admission.m00,admission.m01*documentY+admission.m02-r.x,r.width)
            || !clipAxis(admission.m10,admission.m11*documentY+admission.m12-r.y,r.height)))) continue;
        // Division used for row admission can round to the other side of an
        // exact edge. Admit one extra pixel; sampleRaster's original inverse
        // evaluation remains authoritative for the final coverage decision.
        const auto begin=std::max(first,std::int64_t(std::ceil(lo-documentX-.5))-1);
        const auto end=std::min(last,std::int64_t(std::floor(hi-documentX-.5))+2);
        if (end<=begin) continue;
        const bool aligned=!s.effects && s.inverse.isAffine() && s.inverse.m00==1 && s.inverse.m11==1 && s.inverse.m01==0 && s.inverse.m10==0
            && s.inverse.m02==std::floor(s.inverse.m02) && s.inverse.m12==std::floor(s.inverse.m12)
            && sampleOrigin_.x==std::floor(sampleOrigin_.x) && sampleOrigin_.y==std::floor(sampleOrigin_.y);
        auto b=begin, stop=end;
        if (aligned) {
            b=std::max(b,std::int64_t(-s.inverse.m02-documentX));
            stop=std::min(stop,std::int64_t(e.width-s.inverse.m02-documentX));
            const auto sourceY=double(y)+sampleOrigin_.y+s.inverse.m12;
            if (b>=stop || sourceY<0 || sourceY>=e.height) continue;
            const auto bytes=scratch.first(std::size_t(stop-b)*4);
            s.surface->copyRgba8({int(documentX+double(b)+s.inverse.m02),int(sourceY),int(stop-b),1},bytes,bytes.size());
        }
        for (auto i=b;i<stop;++i) {
            const Vec2d p{documentX+double(i)+.5,documentY};
            PremultipliedColor c;
            if (aligned) {
                const auto* v=scratch.data()+std::size_t(i-b)*4;
                c=decodeColor({std::to_integer<std::uint8_t>(v[0]),std::to_integer<std::uint8_t>(v[1]),
                    std::to_integer<std::uint8_t>(v[2]),std::to_integer<std::uint8_t>(v[3])});
            } else { std::size_t reads=0; c=sampleRaster(s.inverse,*s.surface,p,reads); }
            if (s.adjustments) c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(p));
            if(s.effects){
                const float crop=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,p)[3]*layerMaskCoverage(s.mask,s.documentToLocal.map(p));
                output[std::size_t(i)]=compositeLayerEffects(output[std::size_t(i)],c,*s.effects,s.effectCache.get(),s.documentToLocal.map(p),float(s.opacity),s.blendMode,crop);
            }else{
                c=applyLayerCrop(c,s.crop,s.documentToLocal,p);
                const float coverage=layerMaskCoverage(s.mask,s.documentToLocal.map(p));
                for(auto& channel:c)channel*=coverage;
                output[std::size_t(i)]=compositeLayer(output[std::size_t(i)],c,float(s.opacity),s.blendMode);
            }
        }
    }
    for(const auto& gate:clippingGates_)for(auto i=first;i<last;++i) {
        const auto coverage=gate->sampleLinear({double(x)+double(i)+.5,double(y)+.5})[3];
        for(auto& channel:output[std::size_t(i)])channel*=coverage;
    }
}

PremultipliedColor PinnedDocumentSampler::sampleLinear(Vec2d point) const
{
    if (!std::all_of(sources_.begin(),sources_.end(),[](const auto& s) { return !s.surface || s.surface->revision()==s.revision; }))
        throw std::runtime_error("Image reference changed; selection unchanged");
    const auto extent=snapshot_.canvas.extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x<0 || point.y<0
        || point.x>=extent.width || point.y>=extent.height) return {};
    point=point+sampleOrigin_;
    if(composition_)return sampleComposition(point);
    LinearColor merged{}; std::size_t reads=0;
    for (const auto& s:sources_) {
        auto c=sampleRaster(s.inverse,*s.surface,point,reads,filtering_);
        if (s.adjustments) c=evaluateAdjustments(*s.adjustments,c,s.documentToLocal.map(point));
        if(s.effects){
            const float crop=applyLayerCrop({1,1,1,1},s.crop,s.documentToLocal,point)[3]*layerMaskCoverage(s.mask,s.documentToLocal.map(point));
            merged=compositeLayerEffects(merged,c,*s.effects,s.effectCache.get(),s.documentToLocal.map(point),float(s.opacity),s.blendMode,crop);
        }else{
            c=applyLayerCrop(c,s.crop,s.documentToLocal,point);
            const float coverage=layerMaskCoverage(s.mask,s.documentToLocal.map(point));
            for(auto& channel:c)channel*=coverage;
            merged = compositeLayer(merged,c,float(s.opacity),s.blendMode);
        }
    }
    for(const auto& gate:clippingGates_) {
        const auto coverage=gate->sampleLinear(point)[3];for(auto& channel:merged)channel*=coverage;
    }
    return merged;
}

PreparedRasterSampler::PreparedRasterSampler(const Layer& layer)
{
    const auto* raster = std::get_if<RasterLayer>(&layer.payload);
    const auto inverse = layer.localToDocument.inverted();
    if (raster && raster->surface && inverse) { surface_ = raster->surface.get(); inverse_ = *inverse; }
}
Rgba8 PreparedRasterSampler::sample(Vec2d point) const
{
    if (!surface_) return {};
    if(!inverse_.isAffine()){std::size_t reads=0;return encode(sampleRaster(inverse_,*surface_,point,reads));}
    const auto local = inverse_.map(point);
    const auto extent = surface_->extent();
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || local.x < 0 || local.y < 0
        || local.x >= extent.width || local.y >= extent.height) return {};
    const double x = std::clamp(local.x - 0.5, 0.0, double(extent.width - 1));
    const double y = std::clamp(local.y - 0.5, 0.0, double(extent.height - 1));
    if (x == std::floor(x) && y == std::floor(y)) {
        std::array<std::byte, 4> bytes;
        surface_->copyRgba8({int(x), int(y), 1, 1}, bytes, 4);
        return {std::to_integer<std::uint8_t>(bytes[0]), std::to_integer<std::uint8_t>(bytes[1]),
            std::to_integer<std::uint8_t>(bytes[2]), std::to_integer<std::uint8_t>(bytes[3])};
    }
    std::size_t reads = 0;
    return encode(sampleRaster(inverse_, *surface_, point, reads));
}

std::optional<LayerId> hitTestRasterLayer(const Document& document, Vec2d point)
{
    const auto extent = document.canvas().extent;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= extent.width || point.y >= extent.height)
        return {};
    const auto& layers = document.layers();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        if (!document.isEffectivelyVisible(it->id) || !std::isfinite(it->opacity) || it->opacity <= 0)
            continue;
        if(!hitLayerCrop(*it,point))continue;
        if(clippingVisibility(document,it->id,point)<=0)continue;
        if(const auto inv=it->localToDocument.inverted();inv&&layerMaskCoverage(it->mask,inv->map(point))<=0)continue;
        if(hitTextBounds(*it,point))return it->id;
        if (const auto* shape = std::get_if<ShapeLayer>(&it->payload)) {
            const auto inverse = it->localToDocument.inverted();
            if (inverse && shapeContainsPoint(*shape, inverse->map(point))) return it->id;
            // Exterior strokes use the prepared rendering, not a bounding box.
            // UI path hit-testing separately adds screen-space line tolerance.
            const auto surface = renderedSurface(*it);
            std::size_t reads = 0;
            if (surface && sampleLayer(*it, *surface, point, reads)[3] > 0) return it->id;
            continue;
        }
        const auto* raster = std::get_if<RasterLayer>(&it->payload);
        std::size_t reads = 0;
        if (raster && raster->surface && sampleLayer(*it, *raster->surface, point, reads)[3] > 0)
            return it->id;
    }
    return {};
}

ColorSample sampleDocumentColor(const Document& document,
    std::optional<LayerId> activeLayer, Vec2d documentPosition, ColorSampleSource source)
{
    ColorSample result;
    const auto extent = document.canvas().extent;
    if (!std::isfinite(documentPosition.x) || !std::isfinite(documentPosition.y)
        || documentPosition.x < 0.0 || documentPosition.y < 0.0
        || documentPosition.x >= extent.width || documentPosition.y >= extent.height) {
        return result;
    }
    const Vec2d point {std::floor(documentPosition.x) + 0.5,
        std::floor(documentPosition.y) + 0.5};
    result.status = ColorSampleStatus::Available;
    if (source == ColorSampleSource::ActiveLayer) {
        const auto* layer = activeLayer ? document.layer(*activeLayer) : nullptr;
        const auto surface=layer?intrinsicSurface(*layer):nullptr;
        if (!surface) {
            result.status = ColorSampleStatus::NoActiveRaster;
            return result;
        }
        result.layersVisited = 1;
        const auto inverse=composeAffine(layer->localToDocument,intrinsicPixelsToLocal(*layer)).inverted();
        result.color = inverse?encode(sampleRaster(*inverse,*surface,point,result.texelsRead)):Rgba8{};
        return result;
    }
    if(hasClippingGroups(document.tree()) || std::ranges::any_of(document.layers(),[](const auto& l){return std::holds_alternative<AdjustmentLayer>(l.payload);})) {
        for(const auto& layer:document.layers())if(document.isEffectivelyVisible(layer.id)&&layer.opacity>0&&!std::holds_alternative<AdjustmentLayer>(layer.payload))
            if(!renderedSurface(layer) || (hasActiveSpatialFilters(layer.filters)&&!layerSpatialFilterCacheValid(layer)) || !layerEffectCacheValid(layer)) {
                result.status=ColorSampleStatus::UnsupportedLayer;return result;
            }
        result.color=PinnedDocumentSampler(document,{},ColorSampleSource::MergedVisible).sample(point);
        result.layersVisited=document.layers().size();return result;
    }
    LinearColor merged {};
    for (const auto& original : document.layers()) {
        ++result.layersVisited;
        if (!document.isEffectivelyVisible(original.id) || !std::isfinite(original.opacity) || original.opacity <= 0.0F) {
            continue;
        }
        // Eyedropper hover is demand-driven input, not an output-preparation
        // operation. Never perform a full spatial convolution on its UI thread;
        // the canvas coordinator publishes the shared cache asynchronously.
        if((hasActiveSpatialFilters(original.filters)&&!layerSpatialFilterCacheValid(original))||!layerEffectCacheValid(original)) {
            result.status=ColorSampleStatus::UnsupportedLayer;return result;
        }
        const auto& layer=original;
        const auto surface=renderedSurface(layer);
        if (!surface) {
            if(std::holds_alternative<RasterLayer>(layer.payload))continue;
            // Editable text/shapes must prepare their disposable image first.
            // Never silently return an incomplete merged result.
            result.status = ColorSampleStatus::UnsupportedLayer;
            return result;
        }
        auto color = sampleLayer(layer, *surface, point, result.texelsRead);
        if (const auto compiled=layerSpatialFilterCacheValid(layer)?nullptr:preparedAdjustments(layer.adjustments)) {
            if (const auto local=layer.localToDocument.inverted())
                color=evaluateAdjustments(*compiled,color,local->map(point));
        }
        if(hasActiveLayerEffects(layer.effects)){
            const auto inverse=*layer.localToDocument.inverted();
            const float crop=applyLayerCrop({1,1,1,1},layer.crop,inverse,point)[3]*layerMaskCoverage(layer.mask,inverse.map(point));
            merged=compositeLayerEffects(merged,color,compileLayerEffects(layer.effects,layerEffectReferenceFrame(layer)),layer.effectCache.get(),inverse.map(point),layer.opacity,layer.blendMode,crop);
        }else{
            if(const auto inverse=layer.localToDocument.inverted())color=applyLayerCrop(color,layer.crop,*inverse,point);
            if(const auto inverse=layer.localToDocument.inverted()) {
                const float coverage=layerMaskCoverage(layer.mask,inverse->map(point));
                for(auto& channel:color)channel*=coverage;
            }
            merged = compositeLayer(merged,color,layer.opacity,layer.blendMode);
        }
    }
    result.color = encode(merged);
    return result;
}

float clippingVisibility(const Document& doc,LayerId id,Vec2d point)
{
    float coverage=1;
    const auto& tree=doc.tree();
    for(auto p=tree.placement(id);p&&p->parent;p=tree.placement(id)) {
        const auto* parent=tree.container(p->parent);if(!parent)break;
        if(parent->kind==ContainerKind::ClippingMaskGroup && parent->children.size()>1 && parent->children.front()!=id) {
            const auto base=parent->children.front();
            if(!doc.isEffectivelyVisible(base))return 0;
            if(const auto* original=doc.layer(base)) {
                if(original->opacity<=0)return 0;
                auto layer=*original;layer.effects.reset();layer.effectCache.reset();
                coverage*=PreparedLayerSampler(layer).sample(point)[3]*original->opacity;
            }else {
                std::vector<Layer> layers;std::vector<const Layer*> refs;
                for(auto leaf:tree.descendants(base)) {
                    layers.push_back(*doc.layer(leaf));auto& l=layers.back();l.effects.reset();l.effectCache.reset();l.visible=doc.isEffectivelyVisible(leaf);
                }
                for(const auto& l:layers)refs.push_back(&l);
                coverage*=PinnedDocumentSampler(refs,{1,1},{},{point.x-.5,point.y-.5},&tree).sampleLinear({.5,.5})[3];
            }
        }
        id=parent->id;
    }
    return coverage;
}
} // namespace imageeditor::core
