#pragma once
#include "imageeditor/core/SmartSelection.hpp"
namespace imageeditor::core {
// Native-resolution boundary classification, not alpha matting. Low-resolution
// model logits supply a prior; visible source RGB supplies local edge evidence.
// Ambiguous/one-class neighbourhoods retain the prior. No component/hole cleanup.
std::vector<std::uint8_t> recoverObjectSelectionBoundary(
    const SmartReferenceImage&, std::span<const float> logits, const std::atomic_bool& cancelled);
}
