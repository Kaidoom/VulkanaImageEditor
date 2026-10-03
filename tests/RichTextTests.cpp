#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RichText.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace imageeditor::core;
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template <typename Exception, typename Function> bool throws(Function&& function)
{
    try {
        function();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

TextStyle style(double size, Rgba8 color = { 23, 77, 131, 190 })
{
    TextStyle result;
    result.font = { "Requested font not installed", "Book", 430, false };
    result.sizePixels = size;
    result.color = color;
    return result;
}

TextLayer text(std::string value, TextStyle base = style(9))
{
    TextLayer result;
    result.utf8 = std::move(value);
    result.defaultStyle = std::move(base);
    return normalizedText(std::move(result));
}

bool hintEquals(std::optional<TextEditHint> hint, LayerId layer, std::size_t anchor, std::size_t position)
{
    return hint && hint->layer == layer && hint->anchor == anchor && hint->position == position;
}

struct Fixture {
    Document document { CanvasSpec { .extent = { 100, 100 } } };
    History history;
    LayerId id;
    explicit Fixture(TextLayer value = text("hello"))
    {
        auto layer = Layer::text("Text target", std::move(value));
        id = layer.id;
        CHECK(document.insertLayer(0, std::move(layer)));
    }
    const TextLayer& content() const { return std::get<TextLayer>(document.layer(id)->payload); }
    bool edit(
        TextLayer after, std::uint64_t key = 0, std::size_t beforePosition = 0, std::size_t afterPosition = 0)
    {
        return history.execute(document,
            std::make_unique<TextEditCommand>(id, content(), std::move(after),
                TextEditHint { id, beforePosition, beforePosition },
                TextEditHint { id, afterPosition, afterPosition }, key));
    }
};

void utf8Utf16ScalarMapping()
{
    // A + e-acute + face (surrogate pair) + combining acute + ZWJ + NBSP + LF.
    const std::string value = "A\xc3\xa9\xf0\x9f\x98\x80\xcc\x81\xe2\x80\x8d\xc2\xa0\n";
    const Utf8TextIndex index(value);
    const std::array<std::size_t, 9> byteAtUtf16 { 0, 1, 3, 3, 7, 9, 12, 14, 15 };
    CHECK(index.utf16Length() == 8);
    for (std::size_t u = 0; u < byteAtUtf16.size(); ++u)
        CHECK(index.byteOffset(u) == byteAtUtf16[u]);
    const std::array<std::size_t, 8> boundaries { 0, 1, 3, 7, 9, 12, 14, 15 };
    const std::array<std::size_t, 8> utf16AtBoundary { 0, 1, 2, 4, 5, 6, 7, 8 };
    for (std::size_t i = 0; i < boundaries.size(); ++i) {
        CHECK(index.isBoundary(boundaries[i]));
        CHECK(index.utf16Offset(boundaries[i]) == utf16AtBoundary[i]);
        if (i + 1 < boundaries.size()) {
            for (auto byte = boundaries[i] + 1; byte < boundaries[i + 1]; ++byte) {
                CHECK(!index.isBoundary(byte));
                CHECK(index.utf16Offset(byte) == utf16AtBoundary[i]);
            }
        }
    }
    CHECK(index.byteOffset(std::numeric_limits<std::size_t>::max()) == value.size());
    CHECK(index.utf16Offset(std::numeric_limits<std::size_t>::max()) == index.utf16Length());
    CHECK(!index.isBoundary(value.size() + 1));
    const Utf8TextIndex empty("");
    CHECK(empty.utf16Length() == 0);
    CHECK(empty.byteOffset(100) == 0);
    CHECK(empty.utf16Offset(100) == 0);
    CHECK(empty.isBoundary(0));
}

std::string encodeScalar(std::uint32_t scalar)
{
    std::string result;
    if (scalar < 0x80)
        result.push_back(static_cast<char>(scalar));
    else if (scalar < 0x800) {
        result.push_back(static_cast<char>(0xc0U | (scalar >> 6)));
        result.push_back(static_cast<char>(0x80U | (scalar & 63)));
    } else if (scalar < 0x10000) {
        result.push_back(static_cast<char>(0xe0U | (scalar >> 12)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 63)));
        result.push_back(static_cast<char>(0x80U | (scalar & 63)));
    } else {
        result.push_back(static_cast<char>(0xf0U | (scalar >> 18)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 12) & 63)));
        result.push_back(static_cast<char>(0x80U | ((scalar >> 6) & 63)));
        result.push_back(static_cast<char>(0x80U | (scalar & 63)));
    }
    return result;
}

