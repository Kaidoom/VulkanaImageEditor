#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/PreferencesDialog.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "NativeGraphicsStartup.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures = 0;
#define CHECK(v) do { if (!(v)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " << #v << '\n'; } } while(false)
void settle() { QCoreApplication::sendPostedEvents(); QCoreApplication::processEvents(); QTest::qWait(10); }
struct Fixture {
    QTemporaryDir dir;
    u::MainWindow window;
    u::LayerListView* view {};
    u::LayerListModel* model {};
    r::CanvasWindow* canvas {};
    std::vector<c::LayerId> leaves;
    c::LayerId outer = c::makeLayerId(), inner = c::makeLayerId(), group = c::makeLayerId();
    explicit Fixture(bool persist = false) : window(nullptr, persist, false)
    {
        window.setUnsavedPromptEnabled(false); window.resize(1400,900); window.show();
        CHECK(imageeditor::test::waitForGraphicsStartup(window)); settle();
        view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
        model = view ? dynamic_cast<u::LayerListModel*>(view->model()) : nullptr;
        for (auto* w : QGuiApplication::allWindows()) if (auto* value = dynamic_cast<r::CanvasWindow*>(w)) canvas = value;
        CHECK(view && model && canvas);
        c::Document document({{1024,256},96});
        for (int i = 0; i < 48; ++i) {
            std::vector<std::byte> bytes(16 * 16 * 4, std::byte(255));
            auto layer = c::Layer::raster("Layer " + std::to_string(i),
                std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{16,16}, std::move(bytes)));
            layer.localToDocument.m02 = i * 20; layer.localToDocument.m12 = 40;
            leaves.push_back(layer.id); CHECK(document.insertLayer(document.layers().size(), std::move(layer)));
        }
        auto tree = document.tree(); tree.roots.erase(tree.roots.begin(), tree.roots.begin() + 3);
        tree.containers.push_back({inner, "Inner folder", c::ContainerKind::Folder, c::ColorLabel::None, {leaves[0]}});
        tree.containers.push_back({group, "Atomic group", c::ContainerKind::Group, c::ColorLabel::None, {leaves[1],leaves[2]}});
        tree.containers.push_back({outer, "Outer folder", c::ContainerKind::Folder, c::ColorLabel::None, {inner,group}});
        tree.roots.insert(tree.roots.begin(), outer); CHECK(document.replaceStructure(document.tree(), tree));
        const auto path = dir.filePath("layers.vulkana"); CHECK(u::saveProject(path, document)); CHECK(window.openImageFromPath(path));
        window.findChild<QAction*>("ToolAction_move")->trigger(); settle();
    }
    ~Fixture() { window.close(); settle(); }
    c::EditorSession& session() { return const_cast<c::EditorSession&>(window.editorSession()); }
    void settings(std::function<void(u::PreferencesDialog&, QComboBox&)> operate)
    {
        bool handled = false; QTimer responder; responder.setInterval(5);
        QObject::connect(&responder, &QTimer::timeout, &window, [&] {
            for (auto* d : window.findChildren<QDialog*>("PreferencesDialog")) {
                auto* dialog = dynamic_cast<u::PreferencesDialog*>(d); if (!dialog || !dialog->isVisible()) continue;
                responder.stop(); handled = true;
                auto* combo = dialog->findChild<QComboBox*>("LayerSelectionFocus"); CHECK(combo);
                if (combo) operate(*dialog, *combo); else dialog->reject(); return;
            }
        });
        responder.start(); window.findChild<QAction*>("PreferencesAction")->trigger(); responder.stop(); CHECK(handled); settle();
    }
    void mode(u::LayerSelectionFocus mode, bool apply = true)
    {
        settings([&](u::PreferencesDialog& dialog, QComboBox& combo) {
            combo.setCurrentIndex(combo.findData(int(mode)));
            if (apply) dialog.findChild<QPushButton*>("PreferencesOk")->click(); else dialog.reject();
        });
    }
    void collapse()
    {
        if (model->expanded(inner)) model->toggleExpanded(inner);
        if (model->expanded(outer)) model->toggleExpanded(outer);
        settle(); view->verticalScrollBar()->setValue(0); settle();
    }
    bool visible(c::LayerId id)
    {
        const auto row = model->rowForLayer(id); if (row < 0) return false;
        const auto rect = view->visualRect(model->index(row));
        return rect.top() >= 0 && rect.bottom() < view->viewport()->height();
    }
    void clickCanvas(int layer)
    {
        const auto& scene = canvas->scene();
        const auto point = scene.viewport.documentToViewport({layer * 20.0 + 8, 48}, {1024,256}, {double(canvas->width()),double(canvas->height())});
        const QPointF local(point.x, point.y), global(canvas->mapToGlobal(local.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress, local, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(canvas, &press); QCoreApplication::sendEvent(canvas, &release); settle();
    }
};
void focusBehavior()
{
    Fixture f;
    f.settings([&](auto& dialog, auto& combo) {
        CHECK(combo.currentData().toInt() == int(u::LayerSelectionFocus::Focus)); CHECK(combo.count() == 3);
        const auto screenshot = qEnvironmentVariable("VULKANA_LAYER_FOCUS_SCREENSHOT");
        if (!screenshot.isEmpty()) CHECK(dialog.grab().save(screenshot));
        dialog.reject();
    });
    f.collapse(); CHECK(!f.visible(f.outer));
    const auto history = f.session().history().undoDepth(); const auto revision = f.session().document()->revision();
    f.clickCanvas(0); CHECK(f.session().activeLayer() == f.leaves[0]);
    CHECK(f.visible(f.outer) && !f.model->expanded(f.outer) && !f.model->expanded(f.inner));
    // Re-selecting the same canvas item focuses it again after manual scrolling.
    f.view->verticalScrollBar()->setValue(0); settle(); f.clickCanvas(0); CHECK(f.visible(f.outer));
    f.view->verticalScrollBar()->setValue(0); settle();
    f.canvas->setZoomAt(f.canvas->zoom() * 1.1, QPointF(100,100)); settle();
    CHECK(f.view->verticalScrollBar()->value() == 0); // A refresh does not chase selection.

    f.mode(u::LayerSelectionFocus::Disabled); f.clickCanvas(47); f.collapse();
    f.clickCanvas(0); CHECK(f.session().activeLayer() == f.leaves[0]);
    CHECK(f.view->verticalScrollBar()->value() == 0 && !f.model->expanded(f.outer));
    f.mode(u::LayerSelectionFocus::Expand, false); // Cancel restores Disabled.
    f.clickCanvas(47); f.clickCanvas(0); CHECK(f.view->verticalScrollBar()->value() == 0);
    f.mode(u::LayerSelectionFocus::Expand); f.clickCanvas(47); f.clickCanvas(0);
    CHECK(f.visible(f.leaves[0]) && f.model->expanded(f.outer) && f.model->expanded(f.inner));
    const auto scroll = f.view->verticalScrollBar()->value(); f.clickCanvas(0);
    CHECK(f.view->verticalScrollBar()->value() == scroll); // Already visible: no recentering.
    f.model->toggleExpanded(f.outer); settle(); CHECK(!f.model->expanded(f.outer));
    f.clickCanvas(0); CHECK(f.model->expanded(f.outer) && f.visible(f.leaves[0]));
    f.collapse(); f.clickCanvas(1); CHECK(f.visible(f.group));
    CHECK(f.model->rowForLayer(f.leaves[1]) < 0); // Atomic groups keep their single-row representation.
    CHECK(f.session().history().undoDepth() == history && f.session().document()->revision() == revision);
    CHECK(!f.session().document()->isModified());
    QSettings saved; CHECK(saved.value("preferences/ui/layerSelectionFocus").toInt() == int(u::LayerSelectionFocus::Expand));
}
void startupPersistence()
{
    Fixture f(true); f.collapse(); f.clickCanvas(0);
    CHECK(f.model->expanded(f.outer) && f.model->expanded(f.inner) && f.visible(f.leaves[0]));
    f.mode(u::LayerSelectionFocus::Focus); f.collapse(); f.clickCanvas(47); f.clickCanvas(0);
    CHECK(f.visible(f.outer) && !f.model->expanded(f.outer));
    CHECK(u::layerSelectionFocusFromInt(99) == u::LayerSelectionFocus::Focus);
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv); app.setApplicationName("LayerSelectionFocusTests"); app.setOrganizationName("ImageEditorTests");
    QStandardPaths::setTestModeEnabled(true); QTemporaryDir config;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, config.path());
    u::applyEditorTheme(app); focusBehavior(); startupPersistence();
    std::cout << "Layer selection focus: " << failures << " failures\n"; return failures ? 1 : 0;
}
