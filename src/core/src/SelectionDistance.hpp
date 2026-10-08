#pragma once
#include <algorithm>
#include <vector>

namespace imageeditor::core::detail {
// Exact separable squared Euclidean distance, shared by coverage refinement
// and brush-local selection. Zeroes are sites; 1e16 denotes no site. The caller
// owns dimension validation and supplies a per-line cancellation checkpoint.
template<class Check>
std::vector<float> selectionDistance(std::vector<float> values, int w, int h, Check check)
{
    constexpr float inf = 1e16F;
    const auto length = std::size_t(std::max(w, h));
    std::vector<float> f(length);
    std::vector<int> sites(length);
    std::vector<double> cuts(length + 1);
    const auto line = [&](std::size_t start, std::size_t stride, int n) {
        int last = -1;
        for (int q = 0; q < n; ++q) {
            f[std::size_t(q)] = values[start + std::size_t(q) * stride];
            if (f[std::size_t(q)] >= inf)
                continue;
            double cut = -1e30;
            while (last >= 0) {
                const int p = sites[std::size_t(last)];
                cut = (double(f[std::size_t(q)]) + double(q) * q
                    - double(f[std::size_t(p)]) - double(p) * p) / (2. * (q - p));
                if (cut > cuts[std::size_t(last)])
                    break;
                --last;
            }
            sites[std::size_t(++last)] = q;
            cuts[std::size_t(last)] = last ? cut : -1e30;
            cuts[std::size_t(last) + 1] = 1e30;
        }
        if (last < 0)
            return;
        int site = 0;
        for (int q = 0; q < n; ++q) {
            while (site < last && cuts[std::size_t(site) + 1] < q)
                ++site;
            const int p = sites[std::size_t(site)];
            const float delta = float(q - p);
            values[start + std::size_t(q) * stride] = delta * delta + f[std::size_t(p)];
        }
    };
    for (int y = 0; y < h; ++y) {
        check();
        line(std::size_t(y) * std::size_t(w), 1, w);
    }
    for (int x = 0; x < w; ++x) {
        check();
        line(std::size_t(x), std::size_t(w), h);
    }
    return values;
}
}
