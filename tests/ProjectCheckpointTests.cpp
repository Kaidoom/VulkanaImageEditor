#include "imageeditor/core/DocumentCommands.hpp"
#include "imageeditor/core/EditorSession.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerTransform.hpp"
#include "imageeditor/core/RasterEditTransaction.hpp"
#include "imageeditor/core/RichText.hpp"
#include "imageeditor/core/SelectionCommands.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace imageeditor::core;

int failures = 0;
void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    ++failures;
  }
}
#define CHECK(expression)                                                      \
  check(static_cast<bool>(expression), #expression, __LINE__)

struct Fixture {
  Document document{CanvasSpec{.extent = {64, 64}}};
  std::shared_ptr<ContiguousRasterSurface> surface =
      std::make_shared<ContiguousRasterSurface>(Extent2u{32, 24});
  LayerId id{};
  History history;

  explicit Fixture(std::size_t historyBudget = 256ULL * 1024ULL * 1024ULL)
      : history(historyBudget) {
    auto layer = Layer::raster("Checkpoint target", surface);
    id = layer.id;
    CHECK(document.insertLayer(0, std::move(layer)));
    document.markSaved();
  }

  void opacity(float value, std::uint64_t merge = 0) {
    CHECK(history.execute(
        document, std::make_unique<SetLayerOpacityCommand>(id, value, merge)));
  }
};

TextLayer plainText(std::string value) {
  TextLayer result;
  result.utf8 = std::move(value);
  return normalizedText(std::move(result));
}

std::array<std::byte, 4> pixel(const RasterSurface &surface) {
  std::array<std::byte, 4> result{};
  surface.copyRgba8({2, 3, 1, 1}, result, 4);
  return result;
}

void savedUndoRedoAndExplicitUnsaved() {
  Fixture f;
  CHECK(!f.document.isModified());
  const auto initial = f.document.contentState();
  f.opacity(.75f);
  const auto saved = f.document.contentState();
  CHECK(saved != initial);
  CHECK(f.document.isModified());
  f.document.markSaved();
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  f.opacity(.5f);
  const auto later = f.document.contentState();
  CHECK(later != saved);
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == initial);
  CHECK(f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == later);
  CHECK(f.document.isModified());
  f.document.markSaved();
  f.history.clear();
  CHECK(f.document.contentState() == later);
  CHECK(!f.document.isModified());
  f.document.markUnsaved();
  CHECK(f.document.isModified());
  f.document.markSaved();
  CHECK(!f.document.isModified());
}

void opacityMergesStopAtSavedBoundary() {
  Fixture f;
  const auto initial = f.document.contentState();
  f.opacity(.875f, 71);
  f.opacity(.75f, 71);
  CHECK(f.history.undoDepth() == 1);
  const auto saved = f.document.contentState();
  f.document.markSaved();
  f.opacity(.625f, 71);
  CHECK(f.history.undoDepth() == 2);
  f.opacity(.5f, 71);
  CHECK(f.history.undoDepth() == 2);
  const auto latest = f.document.contentState();
  CHECK(f.history.undo(f.document));
  CHECK(f.document.layer(f.id)->opacity == .75f);
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == initial);
  CHECK(f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == latest);
  CHECK(f.document.isModified());
}

void divergentRedoCannotReuseSavedIdentity() {
  Fixture f;
  f.opacity(.75f);
  const auto beforeSaved = f.document.contentState();
  f.opacity(.5f);
  f.document.markSaved();
  const auto saved = f.document.contentState();
  const auto savedDepth = f.history.undoDepth();
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == beforeSaved);
  const auto retainedMemory = f.history.memoryUsed();
  CHECK(!f.history.execute(
      f.document, std::make_unique<SetLayerOpacityCommand>(f.id, .75f)));
  CHECK(f.document.contentState() == beforeSaved);
  CHECK(f.history.redoDepth() == 1);
  CHECK(f.history.memoryUsed() == retainedMemory);
  f.opacity(.25f);
  CHECK(f.history.undoDepth() == savedDepth);
  CHECK(f.history.redoDepth() == 0);
  const auto divergent = f.document.contentState();
  CHECK(divergent != saved);
  CHECK(divergent != beforeSaved);
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == beforeSaved);
  CHECK(f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == divergent);
  CHECK(f.document.isModified());
}

