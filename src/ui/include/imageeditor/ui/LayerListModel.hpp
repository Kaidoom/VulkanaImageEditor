#pragma once

#include "imageeditor/core/EditorSession.hpp"

#include <QAbstractListModel>
#include <QStringList>

#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <map>
#include <QIcon>
#include <QElapsedTimer>

class QMimeData;

namespace imageeditor::ui {

class LayerListModel final : public QAbstractListModel {
public:
    // A folder containing a selected item, not an additional selected row.
    static constexpr int SelectedDescendantRole = Qt::UserRole + 7;
    explicit LayerListModel(QObject* parent = nullptr);

    void setSession(core::EditorSession* session);
    void refresh();
    void refreshSelectionIndicators();
    void toggleExpanded(core::LayerId);
    [[nodiscard]] bool expanded(core::LayerId) const;
    [[nodiscard]] std::vector<core::LayerId> collapsedFolderIds() const;
    // UI state only: load before the caller's normal, selection-blocked refresh.
    void restoreCollapsedFolderIds(std::span<const core::LayerId>);
    [[nodiscard]] int rowIndent(int row) const;
    [[nodiscard]] bool hasMultipleSelectedItems() const;
    // Reveal folder ancestors for inline editing without changing selection
    // or history. Group members remain represented by their group row.
    bool revealItem(core::LayerId);
    bool renameItem(core::LayerId, const QString&);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] QStringList mimeTypes() const override;
    [[nodiscard]] QMimeData* mimeData(const QModelIndexList& indexes) const override;
    [[nodiscard]] bool canDropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) const override;
    bool dropMimeData(const QMimeData* data, Qt::DropAction action,
        int row, int column, const QModelIndex& parent) override;
    [[nodiscard]] Qt::DropActions supportedDropActions() const override;
    [[nodiscard]] Qt::DropActions supportedDragActions() const override;

    [[nodiscard]] std::optional<core::LayerId> layerIdAt(int row) const;
    [[nodiscard]] int rowForLayer(core::LayerId layerId) const;

    std::function<bool(core::LayerId, bool)> onVisibilityChanged;
    // Destination is the layer's final index in Document's bottom-to-top order.
    std::function<bool(core::LayerId, std::size_t)> onMoveRequested;
    std::function<bool(std::vector<core::LayerId>, core::ItemPlacement)> onReparentRequested;
    std::function<void()> onExpansionChanged;
    std::function<bool(core::LayerId, const QString&)> onRenameRequested;
    std::function<QMimeData*(std::span<const core::LayerId>)> onCaptureTransfer;
    std::function<bool(const QMimeData*)> onCanTransfer;
    std::function<bool(const QMimeData*, core::ItemPlacement)> onTransfer;

private:
    [[nodiscard]] const core::Layer* layerAt(int row) const;

    core::EditorSession* session_ {nullptr};
    struct Row { core::LayerId id; int depth; };
    std::vector<Row> rows_;
    std::set<core::LayerId> collapsed_;
    std::set<core::LayerId> selectedAncestorFolders_;
    struct Thumbnail { std::size_t stamp; std::size_t appearanceStamp; QIcon icon; qint64 renderedAt; };
    mutable std::map<core::LayerId, Thumbnail> thumbnails_;
    QElapsedTimer thumbnailClock_;
    mutable bool thumbnailRefreshQueued_{false};
    std::uint64_t sessionGeneration_ {0};
    void rebuildRows();
    [[nodiscard]] std::set<core::LayerId> selectedAncestorFolders() const;
    [[nodiscard]] QIcon groupThumbnail(core::LayerId) const;
    [[nodiscard]] std::optional<core::ItemPlacement> dropPlacement(int row, const QModelIndex&) const;
};

} // namespace imageeditor::ui
