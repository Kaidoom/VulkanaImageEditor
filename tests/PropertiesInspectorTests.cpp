#include "imageeditor/ui/ColorDialog.hpp"
#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/LayerInfoPanel.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/ProjectFile.hpp"
#include "imageeditor/ui/PropertiesPanel.hpp"
#include "imageeditor/ui/ShapeOptionsPage.hpp"
#include "imageeditor/ui/Theme.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/ToolShortcutHints.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <iostream>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace {
int failures { };
void check(bool ok, const char* message, int line)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL " << line << ": " << message << '\n';
    }
}
#define CHECK(...) check(bool(__VA_ARGS__), #__VA_ARGS__, __LINE__)
void settle()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
    }
}
c::TextLayer mixedText()
{
    c::TextLayer text;
    text.utf8 = "Hello world";
    const auto families = QFontDatabase::families();
    auto first = text.defaultStyle;
    first.font.family
        = (families.contains("Noto Sans") ? QString("Noto Sans") : families.front()).toStdString();
    first.sizePixels = 24;
    first.color = { 240, 90, 30, 255 };
    auto second = first;
    second.font.family
        = (families.contains("Noto Serif") ? QString("Noto Serif") : families.back()).toStdString();
    second.sizePixels = 40;
    second.color = { 60, 80, 240, 255 };
    text.defaultStyle = first;
    text.runs = { { 0, 5, first }, { 5, 6, second } };
    return c::normalizedText(text);
}
QWidget* current(u::PropertiesPanel& panel) { return panel.findChild<QStackedWidget*>()->currentWidget(); }
void shapeGeometryFieldsFit()
{
    u::ShapeOptionsPage page;
    c::ShapeLayer shape;
    page.show();
    for (const double dimension : {444.19, 233.07, 900000.46, c::maximumShapeDimension}) {
        shape.size = {dimension, dimension};
        page.setShape(shape, true);
        page.adjustSize();
        settle();
        for (const auto* name : {"ShapeGeometryWidth", "ShapeGeometryHeight"}) {
            auto* field = page.findChild<QDoubleSpinBox*>(name);
            CHECK(field->width() == 165);
            auto* editor = field->findChild<QLineEdit*>();
            const auto margins = editor->textMargins();
            if (editor->fontMetrics().horizontalAdvance(editor->text()) + 4
                > editor->contentsRect().width() - margins.left() - margins.right())
                std::cerr << editor->text().toStdString() << ": needs "
                          << editor->fontMetrics().horizontalAdvance(editor->text()) + 4
                          << ", available " << editor->contentsRect().width() - margins.left() - margins.right()
                          << ", field hint " << field->sizeHint().width() << '\n';
            CHECK(editor->fontMetrics().horizontalAdvance(editor->text()) + 4
                <= editor->contentsRect().width() - margins.left() - margins.right());
        }
    }
    page.close();
}
void layout()
{
    u::PropertiesPanel panel;
    panel.resize(320, 800);
    panel.show();
    auto layer = c::Layer::text("Heading", mixedText());
    auto cache = std::make_shared<c::LayerRenderCache>();
    cache->logicalExtent = { 200, 50 };
    layer.renderCache = cache;
    layer.localToDocument = { 0, -2, 100, 2, 0, 20 };
    panel.setSelectedLayer(&layer);
    auto* font = dynamic_cast<u::FontFamilyPicker*>(panel.findChild<QComboBox*>("LayerInfoFont"));
    auto* size = dynamic_cast<u::ToolOptionsNumber*>(panel.findChild<QDoubleSpinBox*>("LayerInfoFontSize"));
    CHECK(font && size);
    size->setFocus(); QTest::keyClick(size, Qt::Key_Return); settle();
    CHECK(size->text().contains("Mixed"));
    CHECK(font->lineEdit()->text().contains("Mixed"));
    CHECK(size->text().contains("Mixed"));
    CHECK(panel.findChild<QPushButton*>("LayerInfoTextColor")->text() == "Mixed");
    CHECK(panel.findChild<QLabel*>("LayerInfoSize")->text() == "400.0 × 100.0 px");
    CHECK(panel.findChild<QLabel*>("LayerInfoAngle")->text() == "90.0°");
    auto* name = panel.findChild<QLabel*>("PropertiesTargetName");
    CHECK(name && name->text() == "Heading (Editable text layer)");
    CHECK(!panel.findChild<QLabel*>("LayerInfoType"));
    CHECK(!panel.findChild<QLabel*>("LayerInfoShape"));
    c::LayerContainer folder;
    folder.name = "Effects";
    panel.setSelectedLayer(nullptr, &folder);
    CHECK(name->text() == "Effects (Folder)");
    folder.kind = c::ContainerKind::Group;
    panel.setSelectedLayer(nullptr, &folder);
    CHECK(name->text() == "Effects (Group)");
    auto shape = c::Layer::shape("Badge", c::ShapeLayer{});
    panel.setSelectedLayer(&shape);
    CHECK(name->text() == "Badge (Editable shape layer)");
    CHECK(name->textFormat() == Qt::PlainText);
    panel.setSelectedLayer(nullptr);
    CHECK(name->text() == "No layer selected");
    panel.setSelectedLayer(&layer);
    CHECK(name->text() == "Heading (Editable text layer)");
    for (auto tool : { c::ToolId::Move, c::ToolId::Marquee, c::ToolId::Lasso, c::ToolId::Text,
             c::ToolId::Fill, c::ToolId::Eyedropper, c::ToolId::Transform }) {
        panel.setActiveTool(tool);
        settle();
        auto* page = current(panel);
        CHECK(page->findChild<QWidget*>("LayerInfoPanel"));
        CHECK(page->findChild<QWidget*>("ToolShortcutHints"));
        CHECK(page->findChild<QToolButton*>("LayerInfoSectionHeader")->isChecked());
        CHECK(page->findChild<QToolButton*>("ToolShortcutsSectionHeader")->isChecked());
        CHECK(page->findChildren<QLabel*>("MutedLabel").isEmpty());
        for (auto* label : page->findChildren<QLabel*>("ShortcutKeycap"))
            CHECK(!label->styleSheet().isEmpty());
    }
    panel.setActiveTool(c::ToolId::Brush);
    settle();
    CHECK(!current(panel)->findChild<QToolButton*>("LayerInfoSectionHeader")->isChecked());
    CHECK(!current(panel)->findChild<QToolButton*>("ToolShortcutsSectionHeader")->isChecked());
    panel.setActiveTool(c::ToolId::Eraser);
    CHECK(current(panel)->findChild<QLabel*>("ToolTitle")->text() == "Eraser");
    panel.setActiveTool(c::ToolId::Move);
    current(panel)->findChild<QToolButton*>("LayerInfoSectionHeader")->click();
    panel.setActiveTool(c::ToolId::Text);
    panel.setActiveTool(c::ToolId::Move);
    CHECK(!current(panel)->findChild<QToolButton*>("LayerInfoSectionHeader")->isChecked());
    current(panel)->findChild<QToolButton*>("LayerInfoSectionHeader")->click();
    auto bindings = u::defaultShortcutBindings();
    bindings["LayerTransformAction"] = { QKeySequence("Ctrl+Alt+T") };
    panel.setShortcutBindings(bindings);
    bool found = false;
    for (auto* cap : panel.findChildren<QLabel*>("ShortcutKeycap"))
        if (cap->property("shortcutAction") == "LayerTransformAction") {
            CHECK(cap->text() == "Ctrl+Alt+T");
            found = true;
        }
    CHECK(found);
    // Refresh while searching must not replace the user's query or focus.
    font->lineEdit()->setFocus();
    font->lineEdit()->selectAll();
    QTest::keyClicks(font->lineEdit(), "noto");
    settle();
    CHECK(font->searching());
    panel.setSelectedLayer(&layer);
    CHECK(font->lineEdit()->text() == "noto");
    font->finishSearch(false);
    if (const auto path = qEnvironmentVariable("VULKANA_PROPERTIES_SCREENSHOT"); !path.isEmpty()) {
        font->clearFocus();
        panel.setActiveTool(c::ToolId::Text);
        settle();
        CHECK(panel.grab().save(path));
        panel.setActiveTool(c::ToolId::Brush);
        settle();
        CHECK(panel.grab().save(path + ".brush.png"));
    }
    panel.close();
}
void subtleSectionHeaders(QApplication& app)
{
    u::PropertiesPanel panel;
    panel.resize(340, 700);
    panel.show();
    for (const auto preset : {u::ThemePreset::Dark, u::ThemePreset::Light}) {
        u::ThemeSettings settings;
        settings.preset = preset;
        u::applyEditorTheme(app, settings);
        for (const auto tool : {c::ToolId::Move, c::ToolId::Brush}) {
            panel.setActiveTool(tool);
            for (auto* header : current(panel)->findChildren<QToolButton*>()) {
                if (!header->property("propertiesSectionHeader").toBool() || !header->isVisible()) continue;
                header->setChecked(true);
                header->clearFocus();
                header->setAttribute(Qt::WA_UnderMouse, false);
                settle();
                const auto image = header->grab().toImage();
                const auto ratio = image.devicePixelRatio();
                const auto at = [&](int x, int y) { return image.pixelColor(int(x * ratio), int(y * ratio)); };
                if(at(header->width() - 20, header->height() / 2) != app.palette().color(QPalette::Button))
                    std::cerr << header->objectName().toStdString() << " background " << at(header->width() - 20, header->height() / 2).name().toStdString() << " app " << app.palette().color(QPalette::Button).name().toStdString() << " header " << header->palette().color(QPalette::Button).name().toStdString() << '\n';
                CHECK(at(header->width() - 20, header->height() / 2) == app.palette().color(QPalette::Button));
                CHECK(at(1, header->height() / 2) == header->palette().color(QPalette::Highlight));
                const auto size = header->size();
                header->setChecked(false);
                settle();
                CHECK(header->height() == size.height());
            }
        }
    }
    u::applyEditorTheme(app, u::ThemeSettings{});
    panel.close();
}
void editing()
{
    QTemporaryDir files;
    c::Document doc({ { 500, 300 } });
    auto layer = c::Layer::text("Mixed heading", mixedText());
    const auto id = layer.id;
    CHECK(doc.insertLayer(0, std::move(layer)));
    const auto path = files.filePath("text.vulkana");
    CHECK(u::saveProject(path, doc));
    u::MainWindow window(nullptr, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1500, 900);
    window.show();
    CHECK(window.openImageFromPath(path));
    settle();
    auto& session = const_cast<c::EditorSession&>(window.editorSession());
    auto* properties = dynamic_cast<u::PropertiesPanel*>(window.findChild<QWidget*>("PropertiesPanel"));
    if (!properties)
        for (auto* widget : window.findChildren<QWidget*>())
            if ((properties = dynamic_cast<u::PropertiesPanel*>(widget)))
                break;
    CHECK(properties);
    if (!properties)
        return;
    const auto original = std::get<c::TextLayer>(session.document()->layer(id)->payload);
    auto* font = dynamic_cast<u::FontFamilyPicker*>(window.findChild<QComboBox*>("LayerInfoFont"));
    auto* size = dynamic_cast<u::ToolOptionsNumber*>(window.findChild<QDoubleSpinBox*>("LayerInfoFontSize"));
    CHECK(font && size);
    QMetaObject::invokeMethod(size, "editingFinished");
    CHECK(std::get<c::TextLayer>(session.document()->layer(id)->payload) == original);
    const auto requested = original.runs.back().style.font.family;
    font->onFamilyChosen(QString::fromStdString(requested));
    settle();
    auto text = [&] { return std::get<c::TextLayer>(session.document()->layer(id)->payload); };
    for (const auto& run : text().runs) {
        CHECK(run.style.font.family == requested);
    }
    CHECK(text().runs[0].style.sizePixels == 24 && text().runs[1].style.sizePixels == 40);
    CHECK(session.history().undoDepth() == 1);
    window.findChild<QAction*>("UndoAction")->trigger();
    settle();
    CHECK(text() == original);
    window.findChild<QAction*>("RedoAction")->trigger();
    settle();
    size->setValue(32);
    QMetaObject::invokeMethod(size, "editingFinished");
    settle();
    for (const auto& run : text().runs)
        CHECK(run.style.sizePixels == 32);
    CHECK(session.history().undoDepth() == 2);
    CHECK(!size->text().contains("Mixed"));
    // Repeated acceptance creates no extra undo step and keeps the text itself.
    QMetaObject::invokeMethod(size, "editingFinished");
    CHECK(session.history().undoDepth() == 2);
    CHECK(text().utf8 == original.utf8 && text().paragraphs == original.paragraphs);
    // Dialog cancellation leaves mixed colors unchanged; accepting applies all.
    auto* color = window.findChild<QPushButton*>("LayerInfoTextColor");
    const auto before = text();
    QTimer::singleShot(0, [] {
        for (auto* widget : QApplication::allWidgets())
            if (auto* d = dynamic_cast<u::ColorDialog*>(widget)) {
                d->reject();
                return;
            }
    });
    color->click();
    settle();
    CHECK(text() == before);
    QTimer::singleShot(0, [] {
        for (auto* widget : QApplication::allWidgets())
            if (auto* d = dynamic_cast<u::ColorDialog*>(widget)) {
                d->setCurrentColor(QColor(12, 34, 56, 180));
                d->accept();
                return;
            }
    });
    color->click();
    settle();
    for (const auto& run : text().runs)
        CHECK(run.style.color == c::Rgba8(12, 34, 56, 180));
    window.findChild<QAction*>("UndoAction")->trigger();
    settle();
    CHECK(text() == before);
    window.findChild<QAction*>("ToolAction_shape")->trigger();
    settle();
    auto* width = window.findChild<QDoubleSpinBox*>("ShapeGeometryWidth");
    CHECK(width && width->isVisible());
    CHECK(window.findChild<QWidget*>("ShapeVertexEditorRetained")->isHidden());
    CHECK(!window.findChild<QWidget*>("ShapeVertexX")->isVisible());
    window.close();
    settle();
}
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ImageEditorTests");
    QCoreApplication::setApplicationName("PropertiesInspectorTests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    u::applyEditorTheme(app);
    shapeGeometryFieldsFit();
    layout();
    subtleSectionHeaders(app);
    editing();
    std::cout << (failures ? "Properties inspector FAILED\n" : "Properties inspector passed\n");
    return failures ? 1 : 0;
}
