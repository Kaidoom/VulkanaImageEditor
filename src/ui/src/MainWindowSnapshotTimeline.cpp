#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/SnapshotTimelinePanel.hpp"
#include "imageeditor/ui/SnapshotPlayback.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QDialog>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSinglePointEvent>
#include <QStatusBar>
#include <QTouchEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
const core::SnapshotFrame* findFrame(const core::Document* document, core::SnapshotFrameId id)
{
    if (!document) return nullptr;
    const auto& frames = document->snapshotTimeline().frames;
    const auto found = std::ranges::find(frames, id, &core::SnapshotFrame::id);
    return found == frames.end() ? nullptr : &*found;
}
QImage decode(const core::SnapshotImage& source)
{
    if (source.png.size() > std::size_t(std::numeric_limits<int>::max()))
        throw std::runtime_error("Snapshot image exceeds the decoder byte limit.");
    auto result = QImage::fromData(reinterpret_cast<const uchar*>(source.png.data()),
        int(source.png.size()), "PNG");
    if (result.isNull() || result.size() != QSize(int(source.extent.width), int(source.extent.height)))
        throw std::runtime_error("Cannot decode the snapshot image.");
    result.setColorSpace(QColorSpace::SRgb);
    result.setDotsPerMeterX(qRound(source.dotsPerInch / .0254));
    result.setDotsPerMeterY(qRound(source.dotsPerInch / .0254));
    return result;
}
bool belongsTo(const QObject* target, const QObject* owner)
{
    for (; target; target = popupLogicalParent(target))
        if (target == owner) return true;
    return false;
}

class PreviewShade final : public QWidget {
public:
    explicit PreviewShade(QWidget* parent) : QWidget(parent) { setFocusPolicy(Qt::NoFocus); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), QColor(0, 0, 0, 160)); }
};

// The preview owns one input boundary, rather than asking every editor tool,
// dock and shortcut to know about snapshot mode. Native window carriers are
// checked as well as widgets so canvas events cannot bypass the shield.
class SnapshotPreview final : public QWidget {
public:
    SnapshotPreview(OverlayDockWorkspace& workspace,
        MainWindow& host, SnapshotTimelinePanel& timeline, render::CanvasWindow& canvas,
        std::function<void()> close)
        : QWidget(workspace.panelOverlay()), workspace_(workspace),
          host_(host), timeline_(timeline), canvas_(canvas), close_(std::move(close))
    {
        setObjectName("SnapshotTimelinePreviewOverlay");
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
        title_ = new QLabel(this);
        title_->setObjectName("SnapshotTimelinePreviewTitle");
        auto font = title_->font(); font.setBold(true); title_->setFont(font);
        back_ = new QToolButton(this);
        back_->setObjectName("SnapshotTimelineExitPreview");
        back_->setIcon(toolGlyph(ToolGlyph::Close));
        back_->setIconSize({16, 16});
        back_->setFixedSize(28, 28);
        back_->setAutoRaise(true);
        back_->setStyleSheet(QStringLiteral("QToolButton { padding: 3px; border-radius: 4px; }"));
        i18n::setToolTip(back_, i18n::text("snapshot.timeline.back"));
        i18n::setAccessibleName(back_, i18n::text("snapshot.timeline.back"));
        back_->setFocusPolicy(Qt::NoFocus);
        connect(back_, &QToolButton::clicked, this, [this] { dismiss(); });
        topShade_ = new PreviewShade(&host_);
        topShade_->setObjectName("SnapshotTimelinePreviewTopShade");
        topShade_->hide();
    }
    ~SnapshotPreview() override { deactivate(); delete topShade_.data(); }

