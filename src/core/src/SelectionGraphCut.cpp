#include "imageeditor/core/SelectionGraphCut.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
std::vector<std::uint8_t> selectionGraphCut(unsigned width, unsigned height,
    std::span<const float> foregroundCost, std::span<const float> backgroundCost,
    std::span<const float> right, std::span<const float> down, const std::atomic_bool& cancelled,
    std::span<const std::uint8_t> active)
{
    const auto count = std::uint64_t(width) * height;
    if (!count || count > 16'000'000 || foregroundCost.size() != count || backgroundCost.size() != count
        || right.size() != count || down.size() != count || (!active.empty() && active.size() != count))
        throw std::invalid_argument("Invalid selection graph dimensions");
    const auto none = std::numeric_limits<unsigned>::max();
    std::vector<unsigned> indices(std::size_t(count), none);
    unsigned n = 0;
    for (unsigned i = 0; i < count; ++i)
        if (active.empty() || active[i])
            indices[i] = n++;
    const auto source = n, sink = n + 1;
    struct Edge {
        unsigned to, next, capacity;
    };
    std::vector<Edge> edges;
    edges.reserve(std::size_t(n) * 8);
    std::vector<unsigned> first(n + 2, none), level(n + 2), current(n + 2), queue;
    queue.reserve(n + 2);
    const auto capacity = [](float cost) {
        if (!std::isfinite(cost) || cost < 0 || cost > 1e6f)
            throw std::invalid_argument("Invalid selection graph cost");
        return unsigned(std::lround(cost * 256));
    };
    // Fixed nodes still participate through their neighbours. Validate their
    // costs too, rather than allowing NaNs to choose an arbitrary fixed label.
    for (unsigned i = 0; i < count; ++i) {
        if ((i & 4095) == 0 && cancelled)
            return { };
        (void)capacity(foregroundCost[i]);
        (void)capacity(backgroundCost[i]);
        (void)capacity(right[i]);
        (void)capacity(down[i]);
    }
    const auto add = [&](unsigned a, unsigned b, unsigned ab, unsigned ba) {
        if (!ab && !ba)
            return;
        edges.push_back({ b, first[a], ab });
        first[a] = unsigned(edges.size() - 1);
        edges.push_back({ a, first[b], ba });
        first[b] = unsigned(edges.size() - 1);
    };
    for (unsigned i = 0; i < count; ++i) {
        if ((i & 4095) == 0 && cancelled)
            return { };
        const auto u = indices[i];
        if (u == none)
            continue;
        unsigned bg = capacity(backgroundCost[i]), fg = capacity(foregroundCost[i]);
        const auto fixed = [&](unsigned v, float weight) {
            if (indices[v] != none)
                return;
            (foregroundCost[v] < backgroundCost[v] ? bg : fg) += capacity(weight);
        };
        if (i % width)
            fixed(i - 1, right[i - 1]);
        if (i % width + 1 < width)
            fixed(i + 1, right[i]);
        if (i >= width)
            fixed(i - width, down[i - width]);
        if (i + width < count)
            fixed(i + width, down[i]);
        add(source, u, bg, 0);
        add(u, sink, fg, 0);
        const auto r = capacity(right[i]), d = capacity(down[i]);
        if (i % width + 1 < width && indices[i + 1] != none)
            add(u, indices[i + 1], r, r);
        if (i + width < count && indices[i + width] != none)
            add(u, indices[i + width], d, d);
    }
    // Dinic blocking flow, with an explicit edge-path stack (no recursion).
    // Fixed-point costs prevent floating residual cycles. Each edge has a paired
    // reverse residual; no image-border or implicit-background terminal exists.
    std::vector<unsigned> path;
    path.reserve(n + 2);
    std::uint64_t work = 0;
    for (;;) {
        std::fill(level.begin(), level.end(), none);
        level[source] = 0;
        queue.clear();
        queue.push_back(source);
        for (std::size_t head = 0; head < queue.size(); ++head) {
            if ((++work & 4095) == 0 && cancelled)
                return { };
            const auto u = queue[head];
            for (auto e = first[u]; e != none; e = edges[e].next)
                if (edges[e].capacity && level[edges[e].to] == none) {
                    level[edges[e].to] = level[u] + 1;
                    queue.push_back(edges[e].to);
                }
        }
        if (level[sink] == none)
            break;
        current = first;
        path.clear();
        unsigned u = source;
        for (;;) {
            if ((++work & 4095) == 0 && cancelled)
                return { };
            if (u == sink) {
                unsigned flow = none;
                for (auto e : path)
                    flow = std::min(flow, edges[e].capacity);
                std::size_t firstSaturated = path.size();
                for (std::size_t i = 0; i < path.size(); ++i) {
                    const auto e = path[i];
                    edges[e].capacity -= flow;
                    edges[e ^ 1].capacity += flow;
                    if (!edges[e].capacity)
                        firstSaturated = std::min(firstSaturated, i);
                }
                u = edges[path[firstSaturated] ^ 1].to;
                path.resize(firstSaturated);
                continue;
            }
            auto& e = current[u];
            while (e != none && (!edges[e].capacity || level[edges[e].to] != level[u] + 1))
                e = edges[e].next;
            if (e != none) {
                path.push_back(e);
                u = edges[e].to;
                continue;
            }
            level[u] = none;
            if (path.empty())
                break;
            u = edges[path.back() ^ 1].to;
            path.pop_back();
        }
    }
    std::vector<std::uint8_t> result(std::size_t(count), 0);
    for (unsigned i = 0; i < count; ++i)
        result[i] = indices[i] != none ? level[indices[i]] != none : foregroundCost[i] < backgroundCost[i];
    return result;
}
}
