#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

// Executable-local allocation failure injection; no shipping hooks or Qt.
namespace allocation {
thread_local bool armed = false;
thread_local std::size_t remaining = 0;
void beforeAllocation()
{
    if (armed && remaining-- == 0) throw std::bad_alloc();
}
struct Scope {
    explicit Scope(std::size_t allowed) { armed = true; remaining = allowed; }
    ~Scope() { armed = false; }
};
}
void* operator new(std::size_t size)
{
    allocation::beforeAllocation();
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    allocation::beforeAllocation();
    void* p = nullptr;
    if (posix_memalign(&p, std::size_t(alignment), size ? size : 1) == 0) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

using namespace imageeditor::core;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

struct Fixture {
    EditorSession session;
    std::array<LayerId, 3> ids;
    LayerId group = makeLayerId();
    Fixture()
    {
        auto document = std::make_unique<Document>(CanvasSpec { .extent = { 32, 32 } });
        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto layer = Layer::raster("Authoritative raster layer " + std::to_string(i),
                std::make_shared<ContiguousRasterSurface>(Extent2u { 8, 8 }, Rgba8 { 100, 90, 80, 160 }));
            ids[i] = layer.id;
            CHECK(document->insertLayer(i, std::move(layer)));
        }
        session.replaceDocument(std::move(document));
        CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(ids[2], 0.5F)));
        CHECK(session.undo()); // A redo branch must survive failed structural admission.
        session.setLayerSelection(std::array { ids[1], ids[0] }, ids[0], ids[2]);
    }
    std::unique_ptr<Command> command(bool merge = false)
    {
        if(merge) {
            auto baked=Layer::raster("Pre-rendered merge",std::make_shared<ContiguousRasterSurface>(Extent2u{8,8},Rgba8{50,80,90,255}));
            return consolidateLayerItems(*session.document(),session.layerSelectionState(),std::move(baked));
        }
        auto tree = session.document()->tree();
        tree.roots = { group, ids[2] };
        tree.containers.push_back({ group, "A newly created container with a non-SSO name",
            ContainerKind::Group, ColorLabel::Blue, { ids[0], ids[1] } });
        const LayerSelectionState before { session.selectedLayers(), session.activeLayer(), session.selectionAnchor() };
        const LayerSelectionState after { { group }, group, group };
        return std::make_unique<LayerStructureCommand>("Group layers", *session.document(), std::move(tree),
            std::vector<LayerId> {}, std::vector<Layer> {}, before, after);
    }
};

struct Snapshot {
    LayerTree tree;
    LayerSelectionState selection;
    Revision documentRevision;
    std::uint64_t content;
    std::size_t undo, redo, memory;
    std::array<std::shared_ptr<RasterSurface>, 3> surfaces;
    std::array<Revision, 3> surfaceRevisions;
    explicit Snapshot(const Fixture& f)
        : tree(f.session.document()->tree())
        , selection { f.session.selectedLayers(), f.session.activeLayer(), f.session.selectionAnchor() }
        , documentRevision(f.session.document()->revision())
        , content(f.session.document()->contentState())
        , undo(f.session.history().undoDepth()), redo(f.session.history().redoDepth()), memory(f.session.history().memoryUsed())
    {
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            if(const auto* layer=f.session.document()->layer(f.ids[i])) {
                surfaces[i] = std::get<RasterLayer>(layer->payload).surface;
                surfaceRevisions[i] = surfaces[i]->revision();
            }
        }
    }
    void checkUnchanged(const Fixture& f) const
    {
        const auto& document = *f.session.document();
        CHECK(document.tree() == tree);
        CHECK(document.revision() == documentRevision && document.contentState() == content);
        CHECK(f.session.history().undoDepth() == undo && f.session.history().redoDepth() == redo);
        CHECK(f.session.history().memoryUsed() == memory);
        CHECK(f.session.selectedLayers() == selection.ids && f.session.activeLayer() == selection.primary
            && f.session.selectionAnchor() == selection.anchor);
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            if(!surfaces[i]) { CHECK(!document.layer(f.ids[i]));continue; }
            CHECK(document.layer(f.ids[i]) != nullptr);
            if (!document.layer(f.ids[i])) continue;
            CHECK(std::get<RasterLayer>(document.layer(f.ids[i])->payload).surface == surfaces[i]);
            CHECK(surfaces[i]->revision() == surfaceRevisions[i]);
            CHECK(surfaces[i]->dirtySince(surfaceRevisions[i]).empty());
        }
    }
};

enum class Operation { Execute, Undo, Redo };
void mergePreparationAllocationSweep()
{
    std::size_t failurePoints=0;
    bool completed=false;
    for(std::size_t allowed=0;allowed<512;++allowed) {
        Fixture f;
        f.session.setLayerSelection(std::array{f.ids[2],f.ids[0]},f.ids[0],f.ids[2]);
        const Snapshot before(f);
        std::unique_ptr<Command> command;
        bool failed=false;
        {
            allocation::Scope failAfter(allowed);
            try { command=f.command(true); }
            catch(const std::bad_alloc&) { failed=true; }
        }
        before.checkUnchanged(f);
        if(failed)++failurePoints;
        else { CHECK(bool(command));completed=true;break; }
    }
    CHECK(failurePoints>0 && completed);
    std::cout << "Merge preparation: checked " << failurePoints << " allocation failure points\n";
}
void allocationSweep(Operation operation, bool merge = false)
{
    std::size_t failurePoints = 0;
    bool completed = false;
    for (std::size_t allowed = 0; allowed < 512; ++allowed) {
        Fixture f;
        if(merge)f.session.setLayerSelection(std::array{f.ids[2],f.ids[0]},f.ids[0],f.ids[2]);
        auto command = f.command(merge);
        if (operation != Operation::Execute) CHECK(f.session.execute(std::move(command)));
        if (operation == Operation::Redo) CHECK(f.session.undo());
        const Snapshot before(f);
        bool threw = false, result = false;
        try {
            const allocation::Scope scope(allowed);
            if (operation == Operation::Execute) result = f.session.execute(std::move(command));
            else if (operation == Operation::Undo) result = f.session.undo();
            else result = f.session.redo();
        } catch (const std::bad_alloc&) { threw = true; }
        if (threw) {
            ++failurePoints;
            before.checkUnchanged(f);
            // The original command remains retryable on both history stacks.
            if (operation == Operation::Undo) CHECK(f.session.undo());
            if (operation == Operation::Redo) CHECK(f.session.redo());
        } else {
            CHECK(result);
            completed = result;
            break;
        }
    }
    CHECK(failurePoints > 0 && completed);
    std::cout << (merge?"Merge consolidation ":"Structural ") << (operation == Operation::Execute ? "execute" : operation == Operation::Undo ? "undo" : "redo")
              << ": checked " << failurePoints << " allocation failure points\n";
}
}

int main()
{
    allocationSweep(Operation::Execute);
    allocationSweep(Operation::Undo);
    allocationSweep(Operation::Redo);
    mergePreparationAllocationSweep();
    allocationSweep(Operation::Execute,true);
    allocationSweep(Operation::Undo,true);
    allocationSweep(Operation::Redo,true);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
