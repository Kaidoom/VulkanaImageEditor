#include "imageeditor/ui/DiffusionTransaction.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"
#include "imageeditor/core/ColorMath.hpp"

#include <QColorSpace>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
constexpr std::uint64_t maximumPixels = 64ULL * 1024 * 1024;
void validateSize(QSize size)
{
    if (size.width() <= 0 || size.height() <= 0 || size.width() > 32768 || size.height() > 32768
        || std::uint64_t(size.width()) * std::uint64_t(size.height()) > maximumPixels)
        throw std::invalid_argument("AI input/output images must contain at most 64 megapixels.");
}
QImage rgba(QImage image)
{
    validateSize(image.size());
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb)
        image = image.convertedToColorSpace(QColorSpace::SRgb);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) throw std::bad_alloc();
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
QImage resizeImage(const QImage& source, QSize size)
{
    validateSize(size);
    auto image = rgba(source);
    if (image.size() != size) image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) throw std::bad_alloc();
    return image;
}
std::uint8_t coverage(const core::SelectionState& selection, core::Vec2d point)
{
    if (!selection) return 255;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
        || point.x >= double(std::numeric_limits<std::int32_t>::max())
        || point.y >= double(std::numeric_limits<std::int32_t>::max())) return 0;
    return selection->coverageAtDocumentPixel(static_cast<std::int32_t>(std::floor(point.x)),
        static_cast<std::int32_t>(std::floor(point.y)));
}
void sample(const QImage& image, double x, double y, uchar* output)
{
    // Pixel-centre coordinates, with edge clamping. Work in premultiplied
    // linear light so a transformed translucent boundary has no dark fringe.
    const auto left = int(std::floor(x)), top = int(std::floor(y));
    const double fx = x - left, fy = y - top;
    double color[3] {}, alpha = 0;
    for (int row = 0; row < 2; ++row) for (int column = 0; column < 2; ++column) {
        const auto* pixel = image.constScanLine(std::clamp(top + row, 0, image.height() - 1))
            + std::clamp(left + column, 0, image.width() - 1) * 4;
        const double weight = (row ? fy : 1 - fy) * (column ? fx : 1 - fx);
        const double a = pixel[3] / 255.0;
        alpha += a * weight;
        for (int channel = 0; channel < 3; ++channel)
            color[channel] += core::srgbToLinear(pixel[channel]) * a * weight;
    }
    output[3] = core::alphaToByte(alpha);
    for (int channel = 0; channel < 3; ++channel)
        output[channel] = alpha > 0 ? core::linearToSrgb(color[channel] / alpha) : 0;
}
QImage nativeGenerated(const DiffusionTarget& target, const QImage& generated, bool documentSpace)
{
    if (!documentSpace) return resizeImage(generated, target.originalPixels.size());
    auto source = rgba(generated);
    auto image = target.originalPixels.copy();
    if (image.isNull()) throw std::bad_alloc();
    const double scaleX = source.width() / double(target.canvas.extent.width);
    const double scaleY = source.height() / double(target.canvas.extent.height);
    for (int y = 0; y < image.height(); ++y) {
        auto* out = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto point = target.localToDocument.map(
                {target.rasterOrigin.x + x + .5, target.rasterOrigin.y + y + .5});
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0 || point.y < 0
                || point.x >= target.canvas.extent.width || point.y >= target.canvas.extent.height) continue;
            sample(source, point.x * scaleX - .5, point.y * scaleY - .5, out + x * 4);
        }
    }
    return image;
}
class ReplaceDiffusionPixelsCommand final : public core::Command {
public:
    ReplaceDiffusionPixelsCommand(const core::Document& doc, const DiffusionTarget& target,
        std::shared_ptr<core::RasterSurface> before, std::shared_ptr<core::RasterSurface> after,
        std::optional<core::RectD> frame)
        : owner_(&doc), id_(target.layerId), before_(std::move(before)), after_(std::move(after)),
          origin_(target.rasterOrigin), transform_(target.localToDocument), frame_(frame),
          revision_(target.documentRevision), selectionRevision_(target.selectionRevision),
          sourceRevision_(target.sourceRevision), contentState_(target.contentState) {}
    bool apply(core::Document& doc) override
    {
        if (&doc != owner_ || !matches(doc, before_)) return false;
        if (first_ && (doc.revision() != revision_ || doc.contentState() != contentState_
            || doc.selectionRevision() != selectionRevision_ || before_->revision() != sourceRevision_)) return false;
        if (!doc.setLayerRasterStorage(id_, after_, origin_, frame_)) return false;
        first_ = false;
        return true;
    }
    bool undo(core::Document& doc) override
    {
        return &doc == owner_ && matches(doc, after_) && doc.setLayerRasterStorage(id_, before_, origin_, frame_);
    }
    std::string_view label() const noexcept override { return "AI image edit"; }
    std::size_t memoryCost() const noexcept override
    {
        const auto a = before_->extent(), b = after_->extent();
        return sizeof(*this) + std::size_t(a.width) * a.height * 4 + std::size_t(b.width) * b.height * 4;
    }
    std::optional<core::LayerId> activeLayerAfter(bool) const noexcept override { return id_; }
private:
    bool matches(const core::Document& doc, const std::shared_ptr<core::RasterSurface>& expected) const
    {
        const auto* layer = doc.layer(id_);
        const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
        return raster && raster->surface == expected && layer->localToDocument == transform_
            && layer->rasterOrigin == origin_ && layer->rasterEffectFrame == frame_;
    }
    const core::Document* owner_;
    core::LayerId id_;
    std::shared_ptr<core::RasterSurface> before_, after_;
    core::Vec2d origin_;
    core::AffineTransform transform_;
    std::optional<core::RectD> frame_;
    core::Revision revision_, selectionRevision_, sourceRevision_;
    std::uint64_t contentState_;
    bool first_ {true};
};
}