void evictedCheckpointRemainsReachableByToken() {
  Fixture f(2 * sizeof(SetLayerOpacityCommand));
  f.opacity(.75f);
  f.document.markSaved();
  const auto saved = f.document.contentState();
  f.opacity(.5f);
  f.opacity(.25f); // Evicts the command whose after-state was saved.
  CHECK(f.history.undoDepth() == 2);
  CHECK(f.history.memoryUsed() == 2 * sizeof(SetLayerOpacityCommand));
  CHECK(f.history.undo(f.document));
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(f.document.layer(f.id)->opacity == .75f);
  CHECK(!f.document.isModified());
  CHECK(!f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.history.redo(f.document));
  CHECK(f.document.isModified());
}

void selectionAndSessionChangesAreNotContent() {
  Fixture f;
  const auto saved = f.document.contentState();
  const auto selected = SelectionMask::rectangle({64, 64}, {2, 3, 9, 7});
  CHECK(f.history.execute(f.document,
                          std::make_unique<SetSelectionCommand>(selected)));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(!f.document.isModified());
  f.opacity(.5f);
  const auto content = f.document.contentState();
  CHECK(f.history.execute(
      f.document, std::make_unique<SetSelectionCommand>(SelectionState{})));
  CHECK(f.document.contentState() == content);
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == content);
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());

  EditorSession session;
  auto document = std::make_unique<Document>(CanvasSpec{.extent = {8, 8}});
  auto first = Layer::raster(
      "A", std::make_shared<ContiguousRasterSurface>(Extent2u{8, 8}));
  auto second = Layer::raster(
      "B", std::make_shared<ContiguousRasterSurface>(Extent2u{8, 8}));
  const auto firstId = first.id;
  const auto secondId = second.id;
  CHECK(document->insertLayer(0, std::move(first)));
  CHECK(document->insertLayer(1, std::move(second)));
  session.replaceDocument(std::move(document));
  session.document()->markSaved();
  const auto sessionSaved = session.document()->contentState();
  session.setActiveLayer(firstId);
  session.setActiveLayer(secondId);
  session.setActiveTool(ToolId::Brush);
  session.setForegroundColor({40, 80, 120, 160});
  auto colors = session.colors();
  colors.active = ColorSlot::Secondary;
  session.setColors(colors);
  session.setColorSampleSource(ColorSampleSource::ActiveLayer);
  CHECK(session.document()->contentState() == sessionSaved);
  CHECK(!session.document()->isModified());
  CHECK(session.history().undoDepth() == 0);
}

void canvasChangesAreContentEvenWhenSelectionChangesToo() {
  Fixture f;
  CHECK(
      f.history.execute(f.document, std::make_unique<SetSelectionCommand>(
                                        SelectionMask::filled({64, 64}, 255))));
  f.document.markSaved();
  const auto saved = f.document.contentState();
  const auto canvas = f.document.canvas();
  auto changed = canvas;
  changed.extent = {48, 32};
  changed.dotsPerInch = 300;
  CHECK(f.history.execute(f.document,
                          std::make_unique<ChangeCanvasSpecCommand>(changed)));
  CHECK(f.document.contentState() != saved);
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(f.document.canvas() == canvas);
  CHECK((f.document.selection()->extent() == Extent2u{64, 64}));
  CHECK(!f.document.isModified());
}

void directRasterCommitCancelAndNoOp() {
  Fixture f;
  const auto saved = f.document.contentState();
  const auto transparent = pixel(*f.surface);
  const std::array red{std::byte{220}, std::byte{30}, std::byte{40},
                       std::byte{255}};
  const std::array blue{std::byte{10}, std::byte{30}, std::byte{200},
                        std::byte{127}};
  {
    RasterEditTransaction edit(f.document, f.id, "Paint");
    CHECK(!edit.writeRgba8({2, 3, 1, 1}, red, 4).regions.empty());
    CHECK(edit.commit(f.history) == RasterEditCommitResult::Committed);
  }
  const auto painted = f.document.contentState();
  CHECK(painted != saved);
  CHECK(f.document.isModified());
  CHECK(pixel(*f.surface) == red);
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(pixel(*f.surface) == transparent);
  const auto memory = f.history.memoryUsed();
  {
    RasterEditTransaction edit(f.document, f.id, "Cancelled");
    (void)edit.writeRgba8({2, 3, 1, 1}, blue, 4);
    edit.cancel();
  }
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(pixel(*f.surface) == transparent);
  {
    RasterEditTransaction edit(f.document, f.id, "No effect");
    (void)edit.writeRgba8({2, 3, 1, 1}, blue, 4);
    (void)edit.writeRgba8({2, 3, 1, 1}, transparent, 4);
    CHECK(edit.commit(f.history) == RasterEditCommitResult::NoChanges);
  }
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redoDepth() == 1);
  CHECK(f.history.memoryUsed() == memory);
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == painted);
  CHECK(pixel(*f.surface) == red);
}

