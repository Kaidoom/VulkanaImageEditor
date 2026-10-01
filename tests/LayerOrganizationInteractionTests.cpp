#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PopupOwnership.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolIcons.hpp"

#include <QAction>
#include <QAbstractItemDelegate>
#include <QApplication>
#include <QContextMenuEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>

#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace core = imageeditor::core;
namespace render = imageeditor::render;
namespace ui = imageeditor::ui;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)
void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
bool nearMatrix(const core::AffineTransform& a, const core::AffineTransform& b)
{
    for (const auto p : { core::Vec2d {}, core::Vec2d { 1, 0 }, core::Vec2d { 0, 1 } }) {
        const auto d = a.map(p) - b.map(p);
        if (std::hypot(d.x, d.y) > 1e-8) return false;
    }
    return true;
}

struct Fixture {
    QTemporaryDir assets;
    ui::MainWindow window;
    render::CanvasWindow* canvas {};
    ui::LayerListView* view {};
    ui::LayerListModel* model {};
    std::array<core::LayerId, 3> members {};
    core::LayerId spare {};
    explicit Fixture(QVulkanInstance* instance = nullptr)
        : window(instance, false, false)
    {
        window.setUnsavedPromptEnabled(false);
        window.resize(1440, 920);
        window.show();
        settle();
        for (auto* w : QGuiApplication::allWindows())
            if (w->objectName() == QStringLiteral("VulkanCanvasWindow")) canvas = dynamic_cast<render::CanvasWindow*>(w);
        view = dynamic_cast<ui::LayerListView*>(window.findChild<QListView*>(QStringLiteral("LayerList")));
        model = view ? dynamic_cast<ui::LayerListModel*>(view->model()) : nullptr;
        QImage base(128, 128, QImage::Format_RGBA8888);
        base.fill(QColor(180, 50, 70, 255));
        const auto imagePath = assets.filePath(QStringLiteral("layer.png"));
        CHECK(base.save(imagePath));
        CHECK(window.openImageFromPath(imagePath));
        members[0] = session().activeLayer().value_or(0);
        core::ShapeLayer shape;
        shape.size = { 30, 20 };
        shape.strokeEnabled = true;
        shape.strokeWidth = 3;
        auto shapeLayer = core::Layer::shape("Shape member", shape);
        shapeLayer.localToDocument = { 1, 0, 40, 0, 1, 40 };
        members[1] = shapeLayer.id;
        CHECK(document().insertLayer(document().layers().size(), std::move(shapeLayer)));
        core::TextLayer text;
        text.utf8 = "Group";
        text.defaultStyle.sizePixels = 14;
        auto textLayer = core::Layer::text("Text member", text);
        textLayer.localToDocument = { 1, 0, 15, 0, 1, 80 };
        members[2] = textLayer.id;
        CHECK(document().insertLayer(document().layers().size(), std::move(textLayer)));
        // A normal import publishes the fixture through the real caches/model.
        QImage top(8, 8, QImage::Format_RGBA8888);
        top.fill(QColor(40, 170, 130, 255));
        const auto topPath = assets.filePath(QStringLiteral("spare.png"));
        CHECK(top.save(topPath));
        CHECK(window.importImageAsLayerFromPath(topPath));
        spare = session().activeLayer().value_or(0);
        document().setLayerVisibility(members[0], false);
        session().history().clear();
        document().markSaved();
        if (model) model->refresh();
        settle();
    }
    ~Fixture() { window.close(); settle(); }
    core::EditorSession& session() { return const_cast<core::EditorSession&>(window.editorSession()); }
    core::Document& document() { return *session().document(); }
    bool valid() const { return canvas && view && model && spare != 0; }
    QAction* action(const char* name) { return window.findChild<QAction*>(QString::fromLatin1(name)); }
    bool trigger(const char* name)
    {
        auto* a = action(name);
        CHECK(a && a->isEnabled());
        if (!a || !a->isEnabled()) return false;
        a->trigger(); settle(); return true;
    }
    bool shortcut(const QKeySequence& sequence)
    {
        for (auto* a : window.findChildren<QAction*>()) {
            if (a->shortcuts().contains(sequence)) {
                CHECK(a->isEnabled());
                if (!a->isEnabled()) return false;
                a->trigger(); settle(); return true;
            }
        }
        CHECK(false); return false;
    }
    void visibilityKey(Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QTest::keyClick(canvas, Qt::Key_H, modifiers);
        settle();
    }
    QModelIndex row(core::LayerId id) const { return model->index(model->rowForLayer(id), 0); }
    void click(core::LayerId id, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const auto index = row(id);
        CHECK(index.isValid()); if (!index.isValid()) return;
        view->scrollTo(index); settle();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, modifiers, view->visualRect(index).center());
        settle();
    }
    void rightClick(core::LayerId id)
    {
        const auto index = row(id);
        CHECK(index.isValid()); if (!index.isValid()) return;
        view->scrollTo(index); settle();
        QTest::mouseClick(view->viewport(), Qt::RightButton, Qt::NoModifier, view->visualRect(index).center());
        settle();
    }
    // QTest pointer events do not synthesize a platform context-menu event.
    // Invoke the same view signal explicitly, exercising menu targeting even
    // without a preceding right-button press (as with keyboard invocation).
    bool menuCommand(std::optional<core::LayerId> target, const char* identifier, bool matchText = false)
    {
        QPoint point(-1, -1); // No row: the empty-space context branch.
        if (target) {
            const auto index = row(*target);
            CHECK(index.isValid()); if (!index.isValid()) return false;
            view->scrollTo(index); settle();
            point = view->visualRect(index).center();
        }
        bool invoked = false;
        QTimer::singleShot(0, &window, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                for (auto* candidate : window.findChildren<QMenu*>())
                    if (candidate->isVisible()) { menu = candidate; break; }
            }
            CHECK(menu);
            if (!menu) return;
            const QString requested = QString::fromUtf8(identifier);
            std::function<QAction*(QMenu*)> findAction = [&](QMenu* current) -> QAction* {
                for (auto* action : current->actions()) {
                    if ((matchText ? action->text() : action->objectName()) == requested) return action;
                    if (action->menu()) if (auto* found = findAction(action->menu())) return found;
                }
                return nullptr;
            };
            auto* action = findAction(menu);
            CHECK(action && action->isEnabled());
            menu->close();
            if (action && action->isEnabled()) { invoked = true; action->trigger(); }
        });
        view->customContextMenuRequested(point);
        settle();
        return invoked;
    }
    void disclosure(core::LayerId id)
    {
        const auto index = row(id);
        CHECK(index.isValid()); if (!index.isValid()) return;
        view->scrollTo(index); settle();
        const auto rect = view->visualRect(index);
        const QPoint position(rect.left() + model->rowIndent(index.row()) - 10, rect.center().y());
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        settle();
    }
    void eye(core::LayerId id)
    {
        const auto index = row(id);
        CHECK(index.isValid()); if (!index.isValid()) return;
        view->scrollTo(index); settle();
        const auto rect = view->visibilityIndicatorRect(index);
        CHECK(!rect.isEmpty()); if (rect.isEmpty()) return;
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        settle();
    }
    QPointF logical(core::Vec2d p) const
    {
        const auto& scene = canvas->scene();
        const auto extent = window.editorSession().document()->canvas().extent;
        const auto mapped = scene.viewport.documentToViewport(p,
            { double(extent.width), double(extent.height) }, scene.logicalViewport);
        return { mapped.x, mapped.y };
    }
    void mouse(QEvent::Type type, core::Vec2d p, Qt::MouseButton button, Qt::MouseButtons buttons)
    {
        const auto local = logical(p);
        const QPointF global(canvas->mapToGlobal(local.toPoint()));
        QMouseEvent event(type, local, local, global, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &event);
    }
    void drag(core::Vec2d from, core::Vec2d to)
    {
        mouse(QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton);
        mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
        settle();
    }
    core::LayerId group()
    {
        click(members[0]);
        click(members[1], Qt::ControlModifier);
        click(members[2], Qt::ControlModifier);
        CHECK(session().selectedLayers().size() == 3);
        if (!trigger("GroupLayersAction")) return 0;
        const auto id = session().activeLayer().value_or(0);
        CHECK(document().tree().container(id));
        return id;
    }
};

void contextClicksSelectTheirTargetWithoutCollapsingSelectedSets()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.members[1]); f.click(f.members[2], Qt::ControlModifier);
    const auto before = f.session().layerSelectionState();
    const auto history = f.session().history().undoDepth();
    const auto content = f.document().contentState();
    f.rightClick(f.members[1]);
    CHECK(f.session().selectedLayers() == before.ids);
    CHECK(f.session().activeLayer() == f.members[1]);
    CHECK(f.session().layerSelectionState().anchor == before.anchor);
    CHECK(f.row(f.members[1]).data(Qt::UserRole + 1).toBool());
    CHECK(f.view->selectionModel()->isSelected(f.row(f.members[1])));
    CHECK(f.view->selectionModel()->isSelected(f.row(f.members[2])));
    f.rightClick(f.spare);
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { f.spare });
    CHECK(f.session().activeLayer() == f.spare);
    CHECK(f.view->selectionModel()->isSelected(f.row(f.spare)));
    CHECK(f.session().history().undoDepth() == history && f.document().contentState() == content);

    // A context menu invoked without pointer press must select its row too.
    CHECK(f.menuCommand(f.members[2], "Blue", true));
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { f.members[2] });
    CHECK(f.session().activeLayer() == f.members[2]);
    CHECK(f.view->selectionModel()->isSelected(f.row(f.members[2])));
    CHECK(f.document().layer(f.members[2])->colorLabel == std::uint8_t(core::ColorLabel::Blue));
    CHECK(f.session().history().undoDepth() == history + 1);
}

void newFolderUsesTheExplicitContextInsteadOfTheCurrentSelection()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto originalRoots = f.document().tree().roots;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().placement(folder) == core::ItemPlacement { 0, 0 }));
    CHECK(f.document().tree().roots.back() == originalRoots.back());
    const auto before = f.document().tree();
    const auto selection = f.session().layerSelectionState();
    // The selected folder is intentionally not the empty-space destination.
    CHECK(f.menuCommand(std::nullopt, "ContextNewLayerFolderAction"));
    const auto rootFolder = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().placement(rootFolder) == core::ItemPlacement { 0, 0 }));
    CHECK(f.document().tree().container(folder)->children.empty());
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == before && f.session().layerSelectionState() == selection);

    // Explicitly targeting the folder is the only route to child creation.
    CHECK(f.menuCommand(folder, "ContextNewLayerFolderAction"));
    const auto firstChild = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().placement(firstChild) == core::ItemPlacement { folder, 0 }));
    CHECK(f.menuCommand(folder, "ContextNewLayerFolderAction"));
    const auto secondChild = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().placement(secondChild) == core::ItemPlacement { folder, 1 }));
    // A leaf context also creates at root bottom. Only an explicit folder
    // context can create inside any existing container.
    CHECK(f.menuCommand(f.members[1], "ContextNewLayerFolderAction"));
    const auto sibling = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().placement(sibling) == core::ItemPlacement { 0, 0 }));
}

void addToNewFolderUsesTopSelectedPositionAndExactUndo()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.members[0]); f.click(f.members[2], Qt::ControlModifier);
    f.rightClick(f.members[0]); // Context primary is included in the undo state.
    const auto selection = f.session().layerSelectionState();
    const auto original = f.document().tree();
    const auto history = f.session().history().undoDepth();
    std::array<core::AffineTransform, 3> matrices;
    for (std::size_t i = 0; i < f.members.size(); ++i) matrices[i] = f.document().layer(f.members[i])->localToDocument;
    const auto raster = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
    CHECK(f.menuCommand(f.members[0], "AddToNewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    const auto* item = f.document().tree().container(folder);
    CHECK(item && item->kind == core::ContainerKind::Folder);
    if (!item) return;
    CHECK((item->children == std::vector<core::LayerId> { f.members[0], f.members[2] }));
    CHECK((f.document().tree().roots == std::vector<core::LayerId> { f.members[1], folder, f.spare }));
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { folder });
    CHECK(f.session().history().undoDepth() == history + 1);
    for (std::size_t i = 0; i < matrices.size(); ++i) CHECK(f.document().layer(f.members[i])->localToDocument == matrices[i]);
    CHECK(std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface == raster);
    const auto organized = f.document().tree();
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == original && f.session().layerSelectionState() == selection);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.document().tree() == organized && f.session().activeLayer() == folder);
    CHECK(f.shortcut(QKeySequence::Undo));
    // Empty-space menus retain the existing multi-selection as valid input.
    CHECK(f.menuCommand(std::nullopt, "AddToNewLayerFolderAction"));
    const auto fromEmpty = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().container(fromEmpty)->children == std::vector<core::LayerId> { f.members[0], f.members[2] }));
    CHECK((f.document().tree().roots == std::vector<core::LayerId> { f.members[1], fromEmpty, f.spare }));
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == original && f.session().layerSelectionState() == selection);
}

