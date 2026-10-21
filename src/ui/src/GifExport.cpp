#include "imageeditor/ui/GifExport.hpp"
#include "imageeditor/platform/AvailableMemory.hpp"
#include <QBuffer>
#include <QColorSpace>
#include <QImageReader>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace imageeditor::ui
{
namespace
{
constexpr qsizetype byteBudget = 256 * 1024 * 1024;
struct Cancelled {
};
void check(const std::atomic_bool &cancel)
{
    if (cancel.load())
        throw Cancelled{};
}
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
using Color = std::array<double, 3>;
struct Bin {
    std::uint64_t count{};
    Color sum{};
};
struct Sample {
    std::uint64_t count{};
    Color color{};
};

QImage sourceImage(const core::SnapshotImage &source, QSize size)
{
    require(source.png.size() <= std::size_t(std::numeric_limits<int>::max()),
            "Snapshot exceeds the decoder byte limit.");
    auto image =
        QImage::fromData(reinterpret_cast<const uchar *>(source.png.data()), int(source.png.size()), "PNG");
    require(!image.isNull() && image.size() == QSize(int(source.extent.width), int(source.extent.height)),
            "Cannot decode a snapshot for GIF export.");
    if (image.size() != size)
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied)
                    .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    require(!image.isNull(), "Not enough memory to resize the GIF frame.");
    return image;
}
Color pixelColor(const uchar *pixel, const GifExportSettings &options)
{
    if (options.transparency)
        return {double(pixel[0]), double(pixel[1]), double(pixel[2])};
    const int alpha = pixel[3];
    return {double((pixel[0] * alpha + options.background.red() * (255 - alpha) + 127) / 255),
            double((pixel[1] * alpha + options.background.green() * (255 - alpha) + 127) / 255),
            double((pixel[2] * alpha + options.background.blue() * (255 - alpha) + 127) / 255)};
}

std::vector<Color> makePalette(const std::vector<Bin> &histogram, int maximum)
{
    std::vector<Sample> samples;
    for (const auto &bin : histogram)
        if (bin.count)
            samples.push_back({bin.count,
                               {bin.sum[0] / double(bin.count), bin.sum[1] / double(bin.count),
                                bin.sum[2] / double(bin.count)}});
    if (samples.empty())
        return {Color{0, 0, 0}};
    struct Box {
        std::vector<std::size_t> indices;
        int axis{};
        double score{};
    };
    const auto measure = [&](Box &box) {
        Color low{255, 255, 255}, high{0, 0, 0};
        std::uint64_t count{};
        for (auto i : box.indices) {
            count += samples[i].count;
            for (std::size_t c = 0; c < 3; ++c) {
                low[c] = std::min(low[c], samples[i].color[c]);
                high[c] = std::max(high[c], samples[i].color[c]);
            }
        }
        double range = -1;
        for (int c = 0; c < 3; ++c)
            if (high[std::size_t(c)] - low[std::size_t(c)] > range) {
                range = high[std::size_t(c)] - low[std::size_t(c)];
                box.axis = c;
            }
        box.score = box.indices.size() > 1 ? range * range * double(count) : -1;
    };
    Box first;
    first.indices.resize(samples.size());
    std::iota(first.indices.begin(), first.indices.end(), 0);
    measure(first);
    std::vector<Box> boxes;
    boxes.push_back(std::move(first));
    while (boxes.size() < std::size_t(maximum)) {
        auto found = std::max_element(boxes.begin(), boxes.end(),
                                      [](const auto &a, const auto &b) { return a.score < b.score; });
        if (found->score < 0)
            break;
        auto &ids = found->indices;
        const auto axis = std::size_t(found->axis);
        std::sort(ids.begin(), ids.end(), [&](auto a, auto b) {
            return samples[a].color[axis] != samples[b].color[axis]
                       ? samples[a].color[axis] < samples[b].color[axis]
                       : a < b;
        });
        std::uint64_t count = 0, half = 0;
        for (auto id : ids)
            count += samples[id].count;
        std::size_t split = 0;
        do {
            half += samples[ids[split++]].count;
        } while (split + 1 < ids.size() && half < count / 2);
        split = std::min(split, ids.size() - 1);
        Box right;
        right.indices.assign(ids.begin() + std::ptrdiff_t(split), ids.end());
        ids.resize(split);
        measure(*found);
        measure(right);
        boxes.push_back(std::move(right));
    }
    std::vector<Color> colors;
    for (const auto &box : boxes) {
        Color sum{};
        std::uint64_t count{};
        for (auto id : box.indices) {
            count += samples[id].count;
            for (std::size_t c = 0; c < 3; ++c)
                sum[c] += samples[id].color[c] * double(samples[id].count);
        }
        for (auto &channel : sum)
            channel = std::round(channel / double(count));
        colors.push_back(sum);
    }
    return colors;
}

class PaletteLookup
{
  public:
    PaletteLookup(const std::vector<Color> &colors, int start) : colors_(colors)
    {
        std::vector<int> indices(colors.size() - std::size_t(start));
        std::iota(indices.begin(), indices.end(), start);
        nodes_.reserve(indices.size());
        root_ = build(indices, 0, indices.size(), 0);
    }
    int nearest(const Color &color) const
    {
        int best = nodes_[std::size_t(root_)].color;
        double distance = 1e30;
        search(root_, color, best, distance);
        return best;
    }

  private:
    struct Node {
        int color{}, axis{}, left{-1}, right{-1};
    };
    int build(std::vector<int> &indices, std::size_t from, std::size_t to, int depth)
    {
        if (from == to)
            return -1;
        const auto mid = (from + to) / 2;
        const int axis = depth % 3;
        std::nth_element(indices.begin() + std::ptrdiff_t(from), indices.begin() + std::ptrdiff_t(mid),
                         indices.begin() + std::ptrdiff_t(to), [&](int a, int b) {
                             return colors_[std::size_t(a)][std::size_t(axis)] <
                                    colors_[std::size_t(b)][std::size_t(axis)];
                         });
        const int index = int(nodes_.size());
        nodes_.push_back({indices[mid], axis});
        const int left = build(indices, from, mid, depth + 1), right = build(indices, mid + 1, to, depth + 1);
        nodes_[std::size_t(index)].left = left;
        nodes_[std::size_t(index)].right = right;
        return index;
    }
    void search(int node, const Color &color, int &best, double &distance) const
    {
        if (node < 0)
            return;
        const auto &n = nodes_[std::size_t(node)];
        const auto &candidate = colors_[std::size_t(n.color)];
        double d = 0;
        for (std::size_t c = 0; c < 3; ++c)
            d += (color[c] - candidate[c]) * (color[c] - candidate[c]);
        if (d < distance || (d == distance && n.color < best)) {
            distance = d;
            best = n.color;
        }
        const double delta = color[std::size_t(n.axis)] - candidate[std::size_t(n.axis)];
        search(delta < 0 ? n.left : n.right, color, best, distance);
        if (delta * delta <= distance)
            search(delta < 0 ? n.right : n.left, color, best, distance);
    }
    const std::vector<Color> &colors_;
    std::vector<Node> nodes_;
    int root_{};
};

QByteArray indexImage(const QImage &image, const GifExportSettings &options,
                      const std::vector<Color> &palette, bool transparent, const std::atomic_bool &cancel)
{
    PaletteLookup lookup(palette, transparent ? 1 : 0);
    QByteArray indices(image.width() * image.height(), Qt::Uninitialized);
    std::vector<Color> current(std::size_t(image.width()) + 2), next(current.size());
    constexpr int bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    const double amplitude = 256 / std::cbrt(double(palette.size()));
    for (int y = 0; y < image.height(); ++y) {
        check(cancel);
        const auto *row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const auto *pixel = row + x * 4;
            const auto at = std::size_t(x) + 1;
            if (transparent && pixel[3] < 128) {
                indices[y * image.width() + x] = 0;
                continue;
            }
            auto color = pixelColor(pixel, options);
            double offset = 0;
            if (options.dither == GifDither::Pattern)
                offset = ((bayer[(y % 4) * 4 + x % 4] + .5) / 16 - .5) * amplitude;
            else if (options.dither == GifDither::Noise) {
                std::uint32_t hash = std::uint32_t(x) * 0x9e3779b9U + std::uint32_t(y) * 0x85ebca6bU;
                hash ^= hash >> 16;
                hash *= 0x7feb352dU;
                hash ^= hash >> 15;
                offset = (double(hash & 65535) / 65535 - .5) * amplitude; // Stable across frames.
            }
            for (std::size_t c = 0; c < 3; ++c)
                color[c] = std::clamp(color[c] + offset +
                                          (options.dither == GifDither::Diffusion ? current[at][c] : 0),
                                      0.0, 255.0);
            const auto index = lookup.nearest(color);
            indices[y * image.width() + x] = char(index);
            if (options.dither == GifDither::Diffusion)
                for (std::size_t c = 0; c < 3; ++c) {
                    const double error =
                        (color[c] - palette[std::size_t(index)][c]) * options.strength / 100.0;
                    current[at + 1][c] += error * 7 / 16;
                    next[at - 1][c] += error * 3 / 16;
                    next[at][c] += error * 5 / 16;
                    next[at + 1][c] += error / 16;
                }
        }
        current.swap(next);
        std::fill(next.begin(), next.end(), Color{});
    }
    return indices;
}
void byte(QByteArray &out, int value)
{
    out.append(char(value));
}
void word(QByteArray &out, int value)
{
    byte(out, value & 255);
    byte(out, (value >> 8) & 255);
}
void lzw(QByteArray &out, const QByteArray &indices, int minimum, const std::atomic_bool &cancel,
         qsizetype limit)
{
    byte(out, minimum);
    QByteArray block;
    block.reserve(255);
    std::uint32_t bits = 0;
    int bitCount = 0;
    const auto flush = [&] {
        if (!block.isEmpty()) {
            require(out.size() + block.size() + 2 <= limit,
                    "GIF exceeds the encoded-byte memory budget. Reduce its size or frame count.");
            byte(out, int(block.size()));
            out.append(block);
            block.clear();
        }
    };
    const auto writeCode = [&](int code, int width) {
        bits |= std::uint32_t(code) << bitCount;
        bitCount += width;
        while (bitCount >= 8) {
            block.append(char(bits & 255));
            bits >>= 8;
            bitCount -= 8;
            if (block.size() == 255)
                flush();
        }
    };
    const int clear = 1 << minimum, end = clear + 1;
    int width = minimum + 1, next = clear + 2;
    std::unordered_map<std::uint32_t, int> dictionary;
    dictionary.reserve(4096);
    writeCode(clear, width);
    int prefix = static_cast<uchar>(indices[0]);
    for (qsizetype i = 1; i < indices.size(); ++i) {
        if ((i & 16383) == 0)
            check(cancel);
        const int value = static_cast<uchar>(indices[i]);
        const auto key = (std::uint32_t(prefix) << 8) | std::uint32_t(value);
        const auto found = dictionary.find(key);
        if (found != dictionary.end()) {
            prefix = found->second;
            continue;
        }
        writeCode(prefix, width);
        if (next < 4096) {
            dictionary.emplace(key, next++);
            // The decoder's dictionary trails the encoder by one entry.
            if (next > (1 << width) && width < 12)
                ++width;
        } else {
            writeCode(clear, width);
            dictionary.clear();
            width = minimum + 1;
            next = clear + 2;
        }
        prefix = value;
    }
    writeCode(prefix, width);
    if (next == (1 << width) && width < 12)
        ++width;
    writeCode(end, width);
    if (bitCount)
        block.append(char(bits & 255));
    flush();
    byte(out, 0);
}
} // namespace