void fixedSeedUnicodeAndInvalidInput()
{
    std::mt19937 random(0x74e87U);
    std::string value;
    std::vector<std::pair<std::size_t, std::size_t>> boundaries;
    std::size_t utf16 = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        auto scalar = static_cast<std::uint32_t>(random() % 0x110000U);
        if (scalar >= 0xd800 && scalar <= 0xdfff)
            scalar = 'X';
        boundaries.emplace_back(value.size(), utf16);
        value += encodeScalar(scalar);
        utf16 += scalar > 0xffff ? 2U : 1U;
    }
    boundaries.emplace_back(value.size(), utf16);
    const Utf8TextIndex index(value);
    CHECK(index.utf16Length() == utf16);
    for (const auto& [byte, position] : boundaries) {
        CHECK(index.byteOffset(position) == byte);
        CHECK(index.utf16Offset(byte) == position);
    }
    const std::array invalid { std::string("\x80"), std::string("\xc0\xaf"), std::string("\xc1\xbf"),
        std::string("\xc2"), std::string("\xc2 "), std::string("\xe0\x80\x80"), std::string("\xed\xa0\x80"),
        std::string("\xed\xbf\xbf"), std::string("\xf0\x80\x80\x80"), std::string("\xf4\x90\x80\x80"),
        std::string("\xf5\x80\x80\x80"), std::string("\xff") };
    for (const auto& input : invalid)
        CHECK(throws<std::invalid_argument>([&] { (void)Utf8TextIndex(input); }));
    CHECK(throws<std::length_error>([] { (void)Utf8TextIndex(std::string(kMaximumTextBytes + 1, 'a')); }));
    CHECK(Utf8TextIndex(std::string(kMaximumTextBytes, 'a')).utf16Length() == kMaximumTextBytes);
}

void normalizedRunsAndParagraphs()
{
    TextLayer value;
    value.utf8 = "abcdef\nxy\n";
    value.defaultStyle = style(9);
    const auto alternate = style(20, { 255, 0, 0, 128 });
    value.runs = { { 1, 2, alternate }, { 3, 1, alternate }, { 5, 0, alternate } };
    value.paragraphs = { { 0, TextAlignment::Center }, { 7, TextAlignment::Right } };
    const auto result = normalizedText(value);
    CHECK(result.utf8 == value.utf8);
    CHECK(result.defaultStyle == value.defaultStyle);
    CHECK(result.runs
        == std::vector<TextFormatRun>(
            { { 0, 1, value.defaultStyle }, { 1, 3, alternate }, { 4, 6, value.defaultStyle } }));
    CHECK(result.paragraphs
        == std::vector<TextParagraph>(
            { { 0, TextAlignment::Center }, { 7, TextAlignment::Right }, { 10, TextAlignment::Right } }));
    CHECK(normalizedText(result) == result);

    const auto blank = text("");
    CHECK(blank.runs.empty());
    CHECK(blank.paragraphs == std::vector<TextParagraph>({ { 0, TextAlignment::Left } }));
    CHECK(text("\n\n").paragraphs.size() == 3);

    auto malformed = text("abcdef");
    malformed.runs = { { 2, 3, alternate }, { 3, 1, value.defaultStyle } };
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    malformed.runs = { { 7, 0, alternate } };
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    malformed.runs = { { 2, std::numeric_limits<std::size_t>::max(), alternate } };
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    malformed = text("\xc3\xa9");
    malformed.runs = { { 1, 1, alternate } };
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    malformed.runs = { { 0, 1, alternate } };
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    malformed = text("abc");
    malformed.defaultStyle.sizePixels = std::numeric_limits<double>::quiet_NaN();
    CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    for (const auto& paragraphs : std::vector<std::vector<TextParagraph>> { { { 1, TextAlignment::Left } },
             { { 100, TextAlignment::Left } }, { { 2, TextAlignment::Left }, { 0, TextAlignment::Right } },
             { { 0, TextAlignment::Left }, { 0, TextAlignment::Right } },
             { { 0, static_cast<TextAlignment>(100) } } }) {
        malformed = text("a\nb");
        malformed.paragraphs = paragraphs;
        CHECK(throws<std::invalid_argument>([&] { (void)normalizedText(malformed); }));
    }
    CHECK(throws<std::length_error>([] { (void)text(std::string(4096, '\n')); }));
    CHECK(text(std::string(4095, '\n')).paragraphs.size() == 4096);
}

