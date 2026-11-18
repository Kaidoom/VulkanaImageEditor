#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include <QScopeGuard>
#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/ProjectLayer.hpp"
#include "imageeditor/core/ProjectPaint.hpp"
#include "imageeditor/ui/PixelPreview.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"

#include <QDateTime>
#include <QDialog>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <future>
#include <stdexcept>

namespace imageeditor::ui {
namespace {
using Surface = std::shared_ptr<core::RasterSurface>;
struct Appearance {
    std::shared_ptr<const core::ProjectLayerSource> source;
    Surface pixels;
};

QString projectPathKey(const QString& path)
{
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

std::vector<std::string> dependencies(const core::Document& document)
{
    std::set<std::string> paths;
    for (const auto& layer : document.layers()) {
        if (!layer.projectSource) continue;
        paths.insert(layer.projectSource->path);
        paths.insert(layer.projectSource->dependencies.begin(), layer.projectSource->dependencies.end());
    }
    return {paths.begin(), paths.end()};
}

bool samePixels(const Surface& a, const Surface& b)
{
    if (a == b) return true;
    const auto extent = a->extent();
    if (extent != b->extent()) return false;
    const auto stride = size_t(extent.width) * 4;
    std::vector<std::byte> left(stride), right(stride);
    for (unsigned y = 0; y < extent.height; ++y) {
        core::RectI row{0, int(y), int(extent.width), 1};
        a->copyRgba8(row, left, stride);
        b->copyRgba8(row, right, stride);
        if (left != right) return false;
    }
    return true;
}

void replaceLayers(DocumentContext& owner, std::vector<core::Layer> replacements, const char* label)
{
    auto& document = *owner.session.document();
    std::vector<core::LayerId> ids;
    for (const auto& layer : replacements) ids.push_back(layer.id);
    auto command = std::make_unique<core::LayerStructureCommand>(
        label, document, document.tree(), std::move(ids), std::move(replacements));
    if (!owner.session.execute(std::move(command)))
        throw std::runtime_error("Could not update the project layer.");
    owner.untouched = false;
}
}

// Inspect the known dependency graph without opening files referenced by a
// project. Closed nodes carry the transitive dependencies of their saved render.
bool MainWindow::canLinkProject(const QString& parentPath, const QString& sourcePath,
    const std::vector<std::string>& capturedDependencies) const
{
    const auto parent = projectPathKey(parentPath);
    if (parent.isEmpty()) return true; // Save As checks again against the chosen filename.
    std::set<QString> visited;
    std::vector<QString> pending{projectPathKey(sourcePath)};
    for (const auto& path : capturedDependencies) pending.push_back(projectPathKey(QString::fromStdString(path)));
    while (!pending.empty()) {
        const auto path = pending.back();
        pending.pop_back();
        if (path == parent) return false;
        if (!visited.insert(path).second) continue;
        for (const auto& owner : documents_) {
            if (owner->closed || projectPathKey(owner->projectPath) != path) continue;
            for (const auto& child : dependencies(*owner->session.document()))
                pending.push_back(projectPathKey(QString::fromStdString(child)));
        }
    }
    return true;
}

class ProjectLayerResolver {
public:
    explicit ProjectLayerResolver(MainWindow& window) : window_(window) {}

    DocumentContext* openSource(const QString& path) const
    {
        const auto key = projectPathKey(path);
        for (const auto& owner : window_.documents_)
            if (!owner->closed && !owner->discardingProjectEdits && projectPathKey(owner->projectPath) == key)
                return owner.get();
        return nullptr;
    }

    void refreshOpen(DocumentContext& parent)
    {
        if (visited_.contains(parent.id)) return;
        if (visiting_.size() >= core::LayerTree::maxDepth)
            throw std::runtime_error("Project link nesting is too deep.");
        if (!visiting_.insert(parent.id).second)
            throw std::runtime_error("Circular project link. The saved appearance was kept.");
        const auto finish = qScopeGuard([&] { visiting_.erase(parent.id); });
        std::set<QString> direct;
        for (const auto& layer : parent.session.document()->layers())
            if (layer.projectSource) direct.insert(projectPathKey(QString::fromStdString(layer.projectSource->path)));
        for (const auto& path : direct) {
            auto* open = openSource(path);
            const bool reload = parent.projectSourcesToReload.contains(path);
            if (!open && !reload) continue;
            if (open) {
                refreshOpen(*open); // Settle nested open sources before comparing their parent's state.
                const auto stamp = std::pair{open->id, open->session.document()->contentState()};
                const auto observed = parent.observedProjectSources.find(path);
                if (!reload && observed != parent.observedProjectSources.end() && observed->second == stamp) continue;
            }
            if (!open && !QFileInfo::exists(path)) {
                parent.projectSourcesToReload.erase(path);
                continue; // A removed source still has its persisted appearance.
            }
            auto appearance = read(parent, path, open, true);
            update(parent, appearance, {});
            parent.projectSourcesToReload.erase(path);
        }
        visited_.insert(parent.id);
    }