void addToNewFolderRetainsCollapsedTargetsAndNormalizesAncestors()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto originalFolder = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> data(f.model->mimeData({ f.row(f.members[0]) }));
    CHECK(f.model->dropMimeData(data.get(), Qt::MoveAction, -1, 0, f.row(originalFolder)));
    settle();
    f.click(f.members[0]); f.click(f.members[2], Qt::ControlModifier);
    f.disclosure(originalFolder);
    CHECK(f.model->rowForLayer(f.members[0]) < 0);
    const auto before = f.document().tree();
    const auto selection = f.session().layerSelectionState();
    CHECK(f.menuCommand(std::nullopt, "AddToNewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().container(folder)->children == std::vector<core::LayerId> { f.members[0], f.members[2] }));
    CHECK(f.document().tree().container(originalFolder)->children.empty());
    CHECK((f.document().tree().roots == std::vector<core::LayerId> { originalFolder, f.members[1], folder, f.spare }));
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == before && f.session().layerSelectionState() == selection);
    CHECK(!f.model->expanded(originalFolder));

    f.disclosure(originalFolder);
    f.click(originalFolder); f.click(f.members[0], Qt::ControlModifier); f.click(f.members[2], Qt::ControlModifier);
    const auto ancestorSelection = f.session().layerSelectionState();
    CHECK(f.menuCommand(std::nullopt, "AddToNewLayerFolderAction"));
    const auto outer = f.session().activeLayer().value_or(0);
    CHECK((f.document().tree().container(outer)->children == std::vector<core::LayerId> { originalFolder, f.members[2] }));
    CHECK((f.document().tree().container(originalFolder)->children == std::vector<core::LayerId> { f.members[0] }));
    CHECK((f.document().tree().roots == std::vector<core::LayerId> { f.members[1], outer, f.spare }));
    CHECK(f.document().tree().placement(f.members[0])->parent == originalFolder);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == before && f.session().layerSelectionState() == ancestorSelection);
}

void folderAncestorSelectionIndicatorsAreUiOnly()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto outer = f.session().activeLayer().value_or(0);
    CHECK(f.menuCommand(outer, "ContextNewLayerFolderAction"));
    const auto inner = f.session().activeLayer().value_or(0);
    CHECK(f.menuCommand(std::nullopt, "ContextNewLayerFolderAction"));
    const auto other = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> nested(f.model->mimeData({f.row(f.members[0]),f.row(f.members[1])}));
    CHECK(f.model->dropMimeData(nested.get(),Qt::MoveAction,-1,0,f.row(inner)));
    std::unique_ptr<QMimeData> branch(f.model->mimeData({f.row(f.members[2])}));
    CHECK(f.model->dropMimeData(branch.get(),Qt::MoveAction,-1,0,f.row(other)));
    f.click(f.members[0]); f.click(f.members[1],Qt::ControlModifier);
    CHECK(f.trigger("GroupLayersAction"));
    const auto group = f.session().activeLayer().value_or(0);
    f.click(f.spare);
    // Distinct user colors catch accidental use of the general accent token.
    const auto entryTheme = ui::currentThemeSettings();
    auto theme = entryTheme;
    theme.preset = ui::ThemePreset::Custom;
    theme.custom = ui::resolvedThemeColors(entryTheme);
    const QColor ink("#24C8B1");
    theme.custom[std::size_t(ui::ThemeColor::LayerSelection)] = ink;
    theme.custom[std::size_t(ui::ThemeColor::Accent)] = QColor("#C83072");
    ui::applyEditorTheme(*qApp,theme); settle();
    CHECK(f.model->setData(f.row(f.spare),QStringLiteral("Redo fixture"),Qt::EditRole));
    CHECK(f.shortcut(QKeySequence::Undo));
    f.document().markSaved();
    const auto tree = f.document().tree();
    const auto content = f.document().contentState();
    const auto revision = f.document().revision();
    const auto undo = f.session().history().undoDepth(), redo = f.session().history().redoDepth();
    const auto marked = [&](core::LayerId id) {
        return f.row(id).data(ui::LayerListModel::SelectedDescendantRole).toBool();
    };
    const auto paint = [&](core::LayerId id, bool selected = false) {
        QImage image(280,52,QImage::Format_ARGB32_Premultiplied);
        image.fill(ui::themeColor(ui::ThemeColor::Surface));
        QStyleOptionViewItem option;
        option.initFrom(f.view); option.rect = image.rect(); option.widget = f.view;
        option.decorationSize = f.view->iconSize();
        option.state = QStyle::State_Enabled | (selected ? QStyle::State_Selected : QStyle::State_None);
        QPainter painter(&image);
        f.view->itemDelegate()->paint(&painter,option,f.row(id));
        return image;
    };
    const auto normalOuter = paint(outer), normalInner = paint(inner);
    CHECK(!marked(outer) && !marked(inner) && !marked(other));
    f.click(group);
    CHECK(marked(outer) && marked(inner) && !marked(other) && !marked(group));
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId>{group});
    CHECK(!f.view->selectionModel()->isSelected(f.row(outer)));
    const auto ancestorOuter = paint(outer), ancestorInner = paint(inner);
    CHECK(ancestorOuter.pixelColor(0,26) == ink && ancestorInner.pixelColor(0,26) == ink);
    CHECK(ancestorOuter.copy(3,0,277,52) == normalOuter.copy(3,0,277,52));
    CHECK(ancestorInner.copy(3,0,277,52) == normalInner.copy(3,0,277,52));
    // Selecting the folder itself still uses the established selected-row tint;
    // its unselected ancestor keeps only the new strip.
    f.click(inner);
    CHECK(marked(outer) && !marked(inner));
    const auto selectedInner = paint(inner,true);
    CHECK(selectedInner.pixelColor(0,26) == ink);
    CHECK(selectedInner.pixelColor(250,26) != normalInner.pixelColor(250,26));
    f.click(group,Qt::ControlModifier);
    CHECK(marked(inner) && f.view->selectionModel()->isSelected(f.row(inner)));
    CHECK(paint(inner,true).copy(3,0,277,52) == selectedInner.copy(3,0,277,52));
    f.click(f.spare);
    CHECK(!marked(outer) && !marked(inner) && !marked(other));
    CHECK(paint(outer) == normalOuter);
    f.click(group); f.click(f.members[2],Qt::ControlModifier);
    CHECK(marked(outer) && marked(inner) && marked(other));
    const auto selection = f.session().layerSelectionState();
    f.disclosure(outer);
    CHECK(f.model->rowForLayer(inner) < 0 && marked(outer) && marked(other));
    CHECK(f.session().layerSelectionState() == selection);
    f.model->refresh();
    CHECK(marked(outer) && marked(other));
    f.disclosure(outer);
    CHECK(marked(inner) && f.session().layerSelectionState() == selection);
    // A hidden member inside a group still reaches every strict folder
    // ancestor; refreshing the cache must not normalize/replace the session.
    f.session().setActiveLayer(f.members[0]);
    const auto hiddenSelection = f.session().layerSelectionState();
    f.model->refreshSelectionIndicators();
    CHECK(marked(outer) && marked(inner) && !marked(other) && !marked(group));
    CHECK(f.session().layerSelectionState() == hiddenSelection);
    CHECK(f.document().tree() == tree && f.document().contentState() == content);
    CHECK(f.document().revision() == revision && !f.document().isModified());
    CHECK(f.session().history().undoDepth() == undo && f.session().history().redoDepth() == redo);
    ui::applyEditorTheme(*qApp,entryTheme);
}

void layerAndFolderIconsUseIndependentThumbnailDefaultsAndColorLabels()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    const auto entryTheme = ui::currentThemeSettings();
    ui::ThemeSettings theme;
    theme.preset = ui::ThemePreset::Custom;
    const auto setThemeColor = [&](ui::ThemeColor role, QColor color) {
        theme.custom.at(static_cast<std::size_t>(role)) = color;
    };
    const QColor defaultInk("#C36EB1");
    setThemeColor(ui::ThemeColor::Thumbnail, defaultInk);
    setThemeColor(ui::ThemeColor::Accent, QColor("#35B7CA"));
    ui::applyEditorTheme(*qApp, theme);
    f.model->refresh(); settle();
    const auto iconFor = [&](core::LayerId id) { return qvariant_cast<QIcon>(f.row(id).data(Qt::DecorationRole)); };
    const auto hasTint = [](const QImage& image, QColor ink) {
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (pixel.alpha() >= 200 && std::abs(pixel.red() - ink.red()) <= 2
                && std::abs(pixel.green() - ink.green()) <= 2
                && std::abs(pixel.blue() - ink.blue()) <= 2) return true;
        }
        return false;
    };
    const auto displaysInk = [&](core::LayerId id, QColor ink, QIcon::Mode mode = QIcon::Normal) {
        const auto image = iconFor(id).pixmap(QSize(40, 40), 1.0, mode).toImage();
        const auto* container = f.document().tree().container(id);
        if (!container || container->kind != core::ContainerKind::Group) return hasTint(image, ink);
        // A 15px badge has subpixel strokes, blended into an opaque tile;
        // no pixel need equal the unblended ink. Compare the complete badge
        // against the standalone vector glyph on the same background.
        QPixmap badge(15, 15);
        badge.fill(QApplication::palette().color(QPalette::AlternateBase));
        QPainter painter(&badge);
        ui::toolGlyph(ui::ToolGlyph::Group, ink).paint(&painter, QRect(0, 0, 15, 15));
        painter.end();
        return image.copy(QRect(25, 25, 15, 15)).convertToFormat(QImage::Format_ARGB32)
            == badge.toImage().convertToFormat(QImage::Format_ARGB32);
    };
    const auto checkLabel = [&](core::LayerId id) {
        // Content thumbnails now include the active editing-target frame;
        // compare label changes with that independent UI state held constant.
        f.click(id);
        const auto original = iconFor(id).pixmap(40, 40).toImage();
        const bool container=f.document().tree().container(id)!=nullptr;
        CHECK(!original.isNull());
        if(container) {
            CHECK(displaysInk(id, defaultInk));
            CHECK(displaysInk(id, defaultInk, QIcon::Selected));
        } else {
            // Source thumbnails have no default-ink badge. Labels belong to
            // the row-edge stripe, leaving the preview pixels unchanged.
            CHECK(!displaysInk(id, defaultInk));
        }
        CHECK(f.menuCommand(id, "Red", true));
        CHECK(f.row(id).data(Qt::UserRole + 5).toInt() == int(core::ColorLabel::Red));
        const auto labelled = iconFor(id);
        const auto redInk = ui::layerLabelColor(core::ColorLabel::Red);
        if(container) {
            CHECK(displaysInk(id, redInk));
            CHECK(displaysInk(id, redInk, QIcon::Selected));
        } else CHECK(labelled.pixmap(40,40).toImage()==original);
        CHECK(labelled.cacheKey() == iconFor(id).cacheKey());
        CHECK(f.menuCommand(id, "None", true));
        CHECK(f.row(id).data(Qt::UserRole + 5).toInt() == int(core::ColorLabel::None));
        CHECK(iconFor(id).pixmap(40, 40).toImage() == original);
        CHECK(f.shortcut(QKeySequence::Undo));
        if(container)CHECK(displaysInk(id, redInk));
        else CHECK(iconFor(id).pixmap(40,40).toImage()==original);
        CHECK(f.shortcut(QKeySequence::Undo));
        CHECK(iconFor(id).pixmap(40, 40).toImage() == original);
    };
    for (auto id : {f.members[0], f.members[1], f.members[2], folder}) checkLabel(id);
    const auto group = f.group();
    checkLabel(group);
    const auto groupIcon = iconFor(group);
    for (const auto logicalSize : {32, 34, 40}) {
        for (const auto dpr : {1.0, 1.25, 1.5, 2.0, 3.0}) {
            const auto normal = groupIcon.pixmap(QSize(logicalSize, logicalSize), dpr, QIcon::Normal);
            const auto selected = groupIcon.pixmap(QSize(logicalSize, logicalSize), dpr, QIcon::Selected);
            const QSize physicalSize(qRound(logicalSize * dpr), qRound(logicalSize * dpr));
            CHECK(normal.size() == physicalSize && selected.size() == physicalSize);
            CHECK(std::abs(normal.devicePixelRatioF() - dpr) < 1e-8);
            CHECK(std::abs(selected.devicePixelRatioF() - dpr) < 1e-8);
            CHECK(normal.toImage() == selected.toImage());
            if (logicalSize != 34) {
                QPixmap painted(physicalSize);
                painted.setDevicePixelRatio(dpr);
                painted.fill(Qt::transparent);
                QPainter painter(&painted);
                groupIcon.paint(&painter, QRect(0, 0, logicalSize, logicalSize), Qt::AlignCenter, QIcon::Selected);
                painter.end();
                CHECK(painted.toImage() == selected.toImage());
            }
        }
    }

    // Refresh after a thumbnail-only preview: its palette can be unchanged,
    // so cached icons cannot rely solely on QPalette::cacheKey().
    const auto folderBefore = iconFor(folder).pixmap(40, 40).toImage();
    const auto groupBefore = iconFor(group).pixmap(40, 40).toImage();
    setThemeColor(ui::ThemeColor::Accent, QColor("#ED9748"));
    ui::applyEditorTheme(*qApp, theme);
    f.model->refresh(); settle();
    CHECK(iconFor(folder).pixmap(40, 40).toImage() == folderBefore);
    CHECK(iconFor(group).pixmap(40, 40).toImage() == groupBefore);
    setThemeColor(ui::ThemeColor::Thumbnail, QColor("#81B474"));
    ui::applyEditorTheme(*qApp, theme);
    f.model->refresh(); settle();
    CHECK(hasTint(iconFor(folder).pixmap(40, 40).toImage(), QColor("#81B474")));
    CHECK(displaysInk(group, QColor("#81B474")));
    CHECK(displaysInk(group, QColor("#81B474"), QIcon::Selected));
    CHECK(ui::themeColor(ui::ThemeColor::Accent) == QColor("#ED9748"));
    ui::applyEditorTheme(*qApp, entryTheme);
}

