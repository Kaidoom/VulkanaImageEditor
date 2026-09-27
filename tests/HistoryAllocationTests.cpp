#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>

// Executable-local failure injection; no shipping allocator hooks or Qt.
namespace allocation {
thread_local bool armed = false;
thread_local std::size_t remaining = 0;
thread_local std::size_t attempts = 0;
void beforeAllocation()
{
    if (!armed)
        return;
    ++attempts;
    if (!remaining)
        throw std::bad_alloc();
    --remaining;
}
struct Scope {
    explicit Scope(bool enabled, std::size_t allowed = 0)
    {
        armed = enabled;
        remaining = allowed;
        attempts = 0;
    }
    ~Scope() { armed = false; }
};
}

void* operator new(std::size_t size)
{
    allocation::beforeAllocation();
    if (auto* memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    allocation::beforeAllocation();
    void* memory = nullptr;
    if (posix_memalign(&memory, std::size_t(alignment), size ? size : 1) == 0)
        return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct Probe {
    int value { 0 }, applies { 0 }, undoes { 0 };
    bool rejectAfterMutation { false };
};

// A precise, allocation-free document mutation, also exposing invocation counts
// so an attempted operation cannot hide a mutate-and-rollback implementation.
class ProbeCommand final : public Command {
public:
    ProbeCommand(Probe& probe, int after, SelectionState beforeMask, SelectionState afterMask)
        : probe_(probe)
        , before_(probe.value)
        , after_(after)
        , beforeMask_(std::move(beforeMask))
        , afterMask_(std::move(afterMask))
    {
    }
    bool apply(Document& document) override
    {
        if (probe_.value == after_)
            return false;
        ++probe_.applies;
        probe_.value = after_;
        document.setSelection(afterMask_);
        rejectFurtherAllocations();
        return true;
    }
    bool undo(Document& document) override
    {
        ++probe_.undoes;
        probe_.value = before_;
        document.setSelection(beforeMask_);
        rejectFurtherAllocations();
        return true;
    }
    std::string_view label() const noexcept override { return "Allocation probe"; }
    std::size_t memoryCost() const noexcept override { return sizeof(*this); }
    bool canAdoptApplied(const Document& document) const noexcept override
    {
        return probe_.value == after_ && document.selection() == afterMask_;
    }
    std::optional<std::uint64_t> activeLayerAfter(bool undo) const noexcept override
    {
        return std::uint64_t(undo ? before_ : after_);
    }

private:
    void rejectFurtherAllocations()
    {
        if (probe_.rejectAfterMutation) {
            allocation::armed = true;
            allocation::remaining = 0;
        }
    }
    Probe& probe_;
    int before_, after_;
    SelectionState beforeMask_, afterMask_;
};

struct Fixture {
    Document document { CanvasSpec { .extent = { 16, 16 } } };
    Probe probe;
    std::array<SelectionState, 4> masks;
    Fixture()
    {
        for (int i = 1; i < 4; ++i) {
            masks[std::size_t(i)] = SelectionMask::rectangle({ 16, 16 }, { i, i, 4, 5 });
            (void)masks[std::size_t(i)]->boundaryEdges();
        }
    }
    std::unique_ptr<Command> command(int value)
    {
        return std::make_unique<ProbeCommand>(
            probe, value, document.selection(), masks[std::size_t(value)]);
    }
};

struct Snapshot {
    SelectionState selection;
    Revision revision;
    std::size_t undo, redo, memory;
    std::optional<std::uint64_t> hint;
    int value, applies, undoes;
    Snapshot(const Fixture& f, const History& history)
        : selection(f.document.selection())
        , revision(f.document.selectionRevision())
        , undo(history.undoDepth())
        , redo(history.redoDepth())
        , memory(history.memoryUsed())
        , hint(history.activeLayerHint())
        , value(f.probe.value)
        , applies(f.probe.applies)
        , undoes(f.probe.undoes)
    {
    }
    void checkUnchanged(const Fixture& f, const History& history) const
    {
        CHECK(f.document.selection() == selection);
        CHECK(f.document.selectionRevision() == revision);
        CHECK(history.undoDepth() == undo);
        CHECK(history.redoDepth() == redo);
        CHECK(history.memoryUsed() == memory);
        CHECK(history.activeLayerHint() == hint);
        CHECK(f.probe.value == value);
        CHECK(f.probe.applies == applies);
        CHECK(f.probe.undoes == undoes);
    }
};

// Publishing into an empty history reserves exactly one occupied done slot,
// while transferring the branch's pending redo. No private capacity access or
// assumptions about execute()'s geometric growth factor are required.
void fullHistoryWithRedo(Fixture& f, History& destination)
{
    History branch;
    CHECK(branch.execute(f.document, f.command(1)));
    CHECK(branch.execute(f.document, f.command(2)));
    CHECK(branch.undo(f.document));
    CHECK(destination.publishAppliedBranch(f.document, branch));
    CHECK(destination.undoDepth() == 1);
    CHECK(destination.redoDepth() == 1);
    CHECK(f.probe.value == 1);
}

template <class Function>
bool failsNextAllocation(Function&& function)
{
    bool badAllocation = false;
    {
        allocation::Scope scope(true);
        try {
            function();
        } catch (const std::bad_alloc&) {
            badAllocation = true;
        }
    }
    CHECK(allocation::attempts == 1);
    return badAllocation;
}

void executeReservesBeforeApplyAndDiscardingRedo()
{
    Fixture f;
    History history;
    fullHistoryWithRedo(f, history);
    const Snapshot before(f, history);
    auto command = f.command(3); // Command allocation itself is not under test.
    CHECK(failsNextAllocation([&] { history.execute(f.document, std::move(command)); }));
    before.checkUnchanged(f, history);
    CHECK(history.redo(f.document));
    CHECK(f.probe.value == 2);
    CHECK(f.document.selection() == f.masks[2]);
}

void undoReservesBeforeUndoOrPoppingSource()
{
    Fixture f;
    History history;
    CHECK(history.execute(f.document, f.command(1)));
    CHECK(history.execute(f.document, f.command(2)));
    const Snapshot before(f, history);
    CHECK(failsNextAllocation([&] { history.undo(f.document); }));
    before.checkUnchanged(f, history);
    CHECK(history.undo(f.document));
    CHECK(f.document.selection() == f.masks[1]);
    CHECK(history.redo(f.document));
    CHECK(f.document.selection() == f.masks[2]);
}

void redoReservesBeforeApplyOrPoppingSource()
{
    Fixture f;
    History history;
    fullHistoryWithRedo(f, history);
    const Snapshot before(f, history);
    CHECK(failsNextAllocation([&] { history.redo(f.document); }));
    before.checkUnchanged(f, history);
    CHECK(history.redo(f.document));
    CHECK(f.probe.value == 2);
    CHECK(history.memoryUsed() == before.memory);
    CHECK(history.undoDepth() == 2);
    CHECK(history.redoDepth() == 0);
}

template <class Function>
void succeedsWithoutPostMutationAllocations(Fixture& f, Function&& function)
{
    bool result = false, threw = false;
    f.probe.rejectAfterMutation = true;
    {
        allocation::Scope scope(false);
        try {
            result = function();
        } catch (const std::bad_alloc&) {
            threw = true;
        }
    }
    f.probe.rejectAfterMutation = false;
    CHECK(!threw);
    CHECK(result);
    CHECK(allocation::attempts == 0);
}

void successfulExecuteUndoRedoAllocateOnlyBeforeMutation()
{
    Fixture f;
    History history;
    auto command = f.command(1);
    succeedsWithoutPostMutationAllocations(
        f, [&] { return history.execute(f.document, std::move(command)); });
    const auto memory = history.memoryUsed();
    CHECK(f.document.selection() == f.masks[1]);
    succeedsWithoutPostMutationAllocations(f, [&] { return history.undo(f.document); });
    CHECK(!f.document.selection());
    CHECK(history.memoryUsed() == memory);
    succeedsWithoutPostMutationAllocations(f, [&] { return history.redo(f.document); });
    CHECK(f.document.selection() == f.masks[1]);
    CHECK(history.memoryUsed() == memory);

    Fixture full;
    History fullHistory;
    fullHistoryWithRedo(full, fullHistory);
    const auto retained = fullHistory.memoryUsed();
    succeedsWithoutPostMutationAllocations(full, [&] { return fullHistory.redo(full.document); });
    CHECK(fullHistory.memoryUsed() == retained);
    CHECK(fullHistory.undoDepth() == 2);

    Fixture divergent;
    History divergentHistory;
    fullHistoryWithRedo(divergent, divergentHistory);
    auto newCommand = divergent.command(3);
    succeedsWithoutPostMutationAllocations(
        divergent, [&] { return divergentHistory.execute(divergent.document, std::move(newCommand)); });
    CHECK(divergentHistory.undoDepth() == 2);
    CHECK(divergentHistory.redoDepth() == 0);
    CHECK(divergentHistory.memoryUsed() == 2 * sizeof(ProbeCommand));
}

void actualSelectionCommandIsAtomicAndNoOpRetainsRedo()
{
    Fixture f;
    History history;
    auto selection = std::make_unique<SetSelectionCommand>(f.masks[1], "Lasso selection");
    const auto revision = f.document.selectionRevision();
    CHECK(failsNextAllocation([&] { history.execute(f.document, std::move(selection)); }));
    CHECK(!f.document.selection());
    CHECK(f.document.selectionRevision() == revision);
    CHECK(history.undoDepth() == 0);
    CHECK(history.memoryUsed() == 0);
    CHECK(history.execute(f.document, std::make_unique<SetSelectionCommand>(f.masks[1])));
    CHECK(history.execute(f.document, std::make_unique<SetSelectionCommand>(f.masks[2])));
    CHECK(history.undo(f.document));
    const auto beforeRevision = f.document.selectionRevision();
    const auto memory = history.memoryUsed();
    CHECK(!history.execute(f.document, std::make_unique<SetSelectionCommand>(f.masks[1])));
    CHECK(f.document.selectionRevision() == beforeRevision);
    CHECK(history.memoryUsed() == memory);
    CHECK(history.undoDepth() == 1);
    CHECK(history.redoDepth() == 1);
    CHECK(history.redo(f.document));
    CHECK(f.document.selection() == f.masks[2]);
}

struct GroupFixture {
    Document document { CanvasSpec { .extent = { 32, 32 } } };
    History history;
    std::array<LayerId, 2> ids;
    std::array<AffineTransform, 2> originals;
    std::array<std::shared_ptr<ContiguousRasterSurface>, 2> surfaces;

    GroupFixture()
    {
        originals = { AffineTransform { 1, 0.2, 2, 0, 1, 3 },
            AffineTransform { -1, 0.1, 20, 0.15, 1, 10 } };
        for (std::size_t i = 0; i < ids.size(); ++i) {
            surfaces[i] = std::make_shared<ContiguousRasterSurface>(
                Extent2u { 4, 4 }, Rgba8 { 13, 29, 57, 179 });
            auto layer = Layer::raster("Allocation target", surfaces[i]);
            ids[i] = layer.id;
            layer.localToDocument = originals[i];
            CHECK(document.insertLayer(i, std::move(layer)));
        }
        // Both an undo entry and a pre-existing redo branch must survive a
        // failed transform action or deliberate whole-session cancellation.
        CHECK(history.execute(document, std::make_unique<SetLayerOpacityCommand>(ids[0], 0.5F)));
        CHECK(history.execute(document, std::make_unique<SetLayerOpacityCommand>(ids[0], 0.75F)));
        CHECK(history.undo(document));
        document.markSaved();
    }

    std::array<AffineTransform, 2> matrices() const
    {
        return { document.layer(ids[0])->localToDocument, document.layer(ids[1])->localToDocument };
    }
};

struct HistoryCounts {
    std::size_t undo, redo, memory;
    explicit HistoryCounts(const History& h)
        : undo(h.undoDepth())
        , redo(h.redoDepth())
        , memory(h.memoryUsed())
    {
    }
    void checkUnchanged(const History& h) const
    {
        CHECK(h.undoDepth() == undo);
        CHECK(h.redoDepth() == redo);
        CHECK(h.memoryUsed() == memory);
    }
};

void groupCompletionAllocationFailuresAreRecoverable(bool pointerDrag, bool pendingRedo, bool retry)
{
    bool reachedSuccessfulCompletion = false;
    std::size_t failurePoints = 0;
    // Reconstruct the same session for every allocation index. This covers
    // each reserve/vector/command allocation, not merely the first one, and
    // avoids relying on private vector growth strategies or fixed counts.
    for (std::size_t allowed = 0; allowed < 64; ++allowed) {
        GroupFixture f;
        const HistoryCounts external(f.history);
        const auto entryContent = f.document.contentState();
        const std::array surfaceRevisions { f.surfaces[0]->revision(), f.surfaces[1]->revision() };
        LayerTransformSession session(f.document, f.ids);
        CHECK(session.active() && session.grouped());
        if (pendingRedo) {
            auto values = session.values();
            values.center = values.center + Vec2d { 2, 1 };
            CHECK(session.setValues(values));
            CHECK(session.completeAction());
            values.center = values.center + Vec2d { 5, 3 };
            CHECK(session.setValues(values));
            CHECK(session.completeAction());
            CHECK(session.undo());
            CHECK(session.pendingHistory().undoDepth() == 1);
            CHECK(session.pendingHistory().redoDepth() == 1);
        }
        const auto actionStart = f.matrices();
        if (pointerDrag) {
            const auto point = session.values().center;
            CHECK(session.beginDrag(TransformHandle::Move, point));
            CHECK(session.dragTo(point + Vec2d { 9, -4 }, { }, false));
        } else {
            auto values = session.values();
            values.center = values.center + Vec2d { 9, -4 };
            values.rotationDegrees += 17;
            CHECK(session.setValues(values));
        }
        // Live geometry is intentionally mutated without failure injection.
        // Completion must only adopt that exact preview, atomically, or keep
        // it recoverable for retry/cancel; it must not apply another delta.
        const auto preview = f.matrices();
        CHECK(preview[0] != actionStart[0] && preview[1] != actionStart[1]);
        const auto previewFrame = session.transform();
        const auto previewRevision = f.document.revision();
        const auto previewContent = f.document.contentState();
        const HistoryCounts pending(session.pendingHistory());
        bool threw = false;
        bool completed = false;
        {
            allocation::Scope scope(true, allowed);
            try {
                if (pointerDrag) {
                    session.endDrag();
                    completed = session.pendingHistory().undoDepth() == pending.undo + 1;
                } else {
                    completed = session.completeAction();
                }
            } catch (const std::bad_alloc&) {
                threw = true;
            }
        }
        external.checkUnchanged(f.history);
        CHECK(f.matrices() == preview);
        CHECK(f.document.revision() == previewRevision);
        CHECK(session.transform() == previewFrame);
        CHECK(session.active() && session.targetAvailable());
        CHECK(f.surfaces[0]->revision() == surfaceRevisions[0]);
        CHECK(f.surfaces[1]->revision() == surfaceRevisions[1]);

        if (!threw) {
            CHECK(completed);
            CHECK(session.pendingHistory().undoDepth() == pending.undo + 1);
            CHECK(session.pendingHistory().redoDepth() == 0);
            session.cancel();
            CHECK(f.matrices() == f.originals);
            CHECK(f.document.contentState() == entryContent);
            external.checkUnchanged(f.history);
            reachedSuccessfulCompletion = true;
            break;
        }

        ++failurePoints;
        CHECK(allocation::attempts == allowed + 1);
        CHECK(!completed);
        CHECK(f.document.contentState() == previewContent);
        pending.checkUnchanged(session.pendingHistory());
        if (retry) {
            // endDrag may already have released the pointer handle; retrying
            // it must still record the pending action once, not lose it.
            if (pointerDrag)
                session.endDrag();
            else
                CHECK(session.completeAction());
            CHECK(session.pendingHistory().undoDepth() == pending.undo + 1);
            CHECK(session.pendingHistory().redoDepth() == 0);
            CHECK(f.matrices() == preview);
            external.checkUnchanged(f.history);
            CHECK(session.undo());
            CHECK(f.matrices() == actionStart);
            CHECK(session.redo());
            CHECK(f.matrices() == preview);
            CHECK(session.commit(f.history) == TransformCommitResult::Committed);
            CHECK(f.history.undoDepth() == external.undo + pending.undo + 1);
            CHECK(f.history.redoDepth() == 0);
            CHECK(f.history.undo(f.document));
            CHECK(f.matrices() == actionStart);
            CHECK(f.history.redo(f.document));
            CHECK(f.matrices() == preview);
        } else {
            // Cancellation is the emergency escape path even under continued
            // allocation pressure, including an already-cleared drag handle.
            bool cancellationThrew = false;
            {
                allocation::Scope scope(true);
                try {
                    session.cancel();
                } catch (const std::bad_alloc&) {
                    cancellationThrew = true;
                }
            }
            CHECK(!cancellationThrew);
            CHECK(allocation::attempts == 0);
            CHECK(!session.active());
            CHECK(f.matrices() == f.originals);
            CHECK(f.document.contentState() == entryContent);
            CHECK(!f.document.isModified());
            external.checkUnchanged(f.history);
            CHECK(f.history.redo(f.document));
            CHECK(f.document.layer(f.ids[0])->opacity == 0.75F);
            CHECK(f.matrices() == f.originals);
        }
    }
    CHECK(reachedSuccessfulCompletion);
    CHECK(failurePoints > 0);
}

void groupTransformActionsSurviveEveryHistoryAllocationFailure()
{
    for (const bool pointerDrag : { false, true })
        for (const bool pendingRedo : { false, true })
            for (const bool retry : { false, true })
                groupCompletionAllocationFailuresAreRecoverable(pointerDrag, pendingRedo, retry);
}
}

int main()
{
    executeReservesBeforeApplyAndDiscardingRedo();
    undoReservesBeforeUndoOrPoppingSource();
    redoReservesBeforeApplyOrPoppingSource();
    successfulExecuteUndoRedoAllocateOnlyBeforeMutation();
    actualSelectionCommandIsAtomicAndNoOpRetainsRedo();
    groupTransformActionsSurviveEveryHistoryAllocationFailure();
    std::cout << "History allocation tests: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
