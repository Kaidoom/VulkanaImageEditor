#include "imageeditor/core/BlendCompositing.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectionCommands.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"

#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImageReader>
#include <QSettings>
#include <QProcess>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)

unsigned byte(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<unsigned char>(bytes.at(offset));
}
quint32 be32(const QByteArray& bytes, qsizetype offset)
{
    return byte(bytes, offset) * 16777216U + byte(bytes, offset + 1) * 65536U + byte(bytes, offset + 2) * 256U
        + byte(bytes, offset + 3);
}
quint32 le32(const QByteArray& bytes, qsizetype offset)
{
    return byte(bytes, offset) + byte(bytes, offset + 1) * 256U + byte(bytes, offset + 2) * 65536U
        + byte(bytes, offset + 3) * 16777216U;
}
unsigned be16(const QByteArray& bytes, qsizetype offset)
{
    return byte(bytes, offset) * 256U + byte(bytes, offset + 1);
}

// These parsers inspect the emitted container, independently of Qt's decoded
// image. In particular, a successful decode does not prove lossless WebP/444
// JPEG.
std::map<QByteArray, QByteArray> pngChunks(const QByteArray& bytes)
{
    std::map<QByteArray, QByteArray> result;
    CHECK(bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
    for (qsizetype p = 8; p + 12 <= bytes.size();) {
        const auto count = be32(bytes, p);
        if (quint64(count) + 12 > quint64(bytes.size() - p)) {
            CHECK(false);
            break;
        }
        result[bytes.mid(p + 4, 4)] = bytes.mid(p + 8, count);
        p += qsizetype(count) + 12;
    }
    return result;
}
std::map<QByteArray, QByteArray> webpChunks(const QByteArray& bytes)
{
    std::map<QByteArray, QByteArray> result;
    CHECK(bytes.size() >= 12 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WEBP");
    if (bytes.size() < 12)
        return result;
    CHECK(quint64(le32(bytes, 4)) + 8 == quint64(bytes.size()));
    for (qsizetype p = 12; p + 8 <= bytes.size();) {
        const auto count = le32(bytes, p + 4);
        if (quint64(count) + 8 > quint64(bytes.size() - p)) {
            CHECK(false);
            break;
        }
        result[bytes.mid(p, 4)] = bytes.mid(p + 8, count);
        p += qsizetype(count) + 8 + (count & 1U);
    }
    return result;
}
struct JpegHeaders {
    QSize size;
    std::vector<unsigned> sampling;
    QByteArray icc;
    unsigned dpiX { }, dpiY { };
    bool exif { false };
};
JpegHeaders jpegHeaders(const QByteArray& bytes)
{
    JpegHeaders result;
    CHECK(bytes.size() >= 4 && bytes.startsWith(QByteArray::fromHex("ffd8")));
    for (qsizetype p = 2; p + 4 <= bytes.size();) {
        if (byte(bytes, p++) != 255) {
            CHECK(false);
            break;
        }
        while (p < bytes.size() && byte(bytes, p) == 255)
            ++p;
        if (p >= bytes.size())
            break;
        const auto marker = byte(bytes, p++);
        if (marker == 0xda || marker == 0xd9)
            break;
        if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (p + 2 > bytes.size()) {
            CHECK(false);
            break;
        }
        const auto count = be16(bytes, p);
        if (count < 2 || count > unsigned(bytes.size() - p)) {
            CHECK(false);
            break;
        }
        const auto data = bytes.mid(p + 2, count - 2);
        if ((marker == 0xc0 || marker == 0xc2) && data.size() >= 6) {
            CHECK(byte(data, 0) == 8);
            result.size = QSize(int(be16(data, 3)), int(be16(data, 1)));
            const auto components = byte(data, 5);
            CHECK(components == 3 && data.size() == qsizetype(6 + 3 * components));
            if (data.size() >= qsizetype(6 + 3 * components))
                for (unsigned i = 0; i < components; ++i)
                    result.sampling.push_back(byte(data, 7 + 3 * i));
        }
        if (marker == 0xe0 && data.startsWith(QByteArray("JFIF\0", 5)) && data.size() >= 12) {
            CHECK(byte(data, 7) == 1); // Physical dimensions expressed in dots/inch.
            result.dpiX = be16(data, 8);
            result.dpiY = be16(data, 10);
        }
        if (marker == 0xe1 && data.startsWith("Exif"))
            result.exif = true;
        if (marker == 0xe2 && data.startsWith(QByteArray("ICC_PROFILE\0", 12)))
            result.icc += data.mid(14);
        p += count;
    }
    return result;
}
c::Rgba8 pixel(const QImage& image, int x, int y)
{
    const auto color = image.pixelColor(x, y);
    return { std::uint8_t(color.red()), std::uint8_t(color.green()), std::uint8_t(color.blue()),
        std::uint8_t(color.alpha()) };
}
bool near(c::Rgba8 a, c::Rgba8 b, int tolerance = 1)
{
    return std::abs(int(a.red) - int(b.red)) <= tolerance
        && std::abs(int(a.green) - int(b.green)) <= tolerance
        && std::abs(int(a.blue) - int(b.blue)) <= tolerance
        && std::abs(int(a.alpha) - int(b.alpha)) <= tolerance;
}
bool equalPixels(const QImage& a, const QImage& b, int tolerance = 0)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!near(pixel(a, x, y), pixel(b, x, y), tolerance))
                return false;
    return true;
}
c::Layer raster(const char* name, c::Extent2u size, c::Rgba8 color)
{
    return c::Layer::raster(name, std::make_shared<c::ContiguousRasterSurface>(size, color));
}
u::ExportSettings settings(QSize size, u::ExportFormat format = u::ExportFormat::Png)
{
    u::ExportSettings value;
    value.size = size;
    value.format = format;
    return value;
}
QImage gradient(QSize size, bool transparency)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(x, y,
                QColor(x * 255 / (size.width() - 1), y * 255 / (size.height() - 1),
                    (x + y) * 255 / (size.width() + size.height() - 2),
                    transparency ? (x + 3 * y) % 256 : 255));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.setDotsPerMeterX(11811);
    image.setDotsPerMeterY(11811);
    return image;
}
void canonicalTransparentPixels(QImage& image)
{
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() == 0)
                image.setPixelColor(x, y, Qt::transparent);
}
void report(const u::FlattenedDocumentResult& result)
{
    if (!result)
        std::cerr << "Render: " << result.error.toStdString() << '\n';
}
void report(const u::EncodedExport& result)
{
    if (!result)
        std::cerr << "Encode: " << result.error.toStdString() << '\n';
}

