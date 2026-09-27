#include "imageeditor/ui/QtTextLayoutService.hpp"

#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QRawFont>
#include <QTextBlock>
#include <QTextLayout>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {

int failures = 0;

void check(bool condition, std::string_view message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void near(double actual, double expected, double tolerance, std::string_view message)
{
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        std::cerr << "  actual=" << actual << ", expected=" << expected << ", tolerance=" << tolerance << '\n';
}

QString fixturePath(const char* name)
{
    return QStringLiteral(IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR) + QLatin1Char('/') + QString::fromLatin1(name);
}

std::string bytes(const char8_t* text) { return reinterpret_cast<const char*>(text); }

c::TextStyle regular(double size = 24)
{
    c::TextStyle style;
    style.font = { "Noto Sans", "Regular", 400, false };
    style.sizePixels = size;
    style.color = { 179, 71, 93, 128 };
    return style;
}

c::TextLayer text(std::string contents, double size = 24)
{
    c::TextLayer result;
    result.utf8 = std::move(contents);
    result.defaultStyle = regular(size);
    return c::normalizedText(std::move(result));
}

std::vector<std::byte> pixels(const c::RasterSurface& surface)
{
    const auto e = surface.extent();
    const auto stride = static_cast<std::size_t>(e.width) * 4;
    std::vector<std::byte> result(stride * e.height);
    surface.copyRgba8(
        { 0, 0, static_cast<std::int32_t>(e.width), static_cast<std::int32_t>(e.height) }, result, stride);
    return result;
}

void loadFixtures()
{
    struct Fixture {
        const char* file;
        const char* family;
        const char* hash;
    };
    for (const auto& fixture : { Fixture { "NotoSans-Regular.ttf", "Noto Sans",
                                     "b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5" },
             Fixture { "NotoSansArabic-Regular.ttf", "Noto Sans Arabic",
                 "ceea25b464a656dc3b26849bab9356740401af62aedf1bfa8b7f0d9b75925b1b" } }) {
        QFile file(fixturePath(fixture.file));
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error("Missing pinned font fixture");
        check(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex() == fixture.hash,
            "font fixture has pinned SHA256");
        const int id = QFontDatabase::addApplicationFont(file.fileName());
        if (id < 0)
            throw std::runtime_error("Pinned application font failed to load");
        check(
            QFontDatabase::applicationFontFamilies(id) == QStringList { QString::fromLatin1(fixture.family) },
            "loaded fixture has expected family");
    }
    u::QtTextLayout layout(text(bytes(u8"O e\u0301 ffi Ελληνικά Кириллица"), 32));
    (void)layout.document().documentLayout()->blockBoundingRect(layout.document().begin());
    const QRawFont reference(fixturePath("NotoSans-Regular.ttf"), 32, QFont::PreferNoHinting);
    const auto fixtureRuns
        = layout.document().begin().layout()->glyphRuns(0, layout.document().begin().length() - 1);
    check(!fixtureRuns.isEmpty(), "fixture verification has real shaped glyph runs");
    for (const auto& run : fixtureRuns) {
        for (const char* table : { "head", "cmap", "name" })
            check(run.rawFont().fontTable(table) == reference.fontTable(table),
                "shaped glyph run uses exact fixture, not same-name system font");
        check(std::ranges::none_of(run.glyphIndexes(), [](auto glyph) { return glyph == 0; }),
            "Latin/combining/Greek/Cyrillic sample has no missing glyphs");
    }
}

void roundTrip()
{
    auto source = text(bytes(u8"A\u00a0e\u0301\nO ffi\n\nend\n"), 9.5);
    c::TextStylePatch coloredSize;
    coloredSize.sizePixels = 20.25;
    coloredSize.color = c::Rgba8 { 4, 90, 180, 76 };
    source = c::formatTextRange(source, 1, 3, coloredSize);
    const auto second = source.utf8.find('O');
    c::TextStylePatch missingFont;
    missingFont.family = "ImageEditor deliberately missing requested font 9081";
    missingFont.style = "Requested Custom Style";
    missingFont.weight = 635;
    missingFont.italic = true;
    source = c::formatTextRange(source, second, second + 1, missingFont);
    source.paragraphs[0].alignment = c::TextAlignment::Right;
    source.paragraphs[1].alignment = c::TextAlignment::Center;
    source.paragraphs[2].alignment = c::TextAlignment::Left;
    source.paragraphs.back().alignment = c::TextAlignment::Center;
    u::QtTextLayout layout(source);
    check(layout.text() == source,
        "UTF8, NBSP, combining text, LF, empty paragraphs, formats and alignments round-trip");
    check(!layout.document().isUndoRedoEnabled(), "Qt undo stack remains disabled");
    layout.reset(layout.text());
    check(layout.text() == source,
        "second canonical round-trip does not drift or resolve requested descriptors");

    const auto unchanged = layout.text();
    const auto cache = layout.rasterize(2);
    check(cache && cache->surface && layout.text() == unchanged,
        "fallback rendering does not mutate canonical text");

    auto fallback = text("Fallback");
    fallback.defaultStyle.font = { "ImageEditor missing family fixture 6349", "Requested Face", 617, true };
    fallback = c::normalizedText(std::move(fallback));
    u::QtTextLayout missing(fallback);
    u::QtTextLayoutService service;
    const auto result = service.layout({ fallback, 1 });
    check(result.cache && !result.resolvedFontFamily.empty(),
        "service renders a fallback and reports resolved family");
    check(missing.text() == fallback, "missing family/style/weight/italic request remains canonical");

    u::QtTextLayout edited(text("A"));
    QTextCursor cursor(&edited.document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QString::fromUtf8("\xC2\xA0\nB"), u::QtTextLayout::format(regular(11.75)));
    check(edited.text().utf8 == bytes(u8"A\u00a0\nB"),
        "Qt edits preserve NBSP rather than toPlainText normalization");
}

void newlinesCarryFormatting()
{
    // A newline is a canonical character too, even though Qt stores it on a block.
    auto source = text("a\nb\n\n");
    c::TextStylePatch size;
    size.sizePixels = 41.5;
    source = c::formatTextRange(source, 1, 2, size);
    c::TextStylePatch color;
    color.color = c::Rgba8 { 12, 233, 81, 70 };
    source = c::formatTextRange(source, 3, 5, color);
    u::QtTextLayout layout(source);
    check(layout.text() == source, "newline-only runs and consecutive empty paragraph formatting round-trip");
    if (layout.text() != source) {
        for (const auto& run : layout.text().runs)
            std::cerr << "  actual run " << run.start << '+' << run.length << " size=" << run.style.sizePixels
                      << " red=" << int(run.style.color.red) << '\n';
    }
}

void insertionFormatting()
{
    auto source = text("old", 9);
    u::QtTextLayout layout(source);
    QTextCursor cursor(&layout.document());
    cursor.movePosition(QTextCursor::End);
    auto insertion = source.defaultStyle;
    insertion.sizePixels = 20;
    cursor.setCharFormat(u::QtTextLayout::format(insertion));
    check(layout.text() == source, "insertion style alone does not reformat existing text");
    cursor.insertText(QStringLiteral("new"));
    const auto after = layout.text();
    check(after.utf8 == "oldnew", "new text inserted at caret");
    check(c::textStyleAt(after, 0, false).sizePixels == 9 && c::textStyleAt(after, 2, false).sizePixels == 9
            && c::textStyleAt(after, 3, false).sizePixels == 20,
        "size-9 to size-20 typing affects only newly inserted characters");
    check(c::textStyleAt(after, 3, false).font == source.defaultStyle.font
            && c::textStyleAt(after, 3, false).color == source.defaultStyle.color,
        "insertion size preserves inherited font and color");

    cursor.setPosition(1);
    cursor.setPosition(2, QTextCursor::KeepAnchor);
    const auto beforeFormat = layout.text();
    const auto selected = u::QtTextLayout::style(cursor.charFormat(), source.defaultStyle);
    auto patch = selected;
    patch.color = { 210, 20, 99, 255 };
    cursor.setCharFormat(u::QtTextLayout::format(patch));
    const auto formatted = layout.text();
    c::TextStylePatch color;
    color.color = patch.color;
    check(formatted == c::formatTextRange(beforeFormat, 1, 2, color),
        "selected range color changes only requested characters/property");
}

void logicalGeometryAndCaches()
{
    for (const double size : { 0.5, 9.5, 20.25, 48.75 }) {
        u::QtTextLayout layout(text("O W\nspace  ", size));
        const auto original = layout.text();
        const auto bounds = layout.bounds();
        check(bounds.topLeft() == QPointF(0, 0), "canonical layer-local origin stays top-left");
        check(bounds.width() > 0 && bounds.height() > 0, "fractional document size has nonempty geometry");
        const auto requested = layout.document().begin().begin().fragment().charFormat();
        near(u::QtTextLayout::style(requested).sizePixels, size, 1e-12,
            "fractional em size preserves canonical precision");
        near(requested.fontPointSize(), size * 72.0 / 96.0, 1e-12,
            "document pixels convert to points against fixed 96-DPI metrics device");
        const auto runs
            = layout.document().begin().layout()->glyphRuns(0, layout.document().begin().length() - 1);
        check(!runs.isEmpty(), "fractional-size layout has shaped runs");
        for (const auto& run : runs)
            // Qt's font engine rounds the em to a native pixel at the fixed
            // metrics DPI; canonical fractional sizes retain full precision.
            near(run.rawFont().pixelSize(), size, .5,
                "shaped font em uses fixed document pixels with bounded native rounding");
        const auto caret = layout.caret(1);
        std::optional<c::Vec2d> origin;
        for (const double density : { .5, 1., 1.5, 2., 3. }) {
            const auto cache = layout.rasterize(density);
            check(layout.bounds() == bounds && layout.caret(1) == caret && layout.text() == original,
                "resolution/monitor-density rasterization leaves logical layout, caret and text unchanged");
            check(cache->logicalExtent
                    == c::Extent2u { static_cast<std::uint32_t>(std::ceil(bounds.width())),
                        static_cast<std::uint32_t>(std::ceil(bounds.height())) },
                "logical cache extent excludes raster padding");
            const auto cacheOrigin = cache->pixelsToLocal.map({ 0, 0 });
            if (origin)
                check(cacheOrigin == *origin, "cache density changes never shift image-local origin");
            origin = cacheOrigin;
            near(
                cache->pixelsToLocal.m00 * cache->density, 1, 1e-12, "cache x scale maps to document pixels");
            near(
                cache->pixelsToLocal.m11 * cache->density, 1, 1e-12, "cache y scale maps to document pixels");
            check(cacheOrigin.x <= -2 && cacheOrigin.y <= -2, "transparent cache preserves overhang padding");
            const auto extent = cache->surface->extent();
            check(extent.width <= 8192 && extent.height <= 8192
                    && static_cast<std::uint64_t>(extent.width) * extent.height <= 16 * 1024 * 1024 + 16384,
                "text-cache allocation bounded by dimensions and pixel budget");
        }
    }
    u::QtTextLayout size9(text("WWWW", 9.5)), size20(text("WWWW", 20.25));
    check(size20.bounds().width() > size9.bounds().width() * 1.9,
        "document font size affects geometry, not only raster density");
    const auto before = size9.bounds();
    size9.reset(text("WWWW\nchanged", 42));
    check(size9.bounds().topLeft() == before.topLeft(), "content/format growth preserves local origin");

    u::QtTextLayout image(text("OOO", 40));
    const auto a = image.rasterize(1.5), b = image.rasterize(1.5);
    const auto data = pixels(*a->surface);
    check(data == pixels(*b->surface), "identical font/layout/density produces deterministic pixels");
    bool anyInk = false, anyTransparent = false, anyPartial = false;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        const int alpha = std::to_integer<int>(data[i + 3]);
        anyTransparent |= alpha == 0;
        anyInk |= alpha > 0;
        anyPartial |= alpha > 0 && alpha < 128;
        check(alpha <= 128, "text color alpha applied once");
        if (alpha > 60) {
            check(std::abs(std::to_integer<int>(data[i]) - 179) <= 3,
                "transparent raster uses straight rather than premultiplied RGB");
        }
    }
    check(anyInk && anyTransparent && anyPartial,
        "transparent AA text cache contains glyphs and antialiased edges");

    u::QtTextLayout overhang(text("jj", 256));
    const auto overhangCache = overhang.rasterize(1);
    const auto cacheTopLeft = overhangCache->pixelsToLocal.map({ 0, 0 });
    const auto cacheBottomRight = overhangCache->pixelsToLocal.map(
        { double(overhangCache->surface->extent().width), double(overhangCache->surface->extent().height) });
    for (const auto& run : overhang.document().begin().layout()->glyphRuns(0, 2)) {
        const auto glyphs = run.glyphIndexes();
        const auto positions = run.positions();
        for (qsizetype i = 0; i < glyphs.size(); ++i) {
            const auto ink = run.rawFont().boundingRect(glyphs[i]).translated(positions[i]);
            check(ink.left() >= cacheTopLeft.x && ink.right() <= cacheBottomRight.x
                    && ink.top() >= cacheTopLeft.y && ink.bottom() <= cacheBottomRight.y,
                "cache includes actual large glyph overhang without shifting canonical origin");
        }
    }

    u::QtTextLayout large(text(std::string(2048, 'W'), 128));
    const auto bounded = large.rasterize(8);
    check(bounded->surface->extent().width <= 8192 && bounded->surface->extent().height <= 8192,
        "long text requesting high resolution is downsampled into bounded cache");
    check(large.text().utf8.size() == 2048 && large.text().defaultStyle.sizePixels == 128,
        "cache budget never truncates or resizes canonical content");
}

