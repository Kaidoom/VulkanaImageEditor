#include "LayerCleanupPreview.hpp"
#include "LayerCleanupPixels.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/core/ViewportState.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace imageeditor::ui {
namespace {
constexpr QRgb clearColor = qRgb(255, 255, 255), keepColor = qRgb(88, 88, 88), emptyColor = qRgb(0, 0, 0);
using Counts = std::array<quint64, 256>;

class HoldOriginalButton final : public QPushButton {
public:
    using QPushButton::QPushButton;

protected:
    bool event(QEvent* event) override
    {
        const auto type = event->type();
        if (isDown() && (type == QEvent::UngrabMouse || type == QEvent::WindowDeactivate
                        || type == QEvent::Hide || type == QEvent::FocusOut)) {
            setDown(false);
            emit released();
        }
        return QPushButton::event(event);
    }
};

void includeAlpha(uchar& cell, uchar alpha)
{
    if (alpha && (!cell || alpha < cell)) cell = alpha;
}

QImage maskImage(QSize size, int step)
{
    QImage mask((size.width() + step - 1) / step, (size.height() + step - 1) / step, QImage::Format_Indexed8);
    if (mask.isNull()) throw std::bad_alloc();
    mask.fill(0);
    return mask;
}

// Grid-aligned sampling keeps a single cleanup target visible in an overview
// cell. At level zero each cell contains the exact source pixel's alpha.
struct MaskScan {
    QRect region;
    int level;
    QImage mask;
    int nextRow{};
    int bandHeight;
    std::vector<core::Rgba8> band;

    MaskScan(QRect area, int lod)
        : region(area), level(lod), mask(maskImage(area.size(), 1 << lod)),
          bandHeight(std::clamp(32768 / area.width(), 1, area.height())),
          band(size_t(area.width()) * size_t(bandHeight)) {}

    bool advance(const core::RasterSurface& source, Counts* counts)
    {
        QElapsedTimer slice;
        slice.start();
        do {
            const int rows = std::min(bandHeight, region.height() - nextRow);
            const int w = region.width();
            source.copyRgba8({region.x(), region.y()+nextRow, w, rows}, std::as_writable_bytes(std::span(band)), size_t(w)*4);
            for (int y = 0; y < rows; ++y) {
                auto* cells = mask.scanLine((nextRow+y) >> level);
                const auto* pixels = band.data() + size_t(y)*size_t(w);
                for (int x = 0; x < w; ++x) {
                    const auto alpha = pixels[x].alpha;
                    if (counts) ++(*counts)[alpha];
                    includeAlpha(cells[x >> level], alpha);
                }
            }
            nextRow += rows;
        } while (nextRow < region.height() && slice.elapsed() < 4);
        return nextRow == region.height();
    }
};

QLabel* legendItem(QHBoxLayout& row, QRgb color, QLabel** colorChip = nullptr)
{
    auto* chip = new QLabel;
    QPixmap swatch(12, 12);
    swatch.fill(QColor::fromRgb(color));
    {
        QPainter painter(&swatch);
        painter.setPen(QColor(115, 115, 115));
        painter.drawRect(0, 0, 11, 11);
    }
    chip->setPixmap(swatch);
    if (colorChip) *colorChip = chip;
    row.addWidget(chip);
    auto* label = new QLabel;
    row.addWidget(label);
    return label;
}
} // namespace

class CleanupPreviewViewport final : public QWidget {
public:
    CleanupPreviewViewport(QLabel* clearCount, QLabel* zoomLabel, QWidget* parent)
        : QWidget(parent), clearCount_(clearCount), zoomLabel_(zoomLabel)
    {
        setObjectName("CleanupTargetPreview");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(180, 90);
        setCursor(Qt::OpenHandCursor);
        timer_.setSingleShot(true);
        connect(&timer_, &QTimer::timeout, this, &CleanupPreviewViewport::advance);
        pixelTimer_.setSingleShot(true);
        connect(&pixelTimer_, &QTimer::timeout, this, &CleanupPreviewViewport::advancePixels);
        QPixmap tile(20,20);
        tile.fill(themeColor(ThemeColor::CheckerLight));
        QPainter checker(&tile);
        checker.fillRect(0,0,10,10,themeColor(ThemeColor::CheckerDark));
        checker.fillRect(10,10,10,10,themeColor(ThemeColor::CheckerDark));
        checker.end();
        checker_ = QBrush(tile);
        setThreshold(core::defaultCleanupAlpha);
    }
    QSize sizeHint() const override { return {704, 370}; }

