#include "imageeditor/ui/GifExportDialog.hpp"
#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/SnapshotPlayback.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include <QCache>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <future>

namespace imageeditor::ui
{
namespace
{
class GifPreviewSurface final : public QWidget
{
  public:
    explicit GifPreviewSurface(QWidget *parent) : QWidget(parent)
    {
        setObjectName("GifExportPreview");
        setMinimumSize(240, 200);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QImage image;
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeColor(ThemeColor::Canvas));
        if (image.isNull())
            return;
        const auto size = image.size().scaled(this->size(), Qt::KeepAspectRatio);
        QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
        p.save();
        p.setClipRect(target);
        for (int y = target.top(); y <= target.bottom(); y += 10)
            for (int x = target.left(); x <= target.right(); x += 10)
                p.fillRect(QRect(x, y, 10, 10),
                           themeColor(((x - target.left()) / 10 + (y - target.top()) / 10) % 2
                                          ? ThemeColor::CheckerDark
                                          : ThemeColor::CheckerLight));
        p.setRenderHint(QPainter::SmoothPixmapTransform, target.width() < image.width());
        p.drawImage(target, image);
        p.restore();
    }
};
} // namespace
struct GifExportDialog::Impl {
    GifExportDialog &host;
    core::SnapshotTimelineState source;
    GifPreviewSurface *surface{};
    QWidget *controls{};
    CompactValueControl *colors{}, *strength{};
    QSpinBox *width{}, *height{};
    QComboBox *dither{};
    QCheckBox *transparency{}, *loop{}, *aspect{};
    QPushButton *matte{}, *exportButton{}, *cancelButton{};
    QToolButton *play{};
    QSlider *position{};
    QLabel *status{}, *frameLabel{};
    QProgressBar *progressBar{};
    QColor background{Qt::white};
    QSize original;
    SnapshotPlayback playback;
    QTimer debounce, poll;
    std::future<EncodedGif> encoding;
    std::future<ExportWriteResult> writing;
    std::shared_ptr<std::atomic_bool> cancel;
    std::shared_ptr<std::atomic_int> progress;
    GifExportSettings activeSettings;
    EncodedGif ready;
    QCache<int, QImage> cache{16 * 1024}; // KiB; full-size frames are never all retained.
    QString destination;
    bool closing{}, wantPlaying{true}, requestedPlay{}, resumeAfterSeek{};

