#include "imageeditor/ui/QtTextLayoutService.hpp"
#include <QAbstractTextDocumentLayout>
#include <QFontInfo>
#include <QPainter>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <QTransform>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
constexpr int familyProperty = QTextFormat::UserProperty + 901;
constexpr int styleProperty = familyProperty + 1;
constexpr int weightProperty = familyProperty + 2;
constexpr int italicProperty = familyProperty + 3;
constexpr int sizeProperty = familyProperty + 4;
constexpr int originalFaceProperty = familyProperty + 5;
Qt::Alignment alignment(core::TextAlignment a)
{
    return a == core::TextAlignment::Center ? Qt::AlignHCenter
        : a == core::TextAlignment::Right   ? Qt::AlignRight
                                            : Qt::AlignLeft;
}

QTransform qtTransform(const core::AffineTransform& t)
{
    return { t.m00, t.m10, t.m20, t.m01, t.m11, t.m21, t.m02, t.m12, t.m22 };
}

QRectF textDocumentBounds(const QTextDocument& document, const core::AffineTransform& localToDocument)
{
    const auto transform = qtTransform(localToDocument);
    QRectF rect;
    // Layout bounds can include arbitrarily much trailing whitespace. Only
    // glyph ink contributes to output, while the layout's origin stays fixed.
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        const auto origin = document.documentLayout()->blockBoundingRect(block).topLeft();
        for (const auto& run : block.layout()->glyphRuns(0, block.length() - 1)) {
            const auto glyphs = run.glyphIndexes();
            const auto positions = run.positions();
            for (qsizetype i = 0; i < glyphs.size(); ++i) {
                const auto ink = run.rawFont().boundingRect(glyphs[i]);
                if (!ink.isEmpty())
                    rect = rect.united(transform.mapRect(ink.translated(origin + positions[i])));
            }
        }
    }
    return rect.isEmpty() ? QRectF {} : rect.adjusted(-2, -2, 2, 2);
}
}
QTextCharFormat QtTextLayout::format(const core::TextStyle& s)
{
    QTextCharFormat f;
    QFont font(QString::fromStdString(s.font.family));
    font.setPointSizeF(s.sizePixels * 72.0 / 96.0);
    font.setWeight(static_cast<QFont::Weight>(s.font.weight));
    font.setItalic(s.font.italic);
    if (!s.font.style.empty())
        font.setStyleName(QString::fromStdString(s.font.style));
    f.setFont(font);
    f.setForeground(QColor(s.color.red, s.color.green, s.color.blue, s.color.alpha));
    // Requested descriptors, not resolved fallback faces, round-trip verbatim.
    f.setProperty(familyProperty, QString::fromStdString(s.font.family));
    f.setProperty(styleProperty, QString::fromStdString(s.font.style));
    f.setProperty(weightProperty, s.font.weight);
    f.setProperty(italicProperty, s.font.italic);
    f.setProperty(sizeProperty, s.sizePixels);
    f.setProperty(originalFaceProperty, QString::fromStdString(s.font.originalFace));
    return f;
}
core::TextStyle QtTextLayout::style(const QTextCharFormat& f, core::TextStyle s)
{
    if (f.hasProperty(familyProperty))
        s.font.family = f.property(familyProperty).toString().toStdString();
    if (f.hasProperty(styleProperty))
        s.font.style = f.property(styleProperty).toString().toStdString();
    if (f.hasProperty(weightProperty))
        s.font.weight = f.property(weightProperty).toInt();
    if (f.hasProperty(italicProperty))
        s.font.italic = f.property(italicProperty).toBool();
    if (f.hasProperty(sizeProperty))
        s.sizePixels = f.property(sizeProperty).toDouble();
    if (f.hasProperty(originalFaceProperty))
        s.font.originalFace = f.property(originalFaceProperty).toString().toStdString();
    if (f.hasProperty(QTextFormat::ForegroundBrush)) {
        const auto c = f.foreground().color();
        s.color = { static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha()) };
    }
    return s;
}
QtTextLayout::QtTextLayout(const core::TextLayer& t)
    : metricsDevice_(1, 1, QImage::Format_ARGB32)
{
    metricsDevice_.setDotsPerMeterX(3780);
    metricsDevice_.setDotsPerMeterY(3780);
    document_.setUndoRedoEnabled(false);
    document_.setDocumentMargin(0);
    document_.documentLayout()->setPaintDevice(&metricsDevice_);
    QTextOption option;
    option.setUseDesignMetrics(true);
    option.setWrapMode(QTextOption::NoWrap);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    document_.setDefaultTextOption(option);
    reset(t);
}
void QtTextLayout::reset(const core::TextLayer& source)
{
    const auto t = core::normalizedText(source);
    defaultStyle_ = t.defaultStyle;
    document_.clear();
    document_.setDefaultFont(format(defaultStyle_).font());
    QTextCursor cursor(&document_);
    cursor.setCharFormat(format(defaultStyle_));
    const core::Utf8TextIndex index(t.utf8);
    cursor.insertText(QString::fromUtf8(t.utf8.data(), static_cast<qsizetype>(t.utf8.size())),
        format(defaultStyle_));
    for (const auto& run : t.runs) {
        cursor.setPosition(static_cast<int>(index.utf16Offset(run.start)));
        cursor.setPosition(
            static_cast<int>(index.utf16Offset(run.start + run.length)), QTextCursor::KeepAnchor);
        cursor.setCharFormat(format(run.style));
    }
    for (const auto& p : t.paragraphs) {
        cursor.setPosition(static_cast<int>(index.utf16Offset(p.start)));
        QTextBlockFormat bf;
        bf.setAlignment(alignment(p.alignment));
        cursor.setBlockFormat(bf);
    }
    document_.setTextWidth(-1);
    document_.adjustSize();
    document_.setTextWidth(std::max(1.0, document_.idealWidth()));
    (void)document_.documentLayout()->documentSize();
}
core::TextLayer QtTextLayout::text() const
{
    core::TextLayer result;
    result.defaultStyle = defaultStyle_;
    result.paragraphs.clear();
    for (auto block = document_.begin(); block.isValid(); block = block.next()) {
        const auto a = block.blockFormat().alignment();
        result.paragraphs.push_back({ result.utf8.size(),
            a.testFlag(Qt::AlignHCenter)     ? core::TextAlignment::Center
                : a.testFlag(Qt::AlignRight) ? core::TextAlignment::Right
                                             : core::TextAlignment::Left });
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const auto bytes = fragment.text().toUtf8();
            result.runs.push_back({ result.utf8.size(), static_cast<std::size_t>(bytes.size()),
                style(fragment.charFormat(), defaultStyle_) });
            result.utf8.append(bytes.constData(), static_cast<std::size_t>(bytes.size()));
        }
        if (block.next().isValid()) {
            result.runs.push_back(
                { result.utf8.size(), 1, style(block.next().charFormat(), defaultStyle_) });
            result.utf8 += '\n';
        }
    }
    return core::normalizedText(std::move(result));
}
QRectF QtTextLayout::bounds() const
{
    const auto size = document_.documentLayout()->documentSize();
    return { 0, 0, std::max(1.0, size.width()), std::max(1.0, size.height()) };
}
QRectF QtTextLayout::caret(int pos) const
{
    pos = std::clamp(pos, 0, document_.characterCount() - 1);
    auto block = document_.findBlock(pos);
    auto* layout = block.layout();
    const int inBlock = pos - block.position();
    auto line = layout->lineForTextPosition(inBlock);
    if (!line.isValid() && layout->lineCount())
        line = layout->lineAt(layout->lineCount() - 1);
    const auto rect = document_.documentLayout()->blockBoundingRect(block);
    if (!line.isValid())
        return { rect.topLeft(), QSizeF(1, std::max(1.0, defaultStyle_.sizePixels)) };
    return { rect.x() + line.cursorToX(inBlock), rect.y() + line.y(), 1, line.height() };
}
std::vector<QRectF> QtTextLayout::selection(int first, int last) const
{
    if (first > last)
        std::swap(first, last);
    std::vector<QRectF> out;
    // Glyph-run rectangles preserve disjoint visual ranges in bidi text.
    for (auto block = document_.findBlock(first); block.isValid() && block.position() <= last;
        block = block.next()) {
        auto* layout = block.layout();
        const auto origin = document_.documentLayout()->blockBoundingRect(block).topLeft();
        const int start = std::max(first - block.position(), 0);
        const int end = std::min(last - block.position(), block.length() - 1);
        if (end > start) {
            const auto runs = layout->glyphRuns(start, end - start);
            for (const auto& run : runs) {
                auto rect = run.boundingRect().translated(origin);
                const auto line = layout->lineForTextPosition(start);
                if (line.isValid()) {
                    rect.setTop(origin.y() + line.y());
                    rect.setHeight(line.height());
                }
                if (!rect.isEmpty())
                    out.push_back(rect);
            }
            if (runs.isEmpty()) {
                auto a = caret(block.position() + start), b = caret(block.position() + end);
                out.emplace_back(std::min(a.x(), b.x()), a.y(),
                    std::max(1.0, std::abs(a.x() - b.x())), a.height());
            }
        }
        if (last >= block.position() + block.length() && block.next().isValid()) {
            auto rect = caret(block.position() + block.length() - 1);
            rect.setWidth(defaultStyle_.sizePixels * .4);
            out.push_back(rect);
        }
    }
    return out;
}
int QtTextLayout::hit(core::Vec2d p) const
{
    return std::clamp(document_.documentLayout()->hitTest(QPointF(p.x, p.y), Qt::FuzzyHit), 0,
        document_.characterCount() - 1);
}
std::shared_ptr<const core::LayerRenderCache> QtTextLayout::rasterize(double requested,
    std::optional<std::size_t> exactPixelBudget) const
{
    auto rect = bounds();
    // Include ink overhang without moving the canonical top-left origin.
    for (auto block = document_.begin(); block.isValid(); block = block.next()) {
        const auto origin = document_.documentLayout()->blockBoundingRect(block).topLeft();
        for (const auto& run : block.layout()->glyphRuns(0, block.length() - 1)) {
            const auto glyphs = run.glyphIndexes();
            const auto positions = run.positions();
            for (qsizetype i = 0; i < glyphs.size(); ++i)
                rect = rect.united(
                    run.rawFont().boundingRect(glyphs[i]).translated(origin + positions[i]));
        }
    }
    rect = rect.adjusted(-2, -2, 2, 2);
    const double density = exactPixelBudget ? requested : std::min({ std::clamp(requested, .125, 8.0), 8192.0 / rect.width(),
        8192.0 / rect.height(), std::sqrt(16.0 * 1024 * 1024 / (rect.width() * rect.height())) });
    if(exactPixelBudget) {
        const auto w=std::ceil(rect.width()*density),h=std::ceil(rect.height()*density);
        const auto budget=std::min<std::size_t>(*exactPixelBudget,64ULL*1024*1024);
        if(!std::isfinite(w)||!std::isfinite(h)||w<1||h<1||w>32768||h>32768||w*h>double(budget))
            throw std::runtime_error("Text output-resolution cache exceeds the merge/export budget");
    }
    const int w = std::max(1, static_cast<int>(std::ceil(rect.width() * density)));
    const int h = std::max(1, static_cast<int>(std::ceil(rect.height() * density)));
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        throw std::bad_alloc();
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.scale(density, density);
        painter.translate(-rect.topLeft());
        QAbstractTextDocumentLayout::PaintContext context;
        document_.documentLayout()->draw(&painter, context);
    }
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        throw std::bad_alloc();
    std::vector<std::byte> bytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        std::memcpy(bytes.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4,
            image.constScanLine(y), static_cast<std::size_t>(w) * 4);
    auto cache = std::make_shared<core::LayerRenderCache>();
    cache->surface = std::make_shared<core::ContiguousRasterSurface>(
        core::Extent2u { static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h) },
        std::move(bytes));
    cache->pixelsToLocal = { 1 / density, 0, rect.x(), 0, 1 / density, rect.y() };
    cache->logicalExtent = { static_cast<std::uint32_t>(std::ceil(bounds().width())),
        static_cast<std::uint32_t>(std::ceil(bounds().height())) };
    cache->density = density;
    return cache;
}

