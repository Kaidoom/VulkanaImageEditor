#pragma once

#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace imageeditor::core {

using SnapshotFrameId = std::uint64_t;

// Frozen straight-alpha pixels encoded losslessly. Frames and history share
// immutable compressed storage; full-size decoded images belong to the viewer.
struct SnapshotImage {
    Extent2u extent;
    double dotsPerInch {96.0};
    std::vector<std::byte> png;
};

struct SnapshotFrame {
    SnapshotFrameId id {1};
    std::uint32_t durationMs {100};
    std::shared_ptr<const SnapshotImage> image; // Null is an uncaptured slot.

    friend bool operator==(const SnapshotFrame&, const SnapshotFrame&) = default;
};

struct SnapshotTimelineState {
    std::vector<SnapshotFrame> frames {SnapshotFrame{}};
    SnapshotFrameId nextId {2};

    friend bool operator==(const SnapshotTimelineState&, const SnapshotTimelineState&) = default;
};

// Structural admission only. The image codec validates PNG contents before
// publishing a frame; core deliberately has no image/GUI codec dependency.
[[nodiscard]] bool validSnapshotTimeline(const SnapshotTimelineState&);

class ChangeSnapshotTimelineCommand final : public Command {
public:
    ChangeSnapshotTimelineCommand(SnapshotTimelineState before,
        SnapshotTimelineState after, std::string label);

    bool apply(Document&) override;
    bool undo(Document&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;

private:
    SnapshotTimelineState before_, after_;
    std::string label_;
    std::size_t memoryCost_ {0};
};

} // namespace imageeditor::core
