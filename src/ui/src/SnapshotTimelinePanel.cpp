#include "imageeditor/ui/SnapshotTimelinePanel.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QBuffer>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

namespace imageeditor::ui {
namespace {
constexpr int frameIdRole = Qt::UserRole;
constexpr int durationRole = Qt::UserRole + 2;
constexpr int capturedRole = Qt::UserRole + 3;
constexpr QSize frameTileSize {120, 104};
constexpr QSize thumbnailSize {108, 72};
// This limits decoded preview memory, not the document's frame count.
constexpr std::size_t thumbnailCacheLimit = 64;
constexpr auto frameMimeType = "application/x-vulkana-snapshot-frame";

QImage decodeThumbnail(const core::SnapshotImage& source)
{
    if (source.png.empty() || source.extent.empty()) return {};
    auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(source.png.data()),
        qsizetype(source.png.size()));
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) return {};
    QImageReader reader(&buffer, "PNG");
    const auto size = QSize(int(source.extent.width), int(source.extent.height))
        .scaled(thumbnailSize * 2, Qt::KeepAspectRatio);
    reader.setScaledSize(size);
    // Some image plugins decode at full size before scaling. Never retain
    // their full-resolution result in the timeline thumbnail cache.
    auto image = reader.read();
    if (image.width() > size.width() || image.height() > size.height())
        image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

class SnapshotFrameDelegate final : public QStyledItemDelegate {
public:
    explicit SnapshotFrameDelegate(QObject* parent,
        std::function<QImage(core::SnapshotFrameId)> thumbnail)
        : QStyledItemDelegate(parent), thumbnail_(std::move(thumbnail)) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return frameTileSize;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        painter->save();
        if (!(option.state & QStyle::State_Enabled)) painter->setOpacity(.45);
        const auto tile = option.rect.adjusted(2, 2, -2, -2);
        const bool selected = option.state & QStyle::State_Selected;
        painter->fillRect(tile, themeColor(selected ? ThemeColor::Control : ThemeColor::Surface));
        const QRect imageArea(tile.x() + 4, tile.y() + 4, thumbnailSize.width(), thumbnailSize.height());
        painter->fillRect(imageArea, themeColor(ThemeColor::Canvas));
        const auto image = thumbnail_(index.data(frameIdRole).toULongLong());
        if (!image.isNull()) {
            const auto size = image.size().scaled(imageArea.size(), Qt::KeepAspectRatio);
            const QRect target(imageArea.center().x() - size.width() / 2,
                imageArea.center().y() - size.height() / 2, size.width(), size.height());
            painter->save();
            painter->setClipRect(target);
            for (int y = target.top(); y <= target.bottom(); y += 6)
                for (int x = target.left(); x <= target.right(); x += 6)
                    painter->fillRect(x, y, 6, 6, themeColor(
                        ((x - target.left()) / 6 + (y - target.top()) / 6) % 2
                            ? ThemeColor::CheckerDark : ThemeColor::CheckerLight));
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawImage(target, image);
            painter->restore();
        } else {
            const auto icon = toolGlyph(ToolGlyph::Camera, themeColor(ThemeColor::SecondaryText));
            icon.paint(painter, QRect(imageArea.center().x() - 9, imageArea.top() + 14, 18, 18));
            painter->setPen(themeColor(ThemeColor::SecondaryText));
            painter->drawText(imageArea.adjusted(3, 35, -3, -3), Qt::AlignHCenter | Qt::AlignTop,
                i18n::text(index.data(capturedRole).toBool()
                    ? "snapshot.timeline.unavailable" : "snapshot.timeline.empty"));
        }
        const QRect caption(tile.left() + 7, imageArea.bottom() + 3, tile.width() - 14, 20);
        painter->setPen(themeColor(selected ? ThemeColor::Text : ThemeColor::SecondaryText));
        painter->drawText(caption, Qt::AlignLeft | Qt::AlignVCenter, QString::number(index.row()));
        painter->drawText(caption, Qt::AlignRight | Qt::AlignVCenter,
            i18n::text("snapshot.timeline.duration.value").arg(index.data(durationRole).toUInt()));
        painter->setPen(QPen(themeColor(selected ? ThemeColor::Accent : ThemeColor::Border), 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(tile.adjusted(0, 0, -1, -1));
        painter->restore();
    }
private:
    std::function<QImage(core::SnapshotFrameId)> thumbnail_;
};
} // namespace

class SnapshotFrameList final : public QListWidget {
public:
    explicit SnapshotFrameList(QWidget* parent) : QListWidget(parent)
    {
        setObjectName(QStringLiteral("SnapshotTimelineFrames"));
        setLayoutDirection(Qt::LeftToRight);
        setViewMode(QListView::IconMode);
        setFlow(QListView::LeftToRight);
        setWrapping(false);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setUniformItemSizes(true);
        setGridSize(frameTileSize);
        setSpacing(0);
        setFrameShape(QFrame::NoFrame);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        // QAbstractScrollArea dispatches drops through its viewport, not the
        // outer list widget. Our custom frame MIME is handled there.
        viewport()->setAcceptDrops(true);
        setDropIndicatorShown(false);
        setItemDelegate(new SnapshotFrameDelegate(this, [this](auto id) {
            return thumbnail ? thumbnail(id) : QImage{};
        }));
        setStyleSheet(QStringLiteral("QListWidget { background: transparent; border: none; padding: 0; }"));
        i18n::setAccessibleName(this, i18n::text("snapshot.timeline.title"));
        scrollTimer_.setInterval(30);
        connect(&scrollTimer_, &QTimer::timeout, this, [this] {
            const int direction = dragPosition_.x() < 28 ? -1
                : dragPosition_.x() >= viewport()->width() - 28 ? 1 : 0;
            if (direction) horizontalScrollBar()->setValue(horizontalScrollBar()->value() + direction * 16);
            viewport()->update();
        });
    }
    std::function<void(core::SnapshotFrameId, std::size_t)> onMove;
    std::function<void()> onDelete;
    std::function<void()> onPreviewToggle;
    std::function<void()> onPlayToggle;
    std::function<QImage(core::SnapshotFrameId)> thumbnail;

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::ShortcutOverride) {
            const auto* key = static_cast<QKeyEvent*>(event);
            switch (key->key()) {
            case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
            case Qt::Key_Home: case Qt::Key_End: case Qt::Key_Delete:
            case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter:
                // Frame navigation must not nudge/delete document pixels or
                // start a canvas pan merely because the list owns focus.
                event->accept();
                return true;
            default: break;
            }
        }
        return QListWidget::event(event);
    }
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Delete) {
            if (!event->isAutoRepeat() && onDelete) onDelete();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Space) {
            if (!event->isAutoRepeat() && onPlayToggle) onPlayToggle();
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            if (!event->isAutoRepeat() && onPreviewToggle) onPreviewToggle();
            event->accept();
            return;
        }
        QListWidget::keyPressEvent(event);
    }
    void startDrag(Qt::DropActions) override
    {
        if (!currentItem()) return;
        auto* drag = new QDrag(this);
        auto* mime = new QMimeData;
        mime->setData(frameMimeType, QByteArray::number(currentItem()->data(frameIdRole).toULongLong()));
        drag->setMimeData(mime);
        const auto image = thumbnail ? thumbnail(currentItem()->data(frameIdRole).toULongLong()) : QImage{};
        if (!image.isNull()) {
            drag->setPixmap(QPixmap::fromImage(image.scaled(thumbnailSize, Qt::KeepAspectRatio,
                Qt::SmoothTransformation)));
            drag->setHotSpot({thumbnailSize.width() / 2, thumbnailSize.height() / 2});
        }
        const QPointer<SnapshotFrameList> guard(this);
        drag->exec(Qt::MoveAction);
        if (guard) guard->clearDropIndicator();
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->source() != this || !event->mimeData()->hasFormat(frameMimeType)) {
            event->ignore();
            return;
        }
        dragPosition_ = event->position().toPoint();
        dragging_ = true;
        scrollTimer_.start();
        event->acceptProposedAction();
        viewport()->update();
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->source() != this || !event->mimeData()->hasFormat(frameMimeType)) {
            event->ignore();
            return;
        }
        dragPosition_ = event->position().toPoint();
        event->acceptProposedAction();
        viewport()->update();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        clearDropIndicator();
        event->accept();
    }
    void dropEvent(QDropEvent* event) override
    {
        if (event->source() != this || !event->mimeData()->hasFormat(frameMimeType)) {
            event->ignore();
            return;
        }
        const auto id = event->mimeData()->data(frameMimeType).toULongLong();
        int source = -1;
        for (int row = 0; row < count(); ++row)
            if (item(row)->data(frameIdRole).toULongLong() == id) { source = row; break; }
        int destination = insertionAt(event->position().toPoint());
        if (destination > source) --destination;
        clearDropIndicator();
        if (source < 0 || destination < 0) { event->ignore(); return; }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        // The document is authoritative. It publishes the reordered state
        // after its undoable operation; the view never performs a second move.
        if (destination != source && onMove) onMove(id, std::size_t(destination));
    }
    void paintEvent(QPaintEvent* event) override
    {
        QListWidget::paintEvent(event);
        if (!dragging_ || !count()) return;
        const int insertion = insertionAt(dragPosition_);
        const auto target = visualItemRect(item(std::min(insertion, count() - 1)));
        const int x = insertion == count() ? target.right() : target.left();
        QPainter painter(viewport());
        painter.setPen(QPen(themeColor(ThemeColor::Accent), 2));
        painter.drawLine(x, target.top() + 2, x, target.bottom() - 2);
    }
    void wheelEvent(QWheelEvent* event) override
    {
        const auto pixels = event->pixelDelta();
        const auto angles = event->angleDelta();
        const int delta = !pixels.isNull() ? (pixels.x() ? pixels.x() : pixels.y())
            : (angles.x() ? angles.x() : angles.y()) / 2;
        if (!delta) { QListWidget::wheelEvent(event); return; }
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta);
        event->accept();
    }