void boundsHitTesting()
{
    u::QtTextLayout layout(text("O O", 80));
    const auto left = layout.caret(0), right = layout.caret(1);
    const c::Vec2d hole { (left.x() + right.x()) / 2, left.y() + left.height() * .5 };
    check(layout.hit(hole) >= 0 && layout.hit(hole) <= 1, "inside O targets text by layout, not alpha");
    const auto spaceStart = layout.caret(1), spaceEnd = layout.caret(2);
    const c::Vec2d space { (spaceStart.x() + spaceEnd.x()) / 2, spaceStart.center().y() };
    check(
        layout.hit(space) >= 1 && layout.hit(space) <= 2, "inter-letter whitespace is a valid cursor target");
    const auto cache = layout.rasterize(1);
    const auto localToPixels = cache->pixelsToLocal.inverted();
    check(localToPixels.has_value(), "cache mapping invertible");
    if (localToPixels) {
        const auto p = localToPixels->map(space);
        const auto all = pixels(*cache->surface);
        const auto extent = cache->surface->extent();
        const auto x = static_cast<std::uint32_t>(std::clamp(p.x, 0., double(extent.width - 1)));
        const auto y = static_cast<std::uint32_t>(std::clamp(p.y, 0., double(extent.height - 1)));
        check(all[(static_cast<std::size_t>(y) * extent.width + x) * 4 + 3] == std::byte { 0 },
            "whitespace hit remains valid where cache alpha is zero");
    }
    check(
        layout.hit({ -100, -100 }) >= 0 && layout.hit({ 1e5, 1e5 }) <= layout.document().characterCount() - 1,
        "fuzzy hit safely clamps caret positions outside bounds");
    check(layout.selection(1, 1).empty(), "collapsed character selection has no highlight");
}