    void activate()
    {
        previousFocus_ = QApplication::focusWidget();
        previousProxy_ = workspace_.canvasContainer()->focusProxy();
        active_ = true;
        qApp->installEventFilter(this);
        layoutPreview();
        if (auto* frames = timeline_.findChild<QListWidget*>("SnapshotTimelineFrames")) {
            workspace_.canvasContainer()->setFocusProxy(frames);
            frames->setFocus(Qt::OtherFocusReason);
        }
    }
    void deactivate()
    {
        if (!active_) return;
        active_ = false;
        qApp->removeEventFilter(this);
        if (topShade_) topShade_->hide();
        workspace_.canvasContainer()->setFocusProxy(previousProxy_);
        if (host_.isVisible() && previousFocus_ && previousFocus_->isVisible())
            previousFocus_->setFocus(Qt::OtherFocusReason);
    }
    void setTitle(i18n::Message title) { i18n::setText(title_, std::move(title)); layoutPreview(); }
    void setFrame(const std::shared_ptr<const core::SnapshotImage>& source, i18n::Message title)
    {
        const auto found = std::ranges::find(cache_, source, &CachedImage::source);
        if (found != cache_.end()) {
            auto entry = std::move(*found);
            cache_.erase(found);
            image_ = entry.pixels;
            cache_.push_back(std::move(entry));
        } else {
            image_ = decode(*source);
            constexpr qint64 budget = 64 * 1024 * 1024;
            const auto bytes = image_.sizeInBytes();
            while (!cache_.empty() && cachedBytes_ + bytes > budget) {
                cachedBytes_ -= cache_.front().pixels.sizeInBytes();
                cache_.erase(cache_.begin());
            }
            if (bytes <= budget) { cache_.push_back({source, image_}); cachedBytes_ += bytes; }
        }
        setTitle(std::move(title));
    }
protected:
    void resizeEvent(QResizeEvent*) override { layoutPreview(); }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 160));
        if (imageRect_.isEmpty()) return;
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 65));
        painter.drawRoundedRect(cardRect_.adjusted(-4, -2, 4, 6), 9, 9);
        painter.setBrush(themeColor(ThemeColor::Surface));
        painter.setPen(themeColor(ThemeColor::Border));
        painter.drawRoundedRect(cardRect_, 6, 6);
        painter.setClipRect(imageRect_);
        painter.fillRect(imageRect_, themeColor(ThemeColor::CheckerLight));
        constexpr int step = 10;
        for (int y = imageRect_.top(); y <= imageRect_.bottom(); y += step)
            for (int x = imageRect_.left(); x <= imageRect_.right(); x += step)
                if (((x-imageRect_.left())/step + (y-imageRect_.top())/step) % 2)
                    painter.fillRect(QRect(x,y,step,step), themeColor(ThemeColor::CheckerDark));
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(imageRect_, image_);
    }
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!active_) return false;
        if (watched == &host_ && (event->type() == QEvent::Close || event->type() == QEvent::Hide)) {
            dismiss(); return false; // Preserve the application's ordinary close/save flow.
        }
        if (watched == &host_ && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest))
            layoutPreview();
        const bool ours = watched == &canvas_ || watched == host_.windowHandle()
            || watched == workspace_.panelOverlay()->windowHandle()
            || belongsTo(watched, &host_) || belongsTo(watched, workspace_.panelOverlay());
        if (!ours) return false;
        if (qobject_cast<QMenu*>(watched) && event->type() == QEvent::Show) { dismiss(); return false; }
        if (QApplication::activeModalWidget() || workspace_.hasModalOverlay()) return false;

        auto* widget = qobject_cast<QWidget*>(watched);
        const bool nativeCarrier = qobject_cast<QWindow*>(watched) && watched != &canvas_;
        const bool inTimeline = belongsTo(watched, &timeline_);
        const bool inBack = belongsTo(watched, back_);
        const auto consume = [&] { event->accept(); return true; };
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease
            || event->type() == QEvent::ShortcutOverride) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape) {
                if (event->type() == QEvent::KeyPress) dismiss();
                return consume();
            }
            if (key->key() == Qt::Key_Alt || key->key() == Qt::Key_F10 || key->modifiers().testFlag(Qt::AltModifier)) {
                dismiss(); return false;
            }
            if (const auto* quit = host_.findChild<QAction*>("QuitAction"); quit
                && quit->shortcuts().contains(QKeySequence(key->keyCombination()))) {
                dismiss(); return false;
            }
            auto* focus = QApplication::focusWidget();
            const bool timelineFocus = inTimeline || (nativeCarrier && belongsTo(focus, &timeline_));
            if (!timelineFocus) return consume();
            if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
                if (event->type() == QEvent::KeyPress) {
                    auto* start = focus ? focus : &timeline_;
                    auto* next = start;
                    const bool backwards = key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier);
                    do {
                        next = backwards ? next->previousInFocusChain() : next->nextInFocusChain();
                        if (belongsTo(next, &timeline_) && next->isVisible() && next->isEnabled()
                            && (next->focusPolicy() & Qt::TabFocus)) {
                            next->setFocus(backwards ? Qt::BacktabFocusReason : Qt::TabFocusReason); break;
                        }
                    } while (next != start);
                }
                return consume();
            }
            // Editable timeline fields retain text editing. Other editor
            // shortcuts must not reach the live document from a focused frame.
            if (qobject_cast<QLineEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget)
                || qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus)) return false;
            if (key->modifiers() == Qt::NoModifier || key->modifiers() == Qt::KeypadModifier) {
                switch (key->key()) {
                case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
                case Qt::Key_Home: case Qt::Key_End: case Qt::Key_PageUp: case Qt::Key_PageDown:
                case Qt::Key_Space: case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Delete:
                    return false;
                default: break;
                }
            }
            return consume();
        }
        if (event->type() == QEvent::Shortcut) return consume();
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
            const auto* native = qobject_cast<QWindow*>(watched);
            if (!widget && !native) { event->ignore(); return true; }
            const auto local = static_cast<QDropEvent*>(event)->position().toPoint();
            const auto global = widget ? widget->mapToGlobal(local)
                : native->mapToGlobal(local);
            if (inTimeline || (nativeCarrier && timeline_.rect().contains(timeline_.mapFromGlobal(global))))
                return false;
            event->ignore(); // Never acknowledge an external move without importing its data.
            return true;
        }
        const bool press = event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonDblClick || event->type() == QEvent::TabletPress
            || event->type() == QEvent::TouchBegin;
        if (press) {
            QPoint global;
            if (event->type() == QEvent::TouchBegin) {
                const auto* touch = static_cast<QTouchEvent*>(event);
                if (touch->points().empty()) return consume();
                global = touch->points().front().globalPosition().toPoint();
            } else global = static_cast<QSinglePointEvent*>(event)->globalPosition().toPoint();
            const auto inside = [global](QWidget* target) {
                return target->isVisible() && target->rect().contains(target->mapFromGlobal(global));
            };
            if ((belongsTo(watched, host_.menuBar()) || nativeCarrier) && inside(host_.menuBar())) {
                dismiss(); return false;
            }
            // Ignored child presses may bubble to the editor's parent widget.
            // Their position remains inside the allowed control region.
            if (watched != &canvas_ && (inside(&timeline_) || inside(back_))) return false;
            if (!imageRect_.contains(mapFromGlobal(global))) dismiss();
            return consume(); // Dismissal never clicks through into the editor.
        }
        switch (event->type()) {
        case QEvent::MouseMove: case QEvent::MouseButtonRelease: case QEvent::Wheel:
        case QEvent::TabletMove: case QEvent::TabletRelease: case QEvent::TouchUpdate: case QEvent::TouchEnd:
        case QEvent::ContextMenu:
        case QEvent::NativeGesture: case QEvent::InputMethod:
            if (inTimeline || inBack || nativeCarrier) return false;
            return consume();
        default: return false;
        }
    }
