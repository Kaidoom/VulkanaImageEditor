#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace imageeditor::core {

// A process-wide, lazily created executor: at most eight participants including
// the invoking thread. Workers park between batches, and concurrent batches
// share the same pool instead of creating additional worker sets. Nested calls
// execute serially. A worker count of zero requests the bounded default.
[[nodiscard]] unsigned boundedParallelWorkerCount(unsigned requested = 0);

// Invokes function once per participant with (rank, participantCount). Waiting
// for a busy executor is cancellable; false means the batch never started.
// Once started, the callback owns cooperative cancellation within its work.
// Exceptions are retained and rethrown on the invoking thread after every
// participant returns. User callbacks must not abandon a shared barrier.
[[nodiscard]] bool boundedParallel(unsigned workers,
    const std::function<void(unsigned, unsigned)>& function,
    const std::function<bool()>& cancelled = {});

// Stable dependency schedule for in-place left/up (or right/down) propagation.
// Pixels on one x+y diagonal are independent; each diagonal completes before
// the next. Inputs must be unique nonnegative row-major pixel indices.
// The callback receives the ORIGINAL input ordinal (reverse maps it to reverse
// input order), so random streams/counters can retain their serial ownership.
// Build once and reuse across passes; the schedule does not own image state.
class DiagonalWavefront {
public:
    DiagonalWavefront() = default;
    DiagonalWavefront(std::span<const int> pixels, int width);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t diagonalCount() const noexcept { return offsets_.empty() ? 0 : offsets_.size() - 1; }

    // Cancellation/exceptions are shared between participants, while every
    // participant still reaches the current barrier. Exceptions are rethrown
    // only after the batch has drained, never through a waiting worker.
    [[nodiscard]] bool run(unsigned workers, bool reverse,
        const std::function<void(int, std::size_t, unsigned)>& function,
        const std::function<bool()>& cancelled = {}) const;

private:
    struct Entry { int pixel; std::size_t ordinal; };
    std::vector<Entry> entries_;
    std::vector<std::size_t> offsets_;
};

} // namespace imageeditor::core