void folderCollapsePersistsAsOptionalProjectUiState()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto group = f.group();
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto outer = f.session().activeLayer().value_or(0);
    CHECK(f.menuCommand(outer, "ContextNewLayerFolderAction"));
    const auto inner = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> contents(f.model->mimeData({ f.row(group) }));
    CHECK(f.model->dropMimeData(contents.get(), Qt::MoveAction, -1, 0, f.row(inner)));
    settle();
    CHECK(f.document().tree().placement(inner)->parent == outer);
    CHECK(f.document().tree().placement(group)->parent == inner);
    f.click(group);

    // Keep a real redo branch and a hidden primary target throughout UI-only
    // expansion changes. Neither document content nor history owns this state.
    CHECK(f.model->setData(f.row(inner), QStringLiteral("Temporary name"), Qt::EditRole));
    CHECK(f.shortcut(QKeySequence::Undo));
    f.document().markSaved();
    const auto content = f.document().contentState();
    const auto revision = f.document().revision();
    const auto selection = f.session().layerSelectionState();
    const auto undo = f.session().history().undoDepth();
    const auto redo = f.session().history().redoDepth();
    const auto memory = f.session().history().memoryUsed();
    CHECK(redo > 0);
    const auto unchanged = [&] {
        CHECK(f.document().contentState() == content && f.document().revision() == revision);
        CHECK(!f.document().isModified());
        CHECK(f.session().layerSelectionState() == selection);
        CHECK(f.session().history().undoDepth() == undo && f.session().history().redoDepth() == redo);
        CHECK(f.session().history().memoryUsed() == memory);
    };
    f.disclosure(inner);
    f.disclosure(outer);
    CHECK(!f.model->expanded(inner) && !f.model->expanded(outer));
    CHECK(f.model->rowForLayer(inner) == -1 && f.model->rowForLayer(group) == -1);
    unchanged();
    const std::vector<core::LayerId> closed { outer, inner };
    CHECK(f.model->collapsedFolderIds() == closed);
    const std::array candidates { outer, inner, core::LayerId { 0 }, f.spare, group, core::makeLayerId(), inner };
    f.model->restoreCollapsedFolderIds(candidates);
    CHECK(f.model->collapsedFolderIds() == closed);
    unchanged();
    f.model->restoreCollapsedFolderIds(closed); // Reapplying the same state is harmless.
    unchanged();

    const auto project = f.assets.filePath(QStringLiteral("collapsed.vulkana"));
    QStringList errors;
    ui::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath = [&] { return project; };
    hooks.confirmReplace = [](const QString&) { return true; };
    hooks.reportError = [&](const QString& error) { errors.append(error); };
    f.window.setFileInteractions(std::move(hooks));
    CHECK(f.window.saveDocument(true));
    unchanged();
    const auto saved = ui::loadProject(project);
    CHECK(saved); if (!saved) return;
    const auto storedIds = saved.metadata["ui"].toObject()["layers"].toObject()["collapsedFolders"].toArray();
    CHECK(storedIds.size() == 2);
    CHECK(storedIds.contains(QString::number(qulonglong(outer))));
    CHECK(storedIds.contains(QString::number(qulonglong(inner))));
    for (const auto& value : storedIds) CHECK(value.isString());

    // A project with the exact same stable IDs but no optional UI metadata is
    // a legacy/default-expanded document, not a continuation of the last UI.
    const auto legacy = f.assets.filePath(QStringLiteral("without-ui-state.vulkana"));
    CHECK(ui::saveProject(legacy, *saved.document));
    CHECK(f.window.openImageFromPath(project));
    CHECK(f.model->collapsedFolderIds() == closed);
    CHECK(f.model->rowForLayer(inner) == -1 && f.model->rowForLayer(group) == -1);
    CHECK(f.session().history().undoDepth() == undo && !f.document().isModified()); // Focusing an open project retains its history.
    f.disclosure(outer);
    CHECK(!f.model->expanded(inner) && f.model->rowForLayer(group) == -1);
    f.disclosure(inner);
    CHECK(f.model->rowForLayer(group) >= 0);
    CHECK(f.window.saveDocument());
    const auto expanded = ui::loadProject(project);
    CHECK(expanded);
    CHECK(expanded.metadata["ui"].toObject()["layers"].toObject()["collapsedFolders"].toArray().isEmpty());
    CHECK(f.window.openImageFromPath(project));
    CHECK(f.model->expanded(outer) && f.model->expanded(inner));

    f.disclosure(inner);
    f.disclosure(outer);
    CHECK(f.window.openImageFromPath(legacy));
    CHECK(f.model->collapsedFolderIds().empty());
    CHECK(f.model->expanded(outer) && f.model->expanded(inner) && f.model->rowForLayer(group) >= 0);

    // Optional malformed or stale UI entries cannot make a valid document
    // fail to open, collapse a group/layer, or inherit a previous folder state.
    const auto stale = core::makeLayerId();
    QJsonArray mixed { QString::number(qulonglong(inner)), QString::number(qulonglong(inner)),
        QString::number(qulonglong(stale)), QString::number(qulonglong(group)),
        QString::number(qulonglong(f.spare)), QStringLiteral("0"), QStringLiteral("-1"),
        QStringLiteral("not-an-id"), QStringLiteral("18446744073709551616"), 42, true, QJsonObject {} };
    QJsonObject metadata { { "ui", QJsonObject { { "layers", QJsonObject { { "collapsedFolders", mixed } } } } } };
    const auto malformed = f.assets.filePath(QStringLiteral("mixed-ui-state.vulkana"));
    CHECK(ui::saveProject(malformed, *saved.document, metadata));
    CHECK(f.window.openImageFromPath(malformed));
    CHECK(f.model->collapsedFolderIds() == std::vector<core::LayerId> { inner });
    CHECK(f.model->expanded(outer) && !f.model->expanded(inner));
    CHECK(f.model->rowForLayer(inner) >= 0 && f.model->rowForLayer(group) == -1);
    CHECK(f.session().history().undoDepth() == 0 && !f.document().isModified());
    CHECK(f.window.openImageFromPath(f.assets.filePath(QStringLiteral("layer.png"))));
    CHECK(f.model->collapsedFolderIds().empty());
    CHECK(errors.isEmpty());
}

void visibilityShortcutsUseAncestorGatesAndOneUndoStep()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto outer = f.session().activeLayer().value_or(0);
    CHECK(f.menuCommand(outer, "ContextNewLayerFolderAction"));
    const auto inner = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> children(f.model->mimeData({ f.row(f.members[0]), f.row(f.members[1]) }));
    CHECK(f.model->dropMimeData(children.get(), Qt::MoveAction, -1, 0, f.row(inner)));
    settle();
    f.click(outer); f.click(f.members[1], Qt::ControlModifier);
    const auto selection = f.session().layerSelectionState();
    const auto baselineTree = f.document().tree();
    const auto undoDepth = f.session().history().undoDepth();
    const auto surface = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
    const auto rasterRevision = surface->revision();
    const auto own = [&](core::LayerId id) { return f.row(id).data(Qt::CheckStateRole).toInt() == Qt::Checked; };
    const auto effective = [&](core::LayerId id) {
        const auto value = f.row(id).data(Qt::UserRole + 6);
        CHECK(value.isValid());
        return value.toBool();
    };
    for (const auto* name : { "HideSelectedLayersAction", "ShowSelectedLayersAction",
             "IsolateSelectedLayersAction", "ShowAllLayersAction" })
        CHECK(f.action(name));
    CHECK(own(outer) && own(inner) && !own(f.members[0]) && own(f.members[1]));
    CHECK(effective(outer) && effective(inner) && !effective(f.members[0]) && effective(f.members[1]));

    f.visibilityKey(); // H: ancestor wins; this must not mutate its selected child.
    CHECK(!f.document().tree().container(outer)->visible);
    CHECK(f.document().tree().container(inner)->visible);
    CHECK(!own(outer) && own(inner) && !own(f.members[0]) && own(f.members[1]));
    CHECK(!effective(outer) && !effective(inner) && !effective(f.members[0]) && !effective(f.members[1]));
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.session().history().undoDepth() == undoDepth + 1);
    f.visibilityKey(); // Hide is not a toggle, and an unchanged batch is not history.
    CHECK(!f.document().tree().container(outer)->visible);
    CHECK(f.session().history().undoDepth() == undoDepth + 1);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == baselineTree && f.session().layerSelectionState() == selection);
    const auto redoDepth = f.session().history().redoDepth();
    CHECK(redoDepth > 0);
    f.visibilityKey(Qt::AltModifier); // Ancestor already visible: preserve the redo branch.
    CHECK(f.document().tree() == baselineTree);
    CHECK(f.session().history().redoDepth() == redoDepth);
    CHECK(f.session().history().undoDepth() == undoDepth);
    f.visibilityKey();
    CHECK(f.session().history().redoDepth() == 0);
    f.visibilityKey(Qt::AltModifier);
    CHECK(own(outer) && own(inner) && !own(f.members[0]) && own(f.members[1]));
    CHECK(f.session().history().undoDepth() == undoDepth + 2);
    CHECK(f.session().layerSelectionState() == selection);

    // Every row exposes its own eye bit. Under a closed parent gate, clicking
    // child eyes remains useful without making inherited-hidden content appear.
    f.eye(outer);
    CHECK(!own(outer));
    f.eye(f.members[1]);
    CHECK(!own(f.members[1]) && !effective(f.members[1]));
    f.eye(f.members[1]);
    CHECK(own(f.members[1]) && !effective(f.members[1]));
    CHECK(f.session().layerSelectionState() == selection);
    f.eye(outer);
    CHECK(own(outer) && effective(f.members[1]) && !effective(f.members[0]));
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(surface->revision() == rasterRevision);

    // Hidden displayed rows are still selected session targets. Closing a
    // folder neither loses those targets nor changes ancestor normalization.
    f.disclosure(outer);
    CHECK(!f.row(f.members[1]).isValid());
    f.visibilityKey();
    CHECK(!f.document().tree().container(outer)->visible && f.document().layer(f.members[1])->visible);
    CHECK(f.session().layerSelectionState() == selection);
    f.visibilityKey(Qt::ShiftModifier | Qt::AltModifier);
    for (const auto& item : f.document().tree().containers) CHECK(item.visible);
    for (const auto& layer : f.document().layers()) CHECK(layer.visible);
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(!f.model->expanded(outer));

    // Isolation respects the same selected-ancestor authority: it reveals that
    // gate, but does not overwrite an independently hidden descendant's eye.
    f.disclosure(outer);
    f.eye(f.members[0]);
    CHECK(!f.document().layer(f.members[0])->visible);
    f.click(outer); f.click(f.members[0], Qt::ControlModifier);
    const auto isolateSelection = f.session().layerSelectionState();
    const auto isolateDepth = f.session().history().undoDepth();
    f.visibilityKey(Qt::ShiftModifier);
    CHECK(f.document().tree().container(outer)->visible && f.document().tree().container(inner)->visible);
    CHECK(!f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
    CHECK(!f.document().layer(f.spare)->visible && !f.document().layer(f.members[2])->visible);
    CHECK(f.session().history().undoDepth() == isolateDepth + 1);
    CHECK(f.session().layerSelectionState() == isolateSelection);
    f.visibilityKey();
    CHECK(!f.document().tree().container(outer)->visible);
    f.click(f.members[1]);
    f.visibilityKey(Qt::AltModifier); // Show selected is not a parent-gate override.
    CHECK(!f.document().tree().container(outer)->visible && f.document().layer(f.members[1])->visible);
    f.visibilityKey(Qt::ShiftModifier); // Isolate deliberately exposes the path to the selected leaf.
    CHECK(f.document().tree().container(outer)->visible && f.document().tree().container(inner)->visible);
    CHECK(effective(f.members[1]) && !effective(f.members[0]));
    CHECK(!f.document().layer(f.spare)->visible && !f.document().layer(f.members[2])->visible);
}

