#pragma once
#include "imageeditor/core/SnapshotTimeline.hpp"
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QSize>
#include <atomic>
#include <functional>

namespace imageeditor::ui
{
enum class GifDither { None, Diffusion, Pattern, Noise };
struct GifExportSettings {
    QSize size;
    int colors{256};
    GifDither dither{GifDither::Diffusion};
    int strength{100};
    bool transparency{true};
    QColor background{Qt::white};
    bool loop{true};
    bool operator==(const GifExportSettings &) const = default;
};
struct GifExportFrame {
    qsizetype offset{}, length{};
    std::uint32_t durationMs{};
};
struct EncodedGif {
    QByteArray bytes;
    qsizetype headerLength{};
    std::vector<GifExportFrame> frames;
    QString error;
    bool cancelled{};
    explicit operator bool() const
    {
        return !bytes.isEmpty() && error.isEmpty() && !cancelled;
    }
};
// Frozen snapshots only. One shared palette avoids inter-frame color flicker.
// Worker-safe; no document mutation, GUI objects, or filesystem publication.
EncodedGif encodeGif(const core::SnapshotTimelineState &, const GifExportSettings &,
                     const std::atomic_bool &cancel, std::function<void(int)> progress = {});
// Decode one independently stored frame from the actual GIF bytes, not a
// pristine source render. No replay of earlier frames or all-frame RGBA cache.
QImage decodeGifPreview(const EncodedGif &, std::size_t frame, QSize maximumSize = {});
} // namespace imageeditor::ui
