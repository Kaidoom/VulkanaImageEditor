#pragma once

#include "imageeditor/core/RasterEditTransaction.hpp"

namespace imageeditor::core {
// Values and edit coverage are separate, never premultiplied. This payload is
// independent of the source document and retains its document-space placement.
struct ScalarFragment {
    Extent2u extent;
    AffineTransform pixelsToDocument;
    std::vector<std::byte> values;
    std::vector<std::byte> coverage;
};
[[nodiscard]] bool validScalarFragment(const ScalarFragment&) noexcept;
[[nodiscard]] std::optional<ScalarFragment> captureScalarFragment(const Document&, LayerId);
[[nodiscard]] RasterEditCommitResult pasteScalarFragment(Document&, LayerId, const ScalarFragment&, History&);
} // namespace imageeditor::core