    void setSource(std::shared_ptr<const core::RasterSurface> source)
    try {
        timer_.stop();
        job_.reset();
        overviews_.clear();
        detail_ = {};
        detailRegion_ = {};
        detailLevel_ = -1;
        endPan();
        pixelTimer_.stop();
        pixelJob_.reset();
        wholePixels_.reset();
        windowPixels_.reset();
        pixelError_ = false;
        source_ = std::move(source);
        revision_ = source_->revision();
        const auto extent = source_->extent();
        native_ = {int(extent.width), int(extent.height)};
        counts_.fill(0);
        baseLevel_ = 0;
        if (native_.isEmpty()) { unavailable(); return; }
        // Keep at most one million whole-layer cells. Finer levels are read
        // only for the visible window, including when the layer is off-canvas.
        QSize size = native_;
        while (qint64(size.width()) * size.height() > 1024*1024) {
            size = {(size.width()+1)/2, (size.height()+1)/2};
            ++baseLevel_;
        }
        job_.emplace(QRect(QPoint{}, native_), baseLevel_);
        i18n::setText(clearCount_, i18n::text("layers.cleanup.preview.clear").arg(QStringLiteral("…")));
        fit();
        advance();
    } catch (const std::exception&) { unavailable(); }

    void setThreshold(int value)
    {
        threshold_ = value;
        colors_.fill(keepColor, 256);
        colors_[0] = emptyColor;
        for (int alpha = 1; alpha <= 255; ++alpha)
            if (core::cleanupTargetsAlpha(std::uint8_t(alpha), std::uint8_t(value))) colors_[alpha] = clearColor;
        for (auto& image : overviews_) image.setColorTable(colors_);
        if (!detail_.isNull()) detail_.setColorTable(colors_);
        if (!overviews_.empty()) updateCount();
        viewChanged();
    }
    void showMask(bool enabled) { maskMode_ = enabled; viewChanged(); }
    void holdOriginal(bool held) { originalHeld_ = held; viewChanged(); }