private:
    int insertionAt(QPoint position) const
    {
        if (const auto* target = itemAt(position)) {
            const auto rect = visualItemRect(target);
            return row(target) + (position.x() >= rect.center().x() ? 1 : 0);
        }
        return !count() || position.x() < visualItemRect(item(0)).left() ? 0 : count();
    }
    void clearDropIndicator()
    {
        dragging_ = false;
        scrollTimer_.stop();
        viewport()->update();
    }
    QTimer scrollTimer_;
    QPoint dragPosition_;
    bool dragging_ {};
};

SnapshotTimelinePanel::SnapshotTimelinePanel(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("SnapshotTimelinePanel"));
    setFixedHeight(164);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 5, 8, 4);
    layout->setSpacing(3);
    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(5);
    auto* title = i18n::make<QLabel>(i18n::text("snapshot.timeline.title"), this);
    auto titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    toolbar->addWidget(title);
    toolbar->addSpacing(8);
    const auto button = [this, toolbar](const char* name, ToolGlyph glyph, const char* key) {
        auto* control = new QToolButton(this);
        control->setObjectName(QString::fromLatin1(name));
        control->setIcon(toolGlyph(glyph));
        control->setIconSize({16, 16});
        control->setFixedSize(28, 28);
        control->setAutoRaise(true);
        control->setStyleSheet(QStringLiteral("QToolButton { padding: 3px; border-radius: 4px; }"));
        i18n::setToolTip(control, i18n::text(key));
        i18n::setAccessibleName(control, i18n::text(key));
        toolbar->addWidget(control);
        return control;
    };
    capture_ = button("SnapshotTimelineCapture", ToolGlyph::Camera, "snapshot.timeline.capture");
    capture_->setMaximumWidth(QWIDGETSIZE_MAX);
    capture_->setMinimumWidth(0);
    capture_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    i18n::setText(capture_, i18n::text("snapshot.timeline.capture"));
    add_ = button("SnapshotTimelineAdd", ToolGlyph::Plus, "snapshot.timeline.add");
    delete_ = button("SnapshotTimelineDelete", ToolGlyph::Trash, "snapshot.timeline.delete");
    toolbar->addSpacing(5);
    preview_ = button("SnapshotTimelinePreview", ToolGlyph::Visible, "snapshot.timeline.preview");
    preview_->setCheckable(true);
    play_ = button("SnapshotTimelinePlay", ToolGlyph::Play, "snapshot.timeline.play");
    play_->setCheckable(true);
    insert_ = button("SnapshotTimelineInsertLayer", ToolGlyph::NewLayer, "snapshot.timeline.insert");
    export_ = button("SnapshotTimelineExport", ToolGlyph::Export, "snapshot.timeline.export");
    toolbar->addSpacing(5);
    auto* durationLabel = i18n::make<QLabel>(i18n::text("snapshot.timeline.duration"), this);
    toolbar->addWidget(durationLabel);
    duration_ = new QSpinBox(this);
    duration_->setObjectName(QStringLiteral("SnapshotTimelineDuration"));
    duration_->setRange(1, std::numeric_limits<int>::max());
    duration_->setValue(100);
    duration_->setSingleStep(10);
    duration_->setKeyboardTracking(false);
    duration_->setFixedHeight(28);
    duration_->setMinimumWidth(106);
    duration_->setMaximumWidth(150);
    duration_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    i18n::setSuffix(duration_, i18n::text("snapshot.timeline.duration.suffix"));
    i18n::setAccessibleName(duration_, i18n::text("snapshot.timeline.duration"));
    durationLabel->setBuddy(duration_);
    toolbar->addWidget(duration_);
    toolbar->addStretch(1);
    auto* close = button("SnapshotTimelineClose", ToolGlyph::Close, "snapshot.timeline.close");
    layout->addLayout(toolbar);
    frames_ = new SnapshotFrameList(this);
    frames_->thumbnail = [this](auto id) { return thumbnail(id); };
    layout->addWidget(frames_, 1);

    connect(add_, &QToolButton::clicked, this, [this] { if (onAdd) onAdd(); });
    connect(delete_, &QToolButton::clicked, this, [this] { if (onDelete) onDelete(selectedFrameId()); });
    connect(capture_, &QToolButton::clicked, this, [this] { if (onCapture) onCapture(selectedFrameId()); });
    connect(preview_, &QToolButton::toggled, this, [this](bool enabled) { if (onPreview) onPreview(enabled); });
    connect(play_, &QToolButton::toggled, this, [this](bool enabled) { if (onPlay) onPlay(enabled); });
    connect(insert_, &QToolButton::clicked, this, [this] { if (onInsert) onInsert(selectedFrameId()); });
    connect(export_, &QToolButton::clicked, this, [this] { if (onExport) onExport(selectedFrameId()); });
    connect(close, &QToolButton::clicked, this, [this] { if (onClose) onClose(); });
    connect(duration_, &QSpinBox::valueChanged, this, [this](int value) {
        const auto* frame = selectedFrame();
        if (frame && frame->durationMs != std::uint32_t(value) && onDurationChanged)
            onDurationChanged(frame->id, std::uint32_t(value));
    });
    connect(frames_, &QListWidget::currentRowChanged, this, [this] {
        refreshControls();
        if (onSelected) onSelected(selectedFrameId());
    });
    frames_->onMove = [this](auto id, auto destination) { if (onReorder) onReorder(id, destination); };
    frames_->onDelete = [this] { if (delete_->isEnabled()) delete_->click(); };
    frames_->onPreviewToggle = [this] { if (preview_->isEnabled()) preview_->click(); };
    frames_->onPlayToggle = [this] { if (play_->isEnabled()) play_->click(); };
    connect(&i18n::Localization::instance(), &i18n::Localization::languageChanged,
        this, [this] { refreshFrameLabels(); frames_->viewport()->update(); });
    setTimeline(timeline_);
}

