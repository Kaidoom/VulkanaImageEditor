#pragma once

#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/TextLayoutService.hpp"
#include <QImage>
#include <QRectF>
#include <QTextCursor>
#include <QTextDocument>

namespace imageeditor::ui {

// Qt positions are UTF-16; canonical run offsets are UTF-8 bytes. This adapter
// is the only owner of Qt layout data. Its undo stack is always disabled.
class QtTextLayout final {
public:
    explicit QtTextLayout(const core::TextLayer& text);
    void reset(const core::TextLayer& text);
    [[nodiscard]] core::TextLayer text() const;
    [[nodiscard]] QTextDocument& document() noexcept { return document_; }
    [[nodiscard]] QRectF bounds() const;
    [[nodiscard]] QRectF caret(int position) const;
    [[nodiscard]] std::vector<QRectF> selection(int first, int last) const;
    [[nodiscard]] int hit(core::Vec2d local) const;
    [[nodiscard]] std::shared_ptr<const core::LayerRenderCache> rasterize(double density,
        std::optional<std::size_t> exactPixelBudget = {}) const;
    // Paint the model's glyph layout once onto the final document pixel grid,
    // keeping the canonical text origin independent of ink overhang/AA bounds.
    [[nodiscard]] core::RectD documentBounds(const core::AffineTransform& localToDocument) const;
    [[nodiscard]] std::shared_ptr<const core::LayerRenderCache> rasterizeDocument(
        const core::AffineTransform& localToDocument, std::size_t exactPixelBudget,
        std::optional<core::RectI> documentClip = {}) const;
    [[nodiscard]] static QTextCharFormat format(const core::TextStyle& style);
    [[nodiscard]] static core::TextStyle style(
        const QTextCharFormat& format, core::TextStyle fallback = { });

private:
    QImage metricsDevice_;
    QTextDocument document_;
    core::TextStyle defaultStyle_;
};

class QtTextLayoutService final : public core::TextLayoutService {
public:
    core::TextLayoutResult layout(const core::TextLayoutRequest&) override;
};
} // namespace imageeditor::ui
