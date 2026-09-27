#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/NewDocumentDialog.hpp"
#include "imageeditor/ui/LayerListModel.hpp"
#include "imageeditor/ui/LayerTransferMimeData.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/CrossWindowPointerRouter.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QAction>
#include <QApplication>
#include <QImage>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QFile>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QDragMoveEvent>
#include <QMouseEvent>
#include <iostream>
namespace u=imageeditor::ui;
namespace c=imageeditor::core;
int failures=0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while(false)

void checkDragInteractions(const QString& imagePath, const QString& projectPath)
{
    u::MainWindow window(nullptr,false);
    window.setUnsavedPromptEnabled(false);window.resize(1300,850);window.show();
    CHECK(window.openImageFromPath(imagePath));
    const auto first=window.activeDocumentId();
    CHECK(window.openImageFromPath(imagePath));
    const auto second=window.activeDocumentId();
    auto* tabs=window.findChild<QTabBar*>("DocumentTabs");
    u::CrossWindowPointerRouter* router=nullptr;
    for(auto* object:window.children())if(auto* r=dynamic_cast<u::CrossWindowPointerRouter*>(object))router=r;
    CHECK(router);
    QTest::qWait(25);
    // Deliver through QWidgetWindow, including Qt's implicit press owner, not
    // just moveTab(). Activating the tab must not cancel its new pointer capture.
    const auto dragInactive=[&](int from,int to) {
        const auto id=tabs->tabData(from).toULongLong();
        CHECK(id!=window.activeDocumentId());
        const auto press=tabs->mapToGlobal(tabs->tabRect(from).center());
        const auto end=tabs->mapToGlobal(tabs->tabRect(to).center());
        const auto mouse=[&](QEvent::Type type,QPoint global,Qt::MouseButton button,Qt::MouseButtons buttons) {
            auto* host=window.windowHandle();
            const auto local=host->mapFromGlobal(global);
            QMouseEvent event(type,local,local,global,button,buttons,Qt::NoModifier);
            QApplication::sendEvent(host,&event);
        };
        mouse(QEvent::MouseButtonPress,press,Qt::LeftButton,Qt::LeftButton);
        CHECK(window.activeDocumentId()==id);
        CHECK(router&&router->captureOwner()==tabs);
        mouse(QEvent::MouseMove,press+(end-press)/3,Qt::NoButton,Qt::LeftButton);
        mouse(QEvent::MouseMove,end,Qt::NoButton,Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease,end,Qt::LeftButton,Qt::NoButton);
        QTest::qWait(200); // Qt's tab-slide animation finishes before next press.
        CHECK(tabs->tabData(to).toULongLong()==id);
        CHECK(window.documentIds()[std::size_t(to)]==id);
        CHECK(router&&router->captureDomain()==u::CrossWindowPointerRouter::CaptureDomain::None);
        CHECK(window.editorSession().history().undoDepth()==0);
    };
    dragInactive(0,1);
    CHECK(window.activateDocument(second));
    dragInactive(1,0);
    CHECK(window.documentIds().front()==first);

    auto* workspace=dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    imageeditor::render::CanvasWindow* canvas=nullptr;
    for(auto* native:QGuiApplication::allWindows())
        if(auto* candidate=dynamic_cast<imageeditor::render::CanvasWindow*>(native))
            for(auto* owner=candidate->parent();owner;owner=owner->parent())
                if(owner==window.windowHandle())canvas=candidate;
    CHECK(workspace&&canvas);
    QMimeData files;files.setUrls({QUrl::fromLocalFile(imagePath)});
    const auto sendDrop=[&](QObject* target,const QMimeData& mime) {
        QDragEnterEvent enter({4,4},Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(target,&enter);
        QDragMoveEvent move({4,4},Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(target,&move);
        QDropEvent drop(QPointF(4,4),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(target,&drop);
        return enter.isAccepted()&&move.isAccepted()&&drop.isAccepted()&&drop.dropAction()==Qt::CopyAction;
    };
    auto* newAction=window.findChild<QAction*>("NewDocumentAction");
    CHECK(newAction);
    // Entire application, including the native canvas and toolbar carrier.
    // Each image opens a tab; the formerly active document must not be imported into.
    for(int recipient=0;recipient<6;++recipient) {
        const auto count=window.documentCount();
        const auto* original=window.editorSession().document();
        const auto revision=original->revision();
        QTimer::singleShot(0,&window,[&] {
            auto* card=window.findChild<QDialog*>("NewDocumentDialog");CHECK(card);
            QObject* target=recipient==0?card->parentWidget():recipient==1?static_cast<QObject*>(canvas)
                :recipient==2?window.windowHandle():recipient==3?tabs
                :recipient==4?workspace->panelOverlay()->windowHandle():static_cast<QObject*>(card);
            CHECK(sendDrop(target,files));
            if(card->isVisible())card->reject(); // Bound a failing regression, never hang exec().
        });
        newAction->trigger();
        if(window.documentCount()!=count+1)std::cerr<<"New Canvas drop recipient "<<recipient<<" did not open a document\n";
        CHECK(window.documentCount()==count+1);
        CHECK(original->revision()==revision&&original->layers().size()==1);
    }
    // Ordered mixed project/image drops use Open's project de-duplication rule.
    const auto count=window.documentCount();
    QMimeData mixed;mixed.setUrls({QUrl::fromLocalFile(projectPath),QUrl::fromLocalFile(imagePath)});
    QTimer::singleShot(0,&window,[&] {
        auto* card=window.findChild<QDialog*>("NewDocumentDialog");CHECK(sendDrop(canvas,mixed));
        if(card->isVisible())card->reject();
    });
    newAction->trigger();CHECK(window.documentCount()==count+2);
    const auto ids=window.documentIds();
    CHECK(window.documentContext(ids[ids.size()-2])->projectPath==projectPath);
    CHECK(window.activeDocumentId()==ids.back());
    QMimeData project;project.setUrls({QUrl::fromLocalFile(projectPath)});
    QTimer::singleShot(0,&window,[&] {
        auto* card=window.findChild<QDialog*>("NewDocumentDialog");CHECK(sendDrop(tabs,project));
        if(card->isVisible())card->reject();
    });
    newAction->trigger();CHECK(window.documentCount()==count+2);
    CHECK(window.activeDocumentId()==ids[ids.size()-2]);

    // No remote URL download; Resize remains modal and never opens documents.
    QMimeData remote;remote.setUrls({QUrl("https://example.invalid/image.png")});
    QTimer::singleShot(0,&window,[&] {
        auto* card=window.findChild<QDialog*>("NewDocumentDialog");
        CHECK(!sendDrop(card->parentWidget(),remote));CHECK(card->isVisible());card->reject();
    });
    newAction->trigger();CHECK(window.documentCount()==count+2);
    QTimer::singleShot(0,&window,[&] {
        auto* card=window.findChild<QDialog*>("NewDocumentDialog");
        sendDrop(canvas,files);CHECK(card->isVisible());card->reject();
    });
    window.findChild<QAction*>("ChangeCanvasSizeAction")->trigger();
    CHECK(window.documentCount()==count+2);
    // With no starter overlay, the same native canvas drop still imports a layer.
    const auto layers=window.editorSession().document()->layers().size();
    sendDrop(canvas,files);
    CHECK(window.documentCount()==count+2);
    CHECK(window.editorSession().document()->layers().size()==layers+1);
    window.close();
}
int main(int argc,char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName("ImageEditorTests");app.setApplicationName("DocumentTabs");
    QStandardPaths::setTestModeEnabled(true);QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,dir.path());
    u::applyEditorTheme(app);
    u::MainWindow window(nullptr,false);window.setUnsavedPromptEnabled(false);window.resize(1300,850);window.show();
    auto* tabs=window.findChild<QTabBar*>("DocumentTabs");CHECK(tabs&&window.documentCount()==1);
    QImage image(40,30,QImage::Format_RGBA8888);image.fill(Qt::red);const auto path=dir.filePath("same.png");CHECK(image.save(path));
    CHECK(window.openImageFromPath(path));CHECK(window.documentCount()==1);
    auto first=window.activeDocumentId();
    auto* firstDocument=window.editorSession().document();
    CHECK(window.openImageFromPath(path));CHECK(window.documentCount()==2);
    auto second=window.activeDocumentId();CHECK(first!=second);
    QTest::qWait(25);
    auto* panel=window.findChild<QWidget*>("DocumentTabsPanel");
    auto* workspace=dynamic_cast<u::OverlayDockWorkspace*>(window.findChild<QWidget*>("CanvasWorkspace"));
    CHECK(panel&&workspace);
    CHECK(tabs->parentWidget()==panel&&panel->parentWidget()==window.centralWidget());
    CHECK(!workspace->isAncestorOf(tabs)); // Never lives over the native canvas.
    CHECK(panel->height()==33&&panel->width()==window.centralWidget()->width());
    CHECK(panel->geometry().bottom()+1==workspace->geometry().top());
    CHECK(workspace->rulerContentRect().top()==0);
    CHECK(workspace->rulerStrip(Qt::Horizontal)->y()==0);
    CHECK(workspace->canvasContainer()->geometry()==workspace->rect());
    const auto workspaceGeometry=workspace->geometry();
    // The whole empty part of the row is opaque, with independent active and
    // inactive document colors (not the generic adjustment-category tab roles).
    u::ThemeSettings custom;custom.preset=u::ThemePreset::Custom;
    custom.custom[std::size_t(u::ThemeColor::Surface)]=QColor("#193147");
    custom.custom[std::size_t(u::ThemeColor::DocumentTabActive)]=QColor("#32714a");
    custom.custom[std::size_t(u::ThemeColor::DocumentTabInactive)]=QColor("#573961");
    u::applyEditorTheme(app,custom);QTest::qWait(10);
    auto row=panel->grab().toImage();
    const auto sample=[&](QPoint p){return row.pixelColor(qRound(p.x()*row.devicePixelRatio()),qRound(p.y()*row.devicePixelRatio()));};
    CHECK(sample(QPoint(panel->width()-3,5))==QColor("#193147"));
    for(int i=0;i<tabs->count();++i)CHECK(sample(tabs->mapTo(panel,tabs->tabRect(i).topLeft()+QPoint(8,5)))
        ==(i==tabs->currentIndex()?QColor("#32714a"):QColor("#573961")));
    u::applyEditorTheme(app,u::ThemeSettings());QTest::qWait(10);
    CHECK(workspace->geometry()==workspaceGeometry);
    if(const auto image=qEnvironmentVariable("IMAGEEDITOR_TABS_PANEL_REVIEW");!image.isEmpty())CHECK(panel->grab().save(image));
    imageeditor::render::CanvasWindow* canvas=nullptr;
    for(auto* q:QGuiApplication::allWindows())if(auto* c=dynamic_cast<imageeditor::render::CanvasWindow*>(q))canvas=c;
    CHECK(canvas);
    c::ViewportState firstView;firstView.setZoom(2.25);firstView.setPan({17.5,-23});
    CHECK(window.activateDocument(first));canvas->restoreDocumentView(firstView,{});
    const auto selection=c::SelectionMask::rectangle({40,30},{2,3,8,7});
    CHECK(const_cast<c::Document*>(window.editorSession().document())->setSelection(selection));
    CHECK(window.activateDocument(second));CHECK(!window.editorSession().document()->selection());
    c::ViewportState secondView;secondView.setZoom(.625);secondView.setPan({-40,12});canvas->restoreDocumentView(secondView,{});
    CHECK(window.activateDocument(first));CHECK(window.editorSession().document()==firstDocument);
    CHECK(canvas->zoom()==firstView.zoom()&&canvas->scene().viewport.pan()==firstView.pan());
    CHECK(window.editorSession().document()->selection()==selection);
    auto& session=const_cast<c::EditorSession&>(window.editorSession());
    CHECK(session.execute(std::make_unique<c::SetLayerOpacityCommand>(*session.activeLayer(),0.5f)));
    const auto depth=session.history().undoDepth();
    CHECK(window.activateDocument(second));CHECK(window.editorSession().history().undoDepth()==0);
    CHECK(window.activateDocument(first));CHECK(window.editorSession().history().undoDepth()==depth);
    CHECK(window.editorSession().document()->layer(*window.editorSession().activeLayer())->opacity==0.5f);
    CHECK(window.activateDocument(second));CHECK(canvas->zoom()==secondView.zoom()&&canvas->scene().viewport.pan()==secondView.pan());
    CHECK(workspace->geometry()==workspaceGeometry);
    CHECK(window.activateDocument(first));
    u::MainWindow::FileInteractions hooks;
    hooks.chooseSavePath=[] {return QString{};};
    hooks.reportError=[](const QString&){};
    window.setFileInteractions(hooks);
    CHECK(!window.saveDocument(true));
    const auto project=dir.filePath("same.vulkana");
    hooks.chooseSavePath=[&]{return project;};window.setFileInteractions(hooks);
    CHECK(window.saveDocument(true));
    CHECK(window.activateDocument(second));CHECK(window.openImageFromPath(project));
    CHECK(window.activeDocumentId()==first&&window.documentCount()==2);
    CHECK(window.activateDocument(second));CHECK(!window.saveDocument(true)); // Owned by first.
    CHECK(!window.openImageFromPath(dir.filePath("missing.png")));CHECK(window.documentCount()==2);
    tabs->moveTab(0,1);CHECK(window.documentIds().front()==second);
    CHECK(workspace->geometry()==workspaceGeometry);
    CHECK(window.closeDocument(first));CHECK(window.activeDocumentId()==second&&window.documentCount()==1);
    QTimer::singleShot(0,&window,[&] {
        if(auto* dialog=window.findChild<QDialog*>("NewDocumentDialog"))dialog->reject();
    });
    CHECK(window.closeDocument(second));CHECK(window.documentCount()==0&&window.activeDocumentId()==0);
    // Close-final queues New Canvas; Cancel must keep the welcome state empty.
    QTimer dismiss;
    QObject::connect(&dismiss,&QTimer::timeout,&window,[&]{if(auto* dialog=window.findChild<QDialog*>("NewDocumentDialog"))dialog->reject();});
    dismiss.start(5);QTest::qWait(25);dismiss.stop();
    CHECK(!window.editorSession().document());CHECK(window.openImageFromPath(path));
    CHECK(workspace->geometry()==workspaceGeometry&&panel->isVisible());
    CHECK(window.documentCount()==1&&window.activeDocumentId()!=first);
    // A copy of the saved project has identical serialized local IDs/revisions,
    // but is an independent runtime document (not a path/index identity).
    CHECK(QFile::copy(project,dir.filePath("other.vulkana")));
    CHECK(window.openImageFromPath(project)); const auto projectId=window.activeDocumentId();
    const auto originalLayer=*window.editorSession().activeLayer();
    CHECK(window.openImageFromPath(dir.filePath("other.vulkana"))); const auto targetId=window.activeDocumentId();
    CHECK(*window.editorSession().activeLayer()==originalLayer);CHECK(targetId!=projectId);
    CHECK(window.activateDocument(projectId));
    u::LayerListModel* model=nullptr;
    for(auto* candidate:window.findChildren<QAbstractItemModel*>()) if(auto* layers=dynamic_cast<u::LayerListModel*>(candidate)) model=layers;
    CHECK(model);
    const auto sourceHistory=window.editorSession().history().undoDepth();
    const auto sourceRevision=window.editorSession().document()->revision();
    const auto sourceSurface=c::intrinsicSurface(*window.editorSession().document()->layer(originalLayer));
    auto payload=std::unique_ptr<QMimeData>(model->mimeData({model->index(0,0)}));CHECK(payload);
    // Capture precedes activation. Destination model must never reinterpret the
    // source layer ID as its own identically numbered layer.
    CHECK(window.activateDocument(targetId));
    CHECK(model->canDropMimeData(payload.get(),Qt::CopyAction,-1,0,{}));
    CHECK(model->dropMimeData(payload.get(),Qt::CopyAction,-1,0,{}));
    CHECK(window.editorSession().document()->layers().size()==2);
    const auto copied=*window.editorSession().activeLayer();CHECK(copied!=originalLayer);
    const auto copiedSurface=c::intrinsicSurface(*window.editorSession().document()->layer(copied));
    CHECK(copiedSurface.get()!=sourceSurface.get());CHECK(copiedSurface->id()!=sourceSurface->id());
    CHECK(window.editorSession().history().undoDepth()==1);
    CHECK(!model->dropMimeData(payload.get(),Qt::CopyAction,-1,0,{})); // Single-use copy.
    CHECK(window.activateDocument(projectId));
    CHECK(window.editorSession().document()->revision()==sourceRevision);
    CHECK(window.editorSession().history().undoDepth()==sourceHistory);
    CHECK(window.activateDocument(targetId));
    CHECK(const_cast<c::EditorSession&>(window.editorSession()).undo());
    CHECK(window.editorSession().document()->layers().size()==1);
    CHECK(const_cast<c::EditorSession&>(window.editorSession()).redo());
    CHECK(window.editorSession().document()->layers().size()==2);
    // Closed source invalidates even a captured payload; it must not fall back
    // to same-document ID reordering after the source owner disappears.
    CHECK(window.activateDocument(projectId));
    auto cancelled=std::unique_ptr<QMimeData>(model->mimeData({model->index(0,0)}));
    CHECK(window.activateDocument(targetId));CHECK(window.closeDocument(projectId));
    CHECK(!model->canDropMimeData(cancelled.get(),Qt::MoveAction,-1,0,{}));
    // Tab drop opens rather than importing into the active canvas.
    QMimeData files;files.setUrls({QUrl::fromLocalFile(path)});
    QTest::qWait(25); // Commit the queued overlay/input-region layout before mapping a drop.
    const auto beforeOpen=window.documentCount();
    QDragEnterEvent enter(tabs->tabRect(0).center(),Qt::CopyAction,&files,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(tabs,&enter);CHECK(enter.isAccepted());
    QDropEvent drop(QPointF(tabs->tabRect(0).center()),Qt::CopyAction,&files,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(tabs,&drop);CHECK(drop.isAccepted());CHECK(window.documentCount()==beforeOpen+1);
    // Hover only changes the view. Drop contributes exactly one destination command.
    const auto hoverSource=window.activeDocumentId();
    auto hover=std::unique_ptr<QMimeData>(model->mimeData({model->index(0,0)}));
    QTest::qWait(25);
    int targetIndex=-1;for(int i=0;i<tabs->count();++i)if(tabs->tabData(i).toULongLong()==targetId)targetIndex=i;
    CHECK(targetIndex>=0);const auto tabPoint=tabs->tabRect(targetIndex).center();
    const auto targetDepth=window.documentContext(targetId)->session.history().undoDepth();
    QDragEnterEvent hoverEnter(tabPoint,Qt::CopyAction,hover.get(),Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(tabs,&hoverEnter);CHECK(hoverEnter.isAccepted());
    QTest::qWait(100);CHECK(window.activeDocumentId()==hoverSource);
    QTest::qWait(450);CHECK(window.activeDocumentId()==targetId);
    CHECK(window.editorSession().history().undoDepth()==targetDepth);
    QDropEvent transferDrop(QPointF(tabPoint),Qt::CopyAction,hover.get(),Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(tabs,&transferDrop);CHECK(transferDrop.isAccepted());
    CHECK(window.editorSession().history().undoDepth()==targetDepth+1);
    if(auto* close=tabs->tabButton(targetIndex,QTabBar::RightSide))CHECK(tabs->tabRect(targetIndex).right()-close->geometry().right()>=6);
    // Completed typing is committed when switching, not silently cancelled.
    u::TextController* editor=nullptr;for(auto* q:window.children())if(auto* t=dynamic_cast<u::TextController*>(q))editor=t;
    auto& current=const_cast<c::EditorSession&>(window.editorSession());
    c::TextLayer text;text.utf8="Original";auto letters=c::Layer::text("Typing",text);const auto textId=letters.id;
    CHECK(current.document()->insertLayer(current.document()->layers().size(),letters));
    CHECK(editor&&editor->editLayer(textId));
    QKeyEvent end(QEvent::KeyPress,Qt::Key_End,Qt::ControlModifier);editor->keyEvent(&end,false);
    QKeyEvent type(QEvent::KeyPress,Qt::Key_Exclam,Qt::NoModifier,"!");editor->keyEvent(&type,false);
    QTest::keyClick(canvas,Qt::Key_Tab,Qt::ControlModifier);
    CHECK(window.activeDocumentId()!=targetId);CHECK(!editor->active());
    CHECK(window.activateDocument(hoverSource));
    CHECK(std::get<c::TextLayer>(window.documentContext(targetId)->session.document()->layer(textId)->payload).utf8=="Original!");
    // Quit decisions are preflighted across all tabs before any are destroyed.
    window.setUnsavedPromptEnabled(true);
    int decisions=0;hooks.askUnsaved=[&]{return ++decisions==1?u::MainWindow::UnsavedChoice::Discard:u::MainWindow::UnsavedChoice::Cancel;};
    window.setFileInteractions(hooks);
    CHECK(current.execute(std::make_unique<c::SetLayerOpacityCommand>(textId,.6f)));
    auto& other=const_cast<c::EditorSession&>(window.editorSession());CHECK(other.execute(std::make_unique<c::SetLayerOpacityCommand>(*other.activeLayer(),.4f)));
    const auto countBeforeQuit=window.documentCount();CHECK(!window.close());CHECK(decisions==2&&window.documentCount()==countBeforeQuit);
    CHECK(window.documentContext(targetId)->session.document()->isModified());
    window.setUnsavedPromptEnabled(false);window.close();
    checkDragInteractions(path,project);
    std::cout<<"Document tabs: "<<failures<<" failures\n";
    return failures?1:0;
}