SnapshotTimelinePanel::~SnapshotTimelinePanel() = default;

void SnapshotTimelinePanel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), themeColor(ThemeColor::Surface));
    painter.setPen(themeColor(ThemeColor::Border));
    painter.drawLine(rect().topLeft(), rect().topRight());
}

void SnapshotTimelinePanel::setTimeline(const core::SnapshotTimelineState& timeline)
{
    if (timeline_ == timeline && frames_->count() == int(timeline.frames.size())) return;
    const auto selected = selectedFrameId();
    const auto previousRow = frames_->currentRow();
    const QSignalBlocker blocker(frames_);
    bool structureChanged = frames_->count() != int(timeline.frames.size());
    for (int row = 0; !structureChanged && row < frames_->count(); ++row)
        structureChanged = frames_->item(row)->data(frameIdRole).toULongLong() != timeline.frames[std::size_t(row)].id;
    if (structureChanged) frames_->clear();
    frameRows_.clear();
    frameRows_.reserve(timeline.frames.size());
    for (std::size_t row = 0; row < timeline.frames.size(); ++row) {
        const auto& frame = timeline.frames[row];
        frameRows_.emplace(frame.id, row);
        auto* item = structureChanged ? new QListWidgetItem(frames_) : frames_->item(int(row));
        item->setData(frameIdRole, QVariant::fromValue(qulonglong(frame.id)));
        item->setData(durationRole, frame.durationMs);
        item->setData(capturedRole, bool(frame.image));
    }
    timeline_ = timeline;
    std::erase_if(thumbnails_, [this](const auto& entry) {
        const auto row = frameRows_.find(entry.first);
        return row == frameRows_.end() || timeline_.frames[row->second].image != entry.second.source;
    });
    int selectedRow = -1;
    for (std::size_t row = 0; row < timeline_.frames.size(); ++row)
        if (timeline_.frames[row].id == selected) { selectedRow = int(row); break; }
    if (selectedRow < 0 && frames_->count())
        selectedRow = std::clamp(previousRow, 0, frames_->count() - 1);
    frames_->setCurrentRow(selectedRow);
    refreshFrameLabels();
    refreshControls();
    frames_->viewport()->update();
    if (selected != selectedFrameId() && onSelected) onSelected(selectedFrameId());
}

