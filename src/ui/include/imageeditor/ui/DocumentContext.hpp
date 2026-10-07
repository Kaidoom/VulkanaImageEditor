#pragma once
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/ViewportState.hpp"
#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include <QJsonObject>
#include <QString>
#include <atomic>

namespace imageeditor::ui {
using DocumentInstanceId = std::uint64_t;
struct RefinementWorkspace;

// Runtime ownership only. Neither tab order nor instance IDs enter .vulkana.
struct DocumentContext final {
    DocumentContext() : id(nextId.fetch_add(1, std::memory_order_relaxed)) {}
    const DocumentInstanceId id;
    core::EditorSession session;
    QJsonObject metadata;
    QString projectPath, sourcePath, displayName {QStringLiteral("Untitled")};
    bool untouched {false}, viewInitialized {false}, closed {false};
    core::ViewportState view;
    std::optional<core::CloneAnchor> cloneAnchor;
    std::optional<core::MeasurementLine> measurement;
    std::vector<core::LayerId> collapsedFolders;
    std::optional<ExportSettings> exportSettings;
    // Unpublished editing workspace, owned by this runtime tab only.
    std::shared_ptr<RefinementWorkspace> refinement;
    std::uint64_t cancellationGeneration {0};
    std::uint64_t lastActivated {0};
private:
    inline static std::atomic<DocumentInstanceId> nextId {1};
};
} // namespace imageeditor::ui