void isolateAndShowAllShortcutsAreAtomicAndDoNotToggle()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.members[0]); f.click(f.spare, Qt::ControlModifier);
    const auto selection = f.session().layerSelectionState();
    std::array<bool, 3> original;
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = f.document().layer(f.members[i])->visible;
    const auto undo = f.session().history().undoDepth();
    f.visibilityKey(Qt::ShiftModifier);
    CHECK(f.document().layer(f.members[0])->visible && f.document().layer(f.spare)->visible);
    CHECK(!f.document().layer(f.members[1])->visible && !f.document().layer(f.members[2])->visible);
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.session().history().undoDepth() == undo + 1);
    f.visibilityKey(Qt::ShiftModifier);
    CHECK(f.session().history().undoDepth() == undo + 1);
    CHECK(f.shortcut(QKeySequence::Undo));
    for (std::size_t i = 0; i < original.size(); ++i) CHECK(f.document().layer(f.members[i])->visible == original[i]);
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.document().layer(f.members[0])->visible && !f.document().layer(f.members[1])->visible);
    f.visibilityKey(Qt::ShiftModifier | Qt::AltModifier);
    for (const auto& layer : f.document().layers()) CHECK(layer.visible);
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.session().history().undoDepth() == undo + 2);
    f.visibilityKey(Qt::ShiftModifier | Qt::AltModifier);
    CHECK(f.session().history().undoDepth() == undo + 2);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().layer(f.members[0])->visible && !f.document().layer(f.members[1])->visible);
    CHECK(f.shortcut(QKeySequence::Redo));
    for (const auto& layer : f.document().layers()) CHECK(layer.visible);
}

void visibilityShortcutsRespectEditableFieldOwnership()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.spare);
    const auto selection = f.session().layerSelectionState();
    const auto history = f.session().history().undoDepth();
    const auto content = f.document().contentState();
    const auto originalName = f.document().layer(f.spare)->name;
    QTest::keyClick(f.canvas, Qt::Key_F2);
    settle();
    auto* editor = f.view->renameEditor();
    CHECK(editor);
    if (!editor) return;
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(editor, Qt::Key_H);
    CHECK(editor->text() == QStringLiteral("h"));
    QTest::keyClick(editor, 'H', Qt::ShiftModifier);
    CHECK(editor->text() == QStringLiteral("hH"));
    QTest::keyClick(editor, Qt::Key_H, Qt::AltModifier);
    QTest::keyClick(editor, Qt::Key_H, Qt::AltModifier | Qt::ShiftModifier);
    CHECK(f.document().layer(f.spare)->visible);
    CHECK(!f.document().layer(f.members[0])->visible);
    CHECK(f.document().layer(f.members[1])->visible && f.document().layer(f.members[2])->visible);
    CHECK(f.session().history().undoDepth() == history);
    QTest::keyClick(editor, Qt::Key_Escape);
    CHECK(f.document().layer(f.spare)->name == originalName);
    CHECK(f.document().contentState() == content);
    CHECK(f.session().history().undoDepth() == history && f.session().layerSelectionState() == selection);
}

void dissolvingHiddenContainersPreservesAppearanceAndExactDescendantBits()
{
    for (const bool asGroup : { false, true }) {
        Fixture f;
        CHECK(f.valid()); if (!f.valid()) return;
        CHECK(f.trigger("NewLayerFolderAction"));
        const auto outer = f.session().activeLayer().value_or(0);
        CHECK(f.menuCommand(outer, "ContextNewLayerFolderAction"));
        const auto inner = f.session().activeLayer().value_or(0);
        std::unique_ptr<QMimeData> immediate(f.model->mimeData({ f.row(f.members[0]) }));
        CHECK(f.model->dropMimeData(immediate.get(), Qt::MoveAction, -1, 0, f.row(outer)));
        std::unique_ptr<QMimeData> descendants(f.model->mimeData({ f.row(f.members[1]), f.row(f.members[2]) }));
        CHECK(f.model->dropMimeData(descendants.get(), Qt::MoveAction, -1, 0, f.row(inner)));
        settle();
        CHECK(f.model->setData(f.row(f.members[0]), Qt::Checked, Qt::CheckStateRole));
        CHECK(f.model->setData(f.row(f.members[2]), Qt::Unchecked, Qt::CheckStateRole));
        core::LayerId target = outer;
        if (asGroup) {
            f.click(f.members[0]); f.click(inner, Qt::ControlModifier);
            CHECK(f.trigger("GroupLayersAction"));
            target = f.session().activeLayer().value_or(0);
        } else f.click(outer);
        f.visibilityKey();
        CHECK(!f.document().tree().container(target)->visible);
        CHECK(f.document().tree().container(inner)->visible);
        CHECK(f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
        CHECK(!f.document().layer(f.members[2])->visible);
        const auto beforeTree = f.document().tree();
        const auto beforeSelection = f.session().layerSelectionState();
        const auto depth = f.session().history().undoDepth();
        const auto raster = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
        const auto rasterRevision = raster->revision();
        std::array<core::AffineTransform, 3> transforms;
        for (std::size_t i = 0; i < transforms.size(); ++i)
            transforms[i] = f.document().layer(f.members[i])->localToDocument;
        if (asGroup) CHECK(f.trigger("UngroupLayersAction"));
        else CHECK(f.menuCommand(target, "Remove folder, keep contents", true));
        CHECK(!f.document().containsItem(target));
        CHECK(!f.document().tree().container(inner)->visible);
        CHECK(!f.document().layer(f.members[0])->visible); // Former immediate leaf inherits the removed gate.
        CHECK(f.document().layer(f.members[1])->visible); // Grandchild own bits remain independent.
        CHECK(!f.document().layer(f.members[2])->visible);
        for (const auto id : f.members) CHECK(!f.row(id).data(Qt::UserRole + 6).toBool());
        CHECK(f.document().layer(f.spare)->visible);
        CHECK(f.session().history().undoDepth() == depth + 1);
        const auto afterTree = f.document().tree();
        const auto afterSelection = f.session().layerSelectionState();
        CHECK(f.shortcut(QKeySequence::Undo));
        CHECK(f.document().tree() == beforeTree && f.session().layerSelectionState() == beforeSelection);
        CHECK(f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
        CHECK(!f.document().layer(f.members[2])->visible);
        CHECK(f.shortcut(QKeySequence::Redo));
        CHECK(f.document().tree() == afterTree && f.session().layerSelectionState() == afterSelection);
        CHECK(!f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
        CHECK(!f.document().layer(f.members[2])->visible);
        CHECK(std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface == raster);
        CHECK(raster->revision() == rasterRevision);
        for (std::size_t i = 0; i < transforms.size(); ++i)
            CHECK(f.document().layer(f.members[i])->localToDocument == transforms[i]);
    }
}

void foldersDisclosureDisplayedRangesAndDnD()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    CHECK(f.document().tree().container(folder));
    if (!f.document().tree().container(folder)) return;
    CHECK(f.action("LayerTransformAction") && !f.action("LayerTransformAction")->isEnabled());
    CHECK(f.menuCommand(folder, "ContextNewLayerFolderAction"));
    const auto nested = f.session().activeLayer().value_or(0);
    CHECK(f.document().tree().placement(nested)->parent == folder);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(!f.document().containsItem(nested));
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.document().tree().placement(nested)->parent == folder);
    std::unique_ptr<QMimeData> cycle(f.model->mimeData({ f.row(folder) }));
    CHECK(!f.model->canDropMimeData(cycle.get(), Qt::MoveAction, -1, 0, f.row(nested)));

    f.click(f.members[0]); f.click(f.members[1], Qt::ControlModifier);
    const auto selectedBefore = f.session().selectedLayers();
    std::unique_ptr<QMimeData> data(f.model->mimeData({ f.row(f.members[0]), f.row(f.members[1]) }));
    CHECK(f.model->dropMimeData(data.get(), Qt::MoveAction, -1, 0, f.row(folder)));
    settle();
    CHECK(f.document().tree().placement(f.members[0])->parent == folder);
    CHECK(f.document().tree().placement(f.members[1])->parent == folder);
    CHECK(f.session().selectedLayers() == selectedBefore);
    const auto history = f.session().history().undoDepth();
    const auto content = f.document().contentState();
    f.disclosure(folder);
    CHECK(!f.model->expanded(folder));
    CHECK(f.model->rowForLayer(f.members[0]) == -1 && f.model->rowForLayer(f.members[1]) == -1);
    CHECK(f.session().selectedLayers() == selectedBefore);
    CHECK(f.session().history().undoDepth() == history && f.document().contentState() == content);
    // The collapsed descendants cannot leak into a range of displayed rows.
    f.click(folder); f.click(f.members[2], Qt::ShiftModifier);
    CHECK(!f.session().isLayerSelected(f.members[0]) && !f.session().isLayerSelected(f.members[1]));
    CHECK(f.session().isLayerSelected(folder) && f.session().isLayerSelected(f.members[2]));
    const auto selection = f.session().selectedLayers();
    f.disclosure(folder);
    CHECK(f.model->expanded(folder) && f.model->rowForLayer(f.members[0]) >= 0);
    CHECK(f.session().selectedLayers() == selection);

    const auto original = f.document().tree();
    std::unique_ptr<QMimeData> leaf(f.model->mimeData({ f.row(f.members[0]) }));
    const auto sparePlace = *original.placement(f.spare);
    CHECK(f.model->dropMimeData(leaf.get(), Qt::MoveAction, f.row(f.spare).row(), 0, {}));
    settle();
    CHECK((f.document().tree().placement(f.members[0]) == core::ItemPlacement { sparePlace.parent, sparePlace.index + 1 }));
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == original);
    CHECK(f.model->dropMimeData(leaf.get(), Qt::MoveAction, -2, 0, f.row(f.spare)));
    settle();
    CHECK(f.document().tree().placement(f.members[0]) == sparePlace);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == original);
}

void groupingAndUngroupingUseOneRowAndExactHistory()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto original = f.document().tree();
    const auto group = f.group();
    if (!group) return;
    CHECK(f.model->rowCount() == 2);
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { group });
    CHECK(f.row(group).data(Qt::UserRole + 1).toBool());
    CHECK(!qvariant_cast<QIcon>(f.row(group).data(Qt::DecorationRole)).isNull());
    CHECK(f.row(group).data(Qt::CheckStateRole).toInt() == Qt::Checked);
    const auto grouped = f.document().tree();
    CHECK(f.trigger("UngroupLayersAction"));
    CHECK(f.document().tree() == original && f.model->rowCount() == 4);
    CHECK(f.session().selectedLayers().size() == 3);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == grouped && f.session().activeLayer() == group);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == original && f.session().selectedLayers().size() == 3);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.document().tree() == grouped && f.session().activeLayer() == group);
}

void collapsedSelectedRowsSurviveDragPressButNotPlainClick()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> leaf(f.model->mimeData({ f.row(f.members[0]) }));
    CHECK(f.model->dropMimeData(leaf.get(), Qt::MoveAction, -1, 0, f.row(folder)));
    settle();
    f.click(f.members[0]);
    f.click(f.spare, Qt::ControlModifier);
    const auto selected = f.session().layerSelectionState();
    CHECK(selected.ids.size() == 2);
    f.disclosure(folder);
    CHECK(f.model->rowForLayer(f.members[0]) == -1);
    CHECK(f.session().layerSelectionState() == selected);
    CHECK(f.view->selectionModel()->selectedIndexes().size() == 1);
    const auto depth = f.session().history().undoDepth();
    const auto content = f.document().contentState();
    f.view->scrollTo(f.row(f.spare)); settle();
    const auto position = f.view->visualRect(f.row(f.spare)).center();
    // Exercise the real press routing. The gesture may become a drag, so its
    // one visible row must not collapse the hidden session-selected target.
    QTest::mousePress(f.view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    settle();
    CHECK(f.session().layerSelectionState() == selected);
    std::unique_ptr<QMimeData> drag(f.model->mimeData(f.view->selectionModel()->selectedIndexes()));
    const auto ids = drag->data(f.model->mimeTypes().front()).split(',');
    CHECK(ids.size() == 2);
    CHECK(ids.contains(QByteArray::number(qulonglong(f.members[0]))));
    CHECK(ids.contains(QByteArray::number(qulonglong(f.spare))));
    // No drag was started: a plain press/release still selects exactly one.
    QTest::mouseRelease(f.view->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    settle();
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { f.spare });
    CHECK(f.session().history().undoDepth() == depth && f.document().contentState() == content);
}