private:
    void dismiss() { if (active_ && close_) close_(); }
    void layoutPreview()
    {
        const auto available = rect().adjusted(48, 76, -48, -36);
        const auto size = image_.size().scaled(available.size().expandedTo(QSize(1, 1)), Qt::KeepAspectRatio);
        const int header = 48;
        const auto backSize = back_->size();
        const int cardWidth = std::min(width() - 32,
            std::max(size.width() + 24, title_->sizeHint().width() + backSize.width() + 44));
        cardRect_ = QRect((width() - cardWidth)/2, (height() - size.height() - header - 12)/2,
            cardWidth, size.height() + header + 12);
        imageRect_ = QRect((width() - size.width())/2, cardRect_.top() + header, size.width(), size.height());
        back_->move(cardRect_.right() - backSize.width() - 11,
            cardRect_.top() + (header - backSize.height()) / 2);
        title_->setGeometry(cardRect_.left() + 12, cardRect_.top() + 8,
            std::max(0, back_->x() - cardRect_.left() - 24), 32);
        setProperty("SnapshotPreviewImageRect", imageRect_);
        if (topShade_ && active_) {
            const int top = host_.menuBar()->geometry().bottom() + 1;
            const int bottom = host_.mapFromGlobal(workspace_.mapToGlobal(QPoint{})).y();
            topShade_->setGeometry(0, top, host_.width(), std::max(0, bottom - top));
            topShade_->setVisible(bottom > top);
            topShade_->raise();
        }
        update();
    }
    QImage image_;
    struct CachedImage { std::shared_ptr<const core::SnapshotImage> source; QImage pixels; };
    std::vector<CachedImage> cache_;
    qint64 cachedBytes_{};
    OverlayDockWorkspace& workspace_;
    MainWindow& host_;
    SnapshotTimelinePanel& timeline_;
    render::CanvasWindow& canvas_;
    std::function<void()> close_;
    QLabel* title_{};
    QToolButton* back_{};
    QPointer<QWidget> topShade_, previousFocus_, previousProxy_;
    QRect cardRect_, imageRect_;
    bool active_{};
};
}