QImage diffusionRawLayer(const core::Layer& layer)
{
    const auto* raster = std::get_if<core::RasterLayer>(&layer.payload);
    if (!raster || !raster->surface) throw std::invalid_argument("Choose a raster layer to edit its pixels.");
    const auto extent = raster->surface->extent();
    validateSize({int(extent.width), int(extent.height)});
    QImage image(int(extent.width), int(extent.height), QImage::Format_RGBA8888);
    if (image.isNull()) throw std::bad_alloc();
    const auto revision = raster->surface->revision();
    const auto stride = std::size_t(image.bytesPerLine());
    raster->surface->copyRgba8({0, 0, image.width(), image.height()},
        {reinterpret_cast<std::byte*>(image.bits()), stride * std::size_t(image.height())}, stride);
    if (revision != raster->surface->revision()) throw std::runtime_error("The layer changed while preparing AI input.");
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

DiffusionTarget captureDiffusionTarget(const core::Document& doc, core::LayerId id, bool captureSelection)
{
    const auto* layer = doc.layer(id);
    const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    if (!raster || !raster->surface) throw std::invalid_argument("Choose a raster layer to edit its pixels.");
    if (!layer->localToDocument.inverted()) throw std::invalid_argument("The layer has an invalid transform.");
    DiffusionTarget target;
    target.owner = &doc;
    target.documentRevision = doc.revision(); target.contentState = doc.contentState();
    target.selectionRevision = doc.selectionRevision();
    target.layerId = id; target.sourceId = raster->surface->id(); target.sourceRevision = raster->surface->revision();
    target.canvas = doc.canvas(); target.localToDocument = layer->localToDocument; target.rasterOrigin = layer->rasterOrigin;
    if (captureSelection) {
        if (!doc.selection() || doc.selection()->bounds().empty())
            throw std::invalid_argument("Create a selection before using Generative Fill.");
        target.selection = doc.selection();
    }
    target.originalPixels = diffusionRawLayer(*layer);
    if (!diffusionTargetCurrent(doc, target)) throw std::runtime_error("The target changed while preparing AI input.");
    return target;
}

bool diffusionTargetCurrent(const core::Document& doc, const DiffusionTarget& target, QString* error)
{
    const auto reject = [&](const char* message) { if (error) *error = QString::fromUtf8(message); return false; };
    if (&doc != target.owner) return reject("The original target document is no longer available.");
    if (doc.revision() != target.documentRevision || doc.contentState() != target.contentState
        || doc.selectionRevision() != target.selectionRevision || doc.canvas() != target.canvas)
        return reject("The target document or selection has changed.");
    const auto* layer = doc.layer(target.layerId);
    const auto* raster = layer ? std::get_if<core::RasterLayer>(&layer->payload) : nullptr;
    if (!raster || !raster->surface || raster->surface->id() != target.sourceId
        || raster->surface->revision() != target.sourceRevision || layer->localToDocument != target.localToDocument
        || layer->rasterOrigin != target.rasterOrigin)
        return reject("The target layer has changed.");
    if (error) error->clear();
    return true;
}

FlattenedDocumentResult diffusionRenderedLayer(const core::Document& document, core::LayerId id)
{
    const std::array ids {id};
    return flattenLayerItems(document, ids);
}

QImage diffusionSelectionMask(const DiffusionTarget& target, QSize size, bool documentSpace)
{
    const QSize native = documentSpace
        ? QSize(int(target.canvas.extent.width), int(target.canvas.extent.height)) : target.originalPixels.size();
    validateSize(native);
    QImage mask(native, QImage::Format_Grayscale8);
    if (mask.isNull()) throw std::bad_alloc();
    for (int y = 0; y < native.height(); ++y) {
        auto* line = mask.scanLine(y);
        for (int x = 0; x < native.width(); ++x) {
            const core::Vec2d point = documentSpace ? core::Vec2d{x + .5, y + .5}
                : target.localToDocument.map({target.rasterOrigin.x + x + .5, target.rasterOrigin.y + y + .5});
            line[x] = coverage(target.selection, point);
        }
    }
    if (size.isValid() && size != native) {
        validateSize(size);
        mask = mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (mask.isNull()) throw std::bad_alloc();
    }
    return mask;
}

QImage diffusionPrepareFillInput(const QImage& source, const QImage& coverage)
{
    auto image = rgba(source);
    if (coverage.size() != image.size() || coverage.format() != QImage::Format_Grayscale8)
        throw std::invalid_argument("Fill input requires a matching grayscale selection mask.");
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = image.scanLine(y);
        const auto* mask = coverage.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            auto* pixel = pixels + x * 4;
            if (mask[x] && pixel[3] == 0) {
                // RGBA-aware models can interpret empty canvas as transparency
                // to preserve. A neutral opaque placeholder makes the selected
                // space available for synthesis without changing real content.
                pixel[0] = pixel[1] = pixel[2] = pixel[3] = 255;
            }
        }
    }
    return image;
}