void dissolvingSelectedFolderAndChildRetainsAUniqueSelection()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    std::unique_ptr<QMimeData> leaves(f.model->mimeData({ f.row(f.members[0]), f.row(f.members[1]) }));
    CHECK(f.model->dropMimeData(leaves.get(), Qt::MoveAction, -1, 0, f.row(folder)));
    settle();
    f.click(f.members[0]);
    f.click(folder, Qt::ShiftModifier);
    CHECK(f.session().isLayerSelected(folder));
    CHECK(f.session().isLayerSelected(f.members[0]) && f.session().isLayerSelected(f.members[1]));
    CHECK(f.session().selectedLayers().size() == 3);
    const auto before = f.session().layerSelectionState();
    const auto tree = f.document().tree();
    const auto depth = f.session().history().undoDepth();
    // Keeping contents is explicit; Shift+Delete removes selected contents.
    CHECK(f.menuCommand(folder,"Remove folder, keep contents",true));
    CHECK(!f.document().containsItem(folder));
    CHECK(f.session().selectedLayers().size() == 2);
    CHECK(f.session().isLayerSelected(f.members[0]) && f.session().isLayerSelected(f.members[1]));
    CHECK(f.session().history().undoDepth() == depth + 1);
    const auto after = f.session().layerSelectionState();
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == tree && f.session().layerSelectionState() == before);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.session().layerSelectionState() == after && f.session().selectedLayers().size() == 2);
}

void groupCanvasMoveTransformAndPixelToolRejection()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto group = f.group();
    if (!group) return;
    CHECK(f.shortcut(QKeySequence(Qt::Key_V)));
    CHECK(f.action("LayerTransformAction") && f.action("LayerTransformAction")->isEnabled());
    std::array<core::AffineTransform, 3> before;
    for (std::size_t i = 0; i < before.size(); ++i) before[i] = f.document().layer(f.members[i])->localToDocument;
    const auto raster = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
    const auto revision = raster->revision();
    const auto depth = f.session().history().undoDepth();
    f.drag({ 50, 50 }, { 60, 56 });
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { group });
    CHECK(f.session().history().undoDepth() == depth + 1);
    for (std::size_t i = 0; i < before.size(); ++i) {
        auto expected = before[i]; expected.m02 += 10; expected.m12 += 6;
        CHECK(nearMatrix(f.document().layer(f.members[i])->localToDocument, expected));
    }
    CHECK(f.shortcut(QKeySequence::Undo));
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(f.document().layer(f.members[i])->localToDocument == before[i]);
    CHECK(f.trigger("LayerTransformAction"));
    CHECK(f.canvas->scene().transformOverlay.has_value());
    CHECK(f.session().activeTool() == core::ToolId::Transform);
    f.drag({ 50, 50 }, { 55, 53 });
    // Canvas right-click routes to the same Apply path and keeps the group row.
    f.mouse(QEvent::MouseButtonPress, { 100, 100 }, Qt::RightButton, Qt::RightButton);
    f.mouse(QEvent::MouseButtonRelease, { 100, 100 }, Qt::RightButton, Qt::NoButton);
    settle();
    CHECK(f.session().activeTool() == core::ToolId::Move && !f.canvas->scene().transformOverlay);
    CHECK(f.session().activeLayer() == group);
    CHECK(f.shortcut(QKeySequence::Undo));
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(f.document().layer(f.members[i])->localToDocument == before[i]);
    CHECK(f.shortcut(QKeySequence(Qt::Key_B)));
    const auto beforeBrush = f.session().history().undoDepth();
    f.drag({ 50, 50 }, { 60, 55 });
    CHECK(f.session().history().undoDepth() == beforeBrush);
    CHECK(f.session().activeLayer() == group);
    CHECK(raster->revision() == revision && raster->dirtySince(revision).empty());
    CHECK(f.action("FillForegroundAction") && !f.action("FillForegroundAction")->isEnabled());
    CHECK(f.action("FillBackgroundAction") && !f.action("FillBackgroundAction")->isEnabled());
}

void ungroupPreservesUnrelatedSelectedItems()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto group = f.group();
    if (!group) return;
    f.click(f.spare); f.click(group, Qt::ControlModifier);
    const auto before = f.session().layerSelectionState();
    const auto tree = f.document().tree();
    CHECK(f.trigger("UngroupLayersAction"));
    CHECK(f.session().isLayerSelected(f.spare));
    for (const auto id : f.members) CHECK(f.session().isLayerSelected(id));
    CHECK(f.session().selectedLayers().size() == 4);
    CHECK(f.session().activeLayer() == f.members.back());
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == tree && f.session().layerSelectionState() == before);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.session().isLayerSelected(f.spare));
    for (const auto id : f.members) CHECK(f.session().isLayerSelected(id));
}

void rowControlsAndRenameDoNotChangeSelection()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.members[1]); f.click(f.members[2], Qt::ControlModifier);
    const auto beforeSelection = f.session().selectedLayers();
    const auto primary = f.session().activeLayer();
    const auto beforeHistory = f.session().history().undoDepth();
    const auto index = f.row(f.spare);
    f.view->scrollTo(index); settle();
    QTest::mouseClick(f.view->viewport(), Qt::LeftButton, Qt::NoModifier, f.view->visibilityIndicatorRect(index).center());
    settle();
    CHECK(f.session().selectedLayers() == beforeSelection && f.session().activeLayer() == primary);
    CHECK(f.session().history().undoDepth() == beforeHistory + 1);
    const auto name = f.document().layer(f.spare)->name;
    CHECK(f.model->setData(f.row(f.spare), QStringLiteral("Renamed spare"), Qt::EditRole));
    settle();
    CHECK(f.document().layer(f.spare)->name == "Renamed spare");
    CHECK(f.session().selectedLayers() == beforeSelection && f.session().activeLayer() == primary);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().layer(f.spare)->name == name);

    // Editable controls retain F2 ownership while the user is typing elsewhere.
    CHECK(f.shortcut(QKeySequence(Qt::Key_B)));
    auto* size = f.window.findChild<QDoubleSpinBox*>(QStringLiteral("BrushSizeControl"));
    auto* editor = size ? size->findChild<QLineEdit*>() : nullptr;
    CHECK(size && editor && size->isVisible());
    if (!size || !editor) return;
    size->setFocus();
    QTest::keyClick(size, Qt::Key_6);
    CHECK(!editor->isReadOnly());
    const auto text = editor->text();
    QKeyEvent override(QEvent::ShortcutOverride, Qt::Key_F2, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &override);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_F2, Qt::NoModifier);
    QCoreApplication::sendEvent(editor, &press);
    settle();
    CHECK(!f.view->renameEditor() && editor->text() == text);
    CHECK(f.session().selectedLayers() == beforeSelection);
    QTest::keyClick(editor, Qt::Key_Escape);
}

void inlineRenamePreservesSelectionHistoryAndStableIds()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    f.click(f.spare);
    const auto before = f.session().layerSelectionState();
    const auto name = f.document().layer(f.spare)->name;
    QTest::keyClick(f.canvas, Qt::Key_F2);
    settle();
    auto* editor = f.view->renameEditor();
    CHECK(editor && editor->isVisible() && !editor->isWindow());
    if (!editor) return;
    CHECK(editor->parentWidget() == f.view->viewport());
    CHECK(editor->property("layerId").toULongLong() == f.spare);
    const auto row = f.view->visualRect(f.row(f.spare));
    CHECK(editor->geometry().left() > f.view->visibilityIndicatorRect(f.row(f.spare)).right());
    CHECK(editor->geometry().left() >= row.left() + f.model->rowIndent(f.row(f.spare).row()) + f.view->iconSize().width());
    CHECK(editor->geometry().right() < row.right() - 3);
    // Keep Qt's ordinary Cut/Copy/Paste/Select All menu inside the editor's
    // ownership, even though its popup occupies a separate native window.
    std::unique_ptr<QMenu> menu(editor->createStandardContextMenu());
    menu->popup(editor->mapToGlobal(editor->rect().bottomLeft()));
    settle();
    QAction* selectAll = nullptr;
    for (auto* action : menu->actions())
        if (action->text().contains(QStringLiteral("Select All"))) selectAll = action;
    CHECK(selectAll && ui::popupLogicalParent(menu.get()) == editor);
    if (selectAll) QTest::mouseClick(menu.get(), Qt::LeftButton, {}, menu->actionGeometry(selectAll).center());
    CHECK(f.view->renameEditor() == editor);
    menu->hide();
    settle();
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor, QStringLiteral("Renamed with F2"));
    CHECK(f.document().layer(f.spare)->name == name);
    CHECK(f.session().history().undoDepth() == 0);
    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(!f.view->renameEditor());
    CHECK(f.document().layer(f.spare)->name == "Renamed with F2");
    CHECK(f.session().layerSelectionState() == before);
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().layer(f.spare)->name == name);
    const auto redo = f.session().history().redoDepth();
    for (const auto& text : { QString::fromStdString(name), QStringLiteral("   "), QStringLiteral("cancelled") }) {
        CHECK(f.view->beginRename(f.spare));
        editor = f.view->renameEditor();
        QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClicks(editor, text);
        QTest::keyClick(editor, text == QStringLiteral("cancelled") ? Qt::Key_Escape : Qt::Key_Return);
        CHECK(f.session().history().undoDepth() == 0 && f.session().history().redoDepth() == redo);
        CHECK(f.document().layer(f.spare)->name == name);
    }
    // A refresh/reorder must keep the pinned ID and draft, never rename the
    // layer that happens to occupy the old row when editing completes.
    CHECK(f.view->beginRename(f.spare));
    editor = f.view->renameEditor();
    QTest::keyClicks(editor, QStringLiteral("Moved draft"));
    CHECK(f.document().moveLayer(f.spare, 0));
    f.model->refresh();
    CHECK(f.view->renameEditor() == editor && editor->text() == QStringLiteral("Moved draft"));
    QTest::keyClick(editor, Qt::Key_Return);
    CHECK(f.document().layer(f.spare)->name == "Moved draft");
    CHECK(f.document().layer(f.members[2])->name == "Text member");
    CHECK(f.view->beginRename(f.spare));
    CHECK(f.document().takeLayer(f.spare).has_value());
    const auto depth = f.session().history().undoDepth();
    f.model->refresh();
    CHECK(!f.view->renameEditor() && f.session().history().undoDepth() == depth);
}

void inlineRenameContainersAndCollapsedDescendants()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.trigger("NewLayerFolderAction"));
    const auto folder = f.session().activeLayer().value_or(0);
    CHECK(f.menuCommand(folder, "Rename", true));
    auto* edit = f.view->renameEditor();
    CHECK(edit); if (!edit) return;
    QTest::keyClicks(edit, QStringLiteral("Named folder"));
    QTest::keyClick(edit, Qt::Key_Return);
    if (f.document().tree().container(folder)->name != "Named folder")
        std::cerr << "Folder rename actual: " << f.document().tree().container(folder)->name << '\n';
    CHECK(f.document().tree().container(folder)->name == "Named folder");
    std::unique_ptr<QMimeData> children(f.model->mimeData({ f.row(f.spare) }));
    CHECK(f.model->dropMimeData(children.get(), Qt::MoveAction, -1, 0, f.row(folder)));
    f.click(f.spare);
    f.disclosure(folder);
    CHECK(f.model->rowForLayer(f.spare) < 0);
    const auto selection = f.session().layerSelectionState();
    const auto history = f.session().history().undoDepth();
    QTest::keyClick(f.canvas, Qt::Key_F2);
    CHECK(f.model->expanded(folder));
    CHECK(f.view->renameEditor());
    CHECK(f.session().layerSelectionState() == selection);
    if (f.view->renameEditor()) QTest::keyClick(f.view->renameEditor(), Qt::Key_Escape);
    CHECK(f.session().history().undoDepth() == history);
    const auto group = f.group();
    CHECK(group);
    CHECK(f.menuCommand(group, "Rename", true));
    edit = f.view->renameEditor();
    CHECK(edit); if (!edit) return;
    QTest::keyClicks(edit, QStringLiteral("Named group"));
    QTest::keyClick(edit, Qt::Key_Return);
    CHECK(f.document().tree().container(group)->name == "Named group");
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree().container(group)->name != "Named group");
}

