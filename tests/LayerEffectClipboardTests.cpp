#include "imageeditor/core/ChannelGroups.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/MirroredLayers.hpp"
#include "imageeditor/ui/LayerEffectClipboard.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
c::LayerEffectState sampleEffects()
{
    auto effects = std::make_shared<c::LayerEffectStack>();
    effects->pixelAlphaShapesEffects = false;
    effects->maskEffectsAfterRendering = true;
    auto& gradient = effects->items[std::size_t(c::LayerEffectType::GradientOverlay)];
    gradient.enabled = true;
    gradient.blendMode = c::BlendMode::Screen;
    gradient.opacity = .43;
    gradient.angle = -81;
    gradient.scale = 1.75;
    gradient.reverse = true;
    gradient.gradientStops = {{0,{20,40,230,255}}, {.36,{200,0,160,64}}, {1,{250,190,30,255}}};
    auto& stroke = effects->items[std::size_t(c::LayerEffectType::Stroke)];
    stroke.enabled = false; // Preserve disabled-but-customized effects too.
    stroke.size = 19;
    stroke.color = {10,20,30,40};
    auto& bevel = effects->items[std::size_t(c::LayerEffectType::BevelEmboss)];
    bevel.enabled = true;
    bevel.bevel.depth = 2.3;
    bevel.bevel.gloss.enabled = true;
    bevel.bevel.gloss.points = {{0,0,false},{.3,.8,true},{1,1,false}};
    return effects;
}
c::Layer raster(const char* name)
{
    return c::Layer::raster(name, std::make_shared<c::ContiguousRasterSurface>(
        c::Extent2u{32,24}, c::Rgba8{80,130,220,128}));
}
void clipboard(const QByteArray& bytes)
{
    auto* mime = new QMimeData;
    mime->setData(u::layerEffectClipboardMime, bytes);
    QApplication::clipboard()->setMimeData(mime);
    QCoreApplication::processEvents();
}
}