QImage SnapshotTimelinePanel::thumbnail(core::SnapshotFrameId id)
{
    // Decoding is driven only by visible-item painting (or its drag pixmap),
    // never by document loading, hidden panels or offscreen list entries.
    if (!isVisible()) return {};
    const auto row = frameRows_.find(id);
    if (row == frameRows_.end()) return {};
    const auto& source = timeline_.frames[row->second].image;
    if (!source) return {};
    if (auto cached = thumbnails_.find(id); cached != thumbnails_.end()) {
        cached->second.lastUse = ++thumbnailClock_;
        return cached->second.image;
    }
    if (thumbnails_.size() >= thumbnailCacheLimit) {
        const auto oldest = std::min_element(thumbnails_.begin(), thumbnails_.end(),
            [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
        thumbnails_.erase(oldest);
    }
    auto image = decodeThumbnail(*source);
    thumbnails_.emplace(id, Thumbnail{source, image, ++thumbnailClock_});
    // No QImage is stored in a list-item role: eviction releases the decoded
    // thumbnail instead of leaving an implicit shared copy in every row.
    return image;
}

core::SnapshotFrameId SnapshotTimelinePanel::selectedFrameId() const
{
    return frames_->currentItem() ? frames_->currentItem()->data(frameIdRole).toULongLong() : 0;
}

void SnapshotTimelinePanel::setSelectedFrameId(core::SnapshotFrameId id)
{
    const QSignalBlocker blocker(frames_);
    for (int row = 0; row < frames_->count(); ++row) {
        if (frames_->item(row)->data(frameIdRole).toULongLong() != id) continue;
        frames_->setCurrentRow(row);
        frames_->scrollToItem(frames_->item(row));
        refreshControls();
        return;
    }
}

const core::SnapshotFrame* SnapshotTimelinePanel::selectedFrame() const
{
    const auto selected = selectedFrameId();
    const auto frame = std::find_if(timeline_.frames.begin(), timeline_.frames.end(),
        [selected](const auto& value) { return value.id == selected; });
    return frame == timeline_.frames.end() ? nullptr : &*frame;
}

void SnapshotTimelinePanel::refreshControls()
{
    const auto* frame = selectedFrame();
    const bool available = documentAvailable_ && !busy_;
    const bool editable = available && !play_->isChecked();
    const bool captured = frame && frame->image;
    frames_->setEnabled(available);
    frames_->setDragEnabled(editable);
    add_->setEnabled(editable);
    delete_->setEnabled(editable && frame);
    capture_->setEnabled(editable && frame);
    duration_->setEnabled(editable && frame);
    preview_->setEnabled(available && captured);
    play_->setEnabled(available && std::ranges::any_of(timeline_.frames, [](const auto& item) { return bool(item.image); }));
    insert_->setEnabled(editable && captured);
    export_->setEnabled(editable && captured);
    if (!captured || !documentAvailable_) setPreviewActive(false);
    const QSignalBlocker blocker(duration_);
    duration_->setValue(frame ? int(std::min(frame->durationMs, std::uint32_t(std::numeric_limits<int>::max()))) : 100);
    i18n::setToolTip(capture_, i18n::text(captured ? "snapshot.timeline.replace" : "snapshot.timeline.capture"));
}

void SnapshotTimelinePanel::refreshFrameLabels()
{
    for (int row = 0; row < frames_->count(); ++row) {
        auto* item = frames_->item(row);
        auto label = i18n::text("snapshot.timeline.frame").arg(row)
            + QStringLiteral(" · ") + i18n::text("snapshot.timeline.duration.value").arg(item->data(durationRole).toUInt());
        if (!item->data(capturedRole).toBool()) label += QStringLiteral(" · ") + i18n::text("snapshot.timeline.empty");
        item->setToolTip(label);
        item->setData(Qt::AccessibleTextRole, QString(label));
    }
}

void SnapshotTimelinePanel::setBusy(bool busy)
{
    busy_ = busy;
    refreshControls();
}

void SnapshotTimelinePanel::setDocumentAvailable(bool available)
{
    documentAvailable_ = available;
    refreshControls();
}

void SnapshotTimelinePanel::setPreviewActive(bool active)
{
    const QSignalBlocker blocker(preview_);
    const auto* frame = selectedFrame();
    preview_->setChecked(active && documentAvailable_ && frame && frame->image);
}

bool SnapshotTimelinePanel::previewActive() const
{
    return preview_->isChecked();
}

void SnapshotTimelinePanel::setPlaying(bool playing)
{
    const QSignalBlocker blocker(play_);
    play_->setChecked(playing);
    play_->setIcon(toolGlyph(playing ? ToolGlyph::Pause : ToolGlyph::Play));
    const auto label = i18n::text(playing ? "snapshot.timeline.pause" : "snapshot.timeline.play");
    i18n::setToolTip(play_, label);
    i18n::setAccessibleName(play_, label);
    refreshControls();
}
} // namespace imageeditor::ui
