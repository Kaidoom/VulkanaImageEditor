#pragma once

#include <cstddef>
#include <memory>

namespace imageeditor::core {

// Immutable, session-only provenance for a selection-producing tool. It is
// deliberately absent from render snapshots and project serialization. History
// restores it atomically with the mask, without callbacks into live UI objects.
class SelectionEvidence {
public:
    virtual ~SelectionEvidence() = default;
    [[nodiscard]] virtual bool equivalent(const SelectionEvidence&) const noexcept = 0;
    [[nodiscard]] virtual std::size_t memoryCost() const noexcept = 0;
};
using SelectionEvidenceState = std::shared_ptr<const SelectionEvidence>;

} // namespace imageeditor::core