void MainWindow::createSnapshotTimeline(QMenu* menu)
{
    snapshotTimeline_ = new SnapshotTimelinePanel(centralWidget());
    static_cast<QVBoxLayout*>(centralWidget()->layout())->addWidget(snapshotTimeline_);
    snapshotTimeline_->hide();
    snapshotPlayback_ = new SnapshotPlayback(this);
    snapshotPlayback_->onPlayingChanged = [this](bool playing) { snapshotTimeline_->setPlaying(playing); };
    snapshotPlayback_->onFrame = [this](auto id) {
        if (!activeDocument_ || snapshotPreviewDocument_ != activeDocumentId()
            || fileBusy_ || workspaceDialog_) { finishSnapshotPreview(); return; }
        activeDocument_->selectedSnapshot = id;
        snapshotTimeline_->setSelectedFrameId(id);
        previewSnapshot(true);
    };
    snapshotTimelineAction_ = i18n::addAction(menu, toolGlyph(ToolGlyph::History), i18n::text("snapshot.timeline.title"));
    snapshotTimelineAction_->setObjectName("SnapshotTimelineVisibilityAction");
    snapshotTimelineAction_->setCheckable(true);
    connect(snapshotTimelineAction_, &QAction::toggled, this, [this](bool shown) {
        if (!shown) finishSnapshotPreview();
        snapshotTimeline_->setVisible(shown);
        refreshSnapshotTimeline();
    });
    snapshotTimeline_->onClose = [this] { snapshotTimelineAction_->setChecked(false); };
    // Button signals can run before the pointer router releases native capture.
    // Defer one event boundary, retaining document ownership across tab changes.
    const auto queue = [this](auto operation) {
        const auto owner = activeDocumentId();
        QTimer::singleShot(0, this, [this, owner, operation] {
            if (!owner || !isVisible() || !snapshotTimeline_->isVisible() || activeDocumentId() != owner) return;
            try { operation(); }
            catch (const std::exception& error) {
                refreshSnapshotTimeline();
                reportFileError(i18n::text("snapshot.timeline.failed").arg(QString::fromUtf8(error.what())));
            }
        });
    };
    snapshotTimeline_->onSelected = [this, queue](auto id) {
        if (!activeDocument_ || snapshotTimelineDocument_ != activeDocumentId()) return;
        activeDocument_->selectedSnapshot = id;
        if (snapshotTimeline_->previewActive()) queue([this, id] {
            if (snapshotTimeline_->previewActive() && snapshotTimeline_->selectedFrameId() == id) {
                if (snapshotPlayback_->currentFrame()) snapshotPlayback_->seek(id);
                else previewSnapshot(true);
            }
        });
        else if (snapshotPreview_) finishSnapshotPreview();
    };
    snapshotTimeline_->onAdd = [this, queue] { queue([this] {
        if (!session().document()) return;
        auto state = session().document()->snapshotTimeline();
        if (state.nextId == std::numeric_limits<core::SnapshotFrameId>::max()) return;
        const auto id = state.nextId++;
        state.frames.push_back({id, 100, {}});
        changeSnapshotTimeline(std::move(state), "Add snapshot frame");
        if (findFrame(session().document(), id)) {
            snapshotTimeline_->setSelectedFrameId(id); activeDocument_->selectedSnapshot = id;
        }
    }); };
    snapshotTimeline_->onDelete = [this, queue](auto id) { queue([this, id] {
        if (!session().document()) return;
        auto state = session().document()->snapshotTimeline();
        std::erase_if(state.frames, [id](const auto& frame) { return frame.id == id; });
        if (state.frames.empty()) {
            if (state.nextId == std::numeric_limits<core::SnapshotFrameId>::max()) return;
            state.frames.push_back({state.nextId++, 100, {}});
        }
        changeSnapshotTimeline(std::move(state), "Delete snapshot frame");
    }); };
    snapshotTimeline_->onReorder = [this, queue](auto id, auto position) { queue([this, id, position] {
        if (!session().document()) return;
        auto state = session().document()->snapshotTimeline();
        const auto found = std::ranges::find(state.frames, id, &core::SnapshotFrame::id);
        if (found == state.frames.end() || position >= state.frames.size()) return;
        auto frame = *found; state.frames.erase(found);
        state.frames.insert(state.frames.begin() + std::ptrdiff_t(position), std::move(frame));
        changeSnapshotTimeline(std::move(state), "Reorder snapshot frames");
    }); };
    snapshotTimeline_->onDurationChanged = [this, queue](auto id, auto duration) { queue([this, id, duration] {
        if (!session().document()) return;
        auto state = session().document()->snapshotTimeline();
        const auto found = std::ranges::find(state.frames, id, &core::SnapshotFrame::id);
        if (found == state.frames.end()) return;
        found->durationMs = duration;
        changeSnapshotTimeline(std::move(state), "Change frame duration");
    }); };
    snapshotTimeline_->onCapture = [this, queue](auto id) { queue([this, id] { captureSnapshot(id); }); };
    snapshotTimeline_->onPreview = [this, queue](bool enabled) { queue([this, enabled] { previewSnapshot(enabled); }); };
    snapshotTimeline_->onPlay = [this, queue](bool enabled) { queue([this, enabled] { playSnapshots(enabled); }); };
    snapshotTimeline_->onLoopChanged = [this](bool enabled) { snapshotPlayback_->setLooping(enabled); };
    snapshotTimeline_->onExportAnimation = [this, queue] { queue([this] { exportGif(); }); };
    snapshotTimeline_->onInsert = [this, queue](auto id) { queue([this, id] { insertSnapshotLayer(id); }); };
    snapshotTimeline_->onExport = [this, queue](auto id) { queue([this, id] { exportSnapshot(id); }); };
}

