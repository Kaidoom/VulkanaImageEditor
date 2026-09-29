#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/PdfImportDialog.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include <QApplication>
#include <QDir>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSettings>
#include <QTest>
#include <QTimer>
#include <QVulkanInstance>
#include <QElapsedTimer>
#include <QPdfWriter>
#include <QPainter>
#include <QScrollBar>
#include <vulkan/vulkan.h>
#include <iostream>
namespace u=imageeditor::ui;
namespace c=imageeditor::core;
int failures=0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { std::cerr<<"FAIL "<<__LINE__<<": " #__VA_ARGS__ "\n"; ++failures; } } while(false)
QString fixture(const char* file) { return QDir(qEnvironmentVariable("VULKANA_PDF_FIXTURES",IMAGEEDITOR_SOURCE_DIR "/tests/fixtures/pdf")).filePath(QString::fromLatin1(file)); }
struct Setup {
    int destination=0;
    QString pages="1-2";
    bool cancel=false,invalid=false;
    std::function<void(u::PdfImportDialog&)> inspect {};
    std::function<void(u::PdfImportDialog&)> inspectInitial {};
};
bool open(u::MainWindow& window, bool current, const Setup& setup={},const QString& path=fixture("pages.pdf")) {
    bool configured=false;int ticks=0;
    QTimer driver;driver.setInterval(20);
    QObject::connect(&driver,&QTimer::timeout,&window,[&] {
        auto* card=dynamic_cast<u::PdfImportDialog*>(window.findChild<QDialog*>("PdfImportDialog"));
        if(!card)return;
        if(++ticks>500) { CHECK(false);card->reject();return; }
        auto* submit=card->findChild<QPushButton*>("PdfImport");
        if(setup.invalid) {
            if(card->findChild<QLabel*>("PdfStatus")->text()!="Reading PDF…") { CHECK(!submit->isEnabled());card->reject(); }
            return;
        }
        if(configured||!submit->isEnabled())return;
        configured=true;
        CHECK(card->options().pages==std::vector<int>{0});
        CHECK(card->options().destination==(current?u::PdfDestination::CurrentDocument:u::PdfDestination::NewDocument));
        if(setup.inspectInitial)setup.inspectInitial(*card);
        auto* range=card->findChild<QLineEdit*>("PdfRange");
        range->setText("2-1");CHECK(!submit->isEnabled());
        range->clear();CHECK(!submit->isEnabled());
        range->setText(setup.pages);
        card->findChild<QDoubleSpinBox*>("PdfPpi")->setValue(72);
        card->findChild<QComboBox*>("PdfDestination")->setCurrentIndex(setup.destination);
        auto* list=card->findChild<QListWidget*>("PdfPages");
        CHECK(std::size_t(list->selectedItems().size())==card->options().pages.size());
        if(setup.inspect)setup.inspect(*card);
        if(setup.cancel) {card->reject();return;}
        CHECK(submit->isEnabled());submit->click();
    });
    driver.start();
    const bool result=current?window.importImageAsLayerFromPath(path):window.openImageFromPath(path);
    CHECK(configured||setup.invalid);
    return result;
}
void checkPageClicks(u::PdfImportDialog& card) {
    auto* pages=card.findChild<QListWidget*>("PdfPages");
    QElapsedTimer timer;timer.start();
    while(pages->item(1)->icon().isNull()&&timer.elapsed()<5000)QTest::qWait(20);
    CHECK(!pages->item(1)->icon().isNull());
    // The fixture has two equal-sized portrait pages. Click empty space first,
    // then each thumbnail's top/middle/bottom and all three caption lines.
    for(int page : {1,0}) {
        const auto rect=pages->visualItemRect(pages->item(page));
        for (int y : {5,70,140,150,170,190}) {
            const QPoint blank(pages->viewport()->width()-20,30);
            CHECK(!pages->indexAt(blank).isValid());
            QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::NoModifier,blank);
            CHECK(card.options().pages.empty());
            CHECK(!card.findChild<QPushButton*>("PdfImport")->isEnabled());
            QTest::qWait(20);
            const QPoint point(rect.center().x(),rect.top()+y);
            CHECK(pages->indexAt(point).row()==page);
            QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::NoModifier,point);
            CHECK(card.options().pages==std::vector<int>{page});
            CHECK(card.findChild<QLineEdit*>("PdfRange")->text()==QString::number(page+1));
            CHECK(card.findChild<QPushButton*>("PdfImport")->isEnabled());
        }
        // The slimmer painted highlight must not shrink the tile's hit area.
        for(int x : {rect.left()+1,rect.right()-1}) {
            pages->clearSelection();
            QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::NoModifier,QPoint(x,rect.center().y()));
            CHECK(card.options().pages==std::vector<int>{page});
        }
    }
    const auto point=pages->visualItemRect(pages->item(1)).center();
    QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::ControlModifier,point);
    CHECK(card.options().pages==std::vector<int>({0,1}));
    CHECK(card.findChild<QLineEdit*>("PdfRange")->text()=="1-2");
}
void checkPageGrid(u::PdfImportDialog& card) {
    auto* pages=card.findChild<QListWidget*>("PdfPages");
    CHECK(pages->count()>=4);
    for(int width : {620,660,780,620}) {
        card.resize(width,card.height());QTest::qWait(60);
        const auto first=pages->visualItemRect(pages->item(0));
        const auto second=pages->visualItemRect(pages->item(1));
        const auto third=pages->visualItemRect(pages->item(2));
        const auto fourth=pages->visualItemRect(pages->item(3));
        CHECK(first.top()==second.top());CHECK(second.top()==third.top());
        CHECK(first.right()<second.left());CHECK(second.right()<third.left());
        CHECK(third.right()<pages->viewport()->width());
        CHECK(fourth.top()>first.bottom());CHECK(fourth.left()==first.left());
        CHECK(pages->horizontalScrollBar()->maximum()==0);
        for(int i=0;i<3;++i) {
            const auto point=pages->visualItemRect(pages->item(i)).center();
            CHECK(pages->indexAt(point).row()==i);
            QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::NoModifier,point);
            CHECK(card.options().pages==std::vector<int>{i});
        }
    }
    // Offscreen rows remain selectable after scroll and lazy thumbnail loading.
    pages->scrollToItem(pages->item(3));QTest::qWait(100);
    const auto point=pages->visualItemRect(pages->item(3)).center();
    CHECK(pages->viewport()->rect().contains(point));
    QTest::mouseClick(pages->viewport(),Qt::LeftButton,Qt::NoModifier,point);
    CHECK(card.options().pages==std::vector<int>{3});
    pages->scrollToTop();
}
void tests(QVulkanInstance* instance) {
    u::MainWindow window(instance,false);window.setUnsavedPromptEnabled(false);window.resize(1250,850);window.show();
    imageeditor::render::CanvasWindow* canvas=nullptr;
    for(auto* native:QGuiApplication::allWindows())
        if(auto* candidate=dynamic_cast<imageeditor::render::CanvasWindow*>(native))canvas=candidate;
    if(instance) {
        QElapsedTimer timer;timer.start();
        while(canvas&&canvas->rendererStats().framesSubmitted==0&&timer.elapsed()<10000)QTest::qWait(20);
        CHECK(canvas&&canvas->rendererStats().framesSubmitted>0);
    }
    QString error;u::MainWindow::FileInteractions interactions;
    interactions.reportError=[&](const QString& text){error=text;};window.setFileInteractions(std::move(interactions));
    // A short PDF exposes stale icon-view hit regions: its thumbnails arrive
    // after the initial text-only layout, without another row forcing a layout.
    QTemporaryDir pickerFixture;
    const auto twoPages=pickerFixture.filePath("two-pages.pdf");
    {
        QPdfWriter writer(twoPages);writer.setPageSize(QPageSize(QPageSize::Letter));
        QPainter painter(&writer);
        painter.fillRect(100,100,200,200,Qt::red);CHECK(writer.newPage());
        painter.fillRect(100,100,200,200,Qt::blue);
    }
    CHECK(!open(window,false,{.cancel=true,.inspectInitial=checkPageClicks},twoPages));
    // Optional private reproduction input; never required by/publicly shipped
    // with the regression suite or copied into an application package.
    if(const auto reference=qEnvironmentVariable("VULKANA_PDF_TWO_PAGE_REFERENCE");!reference.isEmpty())
        CHECK(!open(window,false,{.cancel=true,.inspectInitial=checkPageClicks},reference));
    CHECK(open(window,false));CHECK(window.documentCount()==1);
    const auto first=window.activeDocumentId();
    auto& session=const_cast<c::EditorSession&>(window.editorSession());auto* document=session.document();
    CHECK(document->canvas().extent==c::Extent2u{120,160});CHECK(document->canvas().dotsPerInch==72);
    CHECK(document->layers().size()==2);CHECK(document->layers().front().name=="pages — Page 2");
    CHECK(document->layers().back().name=="pages — Page 1");CHECK(session.activeLayer()==document->layers().back().id);
    CHECK(window.documentContext(first)->projectPath.isEmpty());CHECK(document->isModified());
    const auto oldSelection=session.layerSelectionState();const auto oldRevision=document->revision();
    CHECK(!open(window,true,{.destination=2,.pages="1-4",.cancel=true}));
    CHECK(document->revision()==oldRevision);CHECK(session.layerSelectionState()==oldSelection);CHECK(session.history().undoDepth()==0);
    CHECK(open(window,true,{.destination=2,.pages="1, 3-4"}));CHECK(window.documentCount()==1);CHECK(document->layers().size()==5);
    CHECK(document->canvas().extent==c::Extent2u{120,160});CHECK(document->canvas().dotsPerInch==72);
    CHECK(session.history().undoDepth()==1);CHECK(session.selectedLayers().size()==3);
    const auto newSelection=session.layerSelectionState();const auto bytes=std::get<c::RasterLayer>(document->layers().back().payload).surface;
    CHECK(session.undo());CHECK(document->layers().size()==2);CHECK(session.layerSelectionState()==oldSelection);
    CHECK(session.redo());CHECK(session.layerSelectionState()==newSelection);CHECK(document->layers().size()==5);
    CHECK(std::get<c::RasterLayer>(document->layers().back().payload).surface==bytes);
    CHECK(open(window,false,{.destination=1,.pages="1-4"}));CHECK(window.documentCount()==5);
    const auto ids=window.documentIds();CHECK(window.activeDocumentId()==ids[1]);
    for(std::size_t i=1;i<ids.size();++i) {
        const auto* context=window.documentContext(ids[i]);CHECK(context&&context->projectPath.isEmpty());
        CHECK(context->session.document()->layers().size()==1);CHECK(context->session.history().undoDepth()==0);
        CHECK(context->session.document()->canvas().dotsPerInch==72);
        CHECK(context->displayName==QStringLiteral("pages — Page %1").arg(i));
    }
    CHECK(window.documentContext(ids[2])->session.document()->canvas().extent==c::Extent2u{80,160});
    // Modal ownership rejects tab changes/close rather than retargeting a pending import.
    CHECK(open(window,true,{.destination=2,.pages="2",.inspect=[&](u::PdfImportDialog& card) {
        CHECK(!window.activateDocument(first));CHECK(!window.closeDocument(ids[1]));
        CHECK(window.activeDocumentId()==ids[1]);
        QTest::qWait(200);
        auto* pages=card.findChild<QListWidget*>("PdfPages");
        CHECK(!pages->item(0)->icon().isNull());
        checkPageGrid(card);
        card.findChild<QLineEdit*>("PdfRange")->setText("2");
        card.grab().save(qEnvironmentVariable("VULKANA_PDF_UI_SCREENSHOT","/tmp/vulkana-pdf-import-ui.png"));
    }}));
    CHECK(window.documentContext(first)->session.document()->layers().size()==5);
    CHECK(window.documentContext(ids[1])->session.document()->layers().size()==2);
    const auto count=window.documentCount();
    CHECK(open(window,false,{.destination=0,.pages="3"}));CHECK(window.documentCount()==count+1);
    CHECK(open(window,false,{.destination=0,.pages="3"}));CHECK(window.documentCount()==count+2); // no PDF path deduplication
    const auto active=window.activeDocumentId();const auto depth=window.editorSession().history().undoDepth();
    CHECK(!open(window,false,{.invalid=true},fixture("malformed.pdf")));
    CHECK(window.activeDocumentId()==active);CHECK(window.editorSession().history().undoDepth()==depth);
    CHECK(window.documentCount()==count+2);
    // A late external mutation is detected even if a caller bypasses the UI guard.
    CHECK(!open(window,true,{.destination=2,.pages="1",.inspect=[&](u::PdfImportDialog&) {
        auto* d=const_cast<c::Document*>(window.editorSession().document());CHECK(d->setLayerOpacity(d->layers().back().id,0.5F));
    }}));CHECK(error.contains("destination changed"));
    CHECK(window.editorSession().document()->layers().size()==1);
    QTest::qWait(150);
    if(instance&&canvas) {
        const auto stats=canvas->rendererStats();
        CHECK(stats.framesSubmitted>0);CHECK(stats.maximumImageDimension2D>0);CHECK(stats.compositionError.empty());
        std::cout<<"GPU "<<stats.deviceName<<" frames "<<stats.framesSubmitted<<" max image "<<stats.maximumImageDimension2D<<'\n';
    }
}
int main(int argc,char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc,argv);QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    QCoreApplication::setOrganizationName("Vulkana-Pdf-Test");QCoreApplication::setApplicationName("PdfImport");
    u::applyEditorTheme(app,u::ThemeSettings{});
    QVulkanInstance vulkan;QVulkanInstance* instance=nullptr;int validationMessages=0;
    if(app.arguments().contains("--native")) {
        vulkan.setApiVersion(QVersionNumber(1,2));vulkan.setLayers({"VK_LAYER_KHRONOS_validation"});
        CHECK(vulkan.create());instance=&vulkan;
        vulkan.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
            QVulkanInstance::DebugMessageTypeFlags, const void* message) {
            if(flags & (QVulkanInstance::WarningSeverity | QVulkanInstance::ErrorSeverity)) {
                ++validationMessages;
                std::cerr<<static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message)->pMessage<<'\n';
            }
            return false;
        });
        std::cout<<"Native platform "<<QGuiApplication::platformName().toStdString()<<" requested Vulkan validation\n";
    }
    tests(instance);
    CHECK(validationMessages==0);
    if(instance)std::cout<<"Validation messages "<<validationMessages<<'\n';
    return failures?1:0;
}