class LayerEffectClipboardTests : public QObject {
    Q_OBJECT
    QTemporaryDir settings_;
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("VulkanaTests");
        QCoreApplication::setApplicationName("LayerEffectClipboard");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
        QStandardPaths::setTestModeEnabled(true);
        u::applyEditorTheme(*qobject_cast<QApplication*>(qApp));
    }
    void init() { QSettings().clear(); QApplication::clipboard()->clear(); }
    void codecPreservesCompleteEffectSettings() {
        const auto original = sampleEffects();
        const auto encoded = u::encodeLayerEffectClipboard(original);
        QVERIFY(!encoded.isEmpty());
        const auto decoded = u::decodeLayerEffectClipboard(encoded);
        QVERIFY(decoded && *decoded);
        QVERIFY(**decoded == *original);
        QVERIFY(decoded->get() != original.get());
        const auto empty = u::decodeLayerEffectClipboard(u::encodeLayerEffectClipboard({}));
        QVERIFY(empty && c::equivalentLayerEffects(*empty, {}));
        auto many = std::make_shared<c::LayerEffectStack>(*original);
        auto& stops = many->items[6].gradientStops;
        stops.clear();
        for (std::size_t i=0; i<c::maximumGradientStops; ++i)
            stops.push_back({double(i)/double(c::maximumGradientStops-1), {10,20,30,255}});
        const auto full = u::decodeLayerEffectClipboard(u::encodeLayerEffectClipboard(many));
        QVERIFY(full && **full == *many);
    }
    void malformedClipboardIsRejected() {
        QVERIFY(!u::decodeLayerEffectClipboard({}));
        QVERIFY(!u::decodeLayerEffectClipboard("not effects"));
        QVERIFY(!u::decodeLayerEffectClipboard(QByteArray(128*1024+1, ' ')));
        auto root = QJsonDocument::fromJson(u::encodeLayerEffectClipboard(sampleEffects())).object();
        root["version"] = 2;
        QVERIFY(!u::decodeLayerEffectClipboard(QJsonDocument(root).toJson()));
        root["version"] = 1;
        auto effects = root["effects"].toObject();
        auto items = effects["items"].toArray();
        auto first = items[0].toObject(); first["opacity"] = -1; items[0] = first;
        effects["items"] = items; root["effects"] = effects;
        QVERIFY(!u::decodeLayerEffectClipboard(QJsonDocument(root).toJson()));
        root["effects"] = QJsonArray{};
        QVERIFY(!u::decodeLayerEffectClipboard(QJsonDocument(root).toJson()));
    }
    void contextMenuCopyPasteIsIndependentUndoableAndTargeted() {
        QTemporaryDir directory;
        c::Document original({{32,24},96});
        auto source = raster("Source"); source.effects = sampleEffects();
        const auto copied = source.effects;
        auto target = raster("Target"); target.opacity = .35F; target.fillOpacity = .8F;
        target.blendMode = c::BlendMode::Multiply; target.localToDocument.m02 = 4;
        auto old = std::make_shared<c::LayerEffectStack>(); old->items[0].enabled = true; old->items[0].size = 2;
        target.effects = old;
        target.mask = std::make_shared<c::LayerMask>(c::LayerMask{c::SelectionMask::filled({32,24},180),{},255,true});
        auto unrelated = raster("Unrelated");
        const auto sourceId = source.id, targetId = target.id, unrelatedId = unrelated.id;
        QVERIFY(original.insertLayer(0,target)); QVERIFY(original.insertLayer(1,unrelated)); QVERIFY(original.insertLayer(2,source));
        const auto path = directory.filePath("effects.vulkana");
        QVERIFY(u::saveProject(path,original));
        u::MainWindow window(nullptr,false,false); window.setUnsavedPromptEnabled(false);
        window.resize(1400,850); window.show(); QVERIFY(window.openImageFromPath(path));
        QTest::qWait(20);
        auto& session = const_cast<c::EditorSession&>(window.editorSession());
        auto& doc = *session.document();
        auto* view = dynamic_cast<u::LayerListView*>(window.findChild<QListView*>("LayerList"));
        QVERIFY(view);
        auto* model = dynamic_cast<u::LayerListModel*>(view->model()); QVERIFY(model);
        auto* copy = window.findChild<QAction*>("CopyLayerEffectsAction");
        auto* paste = window.findChild<QAction*>("PasteLayerEffectsAction");
        QVERIFY(copy && paste);
        const auto choose = [&](c::LayerId id, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            view->onRowSelectionRequested(model->rowForLayer(id), modifiers);
        };
        const auto menuAction = [&](c::LayerId id, const char* name) {
            const auto index = model->index(model->rowForLayer(id));
            view->scrollTo(index); QCoreApplication::processEvents();
            const auto point = view->visualRect(index).center();
            bool triggered = false;
            QTimer timer; timer.setSingleShot(true);
            connect(&timer,&QTimer::timeout,&window,[&] {
                if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close();
            });
            QTimer::singleShot(0,&window,[&] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) return;
                for (auto* action : menu->actions()) if (action->objectName()==name && action->isEnabled()) {
                    action->trigger(); triggered = true; break;
                }
                menu->close();
            });
            timer.start(2000);
            QContextMenuEvent event(QContextMenuEvent::Mouse,point,view->viewport()->mapToGlobal(point));
            QApplication::sendEvent(view->viewport(),&event);
            return triggered;
        };
        choose(sourceId); QVERIFY(!paste->isEnabled());
        const auto revision = doc.revision();
        QVERIFY(menuAction(sourceId,"CopyLayerEffectsAction"));
        QCOMPARE(doc.revision(),revision); QVERIFY(!doc.isModified());
        const auto encoded = QApplication::clipboard()->mimeData()->data(u::layerEffectClipboardMime);
        QVERIFY(u::decodeLayerEffectClipboard(encoded));
        QVERIFY(doc.setLayerEffects(sourceId, {})); // Clipboard is a frozen copy.
        choose(unrelatedId); choose(targetId, Qt::ControlModifier);
        const auto selection = session.layerSelectionState();
        const auto pixels = std::get<c::RasterLayer>(doc.layer(targetId)->payload).surface;
        const auto pixelRevision = pixels->revision(), depth = session.history().undoDepth();
        const auto before = *doc.layer(targetId);
        const auto pixelSelection = c::SelectionMask::rectangle({32,24},{1,2,7,9},128);
        doc.setSelection(pixelSelection);
        QVERIFY(menuAction(targetId,"PasteLayerEffectsAction"));
        QVERIFY(c::equivalentLayerEffects(doc.layer(targetId)->effects,copied));
        QVERIFY(!doc.layer(unrelatedId)->effects);
        QCOMPARE(session.layerSelectionState(),selection);
        QCOMPARE(doc.layer(targetId)->opacity,before.opacity); QCOMPARE(doc.layer(targetId)->fillOpacity,before.fillOpacity);
        QCOMPARE(doc.layer(targetId)->blendMode,before.blendMode); QVERIFY(doc.layer(targetId)->localToDocument==before.localToDocument);
        QCOMPARE(doc.layer(targetId)->mask,before.mask); QCOMPARE(doc.selection(),pixelSelection);
        QCOMPARE(pixels->revision(),pixelRevision); QCOMPARE(session.history().undoDepth(),depth+1);
        QVERIFY(session.undo()); QVERIFY(c::equivalentLayerEffects(doc.layer(targetId)->effects,before.effects));
        const auto redo = session.history().redoDepth();
        clipboard(u::encodeLayerEffectClipboard(before.effects));
        choose(targetId); paste->trigger(); QCOMPARE(session.history().redoDepth(),redo);
        QVERIFY(session.redo()); QVERIFY(c::equivalentLayerEffects(doc.layer(targetId)->effects,copied));
        clipboard(u::encodeLayerEffectClipboard({})); paste->trigger();
        QVERIFY(!c::hasActiveLayerEffects(doc.layer(targetId)->effects));
        QVERIFY(session.undo()); QVERIFY(c::equivalentLayerEffects(doc.layer(targetId)->effects,copied));
        clipboard("malformed"); QVERIFY(!paste->isEnabled());
        // Pasting the same visible effects onto a mirror still pins them as
        // overrides. Undo must restore the link, not only identical pixels.
        choose(targetId);
        QVERIFY(session.execute(c::createMirroredLayer(doc, session.layerSelectionState())));
        const auto mirrorId = *session.activeLayer();
        model->refresh(); choose(mirrorId);
        const auto inherited = doc.layer(mirrorId)->mirror;
        QVERIFY(inherited);
        const auto sameEffects = doc.layer(mirrorId)->effects;
        clipboard(u::encodeLayerEffectClipboard(sameEffects));
        const auto mirrorDepth = session.history().undoDepth();
        paste->trigger();
        QCOMPARE(session.history().undoDepth(), mirrorDepth+1);
        const auto* mirrored = doc.layer(mirrorId);
        QVERIFY(c::equivalentLayerEffects(mirrored->effects,sameEffects));
        for (const auto mode : mirrored->mirror->effects) QCOMPARE(mode,c::MirrorEffectMode::Override);
        QVERIFY(mirrored->mirror->pixelAlphaShapesEffectsOverride && mirrored->mirror->maskEffectsAfterRenderingOverride);
        QVERIFY(session.undo()); QCOMPARE(doc.layer(mirrorId)->mirror,inherited);
        QVERIFY(session.redo());
        for (const auto mode : doc.layer(mirrorId)->mirror->effects) QCOMPARE(mode,c::MirrorEffectMode::Override);
        window.close();
    }
};
QTEST_MAIN(LayerEffectClipboardTests)
#include "LayerEffectClipboardTests.moc"