void canonicalAdmissionAndRunBounds()
{
    TextLayer raw;
    raw.utf8 = "Implicit format";
    raw.defaultStyle = style(9);
    Fixture fixture(raw);
    CHECK(fixture.content() == normalizedText(raw));
    TextStylePatch size;
    size.sizePixels = 20;
    CHECK(fixture.edit(formatTextRange(fixture.content(), 0, 3, size)));

    // Input spans leave default-style gaps: the limit applies to canonical
    // output, not only the number of explicitly supplied spans.
    TextLayer fragmented;
    fragmented.utf8.assign(kMaximumTextRuns + 2, 'x');
    fragmented.defaultStyle = style(9);
    for (std::size_t i = 1; i < fragmented.utf8.size(); i += 2)
        fragmented.runs.push_back({ i, 1, style(20) });
    CHECK(fragmented.runs.size() < kMaximumTextRuns);
    CHECK(throws<std::length_error>([&] { (void)normalizedText(fragmented); }));
}

void selectedPropertyPatchesAndAffinity()
{
    const auto small = style(9);
    auto large = style(20, { 220, 40, 80, 99 });
    large.font = { "Another missing family", "Oblique", 600, true };
    auto value = text("abcd\nef", small);
    value.runs = { { 0, 2, small }, { 2, 3, large }, { 5, 2, small } };
    value = normalizedText(value);
    CHECK(textStyleAt(value, 0) == small);
    CHECK(textStyleAt(value, 2) == small); // Preceding affinity at a mixed boundary.
    CHECK(textStyleAt(value, 2, false) == large);
    CHECK(textStyleAt(value, 3) == large);
    CHECK(textStyleAt(value, 5) == small); // Start after LF uses the new paragraph.
    CHECK(textStyleAt(value, value.utf8.size()) == small);
    CHECK(textStyleAt(text(""), 0) == small);

    TextStylePatch onlySize;
    onlySize.sizePixels = 37.5;
    const auto changed = formatTextRange(value, 1, 6, onlySize);
    CHECK(changed.utf8 == value.utf8);
    CHECK(changed.paragraphs == value.paragraphs);
    CHECK(changed.defaultStyle == value.defaultStyle);
    for (std::size_t i = 0; i < value.utf8.size(); ++i) {
        auto expected = textStyleAt(value, i, false);
        if (i >= 1 && i < 6)
            expected.sizePixels = 37.5;
        CHECK(textStyleAt(changed, i, false) == expected);
    }
    CHECK(formatTextRange(value, 2, 2, onlySize) == value);
    CHECK(formatTextRange(value, 0, value.utf8.size(), { }) == value);
    CHECK(throws<std::invalid_argument>([&] { (void)formatTextRange(value, 4, 2, onlySize); }));
    CHECK(throws<std::invalid_argument>([&] { (void)formatTextRange(value, 0, 100, onlySize); }));
    const auto unicode = text("a\xc3\xa9z");
    CHECK(throws<std::invalid_argument>([&] { (void)formatTextRange(unicode, 2, 3, onlySize); }));

    TextStylePatch onlyColor;
    onlyColor.color = Rgba8 { 1, 2, 3, 4 };
    const auto recolored = formatTextRange(value, 0, value.utf8.size(), onlyColor);
    for (std::size_t i = 0; i < value.utf8.size(); ++i) {
        auto expected = textStyleAt(value, i, false);
        expected.color = *onlyColor.color;
        CHECK(textStyleAt(recolored, i, false) == expected);
    }
    TextStylePatch bold;
    bold.weight = 700;
    const auto boldStyle = patchedTextStyle(large, bold);
    CHECK(boldStyle.font.weight == 700);
    CHECK(boldStyle.font.style.empty()); // Avoid QFont style-name overriding weight.
    CHECK(boldStyle.font.italic == large.font.italic);
    CHECK(boldStyle.sizePixels == large.sizePixels);
    CHECK(boldStyle.color == large.color);
    bold.style = "Explicit Bold Italic";
    CHECK(patchedTextStyle(large, bold).font.style == *bold.style);

    for (double invalid : { 0.0, 0.249, 2048.1, std::numeric_limits<double>::infinity() }) {
        TextStylePatch patch;
        patch.sizePixels = invalid;
        CHECK(throws<std::invalid_argument>([&] { (void)patchedTextStyle(small, patch); }));
    }
}