    void fit()
    {
        fitted_ = true;
        view_.fit(layerExtent(), viewportExtent(), 1);
        viewChanged();
    }
    void zoomBy(double factor, QPointF anchor)
    {
        fitted_ = false;
        view_.zoomAround({anchor.x(),anchor.y()}, factor, layerExtent(), viewportExtent());
        viewChanged();
    }
    void zoomBy(double factor) { zoomBy(factor, QRectF(rect()).center()); }
    void actualSize() { zoomBy(1 / view_.zoom()); }

protected:
    void resizeEvent(QResizeEvent*) override { if (fitted_) fit(); else viewChanged(); }
    void wheelEvent(QWheelEvent* e) override
    {
        const auto delta = e->angleDelta().y() ? e->angleDelta().y() : e->pixelDelta().y();
        if (delta && drag_ == Qt::NoButton) zoomBy(std::pow(1.16, double(delta)/120), e->position());
        e->accept();
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (drag_ == Qt::NoButton && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)) {
            drag_ = e->button();
            last_ = e->position();
            setCursor(Qt::ClosedHandCursor);
        }
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (drag_ != Qt::NoButton) {
            if (e->buttons().testFlag(drag_)) panTo(e->position());
            else endPan();
        }
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == drag_) { panTo(e->position()); endPan(); }
        e->accept();
    }
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::UngrabMouse || e->type() == QEvent::WindowDeactivate || e->type() == QEvent::Hide)
            endPan();
        return QWidget::event(e);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        p.setPen(palette().mid().color());
        p.drawRect(rect().adjusted(0,0,-1,-1));
        p.setClipRect(rect().adjusted(1,1,-1,-1));
        const auto origin = view_.documentToViewport({}, layerExtent(), viewportExtent());
        const double zoom = view_.zoom();
        const QRectF visible = QRectF(-origin.x/zoom, -origin.y/zoom, width()/zoom, height()/zoom)
            .intersected(QRectF(QPointF{}, native_));
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // Clip in source space before mapping to widget coordinates. Even at
        // high magnification, the painter only sees a viewport-sized rectangle.
        const bool pixels = !maskMode_ || originalHeld_;
        const auto draw = [&](const QImage& image, QPoint start, int level) {
            const int step = 1 << level;
            const auto part = visible.intersected(QRectF(start, QSizeF(image.size()) * step));
            if (part.isEmpty()) return false;
            const QRectF target(origin.x + zoom*part.x(), origin.y + zoom*part.y(),
                part.width()*zoom, part.height()*zoom);
            if (pixels) {
                p.fillRect(target, checker_);
                p.setRenderHint(QPainter::SmoothPixmapTransform, step*zoom < 1.0);
            }
            p.drawImage(target, image, QRectF((part.x()-start.x())/step, (part.y()-start.y())/step,
                part.width()/step, part.height()/step));
            return true;
        };
        if (pixels) {
            bool drawn = false;
            // Repaint the checker under each patch so transparent detail does
            // not reveal a coarser rendition of the same layer underneath.
            const QRect needed = visible.toAlignedRect();
            for (const auto* patch : {&wholePixels_, &windowPixels_}) {
                if (!*patch) continue;
                const auto& pair = **patch;
                if (!originalHeld_ && threshold_ != 0 && pair.threshold != threshold_) continue;
                drawn |= draw(originalHeld_ || threshold_ == 0 ? pair.original : pair.cleaned,
                    pair.region.topLeft(), pair.level);
                if (pair.covers(needed, pixelLevel())) break;
            }
            if (!drawn && !visible.isEmpty()) drawStatus(p, !pixelError_);
            return;
        }
        if (overviews_.empty()) { drawStatus(p, job_.has_value()); return; }
        const int index = std::clamp(level_ - baseLevel_, 0, int(overviews_.size())-1);
        draw(overviews_[size_t(index)], {}, index + baseLevel_);
        if (level_ < baseLevel_ && !detail_.isNull() && detailLevel_ == level_)
            draw(detail_, detailRegion_.topLeft(), detailLevel_);
    }

