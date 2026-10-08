#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace imageeditor::core::detail {
struct QuickCleanupStats {
    std::size_t regions {}, pixels {};
};

// Connected area cleanup, not an opening/closing or largest-component filter.
// Protection bits: 1 keeps selected evidence; 2 keeps excluded evidence. Work on
// the published R8 threshold so antialiasing cannot leave marching-ants specks.
inline QuickCleanupStats cleanQuickSelection(std::span<std::uint8_t> coverage,
    std::span<const std::uint8_t> protection, unsigned width, unsigned height,
    double brushDiameter, const std::atomic_bool& cancelled)
{
    QuickCleanupStats result;
    if (brushDiameter <= 8 || cancelled)
        return result;
    const double scale = std::clamp((brushDiameter - 8) * .75, 0.0, 24.0);
    const auto maxArea = std::size_t(std::floor(scale * scale));
    const int maxSpan = int(std::ceil(2 * scale));
    if (!maxArea) return result;
    const auto count = coverage.size();
    std::vector<std::uint8_t> labels(count), visited(count), flipped(count);
    for (std::size_t i = 0; i < count; ++i) labels[i] = coverage[i] >= 128;
    std::vector<unsigned> pending, changed;
    const auto neighbors = [&](unsigned i, const auto& visit) {
        const int x = int(i % width), y = int(i / width);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < int(width) && ny < int(height))
                visit(unsigned(ny) * width + unsigned(nx));
        }
    };
    // Remove unsupported islands first. Otherwise a hole inside a removed
    // island could be inverted into a new foreground speck in the same pass.
    for (const unsigned selected : {1U, 0U}) {
        std::fill(visited.begin(), visited.end(), 0);
        changed.clear();
        for (unsigned start = 0; start < count; ++start) {
            if ((start & 4095) == 0 && cancelled) return result;
            if (visited[start] || labels[start] != selected) continue;
            pending.clear(); pending.push_back(start); visited[start] = 1;
            bool protectedRegion = false;
            int left = int(start % width), right = left, top = int(start / width), bottom = top;
            const int originX = left, originY = top;
            double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
            for (std::size_t head = 0; head < pending.size(); ++head) {
                if ((head & 4095) == 0 && cancelled) return result;
                const auto i = pending[head];
                const int x = int(i % width), y = int(i / width);
                left = std::min(left, x); right = std::max(right, x);
                top = std::min(top, y); bottom = std::max(bottom, y);
                if (head < maxArea) {
                    const double dx = x - originX, dy = y - originY;
                    sx += dx; sy += dy; sxx += dx * dx; syy += dy * dy; sxy += dx * dy;
                }
                protectedRegion |= (protection[i] & (selected ? 1 : 2)) != 0
                    || x == 0 || y == 0 || x + 1 == int(width) || y + 1 == int(height);
                neighbors(i, [&](unsigned v) {
                    if (!visited[v] && labels[v] == selected) {
                        visited[v] = 1; pending.push_back(v);
                    }
                });
            }
            const auto wide = std::max(right - left + 1, bottom - top + 1);
            const auto narrow = std::min(right - left + 1, bottom - top + 1);
            // Retain thin gaps/strands and open boundaries, including fingers.
            if (protectedRegion || pending.size() > maxArea || wide > maxSpan
                || (wide > 6 && wide > 4 * narrow)) continue;
            // Principal-axis elongation also protects diagonal thin details;
            // an axis-aligned bounding box alone would mistake them for specks.
            const double n = double(pending.size());
            const double xx = sxx / n - (sx / n) * (sx / n);
            const double yy = syy / n - (sy / n) * (sy / n);
            const double xy = sxy / n - (sx / n) * (sy / n);
            const double spread = std::hypot(xx - yy, 2 * xy);
            if (wide > 6 && xx + yy + spread > 16 * std::max(0.0, xx + yy - spread)) continue;
            ++result.regions;
            changed.insert(changed.end(), pending.begin(), pending.end());
        }
        const std::uint8_t replacement = selected ? 0 : 255;
        const auto assign = [&](unsigned i) {
            if (coverage[i] != replacement) { coverage[i] = replacement; ++result.pixels; }
        };
        std::fill(flipped.begin(), flipped.end(), 0);
        for (auto i : changed) flipped[i] = 1;
        for (auto i : changed) {
            if (cancelled) return result;
            assign(i);
            // Erase the obsolete one-pixel antialias fringe as well. No other
            // contour is resampled, so the main outline remains exactly intact.
            neighbors(i, [&](unsigned v) {
                if (labels[v] == selected || protection[v]) return;
                bool obsolete = true;
                neighbors(v, [&](unsigned adjacent) {
                    if (labels[adjacent] == selected && !flipped[adjacent]) obsolete = false;
                });
                if (obsolete) assign(v);
            });
        }
        for (auto i : changed) labels[i] = std::uint8_t(!selected);
    }
    return result;
}
}
