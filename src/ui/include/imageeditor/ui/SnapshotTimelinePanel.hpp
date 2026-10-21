#pragma once

#include "imageeditor/core/SnapshotTimeline.hpp"
#include <QImage>
#include <QWidget>
#include <functional>
#include <unordered_map>

class QSpinBox;
class QToolButton;

namespace imageeditor::ui {
class SnapshotFrameList;

// A document's ordered, immutable snapshots. Full-resolution image decoding
// belongs to capture/export/preview; this panel retains only small thumbnails.
class SnapshotTimelinePanel final : public QWidget {
public:
    explicit SnapshotTimelinePanel(QWidget* parent = nullptr);
    ~SnapshotTimelinePanel() override;

    void setTimeline(const core::SnapshotTimelineState&);
    [[nodiscard]] core::SnapshotFrameId selectedFrameId() const;
    void setSelectedFrameId(core::SnapshotFrameId);
    void setBusy(bool);
    void setDocumentAvailable(bool);
    void setPreviewActive(bool);
    void setPlaying(bool);
    [[nodiscard]] bool previewActive() const;
    QSize sizeHint() const override { return {760, 164}; }

    std::function<void(core::SnapshotFrameId)> onSelected;
    std::function<void()> onAdd;
    std::function<void(core::SnapshotFrameId)> onDelete;
    std::function<void(core::SnapshotFrameId)> onCapture;
    std::function<void(core::SnapshotFrameId, std::uint32_t)> onDurationChanged;
    // Destination is the final zero-based frame position, after removal.
    std::function<void(core::SnapshotFrameId, std::size_t)> onReorder;
    std::function<void(bool)> onPreview;
    std::function<void(bool)> onPlay;
    std::function<void(core::SnapshotFrameId)> onInsert;
    std::function<void(core::SnapshotFrameId)> onExport;
    std::function<void()> onClose;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Thumbnail {
        std::shared_ptr<const core::SnapshotImage> source;
        QImage image;
        std::uint64_t lastUse {};
    };
    QImage thumbnail(core::SnapshotFrameId);
    const core::SnapshotFrame* selectedFrame() const;
    void refreshControls();
    void refreshFrameLabels();
    SnapshotFrameList* frames_ {};
    QToolButton* add_ {};
    QToolButton* delete_ {};
    QToolButton* capture_ {};
    QToolButton* preview_ {};
    QToolButton* play_ {};
    QToolButton* insert_ {};
    QToolButton* export_ {};
    QSpinBox* duration_ {};
    core::SnapshotTimelineState timeline_;
    std::unordered_map<core::SnapshotFrameId, std::size_t> frameRows_;
    std::unordered_map<core::SnapshotFrameId, Thumbnail> thumbnails_;
    std::uint64_t thumbnailClock_ {};
    bool busy_ {};
    bool documentAvailable_ {};
};
} // namespace imageeditor::ui