void authoritativeMixedRenderingAndStateIsolation(const std::string& fontFamily)
{
    auto document = std::make_unique<c::Document>(c::CanvasSpec { { 96, 72 }, 300 });
    auto base = raster("Partial backdrop", { 96, 72 }, { 45, 90, 170, 177 });
    const auto baseId = base.id;
    CHECK(document->insertLayer(0, std::move(base)));
    c::ShapeLayer geometry;
    geometry.kind = c::ShapeKind::RoundedRectangle;
    geometry.size = { 38, 26 };
    geometry.cornerRadius = 5;
    geometry.fillColor = { 230, 38, 91, 173 };
    geometry.strokeEnabled = true;
    geometry.strokeWidth = 2.5;
    geometry.strokeColor = { 39, 220, 173, 191 };
    auto shape = c::Layer::shape("Editable shape", geometry);
    shape.localToDocument = { -1.3, .25, 64, .15, 1.2, 22 };
    shape.blendMode = c::BlendMode::SoftLight;
    shape.opacity = .63F;
    shape.crop = c::LayerCrop { 1, 1, 36, 24 };
    shape.crop->corners = { 6, 2, 5, 3 };
    auto adjustments = std::make_shared<c::AdjustmentStack>();
    auto& exposure = adjustments->items[std::size_t(c::AdjustmentType::Exposure)];
    exposure.enabled = true;
    exposure.parameters = c::ExposureParameters { .6 };
    exposure.mask = c::AdjustmentMask { c::SelectionMask::rectangle({ 96, 72 }, { 20, 25, 30, 25 }, 127),
        shape.localToDocument };
    shape.adjustments = adjustments;
    const auto shapeId = shape.id;
    CHECK(document->insertLayer(1, std::move(shape)));
    c::TextLayer rich;
    rich.utf8 = "Red O\nBlue";
    rich.defaultStyle.font = { fontFamily, "Regular", 400, false };
    rich.defaultStyle.sizePixels = 13.5;
    rich.defaultStyle.color = { 255, 20, 50, 201 };
    auto text = c::Layer::text("Editable text", rich);
    text.localToDocument = { 1.4, .12, 5, -.06, 1.1, 5 };
    text.blendMode = c::BlendMode::Screen;
    text.opacity = .75F;
    const auto textId = text.id;
    CHECK(document->insertLayer(2, std::move(text)));
    auto hidden = raster("Hidden by folder", { 96, 72 }, { 255, 0, 255, 255 });
    const auto hiddenId = hidden.id;
    CHECK(document->insertLayer(3, std::move(hidden)));
    const auto oldTree = document->tree();
    auto tree = oldTree;
    constexpr c::LayerId groupId = 400000001, folderId = 400000002, hiddenFolderId = 400000003;
    tree.roots = { baseId, folderId, hiddenFolderId };
    tree.containers = { { groupId, "Pass through group", c::ContainerKind::Group, c::ColorLabel::None,
                            { shapeId, textId } },
        { folderId, "Outer folder", c::ContainerKind::Folder, c::ColorLabel::None, { groupId } },
        { hiddenFolderId, "Hidden folder", c::ContainerKind::Folder, c::ColorLabel::None, { hiddenId },
            false } };
    CHECK(document->replaceStructure(oldTree, std::move(tree)));
    c::EditorSession session;
    session.replaceDocument(std::move(document));
    session.setActiveLayer(shapeId);
    session.setActiveTool(c::ToolId::Crop);
    auto& doc = *session.document();
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(baseId, .7F)));
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(baseId, .8F)));
    CHECK(session.undo());
    CHECK(session.execute(std::make_unique<c::SetSelectionCommand>(c::SelectionMask::filled({ 96, 72 }, 0))));
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(textId, .6F)));
    CHECK(session.undo());
    const auto redo = session.history().redoDepth(), undo = session.history().undoDepth(),
               memory = session.history().memoryUsed();
    const auto content = doc.contentState(), revision = doc.revision(),
               selectionRevision = doc.selectionRevision();
    const auto selected = doc.selection();
    const auto layerSelection = session.layerSelectionState();
    const auto expected = u::flattenDocument(doc);
    CHECK(expected);
    if (!expected)
        return;
    const auto result = u::renderExport(doc, settings({ 96, 72 }));
    report(result);
    CHECK(result);
    if (!result)
        return;
    CHECK(equalPixels(result.image, expected.image));
    CHECK(result.image.dotsPerMeterX() == 11811 && result.image.dotsPerMeterY() == 11811);
    CHECK(!doc.layer(shapeId)->renderCache && !doc.layer(textId)->renderCache);
    CHECK(std::get<c::ShapeLayer>(doc.layer(shapeId)->payload) == geometry);
    const auto first = result.image;
    auto stale = std::make_shared<c::LayerRenderCache>();
    stale->surface
        = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 1, 1 }, c::Rgba8 { 255, 0, 255, 255 });
    stale->pixelsToLocal = { 100, 0, -10, 0, 100, -10 };
    stale->density = .01;
    doc.layer(shapeId)->renderCache = stale;
    doc.layer(textId)->renderCache = stale;
    const auto cached = u::renderExport(doc, settings({ 96, 72 }));
    CHECK(cached && equalPixels(cached.image, first));
    CHECK(doc.layer(shapeId)->renderCache == stale && doc.layer(textId)->renderCache == stale);
    CHECK(doc.contentState() == content && doc.revision() == revision
        && doc.selectionRevision() == selectionRevision);
    CHECK(doc.selection() == selected && session.layerSelectionState() == layerSelection
        && session.activeTool() == c::ToolId::Crop);
    CHECK(session.history().undoDepth() == undo && session.history().redoDepth() == redo
        && session.history().memoryUsed() == memory);
}

