#include "imageeditor/core/AdjustmentCommands.hpp"
#include "imageeditor/core/BasicPixelBrushStroke.hpp"
#include "imageeditor/core/CloneStroke.hpp"
#include "imageeditor/core/FillOperation.hpp"
#include "imageeditor/core/FilterCommands.hpp"
#include "imageeditor/core/LayerMaskEdit.hpp"
#include "imageeditor/core/LocalBlurStroke.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/core/SpotHealStroke.hpp"
#include "imageeditor/core/SelectedPixelTransform.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
using namespace imageeditor::core;
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; \
    std::cerr << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

std::vector<std::byte> bytes(const RasterSurface& surface)
{
    const auto extent = surface.extent();
    std::vector<std::byte> result(std::size_t(extent.width) * extent.height * 4);
    surface.copyRgba8({0, 0, int(extent.width), int(extent.height)}, result,
        std::size_t(extent.width) * 4);
    return result;
}

void readOnlyContentAndEditablePromotion()
{
    Document document({{32, 24}});
    auto surface = std::make_shared<ContiguousRasterSurface>(Extent2u{32, 24}, Rgba8{25, 80, 150, 255});
    auto source = Layer::raster("Original", surface);
    const auto sourceId = source.id;
    source.mask = std::make_shared<LayerMask>(LayerMask{SelectionMask::filled({32, 24}, 220), {}, 255, true});
    CHECK(document.insertLayer(0, std::move(source)));
    History history;
    CHECK(history.execute(document, createMirroredLayer(document, {{sourceId}, sourceId, sourceId})));
    const auto mirrorId = *history.layerSelectionHint()->primary;
    CHECK(isMirroredLayer(*document.layer(mirrorId)));
    const auto originalBytes = bytes(*surface);
    const auto documentRevision = document.revision();
    const auto pixelRevision = surface->revision();
    const auto undoDepth = history.undoDepth();
    auto settings = proceduralBrushPreset(ProceduralBrushPreset::HardRound);
    settings.sizePixels = 8;
    NormalizedPointerSample sample{.documentPosition = {10.5, 10.5}, .pressure = 1,
        .pointerType = PointerType::Pen, .buttons = PointerButtonPrimary};

    {
        RasterEditTransaction edit(document, mirrorId, "Forbidden paint");
        CHECK(!edit.active());
        CHECK(!edit.capture({0, 0, 1, 1}));
        CHECK(edit.commit(history) == RasterEditCommitResult::TargetUnavailable);
    }
    for (const auto mode : {BrushCompositeMode::Paint, BrushCompositeMode::Erase,
                           BrushCompositeMode::FilterColor}) {
        BasicPixelBrushStroke stroke(document, mirrorId, settings, mode);
        CHECK(!stroke.valid());
        CHECK(!stroke.begin(sample));
    }
    for (const bool erase : {false, true}) {
        FillOptions options;
        options.color = {255, 0, 0, 255};
        options.eraseSelection = erase;
        FillOperation fill(document, mirrorId, options);
        CHECK(fill.state() == FillState::Failed);
        CHECK(fill.error().find("read-only") != std::string_view::npos);
    }
    {
        bool rejected = false;
        try { LayerMaskEdit edit(document, mirrorId, "Forbidden mask"); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    {
        bool rejected = false;
        try { SelectedPixelTransformSession edit(document, mirrorId); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    {
        AdjustmentEditTransaction adjustments(document, mirrorId);
        FilterEditTransaction filters(document, mirrorId);
        CHECK(!adjustments.active());
        CHECK(!filters.active());
    }
    std::string error;
    auto reference = CloneReference::capture(document, mirrorId, sourceId,
        CloneSampleSource::SourceLayer, error);
    CHECK(reference);
    if (reference) {
        for (const auto mode : {CloneMode::Stamp, CloneMode::Heal}) {
            CloneSettings cloneSettings;
            cloneSettings.mode = mode;
            CloneStroke stroke(document, mirrorId, settings, cloneSettings, *reference, {});
            CHECK(!stroke.begin(sample));
            CHECK(stroke.diagnostic().find("read-only") != std::string::npos);
        }
        SpotHealStroke heal(document, mirrorId, settings, {}, *reference);
        CHECK(!heal.begin(sample));
        CHECK(heal.diagnostic().find("read-only") != std::string::npos);
    }
    {
        LocalBlurStroke blur(document, mirrorId, settings, {});
        CHECK(!blur.begin(sample));
        CHECK(blur.filterStats().snapshotBytes == 0);
    }
    // A mirror can still supply clone/eyedropper/reference pixels.
    auto mirrorReference = CloneReference::capture(document, sourceId, mirrorId,
        CloneSampleSource::SourceLayer, error);
    CHECK(mirrorReference && mirrorReference->validSample({10.5, 10.5}));
    auto originalReference = CloneReference::capture(document, sourceId, sourceId,
        CloneSampleSource::SourceLayer, error);
    CHECK(mirrorReference && originalReference
        && mirrorReference->snapshotBytes() == originalReference->snapshotBytes());
    CHECK(document.revision() == documentRevision);
    CHECK(surface->revision() == pixelRevision);
    CHECK(history.undoDepth() == undoDepth);
    CHECK(bytes(*surface) == originalBytes);

    CHECK(history.execute(document, promoteMirroredLayer(document, {{mirrorId}, mirrorId, mirrorId})));
    CHECK(!isMirroredLayer(*document.layer(mirrorId)));
    {
        RasterEditTransaction edit(document, mirrorId, "Independent paint");
        CHECK(edit.active());
    }
    {
        BasicPixelBrushStroke stroke(document, mirrorId, settings);
        CHECK(stroke.valid());
        CHECK(stroke.begin(sample));
        stroke.cancel();
    }
    CHECK(bytes(*surface) == originalBytes);
    CHECK(history.undo(document));
    CHECK(isMirroredLayer(*document.layer(mirrorId)));
    RasterEditTransaction restored(document, mirrorId, "Forbidden paint");
    CHECK(!restored.active());
}
}

int main()
{
    try { readOnlyContentAndEditablePromotion(); }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
    if (failures) std::cerr << failures << " mirrored editing checks failed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
