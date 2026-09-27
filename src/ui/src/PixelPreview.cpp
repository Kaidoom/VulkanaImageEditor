#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SpatialFilterCache.hpp"
#include <QTimer>
#include <atomic>
#include <future>
#include <map>
#include <set>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
constexpr std::uint64_t inputBudget=256ULL*1024*1024;
struct SourceStamp { core::SurfaceId id{};core::Revision revision{};friend bool operator==(const SourceStamp&,const SourceStamp&)=default; };
struct Input {
    core::DocumentSnapshot snapshot;
    std::vector<SourceStamp> stamps;
};
bool same(const Input& a,const Input& b)
{
    if(a.snapshot.canvas!=b.snapshot.canvas || a.stamps!=b.stamps
        || a.snapshot.layersBottomToTop.size()!=b.snapshot.layersBottomToTop.size())return false;
    for(std::size_t i=0;i<a.snapshot.layersBottomToTop.size();++i) {
        const auto& x=a.snapshot.layersBottomToTop[i];const auto& y=b.snapshot.layersBottomToTop[i];
        if(x.id!=y.id || x.visible!=y.visible || x.opacity!=y.opacity || x.blendMode!=y.blendMode
            || x.localToDocument!=y.localToDocument || x.rasterOrigin!=y.rasterOrigin || x.rasterEffectFrame!=y.rasterEffectFrame || x.crop!=y.crop || x.adjustments!=y.adjustments
            || x.filters!=y.filters || x.effects!=y.effects || x.payload.index()!=y.payload.index())return false;
        if(const auto* t=std::get_if<core::TextLayer>(&x.payload);t && *t!=std::get<core::TextLayer>(y.payload))return false;
        if(const auto* s=std::get_if<core::ShapeLayer>(&x.payload);s && *s!=std::get<core::ShapeLayer>(y.payload))return false;
    }
    return true;
}
Input capture(const core::DocumentSnapshot& snapshot)
{
    if(snapshot.layersBottomToTop.size()>1024)throw std::runtime_error("Pixel Preview exceeds the layer limit");
    const auto e=snapshot.canvas.extent;
    if(e.empty() || e.width>32768 || e.height>32768 || std::uint64_t(e.width)*e.height>64ULL*1024*1024)
        throw std::runtime_error("Pixel Preview exceeds the native-output budget");
    std::size_t metadata=0;
    for(const auto& l:snapshot.layersBottomToTop) {
        metadata+=sizeof(l);
        if(const auto* t=std::get_if<core::TextLayer>(&l.payload))metadata+=core::textMemoryCost(*t);
        if(const auto* s=std::get_if<core::ShapeLayer>(&l.payload))metadata+=core::shapeMemoryCost(*s);
    }
    if(metadata>16ULL*1024*1024)throw std::runtime_error("Pixel Preview exceeds the metadata budget");
    Input result;result.snapshot.canvas=snapshot.canvas;
    for(const auto& l:snapshot.layersBottomToTop) {
        if(!l.visible || l.opacity<=0)continue;
        result.snapshot.layersBottomToTop.push_back(l);
        auto& copy=result.snapshot.layersBottomToTop.back();
        copy.name.clear();copy.renderCache.reset();
        if(!std::holds_alternative<core::RasterLayerSnapshot>(l.payload) || !core::layerSpatialFilterCacheValid(l))copy.filterCache.reset();
        SourceStamp stamp;
        if(const auto* r=std::get_if<core::RasterLayerSnapshot>(&l.payload)) {
            if(!r->surface)throw std::runtime_error("Pixel Preview source has no pixels");
            stamp={r->surface->id(),r->surface->revision()};
        }
        result.stamps.push_back(stamp);
    }
    return result;
}
}
struct PixelPreview::State {
    QTimer timer;
    bool enabled=false;
    std::optional<Input> pending;
    std::uint64_t generation=0,completed=0,frozen=0;
    Profile profile;
    std::uint64_t instance=0, clock=0;
    struct Cached { Input input; std::shared_ptr<const core::RasterSurface> pixels; std::uint64_t used=0; };
    std::map<std::uint64_t,Cached> cache;
    struct Frozen { core::Revision revision;std::shared_ptr<core::RasterSurface> surface; };
    std::map<core::SurfaceId,Frozen> sources;
    struct Job {
        std::atomic_bool cancel{false};
        std::uint64_t generation;
        std::unique_ptr<core::Document> document;
        std::shared_ptr<const core::RasterSurface> result;
        QString error;
        Profile profile;
    };
    std::shared_ptr<Job> job;
    std::future<void> worker;
};
PixelPreview::PixelPreview(QObject* parent):QObject(parent),state_(std::make_unique<State>())
{
    state_->timer.setSingleShot(true);
    connect(&state_->timer,&QTimer::timeout,this,[this]{advance();});
}
PixelPreview::~PixelPreview(){setEnabled(false);if(state_->worker.valid())state_->worker.wait();}
bool PixelPreview::busy() const{return state_->job || state_->timer.isActive();}
std::uint64_t PixelPreview::completedRenders() const{return state_->completed;}
std::uint64_t PixelPreview::frozenBytes() const{return state_->frozen;}
PixelPreview::Profile PixelPreview::lastProfile() const{return state_->profile;}
void PixelPreview::setDocumentInstance(std::uint64_t instance)
{
    if(state_->instance==instance)return;
    state_->instance=instance;++state_->generation;
    if(state_->job)state_->job->cancel=true;
    state_->pending.reset();state_->sources.clear();state_->frozen=0;
    state_->timer.stop();if(state_->job)state_->timer.start(20);
    if(onReady)onReady({},{});
}
void PixelPreview::forgetDocument(std::uint64_t instance){state_->cache.erase(instance);}
void PixelPreview::setEnabled(bool enabled)
{
    if(state_->enabled==enabled)return;
    state_->enabled=enabled;++state_->generation;
    if(state_->job)state_->job->cancel=true;
    state_->pending.reset();state_->sources.clear();state_->frozen=0;
    state_->timer.stop();
    if(state_->job)state_->timer.start(20);
    if(!enabled && onReady)onReady({},{});
}
void PixelPreview::request(const core::DocumentSnapshot& snapshot)
{
    if(!state_->enabled)return;
    if(snapshot.canvas.extent.empty()) {
        ++state_->generation;state_->pending.reset();if(state_->job)state_->job->cancel=true;
        if(onReady)onReady({},{});
        return;
    }
    try {
        auto input=capture(snapshot);
        if(state_->pending && same(*state_->pending,input))return;
        const bool replaced=state_->pending && (state_->pending->snapshot.canvas!=input.snapshot.canvas
            || (!state_->pending->snapshot.layersBottomToTop.empty() && !input.snapshot.layersBottomToTop.empty()
                && state_->pending->snapshot.layersBottomToTop.front().id!=input.snapshot.layersBottomToTop.front().id));
        state_->pending=std::move(input);++state_->generation;
        if(state_->job)state_->job->cancel=true;
        if(auto cached=state_->cache.find(state_->instance);cached!=state_->cache.end() && same(cached->second.input,*state_->pending)) {
            cached->second.used=++state_->clock;
            if(onReady)onReady(cached->second.pixels,{});
            return;
        }
        if(!state_->timer.isActive())state_->timer.start(state_->job?20:70);
        if(replaced && onReady)onReady({},{});
    }catch(const std::exception& e){
        ++state_->generation;state_->pending.reset();if(state_->job)state_->job->cancel=true;
        if(onReady)onReady({},QString::fromUtf8(e.what()));
    }
}
void PixelPreview::advance()
{
    if(state_->job) {
        if(state_->worker.wait_for(std::chrono::seconds(0))!=std::future_status::ready){state_->timer.start(20);return;}
        state_->worker.get();auto job=std::move(state_->job);
        if(state_->enabled && job->generation==state_->generation && !job->cancel) {
            if(job->result)++state_->completed;
            if(job->result && state_->pending) {
                auto key=*state_->pending;
                // The warm-result key needs content/settings, not disposable
                // derived surfaces. Do not pin evicted inactive filter/style
                // caches through this independent native-output cache.
                for(auto& layer:key.snapshot.layersBottomToTop) {
                    layer.renderCache.reset();layer.filterCache.reset();layer.effectCache.reset();
                }
                state_->cache.insert_or_assign(state_->instance,State::Cached{std::move(key),job->result,++state_->clock});
                std::uint64_t bytes=0;
                for(const auto& [id,cached]:state_->cache){(void)id;const auto e=cached.pixels->extent();bytes+=std::uint64_t(e.width)*e.height*4;}
                while(bytes>inputBudget || state_->cache.size()>16) {
                    auto oldest=std::min_element(state_->cache.begin(),state_->cache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
                    if(oldest==state_->cache.end())break;
                    const auto e=oldest->second.pixels->extent();bytes-=std::uint64_t(e.width)*e.height*4;state_->cache.erase(oldest);
                }
            }
            state_->profile=job->profile;
            if(onReady)onReady(std::move(job->result),job->error);
            return;
        }
    }
    if(!state_->enabled || !state_->pending)return;
    if(const auto cached=state_->cache.find(state_->instance);cached!=state_->cache.end() && same(cached->second.input,*state_->pending)) return;
    try {
        const auto started=std::chrono::steady_clock::now();
        const auto& input=*state_->pending;
        std::uint64_t bytes=0;std::set<core::SurfaceId> live;
        for(const auto& l:input.snapshot.layersBottomToTop)if(const auto* r=std::get_if<core::RasterLayerSnapshot>(&l.payload)) {
            if(live.insert(r->surface->id()).second){const auto e=r->surface->extent();bytes+=std::uint64_t(e.width)*e.height*4;}
        }
        if(bytes>inputBudget)throw std::runtime_error("Pixel Preview exceeds the frozen-source budget (256 MiB)");
        std::erase_if(state_->sources,[&](const auto& entry){return !live.contains(entry.first);});
        auto job=std::make_shared<State::Job>();job->generation=state_->generation;
        job->document=std::make_unique<core::Document>(input.snapshot.canvas);
        for(std::size_t i=0;i<input.snapshot.layersBottomToTop.size();++i) {
            const auto& s=input.snapshot.layersBottomToTop[i];
            core::Layer layer;layer.id=s.id;layer.name="Pixel Preview source";
            layer.visible=s.visible;layer.opacity=s.opacity;layer.blendMode=s.blendMode;
            layer.localToDocument=s.localToDocument;layer.crop=s.crop;
            layer.rasterOrigin=s.rasterOrigin;layer.rasterEffectFrame=s.rasterEffectFrame;
            layer.adjustments=s.adjustments;layer.filters=s.filters;layer.effects=s.effects;
            if(const auto* r=std::get_if<core::RasterLayerSnapshot>(&s.payload)) {
                const auto stamp=input.stamps[i];
                if(r->surface->revision()!=stamp.revision)throw std::runtime_error("Pixel Preview source changed before capture");
                auto it=state_->sources.find(stamp.id);
                if(it==state_->sources.end() || it->second.revision!=stamp.revision) {
                    const auto e=r->surface->extent();const auto stride=std::size_t(e.width)*4;
                    std::vector<std::byte> copy(stride*e.height);
                    r->surface->copyRgba8({0,0,int(e.width),int(e.height)},copy,stride);
                    auto frozen=std::make_shared<core::ContiguousRasterSurface>(e,std::move(copy));
                    it=state_->sources.insert_or_assign(stamp.id,State::Frozen{stamp.revision,std::move(frozen)}).first;
                }
                layer.payload=core::RasterLayer{it->second.surface};
                if(s.filterCache) {
                    // The frozen clone has exactly the original source bytes.
                    // Re-key a canonical immutable raster filter result, never
                    // a viewport-density typed cache, to that private identity.
                    auto filtered=std::make_shared<core::LayerSpatialFilterCache>(*s.filterCache);
                    filtered->sourceId=it->second.surface->id();filtered->sourceRevision=it->second.surface->revision();
                    layer.filterCache=std::move(filtered);
                }
            }else if(const auto* t=std::get_if<core::TextLayer>(&s.payload))layer.payload=*t;
            else layer.payload=std::get<core::ShapeLayer>(s.payload);
            if(!job->document->insertLayer(job->document->layers().size(),std::move(layer)))
                throw std::runtime_error("Invalid Pixel Preview layer input");
        }
        state_->frozen=bytes;
        job->profile.freezeMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        state_->worker=std::async(std::launch::async,[job]{
            try {
                auto output=flattenDocument(*job->document,[&](auto,auto){return !job->cancel.load();},{},&job->profile.evaluation);
                const auto transfer=std::chrono::steady_clock::now();
                if(output && !job->cancel)job->result=surfaceFromNativeImage(output.image);
                else if(!output.cancelled)job->error=output.error;
                job->profile.transferMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-transfer).count();
            }catch(const std::exception& e){job->error=QString::fromUtf8(e.what());}
            job->document.reset();
        });
        state_->job=std::move(job);
        state_->timer.start(20);
    }catch(const std::exception& e){if(onReady)onReady({},QString::fromUtf8(e.what()));}
}
}