    Appearance resolve(DocumentContext& parent, const QString& file, bool force)
    {
        const auto path = projectPathKey(file);
        if (path.isEmpty() || QFileInfo(path).suffix().compare("vulkana", Qt::CaseInsensitive))
            throw std::runtime_error("Choose a saved Vulkana project.");
        if (!window_.canLinkProject(parent.projectPath, path, {}))
            throw std::runtime_error("This link would create a circular project reference.");
        auto* open = openSource(path);
        if (open) refreshOpen(*open);
        return read(parent, path, open, force);
    }

    void update(DocumentContext& parent, Appearance appearance, std::optional<core::LayerId> only)
    {
        std::vector<core::Layer> changes;
        std::map<core::SurfaceId, bool> equal;
        for (const auto& layer : parent.session.document()->layers()) {
            if (!layer.projectSource || (only ? layer.id != *only : projectPathKey(QString::fromStdString(layer.projectSource->path)) != QString::fromStdString(appearance.source->path)))
                continue;
            const auto& old = std::get<core::RasterLayer>(layer.payload).surface;
            const auto base = core::projectBasePixels(old);
            auto [match, inserted] = equal.try_emplace(base->id(), false);
            if (inserted) match->second = samePixels(base, appearance.pixels);
            if (match->second && layer.projectSource->path == appearance.source->path
                && layer.projectSource->dependencies == appearance.source->dependencies) continue;
            auto next = layer;
            next.projectSource = appearance.source;
            next.payload = core::RasterLayer{core::withProjectPaint(appearance.pixels, core::projectPaintState(old))};
            next.filterCache.reset();
            next.effectCache.reset();
            changes.push_back(std::move(next));
        }
        if (!changes.empty())
            replaceLayers(parent, std::move(changes), only ? "Relink project layer" : "Refresh project layers");
        observe(parent, *appearance.source);
    }

    void observe(DocumentContext& parent, const core::ProjectLayerSource& source)
    {
        parent.observedProjectSources[QString::fromStdString(source.path)] = {source.instanceId, source.contentState};
    }

private:
    Appearance read(DocumentContext& parent, const QString& path, DocumentContext* open, bool force)
    {
        auto source = std::make_shared<core::ProjectLayerSource>();
        source->path = path.toStdString();
        QFileInfo info(path);
        auto validate = [&](const core::ProjectLayerSource& link) {
            if (!window_.canLinkProject(parent.projectPath, path, link.dependencies))
                throw std::runtime_error("This link would create a circular project reference.");
        };
        if (open) {
            const auto& document = *open->session.document();
            source->dependencies = dependencies(document);
            validate(*source);
            source->instanceId = open->id;
            source->contentState = document.contentState();
            if (open->lastProjectAppearanceState == source->contentState)
                if (auto pixels = open->lastProjectAppearance.lock()) return {source, pixels};
        } else {
            if (!info.isFile()) throw std::runtime_error(QString("Source project missing: %1").arg(info.fileName()).toStdString());
            std::erase_if(window_.diskProjects_, [](const auto& item) {
                return item.second.source.expired() || item.second.pixels.expired();
            });
            const auto cached = window_.diskProjects_.find(path);
            if (!force && cached != window_.diskProjects_.end() && cached->second.size == info.size()
                && cached->second.modified == info.lastModified().toMSecsSinceEpoch()) {
                const auto link = cached->second.source.lock();
                auto pixels = cached->second.pixels.lock();
                if (link && pixels) { validate(*link); return {link, pixels}; }
            }
        }
        auto pixels = render(open ? open->session.document() : nullptr, path, source->dependencies);
        if (!open) validate(*source);
        // A disk reload or a reopened source may have a different runtime
        // identity but the same pixels as an existing placement.
        for (const auto& layer : parent.session.document()->layers()) {
            if (!layer.projectSource || layer.projectSource->path != source->path) continue;
            const auto& old = std::get<core::RasterLayer>(layer.payload).surface;
            const auto base = core::projectBasePixels(old);
            if (samePixels(base, pixels)) { pixels = base; break; }
        }
        if (open) {
            open->lastProjectAppearance = pixels;
            open->lastProjectAppearanceState = source->contentState;
        } else {
            const QFileInfo current(path);
            if (current.size() != info.size() || current.lastModified() != info.lastModified())
                throw std::runtime_error("The source changed on disk during rendering. Refresh the project layer again.");
            window_.diskProjects_[path] = {info.size(), info.lastModified().toMSecsSinceEpoch(), source, pixels};
        }
        return {std::move(source), std::move(pixels)};
    }

