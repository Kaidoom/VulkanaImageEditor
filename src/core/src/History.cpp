#include "imageeditor/core/History.hpp"

#include "imageeditor/core/Document.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace imageeditor::core {
namespace {

std::size_t saturatingAdd(std::size_t left, std::size_t right) noexcept
{
    const auto maximum = std::numeric_limits<std::size_t>::max();
    return right > maximum - left ? maximum : left + right;
}

} // namespace

History::History(std::size_t memoryBudgetBytes)
    : memoryBudgetBytes_(memoryBudgetBytes)
{
}

bool History::execute(Document& document, std::unique_ptr<Command> command)
{
    layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    if (!command) { activeLayerHint_.reset(); return false; }
    // Admission must not allocate after apply changes the document. In
    // particular, a failed selection commit must retain the old mask/redo.
    if (done_.size() == done_.capacity())
        done_.reserve(done_.size() ? done_.size() * 2 : 8);
    if (!command->apply(document)) {
        activeLayerHint_.reset();
        return false;
    }

    activeLayerHint_ = command->activeLayerAfter(false);
    textEditHint_ = command->textEditAfter(false);
    layerSelectionHint_ = command->layerSelectionAfter(false);
    return record(document, std::move(command));
}

bool History::adoptApplied(Document& document, std::unique_ptr<Command>& command)
{
    layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    activeLayerHint_.reset();
    if (!command || !command->canAdoptApplied(document)) {
        return false;
    }
    // Reserve before transferring ownership or discarding redo. Allocation
    // failure leaves the live-edit caller holding its rollback snapshots.
    if (done_.size() == done_.capacity())
        done_.reserve(done_.size() ? done_.size() * 2 : 8);
    return record(document, std::move(command));
}

bool History::record(Document& document, std::unique_ptr<Command> command)
{
    const bool mergeAllowed = document.isModified(); // A saved endpoint is a merge barrier.
    command->contentBefore_ = document.contentState();
    if (command->affectsPersistentContent())
        document.advanceContentState();
    command->contentAfter_ = document.contentState();
    discardRedo();

    if (mergeAllowed && !done_.empty()
        && done_.back()->contentAfter_ == command->contentBefore_) {
        const auto costBeforeMerge = done_.back()->memoryCost();
        if (done_.back()->mergeWith(*command)) {
            done_.back()->contentAfter_ = command->contentAfter_;
            const auto costAfterMerge = done_.back()->memoryCost();
            memoryUsed_ = costBeforeMerge > memoryUsed_ ? 0 : memoryUsed_ - costBeforeMerge;
            memoryUsed_ = saturatingAdd(memoryUsed_, costAfterMerge);
            trimToBudget();
            return true;
        }
    }

    memoryUsed_ = saturatingAdd(memoryUsed_, command->memoryCost());
    done_.push_back(std::move(command));
    trimToBudget();
    return true;
}

bool History::publishAppliedBranch(Document& document, History& branch)
{
    layerSelectionHint_ = nullptr;
    branch.layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    if (&branch == this || branch.done_.empty()
        || branch.done_.back()->contentAfter_ != document.contentState()
        || !branch.done_.back()->canAdoptApplied(document))
        return false;
    // Allocate before changing ownership or destroying an existing redo branch.
    done_.reserve(done_.size() + branch.done_.size());
    discardRedo();
    for (auto& command : branch.done_)
        done_.push_back(std::move(command));
    undone_ = std::move(branch.undone_);
    memoryUsed_ = saturatingAdd(memoryUsed_, branch.memoryUsed_);
    branch.done_.clear();
    branch.undone_.clear();
    branch.memoryUsed_ = 0;
    trimToBudget();
    return true;
}

void History::reserveForPublication(const History& branch, std::size_t extraCommands)
{
    const auto count = saturatingAdd(saturatingAdd(branch.done_.size(), branch.undone_.size()), extraCommands);
    const auto needed = saturatingAdd(done_.size(), count);
    if (needed > done_.max_size())
        throw std::length_error("Too many history commands");
    if (needed > done_.capacity())
        done_.reserve(needed > done_.max_size() / 2 ? needed : std::max(needed, done_.capacity() * 2));
}

void History::discardRedo()
{
    for (const auto& discarded : undone_) {
        const auto cost = discarded->memoryCost();
        memoryUsed_ = cost > memoryUsed_ ? 0 : memoryUsed_ - cost;
    }
    undone_.clear();
}

bool History::undo(Document& document)
{
    layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    if (done_.empty()) {
        activeLayerHint_.reset();
        return false;
    }
    // Reserve the destination before moving the command or changing state.
    if (undone_.size() == undone_.capacity())
        undone_.reserve(undone_.size() ? undone_.size() * 2 : 8);
    if (!done_.back()->undo(document)) {
        activeLayerHint_.reset();
        return false;
    }
    auto command = std::move(done_.back());
    done_.pop_back();
    activeLayerHint_ = command->activeLayerAfter(true);
    document.restoreContentState(command->contentBefore_);
    textEditHint_ = command->textEditAfter(true);
    layerSelectionHint_ = command->layerSelectionAfter(true);
    undone_.push_back(std::move(command));
    return true;
}

bool History::redo(Document& document)
{
    layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    if (undone_.empty()) {
        activeLayerHint_.reset();
        return false;
    }
    if (done_.size() == done_.capacity())
        done_.reserve(done_.size() ? done_.size() * 2 : 8);
    if (!undone_.back()->apply(document)) {
        activeLayerHint_.reset();
        return false;
    }
    auto command = std::move(undone_.back());
    undone_.pop_back();
    activeLayerHint_ = command->activeLayerAfter(false);
    document.restoreContentState(command->contentAfter_);
    textEditHint_ = command->textEditAfter(false);
    layerSelectionHint_ = command->layerSelectionAfter(false);
    done_.push_back(std::move(command));
    return true;
}

/*
 * memoryUsed_ covers both stacks: an undone command is still retained so that
 * redo can restore it. Undo/redo therefore transfer ownership without changing
 * the accounting.
 */

void History::clear()
{
    layerSelectionHint_ = nullptr;
    textEditHint_.reset();
    activeLayerHint_.reset();
    done_.clear();
    undone_.clear();
    memoryUsed_ = 0;
}

std::string_view History::undoLabel() const noexcept
{
    return done_.empty() ? std::string_view {} : done_.back()->label();
}

std::string_view History::redoLabel() const noexcept
{
    return undone_.empty() ? std::string_view {} : undone_.back()->label();
}

void History::trimToBudget()
{
    while (memoryUsed_ > memoryBudgetBytes_ && done_.size() > 1) {
        memoryUsed_ -= done_.front()->memoryCost();
        done_.erase(done_.begin());
    }
}

} // namespace imageeditor::core