void allBlendModesAndMatteIsAfterComposition()
{
    for (const auto mode : c::allBlendModes) {
        c::Document doc({ { 3, 2 }, 96 });
        const c::Rgba8 a { 43, 181, 221, 143 }, b { 209, 72, 119, 117 };
        CHECK(doc.insertLayer(0, raster("Base", { 3, 2 }, a)));
        auto upper = raster("Mode", { 3, 2 }, b);
        upper.opacity = .37F;
        upper.blendMode = mode;
        CHECK(doc.insertLayer(1, std::move(upper)));
        const auto linear = c::compositeLayer(c::decodeColor(a), c::decodeColor(b), .37F, mode,{1.5,1.5},doc.layers().back().blendSeed);
        const auto exported = u::renderExport(doc, settings({ 3, 2 }));
        CHECK(exported);
        if (!exported)
            continue;
        CHECK(near(pixel(exported.image, 1, 1), c::encodeColor(linear), 0));
        auto opaque = settings({ 3, 2 }, u::ExportFormat::Jpeg);
        opaque.jpegMatteColor = QColor(17, 73, 201);
        opaque.pngMatteColor = opaque.jpegMatteColor;
        const auto rendered = u::renderExport(doc, opaque);
        CHECK(rendered);
        if (!rendered)
            continue;
        const auto matte = c::decodeColor({ 17, 73, 201, 255 });
        const auto expected = c::encodeColor(c::compositeLayer(matte, linear, 1, c::BlendMode::Normal));
        CHECK(near(pixel(rendered.image, 1, 1), expected, 1));
        if (mode == c::BlendMode::Multiply) {
            const auto injected = c::encodeColor(
                c::compositeLayer(c::compositeLayer(matte, c::decodeColor(a), 1, c::BlendMode::Normal),
                    c::decodeColor(b), .37F, mode));
            CHECK(!near(expected, injected,
                3)); // Fixture distinguishes the forbidden matte-as-backdrop path.
            opaque.format = u::ExportFormat::Png;
            opaque.pngMatte = true;
            const auto pngMatte = u::renderExport(doc, opaque);
            CHECK(pngMatte && equalPixels(pngMatte.image, rendered.image));
            if (pngMatte) {
                std::atomic_bool cancelled { false };
                const auto png = u::encodeExport(pngMatte.image, opaque, cancelled);
                CHECK(png);
                if (png) {
                    const auto chunks = pngChunks(png.bytes);
                    CHECK(chunks.contains("IHDR") && byte(chunks.at("IHDR"), 8) == 8
                        && byte(chunks.at("IHDR"), 9) == 2); // Actual RGB8, not indexed/palette.
                }
            }
        }
    }
}