QImage diffusionCompositeSelection(const QImage& beforeImage, const QImage& generatedImage, const QImage& coverageImage)
{
    auto before = rgba(beforeImage);
    const auto generated = rgba(generatedImage);
    if (before.size() != generated.size() || before.size() != coverageImage.size()
        || coverageImage.format() != QImage::Format_Grayscale8)
        throw std::invalid_argument("Selection compositing requires matching RGBA images and a grayscale mask.");
    before.detach();
    for (int y = 0; y < before.height(); ++y) {
        auto* output = before.scanLine(y);
        const auto* replacement = generated.constScanLine(y);
        const auto* mask = coverageImage.constScanLine(y);
        for (int x = 0; x < before.width(); ++x) {
            if (!mask[x]) continue;
            const auto offset = x * 4;
            if (mask[x] == 255) { std::memcpy(output + offset, replacement + offset, 4); continue; }
            const auto weight = mask[x] / 255.0;
            const auto a0 = output[offset + 3] / 255.0, a1 = replacement[offset + 3] / 255.0;
            const auto alpha = a0 * (1 - weight) + a1 * weight;
            for (int channel = 0; channel < 3; ++channel) {
                const auto c0 = core::srgbToLinear(output[offset + channel]);
                const auto c1 = core::srgbToLinear(replacement[offset + channel]);
                if (alpha > 0) output[offset + channel] = core::linearToSrgb(
                    (c0 * a0 * (1 - weight) + c1 * a1 * weight) / alpha);
            }
            output[offset + 3] = core::alphaToByte(alpha);
        }
    }
    return before;
}

QImage diffusionFillPreview(const DiffusionTarget& target, const QImage& before,
    const QImage& generated, bool newLayer)
{
    const QSize extent(int(target.canvas.extent.width), int(target.canvas.extent.height));
    if (before.size() != extent)
        throw std::invalid_argument("The Fill preview background must match the captured canvas dimensions.");
    if (!target.selection)
        throw std::invalid_argument("This AI session has no captured selection.");
    if (!newLayer)
        return diffusionCompositeSelection(before, resizeImage(generated, extent),
            diffusionSelectionMask(target, extent, true));

    // Reuse the authoritative compositor rather than approximating its alpha
    // and working-space rules with QPainter's encoded-sRGB SourceOver mode.
    auto base = rasterLayerFromImage(before, QStringLiteral("Preview background"));
    if (!base) throw std::runtime_error(base.error.toStdString());
    QString error;
    auto patch = diffusionResultLayer(&target, generated, true, true,
        QStringLiteral("AI fill preview"), &error);
    if (!patch) throw std::runtime_error(error.toStdString());
    core::Document preview(target.canvas);
    if (!preview.insertLayer(0, std::move(*base.layer)) || !preview.insertLayer(1, std::move(*patch)))
        throw std::runtime_error("Cannot prepare the Fill preview.");
    auto rendered = flattenDocument(preview);
    if (!rendered) throw std::runtime_error(rendered.error.toStdString());
    return rendered.image;
}

