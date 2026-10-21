#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/History.hpp"
#include "imageeditor/core/SnapshotTimeline.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>

namespace c = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #__VA_ARGS__ "\n"; } } while (false)

std::shared_ptr<const c::SnapshotImage> image()
{
    auto result = std::make_shared<c::SnapshotImage>();
    result->extent = {32, 24};
    result->dotsPerInch = 300;
    // Core is intentionally independent of PNG codecs; byte validation belongs
    // to project I/O and the capture boundary, tested by the project suite.
    result->png.assign(2048, std::byte{42});
    return result;
}

void historyAndIndependence()
{
    c::Document document({{32, 24}, 96});
    auto pixels = std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{32, 24}, c::Rgba8{20, 40, 60, 255});
    auto layer = c::Layer::raster("Live image", pixels);
    CHECK(document.insertLayer(0, layer));
    const auto selected = c::SelectionMask::filled({32, 24}, 80);
    CHECK(document.setSelection(selected));
    document.markSaved();
    c::History history;
    const auto initial = document.snapshotTimeline();
    CHECK(initial.frames.size() == 1 && initial.frames[0].id == 1
        && initial.frames[0].durationMs == 100 && !initial.frames[0].image && initial.nextId == 2);
    const auto selectionRevision = document.selectionRevision();
    const auto rasterRevision = pixels->revision();
    const auto layerRevision = document.layer(layer.id)->effectRevision;
    auto captured = initial;
    captured.frames[0].image = image();
    auto command = std::make_unique<c::ChangeSnapshotTimelineCommand>(initial, captured, "Capture canvas snapshot");
    CHECK(command->memoryCost() >= captured.frames[0].image->png.size());
    CHECK(history.execute(document, std::move(command)));
    CHECK(document.isModified() && history.undoDepth() == 1);
    CHECK(document.snapshotTimeline() == captured);
    CHECK(document.selection() == selected && document.selectionRevision() == selectionRevision);
    CHECK(document.layer(layer.id)->effectRevision == layerRevision && pixels->revision() == rasterRevision);
    CHECK(document.canvas().extent == c::Extent2u{32, 24});
    CHECK(history.undo(document));
    CHECK(!document.isModified() && document.snapshotTimeline() == initial);
    CHECK(history.redo(document));
    CHECK(document.isModified() && document.snapshotTimeline().frames[0].image == captured.frames[0].image);

    auto added = captured;
    added.frames.push_back({added.nextId++, 250, {}});
    CHECK(history.execute(document, std::make_unique<c::ChangeSnapshotTimelineCommand>(captured, added, "Add snapshot frame")));
    auto reordered = added;
    std::reverse(reordered.frames.begin(), reordered.frames.end());
    auto reorder = std::make_unique<c::ChangeSnapshotTimelineCommand>(added, reordered, "Reorder snapshots");
    CHECK(reorder->memoryCost() < captured.frames[0].image->png.size());
    CHECK(history.execute(document, std::move(reorder)));
    CHECK(document.snapshotTimeline().frames[1].image == captured.frames[0].image);
    auto duration = reordered;
    duration.frames[0].durationMs = std::numeric_limits<std::uint32_t>::max();
    auto retime = std::make_unique<c::ChangeSnapshotTimelineCommand>(reordered, duration, "Change frame duration");
    CHECK(retime->memoryCost() < captured.frames[0].image->png.size());
    CHECK(history.execute(document, std::move(retime)));
    auto deleted = duration;
    deleted.frames.erase(deleted.frames.begin() + 1);
    auto remove = std::make_unique<c::ChangeSnapshotTimelineCommand>(duration, deleted, "Delete snapshot frame");
    CHECK(remove->memoryCost() >= captured.frames[0].image->png.size());
    CHECK(history.execute(document, std::move(remove)));
    CHECK(document.snapshotTimeline().frames.size() == 1 && !document.snapshotTimeline().frames[0].image);
    CHECK(history.undo(document) && document.snapshotTimeline() == duration);
    CHECK(history.undo(document) && document.snapshotTimeline() == reordered);
    CHECK(history.undo(document) && document.snapshotTimeline() == added);
    CHECK(history.undo(document) && document.snapshotTimeline() == captured);

    // Live document edits never mutate or resize frozen capture bytes.
    CHECK(document.setCanvas({{11, 17}, 72}));
    CHECK(document.setLayerOpacity(layer.id, .4F));
    CHECK(document.snapshotTimeline().frames[0].image == captured.frames[0].image);
    CHECK(document.snapshotTimeline().frames[0].image->extent == c::Extent2u{32, 24});
    CHECK(document.snapshotTimeline().frames[0].image->dotsPerInch == 300);
    CHECK(document.snapshotTimeline().frames[0].image->png == std::vector<std::byte>(2048, std::byte{42}));
}

void atomicAdmission()
{
    c::Document document({{1, 1}, 96});
    const auto initial = document.snapshotTimeline();
    const auto revision = document.revision();
    auto invalid = initial;
    invalid.frames.clear();
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    invalid = initial; invalid.frames[0].durationMs = 0;
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    invalid = initial; invalid.frames[0].id = 0;
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    invalid = initial; invalid.nextId = 1;
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    invalid = initial; invalid.frames.push_back(invalid.frames[0]);
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    invalid = initial; invalid.frames[0].image = std::make_shared<c::SnapshotImage>();
    CHECK(!document.setSnapshotTimeline(initial, invalid));
    CHECK(!document.setSnapshotTimeline(initial, initial));
    CHECK(document.revision() == revision && document.snapshotTimeline() == initial && !document.isModified());
    auto changed = initial; changed.frames[0].durationMs = 300;
    CHECK(document.setSnapshotTimeline(initial, changed));
    auto newer = changed; newer.frames[0].durationMs = 400;
    CHECK(!document.setSnapshotTimeline(initial, newer));
    CHECK(document.snapshotTimeline() == changed);
    c::ChangeSnapshotTimelineCommand stale(initial, newer, "Stale edit");
    CHECK(!stale.apply(document) && !stale.undo(document));

    // Slot count is not tied to the unrelated layer limit or a fixed frame cap.
    auto many = changed;
    for (int n = 0; n < 10000; ++n) many.frames.push_back({many.nextId++, 100, {}});
    CHECK(document.setSnapshotTimeline(changed, many));
    CHECK(document.snapshotTimeline().frames.size() == 10001);
}
}

int main()
{
    historyAndIndependence();
    atomicAdmission();
    std::cout << "Snapshot timeline core: " << failures << " failures\n";
    return failures ? 1 : 0;
}