void bidiSelectionGeometry()
{
    const auto arabic = bytes(u8"سَلَام");
    auto source = text("abc " + arabic + " xyz", 30);
    c::TextStylePatch family;
    family.family = "Noto Sans Arabic";
    source = c::formatTextRange(source, 4, 4 + arabic.size(), family);
    u::QtTextLayout layout(source);
    (void)layout.document().documentLayout()->blockBoundingRect(layout.document().begin());
    const QRawFont reference(fixturePath("NotoSansArabic-Regular.ttf"), 30, QFont::PreferNoHinting);
    bool foundArabic = false;
    for (const auto& run :
        layout.document().begin().layout()->glyphRuns(0, layout.document().begin().length() - 1)) {
        if (run.isRightToLeft()) {
            foundArabic = true;
            check(run.rawFont().fontTable("head") == reference.fontTable("head"),
                "Arabic shaping uses pinned Arabic font");
            check(std::ranges::none_of(run.glyphIndexes(), [](auto glyph) { return glyph == 0; }),
                "Arabic marks/joining contain no missing glyphs");
        }
    }
    check(foundArabic, "mixed script paragraph produces RTL shaped run");
    const c::Utf8TextIndex index(source.utf8);
    const int end = static_cast<int>(index.utf16Offset(4 + arabic.size()));
    const auto selected = layout.selection(2, end);
    check(selected.size() >= 2, "mixed-direction selection retains distinct visual run highlights");
    check(selected == layout.selection(end, 2), "selection rectangles independent of anchor direction");
    const auto bounds = layout.bounds();
    for (const auto& rect : selected) {
        check(std::isfinite(rect.x()) && std::isfinite(rect.y()) && rect.width() > 0 && rect.height() > 0,
            "bidi highlights have finite positive geometry");
        check(rect.top() >= bounds.top() - 1 && rect.bottom() <= bounds.bottom() + 1,
            "bidi highlights remain at their line height");
    }
    check(layout.text() == source, "hit/highlight queries do not alter rich text");
}