void resizeUsesLinearPremultipliedCoverageAndNativeOrientation()
{
    c::Document doc({ { 2, 2 }, 300 });
    std::vector<std::byte> bytes { std::byte { 255 }, std::byte { 0 }, std::byte { 0 }, std::byte { 255 },
        std::byte { 0 }, std::byte { 255 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 },
        std::byte { 0 }, std::byte { 255 }, std::byte { 128 }, std::byte { 255 }, std::byte { 255 },
        std::byte { 255 }, std::byte { 255 } };
    CHECK(doc.insertLayer(0,
        c::Layer::raster("Corners",
            std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 2, 2 }, std::move(bytes)))));
    const auto native = u::renderExport(doc, settings({ 2, 2 }));
    CHECK(native);
    if (!native)
        return;
    CHECK(pixel(native.image, 0, 0) == c::Rgba8 { 255, 0, 0, 255 });
    CHECK(pixel(native.image, 1, 0) == c::Rgba8 { });
    CHECK(pixel(native.image, 0, 1) == c::Rgba8 { 0, 0, 255, 128 });
    CHECK(pixel(native.image, 1, 1) == c::Rgba8 { 255, 255, 255, 255 });
    const auto small = u::renderExport(doc, settings({ 1, 1 }));
    CHECK(small);
    if (!small)
        return;
    const double alpha = (1 + 128.0 / 255 + 1) / 4;
    const c::Rgba8 expected { c::linearToSrgb(.5 / alpha), c::linearToSrgb(.25 / alpha),
        c::linearToSrgb((.25 + 128.0 / 255 / 4) / alpha), c::alphaToByte(alpha) };
    CHECK(near(pixel(small.image, 0, 0), expected, 1));
    CHECK(small.image.dotsPerMeterX() == native.image.dotsPerMeterX());
    const auto large = u::renderExport(doc, settings({ 12, 12 }));
    CHECK(large);
    if (!large)
        return;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 12; ++x) {
            const auto p = pixel(large.image, x, y);
            if (p.alpha == 0)
                CHECK(p == c::Rgba8 { });
            // Hidden green in the upper-right texel cannot contaminate its red edge.
            if (y < 3 && p.alpha > 0)
                CHECK(p.green == 0 && p.blue == 0);
        }
    // Identity/scaling are output-only: dimensions/PPI/matrices remain exact.
    CHECK(doc.canvas() == c::CanvasSpec { { 2, 2 }, 300 });
    CHECK(doc.layers().front().localToDocument == c::AffineTransform { });
}

