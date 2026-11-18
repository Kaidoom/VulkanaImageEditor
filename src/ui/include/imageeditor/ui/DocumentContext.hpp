#pragma once
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/ViewportState.hpp"
#include "imageeditor/core/Measurement.hpp"
#include "imageeditor/ui/ImageExport.hpp"
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <map>
#include <set>

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
    // Canvas dimensions when the export draft was last stored. Custom output
    // sizes remain valid until the document boundary changes.
    QSize exportCanvasSize;
    core::SnapshotFrameId selectedSnapshot {1}; // Session-only timeline selection.
    // Unpublished editing workspace, owned by this runtime tab only.
    std::shared_ptr<RefinementWorkspace> refinement;
    std::uint64_t cancellationGeneration {0};
    std::uint64_t lastActivated {0};
    // Observe a source once per committed state, even if its parent later
    // undoes an automatic refresh. History remains navigable across tabs.
    std::map<QString, std::pair<DocumentInstanceId, std::uint64_t>> observedProjectSources;
    std::set<QString> projectSourcesToReload;
    bool discardingProjectEdits{};
    std::weak_ptr<core::RasterSurface> lastProjectAppearance;
    std::uint64_t lastProjectAppearanceState{};
private:
    inline static std::atomic<DocumentInstanceId> nextId {1};
};
} // namespace imageeditor::ui