core::RectD QtTextLayout::documentBounds(const core::AffineTransform& localToDocument) const
{
    const auto support=bounds().adjusted(-2,-2,2,2).united(textDocumentBounds(document_,{}));
    if (!localToDocument.validOver({support.x(),support.y(),support.width(),support.height()}))
        throw std::invalid_argument("Invalid text transform in document rasterization");
    const auto rect = textDocumentBounds(document_, localToDocument);
    return { rect.x(), rect.y(), rect.width(), rect.height() };
}

std::shared_ptr<const core::LayerRenderCache> QtTextLayout::rasterizeDocument(
    const core::AffineTransform& localToDocument, std::size_t exactPixelBudget,
    std::optional<core::RectI> documentClip) const
{
    const auto inverse = localToDocument.inverted();
    const auto support=bounds().adjusted(-2,-2,2,2).united(textDocumentBounds(document_,{}));
    if (!inverse || !localToDocument.validOver({support.x(),support.y(),support.width(),support.height()}))
        throw std::invalid_argument("Invalid text transform in document rasterization");
    auto rect = textDocumentBounds(document_, localToDocument);
    if (documentClip)
        rect = rect.intersected(QRectF(documentClip->x, documentClip->y,
            documentClip->width, documentClip->height));
    const bool empty = rect.isEmpty();
    const double left = empty ? 0 : std::floor(rect.left()), top = empty ? 0 : std::floor(rect.top());
    const double width = empty ? 1 : std::ceil(rect.right()) - left;
    const double height = empty ? 1 : std::ceil(rect.bottom()) - top;
    const auto budget = std::min<std::size_t>(exactPixelBudget, 64ULL * 1024 * 1024);
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height)
        || width < 1 || height < 1 || width > 32768 || height > 32768 || width * height > double(budget))
        throw std::runtime_error("Text document raster cache exceeds the merge/export budget");

    const int w = int(width), h = int(height);
    std::vector<std::byte> bytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    if (!empty) {
        // Paint into the final allocation. Qt performs the same-depth format
        // conversion in place when supported; its conversion routine is also
        // the live text path's authoritative unpremultiplication contract.
        QImage image(reinterpret_cast<uchar*>(bytes.data()), w, h, w * 4, QImage::Format_ARGB32_Premultiplied);
        if (image.isNull())
            throw std::bad_alloc();
        image.setDotsPerMeterX(metricsDevice_.dotsPerMeterX());
        image.setDotsPerMeterY(metricsDevice_.dotsPerMeterY());
        {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::TextAntialiasing);
            auto localToPixels = core::composeTransform({1,0,-left,0,1,-top},localToDocument);
            painter.setWorldTransform(qtTransform(localToPixels));
            QAbstractTextDocumentLayout::PaintContext context;
            document_.documentLayout()->draw(&painter, context);
        }
        image.convertTo(QImage::Format_RGBA8888);
        if (image.isNull())
            throw std::bad_alloc();
        // Some Qt paint engines detach an externally owned image during
        // conversion. Preserve the same pixels without a second vector.
        if (image.constBits() != reinterpret_cast<const uchar*>(bytes.data()))
            for (int y = 0; y < h; ++y)
                std::memcpy(bytes.data() + std::size_t(y) * std::size_t(w) * 4,
                    image.constScanLine(y), std::size_t(w) * 4);
    }
    auto cache = std::make_shared<core::LayerRenderCache>();
    cache->surface = std::make_shared<core::ContiguousRasterSurface>(
        core::Extent2u { std::uint32_t(w), std::uint32_t(h) }, std::move(bytes));
    cache->pixelsToLocal = core::composeTransform(*inverse,{1,0,left,0,1,top});
    cache->logicalExtent = { static_cast<std::uint32_t>(std::ceil(bounds().width())),
        static_cast<std::uint32_t>(std::ceil(bounds().height())) };
    cache->rasterizedDocumentTransform = localToDocument;
    cache->documentOrigin = { left, top };
    const auto localRect = bounds().adjusted(-2, -2, 2, 2).united(textDocumentBounds(document_, {}));
    cache->localSourceBounds = core::RectD { localRect.x(), localRect.y(),
        std::ceil(localRect.width()), std::ceil(localRect.height()) };
    return cache;
}

core::TextLayoutResult QtTextLayoutService::layout(const core::TextLayoutRequest& r)
{
    QtTextLayout layout(r.text);
    core::TextLayoutResult result;
    result.cache = layout.rasterize(r.rasterDensity);
    result.resolvedFontFamily
        = QFontInfo(QtTextLayout::format(r.text.defaultStyle).font()).family().toStdString();
    return result;
}
} // namespace imageeditor::ui