QImage diffusionResolvedPixels(const DiffusionTarget& target, const QImage& generated,
    bool documentSpace, bool useSelection)
{
    const auto native = nativeGenerated(target, generated, documentSpace);
    if (!useSelection) return native;
    if (!target.selection) throw std::invalid_argument("This AI session has no captured selection.");
    return diffusionCompositeSelection(target.originalPixels, native, diffusionSelectionMask(target));
}

std::unique_ptr<core::Command> diffusionReplaceCommand(const core::Document& doc, const DiffusionTarget& target,
    const QImage& generated, bool documentSpace, bool useSelection, QString* error)
{
    try {
        if (!diffusionTargetCurrent(doc, target, error)) return {};
        const auto pixels = diffusionResolvedPixels(target, generated, documentSpace, useSelection);
        if (pixels == target.originalPixels)
            throw std::invalid_argument("The AI result does not change this raster layer. For a fill outside its bounds, choose Add as New Layer.");
        const auto loaded = rasterLayerFromImage(pixels, QStringLiteral("AI result"));
        if (!loaded) throw std::runtime_error(loaded.error.toStdString());
        const auto* layer = doc.layer(target.layerId);
        return std::make_unique<ReplaceDiffusionPixelsCommand>(doc, target,
            std::get<core::RasterLayer>(layer->payload).surface,
            std::get<core::RasterLayer>(loaded.layer->payload).surface, layer->rasterEffectFrame);
    } catch (const std::exception& exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return {};
    }
}

std::unique_ptr<core::Command> diffusionReplacePlacedCommand(const core::Document& doc,
    const DiffusionTarget& target, const QImage& generated, const QTransform& placement, QString* error)
{
    try {
        const QSize size(int(target.canvas.extent.width), int(target.canvas.extent.height));
        validateSize(size);
        const auto bounds = placement.mapRect(QRectF(QPointF{}, generated.size()))
                                .intersected(QRectF(QPointF{}, size)).toAlignedRect()
                                .intersected(QRect(QPoint{}, size));
        if (bounds.isEmpty()) throw std::invalid_argument("The cropped result is outside the destination canvas.");
        QImage positioned(size, QImage::Format_RGBA8888);
        if (positioned.isNull()) throw std::bad_alloc();
        positioned.fill(Qt::transparent);
        positioned.setColorSpace(QColorSpace::SRgb);
        QPainter painter(&positioned);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(placement);
        painter.drawImage(QPointF{}, rgba(generated));
        painter.end();
        auto scoped = target;
        scoped.selection = core::SelectionMask::rectangle(target.canvas.extent,
            {bounds.x(), bounds.y(), bounds.width(), bounds.height()});
        return diffusionReplaceCommand(doc, scoped, positioned, true, true, error);
    } catch (const std::exception& e) {
        if (error) *error = QString::fromUtf8(e.what());
        return {};
    }
}

std::optional<core::Layer> diffusionResultLayer(const DiffusionTarget* target, const QImage& generated,
    bool documentSpace, bool useSelection, const QString& name, QString* error)
{
    try {
        QImage image;
        if (!target) {
            if (useSelection) throw std::invalid_argument("Selection-limited output requires a captured editing target.");
            image = rgba(generated);
        } else {
            image = resizeImage(generated, documentSpace
                ? QSize(int(target->canvas.extent.width), int(target->canvas.extent.height)) : target->originalPixels.size());
            if (useSelection) {
                if (!target->selection) throw std::invalid_argument("This AI session has no captured selection.");
                const auto mask = diffusionSelectionMask(*target, image.size(), documentSpace);
                image.detach();
                for (int y = 0; y < image.height(); ++y) {
                    auto* pixels = image.scanLine(y);
                    const auto* weights = mask.constScanLine(y);
                    for (int x = 0; x < image.width(); ++x)
                        pixels[x * 4 + 3] = std::uint8_t((unsigned(pixels[x * 4 + 3]) * weights[x] + 127) / 255);
                }
            }
        }
        auto result = rasterLayerFromImage(std::move(image), name);
        if (!result) throw std::runtime_error(result.error.toStdString());
        if (target && !documentSpace) {
            result.layer->localToDocument = target->localToDocument;
            result.layer->rasterOrigin = target->rasterOrigin;
        }
        if (error) error->clear();
        return std::move(result.layer);
    } catch (const std::exception& exception) {
        if (error) *error = QString::fromUtf8(exception.what());
        return {};
    }
}
} // namespace imageeditor::ui