void graphemeNavigationAndBridge()
{
    for (const auto& cluster : { bytes(u8"e\u0301"), bytes(u8"👩‍👩‍👧‍👦"), bytes(u8"👍🏽"),
             bytes(u8"🇬🇧"), bytes(u8"😀") }) {
        auto source = text("x" + cluster);
        u::QtTextLayout layout(source);
        const c::Utf8TextIndex original(source.utf8);
        const auto qt = QString::fromUtf8(source.utf8.data(), static_cast<qsizetype>(source.utf8.size()));
        check(original.utf16Length() == static_cast<std::size_t>(qt.size()),
            "UTF8 bridge agrees with Qt UTF16 length");
        QTextCursor cursor(&layout.document());
        cursor.movePosition(QTextCursor::End);
        check(cursor.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor),
            "previous-character navigation finds final grapheme");
        check(cursor.selectedText().toUtf8().toStdString() == cluster,
            "combining/ZWJ/skin-tone/flag/surrogate grapheme stays intact");
        check(original.byteOffset(static_cast<std::size_t>(cursor.selectionStart())) == 1
                && original.byteOffset(static_cast<std::size_t>(cursor.selectionEnd())) == source.utf8.size(),
            "Qt selection maps to complete canonical UTF8 range");
        cursor.removeSelectedText();
        check(layout.text().utf8 == "x", "grapheme-safe removal never leaves broken codepoints or marks");
    }
    const auto source = text(bytes(u8"a😀e\u0301z"));
    u::QtTextLayout layout(source);
    const c::Utf8TextIndex index(source.utf8);
    for (std::size_t offset = 0; offset <= source.utf8.size(); ++offset) {
        if (!index.isBoundary(offset))
            continue;
        const auto qtOffset = index.utf16Offset(offset);
        check(index.byteOffset(qtOffset) == offset,
            "canonical Unicode boundary round-trips through Qt positions");
        const auto caret = layout.caret(static_cast<int>(qtOffset));
        check(std::isfinite(caret.x()) && std::isfinite(caret.y()), "Unicode caret geometry finite");
    }
}