EncodedGif encodeGif(const core::SnapshotTimelineState &timeline, const GifExportSettings &options,
                     const std::atomic_bool &cancel, std::function<void(int)> progress)
try {
    require(options.size.width() > 0 && options.size.height() > 0 && options.size.width() <= 32768 &&
                options.size.height() <= 32768 &&
                std::uint64_t(options.size.width()) * std::uint64_t(options.size.height()) <=
                    32 * 1024 * 1024,
            "GIF output exceeds the image size limits.");
    require(options.colors >= 2 && options.colors <= 256 && options.strength >= 0 &&
                options.strength <= 100 && options.dither >= GifDither::None &&
                options.dither <= GifDither::Noise && options.background.isValid(),
            "Invalid GIF export settings.");
    std::vector<const core::SnapshotFrame *> frames;
    std::uint64_t largest = std::uint64_t(options.size.width()) * std::uint64_t(options.size.height()) * 4;
    for (const auto &frame : timeline.frames)
        if (frame.image) {
            frames.push_back(&frame);
            largest =
                std::max(largest, std::uint64_t(frame.image->extent.width) * frame.image->extent.height * 4);
            require(frame.durationMs > 0 && frame.durationMs <= 655350,
                    "GIF frame duration must be between 1 and 655350 ms.");
        }
    require(!frames.empty(), "Capture a frame before exporting GIF.");
    const auto working = largest * 5 + 64 * 1024 * 1024;
    const auto available = platform::availableWorkingMemoryBytes();
    require(working < available, "Not enough working memory to export this GIF.");
    const auto encodedLimit = qsizetype(std::min<std::uint64_t>(byteBudget, (available - working) / 2));
    std::vector<Bin> histogram(32768);
    std::map<std::uint32_t, std::uint64_t> exact;
    bool exactOverflow = false, transparent = false;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        check(cancel);
        auto image = sourceImage(*frames[i]->image, options.size);
        for (int y = 0; y < image.height(); ++y) {
            check(cancel);
            const auto *row = image.constScanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                const auto *pixel = row + 4 * x;
                if (options.transparency && pixel[3] < 128) {
                    transparent = true;
                    continue;
                }
                const auto color = pixelColor(pixel, options);
                const auto r = std::uint32_t(color[0]), g = std::uint32_t(color[1]),
                           b = std::uint32_t(color[2]);
                auto &bin = histogram[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
                ++bin.count;
                for (std::size_t c = 0; c < 3; ++c)
                    bin.sum[c] += color[c];
                if (!exactOverflow) {
                    ++exact[(r << 16) | (g << 8) | b];
                    if (exact.size() > 256) {
                        exactOverflow = true;
                        exact.clear();
                    }
                }
            }
        }
        if (progress)
            progress(int((i + 1) * 40 / frames.size()));
    }
    std::vector<Color> palette;
    if (!exactOverflow && exact.size() <= std::size_t(options.colors - (transparent ? 1 : 0))) {
        for (const auto &[rgb, count] : exact) {
            Q_UNUSED(count);
            palette.push_back({double((rgb >> 16) & 255), double((rgb >> 8) & 255), double(rgb & 255)});
        }
        if (palette.empty())
            palette.push_back({0, 0, 0});
    } else
        palette = makePalette(histogram, options.colors - (transparent ? 1 : 0));
    if (transparent)
        palette.insert(palette.begin(), Color{});
    int tableBits = 1;
    while ((1 << tableBits) < int(palette.size()))
        ++tableBits;
    EncodedGif result;
    auto &out = result.bytes;
    out.append("GIF89a", 6);
    word(out, options.size.width());
    word(out, options.size.height());
    byte(out, 0xf0 | (tableBits - 1));
    byte(out, 0);
    byte(out, 0);
    for (int i = 0; i < (1 << tableBits); ++i) {
        const auto color = std::size_t(i) < palette.size() ? palette[std::size_t(i)] : Color{};
        for (auto channel : color)
            byte(out, int(channel));
    }
    result.headerLength = out.size();
    if (options.loop)
        out.append(QByteArray::fromHex("21ff0b4e45545343415045322e300301000000"));
    std::uint64_t totalMs = 0, writtenTicks = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        check(cancel);
        auto image = sourceImage(*frames[i]->image, options.size);
        const auto indices = indexImage(image, options, palette, transparent, cancel);
        totalMs += frames[i]->durationMs;
        const auto desired = (totalMs + 5) / 10;
        const auto delay = std::max<std::uint64_t>(1, desired > writtenTicks ? desired - writtenTicks : 1);
        require(delay <= 65535, "GIF frame delay exceeds the format limit.");
        writtenTicks += delay;
        GifExportFrame frame{out.size(), 0, std::uint32_t(delay * 10)};
        byte(out, 0x21);
        byte(out, 0xf9);
        byte(out, 4);
        byte(out, 8 | (transparent ? 1 : 0));
        word(out, int(delay));
        byte(out, 0);
        byte(out, 0);
        byte(out, 0x2c);
        word(out, 0);
        word(out, 0);
        word(out, options.size.width());
        word(out, options.size.height());
        byte(out, 0);
        lzw(out, indices, std::max(2, tableBits), cancel, encodedLimit);
        frame.length = out.size() - frame.offset;
        result.frames.push_back(frame);
        if (progress)
            progress(40 + int((i + 1) * 60 / frames.size()));
    }
    byte(out, 0x3b);
    check(cancel);
    return result;
} catch (const Cancelled &) {
    EncodedGif result;
    result.cancelled = true;
    return result;
} catch (const std::exception &error) {
    EncodedGif result;
    result.error = QString::fromUtf8(error.what());
    return result;
}

QImage decodeGifPreview(const EncodedGif &encoded, std::size_t index, QSize maximum)
{
    if (!encoded || index >= encoded.frames.size())
        return {};
    const auto &frame = encoded.frames[index];
    if (encoded.headerLength < 13 || encoded.headerLength > encoded.bytes.size() ||
        frame.offset < encoded.headerLength || frame.length <= 0 || frame.length > encoded.bytes.size() ||
        frame.offset > encoded.bytes.size() - frame.length)
        return {};
    QByteArray single = encoded.bytes.left(encoded.headerLength);
    single.append(encoded.bytes.constData() + frame.offset, frame.length);
    single.append(char(0x3b));
    QBuffer buffer(&single);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "GIF");
    const auto original = reader.size();
    if (maximum.isValid() && (original.width() > maximum.width() || original.height() > maximum.height()))
        reader.setScaledSize(original.scaled(maximum, Qt::KeepAspectRatio));
    auto image = reader.read();
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
} // namespace imageeditor::ui