private:
    void drawStatus(QPainter& p, bool preparing)
    {
        p.setPen(palette().text().color());
        p.drawText(rect(), Qt::AlignCenter, i18n::text(preparing ? "navigator.preparing" : "navigator.unavailable"));
    }
    core::Extent2d layerExtent() const { return {double(native_.width()), double(native_.height())}; }
    core::Extent2d viewportExtent() const { return {double(width()), double(height())}; }
    void endPan() { drag_ = Qt::NoButton; setCursor(Qt::OpenHandCursor); }
    void panTo(QPointF pos)
    {
        const auto delta = pos - last_;
        if (delta.isNull()) return;
        last_ = pos;
        fitted_ = false;
        view_.panBy({delta.x(),delta.y()});
        viewChanged();
    }
    void viewChanged()
    try {
        zoomLabel_->setText(QString::number(view_.zoom()*100, 'f', view_.zoom() < .1 ? 1 : 0) + '%');
        update();
        if (!source_ || native_.isEmpty()) return;
        level_ = std::max(0, int(std::ceil(std::log2(1 / view_.zoom()) - 1e-9)));
        const auto top = view_.viewportToDocument({}, layerExtent(), viewportExtent());
        const QRect bounds(QPoint{}, native_);
        // Intersect in floating point first: the view can pan beyond the layer.
        const QRect visible = QRectF(top.x,top.y,width()/view_.zoom(),height()/view_.zoom())
            .intersected(QRectF(bounds)).toAlignedRect().intersected(bounds);
        requestPixels(visible);
        if (overviews_.empty()) return;
        if (level_ >= baseLevel_ || visible.isEmpty() || (detailLevel_ == level_ && detailRegion_.contains(visible))) {
            job_.reset();
            timer_.stop();
            return;
        }
        if (job_ && job_->level == level_ && job_->region.contains(visible)) return;
        job_.emplace(inspectedArea(visible, level_), level_);
        timer_.start(0);
    } catch (const std::exception&) { unavailable(); }

    QRect inspectedArea(QRect visible, int level) const
    {
        // Both samples of every comparison use the same source-aligned cells,
        // including at patch edges when panning.
        const int margin = int(std::ceil(32 / view_.zoom()));
        const int step = 1 << level;
        const auto r = visible.adjusted(-margin,-margin,margin,margin).intersected(QRect(QPoint{}, native_));
        const int x = r.x()/step*step, y = r.y()/step*step;
        const int right = std::min(native_.width(), ((r.right()+step)/step)*step);
        const int bottom = std::min(native_.height(), ((r.bottom()+step)/step)*step);
        return {x,y,right-x,bottom-y};
    }

    int pixelLevel() const { return std::max(0, int(std::floor(std::log2(1 / view_.zoom()) + 1e-9))); }

    void requestPixels(QRect visible)
    try {
        if ((maskMode_ && !originalHeld_) || visible.isEmpty()) {
            pixelJob_.reset(); pixelTimer_.stop(); return;
        }
        const int lod = pixelLevel();
        for (auto* entry : {&windowPixels_, &wholePixels_}) {
            if (!*entry || !(**entry).covers(visible, lod)) continue;
            auto& pair = **entry;
            if (lod == 0) pair.rethresholdNative(threshold_);
            if (pair.threshold == threshold_ || threshold_ == 0) {
                pixelJob_.reset(); pixelTimer_.stop(); return;
            }
        }
        if (pixelJob_ && pixelJob_->patch.covers(visible, lod) && pixelJob_->patch.threshold == threshold_) return;
        pixelError_ = false;
        pixelJob_.emplace(inspectedArea(visible,lod), lod, threshold_);
        pixelTimer_.start(0);
    } catch (const std::exception&) { pixelsUnavailable(); }

    void advancePixels()
    try {
        if (!pixelJob_ || source_->revision() != revision_) { pixelsUnavailable(); return; }
        if (!pixelJob_->advance(*source_)) { pixelTimer_.start(1); return; }
        auto& patch = pixelJob_->patch;
        if (patch.region == QRect(QPoint{},native_)) {
            wholePixels_ = std::move(patch);
            windowPixels_.reset();
        } else windowPixels_ = std::move(patch);
        pixelJob_.reset();
        update();
    } catch (const std::exception&) { pixelsUnavailable(); }

    void pixelsUnavailable()
    {
        pixelTimer_.stop(); pixelJob_.reset();
        wholePixels_.reset(); windowPixels_.reset();
        pixelError_ = true; update();
    }

    void advance()
    try {
        if (!job_ || source_->revision() != revision_) { unavailable(); return; }
        const bool overview = overviews_.empty();
        if (!job_->advance(*source_, overview ? &counts_ : nullptr)) { timer_.start(1); return; }
        job_->mask.setColorTable(colors_);
        if (overview) {
            overviews_.push_back(std::move(job_->mask));
            // At smaller zooms each overview cell still shows the faintest
            // nontransparent pixel it covers. No isolated target is sampled away.
            while (overviews_.back().width() > 1 || overviews_.back().height() > 1) {
                const auto& input = overviews_.back();
                auto small = maskImage(input.size(), 2);
                for (int y = 0; y < input.height(); ++y) {
                    const auto* in = input.constScanLine(y);
                    auto* out = small.scanLine(y/2);
                    for (int x = 0; x < input.width(); ++x) includeAlpha(out[x/2], in[x]);
                }
                small.setColorTable(colors_);
                overviews_.push_back(std::move(small));
            }
            updateCount();
        } else {
            detail_ = std::move(job_->mask);
            detailRegion_ = job_->region;
            detailLevel_ = job_->level;
        }
        job_.reset();
        viewChanged();
    } catch (const std::exception&) { unavailable(); }

    void updateCount()
    {
        const auto n = std::accumulate(counts_.begin()+1, counts_.begin()+threshold_+1, quint64{0});
        i18n::setText(clearCount_, i18n::text("layers.cleanup.preview.clear").arg(locale().toString(n)));
    }
    void unavailable()
    {
        timer_.stop();
        job_.reset();
        overviews_.clear();
        detail_ = {};
        i18n::setText(clearCount_, i18n::text("layers.cleanup.preview.clear").arg(QStringLiteral("—")));
        update();
    }

    QLabel *clearCount_, *zoomLabel_;
    QTimer timer_, pixelTimer_;
    std::optional<CleanupPixelScan> pixelJob_;
    std::optional<CleanupPixelPatch> wholePixels_, windowPixels_;
    QBrush checker_;
    bool maskMode_{}, originalHeld_{}, pixelError_{};
    core::ViewportState view_;
    std::shared_ptr<const core::RasterSurface> source_;
    core::Revision revision_{};
    QSize native_;
    Counts counts_{};
    QList<QRgb> colors_;
    std::vector<QImage> overviews_;
    std::optional<MaskScan> job_;
    QImage detail_;
    QRect detailRegion_;
    int threshold_{}, baseLevel_{}, level_{}, detailLevel_{-1};
    bool fitted_{true};
    Qt::MouseButton drag_{Qt::NoButton};
    QPointF last_;
};

