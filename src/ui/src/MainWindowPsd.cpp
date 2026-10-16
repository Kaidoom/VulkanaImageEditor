#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PsdImportDialog.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QFile>
#include <QFileInfo>
#include <QScopedValueRollback>
#include <QStatusBar>

namespace imageeditor::ui {
bool MainWindow::importPsdFromPath(const QString &input, bool intoCurrent) {
  if (fileBusy_ || workspaceDialog_)
    return false;
  const auto path = QFileInfo(input).absoluteFilePath();
  // A missing recent entry is an ordinary open error, not an import review.
  // Check before settling edits or constructing the asynchronous review UI;
  // the reader still checks again in case the file changes afterwards.
  {
    QFile source(path);
    const bool opened = source.open(QIODevice::ReadOnly);
    if (!opened || !QFileInfo(path).isFile()) {
      reportFileError(i18n::text("main.window.files.could.not.open.1.the.current.document.was.kept")
          .arg(path, opened ? QStringLiteral("Not a regular file") : source.errorString()));
      return false;
    }
  }
  if (!settleForFileOperation()) return false;
  const auto target = activeDocument_;
  const auto *doc = target ? target->session.document() : nullptr;
  const auto revision = doc ? doc->revision() : 0,
             generation = target ? target->cancellationGeneration : 0;
  const auto selection = target ? target->session.layerSelectionState()
                                : core::LayerSelectionState{};
  auto placement = core::ItemPlacement{0, doc ? doc->tree().roots.size() : 0};
  if (doc && selection.primary)
    placement=doc->tree().insertionAbove(*selection.primary);
  PsdLimits limits;
  const auto memory = documentMemory();
  limits.existingBytes =
      memory.sourceBytes + memory.derivedBytes + memory.historyBytes;
  auto renderer = canvasWindow_->rendererStats();
  if (renderer.maximumImageDimension2D)
    limits.dimension =
        std::min(limits.dimension, renderer.maximumImageDimension2D);
  WorkspaceDialog host(*workspace_, *this);
  PsdImportDialog dialog(path, doc != nullptr, intoCurrent, limits, &host);
  {
    const QScopedValueRollback active(workspaceDialog_, &host);
    const QScopedValueRollback busy(fileBusy_, true);
    if (host.exec(dialog) != QDialog::Accepted)
      return false;
  }
  auto result = dialog.takeResult();
  if (!result.document || result.cancelled || !result.error.isEmpty())
    return false;
  try {
    if (dialog.intoCurrent()) {
      if (!target || target->closed || target != activeDocument_ ||
          target->session.document() != doc ||
          target->cancellationGeneration != generation ||
          doc->revision() != revision ||
          target->session.layerSelectionState() != selection) {
        reportFileError(i18n::text("main.window.psd.the.psd.destination.changed.nothing.was.imported.please.try"));
        return false;
      }
      auto tree = doc->tree();
      if (!guardRefinement(target->id)) return false;
      tree.containers.insert(tree.containers.end(),
                             result.document->tree().containers.begin(),
                             result.document->tree().containers.end());
      auto *siblings = tree.children(placement.parent);
      if (!siblings || placement.index > siblings->size())
        return false;
      const auto ids = result.document->tree().roots;
      siblings->insert(siblings->begin() + std::ptrdiff_t(placement.index),
                       ids.begin(), ids.end());
      auto command = std::make_unique<core::LayerStructureCommand>(
          "Import PSD layers", *doc, std::move(tree),
          std::vector<core::LayerId>{}, result.document->layers(), selection,
          core::LayerSelectionState{ids, ids.back(), ids.back()});
      // Structural history shares the staged surfaces. History retains one
      // oversized newest action and trims older entries by its normal policy;
      // its retention target is not a maximum import/document size.
      if (!executeDocumentCommand(std::move(command)))
        return false;
      target->untouched = false;
      synchronizeUi(true, false);
    } else {
      auto context = std::make_shared<DocumentContext>();
      const auto primary = result.document->tree().roots.back();
      context->session.replaceDocument(std::move(result.document));
      context->session.setActiveLayer(primary);
      context->sourcePath = path;
      context->displayName = QFileInfo(path).fileName();
      if (!publishDocuments({std::move(context)}))
        return false;
    }
    if (persistWindowState_) {
      recentFiles_.recordSuccess(path, RecentFileKind::Psd);
      rememberOpenedDocument(path);
    }
    updateDocumentTitle();
    statusBar()->showMessage(
        i18n::text("main.window.psd.imported.psd.as.independent.editable.vulkana.content"), 4000);
    return true;
  } catch (const std::exception &) {
    reportFileError(
        i18n::text("main.window.psd.unable.to.insert.psd.content.safely.no.partial.import"));
    return false;
  }
}
} // namespace imageeditor::ui