    Surface render(const core::Document* live, const QString& path, std::vector<std::string>& sourceDependencies)
    {
        // Use the same frozen-input worker as native Pixel Preview. Only
        // finished results can enter history. Keep the previous surface live
        // while rendering, including on cancellation or allocation failure.
        WorkspaceDialog presenter(*window_.workspace_, window_);
        QDialog card(&presenter, Qt::SubWindow | Qt::FramelessWindowHint);
        auto* layout = new QVBoxLayout(&card);
        layout->addWidget(new QLabel(i18n::text("project.layer.rendering"), &card));
        auto* bar = new QProgressBar(&card);
        bar->setRange(0, 0);
        bar->setMinimumWidth(240);
        layout->addWidget(bar);
        auto* cancel = i18n::make<QPushButton>(i18n::text("main.window.layer.organization.cancel"), &card);
        layout->addWidget(cancel);
        QScopedValueRollback busy(window_.fileBusy_, true);
        QScopedValueRollback modal(window_.workspaceDialog_, &presenter);
        QScopedValueRollback<QWidget*> progress(window_.fileProgress_, &card);
        PixelPreview worker(nullptr, PixelPreview::Purpose::ProjectLayer);
        QEventLoop loop;
        std::shared_ptr<const core::RasterSurface> pixels;
        QString error;
        worker.onReady = [&](auto result, QString diagnostic) {
            pixels = std::move(result);
            error = std::move(diagnostic);
            if (pixels || !error.isEmpty()) loop.quit();
        };
        QObject::connect(cancel, &QPushButton::clicked, &card, &QDialog::reject);
        QObject::connect(&card, &QDialog::rejected, &loop, &QEventLoop::quit);
        QTimer::singleShot(250, &card, [&] { presenter.open(card); });
        // A closed source can be loaded and rendered entirely on the worker.
        // An open source uses PixelPreview to freeze the mutable document.
        std::atomic_bool cancelled{false};
        std::future<std::pair<Surface, std::vector<std::string>>> disk;
        QTimer poll;
        if (live) {
            worker.setEnabled(true);
            worker.request(live->snapshot());
        } else {
            disk = std::async(std::launch::async, [path, &cancelled] {
                const auto keepGoing = [&](auto, auto) { return !cancelled.load(); };
                auto loaded = loadProject(path, keepGoing);
                if (!loaded) throw std::runtime_error(loaded.error.toStdString());
                auto image = flattenDocument(*loaded.document, keepGoing);
                if (!image) throw std::runtime_error(image.error.toStdString());
                return std::pair{surfaceFromNativeImage(image.image), dependencies(*loaded.document)};
            });
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (disk.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
                poll.stop();
                try {
                    auto result = disk.get();
                    pixels = std::move(result.first);
                    sourceDependencies = std::move(result.second);
                } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); }
                loop.quit();
            });
            poll.start(15);
        }
        if (!pixels && error.isEmpty()) loop.exec();
        cancelled = true;
        worker.onReady = {};
        worker.setEnabled(false);
        presenter.finish();
        if (!pixels) throw std::runtime_error(error.isEmpty() ? "Project render cancelled." : error.toStdString());
        return core::readOnlyProjectSurface(std::move(pixels));
    }

    MainWindow& window_;
    std::set<DocumentInstanceId> visiting_, visited_;
};

QString MainWindow::chooseProjectLayerPath()
{
    return QFileDialog::getOpenFileName(this, i18n::text("project.layer.choose"), fileState().projectPath,
        i18n::text("project.layer.filter"));
}

