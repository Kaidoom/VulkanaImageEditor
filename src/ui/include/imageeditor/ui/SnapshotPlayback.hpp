#pragma once
#include "imageeditor/core/SnapshotTimeline.hpp"
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <functional>

namespace imageeditor::ui {
// Presentation-only clock. Absolute deadlines prevent decoding/painting time
// from accumulating into timing drift. Empty capture slots are skipped.
class SnapshotPlayback final : public QObject {
public:
    explicit SnapshotPlayback(QObject* parent = nullptr);
    void setTimeline(const core::SnapshotTimelineState&);
    void play(core::SnapshotFrameId);
    void pause();
    void clear();
    void seek(core::SnapshotFrameId);
    bool playing() const { return playing_; }
    core::SnapshotFrameId currentFrame() const { return current_; }
    std::function<void(core::SnapshotFrameId)> onFrame;
    std::function<void(bool)> onPlayingChanged;
private:
    void present(bool force = false);
    qint64 position() const;
    struct Entry { core::SnapshotFrameId id; qint64 end; };
    core::SnapshotTimelineState timeline_;
    std::vector<Entry> entries_;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 offset_{}, total_{};
    core::SnapshotFrameId current_{};
    bool playing_{};
};
}