void pendingTransformCancelRestoresCheckpointAndRedo() {
  Fixture f;
  f.opacity(.75f);
  f.document.markSaved();
  const auto saved = f.document.contentState();
  f.opacity(.5f);
  CHECK(f.history.undo(f.document));
  const auto memory = f.history.memoryUsed();
  const auto original = f.document.layer(f.id)->localToDocument;
  LayerTransformSession edit(f.document, f.id);
  CHECK(edit.active());
  auto values = edit.values();
  values.center.x += 7;
  CHECK(edit.setValues(values));
  CHECK(edit.completeAction());
  const auto first = f.document.layer(f.id)->localToDocument;
  CHECK(first != original);
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  values.rotationDegrees = 25;
  CHECK(edit.setValues(values));
  CHECK(edit.completeAction());
  const auto second = f.document.layer(f.id)->localToDocument;
  CHECK(second != first);
  CHECK(f.document.contentState() == saved);
  CHECK(edit.undo());
  CHECK(f.document.layer(f.id)->localToDocument == first);
  CHECK(edit.undo());
  CHECK(f.document.layer(f.id)->localToDocument == original);
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(edit.redo());
  CHECK(edit.redo());
  CHECK(f.document.layer(f.id)->localToDocument == second);
  edit.cancel();
  CHECK(f.document.layer(f.id)->localToDocument == original);
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.undoDepth() == 1);
  CHECK(f.history.redoDepth() == 1);
  CHECK(f.history.memoryUsed() == memory);
  CHECK(f.history.redo(f.document));
  CHECK(f.document.layer(f.id)->opacity == .5f);
  CHECK(f.document.isModified());
}

void transformPublicationAssignsActionTokens() {
  Fixture f;
  const auto saved = f.document.contentState();
  LayerTransformSession edit(f.document, f.id);
  auto values = edit.values();
  values.center.x += 7;
  CHECK(edit.setValues(values));
  CHECK(edit.completeAction());
  const auto firstFrame = f.document.layer(f.id)->localToDocument;
  values.center.y += 4;
  CHECK(edit.setValues(values));
  CHECK(edit.completeAction());
  const auto secondFrame = f.document.layer(f.id)->localToDocument;
  CHECK(edit.undo());
  CHECK(f.document.layer(f.id)->localToDocument == firstFrame);
  CHECK(f.document.contentState() == saved);
  CHECK(edit.commit(f.history) == TransformCommitResult::Committed);
  const auto first = f.document.contentState();
  CHECK(first != saved);
  CHECK(f.history.undoDepth() == 1);
  CHECK(f.history.redoDepth() == 1);
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == first);
  CHECK(f.document.layer(f.id)->localToDocument == firstFrame);
  CHECK(f.history.redo(f.document));
  const auto second = f.document.contentState();
  CHECK(second != first && second != saved);
  CHECK(f.document.layer(f.id)->localToDocument == secondFrame);
  CHECK(f.document.isModified());
  CHECK(f.history.undo(f.document));
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == second);
}

