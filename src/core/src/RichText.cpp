#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/Document.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace imageeditor::core {
Utf8TextIndex::Utf8TextIndex(std::string_view text)
{
    if (text.size() > kMaximumTextBytes)
        throw std::length_error("Text exceeds 256 KiB limit");
    bytes_.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size();) {
        const auto begin = i;
        const auto c = static_cast<unsigned char>(text[i++]);
        std::uint32_t code = c;
        unsigned continuation = 0;
        if (c >= 0xf0 && c <= 0xf4) {
            code = c & 7;
            continuation = 3;
        } else if (c >= 0xe0 && c <= 0xef) {
            code = c & 15;
            continuation = 2;
        } else if (c >= 0xc2 && c <= 0xdf) {
            code = c & 31;
            continuation = 1;
        } else if (c >= 0x80)
            throw std::invalid_argument("Invalid UTF-8 text");
        for (unsigned n = 0; n < continuation; ++n) {
            if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80)
                throw std::invalid_argument("Invalid UTF-8 text");
            code = (code << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        }
        if ((continuation == 1 && code < 0x80) || (continuation == 2 && code < 0x800)
            || (continuation == 3 && code < 0x10000) || code > 0x10ffff
            || (code >= 0xd800 && code <= 0xdfff))
            throw std::invalid_argument("Invalid UTF-8 scalar");
        bytes_.push_back(begin);
        if (code > 0xffff)
            bytes_.push_back(begin);
    }
    bytes_.push_back(text.size());
}
std::size_t Utf8TextIndex::byteOffset(std::size_t p) const noexcept
{
    return bytes_[std::min(p, bytes_.size() - 1)];
}
std::size_t Utf8TextIndex::utf16Offset(std::size_t p) const noexcept
{
    auto it = std::lower_bound(bytes_.begin(), bytes_.end(), p);
    if (it == bytes_.end())
        return bytes_.size() - 1;
    if (*it > p && it != bytes_.begin())
        --it;
    while (it != bytes_.begin() && *(it - 1) == *it)
        --it;
    return std::size_t(it - bytes_.begin());
}
bool Utf8TextIndex::isBoundary(std::size_t p) const noexcept
{
    return std::binary_search(bytes_.begin(), bytes_.end(), p);
}
namespace {
void validateStyle(const TextStyle& s)
{
    if (!std::isfinite(s.sizePixels) || s.sizePixels < 0.25 || s.sizePixels > 2048
        || s.font.family.size() > 512 || s.font.style.size() > 512 || s.font.weight < 1
        || s.font.weight > 1000)
        throw std::invalid_argument("Invalid text style (size range 0.25–2048 document px)");
}
void append(
    std::vector<TextFormatRun>& runs, std::size_t start, std::size_t length, const TextStyle& style)
{
    if (!length)
        return;
    if (!runs.empty() && runs.back().start + runs.back().length == start
        && runs.back().style == style)
        runs.back().length += length;
    else
        runs.push_back({ start, length, style });
}
}
TextStyle patchedTextStyle(TextStyle s, const TextStylePatch& p)
{
    if (p.family)
        s.font.family = *p.family;
    if (p.style)
        s.font.style = *p.style;
    if (p.weight) {
        s.font.weight = *p.weight;
        if (!p.style)
            s.font.style.clear();
    }
    if (p.italic) {
        s.font.italic = *p.italic;
        if (!p.style)
            s.font.style.clear();
    }
    if (p.sizePixels)
        s.sizePixels = *p.sizePixels;
    if (p.color)
        s.color = *p.color;
    validateStyle(s);
    return s;
}
TextStyle textStyleAt(const TextLayer& t, std::size_t byte, bool preceding)
{
    byte = std::min(byte, t.utf8.size());
    if (preceding && byte && t.utf8[byte - 1] != '\n')
        --byte;
    for (const auto& r : t.runs)
        if (byte >= r.start && byte < r.start + r.length)
            return r.style;
    if (byte == t.utf8.size() && !t.runs.empty())
        return t.runs.back().style;
    return t.defaultStyle;
}
TextLayer normalizedText(TextLayer t)
{
    const Utf8TextIndex index(t.utf8);
    validateStyle(t.defaultStyle);
    if (t.runs.size() > kMaximumTextRuns)
        throw std::length_error("Too many text formatting runs");
    std::vector<TextFormatRun> runs;
    std::size_t pos = 0;
    for (const auto& r : t.runs) {
        validateStyle(r.style);
        if (r.start < pos || r.start > t.utf8.size() || r.length > t.utf8.size() - r.start
            || !index.isBoundary(r.start) || !index.isBoundary(r.start + r.length))
            throw std::invalid_argument("Text runs must be ordered UTF-8 ranges");
        append(runs, pos, r.start - pos, t.defaultStyle);
        append(runs, r.start, r.length, r.style);
        pos = r.start + r.length;
    }
    append(runs, pos, t.utf8.size() - pos, t.defaultStyle);
    if (runs.size() > kMaximumTextRuns)
        throw std::length_error("Too many normalized text runs");
    t.runs = std::move(runs);
    std::optional<std::size_t> previous;
    for (const auto& p : t.paragraphs) {
        if (p.start > t.utf8.size() || (p.start && t.utf8[p.start - 1] != '\n')
            || (previous && p.start <= *previous) || p.alignment < TextAlignment::Left
            || p.alignment > TextAlignment::Right)
            throw std::invalid_argument("Invalid paragraph descriptor");
        previous = p.start;
    }
    std::vector<TextParagraph> paragraphs;
    auto alignment = TextAlignment::Left;
    std::size_t supplied = 0;
    for (std::size_t start = 0;;) {
        while (supplied < t.paragraphs.size() && t.paragraphs[supplied].start <= start)
            alignment = t.paragraphs[supplied++].alignment;
        if (alignment < TextAlignment::Left || alignment > TextAlignment::Right)
            throw std::invalid_argument("Invalid alignment");
        paragraphs.push_back({ start, alignment });
        if (paragraphs.size() > 4096)
            throw std::length_error("Text exceeds 4096 paragraphs");
        const auto newline = t.utf8.find('\n', start);
        if (newline == std::string::npos)
            break;
        start = newline + 1;
    }
    t.paragraphs = std::move(paragraphs);
    return t;
}
TextLayer formatTextRange(
    const TextLayer& source, std::size_t first, std::size_t last, const TextStylePatch& patch)
{
    auto t = normalizedText(source);
    const Utf8TextIndex index(t.utf8);
    if (first > last || last > t.utf8.size() || !index.isBoundary(first) || !index.isBoundary(last))
        throw std::invalid_argument("Invalid text range");
    if (first == last)
        return t;
    std::vector<TextFormatRun> runs;
    for (const auto& r : t.runs) {
        const auto a = std::clamp(first, r.start, r.start + r.length),
                   b = std::clamp(last, r.start, r.start + r.length);
        append(runs, r.start, a - r.start, r.style);
        append(runs, a, b - a, patchedTextStyle(r.style, patch));
        append(runs, b, r.start + r.length - b, r.style);
    }
    t.runs = std::move(runs);
    return normalizedText(std::move(t));
}
std::size_t textMemoryCost(const TextLayer& t) noexcept
{
    std::size_t n = t.utf8.capacity() + t.runs.capacity() * sizeof(TextFormatRun)
        + t.paragraphs.capacity() * sizeof(TextParagraph) + t.defaultStyle.font.family.capacity()
        + t.defaultStyle.font.style.capacity();
    for (const auto& r : t.runs)
        n += r.style.font.family.capacity() + r.style.font.style.capacity();
    return n;
}
TextEditCommand::TextEditCommand(LayerId id, TextLayer before, TextLayer after, TextEditHint a,
    TextEditHint b, std::uint64_t key, std::string label)
    : id_(id)
    , before_(normalizedText(std::move(before)))
    , after_(normalizedText(std::move(after)))
    , beforeCursor_(a)
    , afterCursor_(b)
    , mergeKey_(key)
    , label_(std::move(label))
{
}
TextEditCommand::TextEditCommand(Layer created, std::size_t index, std::optional<LayerId> previous,
    TextEditHint cursor, std::uint64_t key)
    : id_(created.id)
    , after_(normalizedText(std::get<TextLayer>(created.payload)))
    , beforeCursor_ { id_, 0, 0 }
    , afterCursor_(cursor)
    , mergeKey_(key)
    , label_("Create text")
    , created_(std::move(created))
    , index_(index)
    , previousActive_(previous)
{
    created_->renderCache.reset();
    created_->filterCache.reset();
    created_->effectCache.reset();
}
bool TextEditCommand::apply(Document& doc)
{
    if (created_ && !doc.containsLayer(id_)) {
        if (after_.utf8.empty())
            return false;
        auto layer = *created_;
        layer.payload = after_;
        layer.renderCache.reset();
        layer.filterCache.reset();
        layer.effectCache.reset();
        return doc.insertLayer(index_, std::move(layer));
    }
    const auto* layer = doc.layer(id_);
    const auto* text = layer ? std::get_if<TextLayer>(&layer->payload) : nullptr;
    return text && *text == before_ && before_ != after_ && doc.setLayerText(id_, after_);
}
bool TextEditCommand::undo(Document& doc)
{
    const auto* layer = doc.layer(id_);
    const auto* text = layer ? std::get_if<TextLayer>(&layer->payload) : nullptr;
    if (!text || *text != after_)
        return false;
    if (created_)
        return doc.takeLayer(id_).has_value();
    return doc.setLayerText(id_, before_);
}
bool TextEditCommand::mergeWith(const Command& command)
{
    const auto* next = dynamic_cast<const TextEditCommand*>(&command);
    if (!next || !mergeKey_ || mergeKey_ != next->mergeKey_ || id_ != next->id_ || next->created_
        || after_ != next->before_)
        return false;
    if (next->after_ == before_ || (created_ && next->after_.utf8.empty()))
        return false;
    // Admission is already complete. Under memory pressure keep two commands
    // instead of throwing after document mutation or losing the admitted edit.
    try {
        auto after = next->after_;
        after_ = std::move(after);
    } catch (const std::bad_alloc&) {
        return false;
    }
    afterCursor_ = next->afterCursor_;
    return true;
}
bool TextEditCommand::canAdoptApplied(const Document& document) const noexcept
{
    const auto* layer = document.layer(id_);
    const auto* text = layer ? std::get_if<TextLayer>(&layer->payload) : nullptr;
    // Creation can be published from a retractable initial editing branch too.
    return text && *text == after_ && before_ != after_
        && (!created_ || layer->localToDocument == created_->localToDocument);
}
std::size_t TextEditCommand::memoryCost() const noexcept
{
    return sizeof(*this) + textMemoryCost(before_) + textMemoryCost(after_) + label_.capacity()
        + (created_
                ? created_->name.capacity() + textMemoryCost(std::get<TextLayer>(created_->payload))
                    + adjustmentMemoryCost(created_->adjustments)
                    + spatialFilterMemoryCost(created_->filters)
                    + (created_->effects?sizeof(LayerEffectStack):0)
                : 0);
}
std::optional<std::uint64_t> TextEditCommand::activeLayerAfter(bool undo) const noexcept
{
    return undo && created_ ? previousActive_ : std::optional<LayerId>(id_);
}
std::optional<TextEditHint> TextEditCommand::textEditAfter(bool undo) const noexcept
{
    return undo ? beforeCursor_ : afterCursor_;
}
}
