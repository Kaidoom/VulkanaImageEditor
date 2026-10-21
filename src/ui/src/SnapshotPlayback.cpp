#include "imageeditor/ui/SnapshotPlayback.hpp"
#include <algorithm>
#include <limits>

namespace imageeditor::ui {
SnapshotPlayback::SnapshotPlayback(QObject* parent) : QObject(parent)
{
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { present(); });
}
void SnapshotPlayback::setTimeline(const core::SnapshotTimelineState& timeline)
{
    if (timeline_ == timeline) return;
    clear();
    timeline_ = timeline;
    for (const auto& frame : timeline.frames) if (frame.image) {
        total_ += std::max<std::uint32_t>(1, frame.durationMs);
        entries_.push_back({frame.id, total_});
    }
}
qint64 SnapshotPlayback::position() const
{
    if (!total_) return 0;
    const auto elapsed = offset_ + (playing_ ? clock_.elapsed() : 0);
    return looping_ ? elapsed % total_ : std::min(elapsed, total_);
}
void SnapshotPlayback::setLooping(bool looping)
{
    if (looping_ == looping) return;
    offset_ = position();
    if (playing_) clock_.restart();
    looping_ = looping;
}
void SnapshotPlayback::play(core::SnapshotFrameId id)
{
    if (entries_.empty()) return;
    if (!looping_ && offset_ >= total_ && current_ == id) id = entries_.front().id;
    if (current_ != id) seek(id);
    if (entries_.empty()) return; // A failed preview may cancel from onFrame.
    if (playing_) return;
    clock_.start();
    playing_ = true;
    if (onPlayingChanged) onPlayingChanged(true);
    present(true);
}
void SnapshotPlayback::pause()
{
    if (!playing_) return;
    offset_ = position();
    timer_.stop();
    playing_ = false;
    if (onPlayingChanged) onPlayingChanged(false);
}
void SnapshotPlayback::clear()
{
    pause();
    timeline_ = {};
    entries_.clear();
    total_ = offset_ = 0;
    current_ = 0;
}
void SnapshotPlayback::seek(core::SnapshotFrameId id)
{
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found == entries_.end()) return;
    offset_ = found == entries_.begin() ? 0 : (found - 1)->end;
    if (playing_) clock_.restart();
    present(true);
}
void SnapshotPlayback::present(bool force)
{
    if (entries_.empty()) return;
    const auto started = playing_ ? clock_.elapsed() : 0;
    const auto time = position();
    const bool finished = !looping_ && time >= total_;
    const auto found = std::upper_bound(entries_.begin(), entries_.end(), time,
        [](qint64 value, const Entry& entry) { return value < entry.end; });
    const auto selected = finished ? entries_.end() - 1 : found;
    if (selected == entries_.end()) return;
    // Copy before callbacks, which may stop playback or replace the document.
    const auto deadline = selected->end;
    const auto id = selected->id;
    if (force || current_ != id) {
        current_ = id;
        if (onFrame) onFrame(id);
    }
    if (finished) { pause(); return; }
    if (playing_) {
        // Account for decoding time. Late frames are skipped on the next tick,
        // rather than slowing the entire animation or queuing a catch-up burst.
        const auto spent = std::max(qint64(0), clock_.elapsed() - started);
        timer_.start(int(std::clamp(deadline - time - spent,
            qint64(1), qint64(std::numeric_limits<int>::max()))));
    }
}
}