void documentGridRasterization()
{
    auto source = text(bytes(u8"jO e\u0301\nffi"), 25.5);
    c::TextStylePatch patch;
    patch.color = c::Rgba8 { 29, 187, 61, 47 };
    source = c::formatTextRange(source, 1, 3, patch);
    u::QtTextLayout layout(source);
    const auto canonicalBounds = layout.bounds();
    for (const c::AffineTransform transform : { c::AffineTransform {},
             { 0, -1.3, 100.25, .7, 0, -9.5 }, { -1.1, .4, 80.125, .3, 1.7, -20.25 } }) {
        const auto full = layout.rasterizeDocument(transform, 65536);
        const auto extent = full->surface->extent();
        const auto origin = full->documentOrigin;
        check(full->rasterizedDocumentTransform == transform
                && origin.x == std::floor(origin.x) && origin.y == std::floor(origin.y),
            "text raster retains affine identity and an integer document-grid origin");
        const auto mapped = transform.map(full->pixelsToLocal.map({ 3.5, 5.5 }));
        near(mapped.x, origin.x + 3.5, 1e-10, "direct text x mapping keeps the model's canonical origin");
        near(mapped.y, origin.y + 5.5, 1e-10, "direct text y mapping keeps the model's canonical origin");

        // Independent Qt paint+format-conversion reference verifies that the
        // direct output's in-place unpremultiply keeps Qt's color/alpha contract.
        QImage reference(int(extent.width), int(extent.height), QImage::Format_ARGB32_Premultiplied);
        reference.setDotsPerMeterX(3780);
        reference.setDotsPerMeterY(3780);
        reference.fill(Qt::transparent);
        {
            QPainter painter(&reference);
            painter.setRenderHint(QPainter::TextAntialiasing);
            painter.setWorldTransform(QTransform(transform.m00, transform.m10, transform.m01,
                transform.m11, transform.m02 - origin.x, transform.m12 - origin.y));
            QAbstractTextDocumentLayout::PaintContext context;
            layout.document().documentLayout()->draw(&painter, context);
        }
        reference = reference.convertToFormat(QImage::Format_RGBA8888);
        const auto actual = pixels(*full->surface);
        bool same = true;
        for (std::uint32_t y = 0; y < extent.height; ++y)
            for (std::uint32_t x = 0; x < extent.width * 4; ++x)
                same &= actual[(std::size_t(y) * extent.width * 4) + x]
                    == std::byte(reference.constScanLine(int(y))[x]);
        check(same, "transformed rich text paints directly from the model with exact Qt RGBA conversion");

        const c::RectI clip { int(origin.x) + 3, int(origin.y) + 4,
            int(extent.width) - 7, int(extent.height) - 8 };
        const auto clipped = layout.rasterizeDocument(transform,
            std::size_t(clip.width) * std::size_t(clip.height), clip);
        const auto clippedBytes = pixels(*clipped->surface);
        check(clipped->surface->extent() == c::Extent2u { std::uint32_t(clip.width), std::uint32_t(clip.height) },
            "text document clip bounds allocation before painting");
        same = true;
        for (int y = 0; y < clip.height; ++y)
            for (int x = 0; x < clip.width * 4; ++x)
                same &= clippedBytes[std::size_t(y) * std::size_t(clip.width) * 4 + std::size_t(x)]
                    == actual[std::size_t(y + 4) * extent.width * 4 + std::size_t(x + 12)];
        check(same, "clipped text preserves exact document-grid glyph coverage and colors");
        bool rejected = false;
        try {
            (void)layout.rasterizeDocument(transform, std::size_t(extent.width) * extent.height - 1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "text exact output fails rather than silently reducing requested resolution");
    }
    check(layout.bounds() == canonicalBounds && layout.text() == source,
        "document rasterization leaves text model, layout dimensions and origin unchanged");
    u::QtTextLayout whitespace(text("O" + std::string(4096, ' '), 24));
    const auto tight = whitespace.rasterizeDocument({}, 4096);
    check(tight->surface->extent().width < 40,
        "document text allocation follows glyph ink rather than unpainted trailing layout whitespace");
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const std::pair<const char*, std::function<void()>> groups[] = { { "pinned fixtures", loadFixtures },
        { "canonical round-trip", roundTrip }, { "newline styles", newlinesCarryFormatting },
        { "insertion formatting", insertionFormatting },
        { "logical geometry/cache", logicalGeometryAndCaches }, { "layout bounds hit", boundsHitTesting },
        { "document-grid rasterization", documentGridRasterization },
        { "bidi highlight", bidiSelectionGeometry }, { "graphemes and UTF8", graphemeNavigationAndBridge } };
    for (const auto& [name, run] : groups) {
        const int before = failures;
        try {
            run();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
        std::cout << name << ": " << (before == failures ? "PASS" : "FAIL") << '\n';
    }
    std::cout << "Qt text layout tests: " << failures << " failures\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
