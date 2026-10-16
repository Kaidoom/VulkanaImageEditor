#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerListView.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerStructureCommands.hpp"
#include "LayerDragTestSupport.hpp"

#include <QApplication>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <iostream>
#include <memory>

namespace u = imageeditor::ui;
namespace c = imageeditor::core;
namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

struct Fixture {
    c::EditorSession session;
    u::LayerListModel model;
    u::LayerListView view;
    c::LayerId dragged {};
    int moves {};

    Fixture()
    {
        auto doc = std::make_unique<c::Document>(c::CanvasSpec{.extent={2,2}});
        for (int i=0; i<50; ++i) {
            auto layer = c::Layer::raster("Layer " + std::to_string(i),
                std::make_shared<c::ContiguousRasterSurface>(c::Extent2u{2,2}));
            dragged = layer.id;
            CHECK(doc->insertLayer(doc->layers().size(), std::move(layer)));
        }
        session.replaceDocument(std::move(doc));
        session.setActiveLayer(dragged);
        model.setSession(&session);
        model.onReparentRequested = [this](auto ids, auto destination) {
            auto tree = session.document()->tree();
            if (!tree.reparent(ids,destination)) return true;
            ++moves;
            const bool moved = session.execute(std::make_unique<c::LayerStructureCommand>("Move layers",
                *session.document(),std::move(tree),std::vector<c::LayerId>{},std::vector<c::Layer>{},
                session.layerSelectionState(),session.layerSelectionState()));
            model.refresh();
            return moved;
        };
        view.setModel(&model);
        view.setDragEnabled(true);
        view.setAcceptDrops(true);
        view.setDragDropMode(QAbstractItemView::DragDrop);
        view.setDefaultDropAction(Qt::MoveAction);
        view.setDragDropOverwriteMode(false);
        view.setEditTriggers(QAbstractItemView::NoEditTriggers);
        view.setSelectionMode(QAbstractItemView::ExtendedSelection);
        view.setIconSize({34,34});
        view.resize(360,380);
        view.show();
        CHECK(QTest::qWaitForWindowExposed(&view));
        QTest::qWait(50);
        view.selectionModel()->select(model.index(0), QItemSelectionModel::ClearAndSelect);
    }

    void mouse(QEvent::Type type, QPoint point, Qt::MouseButton button = Qt::NoButton)
    {
        QMouseEvent event(type, point, view.viewport()->mapToGlobal(point), button,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &event);
    }

    void wheel(QPoint point, int angle, int pixels = 0)
    {
        QWheelEvent event(point, view.viewport()->mapToGlobal(point), {0,pixels}, {0,angle},
            Qt::LeftButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view.viewport(), &event);
    }

    void drag(const std::function<void(QPoint)>& body)
    {
        QScopedValueRollback scripted(static_cast<LayerDragTestApplication*>(qApp)->scriptedDrag,true);
        const auto start = view.visualRect(model.index(0)).center();
        const QPoint position(start.x(), view.viewport()->height()/2);
        bool inMove = false, ran = false;
        QTimer work;
        work.setSingleShot(true);
        QObject::connect(&work, &QTimer::timeout, [&] {
            CHECK(inMove); // Exercise the actual startDrag loop, not just target events.
            ran = true;
            body(position);
            QTest::keyClick(view.viewport(), Qt::Key_Escape); // Safety if body didn't release.
        });
        mouse(QEvent::MouseButtonPress, start, Qt::LeftButton);
        work.start(30);
        inMove = true;
        mouse(QEvent::MouseMove, position);
        if (!ran) mouse(QEvent::MouseMove, position + QPoint(0,1));
        inMove = false;
        CHECK(ran);
        QTest::qWait(30);
        CHECK(QWidget::mouseGrabber() == nullptr);
        CHECK(QApplication::overrideCursor() == nullptr);
    }
};

void wheelAndDrop()
{
    Fixture f;
    c::LayerId target {};
    bool below {};
    f.drag([&](QPoint p) {
        const int before = f.view.verticalScrollBar()->value();
        f.wheel(p,-120);
        const int down = f.view.verticalScrollBar()->value();
        CHECK(down > before);
        f.wheel(p,120);
        CHECK(f.view.verticalScrollBar()->value() < down);
        for (int i=0; i<8; ++i) f.wheel(p,-120);
        const auto index = f.view.indexAt(p);
        CHECK(index.isValid() && index.row() > 10);
        target = f.model.layerIdAt(index.row()).value_or(0);
        below = p.y() >= f.view.visualRect(index).center().y();
        CHECK(f.moves == 0 && f.session.history().undoDepth() == 0);
        // No move after scrolling: release must use the row now under the pointer.
        f.mouse(QEvent::MouseButtonRelease,p,Qt::LeftButton);
    });
    CHECK(f.moves == 1);
    CHECK(f.model.rowForLayer(f.dragged) == f.model.rowForLayer(target) + (below ? 1 : -1));
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(f.session.undo());
    f.model.refresh();
    CHECK(f.model.rowForLayer(f.dragged) == 0);
    CHECK(f.session.redo());
    f.model.refresh();
    CHECK(f.model.rowForLayer(f.dragged) > 10);
}