void inlineRenameControlFocusAndClickAway(Fixture& f, bool native)
{
    CHECK(f.shortcut(QKeySequence(Qt::Key_V)));
    f.click(f.members[1]);
    f.click(f.spare, Qt::ControlModifier);
    const auto selection = f.session().layerSelectionState();
    const auto original = f.document().layer(f.spare)->name;
    const auto initialDepth = f.session().history().undoDepth();
    auto key = [&](Qt::Key code, Qt::KeyboardModifiers modifiers = {}) {
        if (native) {
            QElapsedTimer activation;
            activation.start();
            while (!QGuiApplication::focusWindow() && activation.elapsed() < 1000) QTest::qWait(5);
            auto* carrier = QGuiApplication::focusWindow();
            CHECK(carrier);
            if (carrier) QTest::keyClick(carrier, code, modifiers);
        } else QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &f.window, code, modifiers);
        settle();
    };
    auto clickWidget = [&](QWidget* widget, const QPoint& position) {
        if (native) {
            auto* carrier = widget->window()->windowHandle();
            CHECK(carrier);
            if (carrier) QTest::mouseClick(carrier, Qt::LeftButton, {}, carrier->mapFromGlobal(widget->mapToGlobal(position)));
        } else QTest::mouseClick(widget, Qt::LeftButton, {}, position);
        settle();
    };
    auto undo = [&] {
        if (native) key(Qt::Key_Z, Qt::ControlModifier);
        else CHECK(f.shortcut(QKeySequence::Undo));
    };
    auto* combo = f.window.findChild<QComboBox*>("LayerBlendModeCombo");
    CHECK(combo); if (!combo) return;
    clickWidget(combo, combo->rect().center());
    if (auto* popup = QApplication::activePopupWidget()) {
        // Do not open/close a native popup before the compositor has activated
        // it. Otherwise its delayed deactivation can arrive AFTER F2 and quite
        // correctly finish the newly opened inline editor as application loss.
        if (native) CHECK(QTest::qWaitForWindowActive(popup->windowHandle()));
        QTest::keyClick(popup, Qt::Key_Escape);
    }
    settle();
    CHECK(!QApplication::activePopupWidget());
    if (native) {
        CHECK(QTest::qWaitForWindowActive(f.window.windowHandle()));
        // QWidget's active bit can remain true through popup teardown while
        // Wayland still has focus-out/focus-in messages queued. Wait for a
        // stable native owner before sending the next independent gesture.
        // Otherwise delayed application loss correctly commits the F2 editor
        // opened by this synthetic, zero-time Escape/F2 sequence.
        QElapsedTimer stable, deadline;
        stable.start(); deadline.start();
        const auto focus = QObject::connect(qApp, &QGuiApplication::focusWindowChanged,
            &f.window, [&](QWindow*) { stable.restart(); });
        const auto state = QObject::connect(qApp, &QGuiApplication::applicationStateChanged,
            &f.window, [&](Qt::ApplicationState) { stable.restart(); });
        const auto ready = [&] {
            return QGuiApplication::focusWindow() == f.window.windowHandle()
                && QGuiApplication::applicationState() == Qt::ApplicationActive
                && !QApplication::activePopupWidget() && stable.elapsed() >= 80;
        };
        while (!ready() && deadline.elapsed() < 2000) QTest::qWait(5);
        CHECK(ready());
        QObject::disconnect(focus); QObject::disconnect(state);
    }
    key(Qt::Key_F2);
    auto* edit = f.view->renameEditor();
    CHECK(edit && edit->isVisible()); if (!edit) return;
    CHECK(QGuiApplication::focusObject() == edit || QApplication::focusWidget() == edit);
    // A native carrier arrives before its real QWidget recipient. Clicking
    // inside the name must not be mistaken for an outside-canvas confirmation.
    clickWidget(edit, edit->rect().center());
    CHECK(f.view->renameEditor() == edit);
    key(Qt::Key_A, Qt::ControlModifier);
    key(Qt::Key_B); key(Qt::Key_E); key(Qt::Key_T); key(Qt::Key_Space); key(Qt::Key_6);
    CHECK(edit->text() == QStringLiteral("bet 6"));
    CHECK(f.session().activeTool() == core::ToolId::Move);
    CHECK(f.document().layer(f.spare)->name == original);
    CHECK(f.session().history().undoDepth() == initialDepth);
    key(Qt::Key_Return);
    CHECK(!f.view->renameEditor());
    CHECK(f.document().layer(f.spare)->name == "bet 6");
    CHECK(f.session().layerSelectionState() == selection);
    CHECK(f.session().history().undoDepth() == initialDepth + 1);
    undo();
    CHECK(f.document().layer(f.spare)->name == original);
    const auto redo = f.session().history().redoDepth();
    CHECK(f.shortcut(QKeySequence(Qt::Key_B)));
    auto* size = dynamic_cast<ui::CompactValueControl*>(f.window.findChild<QDoubleSpinBox*>("BrushSizeControl"));
    CHECK(size); if (!size) return;
    clickWidget(size, size->progressTrackRect().center());
    CHECK(!size->isManualEntryActive());
    key(Qt::Key_F2);
    edit = f.view->renameEditor();
    CHECK(edit); if (!edit) return;
    const auto screenshot = qEnvironmentVariable("IMAGEEDITOR_INLINE_RENAME_SCREENSHOT");
    if (!screenshot.isEmpty()) CHECK(f.view->viewport()->grab().save(screenshot));
    key(Qt::Key_A, Qt::ControlModifier); key(Qt::Key_7);
    if (edit->text() != QStringLiteral("7"))
        qWarning() << "Inline rename after slider: text" << edit->text() << "widget focus" << QApplication::focusWidget()
            << "native focus" << QGuiApplication::focusWindow() << "focus object" << QGuiApplication::focusObject()
            << "expected editor" << edit << "size now" << size->value();
    CHECK(edit->text() == QStringLiteral("7"));
    key(Qt::Key_Escape);
    CHECK(!f.view->renameEditor());
    CHECK(f.document().layer(f.spare)->name == original);
    CHECK(f.session().history().undoDepth() == initialDepth && f.session().history().redoDepth() == redo);
    if (native) {
        key(Qt::Key_V);
        CHECK(f.session().activeTool() == core::ToolId::Move);
        key(Qt::Key_B);
        CHECK(f.session().activeTool() == core::ToolId::Brush);
    }
    key(Qt::Key_F2);
    edit = f.view->renameEditor();
    CHECK(edit); if (!edit) return;
    key(Qt::Key_A, Qt::ControlModifier); key(Qt::Key_8);
    // A NoFocus tool button must still commit the inline name before its own
    // action is dispatched. FocusOut alone would miss this path.
    auto* noFocus = f.window.findChild<QWidget*>("BrushDirectionButton");
    CHECK(noFocus);
    if (noFocus) clickWidget(noFocus, noFocus->rect().center());
    else f.view->finishRename();
    CHECK(!f.view->renameEditor());
    CHECK(f.document().layer(f.spare)->name == "8");
    CHECK(f.session().history().undoDepth() == initialDepth + 1);
    undo();
    CHECK(f.shortcut(QKeySequence(Qt::Key_V)));
    // A canvas click confirms too, including on the native Vulkan surface.
    key(Qt::Key_F2);
    edit = f.view->renameEditor();
    CHECK(edit); if (!edit) return;
    key(Qt::Key_A, Qt::ControlModifier); key(Qt::Key_9);
    QTest::mouseClick(f.canvas, Qt::LeftButton, {}, QPoint(420, 260));
    settle();
    CHECK(!f.view->renameEditor());
    CHECK(f.document().layer(f.spare)->name == "9");
    undo();
    CHECK(f.document().layer(f.spare)->name == original);
}

void mergeActionReplacesGroupAndRestoresTypedMembers()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    CHECK(f.document().setLayerVisibility(f.members[0], true));
    const auto group = f.group();
    if (!group) return;
    const auto beforeTree = f.document().tree();
    const auto beforeSelection = f.session().layerSelectionState();
    const auto beforeDepth = f.session().history().undoDepth();
    const auto originalRaster = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
    const auto originalShape = std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload);
    const auto originalText = std::get<core::TextLayer>(f.document().layer(f.members[2])->payload);
    std::array<core::AffineTransform, 3> transforms;
    for (std::size_t i = 0; i < transforms.size(); ++i) transforms[i] = f.document().layer(f.members[i])->localToDocument;
    CHECK(f.trigger("MergeLayersAction"));
    const auto mergedId = f.session().activeLayer().value_or(0);
    CHECK(mergedId != group && mergedId != f.spare);
    CHECK(!f.document().containsItem(group));
    CHECK(f.document().layers().size() == 2 && f.model->rowCount() == 2);
    const auto* merged = f.document().layer(mergedId);
    CHECK(merged && std::holds_alternative<core::RasterLayer>(merged->payload));
    if (!merged) return;
    CHECK(merged->opacity == 1.0F);
    CHECK(f.session().history().undoDepth() == beforeDepth + 1);
    CHECK(f.session().selectedLayers() == std::vector<core::LayerId> { mergedId });
    const auto mergedTree = f.document().tree();
    for (const auto id : f.members) CHECK(!f.document().containsLayer(id));
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(f.document().tree() == beforeTree && f.session().layerSelectionState() == beforeSelection);
    CHECK(f.document().layers().size() == 4 && !f.document().containsLayer(mergedId));
    CHECK(std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface == originalRaster);
    CHECK(std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload) == originalShape);
    CHECK(std::get<core::TextLayer>(f.document().layer(f.members[2])->payload) == originalText);
    for (std::size_t i = 0; i < transforms.size(); ++i) CHECK(f.document().layer(f.members[i])->localToDocument == transforms[i]);
    CHECK(f.shortcut(QKeySequence::Redo));
    CHECK(f.document().tree() == mergedTree && f.session().activeLayer() == mergedId);
    CHECK(std::holds_alternative<core::RasterLayer>(f.document().layer(mergedId)->payload));
}

void mergeActionConsolidatesSeparatedRoots()
{
    for(int hierarchy=0;hierarchy<3;++hierarchy)for(bool reverse:{false,true}) {
        Fixture f;CHECK(f.valid());if(!f.valid())return;
        CHECK(f.document().setLayerVisibility(f.members[0],true));
        const auto low=core::makeLayerId(),high=core::makeLayerId();
        if(hierarchy) {
            auto tree=f.document().tree();tree.roots={low,f.members[1],high};
            tree.containers={{low,"Lower",core::ContainerKind::Folder,core::ColorLabel::Red,{f.members[0]}},
                {high,"Upper",core::ContainerKind::Folder,core::ColorLabel::Blue,{f.members[2],f.spare}}};
            CHECK(f.document().replaceStructure(f.document().tree(),std::move(tree),{},{}));
        }
        std::vector<core::LayerId> ids=hierarchy==2?std::vector{low,f.members[0],f.members[2]}:
            std::vector{f.members[0],f.members[2]};
        if(reverse)std::ranges::reverse(ids);
        f.session().setLayerSelection(ids,ids.back(),ids.front());
        f.model->refresh();CHECK(f.trigger("ToolAction_move"));
        const auto before=f.document().tree();const auto selection=f.session().layerSelectionState();
        const auto shape=std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload);
        const auto depth=f.session().history().undoDepth();
        CHECK(f.trigger("MergeLayersAction"));
        const auto merged=f.session().activeLayer().value_or(0);
        CHECK(merged!=f.members[0] && merged!=f.members[2] && f.document().containsLayer(merged));
        CHECK(f.document().tree().placement(merged)==core::ItemPlacement(hierarchy?high:0,hierarchy?0:1));
        CHECK(f.session().history().undoDepth()==depth+1);
        CHECK(std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload)==shape);
        if(hierarchy==1)CHECK(f.document().tree().container(low) && f.document().tree().container(low)->children.empty());
        if(hierarchy==2)CHECK(!f.document().containsItem(low));
        CHECK(f.shortcut(QKeySequence::Undo));
        CHECK(f.document().tree()==before && f.session().layerSelectionState()==selection);
        CHECK(f.shortcut(QKeySequence::Redo));CHECK(f.session().activeLayer()==merged);
    }
}

void cancellingHiddenMergeLeavesDocumentAndHistoryUntouched()
{
    Fixture f;
    CHECK(f.valid()); if (!f.valid()) return;
    const auto group = f.group();
    if (!group) return;
    const auto tree = f.document().tree();
    const auto selection = f.session().layerSelectionState();
    const auto content = f.document().contentState();
    const auto revision = f.document().revision();
    const auto depth = f.session().history().undoDepth();
    const auto memory = f.session().history().memoryUsed();
    bool warned = false;
    QTimer::singleShot(0, [&] {
        for (auto* box : f.window.findChildren<QMessageBox*>()) {
            if (!box->isVisible()) continue;
            warned = true;
            CHECK(!box->isWindow());
            CHECK(box->text().contains(QStringLiteral("hidden"), Qt::CaseInsensitive));
            box->reject();
        }
    });
    CHECK(f.trigger("MergeLayersAction"));
    CHECK(warned);
    CHECK(f.document().tree() == tree && f.session().layerSelectionState() == selection);
    CHECK(f.document().contentState() == content && f.document().revision() == revision);
    CHECK(f.session().history().undoDepth() == depth && f.session().history().memoryUsed() == memory);
    CHECK(!f.document().layer(f.members[0])->visible);
}

template<class Predicate> bool waitUntil(Predicate predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) QTest::qWait(10);
    return predicate();
}

