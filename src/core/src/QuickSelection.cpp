#include "imageeditor/core/SmartSelection.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace imageeditor::core {
namespace {
    NormalizedPointerSample hintSample(NormalizedPointerSample sample)
    {
        sample.modifiers &= ~PointerModifierShift; // Selection modifiers, not the paint line constraint.
        return sample;
    }
    using Color = std::array<std::uint8_t, 4>;
    Color feature(Rgba8 c)
    {
        // Encoded premultiplied RGB + alpha: invisible RGB cannot create edges.
        const auto p = [&](unsigned v) { return std::uint8_t((v * c.alpha + 127) / 255); };
        return { p(c.red), p(c.green), p(c.blue), c.alpha };
    }
    unsigned difference(const Color& a, const Color& b)
    {
        unsigned d = 0;
        for (unsigned i = 0; i < 4; ++i)
            d = std::max(d, unsigned(std::abs(int(a[i]) - int(b[i]))));
        return d;
    }
    unsigned boundaryEdge(const SmartReferenceImage& image, int x, int y, int dx, int dy, const Color& p,
        const Color& q, double sensitivity)
    {
        unsigned edge = difference(p, q);
        if (sensitivity == 0)
            return edge;
        const int ax = x - dx, ay = y - dy, bx = x + 2 * dx, by = y + 2 * dy;
        if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= int(image.extent.width)
            || bx >= int(image.extent.width) || ay >= int(image.extent.height)
            || by >= int(image.extent.height))
            return edge;
        const auto a = std::size_t(ay) * image.extent.width + std::size_t(ax);
        const auto b = std::size_t(by) * image.extent.width + std::size_t(bx);
        // The halo belongs to the coherent reference, not the compute window.
        // Invalid/cropped/transparent surroundings must not manufacture an edge.
        if (!image.valid[a] || !image.valid[b] || ((image.pixels[a].alpha == 0) != (p[3] == 0))
            || ((image.pixels[b].alpha == 0) != (q[3] == 0)))
            return edge;
        const auto first = feature(image.pixels[a]), last = feature(image.pixels[b]);
        for (unsigned k = 0; k < 4; ++k) {
            const int before = int(p[k]) - first[k], center = int(q[k]) - p[k], after = int(last[k]) - q[k];
            // Strengthen ramps, not a ridge/valley: otherwise a dark thin rim
            // between two lighter colors acquires a false barrier along itself.
            if ((before >= 0 && center >= 0 && after >= 0) || (before <= 0 && center <= 0 && after <= 0)) {
                const auto extended = unsigned(std::lround(sensitivity * std::abs(int(last[k]) - first[k])));
                // Localize the wider cue to an actual fine edge. At most 1.5x
                // before integer rounding; no edge on a constant center pair.
                edge = std::max(edge, std::min(extended, unsigned((3 * std::abs(center) + 1) / 2)));
            }
        }
        return edge;
    }
    unsigned bin(const Color& c)
    {
        unsigned b = 0;
        for (unsigned i = 0; i < 4; ++i)
            b |= unsigned((unsigned(c[i]) * 15 + 127) / 255) << (4 * i);
        return b;
    }
    Color binColor(unsigned b)
    {
        return { std::uint8_t((b & 15) * 17), std::uint8_t(((b >> 4) & 15) * 17),
            std::uint8_t(((b >> 8) & 15) * 17), std::uint8_t(((b >> 12) & 15) * 17) };
    }
    std::vector<std::uint8_t> appearance(
        const std::vector<unsigned>& histogram, const std::atomic_bool& cancelled)
    {
        std::vector<unsigned> bins;
        for (unsigned i = 0; i < histogram.size(); ++i)
            if (histogram[i])
                bins.push_back(i);
        std::vector<Color> prototypes;
        if (!bins.empty()) {
            const auto first = *std::max_element(
                bins.begin(), bins.end(), [&](auto a, auto b) { return histogram[a] < histogram[b]; });
            prototypes.push_back(binColor(first));
            while (prototypes.size() < 24 && prototypes.size() < bins.size()) {
                if (cancelled)
                    return { };
                unsigned farthest = 0, chosen = first;
                for (auto b : bins) {
                    unsigned nearest = 255;
                    for (const auto& p : prototypes)
                        nearest = std::min(nearest, difference(binColor(b), p));
                    if (nearest > farthest) {
                        farthest = nearest;
                        chosen = b;
                    }
                }
                if (!farthest)
                    break;
                prototypes.push_back(binColor(chosen));
            }
        }
        std::vector<std::uint8_t> lut(65536, 0);
        for (unsigned b = 0; b < lut.size(); ++b) {
            if (b % 1024 == 0 && cancelled)
                return { };
            unsigned nearest = prototypes.empty() ? 0 : 255;
            for (const auto& p : prototypes)
                nearest = std::min(nearest, difference(binColor(b), p));
            lut[b] = std::uint8_t(nearest);
        }
        return lut;
    }
    constexpr std::uint16_t unreachable = 65535;
    struct Distance {
        std::vector<std::uint16_t> barrier;
        std::vector<std::uint32_t> length;
    };
    // Indexed lexicographic watershed queue. Distance within the current barrier
    // plateau only breaks ties; it NEVER limits how far the selection can grow.
    class Heap {
    public:
        Heap(Distance& d, std::size_t count)
            : d_(d)
            , position_(count, missing)
        {
            ids_.reserve(count);
        }
        void insert(unsigned id)
        {
            unsigned p = position_[id];
            if (p == missing) {
                p = unsigned(ids_.size());
                ids_.push_back(id);
                position_[id] = p;
            }
            while (p > 0) {
                const auto parent = (p - 1) / 2;
                if (!less(ids_[p], ids_[parent]))
                    break;
                swap(p, parent);
                p = parent;
            }
        }
        unsigned pop()
        {
            const auto result = ids_.front();
            position_[result] = missing;
            if (ids_.size() == 1) {
                ids_.pop_back();
                return result;
            }
            ids_[0] = ids_.back();
            ids_.pop_back();
            position_[ids_[0]] = 0;
            unsigned p = 0;
            for (;;) {
                auto c = 2 * p + 1;
                if (c >= ids_.size())
                    break;
                if (c + 1 < ids_.size() && less(ids_[c + 1], ids_[c]))
                    ++c;
                if (!less(ids_[c], ids_[p]))
                    break;
                swap(c, p);
                p = c;
            }
            return result;
        }
        bool empty() const { return ids_.empty(); }

    private:
        bool less(unsigned a, unsigned b) const
        {
            if (d_.barrier[a] != d_.barrier[b])
                return d_.barrier[a] < d_.barrier[b];
            if (d_.length[a] != d_.length[b])
                return d_.length[a] < d_.length[b];
            return a < b;
        }
        void swap(unsigned a, unsigned b)
        {
            std::swap(ids_[a], ids_[b]);
            position_[ids_[a]] = a;
            position_[ids_[b]] = b;
        }
        static constexpr unsigned missing = std::numeric_limits<unsigned>::max();
        Distance& d_;
        std::vector<unsigned> position_, ids_;
    };
    std::uint8_t contourCoverage(double value, double dx, double dy)
    {
        const double a = std::abs(dx), b = std::abs(dy), z = value + (a + b) * .5;
        if (z <= 0)
            return 0;
        if (z >= a + b)
            return 255;
        const auto square = [](double v) { return std::max(0.0, v) * std::max(0.0, v); };
        const double area = a < 1e-9 || b < 1e-9
            ? z / std::max(a, b)
            : (square(z) - square(z - a) - square(z - b) + square(z - a - b)) / (2 * a * b);
        return std::uint8_t(std::lround(std::clamp(area, 0.0, 1.0) * 255));
    }
    // Prefer a persistent seeded basin before it merges with a neighboring object.
    // The largest global escape alone may occur AFTER a shoulder joins a strap.
    // Retain the area-escape heuristic as a fallback for short/ambiguous plateaus.
    float basinCut(const Distance& distance, std::size_t freshCount, const std::atomic_bool& cancelled)
    {
        std::array<std::uint64_t, 256> histogram { };
        for (std::size_t i = 0; i < distance.barrier.size(); ++i) {
            if (i % 4096 == 0 && cancelled)
                return 0;
            const auto value = distance.barrier[i];
            if (value < histogram.size())
                ++histogram[value];
        }
        std::uint64_t area = histogram[0];
        std::array<std::uint64_t, 256> cumulative { };
        for (unsigned level = 0; level < cumulative.size(); ++level)
            cumulative[level] = histogram[level] + (level ? cumulative[level - 1] : 0);
        for (unsigned level = 4; level < cumulative.size(); ++level) {
            const auto before = cumulative[level - 1], earlier = cumulative[level - 4];
            if (before * 5 > std::uint64_t(freshCount) * 6 && earlier > 0 && before * 100 <= earlier * 108
                && histogram[level] * 100 >= before * 15) {
                unsigned occupied = level - 1;
                while (occupied > 0 && !histogram[occupied])
                    --occupied;
                // Preserve exact pixel edges across wide contrast gaps as well
                // as softly varying boundaries. Never use the upper endpoint.
                return .5f * float(occupied + level);
            }
        }
        unsigned previous = 0;
        float cut = 255.5f;
        double best = 0;
        for (unsigned level = 1; level < histogram.size(); ++level) {
            if (!histogram[level])
                continue;
            if (area >= freshCount && area) {
                const double growth = std::log1p(double(histogram[level]) / double(area));
                const double support
                    = std::log1p(double(area) / double(std::max(std::size_t(1), freshCount)));
                const double persistence = std::sqrt(double(level - previous));
                const double score = growth * support * persistence;
                if (score > best) {
                    best = score;
                    // Mid-gap placement gives a hard two-color boundary its actual
                    // pixel edge, rather than spreading AA across a background pixel.
                    cut = .5f * float(previous + level);
                }
            }
            area += histogram[level];
            previous = level;
        }
        return cut; // A completely flat connected region has no boundary: select it.
    }
    struct WindowResult {
        SelectionState incoming;
        QuickSelectionHints hints;
        SmartSelectionStats stats;
        bool touchesArtificialEdge { };
    };
    WindowResult solveWindow(const SmartReferenceImage& image, std::span<const QuickHintDab> dabs,
        RectI region, QuickSelectionHints previous, bool subtract, const std::atomic_bool& cancelled,
        QuickSelectionSettings settings)
    {
        const auto count = std::size_t(region.width) * std::size_t(region.height);
        // Includes both class maps, indexed queue, feature/model/hint/output buffers.
        // Failure is atomic; a compute-window edge is never published as a crop.
        if (count > 16'000'000)
            throw std::length_error("Quick Selection needs more than its 16-million-pixel workspace budget");
        const auto stride = std::size_t(region.width);
        const auto global = [&](std::size_t i) {
            return (std::size_t(region.y) + i / stride) * image.extent.width + std::size_t(region.x)
                + i % stride;
        };
        std::vector<Color> colors(count);
        std::vector<std::uint8_t> fresh(count), hints(count), valid(count), rejected(count);
        std::vector<std::uint16_t> bins(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled)
                return { };
            const int x = region.x + int(i % stride), y = region.y + int(i / stride);
            valid[i] = image.valid[global(i)];
            colors[i] = feature(image.pixels[global(i)]);
            bins[i] = std::uint16_t(bin(colors[i]));
            if (previous.foreground && previous.foreground->coverageAtDocumentPixel(x, y))
                hints[i] = 1;
            if (previous.background && previous.background->coverageAtDocumentPixel(x, y))
                hints[i] = 2;
            if (!subtract && previous.rejected)
                rejected[i] = previous.rejected->coverageAtDocumentPixel(x, y);
        }
        // Each undirected edge is prepared once and shared by both evidence
        // classes. Two bytes/pixel avoid sampling the halo on every queue visit.
        std::vector<std::array<std::uint8_t, 2>> edges(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled)
                return { };
            if (!valid[i])
                continue;
            const int x = region.x + int(i % stride), y = region.y + int(i / stride);
            if (x + 1 < region.right() && valid[i + 1])
                edges[i][0] = std::uint8_t(
                    boundaryEdge(image, x, y, 1, 0, colors[i], colors[i + 1], settings.edgeSensitivity));
            if (y + 1 < region.bottom() && valid[i + stride])
                edges[i][1] = std::uint8_t(
                    boundaryEdge(image, x, y, 0, 1, colors[i], colors[i + stride], settings.edgeSensitivity));
        }
        std::size_t freshCount = 0;
        for (const auto& dab : dabs) {
            if (cancelled)
                return { };
            const auto box = RectI { int(std::floor(dab.center.x - dab.radius)),
                int(std::floor(dab.center.y - dab.radius)), int(std::ceil(2 * dab.radius)) + 2,
                int(std::ceil(2 * dab.radius)) + 2 }
                                 .clippedTo(region);
            for (int y = box.y; y < box.bottom(); ++y)
                for (int x = box.x; x < box.right(); ++x) {
                    const double dx = x + .5 - dab.center.x, dy = y + .5 - dab.center.y;
                    const auto i = std::size_t(y - region.y) * stride + std::size_t(x - region.x);
                    if (valid[i] && dx * dx + dy * dy <= dab.radius * dab.radius) {
                        if (!fresh[i])
                            ++freshCount;
                        fresh[i] = 255;
                        hints[i] = subtract ? 2 : 1;
                        rejected[i] = 0; // Deliberate new input overrides a prior correction here.
                    }
                }
        }
        if (!freshCount)
            return { };
        const auto stroke = SelectionMask::fromR8Region(image.extent, region, fresh, stride);
        const auto addHint = [&](SelectionState old) {
            return old ? combineSelection(old, stroke, SelectionOperation::Add) : stroke;
        };
        const auto removeHint = [&](SelectionState old) {
            return old ? combineSelection(old, stroke, SelectionOperation::Subtract) : SelectionState { };
        };
        QuickSelectionHints updated = subtract
            ? QuickSelectionHints { removeHint(previous.foreground), addHint(previous.background),
                  previous.rejected }
            : QuickSelectionHints { addHint(previous.foreground), removeHint(previous.background),
                  removeHint(previous.rejected) };
        const unsigned targetClass = subtract ? 2 : 1, otherClass = subtract ? 1 : 2;
        std::array<std::vector<unsigned>, 2> histogram { std::vector<unsigned>(65536),
            std::vector<unsigned>(65536) };
        std::array<std::size_t, 2> hintCounts { };
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled)
                return { };
            if (hints[i] && valid[i]) {
                ++histogram[hints[i] - 1][bins[i]];
                ++hintCounts[hints[i] - 1];
            }
        }
        const bool competing = hintCounts[0] && hintCounts[1];
        std::array<std::vector<std::uint8_t>, 2> model;
        if (competing) {
            model[0] = appearance(histogram[0], cancelled);
            model[1] = appearance(histogram[1], cancelled);
            if (cancelled)
                return { };
        }
        std::uint64_t pops = 0;
        const auto neighbors = [&](unsigned u, const auto& visit) {
            const unsigned x = u % unsigned(region.width), y = u / unsigned(region.width);
            if (x)
                visit(u - 1);
            if (x + 1 < unsigned(region.width))
                visit(u + 1);
            if (y)
                visit(u - unsigned(region.width));
            if (y + 1 < unsigned(region.height))
                visit(u + unsigned(region.width));
        };
        const auto solve = [&](bool target) {
            Distance d { std::vector<std::uint16_t>(count, unreachable),
                std::vector<std::uint32_t>(count, std::numeric_limits<std::uint32_t>::max()) };
            Heap heap(d, count);
            const auto own = target ? targetClass : otherClass, opposite = target ? otherClass : targetClass;
            for (unsigned i = 0; i < count; ++i) {
                if (i % 4096 == 0 && cancelled)
                    return Distance { };
                if (valid[i] && (target ? fresh[i] != 0 : hints[i] == otherClass)) {
                    d.barrier[i] = 0;
                    d.length[i] = 0;
                    heap.insert(i);
                }
            }
            while (!heap.empty()) {
                if (++pops % 2048 == 0 && cancelled)
                    return Distance { };
                const auto u = heap.pop();
                neighbors(u, [&](unsigned v) {
                    if (!valid[v] || hints[v] == opposite || (target && rejected[v] >= 128)
                        || ((colors[u][3] == 0) != (colors[v][3] == 0)))
                        return;
                    const auto first = std::min(u, v);
                    unsigned edge = edges[first][u / stride == v / stride ? 0 : 1];
                    // Appearance is discriminative evidence ONLY when both classes
                    // have user examples. A lone dark pupil must be able to reach
                    // the purple iris across gentle interior gradients.
                    if (competing) {
                        const int mismatch = int(model[own - 1][bins[v]]) - int(model[opposite - 1][bins[v]]);
                        edge = std::max(edge, unsigned(std::max(0, mismatch)) / 4);
                    }
                    const auto barrier = std::uint16_t(std::max(unsigned(d.barrier[u]), edge));
                    // Reset at a higher barrier: total path length is NOT an
                    // isotone minimax tie-break and would discard valid routes.
                    const auto length = barrier > d.barrier[u] ? 1U : d.length[u] + 1;
                    if (barrier < d.barrier[v] || (barrier == d.barrier[v] && length < d.length[v])) {
                        d.barrier[v] = barrier;
                        d.length[v] = length;
                        heap.insert(v);
                    }
                });
            }
            return d;
        };
        auto target = solve(true);
        if (cancelled || target.barrier.empty())
            return { };
        const auto cut = basinCut(target, freshCount, cancelled);
        if (cancelled)
            return { };
        Distance other;
        if (competing) {
            other = solve(false);
            if (cancelled || other.barrier.empty())
                return { };
        }
        std::vector<float> field(count, -1);
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled)
                return { };
            if (target.barrier[i] == unreachable)
                continue;
            float value = cut - target.barrier[i];
            if (competing && other.barrier[i] != unreachable) {
                float competition = float(other.barrier[i]) - target.barrier[i];
                if (competition == 0) {
                    const double length = double(target.length[i]) + other.length[i] + 1;
                    competition = float((double(other.length[i]) - target.length[i]) / length);
                }
                value = std::min(value, competition);
            }
            field[i] = value;
        }
        // Competition can sever a bridge. Admit only winners connected to fresh
        // seeds, not detached positive-score islands. Keep narrow paths and holes.
        std::vector<std::uint8_t> connected(count, 0);
        std::vector<unsigned> frontier;
        frontier.reserve(count);
        for (unsigned i = 0; i < count; ++i)
            if (fresh[i]) {
                connected[i] = 1;
                frontier.push_back(i);
            }
        for (std::size_t head = 0; head < frontier.size(); ++head) {
            if (head % 4096 == 0 && cancelled)
                return { };
            neighbors(frontier[head], [&](unsigned v) {
                if (!connected[v] && field[v] > 0 && valid[v] && hints[v] != otherClass) {
                    connected[v] = 1;
                    frontier.push_back(v);
                }
            });
        }
        bool touches = false;
        for (unsigned i : frontier) {
            const int x = int(i % stride), y = int(i / stride);
            touches |= (x <= 1 && region.x > 0) || (y <= 1 && region.y > 0)
                || (x + 2 >= region.width && region.right() < int(image.extent.width))
                || (y + 2 >= region.height && region.bottom() < int(image.extent.height));
        }
        const SmartSelectionStats stats { count, pops, count * 38 + 2 * 65536 * (sizeof(unsigned) + 1),
            region };
        if (touches)
            return { { }, { }, stats, true };
        std::vector<std::uint8_t> coverage(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 4096 == 0 && cancelled)
                return { };
            if (!valid[i] || hints[i] == otherClass)
                continue;
            if (fresh[i]) {
                coverage[i] = 255;
                continue;
            }
            bool fringe = false;
            if (!connected[i]) {
                if (field[i] > 0)
                    continue;
                neighbors(unsigned(i), [&](unsigned v) { fringe |= connected[v] != 0; });
                if (!fringe)
                    continue;
            }
            const int x = int(i % stride), y = int(i / stride);
            const auto at = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= region.width || ny >= region.height)
                    return field[i];
                const auto v = std::size_t(ny) * stride + std::size_t(nx);
                if (!valid[v] || ((colors[i][3] == 0) != (colors[v][3] == 0)))
                    return field[i];
                return field[v];
            };
            coverage[i] = contourCoverage(
                field[i], (at(x + 1, y) - at(x - 1, y)) * .5, (at(x, y + 1) - at(x, y - 1)) * .5);
            coverage[i] = std::min(coverage[i], std::uint8_t(255 - rejected[i]));
        }
        return { SelectionMask::fromR8Region(image.extent, region, coverage, stride), updated, stats, false };
    }
} // namespace
bool QuickSelectionPath::begin(double diameter, NormalizedPointerSample sample)
{
    dabs_.clear();
    BrushSettings settings;
    settings.sizePixels = std::clamp(diameter, 2.0, 512.0);
    settings.hardness = 1;
    settings.spacingPercent = 20;
    settings.pressureToSize = false;
    settings.pressureToFlow = false;
    settings.tip.rotationMode = BrushTipRotationMode::Fixed;
    return engine_.beginStroke(settings, hintSample(sample), *this);
}
bool QuickSelectionPath::append(NormalizedPointerSample sample)
{
    return engine_.appendSample(hintSample(sample), *this);
}
bool QuickSelectionPath::end(NormalizedPointerSample sample)
{
    return engine_.endStroke(hintSample(sample), *this);
}
void QuickSelectionPath::emitDab(const BrushDab& dab)
{
    if (dabs_.size() >= 16384)
        throw std::length_error("Quick Selection stroke is too long; continue with another stroke");
    dabs_.push_back({ dab.documentCenter, dab.diameterPixels * .5 });
}
SmartSelectionResult buildQuickSelection(const SmartReferenceImage& image, std::span<const QuickHintDab> dabs,
    QuickSelectionHints previous, SelectionState original, SelectionOperation operation,
    const std::atomic_bool& cancelled, QuickSelectionSettings settings)
{
    const auto total = std::uint64_t(image.extent.width) * image.extent.height;
    if (!total || total > SmartSelectionReference::maximumPixels || image.pixels.size() != total
        || image.valid.size() != total)
        throw std::invalid_argument("Invalid Smart Select reference");
    if (!std::isfinite(settings.edgeSensitivity) || settings.edgeSensitivity < 0
        || settings.edgeSensitivity > 1)
        throw std::invalid_argument("Quick Selection edge sensitivity must be in [0, 1]");
    if (dabs.empty() || cancelled)
        return { };
    if (dabs.size() > 16384)
        throw std::length_error("Quick Selection stroke exceeds 16,384 evidence dabs");
    const RectI canvas { 0, 0, int(image.extent.width), int(image.extent.height) };
    RectI hintBounds;
    for (const auto& dab : dabs) {
        if (!std::isfinite(dab.center.x) || !std::isfinite(dab.center.y) || !std::isfinite(dab.radius)
            || std::abs(dab.center.x) > 1e8 || std::abs(dab.center.y) > 1e8 || dab.radius < 1
            || dab.radius > 256)
            throw std::invalid_argument("Invalid Quick Selection hint");
        const int x = int(std::floor(dab.center.x - dab.radius)),
                  y = int(std::floor(dab.center.y - dab.radius));
        hintBounds = hintBounds.united(RectI { x, y, int(std::ceil(dab.center.x + dab.radius)) - x,
            int(std::ceil(dab.center.y + dab.radius)) - y }
                .clippedTo(canvas));
    }
    if (hintBounds.empty())
        return { };
    if (operation == SelectionOperation::Replace || operation == SelectionOperation::Intersect)
        previous = { };
    // Compute windows expand until the image-supported result closes. No radius
    // or window edge is ever an inferred boundary or implicit background hint.
    int padding = 128;
    SmartSelectionStats aggregate;
    for (;;) {
        const auto region = RectI { hintBounds.x - padding, hintBounds.y - padding,
            hintBounds.width + 2 * padding, hintBounds.height + 2 * padding }
                                .clippedTo(canvas);
        auto result = solveWindow(
            image, dabs, region, previous, operation == SelectionOperation::Subtract, cancelled, settings);
        if (cancelled)
            return { };
        aggregate.evaluatedPixels += result.stats.evaluatedPixels;
        aggregate.queuePops += result.stats.queuePops;
        aggregate.workspaceBytes = std::max(aggregate.workspaceBytes, result.stats.workspaceBytes);
        aggregate.workRegion = region;
        if (!result.touchesArtificialEdge) {
            if (!result.incoming)
                return { };
            auto combined = combineSelection(original, result.incoming, operation);
            if (operation == SelectionOperation::Subtract) {
                // Keep the accepted correction contour stable across later Add
                // strokes. This inferred ceiling is NOT a hard training sample.
                const auto bounds = result.incoming->bounds();
                std::vector<std::uint8_t> ceiling(std::size_t(bounds.width) * std::size_t(bounds.height));
                for (int y = 0; y < bounds.height; ++y) {
                    if (cancelled)
                        return { };
                    for (int x = 0; x < bounds.width; ++x)
                        if (result.incoming->coverageAtDocumentPixel(bounds.x + x, bounds.y + y))
                            ceiling[std::size_t(y) * std::size_t(bounds.width) + std::size_t(x)]
                                = std::uint8_t(
                                    255 - combined->coverageAtDocumentPixel(bounds.x + x, bounds.y + y));
                }
                const auto protection
                    = SelectionMask::fromR8Region(image.extent, bounds, ceiling, std::size_t(bounds.width));
                result.hints.rejected = result.hints.rejected
                    ? combineSelection(result.hints.rejected, protection, SelectionOperation::Add)
                    : protection;
            }
            if (!prepareSelectionBoundary(result.incoming, cancelled)
                || (combined != result.incoming && !prepareSelectionBoundary(combined, cancelled))
                || (result.hints.foreground && !prepareSelectionBoundary(result.hints.foreground, cancelled))
                || (result.hints.background && !prepareSelectionBoundary(result.hints.background, cancelled))
                || (result.hints.rejected && !prepareSelectionBoundary(result.hints.rejected, cancelled)))
                return { };
            return { result.incoming, combined, result.hints, aggregate };
        }
        padding *= 2;
    }
}
} // namespace imageeditor::core
