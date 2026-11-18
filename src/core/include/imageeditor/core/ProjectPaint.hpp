#pragma once

#include "imageeditor/core/RasterEditTransaction.hpp"
#include <array>
#include <map>

namespace imageeditor::core {

// Coordinates are source-local. A smaller refreshed source clips the view but
// keeps the paint intact for a subsequent refresh at its original dimensions.
struct ProjectPaint {
    static constexpr int tileSide = 64;
    using Tile = std::array<Rgba8, tileSide * tileSide>;
    using Tiles = std::map<std::pair<int, int>, std::shared_ptr<const Tile>>;
    Tiles tiles;
};
using ProjectPaintState = std::shared_ptr<const ProjectPaint>;

ProjectPaintState projectPaintState(const std::shared_ptr<RasterSurface>& combined);
std::shared_ptr<RasterSurface> projectBasePixels(const std::shared_ptr<RasterSurface>& combined);
std::shared_ptr<RasterSurface> withProjectPaint(std::shared_ptr<RasterSurface> base, ProjectPaintState paint);
bool hasProjectPaint(const Layer&);
std::unique_ptr<Command> makeProjectPaintCommand(
    const Document&, LayerId, ProjectPaintState after, std::string label);
std::unique_ptr<Command> makeProjectPaintClearCommand(const Document&, LayerId, bool useSelection);

class ProjectPaintEdit final {
public:
    ProjectPaintEdit(Document&, LayerId, std::string label);
    ~ProjectPaintEdit();
    ProjectPaintEdit(const ProjectPaintEdit&) = delete;
    ProjectPaintEdit& operator=(const ProjectPaintEdit&) = delete;
    Document& proxy() noexcept { return proxy_; }
    History& provisionalHistory() noexcept { return provisional_; }
    RasterEditCommitResult commit(History&);
    void cancel() noexcept;
    bool valid() const noexcept;

private:
    class SurfaceProxy;
    std::shared_ptr<SurfaceProxy> editable_;
    std::shared_ptr<const ProjectLayerSource> source_;
    Document& owner_;
    LayerId id_;
    std::shared_ptr<RasterSurface> before_, current_;
    AffineTransform transform_;
    SelectionState selection_;
    std::string label_;
    Document proxy_;
    History provisional_;
    bool finished_ { false };
};

} // namespace imageeditor::core