int nativeValidation()
{
    if (QGuiApplication::platformName() != QStringLiteral("wayland")) {
        std::cerr << "Layer organization validation requires native Wayland\n";
        return EXIT_FAILURE;
    }
    int warnings = 0, errors = 0;
    QVulkanInstance instance;
    instance.setApiVersion(QVersionNumber(1, 2));
    if (!instance.supportedLayers().contains(QByteArrayLiteral("VK_LAYER_KHRONOS_validation"))) {
        std::cerr << "Layer organization validation requires VK_LAYER_KHRONOS_validation\n";
        return EXIT_FAILURE;
    }
    instance.setLayers({ QByteArrayLiteral("VK_LAYER_KHRONOS_validation") });
    instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags severity,
        QVulkanInstance::DebugMessageTypeFlags types, const void* message) {
        if (!types.testFlag(QVulkanInstance::ValidationMessage)) return false;
        if (severity.testFlag(QVulkanInstance::ErrorSeverity)) ++errors;
        else if (severity.testFlag(QVulkanInstance::WarningSeverity)) ++warnings;
        else return false;
        const auto* details = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
        std::cerr << (details ? details->pMessage : "Vulkan validation message") << '\n';
        return false;
    });
    if (!instance.create()) {
        std::cerr << "Could not create Vulkan instance\n";
        return EXIT_FAILURE;
    }
    {
        Fixture f(&instance);
        CHECK(f.valid());
        if (f.valid()) {
            f.window.activateWindow();
            CHECK(waitUntil([&] { return f.window.windowHandle()->isExposed()
                && QGuiApplication::focusWindow() == f.window.windowHandle(); }));
            inlineRenameControlFocusAndClickAway(f, true);
            f.session().history().clear();
            CHECK(f.shortcut(QKeySequence(Qt::Key_V)));
            f.click(f.members[1]);
            // Publish the fixture's intentionally hidden raster before taking
            // the settled baseline; production organization updates thereafter
            // must publish themselves through their usual UI command paths.
            f.canvas->setDocument(f.document().snapshot(), false);
            CHECK(waitUntil([&] { return f.canvas->rendererStats().framesSubmitted > 0
                && f.canvas->rendererStats().fullUploads >= 3; }));
            auto observed = f.canvas->rendererStats().uploadedBytes;
            QElapsedTimer stable;
            stable.start();
            CHECK(waitUntil([&] {
                const auto current = f.canvas->rendererStats().uploadedBytes;
                if (current != observed) { observed = current; stable.restart(); }
                return stable.elapsed() >= 200;
            }));
            const auto baseline = f.canvas->rendererStats();
            const auto textCache = f.document().layer(f.members[2])->renderCache;
            const auto shapeCache = f.document().layer(f.members[1])->renderCache;
            const auto raster = std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface;
            const auto rasterRevision = raster->revision();
            const auto renderedWithoutUploads = [&] {
                const auto frames = f.canvas->rendererStats().framesSubmitted;
                f.canvas->scheduleFrame();
                CHECK(waitUntil([&] { return f.canvas->rendererStats().framesSubmitted > frames; }));
                const auto now = f.canvas->rendererStats();
                CHECK(now.fullUploads == baseline.fullUploads);
                CHECK(now.regionalUploads == baseline.regionalUploads);
                CHECK(now.regionalUploadBatches == baseline.regionalUploadBatches);
                CHECK(now.uploadedBytes == baseline.uploadedBytes);
                CHECK(now.resourceGeneration == baseline.resourceGeneration);
                CHECK(f.document().layer(f.members[2])->renderCache == textCache);
                CHECK(f.document().layer(f.members[1])->renderCache == shapeCache);
                CHECK(raster->revision() == rasterRevision);
            };
            const auto group = f.group();
            CHECK(group != 0);
            const auto* target = f.window.findChild<QLabel*>(QStringLiteral("PropertiesTargetName"));
            CHECK(target && target->text() == QStringLiteral("Group"));
            renderedWithoutUploads();
            CHECK(f.model->setData(f.row(group), QStringLiteral("Native validated group"), Qt::EditRole));
            renderedWithoutUploads();
            CHECK(f.trigger("HideSelectedLayersAction"));
            CHECK(!f.document().tree().container(group)->visible);
            CHECK(!f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
            renderedWithoutUploads();
            CHECK(f.trigger("ShowSelectedLayersAction"));
            CHECK(f.document().tree().container(group)->visible);
            CHECK(!f.document().layer(f.members[0])->visible && f.document().layer(f.members[1])->visible);
            renderedWithoutUploads();
            std::array<core::AffineTransform, 3> originals;
            for (std::size_t i = 0; i < originals.size(); ++i) originals[i] = f.document().layer(f.members[i])->localToDocument;
            f.drag({ 50, 50 }, { 54, 53 });
            for (std::size_t i = 0; i < originals.size(); ++i) {
                auto moved = originals[i]; moved.m02 += 4; moved.m12 += 3;
                CHECK(nearMatrix(f.document().layer(f.members[i])->localToDocument, moved));
            }
            CHECK(!f.document().layer(f.members[0])->visible);
            renderedWithoutUploads();
            CHECK(f.trigger("LayerTransformAction"));
            CHECK(f.canvas->scene().transformOverlay.has_value());
            f.drag({ 55, 55 }, { 58, 57 });
            renderedWithoutUploads();
            f.mouse(QEvent::MouseButtonPress, { 100, 100 }, Qt::RightButton, Qt::RightButton);
            f.mouse(QEvent::MouseButtonRelease, { 100, 100 }, Qt::RightButton, Qt::NoButton);
            settle();
            CHECK(!f.canvas->scene().transformOverlay);
            renderedWithoutUploads();
            CHECK(f.shortcut(QKeySequence::Undo));
            CHECK(f.shortcut(QKeySequence::Undo));
            for (std::size_t i = 0; i < originals.size(); ++i) CHECK(f.document().layer(f.members[i])->localToDocument == originals[i]);
            renderedWithoutUploads();
            CHECK(f.trigger("UngroupLayersAction"));
            renderedWithoutUploads();
            CHECK(f.trigger("NewLayerFolderAction"));
            const auto folder = f.session().activeLayer().value_or(0);
            CHECK(f.document().tree().container(folder));
            CHECK(f.action("LayerTransformAction") && !f.action("LayerTransformAction")->isEnabled());
            std::unique_ptr<QMimeData> contents(f.model->mimeData({ f.row(f.members[0]), f.row(f.members[1]), f.row(f.members[2]) }));
            CHECK(f.model->dropMimeData(contents.get(), Qt::MoveAction, -1, 0, f.row(folder)));
            renderedWithoutUploads();
            CHECK(f.shortcut(QKeySequence::Undo));
            renderedWithoutUploads();
            CHECK(f.canvas->rendererStats().framesSubmitted > baseline.framesSubmitted);
            f.window.logRendererDiagnostics();
            std::cout << "Layer organization native frames=" << f.canvas->rendererStats().framesSubmitted - baseline.framesSubmitted
                      << ", additional texture bytes=" << f.canvas->rendererStats().uploadedBytes - baseline.uploadedBytes << '\n';
            // Separate from the metadata/geometry-only zero-upload phase:
            // actual Merge must publish a new raster texture, and Undo must
            // restore all editable sources through real rendered frames.
            CHECK(f.model->setData(f.row(f.members[0]), Qt::Checked, Qt::CheckStateRole));
            const auto mergeGroup = f.group();
            CHECK(mergeGroup != 0);
            const auto groupedTree = f.document().tree();
            const auto groupedSelection = f.session().layerSelectionState();
            const auto shapeBeforeMerge = std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload);
            const auto textBeforeMerge = std::get<core::TextLayer>(f.document().layer(f.members[2])->payload);
            const auto beforeMergeStats = f.canvas->rendererStats();
            CHECK(f.trigger("MergeLayersAction"));
            const auto mergedId = f.session().activeLayer().value_or(0);
            const auto* merged = f.document().layer(mergedId);
            CHECK(merged && std::holds_alternative<core::RasterLayer>(merged->payload));
            CHECK(!f.document().containsItem(mergeGroup));
            for (auto id : f.members) CHECK(!f.document().containsLayer(id));
            f.canvas->scheduleFrame();
            CHECK(waitUntil([&] {
                const auto now = f.canvas->rendererStats();
                return now.framesSubmitted > beforeMergeStats.framesSubmitted
                    && now.uploadedBytes > beforeMergeStats.uploadedBytes;
            }));
            const auto mergedFrames = f.canvas->rendererStats().framesSubmitted;
            const auto mergeStats = f.canvas->rendererStats();
            const auto mergeExtent=std::get<core::RasterLayer>(merged->payload).surface->extent();
            CHECK(mergeStats.fullUploads-beforeMergeStats.fullUploads==1);
            CHECK(mergeStats.uploadedBytes-beforeMergeStats.uploadedBytes==std::uint64_t(mergeExtent.width)*mergeExtent.height*4);
            std::cout << "MERGE_UPLOAD bytes=" << mergeStats.uploadedBytes-beforeMergeStats.uploadedBytes
                      << " full_uploads=" << mergeStats.fullUploads-beforeMergeStats.fullUploads
                      << " preparation_ms=" << double(mergeStats.uploadPreparationNanoseconds-beforeMergeStats.uploadPreparationNanoseconds)/1e6 << '\n';
            CHECK(f.shortcut(QKeySequence::Undo));
            CHECK(f.document().tree() == groupedTree && f.session().layerSelectionState() == groupedSelection);
            CHECK(!f.document().containsLayer(mergedId));
            CHECK(std::get<core::RasterLayer>(f.document().layer(f.members[0])->payload).surface == raster);
            CHECK(std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload) == shapeBeforeMerge);
            CHECK(std::get<core::TextLayer>(f.document().layer(f.members[2])->payload) == textBeforeMerge);
            for (std::size_t i = 0; i < originals.size(); ++i)
                CHECK(f.document().layer(f.members[i])->localToDocument == originals[i]);
            f.canvas->scheduleFrame();
            CHECK(waitUntil([&] { return f.canvas->rendererStats().framesSubmitted > mergedFrames; }));
            std::cout << "Layer organization native merge/undo rendered; new texture bytes="
                      << f.canvas->rendererStats().uploadedBytes - beforeMergeStats.uploadedBytes << '\n';
            const auto previewDepth=f.session().history().undoDepth();
            const auto previewContent=f.document().contentState();
            CHECK(f.trigger("PixelPreviewAction"));
            CHECK(waitUntil([&]{return bool(f.canvas->scene().pixelPreview);}));
            const auto nativePixels=f.canvas->scene().pixelPreview;
            const auto beforePreviewFrame=f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitUntil([&]{return f.canvas->rendererStats().framesSubmitted>beforePreviewFrame;}));
            const auto previewStats=f.canvas->rendererStats();
            f.canvas->resetTo100Percent();
            CHECK(waitUntil([&]{return f.canvas->rendererStats().framesSubmitted>previewStats.framesSubmitted;}));
            CHECK(f.canvas->scene().pixelPreview==nativePixels);
            CHECK(f.canvas->rendererStats().uploadedBytes==previewStats.uploadedBytes);
            CHECK(f.document().contentState()==previewContent && f.session().history().undoDepth()==previewDepth);
            CHECK(f.trigger("PixelPreviewAction"));
            CHECK(f.trigger("RasterizeLayersAction"));
            CHECK(f.document().tree()==groupedTree && f.session().layerSelectionState()==groupedSelection);
            for(auto id:f.members)CHECK(std::holds_alternative<core::RasterLayer>(f.document().layer(id)->payload));
            CHECK(f.session().history().undoDepth()==previewDepth+1);
            const auto beforeRasterFrame=f.canvas->rendererStats().framesSubmitted;
            f.canvas->scheduleFrame();
            CHECK(waitUntil([&]{return f.canvas->rendererStats().framesSubmitted>beforeRasterFrame;}));
            CHECK(f.shortcut(QKeySequence::Undo));
            CHECK(std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload)==shapeBeforeMerge);
            CHECK(std::get<core::TextLayer>(f.document().layer(f.members[2])->payload)==textBeforeMerge);
            std::cout << "Native Pixel Preview cache/zoom and independent Rasterize/undo passed\n";
            CHECK(f.trigger("UngroupLayersAction"));
            // Opt-in visual artifact; normal automated runs neither capture the
            // desktop nor leave a fixture window behind.
            const auto visibilityScreenshot = qEnvironmentVariable("IMAGEEDITOR_VISIBILITY_SCREENSHOT");
            if (!visibilityScreenshot.isEmpty()) {
                CHECK(f.model->dropMimeData(contents.get(), Qt::MoveAction, -1, 0, f.row(folder)));
                CHECK(f.model->setData(f.row(folder), QStringLiteral("Folder visibility"), Qt::EditRole));
                CHECK(f.model->setData(f.row(f.members[0]), QStringLiteral("Raster — own eye off"), Qt::EditRole));
                CHECK(f.model->setData(f.row(f.members[1]), QStringLiteral("Shape — own eye on"), Qt::EditRole));
                CHECK(f.model->setData(f.row(f.members[2]), QStringLiteral("Text — own eye on"), Qt::EditRole));
                CHECK(f.model->setData(f.row(f.members[0]), Qt::Unchecked, Qt::CheckStateRole));
                f.click(folder);
                const auto capture = [&](const QString& path) {
                    const auto frames = f.canvas->rendererStats().framesSubmitted;
                    f.canvas->scheduleFrame();
                    CHECK(waitUntil([&] { return f.canvas->rendererStats().framesSubmitted > frames; }));
                    f.window.activateWindow();
                    QTest::qWait(300);
                    QProcess screenshot;
                    screenshot.start(QStringLiteral("spectacle"), { QStringLiteral("--background"), QStringLiteral("--nonotify"),
                        QStringLiteral("--activewindow"), QStringLiteral("--output"), path });
                    CHECK(screenshot.waitForFinished(5000));
                    CHECK(screenshot.exitCode() == 0);
                };
                const auto visibleScreenshot = visibilityScreenshot.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
                    ? visibilityScreenshot.left(visibilityScreenshot.size() - 4) + QStringLiteral("-visible.png")
                    : visibilityScreenshot + QStringLiteral("-visible.png");
                capture(visibleScreenshot);
                CHECK(f.trigger("HideSelectedLayersAction"));
                CHECK(!f.document().tree().container(folder)->visible);
                CHECK(!f.document().layer(f.members[0])->visible);
                CHECK(f.document().layer(f.members[1])->visible && f.document().layer(f.members[2])->visible);
                for (const auto id : f.members) CHECK(!f.row(id).data(Qt::UserRole + 6).toBool());
                capture(visibilityScreenshot);
                std::cout << "Visibility review screenshots: " << visibleScreenshot.toStdString()
                          << " and " << visibilityScreenshot.toStdString() << '\n';
                CHECK(f.trigger("ShowSelectedLayersAction"));
            }
            const auto screenshot = qEnvironmentVariable("IMAGEEDITOR_ORGANIZATION_SCREENSHOT");
            if (!screenshot.isEmpty()) {
                CHECK(f.model->dropMimeData(contents.get(), Qt::MoveAction, -1, 0, f.row(folder)));
                f.click(f.members[0]);
                f.click(f.members[2], Qt::ShiftModifier);
                CHECK(f.trigger("GroupLayersAction"));
                CHECK(f.model->setData(f.row(folder), QStringLiteral("Artwork folder"), Qt::EditRole));
                const auto groupId = f.session().activeLayer().value_or(0);
                CHECK(f.model->setData(f.row(groupId), QStringLiteral("Editable artwork"), Qt::EditRole));
                f.window.activateWindow();
                QTest::qWait(300);
                QProcess capture;
                capture.start(QStringLiteral("spectacle"), { QStringLiteral("--background"), QStringLiteral("--nonotify"),
                    QStringLiteral("--activewindow"), QStringLiteral("--output"), screenshot });
                CHECK(capture.waitForFinished(5000));
                CHECK(capture.exitCode() == 0);
            }
        }
    }
    instance.destroy();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Layer organization Vulkan: " << warnings << " warnings, " << errors << " errors\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
}