void insertionOnlyStateAndNineToTwenty()
{
    Fixture fixture(text("old", style(9)));
    const auto original = fixture.content();
    const auto revision = fixture.document.revision();
    const auto textRevision = fixture.document.layer(fixture.id)->textRevision;
    TextStylePatch size;
    size.sizePixels = 20;
    const auto insertion = patchedTextStyle(textStyleAt(original, 3), size);
    CHECK(insertion.sizePixels == 20);
    CHECK(fixture.content() == original);
    CHECK(fixture.document.revision() == revision);
    CHECK(fixture.document.layer(fixture.id)->textRevision == textRevision);
    CHECK(fixture.history.undoDepth() == 0);

    auto after = original;
    after.utf8 += "new";
    after.runs.push_back({ 3, 3, insertion });
    after = normalizedText(after);
    CHECK(fixture.edit(after, 51, 3, 6));
    CHECK(fixture.content().runs.size() == 2);
    CHECK(textStyleAt(fixture.content(), 0, false).sizePixels == 9);
    CHECK(textStyleAt(fixture.content(), 3, false).sizePixels == 20);
    CHECK(textStyleAt(fixture.content(), 3, false).font == original.defaultStyle.font);
    CHECK(textStyleAt(fixture.content(), 3, false).color == original.defaultStyle.color);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content() == original);
    CHECK(hintEquals(fixture.history.textEditHint(), fixture.id, 3, 3));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.content() == after);
    CHECK(hintEquals(fixture.history.textEditHint(), fixture.id, 6, 6));
}

