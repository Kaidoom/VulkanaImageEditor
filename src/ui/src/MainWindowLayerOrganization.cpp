#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FlattenedDocument.hpp"
#include "imageeditor/ui/NativeRasterBake.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolIcons.hpp"
#include "imageeditor/ui/WorkspaceDialog.hpp"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <algorithm>
#include <cstring>

namespace imageeditor::ui {
namespace {
    using namespace core;
    void eraseSubtree(LayerTree& tree, LayerId id)
    {
        if (const auto* c = tree.container(id)) {
            const auto children = c->children;
            for (auto child : children)
                eraseSubtree(tree, child);
            std::erase_if(tree.containers, [id](const auto& c) { return c.id == id; });
        }
        if (auto p = tree.placement(id))
            std::erase(*tree.children(p->parent), id);
    }
    std::pair<std::string, ColorLabel> metadata(const Document& doc, LayerId id)
    {
        if (const auto* l = doc.layer(id))
            return { l->name, ColorLabel(l->colorLabel) };
        if (const auto* c = doc.tree().container(id))
            return { c->name, c->colorLabel };
        return { };
    }
    LayerSelectionState selected(std::vector<LayerId> ids)
    {
        const auto primary = ids.empty() ? std::nullopt : std::optional(ids.back());
        return { std::move(ids), primary, primary };
    }
}
void MainWindow::createLayerOrganizationActions()
{
    createLayerMaskActions();
    const auto action = [&](i18n::Message name, QString object, ToolGlyph glyph, const auto& callback) {
        auto* a = i18n::make<QAction>(toolGlyph(glyph), std::move(name), this);
        a->setObjectName(object);
        connect(a, &QAction::triggered, this, callback);
        registerEditorWindowAction(a);
        return a;
    };
    newFolderAction_ = action(i18n::text("main.window.layer.organization.new.folder"), "NewLayerFolderAction", ToolGlyph::Folder, [this] { createLayerFolder(); });
    groupLayersAction_ = action(i18n::text("main.window.layer.organization.group.selected.items"), "GroupLayersAction", ToolGlyph::Group, [this] { groupLayerItems(); });
    clippingGroupAction_ = action(i18n::text("main.window.layer.organization.add.to.clipping.mask.group"), "ClippingGroupAction", ToolGlyph::ClippingGroup, [this] { clippingGroupItems(); });
    ungroupLayersAction_ = action(i18n::text("main.window.layer.organization.ungroup"), "UngroupLayersAction", ToolGlyph::Group, [this] {if(session().activeLayer())dissolveLayerItem(*session().activeLayer()); });
    mergeLayersAction_ = action(i18n::text("main.window.layer.organization.merge.layers"), "MergeLayersAction", ToolGlyph::MergedVisible, [this] { mergeLayerItems(); });
    i18n::setToolTip(mergeLayersAction_, i18n::text("main.window.layer.organization.bake.selected.content.together.over.transparency.unselected.backdrop.layers"));
    rasterizeLayersAction_ = action(i18n::text("main.window.layer.organization.rasterize.layer"), "RasterizeLayersAction", ToolGlyph::ActiveLayer, [this] { rasterizeLayerItems(); });
    i18n::setToolTip(rasterizeLayersAction_, i18n::text("main.window.layer.organization.commit.selected.layers.to.native.document.pixels.separately.retaining"));
    duplicateLayersAction_ = action(i18n::text("main.window.layer.organization.duplicate.selected.layers"), "DuplicateLayersAction", ToolGlyph::ActiveLayer, [this] { duplicateLayerItems(); });
    duplicateLayersAction_->setShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_D));
    duplicateLayersAction_->setAutoRepeat(false);
    renameLayerAction_ = action(i18n::text("main.window.layer.organization.rename"), "RenameLayerItemAction", ToolGlyph::ActiveLayer, [this] {
        if (session().activeLayer() && !editorTextInputActive() && !(textController_ && textController_->active()))
            renameLayerItem(*session().activeLayer());
    });
    renameLayerAction_->setShortcut(QKeySequence(Qt::Key_F2));
    const std::array names { "HideSelectedLayersAction", "ShowSelectedLayersAction", "IsolateSelectedLayersAction", "ShowAllLayersAction" };
    const std::array titles { i18n::text("main.window.layer.organization.hide.selected"), i18n::text("main.window.layer.organization.reveal.selected"), i18n::text("main.window.layer.organization.isolate.selected"), i18n::text("main.window.layer.organization.show.all.layers") };
    const std::array shortcuts { QKeySequence(Qt::Key_H), QKeySequence(Qt::AltModifier | Qt::Key_H),
        QKeySequence(Qt::ShiftModifier | Qt::Key_H), QKeySequence(Qt::ShiftModifier | Qt::AltModifier | Qt::Key_H) };
    for (std::size_t i = 0; i < layerVisibilityActions_.size(); ++i) {
        auto* a = i18n::make<QAction>(titles[i], this);
        a->setObjectName(QString::fromLatin1(names[i]));
        a->setShortcut(shortcuts[i]);
        a->setAutoRepeat(false);
        registerEditorWindowAction(a);
        connect(a, &QAction::triggered, this, [this, i] { changeLayerVisibility(core::VisibilityOperation(i)); });
        layerVisibilityActions_[i] = a;
    }
}
void MainWindow::changeLayerVisibility(core::VisibilityOperation operation)
{
    if (fileBusy_ || !session().document() || !settleForFileOperation()) return;
    try {
        if (session().execute(std::make_unique<core::SetItemsVisibilityCommand>(
                *session().document(), session().selectedLayers(), operation))) {
            fileState().untouched = false;
            synchronizeUi(false, false);
            layerList_->viewport()->update();
            canvasWindow_->refreshColorSample();
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.could.not.change.layer.visibility.1").arg(QString::fromUtf8(error.what())), 5000);
    }
}
bool MainWindow::commitLayerStructure(const i18n::Message& label, core::LayerTree after,
    std::vector<core::LayerId> removed, std::vector<core::Layer> added, core::LayerSelectionState afterSelection,
    std::vector<core::ItemVisibilityUpdate> retainedLeafVisibility)
{
    if (fileBusy_ || !session().document())
        return false;
    bool applied = false;
    try {
        auto command = std::make_unique<core::LayerStructureCommand>(label.source().toStdString(), *session().document(),
            std::move(after), std::move(removed), std::move(added), session().layerSelectionState(), std::move(afterSelection), std::move(retainedLeafVisibility));
        if (!session().execute(std::move(command)))
            return false;
        applied = true;
        fileState().untouched = false;
        synchronizeUi(true, false);
        return true;
    } catch (const std::exception& e) {
        statusBar()->showMessage((applied ? i18n::text("main.window.layer.organization.layers.updated.but.display.refresh.failed.1")
                                         : i18n::text("main.window.layer.organization.layer.organization.failed.document.unchanged.1"))
                                     .arg(QString::fromUtf8(e.what())), 6000);
        return applied;
    }
}
void MainWindow::createLayerFolder(std::optional<core::LayerId> contextItem)
{
    if (!session().document() || !settleForFileOperation())
        return;
    auto tree = session().document()->tree();
    // The target belongs to the invocation, never to a stale active row.
    // Empty-space/global creation is root-bottom; explicit folder context nests.
    core::ItemPlacement placement { 0, 0 };
    if (contextItem) {
        const auto* c = tree.container(*contextItem);
        if (c && c->kind == core::ContainerKind::Folder)
            placement = { *contextItem, c->children.size() };
        else if (!tree.placement(*contextItem))
            return;
    }
    const auto id = core::makeLayerId();
    tree.containers.push_back({ id, "Folder", core::ContainerKind::Folder, core::ColorLabel::None, { } });
    auto& siblings = *tree.children(placement.parent);
    siblings.insert(siblings.begin() + std::ptrdiff_t(placement.index), id);
    commitLayerStructure(i18n::text("main.window.layer.organization.new.folder.2"), std::move(tree), { }, { }, selected({ id }));
}
void MainWindow::addSelectionToNewFolder()
{
    if (!session().document() || !settleForFileOperation())
        return;
    auto tree = session().document()->tree();
    const auto ids = tree.normalize(session().selectedLayers());
    if (ids.empty())
        return;
    // Gather at the highest selected item's stack slot. Unlike Group/Merge,
    // this explicit organization action may collect nonconsecutive siblings
    // or items from other parents, retaining their relative traversal order.
    const auto top = *tree.placement(ids.back());
    const auto folder = core::makeLayerId();
    tree.containers.push_back({ folder, "Folder", core::ContainerKind::Folder, core::ColorLabel::None, { } });
    auto& siblings = *tree.children(top.parent);
    siblings.insert(siblings.begin() + std::ptrdiff_t(top.index + 1), folder);
    if (!tree.reparent(ids, { folder, 0 }))
        return;
    commitLayerStructure(i18n::text("main.window.layer.organization.add.to.new.folder"), std::move(tree), { }, { }, selected({ folder }));
}
void MainWindow::groupLayerItems()
{
    if (!session().document() || !settleForFileOperation())
        return;
    auto tree = session().document()->tree();
    const auto ids = tree.normalize(session().selectedLayers());
    if (!tree.consecutiveSiblings(ids)) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.group.requires.consecutive.items.in.the.same.folder.reorder"), 6000);
        return;
    }
    const auto p = *tree.placement(ids.front());
    const auto id = core::makeLayerId();
    tree.containers.push_back({ id, "Group", core::ContainerKind::Group, core::ColorLabel::None, ids });
    auto& siblings = *tree.children(p.parent);
    siblings.erase(siblings.begin() + std::ptrdiff_t(p.index), siblings.begin() + std::ptrdiff_t(p.index + ids.size()));
    siblings.insert(siblings.begin() + std::ptrdiff_t(p.index), id);
    commitLayerStructure(i18n::text("main.window.layer.organization.group.layers"), std::move(tree), { }, { }, selected({ id }));
}
void MainWindow::clippingGroupItems()
{
    if (!session().document() || !settleForFileOperation()) return;
    auto tree=session().document()->tree();
    const auto ids=tree.normalize(session().selectedLayers());
    if(ids.empty())return;
    if(ids.size()==1)if(auto* c=tree.container(ids.front()); c && core::isGroup(c->kind)) {
        const bool release=c->kind==core::ContainerKind::ClippingMaskGroup;
        c->kind=release?core::ContainerKind::Group:core::ContainerKind::ClippingMaskGroup;
        commitLayerStructure(release?i18n::text("main.window.layer.organization.release.clipping"):i18n::text("main.window.layer.organization.convert.to.clipping.mask.group"),std::move(tree),{},{},selected(ids));
        return;
    }
    const auto top=*tree.placement(ids.back());
    const auto id=core::makeLayerId();
    tree.containers.push_back({id,"Clipping Mask Group",core::ContainerKind::ClippingMaskGroup,core::ColorLabel::None,{}});
    auto& siblings=*tree.children(top.parent);
    siblings.insert(siblings.begin()+std::ptrdiff_t(top.index+1),id);
    if(tree.reparent(ids,{id,0}))commitLayerStructure(i18n::text("main.window.layer.organization.add.to.clipping.mask.group.2"),std::move(tree),{},{},selected({id}));
}
void MainWindow::dissolveLayerItem(core::LayerId id)
{
    if (!session().document() || !settleForFileOperation())
        return;
    auto tree = session().document()->tree();
    const auto* c = tree.container(id);
    if (!c)
        return;
    auto selection = session().layerSelectionState();
    if (std::ranges::find(selection.ids, id) != selection.ids.end()) {
        std::erase(selection.ids, id);
        for (auto child : c->children)
            if (std::ranges::find(selection.ids, child) == selection.ids.end())
                selection.ids.push_back(child);
    }
    const auto replacement = c->children.empty() ? std::nullopt : std::optional(c->children.back());
    if (selection.primary == id)
        selection.primary = replacement;
    if (selection.anchor == id)
        selection.anchor = replacement;
    const auto label = core::isGroup(c->kind) ? i18n::text("main.window.layer.organization.ungroup") : i18n::text("main.window.layer.organization.remove.folder.keep.contents");
    std::vector<core::ItemVisibilityUpdate> hiddenLeaves;
    if (!c->visible) {
        // Removing a hidden gate must not suddenly reveal its contents. Carry
        // it onto immediate children only, atomically with dissolution. Undo
        // restores their original bits without retaining any raster copies.
        for (auto child : c->children) {
            if (auto* container = tree.container(child)) container->visible = false;
            else if (session().document()->layer(child)->visible)
                hiddenLeaves.push_back({ child, true, false });
        }
    }
    if (tree.dissolve(id))
        commitLayerStructure(label, std::move(tree), { }, { }, selection, std::move(hiddenLeaves));
}
void MainWindow::duplicateLayerItems()
{
    if (fileBusy_ || !session().document() || editorTextInputActive()
        || (textController_ && textController_->active()) || !settleForFileOperation()) return;
    try {
        auto command = core::duplicateLayerItems(*session().document(), session().layerSelectionState());
        if (command && session().execute(std::move(command))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& error) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.could.not.duplicate.layers.1").arg(QString::fromUtf8(error.what())), 5000);
    }
}
void MainWindow::deleteActiveLayer()
{
    if (fileBusy_ || !session().document() || editorTextInputActive()
        || (textController_ && textController_->active()) || !settleForFileOperation()) return;
    const auto& doc = *session().document();
    const auto ids = doc.tree().normalize(session().selectedLayers());
    if (ids.empty()) return;
    const auto leaves = doc.expandedLayers(ids);
    if (leaves.size() >= doc.layers().size()) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.a.document.must.keep.at.least.one.content.layer"), 4000); return;
    }
    if (std::ranges::any_of(ids, [&](auto id) { const auto* c=doc.tree().container(id); return c && !c->children.empty(); })
        && !confirmLayerChange(i18n::text("main.window.layer.organization.delete.selected.items"), i18n::text("main.window.layer.organization.delete.the.selected.containers.and.layers.including.their.contents"))) return;
    auto tree = doc.tree();
    for (auto id : ids) eraseSubtree(tree, id);
    auto afterSelection = selected({ tree.roots.back() });
    commitLayerStructure(i18n::text("main.window.layer.organization.delete.selected.layers"), std::move(tree), leaves, {},
        std::move(afterSelection));
}
void MainWindow::removeLayerItem(core::LayerId id, bool contents)
{
    if (!session().document() || !settleForFileOperation())
        return;
    const auto& doc = *session().document();
    if (doc.tree().container(id) && !contents) {
        dissolveLayerItem(id);
        return;
    }
    const auto leaves = doc.tree().descendants(id);
    if (leaves.size() >= doc.layers().size()) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.a.document.must.keep.at.least.one.content.layer"), 4000);
        return;
    }
    if (contents && !leaves.empty()) {
        if (!confirmLayerChange(i18n::text("main.window.layer.organization.delete.contents"), i18n::text("main.window.layer.organization.delete.this.container.and.its.1.content.layers.this").arg(leaves.size())))
            return;
    }
    auto tree = doc.tree();
    eraseSubtree(tree, id);
    auto state = session().layerSelectionState();
    std::erase_if(state.ids, [&](auto selectedId) { return !tree.placement(selectedId); });
    if (state.ids.empty() && !tree.roots.empty())
        state = selected({ tree.roots.back() });
    else if (state.primary == id || (state.primary && !tree.placement(*state.primary)))
        state.primary = state.ids.empty() ? std::nullopt : std::optional(state.ids.back());
    if (state.anchor && !tree.placement(*state.anchor))
        state.anchor = state.primary;
    commitLayerStructure(contents ? i18n::text("main.window.layer.organization.delete.container.and.contents") : i18n::text("main.window.layer.organization.delete.layer"), std::move(tree), leaves, { }, std::move(state));
}
bool MainWindow::setLayerItemName(core::LayerId id, const QString& name)
{
    if (!session().document() || fileBusy_ || name.trimmed().isEmpty() || !settleForFileOperation())
        return false;
    const auto info = metadata(*session().document(), id);
    if (!session().execute(std::make_unique<core::SetItemMetadataCommand>(id, name.trimmed().toStdString(), info.second)))
        return false;
    synchronizeUi(true, false);
    return true;
}
void MainWindow::renameLayerItem(core::LayerId id)
{
    if (fileBusy_ || workspaceDialog_ || !session().document() || !settleForFileOperation())
        return;
    if (layersPanelAction_ && !layersPanelAction_->isChecked()) layersPanelAction_->trigger();
    if (auto* view = dynamic_cast<LayerListView*>(layerList_))
        view->beginRename(id);
}
void MainWindow::setLayerItemLabel(core::LayerId id, core::ColorLabel label)
{
    if (!session().document() || !settleForFileOperation())
        return;
    const auto info = metadata(*session().document(), id);
    if (session().execute(std::make_unique<core::SetItemMetadataCommand>(id, info.first, label)))
        synchronizeUi(true, false);
}
void MainWindow::showLayerItemMenu(const QPoint& point)
{
    if (fileBusy_ || !session().document())
        return;
    const auto index = layerList_->indexAt(point);
    const auto id = layerModel_->layerIdAt(index.row());
    const auto* view = static_cast<LayerListView*>(layerList_);
    const auto* layer = id ? session().document()->layer(*id) : nullptr;
    const bool maskThumbnail = layer && layer->mask && view->thumbnailRect(index,true).contains(point);
    const bool contentThumbnail = layer && view->thumbnailRect(index,false).contains(point);
    // Also cover keyboard/synthetic context-menu invocation. The pointer path
    // already selected on press, so do not settle/cancel the interaction twice.
    if (id && (!session().isLayerSelected(*id) || session().activeLayer() != id))
        selectLayerFromRow(layerModel_->rowForLayer(*id), Qt::NoModifier, true);
    if(maskThumbnail || contentThumbnail) {
        session().setEditingLayerMask(maskThumbnail);
        synchronizeUi(false,false);
    }
    QMenu menu(layerList_);
    if(maskThumbnail) {
        menu.setObjectName(QStringLiteral("LayerMaskContextMenu"));
        // This thumbnail already owns a mask: offer only operations on it.
        for(size_t i=2;i<maskActions_.size();++i)menu.addAction(maskActions_[i]);
        menu.addAction(refineMaskAction_);
        menu.exec(layerList_->viewport()->mapToGlobal(point));
        return;
    }
    menu.setObjectName(QStringLiteral("LayerContextMenu"));
    menu.addAction(newAdjustmentLayerAction_);
    if(layer && std::holds_alternative<core::AdjustmentLayer>(layer->payload)) {
        auto* scopeMenu=menu.addMenu(i18n::text("main.window.layer.organization.adjustment.scope"));
        const auto scope=std::get<core::AdjustmentLayer>(layer->payload).scope;
        const auto position=session().document()->tree().placement(layer->id);
        for(auto value:{core::AdjustmentScope::AllBelow,core::AdjustmentScope::ThisGroup}) {
            if(value==core::AdjustmentScope::ThisGroup && (!position || !position->parent))continue;
            auto* action=i18n::addAction(scopeMenu, value==core::AdjustmentScope::AllBelow?i18n::text("main.window.layer.organization.all.below"):i18n::text("main.window.layer.organization.this.group"));
            action->setCheckable(true);action->setChecked(value==scope || ((!position||!position->parent)&&value==core::AdjustmentScope::AllBelow));
            connect(action,&QAction::triggered,this,[this,id=*id,scope,value,instance=activeDocumentId()]{
                if(activeDocumentId()!=instance)return;
                if(executeDocumentCommand(std::make_unique<core::SetAdjustmentScopeCommand>(id,scope,value)))synchronizeUi(true,false);
            });
        }
    }
    auto* newFolder = menu.addAction(toolGlyph(ToolGlyph::Folder), i18n::text("main.window.layer.organization.new.folder"),
        this, [this, id] { createLayerFolder(id); });
    newFolder->setObjectName(QStringLiteral("ContextNewLayerFolderAction"));
    if (!session().document()->tree().normalize(session().selectedLayers()).empty()) {
        auto* gather = menu.addAction(toolGlyph(ToolGlyph::Folder), i18n::text("main.window.layer.organization.add.to.new.folder.2"),
            this, [this] { addSelectionToNewFolder(); });
        gather->setObjectName(QStringLiteral("AddToNewLayerFolderAction"));
    }
    if (id) {
        menu.addAction(i18n::text("main.window.layer.organization.rename"), this, [this, id] { renameLayerItem(*id); });
        auto* labels = menu.addMenu(i18n::text("main.window.layer.organization.color.label"));
        const QStringList names { i18n::text("main.window.layer.organization.none"), i18n::text("main.window.layer.organization.red"), i18n::text("main.window.layer.organization.orange"), i18n::text("main.window.layer.organization.yellow"), i18n::text("main.window.layer.organization.green"), i18n::text("main.window.layer.organization.blue"), i18n::text("main.window.layer.organization.purple") };
        const auto current = metadata(*session().document(), *id).second;
        for (int i = 0; i < names.size(); ++i) {
            auto* action = labels->addAction(names[i], this, [this, id, i] { setLayerItemLabel(*id, core::ColorLabel(i)); });
            action->setCheckable(true);
            action->setChecked(int(current) == i);
        }
        if (const auto* c = session().document()->tree().container(*id)) {
            menu.addSeparator();
            for (bool immediate : { true, false })
                menu.addAction(immediate ? i18n::text("main.window.layer.organization.select.immediate.layers") : i18n::text("main.window.layer.organization.select.all.descendant.layers"), this, [this, id, immediate] {
                    cancelPendingEdits();
                    const auto ids = session().document()->tree().descendants(*id, immediate);
                    session().setLayerSelection(ids, ids.empty() ? std::nullopt : std::optional(ids.back()));
                    synchronizeUi(false, false);
                });
            menu.addAction(core::isGroup(c->kind) ? i18n::text("main.window.layer.organization.ungroup") : c->children.empty() ? i18n::text("main.window.layer.organization.remove.empty.folder")
                                                                                                       : i18n::text("main.window.layer.organization.remove.folder.keep.contents"),
                this, [this, id] { dissolveLayerItem(*id); });
            if (!c->children.empty())
                menu.addAction(i18n::text("main.window.layer.organization.delete.container.and.contents.2"), this, [this, id] { removeLayerItem(*id, true); });
        }
    }
    menu.addAction(duplicateLayersAction_);
    menu.addAction(deleteLayerAction_);
    menu.addSeparator();
    for (auto* action : layerVisibilityActions_) menu.addAction(action);
    menu.addSeparator();
    menu.addAction(groupLayersAction_);
    menu.addAction(clippingGroupAction_);
    menu.addAction(mergeLayersAction_);
    menu.addAction(rasterizeLayersAction_);
    menu.addSeparator();
    for (auto* action : maskActions_) menu.addAction(action);
    menu.exec(layerList_->viewport()->mapToGlobal(point));
}
void MainWindow::mergeLayerItems()
{
    if (!session().document() || !settleForFileOperation())
        return;
    auto& doc = *session().document();
    const auto selection = session().layerSelectionState();
    const auto ids = doc.tree().normalize(selection.ids);
    if (ids.empty()) return;
    if (ids.size() == 1 && doc.layer(ids.front())) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.select.at.least.two.layers.or.one.folder.or"), 5000);
        return;
    }
    const auto leaves = doc.expandedLayers(ids);
    const auto hidden = std::ranges::count_if(leaves, [&](auto id) { return !doc.isEffectivelyVisible(id); });
    if (hidden && !confirmLayerChange(i18n::text("main.window.layer.organization.merge.discards.hidden.content"), i18n::text("main.window.layer.organization.1.hidden.layers.will.be.discarded.by.this.merge").arg(hidden)))
        return;
    try {
        const auto budget = session().history().memoryBudget();
        std::size_t retained = 2 * doc.tree().memoryCost() + 65536;
        for (auto id : leaves)
            retained += core::retainedLayerMemory(*doc.layer(id));
        if (retained >= budget)
            throw std::runtime_error("Original layers exceed the merge undo budget (256 MiB). No layers were changed.");
        FlattenedDocumentLimits limits;
        limits.outputPixels = std::min<std::uint64_t>(limits.outputPixels, (budget - retained) / 4);
        QProgressDialog progress(i18n::text("main.window.layer.organization.merging.authoritative.layer.content"), i18n::text("main.window.layer.organization.cancel"), 0, 1000, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(180);
        progress.setAutoClose(false);
        QElapsedTimer timer;
        timer.start();
        FlattenedDocumentResult result;
        {
            const QScopedValueRollback busy(fileBusy_, true);
            const QScopedValueRollback active(fileProgress_, &progress);
            result = flattenLayerItems(doc, ids, [&](std::uint64_t done, std::uint64_t total) {
                if(timer.elapsed()>=12){progress.setValue(int(done*1000/std::max<std::uint64_t>(total,1)));QApplication::processEvents();timer.restart();}
                return !progress.wasCanceled(); }, limits);
        }
        progress.close();
        if (!result) {
            if (!result.cancelled)
                statusBar()->showMessage(result.error, 7000);
            return;
        }
        auto surface=surfaceFromNativeImage(result.image);
        result.image = { };
        auto layer = core::Layer::raster(ids.size() == 1 ? metadata(doc, ids.front()).first : "Merged layers",
            std::move(surface));
        layer.localToDocument.m02 = result.origin.x;
        layer.localToDocument.m12 = result.origin.y;
        auto command = core::consolidateLayerItems(doc, selection, std::move(layer));
        if (command->memoryCost() > budget)
            throw std::runtime_error("Merge exceeds the undo budget. No layers were changed.");
        if (session().execute(std::move(command))) {
            fileState().untouched = false;
            synchronizeUi(true, false);
        }
    } catch (const std::exception& e) {
        statusBar()->showMessage(i18n::text("main.window.layer.organization.merge.failed.1").arg(QString::fromUtf8(e.what())), 7000);
    }
}
void MainWindow::rasterizeLayerItems()
{
    if(fileBusy_ || !session().document() || !settleForFileOperation())return;
    auto& doc=*session().document();
    const auto selection=session().layerSelectionState();
    QProgressDialog progress(i18n::text("main.window.layer.organization.rasterizing.authoritative.layer.content"),i18n::text("main.window.layer.organization.cancel"),0,1000,this);
    progress.setWindowModality(Qt::WindowModal);progress.setMinimumDuration(180);progress.setAutoClose(false);
    QElapsedTimer timer;timer.start();
    RasterizeResult result;
    {
        const QScopedValueRollback busy(fileBusy_,true);
        const QScopedValueRollback active(fileProgress_,&progress);
        result=prepareRasterizeLayers(doc,selection,session().history().memoryBudget(),[&](auto done,auto total){
            if(timer.elapsed()>=12){progress.setValue(int(done*1000/std::max<std::uint64_t>(total,1)));QApplication::processEvents();timer.restart();}
            return !progress.wasCanceled();
        });
    }
    progress.close();
    if(!result.error.isEmpty()){statusBar()->showMessage(i18n::text("main.window.layer.organization.rasterize.failed.1").arg(result.error),7000);return;}
    if(result.command) {
        try {if(session().execute(std::move(result.command))){fileState().untouched=false;synchronizeUi(true,false);}}
        catch(const std::exception& e){statusBar()->showMessage(QString::fromUtf8(e.what()),7000);}
    }
}
bool MainWindow::confirmLayerChange(const QString& title,const QString& question)
{
    if(workspaceDialog_)return false;
    WorkspaceDialog presenter(*workspace_,*this);
    QMessageBox dialog(QMessageBox::Warning,title,question,QMessageBox::Yes|QMessageBox::Cancel,&presenter);
    dialog.setWindowFlags(Qt::Widget);dialog.setDefaultButton(QMessageBox::Cancel);
    const QScopedValueRollback active(workspaceDialog_,&presenter);
    return presenter.exec(dialog)==QMessageBox::Yes;
}
} // namespace imageeditor::ui