void pngAndWebpContainersAndAlpha()
{
    std::atomic_bool cancel { false };
    auto image = gradient({ 128, 80 }, true);
    image.setText(QStringLiteral("GPSLatitude"), QStringLiteral("private-location-marker"));
    image.setText(QStringLiteral("Description"), QStringLiteral("private-project-path-marker"));
    auto expected = image;
    canonicalTransparentPixels(expected);
    auto options = settings(image.size());
    const auto png = u::encodeExport(image, options, cancel);
    report(png);
    CHECK(png);
    if (!png)
        return;
    CHECK(equalPixels(png.decoded, expected));
    CHECK(equalPixels(QImage::fromData(png.bytes, "PNG"), expected));
    const auto chunks = pngChunks(png.bytes);
    CHECK(chunks.contains("IHDR"));
    if (chunks.contains("IHDR")) {
        CHECK(byte(chunks.at("IHDR"), 8) == 8);
        CHECK(byte(chunks.at("IHDR"), 9) == 6);
    }
    CHECK(chunks.contains("iCCP") || chunks.contains("sRGB"));
    CHECK(!chunks.contains("eXIf") && !chunks.contains("tEXt") && !chunks.contains("iTXt")
        && !chunks.contains("zTXt"));
    CHECK(png.decoded.colorSpace().isValid());
    CHECK(QImage::fromData(png.bytes, "PNG").colorSpace().isValid());
    CHECK(png.decoded.dotsPerMeterX() == 11811 && png.decoded.dotsPerMeterY() == 11811);
    options.format = u::ExportFormat::WebP;
    options.webpLossless = true;
    options.webpEffort = 3;
    const auto lossless = u::encodeExport(image, options, cancel);
    report(lossless);
    CHECK(lossless);
    if (!lossless)
        return;
    const auto losslessChunks = webpChunks(lossless.bytes);
    CHECK(losslessChunks.contains("VP8L") && !losslessChunks.contains("VP8 ")
        && losslessChunks.contains("ICCP"));
    if (losslessChunks.contains("ICCP"))
        CHECK(QColorSpace::fromIccProfile(losslessChunks.at("ICCP")).isValid());
    CHECK(!losslessChunks.contains("EXIF") && !losslessChunks.contains("XMP "));
    CHECK(equalPixels(lossless.decoded, expected));
    options.webpLossless = false;
    options.webpQuality = 85;
    const auto lossy = u::encodeExport(image, options, cancel);
    report(lossy);
    CHECK(lossy);
    if (!lossy)
        return;
    const auto lossyChunks = webpChunks(lossy.bytes);
    CHECK(lossyChunks.contains("VP8 ") && !lossyChunks.contains("VP8L") && lossyChunks.contains("ALPH")
        && lossyChunks.contains("ICCP"));
    double totalError = 0;
    std::size_t compared = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto a = pixel(expected, x, y), b = pixel(lossy.decoded, x, y);
            CHECK(a.alpha == b.alpha); // Independent of lossy color quality.
            // VP8 color is lossy even behind zero alpha; only alpha and visible
            // RGB are meaningful here. PNG and VP8L have strict canonical tests.
            if (a.alpha > 64) {
                totalError += std::abs(int(a.red) - b.red) + std::abs(int(a.green) - b.green)
                    + std::abs(int(a.blue) - b.blue);
                compared += 3;
            }
        }
    CHECK(compared > 0 && totalError / double(compared) < 8);
    // Lossy quality100 must stay explicitly lossy; it is not our Lossless switch.
    options.webpQuality = 100;
    const auto highestLossy = u::encodeExport(image, options, cancel);
    CHECK(highestLossy);
    if (highestLossy)
        CHECK(webpChunks(highestLossy.bytes).contains("VP8 "));
}