void MainWindow::refreshSnapshotTimeline()
{
    if (!snapshotTimeline_) return;
    const auto* document = activeDocument_ ? session().document() : nullptr;
    if (snapshotPreview_ && (snapshotPreviewDocument_ != activeDocumentId()
            || !document || snapshotPreviewRevision_ != document->revision()
            || snapshotPreviewContentState_ != document->contentState())) finishSnapshotPreview();
    snapshotTimeline_->setDocumentAvailable(document != nullptr);
    snapshotTimeline_->setBusy(fileBusy_ || workspaceDialog_ || bool(backgroundRemoval_) || bool(refinement_));
    if (!document) { snapshotTimelineDocument_ = 0; return; }
    const bool changed = snapshotTimelineDocument_ != activeDocumentId();
    if (changed) snapshotTimelineDocument_ = 0; // Ignore selection callbacks during rebinding.
    snapshotTimeline_->setTimeline(document->snapshotTimeline());
    if (changed) snapshotTimeline_->setSelectedFrameId(activeDocument_->selectedSnapshot);
    snapshotTimelineDocument_ = activeDocumentId();
    activeDocument_->selectedSnapshot = snapshotTimeline_->selectedFrameId();
}

bool MainWindow::changeSnapshotTimeline(core::SnapshotTimelineState state, const char* label)
{
    // Spin buttons step on press and while held. Their timeline-owned capture
    // is safe; all live canvas/edit gesture guards must still apply.
    if (!activeDocument_ || !session().document() || fileBusy_ || workspaceDialog_ || backgroundRemoval_
        || !shortcutGestureIdle(true, snapshotTimeline_) || (textController_ && textController_->active())) {
        statusBar()->showMessage(i18n::text("snapshot.timeline.finish.edit"), 4000);
        refreshSnapshotTimeline(); return false;
    }
    auto command = std::make_unique<core::ChangeSnapshotTimelineCommand>(session().document()->snapshotTimeline(), std::move(state), label);
    snapshotPlayback_->clear(); // Frame edits pause playback; viewing never enters history.
    if (!session().execute(std::move(command))) { refreshSnapshotTimeline(); return false; }
    // Timeline metadata can be edited without leaving its preview. Changes
    // to the actual artwork still invalidate it at the presentation boundary.
    if (snapshotPreview_) {
        snapshotPreviewRevision_ = session().document()->revision();
        snapshotPreviewContentState_ = session().document()->contentState();
    }
    fileState().untouched = false;
    synchronizeUi(false, false);
    if (snapshotPreview_) previewSnapshot(true);
    updateDocumentTitle();
    return true;
}

