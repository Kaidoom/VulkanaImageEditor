#include "imageeditor/core/SnapshotTimeline.hpp"
#include "imageeditor/core/Document.hpp"

#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace imageeditor::core {
namespace {
std::size_t addCost(std::size_t a, std::size_t b) noexcept
{
    return b > std::numeric_limits<std::size_t>::max() - a
        ? std::numeric_limits<std::size_t>::max() : a + b;
}
}

bool validSnapshotTimeline(const SnapshotTimelineState& state)
{
    if (state.frames.empty() || state.nextId < 2) return false;
    std::unordered_set<SnapshotFrameId> ids;
    ids.reserve(state.frames.size());
    for (const auto& frame : state.frames) {
        if (!frame.id || frame.id >= state.nextId || !frame.durationMs
            || !ids.insert(frame.id).second) return false;
        if (frame.image && (frame.image->extent.empty() || frame.image->png.empty()
            || !std::isfinite(frame.image->dotsPerInch) || frame.image->dotsPerInch <= 0))
            return false;
    }
    return true;
}

ChangeSnapshotTimelineCommand::ChangeSnapshotTimelineCommand(SnapshotTimelineState before,
    SnapshotTimelineState after, std::string label)
    : before_(std::move(before)), after_(std::move(after)), label_(std::move(label))
{
    // Unchanged pixels remain document-owned and are not additional history
    // storage. Charge only added/removed/replaced image allocations, once each;
    // duration and ordering edits retain cheap metadata and shared pointers.
    memoryCost_ = addCost(sizeof(*this), label_.capacity());
    std::unordered_set<const SnapshotImage*> beforeImages, afterImages;
    const auto collect = [&](const SnapshotTimelineState& state, auto& images) {
        memoryCost_ = addCost(memoryCost_, state.frames.capacity() * sizeof(SnapshotFrame));
        for (const auto& frame : state.frames)
            if (frame.image) images.insert(frame.image.get());
    };
    collect(before_, beforeImages);
    collect(after_, afterImages);
    for (const auto* image : beforeImages)
        if (!afterImages.contains(image))
            memoryCost_ = addCost(memoryCost_, addCost(sizeof(SnapshotImage), image->png.capacity()));
    for (const auto* image : afterImages)
        if (!beforeImages.contains(image))
            memoryCost_ = addCost(memoryCost_, addCost(sizeof(SnapshotImage), image->png.capacity()));
}

bool ChangeSnapshotTimelineCommand::apply(Document& document)
{
    return document.setSnapshotTimeline(before_, after_);
}

bool ChangeSnapshotTimelineCommand::undo(Document& document)
{
    return document.setSnapshotTimeline(after_, before_);
}

std::size_t ChangeSnapshotTimelineCommand::memoryCost() const noexcept
{
    return memoryCost_;
}

} // namespace imageeditor::core