void jpegIsRgb444WithFreshMetadata()
{
    auto image = gradient({ 160, 96 }, false);
    // Thin saturated text-like strokes reveal chroma subsampling clearly.
    for (int y = 18; y < 78; ++y)
        for (int x = 22; x < 140; ++x)
            if ((x % 17) < 2 || (y % 19) < 2)
                image.setPixelColor(x, y, QColor(240, 15, 35));
    image.setText(QStringLiteral("GPSLatitude"), QStringLiteral("private-location-marker"));
    image.setText(QStringLiteral("Orientation"), QStringLiteral("6"));
    const auto options = settings(image.size(), u::ExportFormat::Jpeg);
    CHECK(options.jpegQuality == 92);
    std::atomic_bool cancel { false };
    const auto encoded = u::encodeExport(image, options, cancel);
    report(encoded);
    CHECK(encoded);
    if (!encoded)
        return;
    const auto headers = jpegHeaders(encoded.bytes);
    CHECK(headers.size == image.size());
    CHECK(headers.sampling == std::vector<unsigned>({ 0x11, 0x11, 0x11 }));
    CHECK(!headers.icc.isEmpty() && !headers.exif && headers.dpiX == 300 && headers.dpiY == 300);
    CHECK(QColorSpace::fromIccProfile(headers.icc).isValid());
    CHECK(QImage::fromData(encoded.bytes, "JPEG").colorSpace().isValid());
    CHECK(!encoded.bytes.contains("private-location-marker"));
    CHECK(encoded.decoded.colorSpace().isValid());
    double error = 0;
    int maximum = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const auto a = pixel(image, x, y), b = pixel(encoded.decoded, x, y);
            CHECK(b.alpha == 255);
            for (const int difference : { std::abs(int(a.red) - b.red), std::abs(int(a.green) - b.green),
                     std::abs(int(a.blue) - b.blue) }) {
                error += difference;
                maximum = std::max(maximum, difference);
            }
        }
    CHECK(error / double(image.width() * image.height() * 3) < 9);
    CHECK(maximum < 85);
}

QByteArray fileBytes(const QString& path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly));
    return file.readAll();
}
void seedFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    CHECK(file.write(bytes) == bytes.size());
}
void pathsAtomicWritesAndCancellation()
{
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const auto project = dir.filePath(QStringLiteral("正在编辑.vulkana"));
    seedFile(project, "project-sentinel");
    CHECK(!u::validateExportDestination(project, project).isEmpty());
    const auto alias = dir.filePath(QStringLiteral("project-alias.png"));
    CHECK(QFile::link(project, alias));
    CHECK(!u::validateExportDestination(alias, project).isEmpty());
    const auto png = dir.filePath(QStringLiteral("épreuve-画像.PNG"));
    CHECK(u::exportPathForFormat(png, u::ExportFormat::Png) == png);
    const auto jpeg = dir.filePath(QStringLiteral("photo.JPEG"));
    CHECK(u::exportPathForFormat(jpeg, u::ExportFormat::Jpeg) == jpeg);
    CHECK(QFileInfo(u::exportPathForFormat(png, u::ExportFormat::Jpeg))
              .suffix()
              .compare("jpg", Qt::CaseInsensitive)
        == 0);
    CHECK(
        QFileInfo(u::exportPathForFormat("image", u::ExportFormat::WebP)).suffix() == QStringLiteral("webp"));
    CHECK(u::validateExportDestination(png, project).isEmpty());
    CHECK(!u::validateExportDestination(dir.path(), project).isEmpty());
    CHECK(!u::validateExportDestination(QStringLiteral(""), project).isEmpty());
    std::atomic_bool cancel { false };
    seedFile(png, "previous-destination");
    cancel = true;
    const auto cancelled = u::writeExportAtomically(png, "replacement", cancel);
    CHECK(!cancelled && cancelled.cancelled && fileBytes(png) == "previous-destination");
    const auto image = gradient({ 16, 16 }, true);
    const auto cancelledEncode = u::encodeExport(image, settings(image.size()), cancel);
    CHECK(!cancelledEncode && cancelledEncode.cancelled && cancelledEncode.bytes.isEmpty());
    cancel = false;
    const auto failure = u::writeExportAtomically(dir.filePath("absent/target.png"), "data", cancel);
    CHECK(!failure && !failure.error.isEmpty() && !failure.cancelled);
    CHECK(!QFileInfo::exists(dir.filePath("absent/target.png")));
    const auto encoded = u::encodeExport(image, settings(image.size()), cancel);
    CHECK(encoded);
    if (!encoded)
        return;
    const auto saved = u::writeExportAtomically(png, encoded.bytes, cancel);
    CHECK(saved);
    CHECK(fileBytes(png) == encoded.bytes && equalPixels(QImage(png), encoded.decoded));
    CHECK(fileBytes(project) == "project-sentinel");
    CHECK(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() == 3);
}