void MainWindow::captureSnapshot(core::SnapshotFrameId id)
try {
    if (!findFrame(session().document(), id) || fileBusy_ || workspaceDialog_ || backgroundRemoval_) return;
    if (!settleForFileOperation()) return;
    finishSnapshotPreview();
    const auto owner = activeDocument_;
    const auto extent = session().document()->canvas().extent;
    const auto dpi = session().document()->canvas().dotsPerInch;
    FlattenedDocumentResult rendered;
    QByteArray png;
    bool cancelled = false;
    {
        WorkspaceDialog presenter(*workspace_, *this);
        QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
        card.setObjectName("SnapshotTimelineCaptureProgress");
        card.setAttribute(Qt::WA_StyledBackground);
        auto* layout = new QVBoxLayout(&card);
        layout->addWidget(i18n::make<QLabel>(i18n::text("snapshot.timeline.preparing"), &card));
        auto* progress = new QProgressBar(&card); progress->setRange(0,1000); progress->setMinimumWidth(280);
        layout->addWidget(progress);
        auto* cancel = i18n::make<QPushButton>(i18n::text("main.window.layer.organization.cancel"), &card);
        layout->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, &card, &QDialog::reject);
        connect(&card, &QDialog::rejected, &card, [&] { cancelled = true; });
        const QScopedValueRollback busy(fileBusy_, true);
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        const QScopedValueRollback<QWidget*> activeProgress(fileProgress_, &card);
        snapshotTimeline_->setBusy(true);
        presenter.open(card);
        QElapsedTimer clock; clock.start();
        // Native-size output from the same authoritative compositor as export,
        // without the export-format dialog's separate output-size restrictions.
        rendered = flattenDocument(*owner->session.document(), [&](auto done, auto total) {
            if (fileInteractions_.progress && !fileInteractions_.progress(done, total)) cancelled = true;
            if (clock.elapsed() >= 12) {
                progress->setValue(int(done * 1000 / std::max<std::uint64_t>(total, 1)));
                QApplication::processEvents(QEventLoop::AllEvents, 4); clock.restart();
            }
            return !cancelled && !owner->closed && activeDocument_ == owner;
        });
        if (rendered && !cancelled) {
            auto image = rendered.image.convertToFormat(QImage::Format_RGBA8888);
            image.setColorSpace({});
            image.setDotsPerMeterX(qRound(dpi / .0254));
            image.setDotsPerMeterY(qRound(dpi / .0254));
            QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly);
            QImageWriter writer(&buffer, "PNG");
            writer.setCompression(1);
            if (!writer.write(image)) throw std::runtime_error(writer.errorString().toStdString());
        }
        QApplication::processEvents(QEventLoop::AllEvents, 4);
        presenter.finish();
    }
    refreshSnapshotTimeline();
    if (cancelled || rendered.cancelled || owner->closed || activeDocument_ != owner) return;
    if (!rendered) throw std::runtime_error(rendered.error.toStdString());
    auto image = std::make_shared<core::SnapshotImage>();
    image->extent = extent; image->dotsPerInch = dpi;
    image->png.resize(std::size_t(png.size()));
    std::memcpy(image->png.data(), png.constData(), std::size_t(png.size()));
    auto state = owner->session.document()->snapshotTimeline();
    const auto found = std::ranges::find(state.frames, id, &core::SnapshotFrame::id);
    if (found == state.frames.end()) return;
    const auto index = std::distance(state.frames.begin(), found);
    found->image = std::move(image);
    if (!changeSnapshotTimeline(std::move(state), "Capture canvas snapshot")) return;
    statusBar()->showMessage(i18n::text("snapshot.timeline.captured").arg(index), 4000);
} catch (const std::exception& error) {
    refreshSnapshotTimeline();
    reportFileError(i18n::text("snapshot.timeline.failed").arg(QString::fromUtf8(error.what())));
}