    Impl(GifExportDialog &owner, core::SnapshotTimelineState timeline)
        : host(owner), source(std::move(timeline)), playback(&owner)
    {
        host.setObjectName("GifExportDialog");
        host.setAttribute(Qt::WA_StyledBackground);
        host.setSizeGripEnabled(false);
        host.setProperty("workspacePreferredSize", QSize(900, 620));
        host.setMinimumSize(800, 540);
        host.resize(900, 620);
        auto *root = new QVBoxLayout(&host);
        auto *title = i18n::make<QLabel>(i18n::text("gif.export.title"), &host);
        auto font = title->font();
        font.setBold(true);
        title->setFont(font);
        root->addWidget(title);
        auto *body = new QHBoxLayout;
        auto *preview = new QVBoxLayout;
        surface = new GifPreviewSurface(&host);
        preview->addWidget(surface, 1);
        i18n::setToolTip(
            surface, i18n::text("export.dialog.decoded.from.the.encoded.file.checkerboard.is.preview.only"));
        auto *transport = new QHBoxLayout;
        play = new QToolButton(&host);
        play->setObjectName("GifExportPlay");
        play->setCheckable(true);
        play->setIcon(toolGlyph(ToolGlyph::Play));
        i18n::setToolTip(play, i18n::text("snapshot.timeline.play"));
        transport->addWidget(play);
        position = new QSlider(Qt::Horizontal, &host);
        position->setObjectName("GifExportPosition");
        transport->addWidget(position, 1);
        frameLabel = new QLabel(&host);
        transport->addWidget(frameLabel);
        preview->addLayout(transport);
        body->addLayout(preview, 1);
        controls = new QWidget(&host);
        controls->setFixedWidth(275);
        auto *form = new QFormLayout(controls);
        form->setContentsMargins(10, 0, 0, 0);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        const auto spin = [&](const char *name, int min, int max, int value, const char *label) {
            auto *input = new QSpinBox(controls);
            input->setObjectName(name);
            input->setRange(min, max);
            input->setValue(value);
            input->setKeyboardTracking(false);
            form->addRow(i18n::make<QLabel>(i18n::text(label), controls), input);
            return input;
        };
        const auto slider = [&](const char *name, int min, int max, int value, const char *label) {
            auto *input = new CompactValueControl(controls);
            input->setObjectName(name);
            input->setDecimals(0);
            input->setRange(min, max);
            input->setSingleStep(1);
            input->setValue(value);
            input->setFixedHeight(30);
            input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            i18n::setAccessibleName(input, i18n::text(label));
            auto *caption = i18n::make<QLabel>(i18n::text(label), controls);
            caption->setBuddy(input);
            form->addRow(caption, input);
            return input;
        };
        colors = slider("GifExportColors", 2, 256, 256, "gif.export.colors");
        dither = new QComboBox(controls);
        dither->setObjectName("GifExportDither");
        for (const auto *key :
             {"gif.export.none", "gif.export.diffusion", "gif.export.pattern", "gif.export.noise"})
            i18n::addItem(dither, i18n::text(key));
        dither->setCurrentIndex(1);
        form->addRow(i18n::make<QLabel>(i18n::text("gif.export.dither"), controls), dither);
        strength = slider("GifExportStrength", 0, 100, 100, "gif.export.strength");
        strength->setSuffix("%");
        transparency = i18n::make<QCheckBox>(i18n::text("gif.export.transparency"), controls);
        transparency->setObjectName("GifExportTransparency");
        transparency->setChecked(true);
        form->addRow(transparency);
        matte = i18n::make<QPushButton>(i18n::text("gif.export.background"), controls);
        matte->setObjectName("GifExportBackground");
        matte->setAutoDefault(false);
        form->addRow(matte);
        for (const auto &frame : source.frames)
            if (frame.image) {
                original = {int(frame.image->extent.width), int(frame.image->extent.height)};
                break;
            }
        width = spin("GifExportWidth", 1, 32768, std::max(1, original.width()), "export.dialog.width");
        height = spin("GifExportHeight", 1, 32768, std::max(1, original.height()), "export.dialog.height");
        width->setSuffix(" px");
        height->setSuffix(" px");
        aspect = i18n::make<QCheckBox>(i18n::text("export.dialog.lock.aspect.ratio"), controls);
        aspect->setChecked(true);
        form->addRow(aspect);
        loop = i18n::make<QCheckBox>(i18n::text("snapshot.timeline.loop"), controls);
        loop->setObjectName("GifExportLoop");
        loop->setChecked(true);
        form->addRow(loop);
        body->addWidget(controls, 0, Qt::AlignTop);
        root->addLayout(body, 1);
        progressBar = new QProgressBar(&host);
        progressBar->setRange(0, 100);
        progressBar->setFixedHeight(5);
        progressBar->setTextVisible(false);
        root->addWidget(progressBar);
        auto *footer = new QHBoxLayout;
        status = new QLabel(&host);
        status->setWordWrap(true);
        footer->addWidget(status, 1);
        cancelButton = i18n::make<QPushButton>(i18n::text("export.dialog.cancel"), &host);
        cancelButton->setAutoDefault(false);
        footer->addWidget(cancelButton);
        exportButton = i18n::make<QPushButton>(i18n::text("export.dialog.export"), &host);
        exportButton->setObjectName("GifExportWrite");
        exportButton->setAutoDefault(false);
        exportButton->setIcon(toolGlyph(ToolGlyph::Export));
        footer->addWidget(exportButton);
        root->addLayout(footer);
        debounce.setSingleShot(true);
        debounce.setInterval(150);
        poll.setInterval(16);
        QObject::connect(&debounce, &QTimer::timeout, &host, [this] { start(); });
        QObject::connect(&poll, &QTimer::timeout, &host, [this] { checkWork(); });
        const auto changed = [this] { request(); };
        for (auto *input : {colors, strength})
            QObject::connect(input, &QDoubleSpinBox::valueChanged, &host, changed);
        QObject::connect(dither, &QComboBox::currentIndexChanged, &host, changed);
        QObject::connect(transparency, &QCheckBox::toggled, &host, changed);
        QObject::connect(loop, &QCheckBox::toggled, &host, changed);
        QObject::connect(width, &QSpinBox::valueChanged, &host, [this](int value) {
            if (aspect->isChecked() && original.width() > 0) {
                QSignalBlocker block(height);
                height->setValue(qRound(double(value) * original.height() / original.width()));
            }
            request();
        });
        QObject::connect(height, &QSpinBox::valueChanged, &host, [this](int value) {
            if (aspect->isChecked() && original.height() > 0) {
                QSignalBlocker block(width);
                width->setValue(qRound(double(value) * original.width() / original.height()));
            }
            request();
        });
        QObject::connect(matte, &QPushButton::clicked, &host, [this] {
            const auto color = ColorDialog::getColor(background, &host, i18n::text("gif.export.background"));
            if (color.isValid()) {
                background = color;
                request();
            }
        });
        QObject::connect(cancelButton, &QPushButton::clicked, &host, [this] { host.reject(); });
        QObject::connect(exportButton, &QPushButton::clicked, &host, [this] {
            if (ready && host.onExportRequested)
                host.onExportRequested();
        });
        QObject::connect(play, &QToolButton::pressed, &host, [this] { requestedPlay = !play->isChecked(); });
        QObject::connect(play, &QToolButton::clicked, &host, [this] {
            wantPlaying = requestedPlay;
            if (requestedPlay)
                playback.play(core::SnapshotFrameId(position->value() + 1));
            else
                playback.pause();
            if (playback.onPlayingChanged)
                playback.onPlayingChanged(playback.playing());
        });
        QObject::connect(position, &QSlider::valueChanged, &host,
                         [this](int value) { playback.seek(core::SnapshotFrameId(value + 1)); });
        QObject::connect(position, &QSlider::sliderPressed, &host, [this] {
            resumeAfterSeek = playback.playing();
            playback.pause();
        });
        QObject::connect(position, &QSlider::sliderReleased, &host, [this] {
            if (resumeAfterSeek)
                playback.play(core::SnapshotFrameId(position->value() + 1));
        });
        playback.onPlayingChanged = [this](bool playing) {
            QSignalBlocker block(play);
            play->setChecked(playing);
            play->setIcon(toolGlyph(playing ? ToolGlyph::Pause : ToolGlyph::Play));
            i18n::setToolTip(play,
                             i18n::text(playing ? "snapshot.timeline.pause" : "snapshot.timeline.play"));
        };
        playback.onFrame = [this](auto id) {
            const auto index = int(id - 1);
            QSignalBlocker block(position);
            position->setValue(index);
            auto *cached = cache.object(index);
            if (!cached) {
                auto decoded = decodeGifPreview(ready, std::size_t(index), QSize(1000, 800));
                if (decoded.isNull()) {
                    playback.pause();
                    ready.error = QStringLiteral("Cannot decode the GIF preview.");
                    exportButton->setEnabled(false);
                    play->setEnabled(false);
                    position->setEnabled(false);
                    i18n::setText(status, i18n::text("snapshot.timeline.unavailable"));
                    return;
                }
                surface->image = decoded;
                cache.insert(index, new QImage(decoded), std::max(1, int(decoded.sizeInBytes() / 1024)));
            } else
                surface->image = *cached;
            surface->update();
            i18n::setText(frameLabel, i18n::text("snapshot.timeline.frame").arg(index));
        };
        request();
    }
    ~Impl()
    {
        if (cancel)
            cancel->store(true);
    }
    GifExportSettings settings() const
    {
        return {{width->value(), height->value()},
                qRound(colors->value()),
                GifDither(dither->currentIndex()),
                qRound(strength->value()),
                transparency->isChecked(),
                background,
                loop->isChecked()};
    }
    void request()
    {
        if (closing || writing.valid())
            return;
        strength->setEnabled(dither->currentIndex() == int(GifDither::Diffusion));
        matte->setEnabled(!transparency->isChecked());
        QPixmap chip(20, 20);
        chip.fill(background);
        matte->setIcon(QIcon(chip));
        playback.clear();
        ready = {};
        cache.clear();
        surface->image = {};
        surface->update();
        exportButton->setEnabled(false);
        play->setEnabled(false);
        position->setEnabled(false);
        i18n::setText(status, i18n::text("export.dialog.preparing.preview"));
        progressBar->setRange(0, 100);
        progressBar->show();
        progressBar->setValue(0);
        if (cancel)
            cancel->store(true);
        debounce.start();
    }
    void start()
    {
        if (closing || encoding.valid() || writing.valid())
            return;
        activeSettings = settings();
        cancel = std::make_shared<std::atomic_bool>(false);
        progress = std::make_shared<std::atomic_int>(0);
        const auto token = cancel;
        const auto amount = progress;
        const auto options = activeSettings;
        const auto frames = source;
        try {
            encoding = std::async(std::launch::async, [frames, options, token, amount] {
                return encodeGif(frames, options, *token, [amount](int p) { amount->store(p); });
            });
            poll.start();
        } catch (const std::exception &error) {
            progressBar->hide();
            status->setText(QString::fromUtf8(error.what()));
        }
    }
    void checkWork()
    {
        if (encoding.valid()) {
            progressBar->setValue(progress->load());
            if (encoding.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            auto result = encoding.get();
            poll.stop();
            if (closing) {
                host.done(QDialog::Rejected);
                return;
            }
            if (result.cancelled || activeSettings != settings()) {
                if (!debounce.isActive())
                    start();
                return;
            }
            progressBar->hide();
            if (!result) {
                status->setText(result.error);
                return;
            }
            ready = std::move(result);
            exportButton->setEnabled(true);
            position->setEnabled(true);
            play->setEnabled(ready.frames.size() > 1);
            {
                QSignalBlocker block(position);
                position->setRange(0, int(ready.frames.size()) - 1);
                position->setValue(0);
            }
            core::SnapshotTimelineState state;
            state.frames.clear();
            state.nextId = 1;
            for (const auto &frame : source.frames)
                if (frame.image) {
                    const auto index = state.frames.size();
                    state.frames.push_back({state.nextId++, ready.frames[index].durationMs, frame.image});
                }
            playback.setTimeline(state);
            playback.setLooping(settings().loop);
            playback.seek(1);
            if (!ready)
                return;
            if (wantPlaying && ready.frames.size() > 1)
                playback.play(1);
            i18n::setText(status, i18n::text("gif.export.ready")
                                      .arg(ready.frames.size())
                                      .arg((ready.bytes.size() + 1023) / 1024));
        } else if (writing.valid()) {
            if (writing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            const auto result = writing.get();
            poll.stop();
            progressBar->hide();
            if (result) {
                if (host.onExported)
                    host.onExported(destination);
                host.done(QDialog::Accepted);
                return;
            }
            if (closing) {
                host.done(QDialog::Rejected);
                return;
            }
            controls->setEnabled(true);
            cancelButton->setEnabled(true);
            exportButton->setEnabled(true);
            play->setEnabled(ready.frames.size() > 1);
            position->setEnabled(true);
            status->setText(result.error);
        }
    }
};
GifExportDialog::GifExportDialog(core::SnapshotTimelineState source, QWidget *parent)
    : QDialog(parent, Qt::SubWindow | Qt::FramelessWindowHint),
      impl_(std::make_unique<Impl>(*this, std::move(source)))
{
}
GifExportDialog::~GifExportDialog() = default;
GifExportSettings GifExportDialog::settings() const
{
    return impl_->settings();
}
bool GifExportDialog::previewReady() const
{
    return bool(impl_->ready) && !impl_->encoding.valid() && !impl_->writing.valid();
}
void GifExportDialog::writeTo(const QString &path)
{
    auto &s = *impl_;
    if (!previewReady() || s.closing)
        return;
    s.playback.pause();
    s.destination = path;
    s.controls->setEnabled(false);
    s.exportButton->setEnabled(false);
    s.play->setEnabled(false);
    s.position->setEnabled(false);
    s.cancel = std::make_shared<std::atomic_bool>(false);
    const auto token = s.cancel;
    const auto bytes = s.ready.bytes;
    i18n::setText(s.status, i18n::text("main.window.export.writing"));
    s.progressBar->setRange(0, 0);
    s.progressBar->show();
    try {
        s.writing = std::async(std::launch::async,
                               [path, bytes, token] { return writeExportAtomically(path, bytes, *token); });
        s.poll.start();
    } catch (const std::exception &error) {
        s.progressBar->hide();
        s.controls->setEnabled(true);
        s.exportButton->setEnabled(true);
        s.status->setText(QString::fromUtf8(error.what()));
    }
}
void GifExportDialog::reject()
{
    auto &s = *impl_;
    s.closing = true;
    s.debounce.stop();
    s.playback.clear();
    if (s.cancel)
        s.cancel->store(true);
    if (!s.encoding.valid() && !s.writing.valid()) {
        done(QDialog::Rejected);
        return;
    }
    s.controls->setEnabled(false);
    s.exportButton->setEnabled(false);
    s.cancelButton->setEnabled(false);
    i18n::setText(s.status, i18n::text("main.window.export.cancelling.encoding"));
    s.poll.start();
}
} // namespace imageeditor::ui