void historyGroupingNoOpsAndRedo()
{
    Fixture fixture(text("a"));
    const auto original = fixture.content();
    CHECK(fixture.edit(text("ab"), 101, 1, 2));
    CHECK(fixture.edit(text("abc"), 101, 2, 3));
    CHECK(fixture.edit(text("abcd"), 101, 3, 4));
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content() == original);
    CHECK(hintEquals(fixture.history.textEditHint(), fixture.id, 1, 1));
    const auto revision = fixture.document.revision();
    const auto retained = fixture.history.memoryUsed();
    CHECK(!fixture.edit(fixture.content(), 999));
    CHECK(fixture.document.revision() == revision);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == retained);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.content().utf8 == "abcd");
    CHECK(hintEquals(fixture.history.textEditHint(), fixture.id, 4, 4));
    CHECK(fixture.edit(text("abcde"), 102, 4, 5)); // Caret/navigation starts a new key.
    CHECK(fixture.history.undoDepth() == 2);

    TextStylePatch size;
    size.sizePixels = 10;
    CHECK(fixture.edit(formatTextRange(fixture.content(), 0, 5, size), 200));
    size.sizePixels = 11;
    CHECK(fixture.edit(formatTextRange(fixture.content(), 0, 5, size), 200));
    size.sizePixels = 12;
    CHECK(fixture.edit(formatTextRange(fixture.content(), 0, 5, size), 200));
    CHECK(fixture.history.undoDepth() == 3); // One numeric adjustment, not three.
    CHECK(fixture.history.undo(fixture.document));
    CHECK(textStyleAt(fixture.content(), 1).sizePixels == 9);
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(!fixture.edit(formatTextRange(fixture.content(), 0, 0, size), 333));
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.edit(text("different"), 103));
    CHECK(fixture.history.redoDepth() == 0);
    CHECK(fixture.history.undoDepth() == 3);

    Fixture discrete(text("x"));
    CHECK(discrete.edit(text("xy"), 0));
    CHECK(discrete.edit(text("xyz"), 0));
    CHECK(discrete.history.undoDepth() == 2);
}

void creationCleanupAndHistoryConsequences()
{
    EditorSession session;
    auto doc = std::make_unique<Document>(CanvasSpec { .extent = { 128, 128 } });
    auto base = Layer::raster("Background", std::make_shared<ContiguousRasterSurface>(Extent2u { 1, 1 }));
    const auto baseId = base.id;
    CHECK(doc->insertLayer(0, std::move(base)));
    session.replaceDocument(std::move(doc));
    CHECK(session.activeLayer() == baseId);
    CHECK(session.execute(std::make_unique<SetLayerOpacityCommand>(baseId, 0.5F)));
    CHECK(session.undo());
    const auto revision = session.document()->revision();
    auto abandoned = Layer::text("Empty pending", text(""));
    const auto abandonedId = abandoned.id;
    CHECK(!session.execute(std::make_unique<TextEditCommand>(
        std::move(abandoned), 1, baseId, TextEditHint { abandonedId, 0, 0 }, 400)));
    CHECK(session.document()->layers().size() == 1);
    CHECK(session.document()->revision() == revision);
    CHECK(session.history().undoDepth() == 0);
    CHECK(session.history().redoDepth() == 1);

    auto created = Layer::text("New editable text", text("H"));
    const auto id = created.id;
    created.localToDocument = { 1, 0, 12.5, 0, 1, 27.25 };
    const auto geometry = created.localToDocument;
    CHECK(session.execute(
        std::make_unique<TextEditCommand>(std::move(created), 1, baseId, TextEditHint { id, 1, 1 }, 401)));
    CHECK(session.activeLayer() == id);
    CHECK(session.history().redoDepth() == 0);
    CHECK(session.history().undoDepth() == 1);
    CHECK(session.execute(std::make_unique<TextEditCommand>(
        id, text("H"), text("Hi"), TextEditHint { id, 1, 1 }, TextEditHint { id, 2, 2 }, 401)));
    CHECK(session.history().undoDepth() == 1);
    CHECK(session.document()->layer(id)->localToDocument == geometry);
    CHECK(session.undo());
    CHECK(!session.document()->containsLayer(id));
    CHECK(session.activeLayer() == baseId);
    CHECK(session.redo());
    CHECK(session.activeLayer() == id);
    CHECK(std::get<TextLayer>(session.document()->layer(id)->payload) == text("Hi"));
    CHECK(session.document()->layer(id)->localToDocument == geometry);
    CHECK(hintEquals(session.history().textEditHint(), id, 2, 2));
}

