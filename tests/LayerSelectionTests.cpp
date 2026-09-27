#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/ShapeCommands.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

using namespace imageeditor::core;

namespace {
int failures = 0;
#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; \
            ++failures;                                                    \
        }                                                                  \
    } while (false)

struct Fixture {
    EditorSession session;
    std::array<LayerId, 5> ids;
    Fixture()
    {
        auto document = std::make_unique<Document>(CanvasSpec { .extent = { 32, 32 } });
        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto layer = Layer::shape("Shape " + std::to_string(i), ShapeLayer { });
            ids[i] = layer.id;
            CHECK(document->insertLayer(i, std::move(layer)));
        }
        session.replaceDocument(std::move(document));
        session.document()->markSaved();
    }
    std::array<LayerId, 5> displayed() const
    {
        auto rows = ids;
        std::ranges::reverse(rows);
        return rows;
    }
};

void togglesAndPrimary()
{
    Fixture f;
    auto& s = f.session;
    CHECK(s.activeLayer() == f.ids[4]);
    CHECK(s.selectedLayers() == std::vector<LayerId> { f.ids[4] });
    s.setActiveLayer(f.ids[0]);
    s.toggleSelectedLayer(f.ids[2]);
    CHECK(s.isLayerSelected(f.ids[0]) && s.isLayerSelected(f.ids[2]));
    CHECK(s.activeLayer() == f.ids[2]);
    CHECK(s.selectionAnchor() == f.ids[2]);
    s.toggleSelectedLayer(f.ids[2]);
    CHECK(s.activeLayer() == f.ids[0]);
    CHECK(s.selectionAnchor() == f.ids[2]); // A toggled-off anchor is valid.
    s.toggleSelectedLayer(f.ids[0]);
    CHECK(s.selectedLayers().empty());
    CHECK(!s.activeLayer());
    CHECK(!s.undo());
    CHECK(s.selectedLayers().empty()); // Unrelated history cannot repopulate it.
    s.toggleSelectedLayer(0);
    CHECK(s.selectedLayers().empty());
    s.setActiveLayer(f.ids[1]);
    s.setActiveLayer(0);
    CHECK(s.activeLayer() == f.ids[1]);
    s.setActiveLayer({ });
    CHECK(s.selectedLayers().empty() && !s.selectionAnchor());
}

void rangesFollowDisplayedOrder()
{
    Fixture f;
    auto& s = f.session;
    s.setActiveLayer(f.ids[3]);
    s.selectLayerRange(f.ids[1], f.displayed());
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[3], f.ids[2], f.ids[1] }));
    CHECK(s.activeLayer() == f.ids[1]);
    CHECK(s.selectionAnchor() == f.ids[3]);
    s.selectLayerRange(f.ids[4], f.displayed());
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[4], f.ids[3] }));
    CHECK(s.selectionAnchor() == f.ids[3]);
    s.selectLayerRange(f.ids[0], f.displayed(), true);
    CHECK(s.selectedLayers().size() == 5);
    CHECK(s.activeLayer() == f.ids[0]);

    // Reordering alters range membership, never existing selection identity.
    CHECK(s.execute(std::make_unique<MoveLayerCommand>(f.ids[2], 4)));
    CHECK(s.selectedLayers().size() == 5);
    const std::array reordered { f.ids[2], f.ids[4], f.ids[3], f.ids[1], f.ids[0] };
    s.selectLayerRange(f.ids[4], reordered);
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[4], f.ids[3] }));
    CHECK(s.undo());
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[4], f.ids[3] }));
}

