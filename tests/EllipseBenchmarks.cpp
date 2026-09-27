#include "imageeditor/core/EllipseCoverageRasterizer.hpp"
#include "imageeditor/core/SelectionMask.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>

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
            const double radiusX = localized ? 140.125 : canvas.width * 0.45;
            const double radiusY = localized ? 90.375 : canvas.height * 0.45;
            const Vec2d center { canvas.width * 0.5 + 0.125,
                canvas.height * 0.5 + 0.25 };
            const EllipseGeometry ellipse(center - Vec2d { radiusX, radiusY },
                center + Vec2d { radiusX, radiusY });
            const auto start = Clock::now();
            EllipseCoverageRasterizer producer(canvas, ellipse);
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
            double maximumPreview = 0, totalPreview = 0;
            std::size_t previewEdges = 0;
            constexpr unsigned previewCount = 24;
            for (unsigned i = 0; i < previewCount; ++i) {
                const double delta = i / 32.0;
                const EllipseGeometry varied(ellipse.minimum(),
                    ellipse.maximum() + Vec2d { delta, delta });
                const auto before = Clock::now();
                const auto preview = varied.previewEdges(canvas);
                const double duration = milliseconds(Clock::now() - before);
                maximumPreview = std::max(maximumPreview, duration);
                totalPreview += duration;
                previewEdges = std::max(previewEdges, preview.size());
            }
            std::cout << std::fixed << std::setprecision(3) << canvas.width << 'x'
                      << canvas.height << (localized ? " local" : " broad")
                      << " prepare_ms=" << milliseconds(constructed - start)
                      << " raster_ms=" << milliseconds(rasterized - constructed)
                      << " max_step_ms=" << maximumStep << " steps=" << steps
                      << " mask_ms=" << milliseconds(converted - rasterized)
                      << " boundaries_ms=" << milliseconds(boundaryBuilt - converted)
                      << " preview_avg_ms=" << totalPreview / previewCount
                      << " preview_max_ms=" << maximumPreview
                      << " region=" << producer.region().width << 'x'
                      << producer.region().height << " raster_MiB="
                      << double(producer.memoryBytes()) / (1024.0 * 1024.0)
                      << " mask_MiB="
                      << double(mask->memoryCost()) / (1024.0 * 1024.0)
                      << " partial_evaluations=" << producer.stats().boundaryPixels
                      << " written_pixels=" << producer.stats().writtenPixels
                      << " boundary_edges=" << boundaries.size()
                      << " preview_edges=" << previewEdges << '\n';
            if (mask->bounds().empty() || boundaries.empty() || producer.stats().writtenPixels > std::uint64_t(producer.region().width) * std::uint64_t(producer.region().height))
                return 1;
        }
    }
    return 0;
}