void targetGuardsRevisionsAndMemory()
{
    Fixture fixture(text("before"));
    const auto original = fixture.content();
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = std::make_shared<ContiguousRasterSurface>(Extent2u { 10, 10 });
    cache->logicalExtent = { 10, 10 };
    cache->contentRevision = fixture.document.layer(fixture.id)->textRevision;
    fixture.document.layer(fixture.id)->renderCache = cache;
    const auto contentRevision = fixture.document.layer(fixture.id)->textRevision;
    const auto documentRevision = fixture.document.revision();
    CHECK(!fixture.document.setLayerText(fixture.id, original));
    CHECK(fixture.document.layer(fixture.id)->renderCache == cache);
    CHECK(fixture.document.revision() == documentRevision);

    CHECK(fixture.edit(text("after"), 501));
    CHECK(fixture.document.layer(fixture.id)->textRevision == contentRevision + 1);
    CHECK(!fixture.document.layer(fixture.id)->renderCache);
    CHECK(fixture.document.revision() == documentRevision + 1);
    const auto retained = fixture.history.memoryUsed();
    CHECK(retained == fixture.history.latestUndoMemoryCost());
    CHECK(retained > sizeof(TextEditCommand));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.memoryUsed() == retained);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.memoryUsed() == retained);
    CHECK(fixture.document.layer(fixture.id)->textRevision == contentRevision + 3);

    auto removed = fixture.document.takeLayer(fixture.id);
    CHECK(removed.has_value());
    CHECK(!fixture.history.undo(fixture.document));
    CHECK(fixture.history.undoDepth() == 1);
    CHECK(fixture.history.memoryUsed() == retained);
    CHECK(fixture.document.insertLayer(removed->index, std::move(removed->layer)));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content() == original);

    auto mismatched = text("not the command baseline");
    CHECK(!fixture.history.execute(fixture.document,
        std::make_unique<TextEditCommand>(
            fixture.id, mismatched, text("replacement"), TextEditHint { }, TextEditHint { }, 502)));
    CHECK(fixture.history.redoDepth() == 1);
    CHECK(fixture.content() == original);

    auto large = text(std::string(1024, 'm'), style(9));
    large.defaultStyle.font.family.reserve(256);
    large.defaultStyle.font.style.reserve(64);
    large.runs.reserve(30);
    large.paragraphs.reserve(10);
    large.runs.front().style.font.family.reserve(128);
    std::size_t expected = large.utf8.capacity() + large.runs.capacity() * sizeof(TextFormatRun)
        + large.paragraphs.capacity() * sizeof(TextParagraph) + large.defaultStyle.font.family.capacity()
        + large.defaultStyle.font.style.capacity() + large.defaultStyle.font.originalFace.capacity();
    for (const auto& run : large.runs)
        expected += run.style.font.family.capacity() + run.style.font.style.capacity() + run.style.font.originalFace.capacity();
    CHECK(textMemoryCost(large) == expected);

    Fixture merged(text("start"));
    CHECK(merged.edit(text(std::string(1000, 'x')), 600));
    const auto firstCost = merged.history.memoryUsed();
    CHECK(merged.edit(text(std::string(3000, 'x')), 600));
    CHECK(merged.history.undoDepth() == 1);
    CHECK(merged.history.memoryUsed() > firstCost);
    CHECK(merged.history.memoryUsed() == merged.history.latestUndoMemoryCost());
    merged.history.clear();
    CHECK(merged.history.memoryUsed() == 0);
    CHECK(!merged.history.textEditHint());
}

std::shared_ptr<const LayerRenderCache> makeCache(
    Extent2u logical, unsigned density, Revision revision, Rgba8 color = { })
{
    auto cache = std::make_shared<LayerRenderCache>();
    cache->surface = std::make_shared<ContiguousRasterSurface>(
        Extent2u { logical.width * density, logical.height * density }, color);
    cache->logicalExtent = logical;
    cache->density = density;
    cache->contentRevision = revision;
    cache->pixelsToLocal = { 1.0 / density, 0, 0, 0, 1.0 / density, 0 };
    return cache;
}