void MainWindow::finishSnapshotPreview()
{
    if (snapshotPlayback_) snapshotPlayback_->clear();
    if (snapshotTimeline_) snapshotTimeline_->setPreviewActive(false);
    if (!snapshotPreview_) return;
    static_cast<SnapshotPreview*>(snapshotPreview_.data())->deactivate();
    workspace_->setCanvasPreviewOverlay(nullptr, false);
    snapshotPreview_->hide(); snapshotPreview_->deleteLater(); snapshotPreview_.clear();
    snapshotPreviewImage_.reset(); snapshotPreviewDocument_ = 0;
}

void MainWindow::shutdownSnapshotTimeline()
{
    finishSnapshotPreview();
    if (!snapshotTimeline_) return;
    snapshotTimeline_->hide();
    snapshotTimeline_->onSelected = {}; snapshotTimeline_->onAdd = {};
    snapshotTimeline_->onDelete = {}; snapshotTimeline_->onCapture = {};
    snapshotTimeline_->onDurationChanged = {}; snapshotTimeline_->onReorder = {};
    snapshotTimeline_->onPreview = {}; snapshotTimeline_->onInsert = {};
    snapshotTimeline_->onPlay = {};
    snapshotTimeline_->onLoopChanged = {}; snapshotTimeline_->onExportAnimation = {};
    snapshotTimeline_->onExport = {}; snapshotTimeline_->onClose = {};
}

void MainWindow::previewSnapshot(bool enabled)
try {
    if (!enabled) { finishSnapshotPreview(); return; }
    if (!snapshotTimeline_->isVisible() || !snapshotTimelineAction_->isChecked()) {
        finishSnapshotPreview(); return;
    }
    if (fileBusy_ || workspaceDialog_ || QApplication::activePopupWidget() || QApplication::activeModalWidget()
        || backgroundRemoval_ || (!snapshotPreview_ && (!shortcutGestureIdle(true)
            || (textController_ && textController_->active())))) { finishSnapshotPreview(); return; }
    // Idle-gesture admission applies when entering preview, not on every frame.
    // Timeline controls legitimately own pointer capture between press/release;
    // a playback tick must not dismiss the overlay and reset Pause to Play.
    const auto* frame = findFrame(session().document(), snapshotTimeline_->selectedFrameId());
    if (!frame || !frame->image) { finishSnapshotPreview(); return; }
    const auto image = frame->image;
    const auto& frames = session().document()->snapshotTimeline().frames;
    const auto index = std::distance(frames.data(), frame);
    const auto title = i18n::text("snapshot.timeline.preview.title").arg(index);
    if (snapshotPreview_) {
        auto* preview = static_cast<SnapshotPreview*>(snapshotPreview_.data());
        if (snapshotPreviewImage_ == image) preview->setTitle(title);
        else preview->setFrame(image, title);
        snapshotPreviewImage_ = image;
        return;
    }
    auto previewGuard = std::make_unique<SnapshotPreview>(*workspace_, *this,
        *snapshotTimeline_, *canvasWindow_, [this] { finishSnapshotPreview(); });
    previewGuard->setFrame(image, title);
    auto* preview = previewGuard.release();
    snapshotPreview_ = preview;
    snapshotPreviewImage_ = image;
    snapshotPreviewDocument_ = activeDocumentId(); snapshotPreviewRevision_ = session().document()->revision();
    snapshotPreviewContentState_ = session().document()->contentState();
    snapshotTimeline_->setPreviewActive(true);
    workspace_->setCanvasPreviewOverlay(snapshotPreview_, true);
    preview->activate();
} catch (const std::exception& error) {
    finishSnapshotPreview();
    reportFileError(i18n::text("snapshot.timeline.failed").arg(QString::fromUtf8(error.what())));
}