void pixelScrollingAndCancel()
{
    Fixture f;
    f.view.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    f.drag([&](QPoint p) {
        f.wheel(p,0,-37);
        CHECK(f.view.verticalScrollBar()->value() == 37);
        f.wheel(p,0,13);
        CHECK(f.view.verticalScrollBar()->value() == 24);
        QTest::keyClick(f.view.viewport(), Qt::Key_Escape);
    });
    CHECK(f.moves == 0 && f.session.history().undoDepth() == 0);
    CHECK(f.model.rowForLayer(f.dragged) == 0);
    const int before = f.view.verticalScrollBar()->value();
    f.wheel(f.view.viewport()->rect().center(), -120);
    CHECK(f.view.verticalScrollBar()->value() > before); // No lingering filter/grab.
}

void edgeScrollingStillWorks()
{
    Fixture f;
    f.drag([&](QPoint p) {
        p.setY(f.view.viewport()->height()-3);
        f.mouse(QEvent::MouseMove,p);
        QTest::qWait(900);
        CHECK(f.view.verticalScrollBar()->value() > 0);
        f.mouse(QEvent::MouseMove,f.view.viewport()->rect().center());
        QTest::qWait(200);
        const int stopped = f.view.verticalScrollBar()->value();
        QTest::qWait(300);
        CHECK(f.view.verticalScrollBar()->value() == stopped);
        QTest::keyClick(f.view.viewport(),Qt::Key_Escape);
    });
    CHECK(f.moves == 0);
}

void leaveReenterAndHide()
{
    Fixture f;
    f.drag([&](QPoint p) {
        const QPoint outside(-40,-40);
        f.mouse(QEvent::MouseMove,outside);
        f.wheel(outside,-120);
        CHECK(f.view.verticalScrollBar()->value() == 0);
        f.mouse(QEvent::MouseMove,p);
        f.wheel(p,-120);
        CHECK(f.view.verticalScrollBar()->value() > 0);
        f.view.hide();
    });
    CHECK(f.moves == 0 && f.session.history().undoDepth() == 0);
}

void multipleLayersIntoFolder()
{
    Fixture f;
    const auto second = f.model.layerIdAt(1).value();
    const auto folder = c::makeLayerId();
    auto tree = f.session.document()->tree();
    tree.containers.push_back({folder,"Destination folder",c::ContainerKind::Folder,c::ColorLabel::None,{}});
    tree.roots.insert(tree.roots.begin()+10,folder);
    CHECK(f.session.execute(std::make_unique<c::LayerStructureCommand>("Folder",*f.session.document(),std::move(tree))));
    f.session.history().clear();
    const std::array selected{f.dragged,second};
    f.session.setLayerSelection(selected,f.dragged,f.dragged);
    f.model.refresh();
    f.view.selectionModel()->select(f.model.index(0),QItemSelectionModel::ClearAndSelect);
    f.view.selectionModel()->select(f.model.index(1),QItemSelectionModel::Select);
    f.view.onRowSelectionRequested=[](int,Qt::KeyboardModifiers){};
    f.drag([&](QPoint p) {
        const auto row=f.model.index(f.model.rowForLayer(folder));
        for(int i=0;i<30 && !f.view.viewport()->rect().adjusted(0,25,0,-25).contains(f.view.visualRect(row).center());++i)
            f.wheel(p,-120);
        const auto center=f.view.visualRect(row).center();
        CHECK(f.view.viewport()->rect().contains(center));
        f.mouse(QEvent::MouseMove,center);
        f.mouse(QEvent::MouseButtonRelease,center,Qt::LeftButton);
    });
    CHECK(f.moves==1 && f.session.history().undoDepth()==1);
    CHECK(f.session.document()->tree().placement(f.dragged)->parent==folder);
    CHECK(f.session.document()->tree().placement(second)->parent==folder);
    CHECK(f.session.selectedLayers().size()==2);
    CHECK(f.session.undo());
    CHECK(f.session.document()->tree().placement(f.dragged)->parent==0);
    CHECK(f.session.document()->tree().placement(second)->parent==0);
}

void noOpAndDeactivation()
{
    Fixture f;
    f.drag([&](QPoint) {
        const auto r=f.view.visualRect(f.model.index(0));
        const auto p=QPoint(r.center().x(),r.top()+2);
        f.mouse(QEvent::MouseButtonRelease,p,Qt::LeftButton);
    });
    CHECK(f.moves==0 && f.session.history().undoDepth()==0);
    f.drag([&](QPoint p) {
        f.wheel(p,-120);
        QEvent event(QEvent::ApplicationDeactivate);
        QApplication::sendEvent(qApp,&event);
    });
    CHECK(f.moves==0 && f.session.history().undoDepth()==0);
}
}

int main(int argc,char** argv)
{
    LayerDragTestApplication app(argc,argv);
    wheelAndDrop();
    pixelScrollingAndCancel();
    edgeScrollingStillWorks();
    leaveReenterAndHide();
    multipleLayersIntoFolder();
    noOpAndDeactivation();
    if (!failures) std::cout << "Layer drag wheel, pixel scroll, edge scroll, drop, undo and cancellation passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