void transformPublicationFollowsInterleavedVisibilityEdit() {
  Fixture f;
  f.opacity(.75f);
  f.document.markSaved();
  const auto saved = f.document.contentState();
  const auto original = f.document.layer(f.id)->localToDocument;

  LayerTransformSession edit(f.document, f.id);
  auto values = edit.values();
  values.center.x += 7;
  values.rotationDegrees = 19;
  CHECK(edit.setValues(values));
  CHECK(edit.completeAction());
  const auto transformed = f.document.layer(f.id)->localToDocument;
  CHECK(f.document.contentState() == saved);
  CHECK(f.history.execute(
      f.document, std::make_unique<SetLayerVisibilityCommand>(f.id, false)));
  const auto hidden = f.document.contentState();
  CHECK(hidden != saved);
  CHECK(edit.commit(f.history) == TransformCommitResult::Committed);
  const auto applied = f.document.contentState();
  CHECK(applied != hidden && applied != saved);
  CHECK(!edit.active());
  CHECK(f.document.layer(f.id)->localToDocument == transformed);
  CHECK(!f.document.layer(f.id)->visible);

  CHECK(f.history.undo(f.document));
  CHECK(f.document.layer(f.id)->localToDocument == original);
  CHECK(!f.document.layer(f.id)->visible);
  CHECK(f.document.contentState() == hidden);
  CHECK(f.history.undo(f.document));
  CHECK(f.document.layer(f.id)->visible);
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == hidden);
  CHECK(f.history.redo(f.document));
  CHECK(f.document.contentState() == applied);
  CHECK(f.document.layer(f.id)->localToDocument == transformed);
  CHECK(!f.document.layer(f.id)->visible);
}

void textTypingMergeAndUndoneInitialBranch() {
  Fixture f;
  auto layer = Layer::text("Existing", plainText("a"));
  const auto id = layer.id;
  CHECK(f.document.insertLayer(1, std::move(layer)));
  f.document.markSaved();
  CHECK(f.history.execute(f.document, std::make_unique<TextEditCommand>(
                                          id, plainText("a"), plainText("ab"),
                                          TextEditHint{id, 1, 1},
                                          TextEditHint{id, 2, 2}, 44)));
  f.document.markSaved();
  const auto saved = f.document.contentState();
  CHECK(f.history.execute(f.document, std::make_unique<TextEditCommand>(
                                          id, plainText("ab"), plainText("abc"),
                                          TextEditHint{id, 2, 2},
                                          TextEditHint{id, 3, 3}, 44)));
  CHECK(f.history.undoDepth() == 2);
  CHECK(f.history.undo(f.document));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  const auto memory = f.history.memoryUsed();
  History branch;
  auto draft = Layer::text("Draft", plainText("draft"));
  const auto draftId = draft.id;
  CHECK(branch.execute(f.document,
                       std::make_unique<TextEditCommand>(
                           std::move(draft), f.document.layers().size(), id,
                           TextEditHint{draftId, 5, 5}, 91)));
  CHECK(f.document.isModified());
  CHECK(branch.execute(f.document,
                       std::make_unique<TextEditCommand>(
                           draftId, plainText("draft"), plainText(""),
                           TextEditHint{draftId, 0, 5},
                           TextEditHint{draftId, 0, 0}, 0, "Delete text")));
  CHECK(branch.undo(f.document));
  CHECK(branch.undo(f.document));
  CHECK(!f.document.containsLayer(draftId));
  CHECK(f.document.contentState() == saved);
  CHECK(!f.document.isModified());
  CHECK(!f.history.publishAppliedBranch(f.document, branch));
  CHECK(f.history.redoDepth() == 1);
  CHECK(f.history.memoryUsed() == memory);
  CHECK(f.document.contentState() == saved);
  CHECK(f.history.redo(f.document));
  CHECK(f.document.isModified());
}
} // namespace

int main() {
  try {
    savedUndoRedoAndExplicitUnsaved();
    opacityMergesStopAtSavedBoundary();
    divergentRedoCannotReuseSavedIdentity();
    evictedCheckpointRemainsReachableByToken();
    selectionAndSessionChangesAreNotContent();
    canvasChangesAreContentEvenWhenSelectionChangesToo();
    directRasterCommitCancelAndNoOp();
    pendingTransformCancelRestoresCheckpointAndRedo();
    transformPublicationAssignsActionTokens();
    transformPublicationFollowsInterleavedVisibilityEdit();
    textTypingMergeAndUndoneInitialBranch();
  } catch (const std::exception &error) {
    std::cerr << "Unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  std::cout << (failures ? "Project checkpoint checks failed: "
                         : "Project checkpoint checks passed: ")
            << failures << '\n';
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
