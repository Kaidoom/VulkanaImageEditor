#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PdfImportDialog.hpp"
#include "imageeditor/ui/FileDialogLocations.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QFileInfo>
#include <QFile>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QDebug>

namespace imageeditor::ui {
bool MainWindow::importPdfFromPath(const QString& filePath, bool intoCurrent)
{
    if (fileBusy_ || workspaceDialog_ || !settleForFileOperation()) return false;
    const auto path = QFileInfo(filePath).absoluteFilePath();
    // Pin both runtime owner and insertion context before any nested event loop.
    const auto target = activeDocument_;
    const auto* targetDocument = target ? target->session.document() : nullptr;
    const auto targetRevision = targetDocument ? targetDocument->revision() : 0;
    const auto targetGeneration = target ? target->cancellationGeneration : 0;
    auto placement = core::ItemPlacement{0, targetDocument ? targetDocument->tree().roots.size() : 0};
    const auto selection = target ? target->session.layerSelectionState() : core::LayerSelectionState{};
    if (targetDocument && selection.primary)
        placement=targetDocument->tree().insertionAbove(*selection.primary);
    PdfLimits limits;
    const auto memory = documentMemory();
    limits.existingBytes = memory.sourceBytes + memory.derivedBytes + memory.historyBytes;
#ifdef Q_OS_LINUX
    // A conservative advisory preflight, not a sandbox or a reservation. Keep
    // headroom for the desktop; the fixed budget still applies on other hosts.
    QFile meminfo(QStringLiteral("/proc/meminfo"));
    if (meminfo.open(QIODevice::ReadOnly)) {
        for (const auto& line : meminfo.readAll().split('\n')) {
            if (!line.startsWith("MemAvailable:")) continue;
            bool ok = false;
            const auto kib = line.simplified().split(' ').value(1).toULongLong(&ok);
            if (ok && kib < (1ULL << 40))
                limits.workingBytes = std::min<std::uint64_t>(limits.workingBytes, limits.existingBytes + kib * 768);
            break;
        }
    }
#endif
    const auto renderer = canvasWindow_->rendererStats();
    if (renderer.maximumImageDimension2D) limits.maximumDimension = std::min(limits.maximumDimension, renderer.maximumImageDimension2D);
    if (targetDocument) {
        limits.currentDocumentLayers = core::LayerTree::maxItems - targetDocument->layers().size() - targetDocument->tree().containers.size();
        // Leave room for structural history metadata as well as retained pages.
        limits.currentDocumentRasterBytes = std::min<std::uint64_t>(limits.rasterBytes,
            target->session.history().memoryBudget() > 1024*1024 ? target->session.history().memoryBudget()-1024*1024 : 0);
    }
    WorkspaceDialog presenter(*workspace_, *this);
    PdfImportDialog dialog(path, targetDocument != nullptr, intoCurrent, limits, &presenter);
    {
        const QScopedValueRollback active(workspaceDialog_, &presenter);
        const QScopedValueRollback busy(fileBusy_, true);
        if (presenter.exec(dialog) != QDialog::Accepted) return false;
    }
    const auto options = dialog.options();
    const auto plan = dialog.plan();
    auto rendered = dialog.takePages();
    if (!plan || rendered.cancelled || !rendered.error.isEmpty() || rendered.pages.size() != options.pages.size()) return false;
    try {
        if (options.destination == PdfDestination::CurrentDocument) {
            if (!target || target->closed || target != activeDocument_ || target->session.document() != targetDocument
                || target->cancellationGeneration != targetGeneration || targetDocument->revision() != targetRevision
                || target->session.layerSelectionState() != selection) {
                reportFileError(tr("The PDF destination changed while rendering. Nothing was imported; please try again.")); return false;
            }
            auto tree = targetDocument->tree();
            auto* siblings = tree.children(placement.parent);
            if (!siblings || placement.index > siblings->size()) return false;
            std::vector<core::Layer> layers;
            std::vector<core::LayerId> ids;
            layers.reserve(rendered.pages.size()); ids.reserve(rendered.pages.size());
            for (auto i = rendered.pages.rbegin(); i != rendered.pages.rend(); ++i) {
                ids.push_back(i->layer.id); layers.push_back(std::move(i->layer));
            }
            siblings->insert(siblings->begin()+std::ptrdiff_t(placement.index), ids.begin(), ids.end());
            auto command = std::make_unique<core::LayerStructureCommand>("Import PDF pages", *targetDocument, std::move(tree),
                std::vector<core::LayerId>{}, std::move(layers), selection, core::LayerSelectionState{ids,ids.back(),ids.back()});
            if (command->memoryCost() > target->session.history().memoryBudget()) {
                reportFileError(tr("PDF pages exceed the history budget. Choose fewer pages or lower the PPI.")); return false;
            }
            if (!executeDocumentCommand(std::move(command))) return false;
            target->untouched = false; synchronizeUi(true,false);
        } else {
            std::vector<std::shared_ptr<DocumentContext>> staged;
            const bool separate = options.destination == PdfDestination::SeparateDocuments;
            const auto makeContext = [&](QSize size, QString name) {
                auto context = std::make_shared<DocumentContext>();
                context->session.replaceDocument(std::make_unique<core::Document>(core::CanvasSpec{
                    {std::uint32_t(size.width()),std::uint32_t(size.height())},options.ppi}));
                context->sourcePath = path; context->displayName = std::move(name);
                return context;
            };
            if (!separate) staged.push_back(makeContext(plan.canvas,QFileInfo(path).fileName()));
            for (auto& page : rendered.pages) {
                auto context = separate ? makeContext(page.size,QString::fromStdString(page.layer.name)) : staged.front();
                const auto id = page.layer.id;
                // First selected page remains topmost. Later pages insert below it.
                if (!context->session.document()->insertLayer(0,std::move(page.layer))) throw std::runtime_error("Invalid PDF layer");
                if (separate || !context->session.activeLayer()) context->session.setActiveLayer(id);
                context->session.document()->markUnsaved();
                if (separate) staged.push_back(std::move(context));
            }
            if (!publishDocuments(std::move(staged))) return false;
        }
        if (persistWindowState_) {
            recentFiles_.recordSuccess(path,RecentFileKind::Pdf);
            rememberOpenedDocument(path); savePdfImportPreferences(options);
        }
        qint64 total = 0;
        for (const auto& page : rendered.pages) {
            total += page.milliseconds;
            qInfo() << "PDF import page" << page.page+1 << page.size << "render/normalize ms" << page.milliseconds;
        }
        qInfo() << "PDF import pixels MiB" << double(plan.rasterBytes)/1048576
            << "estimated working MiB" << double(plan.estimatedWorkingBytes)/1048576 << "render/normalize ms" << total;
        updateDocumentTitle();
        statusBar()->showMessage(tr("Imported %1 PDF page(s) at %2 PPI").arg(options.pages.size()).arg(options.ppi),4000);
        return true;
    } catch (const std::exception&) {
        reportFileError(tr("Unable to prepare PDF layers. Check available memory and try fewer pages or a lower PPI."));
        return false;
    }
}
} // namespace imageeditor::ui