void textGeometryAndDensityIndependentHistory()
{
    Fixture fixture(text("O  \nTitle", style(9)));
    TextStylePatch title;
    title.sizePixels = 20;
    CHECK(fixture.document.setLayerText(fixture.id, formatTextRange(fixture.content(), 4, 9, title)));
    auto* layer = fixture.document.layer(fixture.id);
    layer->renderCache = makeCache({ 80, 40 }, 1, layer->textRevision);
    const auto content = fixture.content();
    const auto contentRevision = layer->textRevision;
    const auto original = layer->localToDocument;
    const auto cache = layer->renderCache;
    LayerTransformSession transform(fixture.document, fixture.id);
    CHECK(transform.active());
    CHECK(transform.extent() == Extent2u({ 80, 40 }));
    CHECK(transform.beginDrag(TransformHandle::Move, { 10, 10 }));
    CHECK(transform.dragTo({ 15, 18 }, { }, false));
    transform.endDrag();
    CHECK(transform.flip(true));
    auto values = transform.values();
    values.scaleY = 1.5;
    values.rotationDegrees = 30;
    CHECK(transform.setValues(values));
    CHECK(transform.completeAction());
    const auto geometry = transform.transform();
    CHECK(fixture.content() == content);
    CHECK(layer->textRevision == contentRevision);
    CHECK(layer->renderCache == cache);

    const auto documentRevision = fixture.document.revision();
    layer->renderCache = makeCache({ 80, 40 }, 4, layer->textRevision);
    CHECK(transform.targetAvailable());
    CHECK(transform.extent() == Extent2u({ 80, 40 }));
    CHECK(fixture.document.revision() == documentRevision);
    CHECK(layerGeometryExtent(*layer) == Extent2u({ 80, 40 }));
    const auto pixelPoint = renderTransform(*layer).map({ 40, 80 });
    const auto logicalPoint = layer->localToDocument.map({ 10, 20 });
    CHECK(std::hypot(pixelPoint.x - logicalPoint.x, pixelPoint.y - logicalPoint.y) < 1.0e-10);
    CHECK(transform.commit(fixture.history) == TransformCommitResult::Committed);
    CHECK(fixture.history.undoDepth() == 3);

    auto edited = content;
    edited.utf8 += "!";
    edited = normalizedText(edited);
    CHECK(fixture.edit(edited, 701));
    CHECK(layer->textRevision == contentRevision + 1);
    CHECK(!layer->renderCache);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content() == content);
    CHECK(layer->textRevision == contentRevision + 2);
    CHECK(fixture.history.undo(fixture.document)); // Must not guard old text revision or cache ID.
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.history.undo(fixture.document));
    CHECK(layer->localToDocument == original);
    CHECK(fixture.content() == content);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(layer->localToDocument == geometry);
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.content() == edited);
}