LayerCleanupPreview::LayerCleanupPreview(const core::Document& doc, const core::LayerSelectionState& selected, QWidget* parent)
    : QWidget(parent), threshold_(new CompactValueControl(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const auto ids = doc.expandedLayers(selected.ids);
    QComboBox* picker = nullptr;
    if (ids.size() > 1) {
        auto* row = new QHBoxLayout;
        auto* label = i18n::make<QLabel>(i18n::text("layers.cleanup.preview.layer"), this);
        picker = new QComboBox(this);
        picker->setObjectName("CleanupPreviewLayer");
        picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        label->setBuddy(picker);
        i18n::setToolTip(picker, i18n::text("layers.cleanup.preview.all"));
        row->addWidget(label);
        row->addWidget(picker, 1);
        layout->addLayout(row);
    }

    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    auto* mask = i18n::make<QCheckBox>(i18n::text("layers.cleanup.preview.mask"), this);
    mask->setObjectName("CleanupShowMask");
    auto* compare = i18n::make<HoldOriginalButton>(i18n::text("main.window.refinement.hold.for.original"), this);
    compare->setObjectName("CleanupHoldOriginal");
    compare->setAutoDefault(false);
    bar->addWidget(mask);
    bar->addSpacing(6);
    QLabel* clearChip{};
    auto* count = legendItem(*bar, clearColor, &clearChip);
    count->setObjectName("CleanupPreviewCount");
    auto* maskKey = new QWidget(this);
    auto* keyRow = new QHBoxLayout(maskKey);
    keyRow->setContentsMargins(6,0,0,0);
    keyRow->setSpacing(4);
    i18n::setText(legendItem(*keyRow, keepColor), i18n::text("layers.cleanup.preview.keep"));
    keyRow->addSpacing(6);
    i18n::setText(legendItem(*keyRow, emptyColor), i18n::text("new.document.dialog.transparent"));
    bar->addWidget(maskKey);
    clearChip->hide();
    maskKey->hide();
    bar->addStretch();
    auto* zoomLabel = new QLabel(this);
    zoomLabel->setObjectName("CleanupPreviewZoom");
    zoomLabel->setAlignment(Qt::AlignCenter);
    zoomLabel->setMinimumWidth(zoomLabel->fontMetrics().horizontalAdvance("6400%") + 4);
    i18n::setToolTip(zoomLabel, i18n::text("layers.cleanup.preview.navigation"));
    auto* preview = new CleanupPreviewViewport(count, zoomLabel, this);
    const auto updateLegend = [=] {
        const bool showKey = mask->isChecked() && !compare->isDown();
        clearChip->setVisible(showKey);
        maskKey->setVisible(showKey);
    };
    connect(mask, &QCheckBox::toggled, this, [preview, updateLegend](bool enabled) {
        preview->showMask(enabled);
        updateLegend();
    });
    connect(compare, &QPushButton::pressed, this, [preview, updateLegend] {
        preview->holdOriginal(true);
        updateLegend();
    });
    connect(compare, &QPushButton::released, this, [preview, updateLegend] {
        preview->holdOriginal(false);
        updateLegend();
    });
    layout->addWidget(preview, 1);
    layout->addLayout(bar);
    const auto button = [&](const char* id, const i18n::Message& hint) {
        auto* b = new QToolButton(this);
        b->setObjectName(id);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        i18n::setAccessibleName(b, hint);
        i18n::setToolTip(b, hint);
        bar->addWidget(b);
        return b;
    };
    auto* minus = button("CleanupZoomOut", i18n::text("layers.cleanup.preview.zoom.out"));
    minus->setText(QStringLiteral("−"));
    bar->addWidget(zoomLabel);
    auto* plus = button("CleanupZoomIn", i18n::text("layers.cleanup.preview.zoom.in"));
    plus->setText(QStringLiteral("+"));
    auto* actual = button("CleanupZoomActual", i18n::text("diffusion.ui.actual.pixels.1.1"));
    actual->setIcon(toolGlyph(ToolGlyph::ZoomActual));
    auto* fit = button("CleanupZoomFit", i18n::text("diffusion.ui.fit.to.view"));
    fit->setIcon(toolGlyph(ToolGlyph::FitCanvas));
    connect(minus, &QToolButton::clicked, this, [preview]{ preview->zoomBy(1/1.25); });
    connect(plus, &QToolButton::clicked, this, [preview]{ preview->zoomBy(1.25); });
    connect(actual, &QToolButton::clicked, preview, &CleanupPreviewViewport::actualSize);
    connect(fit, &QToolButton::clicked, preview, &CleanupPreviewViewport::fit);

    const auto prompt = i18n::text("layers.cleanup.threshold");
    auto* label = i18n::make<QLabel>(prompt, this);
    label->setBuddy(threshold_);
    layout->addWidget(label);
    threshold_->setObjectName("CleanupThreshold");
    threshold_->setDecimals(0);
    threshold_->setRange(0, 255);
    threshold_->setSingleStep(1);
    threshold_->setValue(core::defaultCleanupAlpha);
    threshold_->setFixedHeight(compare->sizeHint().height());
    threshold_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    i18n::setAccessibleName(threshold_, prompt);
    auto* thresholdRow = new QHBoxLayout;
    thresholdRow->addWidget(threshold_, 1);
    thresholdRow->addWidget(compare);
    layout->addLayout(thresholdRow);
    connect(threshold_, &QDoubleSpinBox::valueChanged, preview,
        [preview](double value) { preview->setThreshold(int(value)); });

    int current = 0;
    std::vector<std::shared_ptr<const core::RasterSurface>> sources;
    for (const auto id : ids) {
        const auto* layer = doc.layer(id);
        if (id == selected.primary) current = int(sources.size());
        sources.push_back(std::get<core::RasterLayer>(layer->payload).surface);
        if (picker) picker->addItem(QString::fromStdString(layer->name));
    }
    if (picker) {
        picker->setCurrentIndex(current);
        connect(picker, &QComboBox::currentIndexChanged, this,
            [preview, sources](int index) { if (index >= 0) preview->setSource(sources.at(size_t(index))); });
    }
    preview->setSource(sources.at(size_t(current)));
}

int LayerCleanupPreview::threshold() const { return int(threshold_->value()); }
} // namespace imageeditor::ui
