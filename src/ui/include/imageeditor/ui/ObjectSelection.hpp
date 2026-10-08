#pragma once
#include "imageeditor/core/SmartSelection.hpp"
#include "imageeditor/core/SmartSelectionEvidence.hpp"
#include <QString>
#include <memory>

namespace imageeditor::ui {
struct ObjectSelectionPoint {
    core::Vec2d position;
    bool include;
};
struct ObjectSelectionPrompt {
    core::RectD box;
    std::vector<ObjectSelectionPoint> corrections;
};
class ObjectSelectionEvidence final : public core::SelectionEvidence {
public:
    ObjectSelectionEvidence(const core::Document& document, std::optional<core::LayerId> layer,
        core::ColorSampleSource source, ObjectSelectionPrompt value, core::SelectionState baseline,
        core::SelectionOperation operation, std::uint64_t documentInstance)
        : identity(document, layer, source, { }, documentInstance)
        , prompt(std::move(value))
        , original(std::move(baseline))
        , operation(operation)
    {
    }
    core::SmartSelectionEvidence identity;
    ObjectSelectionPrompt prompt;
    core::SelectionState original;
    core::SelectionOperation operation;
    bool equivalent(const core::SelectionEvidence& other) const noexcept override;
    std::size_t memoryCost() const noexcept override;
};
// One worker-owned local runtime per window, one revision-pinned feature cache.
// Bundled CPU inference; no Python, network or GPU driver changes.
class ObjectSelectionEngine {
public:
    ObjectSelectionEngine();
    explicit ObjectSelectionEngine(QString directory);
    ~ObjectSelectionEngine();
    [[nodiscard]] core::SelectionState evaluate(std::shared_ptr<const core::SmartReferenceImage>,
        const ObjectSelectionPrompt&, const std::atomic_bool& cancelled);
    [[nodiscard]] std::size_t encodedImageCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