void preparedTextBoundsAndSampling()
{
    Fixture fixture(text("O   O"));
    auto* layer = fixture.document.layer(fixture.id);
    layer->renderCache = makeCache({ 20, 10 }, 2, layer->textRevision);
    layer->localToDocument = { 0, -2, 50, -1, 0, 40 }; // Rotate, stretch, flip.
    const auto interior = layer->localToDocument.map({ 8, 4 });
    CHECK(hitTextBounds(*layer, interior)); // All raster pixels are transparent: use layout, not ink.
    CHECK(hitTestRasterLayer(fixture.document, interior) == fixture.id);
    CHECK(!hitTextBounds(*layer, layer->localToDocument.map({ 20.1, 4 })));

    auto top = Layer::text("Higher overlapping text", text(" "));
    const auto topId = top.id;
    top.localToDocument = layer->localToDocument;
    top.renderCache = makeCache({ 20, 10 }, 1, top.textRevision);
    CHECK(fixture.document.insertLayer(1, std::move(top)));
    CHECK(hitTestRasterLayer(fixture.document, interior) == topId);
    CHECK(fixture.document.setLayerVisibility(topId, false));
    CHECK(hitTestRasterLayer(fixture.document, interior) == fixture.id);
    CHECK(fixture.document.setLayerVisibility(topId, true));
    CHECK(fixture.document.setLayerOpacity(topId, 0));
    CHECK(hitTestRasterLayer(fixture.document, interior) == fixture.id);

    layer = fixture.document.layer(fixture.id); // Vector insertion invalidates pointers.
    layer->renderCache = makeCache({ 20, 10 }, 3, layer->textRevision, { 90, 140, 200, 128 });
    CHECK(fixture.document.setLayerOpacity(fixture.id, 0.5F));
    const auto result
        = sampleDocumentColor(fixture.document, fixture.id, interior, ColorSampleSource::MergedVisible);
    CHECK(result.available());
    CHECK(result.color == Rgba8({ 90, 140, 200, 64 }));
    CHECK(result.texelsRead <= 4);
    const PinnedDocumentSampler sampler(fixture.document, fixture.id, ColorSampleSource::MergedVisible);
    CHECK(sampler.matches(fixture.document));
    CHECK(sampler.sample(interior) == result.color);
    const auto snapshot = fixture.document.snapshot();
    CHECK(renderedSurface(snapshot.layersBottomToTop.front()) == layer->renderCache->surface);
    CHECK(renderTransform(snapshot.layersBottomToTop.front()) == renderTransform(*layer));
}

void netZeroTypingAndEmptyCreationRemainTraversable()
{
    Fixture fixture(text("a"));
    CHECK(fixture.edit(text("ab"), 900));
    CHECK(fixture.edit(text("a"), 900));
    CHECK(fixture.history.undoDepth() == 2);
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content().utf8 == "ab");
    CHECK(fixture.history.undo(fixture.document));
    CHECK(fixture.content().utf8 == "a");
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.history.redo(fixture.document));
    CHECK(fixture.content().utf8 == "a");

    Document doc(CanvasSpec { .extent = { 10, 10 } });
    History history;
    auto layer = Layer::text("Temporary content", text("H"));
    const auto id = layer.id;
    CHECK(history.execute(doc,
        std::make_unique<TextEditCommand>(
            std::move(layer), 0, std::nullopt, TextEditHint { id, 1, 1 }, 901)));
    CHECK(history.execute(doc,
        std::make_unique<TextEditCommand>(
            id, text("H"), text(""), TextEditHint { id, 1, 1 }, TextEditHint { id, 0, 0 }, 901)));
    CHECK(history.undoDepth() == 2);
    CHECK(history.undo(doc));
    CHECK(std::get<TextLayer>(doc.layer(id)->payload).utf8 == "H");
    CHECK(history.undo(doc));
    CHECK(!doc.containsLayer(id));
    CHECK(history.redo(doc));
    CHECK(std::get<TextLayer>(doc.layer(id)->payload).utf8 == "H");
    CHECK(history.redo(doc));
    CHECK(std::get<TextLayer>(doc.layer(id)->payload).utf8.empty());
}
}

int main()
{
    utf8Utf16ScalarMapping();
    fixedSeedUnicodeAndInvalidInput();
    normalizedRunsAndParagraphs();
    canonicalAdmissionAndRunBounds();
    selectedPropertyPatchesAndAffinity();
    insertionOnlyStateAndNineToTwenty();
    historyGroupingNoOpsAndRedo();
    creationCleanupAndHistoryConsequences();
    targetGuardsRevisionsAndMemory();
    textGeometryAndDensityIndependentHistory();
    preparedTextBoundsAndSampling();
    netZeroTypingAndEmptyCreationRemainTraversable();
    if (failures)
        std::cerr << failures << " rich-text checks failed\n";
    else
        std::cout << "Rich-text model, Unicode, formatting and history checks passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