void MainWindow::projectSourceLeaving(const DocumentContext& source, bool revertToDisk)
{
    if (source.projectPath.isEmpty()) return;
    const auto path = projectPathKey(source.projectPath);
    const auto finalState = std::pair{source.id, source.session.document()->contentState()};
    for (const auto& parent : documents_) {
        if (parent.get() == &source) continue;
        const auto seen = parent->observedProjectSources.find(path);
        if (!revertToDisk && seen != parent->observedProjectSources.end() && seen->second == finalState) continue;
        for (const auto& layer : parent->session.document()->layers())
            if (layer.projectSource && projectPathKey(QString::fromStdString(layer.projectSource->path)) == path) {
                parent->projectSourcesToReload.insert(path);
                break;
            }
    }
}

bool MainWindow::refreshOpenProjectLayers(DocumentContext& parent)
try {
    ProjectLayerResolver(*this).refreshOpen(parent);
    return true;
} catch (const std::exception& error) {
    statusBar()->showMessage(QString::fromUtf8(error.what()), 6500);
    return false;
}

bool MainWindow::updateProjectLayer(const QString& path, std::optional<core::LayerId> target, bool allPlacements)
try {
    if (!activeDocument_ || fileBusy_ || workspaceDialog_ || !settleForFileOperation()) return false;
    auto& doc = *session().document();
    if (target && (!doc.layer(*target) || !doc.layer(*target)->projectSource)) return false;
    ProjectLayerResolver resolver(*this);
    auto appearance = resolver.resolve(*activeDocument_, path, allPlacements);
    if (target) {
        resolver.update(*activeDocument_, appearance, allPlacements ? std::nullopt : target);
    } else {
        auto layer = core::Layer::raster(QFileInfo(path).completeBaseName().toStdString(), appearance.pixels);
        layer.projectSource = appearance.source;
        const auto extent = appearance.pixels->extent();
        layer.localToDocument.m02 = std::floor((double(doc.canvas().extent.width) - extent.width) * .5);
        layer.localToDocument.m12 = std::floor((double(doc.canvas().extent.height) - extent.height) * .5);
        auto tree = doc.tree();
        auto slot = core::ItemPlacement{0, tree.roots.size()};
        if (const auto active = session().activeLayer()) {
            const auto group = core::channelGroupForLayer(doc, *active);
            slot = tree.insertionAbove(group ? group->id : *active);
        }
        tree.children(slot.parent)->insert(tree.children(slot.parent)->begin() + std::ptrdiff_t(slot.index), layer.id);
        const core::LayerSelectionState selected{{layer.id}, layer.id, layer.id};
        if (!commitLayerStructure(i18n::text("project.layer.add"), std::move(tree), {}, {layer}, selected))
            return false;
        resolver.observe(*activeDocument_, *appearance.source);
    }
    synchronizeUi(true, false);
    return true;
} catch (const std::exception& error) {
    statusBar()->showMessage(QString::fromUtf8(error.what()), 6500);
    return false;
}

bool MainWindow::addProjectLayer(const QString& path) { return updateProjectLayer(path, {}, false); }
bool MainWindow::relinkProjectLayer(core::LayerId id, const QString& path) { return updateProjectLayer(path, id, false); }

bool MainWindow::refreshProjectLayer(core::LayerId id)
{
    const auto* layer = activeDocument_ ? session().document()->layer(id) : nullptr;
    return layer && layer->projectSource && updateProjectLayer(QString::fromStdString(layer->projectSource->path), id, true);
}

bool MainWindow::openProjectLayer(core::LayerId id)
{
    const auto* layer = activeDocument_ ? session().document()->layer(id) : nullptr;
    if (!layer || !layer->projectSource || fileBusy_ || workspaceDialog_) return false;
    const auto path = QString::fromStdString(layer->projectSource->path);
    if (auto* open = ProjectLayerResolver(*this).openSource(path)) return activateDocument(open->id);
    if (QFileInfo(path).isFile()) return openImageFromPath(path);
    statusBar()->showMessage(i18n::text("project.layer.missing"), 6500);
    return false;
}

bool MainWindow::convertProjectLayerToRaster(core::LayerId id)
try {
    if (!activeDocument_ || fileBusy_ || workspaceDialog_ || !settleForFileOperation()) return false;
    const auto* layer = session().document()->layer(id);
    if (!layer || !layer->projectSource) return false;
    auto plain = *layer;
    plain.payload = core::RasterLayer{core::copyColorRaster(*std::get<core::RasterLayer>(layer->payload).surface)};
    plain.projectSource.reset();
    replaceLayers(*activeDocument_, {std::move(plain)}, "Convert project layer to raster");
    synchronizeUi(true, false);
    return true;
} catch (const std::exception& error) {
    statusBar()->showMessage(QString::fromUtf8(error.what()), 6500);
    return false;
}
} // namespace imageeditor::ui
