#include "imageeditor/core/FreehandSelectionPath.hpp"
#include "imageeditor/core/PolygonCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <vector>

using namespace imageeditor::core;
using Clock = std::chrono::steady_clock;
static double milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

int main()
{
    for (const Extent2u canvas : { Extent2u { 3840, 2160 }, Extent2u { 5120, 2880 } }) {
        for (const bool localized : { true, false }) {
            FreehandSelectionPath path;
            const auto start = Clock::now();
            constexpr unsigned count = 20000;
            const double radiusX = localized ? 140.0 : canvas.width * 0.45;
            const double radiusY = localized ? 90.0 : canvas.height * 0.45;
            for (unsigned i = 0; i < count; ++i) {
                const double angle = 2.0 * std::numbers::pi * i / count;
                const double modulation = 1.0 + 0.06 * std::sin(angle * 37.0);
                path.append({ canvas.width * 0.5 + std::cos(angle) * radiusX * modulation,
                    canvas.height * 0.5 + std::sin(angle) * radiusY * modulation });
            }
            const auto captured = Clock::now();
            PolygonCoverageRasterizer producer(canvas, path.points());
            const auto constructed = Clock::now();
            double maximumStep = 0;
            unsigned steps = 0;
            while (!producer.finished()) {
                const auto before = Clock::now();
                producer.step(65536);
                maximumStep = std::max(maximumStep, milliseconds(Clock::now() - before));
                ++steps;
            }
            const auto rasterized = Clock::now();
            const auto mask = SelectionMask::fromR8Region(
                canvas, producer.region(), producer.coverage(), producer.stride());
            const auto converted = Clock::now();
            const auto& boundaries = mask->boundaryEdges();
            const auto boundaryBuilt = Clock::now();
            std::cout << std::fixed << std::setprecision(2) << canvas.width << 'x' << canvas.height
                      << (localized ? " local" : " broad") << " points=" << path.points().size()
                      << " capture_ms=" << milliseconds(captured - start)
                      << " prepare_ms=" << milliseconds(constructed - captured)
                      << " raster_ms=" << milliseconds(rasterized - constructed)
                      << " max_step_ms=" << maximumStep << " steps=" << steps
                      << " mask_ms=" << milliseconds(converted - rasterized)
                      << " boundaries_ms=" << milliseconds(boundaryBuilt - converted)
                      << " region=" << producer.region().width << 'x' << producer.region().height
                      << " raster_MiB=" << double(producer.memoryBytes()) / (1024.0 * 1024.0)
                      << " mask_MiB=" << double(mask->memoryCost()) / (1024.0 * 1024.0)
                      << " edge_samples=" << producer.stats().edgeSamples
                      << " written_pixels=" << producer.stats().writtenPixels
                      << " boundary_edges=" << boundaries.size() << '\n';
            if (mask->bounds().empty()
                || producer.stats().edgeSamples > PolygonCoverageRasterizer::maximumEdgeSamples)
                return 1;
        }
    }
    return 0;
}