void duplicateAndDeleteShortcuts()
{
    Fixture f; CHECK(f.valid()); if(!f.valid())return;
    f.click(f.members[1]); f.click(f.members[2],Qt::ControlModifier);
    const auto before=f.session().layerSelectionState();
    CHECK(f.menuCommand(f.members[2],"DuplicateLayersAction"));
    CHECK(f.document().layers().size()==6 && f.session().selectedLayers().size()==2);
    const auto copies=f.session().layerSelectionState();
    auto* duplicate=f.action("DuplicateLayersAction"), *remove=f.action("DeleteSelectedLayersAction");
    CHECK(duplicate && duplicate->shortcut()==QKeySequence("Shift+D"));
    CHECK(remove && remove->shortcut()==QKeySequence("Shift+Delete"));
    if(auto* focus=QApplication::focusWidget())focus->clearFocus();
    QTest::keyClick(f.canvas,Qt::Key_Delete,Qt::ShiftModifier); settle();
    CHECK(f.document().layers().size()==4 && f.session().history().undoDepth()==2);
    CHECK(f.shortcut(QKeySequence::Undo)); CHECK(f.session().layerSelectionState()==copies);
    CHECK(f.shortcut(QKeySequence::Undo)); CHECK(f.session().layerSelectionState()==before);
    QTest::keyClick(f.canvas,Qt::Key_D,Qt::ShiftModifier); settle();
    CHECK(f.document().layers().size()==6);
    // Neither shortcut steals editable-field input or creates history there.
    QLineEdit field(&f.window); field.show(); field.setFocus(); field.setText("rename"); field.selectAll();
    const auto depth=f.session().history().undoDepth();
    QTest::keyClick(&field,Qt::Key_Delete,Qt::ShiftModifier);
    QTest::keyClick(&field,Qt::Key_D,Qt::ShiftModifier);
    CHECK(f.document().layers().size()==6 && f.session().history().undoDepth()==depth);
}

void rasterizeAndPixelPreviewActions()
{
    Fixture f;CHECK(f.valid());if(!f.valid())return;
    f.click(f.members[1]);f.click(f.members[2],Qt::ControlModifier);
    const auto selection=f.session().layerSelectionState();const auto tree=f.document().tree();
    const auto shape=std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload);
    const auto text=std::get<core::TextLayer>(f.document().layer(f.members[2])->payload);
    const auto depth=f.session().history().undoDepth();
    CHECK(f.trigger("RasterizeLayersAction"));
    CHECK(f.session().history().undoDepth()==depth+1 && f.document().tree()==tree);
    CHECK(f.session().layerSelectionState()==selection);
    for(auto id:{f.members[1],f.members[2]})CHECK(std::holds_alternative<core::RasterLayer>(f.document().layer(id)->payload));
    CHECK(f.shortcut(QKeySequence::Undo));
    CHECK(std::get<core::ShapeLayer>(f.document().layer(f.members[1])->payload)==shape);
    CHECK(std::get<core::TextLayer>(f.document().layer(f.members[2])->payload)==text);
    const auto revision=f.document().revision(),content=f.document().contentState();
    const auto redo=f.session().history().redoDepth();
    const auto zoom=f.canvas->zoom();const auto pan=f.canvas->scene().viewport.pan();
    auto* action=f.action("PixelPreviewAction");
    CHECK(action && action->shortcut()==QKeySequence("Shift+P") && !action->autoRepeat());
    auto* workspace=dynamic_cast<ui::OverlayDockWorkspace*>(f.window.findChild<QWidget*>("CanvasWorkspace"));
    auto* badge=f.window.findChild<QWidget*>("PixelPreviewBadge");
    auto* exit=f.window.findChild<QPushButton*>("ExitPixelPreview");
    CHECK(workspace && badge && exit);if(!workspace || !badge || !exit)return;
    const auto canvasGeometry=workspace->canvasContainer()->geometry();
    if(auto* focus=QApplication::focusWidget())focus->clearFocus();
    QTest::keyClick(f.canvas,Qt::Key_P,Qt::ShiftModifier);settle();
    CHECK(f.canvas->scene().pixelPreviewEnabled && badge->isVisible());
    CHECK(workspace->panelOverlay()->windowHandle()->mask().contains(badge->geometry()));
    workspace->setRulerVisible(Qt::Horizontal,true);workspace->setRulerFarEdge(Qt::Horizontal,false);settle();
    CHECK(badge->geometry().top()>workspace->rulerStrip(Qt::Horizontal)->geometry().bottom());
    workspace->setRulerFarEdge(Qt::Horizontal,true);settle();
    CHECK(badge->geometry().top()==workspace->rulerContentRect().top()+8);
    workspace->setRulerVisible(Qt::Horizontal,false);settle();
    CHECK(badge->isVisible() && workspace->canvasContainer()->geometry()==canvasGeometry);
    QWidget modal(workspace->panelOverlay());workspace->setModalOverlay(&modal);settle();
    CHECK(!badge->isVisible());workspace->setModalOverlay(nullptr);settle();CHECK(badge->isVisible());
    QLineEdit field(&f.window);field.show();field.setFocus();
    QTest::keyClick(&field,Qt::Key_P,Qt::ShiftModifier);settle();
    CHECK(field.text().compare("p",Qt::CaseInsensitive)==0);
    CHECK(action->isChecked());field.clearFocus();field.hide();
    QKeyEvent repeat(QEvent::KeyPress,Qt::Key_P,Qt::ShiftModifier,"P",true);
    QCoreApplication::sendEvent(f.canvas,&repeat);CHECK(action->isChecked());
    QElapsedTimer timer;timer.start();
    while(!f.canvas->scene().pixelPreview && timer.elapsed()<10000){settle();QTest::qWait(2);}
    CHECK(bool(f.canvas->scene().pixelPreview));
    const auto cached=f.canvas->scene().pixelPreview;
    CHECK(f.document().revision()==revision && f.document().contentState()==content && f.session().history().redoDepth()==redo);
    CHECK(f.session().layerSelectionState()==selection && f.canvas->zoom()==zoom && f.canvas->scene().viewport.pan()==pan);
    f.canvas->resetTo100Percent();settle();QTest::qWait(120);settle();
    CHECK(f.canvas->scene().pixelPreview==cached);
    CHECK(f.canvas->scene().document.layersBottomToTop.size()==f.document().layers().size());
    QTest::mouseClick(exit,Qt::LeftButton);settle();
    CHECK(!f.canvas->scene().pixelPreviewEnabled && !f.canvas->scene().pixelPreview && !badge->isVisible());
    QTest::keyClick(f.canvas,Qt::Key_P,Qt::ShiftModifier);settle();CHECK(action->isChecked() && badge->isVisible());
    QTest::keyClick(f.canvas,Qt::Key_P,Qt::ShiftModifier);settle();CHECK(!action->isChecked() && !badge->isVisible());
    CHECK(f.session().history().redoDepth()==redo && f.document().contentState()==content);
}

void pixelPreviewPreference()
{
    // Main uses a temporary QSettings root; no real user preferences are read
    // or overwritten. Startup with no document must retain the checked mode.
    QSettings settings;settings.setValue(QStringLiteral("view/pixelPreview"),true);
    {
        ui::MainWindow window(nullptr,true,false);
        auto* action=window.findChild<QAction*>(QStringLiteral("PixelPreviewAction"));
        CHECK(action && action->isChecked());
        if(action)action->setChecked(false);
        CHECK(!settings.value(QStringLiteral("view/pixelPreview")).toBool());
    }
    {
        ui::MainWindow window(nullptr,true,false);
        auto* action=window.findChild<QAction*>(QStringLiteral("PixelPreviewAction"));
        CHECK(action && !action->isChecked());
    }
    settings.remove(QStringLiteral("view/pixelPreview"));
}

int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("LayerOrganizationInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    CHECK(settings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(app);
    if (qEnvironmentVariableIntValue("IMAGEEDITOR_ORGANIZATION_NATIVE") != 0) return nativeValidation();
    contextClicksSelectTheirTargetWithoutCollapsingSelectedSets();
    duplicateAndDeleteShortcuts();
    newFolderUsesTheExplicitContextInsteadOfTheCurrentSelection();
    addToNewFolderUsesTopSelectedPositionAndExactUndo();
    addToNewFolderRetainsCollapsedTargetsAndNormalizesAncestors();
    folderAncestorSelectionIndicatorsAreUiOnly();
    layerAndFolderIconsUseIndependentThumbnailDefaultsAndColorLabels();
    folderCollapsePersistsAsOptionalProjectUiState();
    visibilityShortcutsUseAncestorGatesAndOneUndoStep();
    isolateAndShowAllShortcutsAreAtomicAndDoNotToggle();
    visibilityShortcutsRespectEditableFieldOwnership();
    dissolvingHiddenContainersPreservesAppearanceAndExactDescendantBits();
    foldersDisclosureDisplayedRangesAndDnD();
    groupingAndUngroupingUseOneRowAndExactHistory();
    collapsedSelectedRowsSurviveDragPressButNotPlainClick();
    dissolvingSelectedFolderAndChildRetainsAUniqueSelection();
    groupCanvasMoveTransformAndPixelToolRejection();
    ungroupPreservesUnrelatedSelectedItems();
    rowControlsAndRenameDoNotChangeSelection();
    inlineRenamePreservesSelectionHistoryAndStableIds();
    inlineRenameContainersAndCollapsedDescendants();
    {
        Fixture f;
        CHECK(f.valid());
        if (f.valid()) inlineRenameControlFocusAndClickAway(f, false);
    }
    mergeActionReplacesGroupAndRestoresTypedMembers();
    mergeActionConsolidatesSeparatedRoots();
    rasterizeAndPixelPreviewActions();
    pixelPreviewPreference();
    cancellingHiddenMergeLeavesDocumentAndHistoryUntouched();
    if (failures) return EXIT_FAILURE;
    std::cout << "Layer organization interaction tests passed\n";
    return EXIT_SUCCESS;
}