void selectionIsOnlySessionState()
{
    Fixture f;
    auto& s = f.session;
    CHECK(s.execute(std::make_unique<SetLayerOpacityCommand>(f.ids[0], 0.5F)));
    CHECK(s.undo());
    const auto revision = s.document()->revision();
    const auto content = s.document()->contentState();
    const auto memory = s.history().memoryUsed();
    const auto redo = s.history().redoDepth();
    s.setActiveLayer(f.ids[0]);
    s.toggleSelectedLayer(f.ids[2]);
    s.selectLayerRange(f.ids[4], f.displayed());
    const std::array dirtyIds { f.ids[1], f.ids[1], LayerId { 0 }, f.ids[3] };
    s.setLayerSelection(dirtyIds, f.ids[1], f.ids[0]);
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[1], f.ids[3] }));
    CHECK(s.activeLayer() == f.ids[1] && s.selectionAnchor() == f.ids[0]);
    s.setLayerSelection(s.selectedLayers(), f.ids[3], f.ids[0]); // Aliased input.
    CHECK(s.activeLayer() == f.ids[3]);
    CHECK(s.document()->revision() == revision);
    CHECK(s.document()->contentState() == content);
    CHECK(!s.document()->isModified());
    CHECK(s.history().memoryUsed() == memory && s.history().redoDepth() == redo);
    CHECK(!s.history().canUndo());
    CHECK(s.redo());
    CHECK(s.selectedLayers().size() == 2);
}

void deletionAndHints()
{
    Fixture f;
    auto& s = f.session;
    const std::array group { f.ids[0], f.ids[2], f.ids[4] };
    s.setLayerSelection(group, f.ids[2], f.ids[2]);
    CHECK(s.execute(std::make_unique<RemoveLayerCommand>(f.ids[2])));
    CHECK((s.selectedLayers() == std::vector<LayerId> { f.ids[0], f.ids[4] }));
    CHECK(s.activeLayer() == f.ids[4] && s.selectionAnchor() == f.ids[4]);
    CHECK(s.undo());
    CHECK(!s.isLayerSelected(f.ids[2])); // History does not resurrect session selection.

    const auto before = std::get<ShapeLayer>(s.document()->layer(f.ids[0])->payload);
    auto after = before;
    after.size.width = before.size.width + 17;
    CHECK(s.execute(std::make_unique<SetShapeCommand>(f.ids[0], before, after)));
    CHECK(s.activeLayer() == f.ids[0] && s.selectedLayers().size() == 2);
    CHECK(s.undo());
    CHECK(s.activeLayer() == f.ids[0] && s.selectedLayers().size() == 2);
    CHECK(s.redo());
    CHECK(s.selectedLayers().size() == 2);

    auto added = Layer::shape("New", ShapeLayer { });
    const auto addedId = added.id;
    CHECK(s.execute(std::make_unique<AddLayerCommand>(added, 5, f.ids[0])));
    CHECK(s.selectedLayers() == std::vector<LayerId> { addedId });
    CHECK(s.undo());
    CHECK(s.selectedLayers() == std::vector<LayerId> { f.ids[0] });
    CHECK(s.redo());
    CHECK(s.selectedLayers() == std::vector<LayerId> { addedId });
    CHECK(s.execute(std::make_unique<RemoveLayerCommand>(addedId)));
    CHECK(s.activeLayer() == f.ids[4]);
}

void externalRemovalIsNormalizedEvenWhenAnActionFails()
{
    Fixture f;
    auto& s = f.session;
    const std::array selected { f.ids[0], f.ids[1] };
    s.setLayerSelection(selected, f.ids[1], f.ids[1]);
    CHECK(s.document()->takeLayer(f.ids[1]).has_value());
    CHECK(!s.execute({ }));
    CHECK(s.selectedLayers() == std::vector<LayerId> { f.ids[0] });
    CHECK(s.activeLayer() == f.ids[0] && s.selectionAnchor() == f.ids[0]);
    CHECK(s.document()->takeLayer(f.ids[0]).has_value());
    CHECK(!s.adoptApplied({ }));
    CHECK(s.activeLayer() == f.ids[4]);
    CHECK(s.document()->takeLayer(f.ids[4]).has_value());
    CHECK(!s.redo());
    CHECK(s.activeLayer() == f.ids[3]);
    CHECK(s.document()->takeLayer(f.ids[3]).has_value());
    CHECK(!s.undo());
    CHECK(s.activeLayer() == f.ids[2]);
    s.replaceDocument({ });
    CHECK(s.selectedLayers().empty() && !s.activeLayer() && !s.selectionAnchor());
}
}

int main()
{
    togglesAndPrimary();
    rangesFollowDisplayedOrder();
    selectionIsOnlySessionState();
    deletionAndHints();
    externalRemovalIsNormalizedEvenWhenAnActionFails();
    if (failures)
        return EXIT_FAILURE;
    std::cout << "Layer selection tests passed\n";
    return EXIT_SUCCESS;
}
