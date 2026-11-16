#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/CompactValueControl.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/TextController.hpp"
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;

class MirroredLayerUiTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("VulkanaTests");
        QCoreApplication::setApplicationName("MirroredLayers");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QStandardPaths::setTestModeEnabled(true);
        u::applyEditorTheme(*qobject_cast<QApplication*>(qApp));
    }
    void mirrorActionsReadonlyStateAndOpacityLinks() {
        QSettings().clear();
        QTemporaryDir directory;
        c::Document original({{32,24},96});
        auto source = c::Layer::raster("Source", std::make_shared<c::ContiguousRasterSurface>(
            c::Extent2u{32,24}, c::Rgba8{80,130,220,255}));
        source.opacity = .8F; source.fillOpacity = .7F;
        source.mask = std::make_shared<c::LayerMask>(c::LayerMask{c::SelectionMask::filled({32,24},180),{},255,true});
        const auto sourceId = source.id;
        QVERIFY(original.insertLayer(0, source));
        const auto path = directory.filePath("mirror.vulkana");
        QVERIFY(u::saveProject(path, original));
        u::MainWindow window(nullptr,false,false);
        window.setUnsavedPromptEnabled(false); window.resize(1400,850); window.show();
        QVERIFY(window.openImageFromPath(path));
        QTest::qWait(20);
        auto& session = const_cast<c::EditorSession&>(window.editorSession());
        auto& doc = *session.document();
        auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
        QVERIFY(view);
        auto* model = dynamic_cast<u::LayerListModel*>(view->model()); QVERIFY(model);
        const auto choose = [&](c::LayerId id) { view->onRowSelectionRequested(model->rowForLayer(id), Qt::NoModifier); };
        auto* create = window.findChild<QAction*>("CreateMirroredLayerAction");
        auto* promote = window.findChild<QAction*>("PromoteMirroredLayerAction");
        auto* sync = window.findChild<QAction*>("SyncMirroredLayerAction");
        auto* selectSource = window.findChild<QAction*>("SelectMirrorSourceAction");
        auto* chain = window.findChild<QToolButton*>("LayerOpacityLink");
        auto* mode = window.findChild<QToolButton*>("LayerOpacityMode");
        auto* opacity = dynamic_cast<u::CompactValueControl*>(window.findChild<QDoubleSpinBox*>("LayerOpacitySlider"));
        QVERIFY(create && promote && sync && selectSource && chain && mode && opacity);
        choose(sourceId);
        QVERIFY(create->isEnabled()); QVERIFY(!promote->isEnabled()); QVERIFY(chain->isHidden());
        create->trigger();
        const auto mirrorId = *session.activeLayer();
        QVERIFY(mirrorId != sourceId); QVERIFY(doc.layer(mirrorId)->mirror);
        QVERIFY(promote->isEnabled()); QVERIFY(sync->isEnabled()); QVERIFY(selectSource->isEnabled());
        QVERIFY(!chain->isHidden()); QVERIFY(chain->isChecked()); QCOMPARE(opacity->value(),80.0);
        const auto mirrorIndex = model->index(model->rowForLayer(mirrorId));
        QVERIFY(mirrorIndex.data(u::LayerListModel::MirroredRole).toBool());
        view->setLayerInfoVisible(true);
        QVERIFY(view->blendModeIndicatorText(mirrorIndex).contains("Mirrored"));
        QVERIFY(view->beginRename(mirrorId));
        QVERIFY(view->blendModeIndicatorText(mirrorIndex).isEmpty());
        QVERIFY(view->renameEditor());
        QTest::keyClick(view->renameEditor(), Qt::Key_Escape);

        opacity->setValue(35);
        QVERIFY(doc.layer(mirrorId)->mirror->opacityOverride); QVERIFY(!chain->isChecked());
        QCOMPARE(doc.layer(sourceId)->opacity,.8F);
        chain->click();
        QVERIFY(!doc.layer(mirrorId)->mirror->opacityOverride); QVERIFY(chain->isChecked());
        QCOMPARE(doc.layer(mirrorId)->opacity,.8F);
        mode->click(); QCOMPARE(opacity->value(),70.0);
        opacity->setValue(25);
        QVERIFY(doc.layer(mirrorId)->mirror->fillOpacityOverride); QVERIFY(!chain->isChecked());
        chain->click();
        QVERIFY(!doc.layer(mirrorId)->mirror->fillOpacityOverride); QCOMPARE(doc.layer(mirrorId)->fillOpacity,.7F);
        for (const auto* name : {"ToolAction_brush", "ToolAction_eraser", "ToolAction_cloning",
                                "ToolAction_local_blur", "ToolAction_fill", "ToolAction_crop",
                                "RefineMaskAction", "ApplyLayerMaskAction", "ErasePixelsAction"}) {
            auto* action = window.findChild<QAction*>(name); QVERIFY2(action,name); QVERIFY2(!action->isEnabled(),name);
        }
        for (const auto* name : {"ToolAction_move", "ToolAction_eyedropper", "ToolAction_marquee",
                                "ToolAction_guides", "CopyLayerMaskAction", "MaskToSelectionAction"}) {
            auto* action = window.findChild<QAction*>(name); QVERIFY2(action,name); QVERIFY2(action->isEnabled(),name);
        }
        auto* tabs = window.findChild<QTabWidget*>("AdjustmentTabs"); QVERIFY(tabs);
        for (int i : {0,1,2,3}) QVERIFY(!tabs->isTabEnabled(i));
        QVERIFY(tabs->isTabEnabled(4));
        auto* selectAll = window.findChild<QAction*>("SelectAllAction");
        auto* refine = window.findChild<QAction*>("RefineSelectionAction");
        QVERIFY(selectAll && refine);
        selectAll->trigger(); QVERIFY(refine->isEnabled());
        refine->trigger();
        QCOMPARE(session.activeTool(), c::ToolId::Brush);
        auto* cancel = window.findChild<QPushButton*>("RefineCancel"); QVERIFY(cancel);
        cancel->click();
        QVERIFY(!window.findChild<QAction*>("ToolAction_brush")->isEnabled());

        auto transform = doc.layer(mirrorId)->localToDocument; transform.m02 = 6;
        QVERIFY(doc.setLayerTransform(mirrorId, transform));
        choose(mirrorId); opacity->setValue(20); sync->trigger();
        QVERIFY(!doc.layer(mirrorId)->mirror->fillOpacityOverride);
        QVERIFY(doc.layer(mirrorId)->localToDocument == transform);
        selectSource->trigger(); QCOMPARE(session.activeLayer(),std::optional(sourceId));
        QVERIFY(chain->isHidden()); QVERIFY(window.findChild<QAction*>("ToolAction_brush")->isEnabled());
        choose(mirrorId); promote->trigger();
        QVERIFY(!doc.layer(mirrorId)->mirror); QVERIFY(chain->isHidden());
        QVERIFY(window.findChild<QAction*>("ToolAction_brush")->isEnabled());
        QVERIFY(session.undo()); choose(mirrorId);
        QVERIFY(doc.layer(mirrorId)->mirror); QVERIFY(!chain->isHidden());
        window.close();
    }
    void typedMirrorsDoNotEditOverlappingSources() {
        QSettings().clear();
        QTemporaryDir directory;
        c::Document original({{256,256},96});
        c::TextLayer text; text.utf8 = "Source text";
        auto textLayer = c::Layer::text("Text", text);
        textLayer.localToDocument.m02 = 10; textLayer.localToDocument.m12 = 10;
        const auto textId = textLayer.id;
        c::ShapeLayer shape; shape.size = {64,64};
        auto shapeLayer = c::Layer::shape("Shape", shape);
        shapeLayer.localToDocument.m02 = 100; shapeLayer.localToDocument.m12 = 100;
        const auto shapeId = shapeLayer.id;
        QVERIFY(original.insertLayer(0, textLayer)); QVERIFY(original.insertLayer(1, shapeLayer));
        const auto path = directory.filePath("typed.vulkana"); QVERIFY(u::saveProject(path, original));
        u::MainWindow window(nullptr,false,false);
        window.setUnsavedPromptEnabled(false); window.resize(1400,850); window.show();
        QVERIFY(window.openImageFromPath(path));
        auto& session = const_cast<c::EditorSession&>(window.editorSession());
        auto& doc = *session.document();
        auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList")); QVERIFY(view);
        auto* model = dynamic_cast<u::LayerListModel*>(view->model()); QVERIFY(model);
        auto* create = window.findChild<QAction*>("CreateMirroredLayerAction"); QVERIFY(create);
        u::TextController* controller = nullptr;
        for (auto* child : window.findChildren<QObject*>())
            if (auto* textController = dynamic_cast<u::TextController*>(child)) controller = textController;
        QVERIFY(controller);
        imageeditor::render::CanvasWindow* canvas = nullptr;
        for (auto* candidate : QGuiApplication::allWindows())
            if (candidate->objectName() == "VulkanCanvasWindow")
                if (auto* value = dynamic_cast<imageeditor::render::CanvasWindow*>(candidate)) canvas = value;
        QVERIFY(canvas);
        view->onRowSelectionRequested(model->rowForLayer(textId), Qt::NoModifier); create->trigger();
        const auto textMirror = *session.activeLayer();
        controller->prepareCaches();
        const auto extent = c::layerGeometryExtent(*doc.layer(textMirror)); QVERIFY(!extent.empty());
        const auto point = doc.layer(textMirror)->localToDocument.map({double(extent.width)/2, double(extent.height)/2});
        auto* textTool = window.findChild<QAction*>("ToolAction_text"); QVERIFY(textTool); textTool->trigger();
        QVERIFY(canvas->onTextPressed(point, Qt::NoModifier, false));
        QVERIFY(controller->layerId() != textId); QVERIFY(controller->layerId() != textMirror);
        controller->finish();
        QCOMPARE(std::get<c::TextLayer>(doc.layer(textId)->payload).utf8, text.utf8);
        view->onRowSelectionRequested(model->rowForLayer(shapeId), Qt::NoModifier); create->trigger();
        const auto shapeMirror = *session.activeLayer(); QVERIFY(shapeMirror != shapeId);
        QVERIFY(canvas->onShapeHit); QVERIFY(!canvas->onShapeHit({130,130}, false));
        QVERIFY(!canvas->onShapeHit({130,130}, true));
        window.close();
    }
};
QTEST_MAIN(MirroredLayerUiTests)
#include "MirroredLayerUiTests.moc"
