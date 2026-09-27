#include "imageeditor/core/RasterSurface.hpp"
#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/ui/QtRasterImageLoader.hpp"

#include <QString>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <variant>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using imageeditor::core::ContiguousRasterSurface;
using imageeditor::core::Extent2d;
using imageeditor::core::Extent2u;
using imageeditor::core::Rgba8;
using imageeditor::core::Vec2d;
using imageeditor::render::CanvasCoordinateMapping;

constexpr double kTolerance = 1e-5;

bool close(Vec2d lhs, Vec2d rhs)
{
    return std::abs(lhs.x - rhs.x) < kTolerance
        && std::abs(lhs.y - rhs.y) < kTolerance;
}

Rgba8 readPixel(const ContiguousRasterSurface& surface, int x, int y)
{
    std::array<std::byte, 4> bytes {};
    surface.copyRgba8({x, y, 1, 1}, bytes, bytes.size());
    return {
        std::to_integer<std::uint8_t>(bytes[0]),
        std::to_integer<std::uint8_t>(bytes[1]),
        std::to_integer<std::uint8_t>(bytes[2]),
        std::to_integer<std::uint8_t>(bytes[3]),
    };
}

Vec2d transformUnitPoint(const std::array<float, 16>& matrix, Vec2d unitPoint)
{
    return {
        static_cast<double>(matrix[0]) * unitPoint.x
            + static_cast<double>(matrix[4]) * unitPoint.y
            + static_cast<double>(matrix[12]),
        static_cast<double>(matrix[1]) * unitPoint.x
            + static_cast<double>(matrix[5]) * unitPoint.y
            + static_cast<double>(matrix[13]),
    };
}

Vec2d positiveViewportClipToFramebuffer(Vec2d clipPoint, Extent2u framebuffer)
{
    return {
        (clipPoint.x + 1.0) * static_cast<double>(framebuffer.width) * 0.5,
        (clipPoint.y + 1.0) * static_cast<double>(framebuffer.height) * 0.5,
    };
}

void referenceRasterRowsAreTopDown()
{
    const auto path = QStringLiteral(IMAGEEDITOR_SOURCE_DIR)
        + QStringLiteral("/tests/assets/orientation-corners-reference.png");
    auto loaded = imageeditor::ui::loadRasterLayer(path);
    CHECK(loaded);
    if (!loaded) {
        return;
    }

    CHECK(loaded.extent == Extent2u({320, 240}));
    CHECK(std::holds_alternative<imageeditor::core::RasterLayer>(loaded.layer->payload));
    const auto& raster = std::get<imageeditor::core::RasterLayer>(loaded.layer->payload);
    CHECK(raster.surface != nullptr);
    const auto* surface = dynamic_cast<const ContiguousRasterSurface*>(raster.surface.get());
    CHECK(surface != nullptr);
    if (!surface) {
        return;
    }
    CHECK(readPixel(*surface, 8, 8) == Rgba8({239, 68, 68, 255}));
    CHECK(readPixel(*surface, 311, 8) == Rgba8({34, 197, 94, 255}));
    CHECK(readPixel(*surface, 8, 231) == Rgba8({59, 130, 246, 255}));
    CHECK(readPixel(*surface, 311, 231)
        == Rgba8({250, 204, 21, 255}));
}

void rasterQuadAndCanvasOverlayShareTopLeftMapping()
{
    constexpr Extent2d document {320.0, 240.0};
    constexpr Extent2d logicalViewport {640.0, 480.0};
    constexpr Extent2u framebuffer {1280, 960};
    const CanvasCoordinateMapping mapping(
        document, logicalViewport, framebuffer, imageeditor::core::ViewportState {});
    const auto matrix = mapping.layerUnitToVulkanClip(
        imageeditor::core::AffineTransform {}, {320, 240});

    struct Corner {
        Vec2d unit;
        Vec2d document;
    };
    constexpr std::array corners {
        Corner {{0.0, 0.0}, {0.0, 0.0}},
        Corner {{1.0, 0.0}, {320.0, 0.0}},
        Corner {{0.0, 1.0}, {0.0, 240.0}},
        Corner {{1.0, 1.0}, {320.0, 240.0}},
    };

    for (const auto& corner : corners) {
        const auto layerFramebuffer = positiveViewportClipToFramebuffer(
            transformUnitPoint(matrix, corner.unit), framebuffer);
        const auto overlayFramebuffer = mapping.documentToFramebuffer(corner.document);
        CHECK(close(layerFramebuffer, overlayFramebuffer));

        const auto logicalPoint = mapping.documentToLogicalViewport(corner.document);
        CHECK(close(mapping.logicalViewportToDocument(logicalPoint), corner.document));
    }

    const auto topLeft = positiveViewportClipToFramebuffer(
        transformUnitPoint(matrix, {0.0, 0.0}), framebuffer);
    const auto topRight = positiveViewportClipToFramebuffer(
        transformUnitPoint(matrix, {1.0, 0.0}), framebuffer);
    const auto bottomLeft = positiveViewportClipToFramebuffer(
        transformUnitPoint(matrix, {0.0, 1.0}), framebuffer);
    const auto bottomRight = positiveViewportClipToFramebuffer(
        transformUnitPoint(matrix, {1.0, 1.0}), framebuffer);
    CHECK(close(topLeft, {320.0, 240.0}));
    CHECK(close(topRight, {960.0, 240.0}));
    CHECK(close(bottomLeft, {320.0, 720.0}));
    CHECK(close(bottomRight, {960.0, 720.0}));
    CHECK(topLeft.y < bottomLeft.y);
    CHECK(topRight.y < bottomRight.y);
}

void documentCanvasClipUsesTheCanonicalFramebufferMapping()
{
    imageeditor::core::ViewportState viewport;
    viewport.setZoom(1.25);
    viewport.setPan({0.3, -0.2});
    const CanvasCoordinateMapping mapping(
        {100.0, 80.0}, {200.0, 160.0}, {400, 320}, viewport);

    const auto rect = mapping.documentCanvasFramebufferRect();
    CHECK(std::abs(rect[0] - 75.6) < kTolerance);
    CHECK(std::abs(rect[1] - 59.6) < kTolerance);
    CHECK(std::abs(rect[2] - 250.0) < kTolerance);
    CHECK(std::abs(rect[3] - 200.0) < kTolerance);
    CHECK(mapping.documentCanvasFramebufferScissor()
        == imageeditor::render::FramebufferScissor({75, 59, 251, 201}));

    // A canvas entirely outside the window yields no layer draw region. The
    // document and its layer transforms remain valid; panning it back into
    // view makes the same content visible again.
    viewport.setPan({-1000.0, -1000.0});
    const CanvasCoordinateMapping outsideMapping(
        {100.0, 80.0}, {200.0, 160.0}, {400, 320}, viewport);
    CHECK(outsideMapping.documentCanvasFramebufferScissor().empty());
}

} // namespace

int main()
{
    referenceRasterRowsAreTopDown();
    rasterQuadAndCanvasOverlayShareTopLeftMapping();
    documentCanvasClipUsesTheCanonicalFramebufferMapping();

    if (failures != 0) {
        std::cerr << failures << " raster orientation assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All raster orientation tests passed\n";
    return EXIT_SUCCESS;
}
