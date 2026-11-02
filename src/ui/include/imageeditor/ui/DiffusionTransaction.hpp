#pragma once

#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"

#include <QImage>
#include <QString>
#include <memory>
#include <optional>

namespace imageeditor::ui {

// Frozen native raster pixels and the geometry needed to map document-space
// selections into that grid. Render caches are never treated as source pixels.
struct DiffusionTarget {
    const core::Document* owner {};
    core::Revision documentRevision {}, selectionRevision {}, sourceRevision {};
    std::uint64_t contentState {};
    core::LayerId layerId {};
    core::SurfaceId sourceId {};
    core::CanvasSpec canvas;
    core::AffineTransform localToDocument;
    core::Vec2d rasterOrigin;
    QImage originalPixels;
    core::SelectionState selection;
};

// Call on the owning thread after finishing existing gestures. The returned
// QImage owns its pixels; immutable selection tiles can safely be shared.
[[nodiscard]] DiffusionTarget captureDiffusionTarget(
    const core::Document&, core::LayerId, bool captureSelection);
[[nodiscard]] bool diffusionTargetCurrent(
    const core::Document&, const DiffusionTarget&, QString* error = nullptr);
[[nodiscard]] QImage diffusionRawLayer(const core::Layer&);
// Isolated appearance over transparency. Backdrop-dependent blend modes can
// differ from the visible canvas; use flattenDocument for a full composite.
[[nodiscard]] FlattenedDocumentResult diffusionRenderedLayer(
    const core::Document&, core::LayerId);

// Native layer-grid coverage (or document grid when requested), resized only
// for inference. Full-size coverage
// is retained when applying; no thresholding discards soft selection values.
[[nodiscard]] QImage diffusionSelectionMask(const DiffusionTarget&, QSize outputSize = {}, bool documentSpace = false);
// Resizes generated pixels to the original native grid and composites through
// the captured selection. Pixels with zero coverage are copied byte-for-byte.
[[nodiscard]] QImage diffusionResolvedPixels(
    const DiffusionTarget&, const QImage& generated, bool resultInDocumentSpace, bool useSelection);
[[nodiscard]] QImage diffusionCompositeSelection(
    const QImage& before, const QImage& generated, const QImage& coverage);

// Full-canvas Fill preview. New-layer output uses the document's compositor:
// generated transparency exposes the existing canvas instead of erasing it.
[[nodiscard]] QImage diffusionFillPreview(const DiffusionTarget&, const QImage& before,
    const QImage& generated, bool newLayer);

// No mutation occurs while constructing this command. Its first apply checks
// target freshness again before the caller executes it through the owning
// EditorSession. Inference itself does not lock the document.
[[nodiscard]] std::unique_ptr<core::Command> diffusionReplaceCommand(
    const core::Document&, const DiffusionTarget&, const QImage& generated,
    bool resultInDocumentSpace, bool useSelection, QString* error = nullptr);
// Native new-layer output retains the target's pixel frame and transform.
// Document-space output occupies the canvas grid with an identity transform.
// Selection-limited new layers are transparent outside the captured region.
// A null target produces an ordinary identity-transform raster layer.
[[nodiscard]] std::optional<core::Layer> diffusionResultLayer(
    const DiffusionTarget* target, const QImage& generated, bool resultInDocumentSpace, bool useSelection,
    const QString& name, QString* error = nullptr);

} // namespace imageeditor::ui