void preflightAndRenderCancellation()
{
    auto options = settings({ 32, 24 });
    CHECK(u::validateExport(options, { 32, 24 }).isEmpty());
    options.size = { 0, 24 };
    CHECK(!u::validateExport(options, { 32, 24 }).isEmpty());
    options.size = { -1, 24 };
    CHECK(!u::validateExport(options, { 32, 24 }).isEmpty());
    options.size = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max() };
    CHECK(!u::validateExport(options, { 32, 24 }).isEmpty());
    options = settings({ 16384, 1 }, u::ExportFormat::WebP);
    CHECK(!u::validateExport(options, { 32, 24 }).isEmpty());
    options.size = { 1, 16384 };
    CHECK(!u::validateExport(options, { 32, 24 }).isEmpty());
    options.size = { 16383, 1 };
    CHECK(u::validateExport(options, { 32, 24 }).isEmpty());
    c::Document doc({ { 128, 128 }, 96 });
    CHECK(doc.insertLayer(0, raster("Cancel fixture", { 128, 128 }, { 63, 85, 177, 219 })));
    const auto revision = doc.revision(), content = doc.contentState();
    const auto before = u::renderExport(doc, settings({ 128, 128 }), [](auto, auto) { return false; });
    CHECK(!before && before.cancelled && before.image.isNull());
    const auto during = u::renderExport(
        doc, settings({ 128, 128 }), [](auto done, auto total) { return done < total / 2; });
    CHECK(!during && during.cancelled && during.image.isNull());
    CHECK(doc.revision() == revision && doc.contentState() == content);
    bool changed = false;
    const auto stale = u::renderExport(doc, settings({ 128, 128 }), [&](auto done, auto) {
        if (done > 0 && !changed) {
            changed = true;
            doc.setLayerOpacity(doc.layers().front().id, .5F);
        }
        return true;
    });
    CHECK(changed && !stale && !stale.error.isEmpty());
}

