#pragma once

#include "imageeditor/core/Command.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace imageeditor::core {

class History {
public:
    explicit History(std::size_t memoryBudgetBytes = 256ULL * 1024ULL * 1024ULL);

    bool execute(Document& document, std::unique_ptr<Command> command);
    // Ownership moves only after validation succeeds. Keeping the pointer in
    // the caller on rejection lets a live edit restore its already-mutated
    // surface instead of losing the only undo snapshot.
    bool adoptApplied(Document& document, std::unique_ptr<Command>& command);
    // Publish an isolated, already-applied edit branch without replay or merge.
    // An all-undone branch is empty for publication and leaves this history intact.
    bool publishAppliedBranch(Document& document, History& branch);
    // Allocate while a retractable edit can still fail safely, not at finish.
    void reserveForPublication(const History& branch, std::size_t extraCommands = 0);
    bool undo(Document& document);
    bool redo(Document& document);
    void clear();

    [[nodiscard]] bool canUndo() const noexcept { return !done_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !undone_.empty(); }
    [[nodiscard]] std::string_view undoLabel() const noexcept;
    [[nodiscard]] std::string_view redoLabel() const noexcept;
    [[nodiscard]] std::size_t memoryUsed() const noexcept { return memoryUsed_; }
    [[nodiscard]] std::size_t memoryBudget() const noexcept { return memoryBudgetBytes_; }
    [[nodiscard]] const LayerSelectionState* layerSelectionHint() const noexcept { return layerSelectionHint_; }
    [[nodiscard]] std::size_t undoDepth() const noexcept { return done_.size(); }
    [[nodiscard]] std::size_t redoDepth() const noexcept { return undone_.size(); }
    [[nodiscard]] std::optional<std::uint64_t> activeLayerHint() const noexcept { return activeLayerHint_; }
    [[nodiscard]] std::optional<TextEditHint> textEditHint() const noexcept { return textEditHint_; }
    [[nodiscard]] std::size_t latestUndoMemoryCost() const noexcept
    {
        return done_.empty() ? 0 : done_.back()->memoryCost();
    }

private:
    bool record(Document&, std::unique_ptr<Command> command);
    void discardRedo();
    void trimToBudget();

    std::size_t memoryBudgetBytes_;
    std::size_t memoryUsed_ {0};
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
    std::optional<std::uint64_t> activeLayerHint_;
    std::optional<TextEditHint> textEditHint_;
    const LayerSelectionState* layerSelectionHint_ {nullptr}; // Command-owned, no post-mutation allocation.
};

} // namespace imageeditor::core
