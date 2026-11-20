#include "LayerCleanupPixels.hpp"
#include <QElapsedTimer>
#include <algorithm>

namespace imageeditor::ui {
namespace {
QRgb* row(QImage& image, int y) { return reinterpret_cast<QRgb*>(image.scanLine(y)); }
}
void CleanupPixelPatch::rethresholdNative(int value)
{
    Q_ASSERT(level == 0);
    if (value == threshold) return;
    for (int y = 0; y < original.height(); ++y) {
        const auto* in = reinterpret_cast<const QRgb*>(original.constScanLine(y));
        auto* out = row(cleaned, y);
        for (int x = 0; x < original.width(); ++x)
            out[x] = core::cleanupTargetsAlpha(std::uint8_t(qAlpha(in[x])), std::uint8_t(value)) ? 0 : in[x];
    }
    threshold = value;
}

void CleanupPixelScan::Sum::add(core::Rgba8 p)
{
    red += unsigned(p.red) * p.alpha;
    green += unsigned(p.green) * p.alpha;
    blue += unsigned(p.blue) * p.alpha;
    alpha += p.alpha;
}

QRgb CleanupPixelScan::Sum::average(int count) const
{
    const auto n = std::uint64_t(count);
    const auto divisor = n * 255;
    const auto component = [&](auto sum) { return int((sum + divisor / 2) / divisor); };
    return qRgba(component(red), component(green), component(blue), int((alpha + n / 2) / n));
}

CleanupPixelScan::CleanupPixelScan(QRect region, int lod, int threshold)
{
    const int step = 1 << lod, w = region.width(), h = region.height();
    const QSize size((w+step-1)/step, (h+step-1)/step);
    patch = {region, lod, threshold, QImage(size, QImage::Format_ARGB32_Premultiplied),
            QImage(size, QImage::Format_ARGB32_Premultiplied)};
    if (patch.original.isNull() || patch.cleaned.isNull()) throw std::bad_alloc();
    rowsPerBand_ = std::clamp(32768/w, 1, h);
    band_.resize(size_t(w) * size_t(rowsPerBand_));
    if (lod) sums_.resize(size_t(size.width()));
}

bool CleanupPixelScan::advance(const core::RasterSurface& source)
{
    QElapsedTimer slice;
    slice.start();
    const int w = patch.region.width(), h = patch.region.height(), step = 1 << patch.level;
    do {
        const int rows = std::min(rowsPerBand_, h-nextRow_);
        source.copyRgba8({patch.region.x(), patch.region.y()+nextRow_, w, rows},
            std::as_writable_bytes(std::span(band_)), size_t(w)*4);
        for (int r = 0; r < rows; ++r) {
            const int y = nextRow_+r;
            const auto* pixels = band_.data()+size_t(r)*size_t(w);
            auto* before = row(patch.original, y >> patch.level);
            auto* after = row(patch.cleaned, y >> patch.level);
            for (int x = 0; x < w; ++x) {
                const auto p = pixels[x];
                const bool clear = core::cleanupTargetsAlpha(p.alpha, std::uint8_t(patch.threshold));
                if (patch.level == 0) {
                    before[x] = qPremultiply(qRgba(p.red,p.green,p.blue,p.alpha));
                    after[x] = clear ? 0 : before[x];
                } else {
                    auto& sums = sums_[size_t(x >> patch.level)];
                    sums[0].add(p);
                    if (!clear) sums[1].add(p);
                }
            }
            if (patch.level && ((y+1)%step == 0 || y+1 == h)) {
                for (int x = 0; x < patch.original.width(); ++x) {
                    auto& sums = sums_[size_t(x)];
                    const int count = std::min(step,w-x*step) * (y%step+1);
                    before[x] = sums[0].average(count);
                    after[x] = sums[1].average(count);
                    sums = {};
                }
            }
        }
        nextRow_ += rows;
    } while (nextRow_ < h && slice.elapsed() < 4);
    return nextRow_ == h;
}
} // namespace imageeditor::ui