void exportPreferencesAreSeparateAndExplicit(const QString& preferencesPath)
{
    CHECK(!u::hasExportPreferences());
    QSettings storage;
    storage.setValue(QStringLiteral("recentFiles/version"), 77);
    storage.setValue(QStringLiteral("window/test-sentinel"), QStringLiteral("unchanged"));
    auto options = settings({ 300, 200 }, u::ExportFormat::WebP);
    options.destination = QStringLiteral("/tmp/export-画像.webp");
    options.pngEffort = 3;
    options.jpegQuality = 94;
    options.webpLossless = true;
    options.webpQuality = 82;
    options.webpEffort = 5;
    options.pngMatteColor = QColor(51, 82, 119);
    options.jpegMatteColor = QColor(235, 217, 193);
    u::saveExportPreferences(options);
    CHECK(u::hasExportPreferences());
    const auto restored = u::loadExportPreferences({ 640, 480 }, QStringLiteral("new-document"));
    CHECK(restored.pngEffort == 3 && restored.jpegQuality == 94 && restored.webpLossless
        && restored.webpQuality == 82 && restored.webpEffort == 5
        && restored.pngMatteColor == options.pngMatteColor
        && restored.jpegMatteColor == options.jpegMatteColor);
    CHECK(storage.value(QStringLiteral("recentFiles/version")).toInt() == 77);
    CHECK(storage.value(QStringLiteral("window/test-sentinel")).toString() == QStringLiteral("unchanged"));
    CHECK(restored.destination == options.destination);
    CHECK(u::lastExportDirectory() == "/tmp");

    QTemporaryDir files;
    CHECK(QDir(files.path()).mkdir("open"));
    CHECK(QDir(files.path()).mkdir("export"));
    CHECK(QDir(files.path()).mkdir("pictures"));
    const auto open = files.filePath("open");
    const auto exported = files.filePath("export");
    const auto pictures = files.filePath("pictures");
    u::rememberOpenedDocument(open + "/document.vulkana");
    options.destination = exported + "/finished.png";
    u::saveExportPreferences(options);
    // Fresh settings readers use the persisted independent locations.
    QSettings restarted;
    restarted.sync();
    CHECK(restarted.value("files/lastOpenedDocument").toString() == open + "/document.vulkana");
    CHECK(restarted.value("files/lastExportDirectory").toString() == exported);
    CHECK(u::openDocumentDirectory() == open);
    CHECK(u::lastExportDirectory() == exported);
    CHECK(u::loadExportPreferences({640, 480}, "other").destination == options.destination);
    QProcess restartedProcess;
    restartedProcess.start(QCoreApplication::applicationFilePath(),
        {"--check-locations", preferencesPath, open, exported, options.destination});
    CHECK(restartedProcess.waitForFinished(10000));
    CHECK(restartedProcess.exitStatus() == QProcess::NormalExit && restartedProcess.exitCode() == 0);
    CHECK(u::existingFileDialogDirectory(open, pictures, files.path()) == open);
    CHECK(QDir().rmdir(open));
    CHECK(u::existingFileDialogDirectory(open, pictures, files.path()) == pictures);
    CHECK(QDir().rmdir(pictures));
    CHECK(u::existingFileDialogDirectory(open, pictures, files.path()) == files.path());
    CHECK(QDir().rmdir(exported));
    const auto fallback = u::existingFileDialogDirectory({},
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        QCoreApplication::applicationDirPath());
    CHECK(u::openDocumentDirectory() == fallback);
    CHECK(u::lastExportDirectory() == fallback);
    CHECK(QFileInfo(u::loadExportPreferences({640, 480}, "other").destination).absolutePath() == fallback);
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--missing-codec"))) {
        QCoreApplication::setLibraryPaths({ QStringLiteral("/nonexistent-vulkana-export-codecs") });
        const auto error = u::validateExport(settings({ 32, 24 }, u::ExportFormat::Jpeg), { 32, 24 });
        CHECK(error.contains(QStringLiteral("codec is unavailable")));
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("ImageExport"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    if (application.arguments().contains("--check-locations")) {
        const auto args = application.arguments();
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, args.value(2));
        CHECK(u::openDocumentDirectory() == args.value(3));
        CHECK(u::lastExportDirectory() == args.value(4));
        CHECK(u::loadExportPreferences({640, 480}, "other").destination == args.value(5));
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    QTemporaryDir preferences;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    const int font = QFontDatabase::addApplicationFont(
        QStringLiteral(IMAGEEDITOR_EXPORT_FONT_DIR "/NotoSans-Regular.ttf"));
    CHECK(font >= 0);
    const auto families = QFontDatabase::applicationFontFamilies(font);
    CHECK(!families.isEmpty());
    if (!families.isEmpty())
        authoritativeMixedRenderingAndStateIsolation(families.front().toStdString());
    allBlendModesAndMatteIsAfterComposition();
    resizeUsesLinearPremultipliedCoverageAndNativeOrientation();
    pngAndWebpContainersAndAlpha();
    jpegIsRgb444WithFreshMetadata();
    pathsAtomicWritesAndCancellation();
    preflightAndRenderCancellation();
    exportPreferencesAreSeparateAndExplicit(preferences.path());
    if (failures)
        std::cerr << failures << " image export assertions failed\n";
    else
        std::cout << "All image export tests passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