void MainWindow::playSnapshots(bool enabled)
{
    if (!enabled) { snapshotPlayback_->pause(); snapshotTimeline_->setPlaying(false); return; }
    const auto* document = session().document();
    if (!document) { snapshotTimeline_->setPlaying(false); return; }
    auto id = snapshotTimeline_->selectedFrameId();
    const auto* frame = findFrame(document, id);
    if (!frame || !frame->image) {
        const auto& frames = document->snapshotTimeline().frames;
        const auto first = std::ranges::find_if(frames, [](const auto& item) { return bool(item.image); });
        if (first == frames.end()) { snapshotTimeline_->setPlaying(false); return; }
        id = first->id;
        snapshotTimeline_->setSelectedFrameId(id);
        activeDocument_->selectedSnapshot = id;
    }
    previewSnapshot(true);
    if (!snapshotPreview_) { snapshotTimeline_->setPlaying(false); return; }
    snapshotPlayback_->setTimeline(document->snapshotTimeline());
    snapshotPlayback_->setLooping(snapshotTimeline_->looping());
    snapshotPlayback_->play(id);
}

void MainWindow::insertSnapshotLayer(core::SnapshotFrameId id)
try {
    if (fileBusy_ || workspaceDialog_ || backgroundRemoval_ || !settleForFileOperation()) return;
    const auto* frame = findFrame(session().document(), id);
    if (!frame || !frame->image) return;
    const auto& frames = session().document()->snapshotTimeline().frames;
    const auto image = decode(*frame->image).convertToFormat(QImage::Format_RGBA8888);
    const auto extent = frame->image->extent;
    const auto stride = std::size_t(extent.width) * 4;
    std::vector<std::byte> pixels(stride * extent.height);
    for (std::uint32_t y = 0; y < extent.height; ++y)
        std::memcpy(pixels.data() + std::size_t(y) * stride, image.constScanLine(int(y)), stride);
    auto layer = core::Layer::raster(i18n::text("snapshot.timeline.layer").arg(std::distance(frames.data(), frame)).toStdString(),
        std::make_shared<core::ContiguousRasterSurface>(extent, std::move(pixels)));
    finishSnapshotPreview();
    const auto layerId = layer.id;
    if (!executeDocumentCommand(std::make_unique<core::AddLayerCommand>(std::move(layer),
            session().document()->layers().size(), session().activeLayer()))) return;
    session().setActiveLayer(layerId); fileState().untouched = false;
    synchronizeUi(true, false); updateDocumentTitle();
} catch (const std::exception& error) {
    reportFileError(i18n::text("snapshot.timeline.failed").arg(QString::fromUtf8(error.what())));
}

void MainWindow::exportSnapshot(core::SnapshotFrameId id)
try {
    if (fileBusy_ || workspaceDialog_) return;
    snapshotPlayback_->pause();
    const auto* frame = findFrame(session().document(), id);
    if (!frame || !frame->image) return;
    const auto image = frame->image; // Pin the frozen result through the file chooser.
    const QScopedValueRollback busy(fileBusy_, true);
    QString path;
    if (fileInteractions_.chooseSnapshotExportPath) path = fileInteractions_.chooseSnapshotExportPath();
    else {
        QFileDialog chooser(this, i18n::text("snapshot.timeline.export.title"), lastExportDirectory(),
            i18n::text("snapshot.timeline.export.filter"));
        chooser.setAcceptMode(QFileDialog::AcceptSave);
        chooser.setDefaultSuffix(QStringLiteral("png"));
        chooser.selectFile(QStringLiteral("Snapshot.png"));
        if (chooser.exec() != QDialog::Accepted || chooser.selectedFiles().isEmpty()) return;
        path = chooser.selectedFiles().front();
    }
    if (path.isEmpty()) return;
    if (!path.endsWith(".png", Qt::CaseInsensitive)) {
        path += QStringLiteral(".png");
        if (QFileInfo::exists(path)) {
            const bool replace = fileInteractions_.confirmReplace ? fileInteractions_.confirmReplace(path)
                : QMessageBox::question(this, i18n::text("snapshot.timeline.export.title"),
                    i18n::text("main.window.files.replace.1").arg(QFileInfo(path).fileName()),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
            if (!replace) return;
        }
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(reinterpret_cast<const char*>(image->png.data()), qint64(image->png.size())) != qint64(image->png.size())
        || !output.commit()) throw std::runtime_error(output.errorString().toStdString());
    rememberExportDirectory(path);
    statusBar()->showMessage(i18n::text("snapshot.timeline.export.done"), 4000);
} catch (const std::exception& error) {
    reportFileError(i18n::text("snapshot.timeline.failed").arg(QString::fromUtf8(error.what())));
}
} // namespace imageeditor::ui
