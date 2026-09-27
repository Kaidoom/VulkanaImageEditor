#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/core/LayerCommands.hpp"
#include "imageeditor/core/SelectionMask.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/TextClipboard.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"

#include "imageeditor/ui/FontFamilyPicker.hpp"
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace c = imageeditor::core;
namespace u = imageeditor::ui;
namespace r = imageeditor::render;
namespace {
int failures = 0;
void check(bool value, const char* expression, int line)
{
    if (!value) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string bytes(const char8_t* value) { return reinterpret_cast<const char*>(value); }
c::TextLayer text(std::string value, double size = 24)
{
    c::TextLayer result;
    result.utf8 = std::move(value);
    result.defaultStyle.font = { "Noto Sans", "Regular", 400, false };
    result.defaultStyle.sizePixels = size;
    result.defaultStyle.color = { 40, 80, 160, 191 };
    return c::normalizedText(std::move(result));
}

struct Fixture {
    c::EditorSession session;
    r::CanvasWindow* canvas = new r::CanvasWindow;
    std::unique_ptr<u::OverlayDockWorkspace> workspace;
    std::unique_ptr<u::TextController> controller;
    c::LayerId rasterId { };
    QStringList errors;
    int notifications { };
    Fixture()
    {
        auto doc = std::make_unique<c::Document>(c::CanvasSpec { .extent = { 512, 512 } });
        auto raster = c::Layer::raster(
            "Base", std::make_shared<c::ContiguousRasterSurface>(c::Extent2u { 512, 512 }));
        rasterId = raster.id;
        CHECK(doc->insertLayer(0, std::move(raster)));
        session.replaceDocument(std::move(doc));
        session.setActiveTool(c::ToolId::Text);
        canvas->resize(1000, 700);
        canvas->setDocument(session.document()->snapshot(), false);
        workspace = std::make_unique<u::OverlayDockWorkspace>(canvas);
        workspace->resize(1000, 700);
        workspace->panelOverlay()->resize(1000, 700);
        controller = std::make_unique<u::TextController>(session, canvas, workspace.get(), nullptr);
        controller->onActivateText = [this] { session.setActiveTool(c::ToolId::Text); };
        controller->onChanged = [this] { ++notifications; };
        controller->onError = [this](const QString& message) { errors << message; };
        canvas->onTextEvent = [this](QEvent* e) { return controller->inputEvent(e); };
        // This suite exercises real controller/layout/controls but never exposes
        // the Vulkan window. Native surface validation belongs to its own probe.
        CHECK(!canvas->isExposed());
    }
    ~Fixture()
    {
        canvas->onTextEvent = { };
        controller->finish();
        controller.reset();
        workspace.reset(); // QWidget window-container owns canvas.
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    template <typename T> T* widget(const char* name)
    {
        auto* result = dynamic_cast<T*>(
            workspace->panelOverlay()->findChild<QWidget*>(QString::fromLatin1(name)));
        if (!result)
            throw std::runtime_error(std::string("Missing control: ") + name);
        return result;
    }
    c::LayerId add(c::TextLayer value, c::AffineTransform transform = { 1, 0, 30, 0, 1, 80 })
    {
        auto layer = c::Layer::text("Existing text", std::move(value));
        const auto id = layer.id;
        layer.localToDocument = transform;
        CHECK(
            session.document()->insertLayer(session.document()->layers().size(), std::move(layer)));
        controller->prepareCaches();
        controller->refreshPresentation();
        return id;
    }
    const c::TextLayer& content(c::LayerId id = 0) const
    {
        const auto* layer = session.document()->layer(id ? id : controller->layerId());
        if (!layer)
            throw std::runtime_error("Expected committed text layer");
        return std::get<c::TextLayer>(layer->payload);
    }
    bool key(int key, Qt::KeyboardModifiers modifiers = { }, QString characters = { },
        bool owned = false)
    {
        QKeyEvent event(QEvent::KeyPress, key, modifiers, characters);
        return controller->keyEvent(&event, owned);
    }
    void type(QString value) { CHECK(key(0, { }, std::move(value))); }
    void select(int start, int end)
    {
        CHECK(key(Qt::Key_Home, Qt::ControlModifier));
        for (int i = 0; i < start; ++i)
            CHECK(key(Qt::Key_Right));
        for (int i = start; i < end; ++i)
            CHECK(key(Qt::Key_Right, Qt::ShiftModifier));
    }
    void patchSize(double size, std::uint64_t key = 0)
    {
        c::TextStylePatch patch;
        patch.sizePixels = size;
        controller->patchStyle(patch, key);
    }
    void begin(c::Vec2d position = { 30, 80 })
    {
        CHECK(controller->press(position, { }, false));
        CHECK(controller->active());
    }
    void preedit(QString value, QList<QInputMethodEvent::Attribute> attributes = { })
    {
        QInputMethodEvent event(value, attributes);
        CHECK(controller->inputEvent(&event));
        CHECK(event.isAccepted());
    }
    void commit(QString value, int start = 0, int length = 0)
    {
        QInputMethodEvent event;
        event.setCommitString(value, start, length);
        CHECK(controller->inputEvent(&event));
    }
    QVariant query(Qt::InputMethodQuery query)
    {
        QInputMethodQueryEvent event(query);
        CHECK(controller->inputEvent(&event));
        return event.value(query);
    }
};

void configurableTextCommandsPreserveTyping()
{
    Fixture f;
    const auto id = f.add(text("abc"));
    auto bindings = u::defaultShortcutBindings();
    u::assignShortcut(bindings, "FinishTextAction", QKeySequence("Ctrl+F6"), true);
    u::assignShortcut(bindings, "UndoAction", QKeySequence("Ctrl+F10"), true);
    u::assignShortcut(bindings, "RedoAction", QKeySequence("Ctrl+Shift+F10"), true);
    u::assignShortcut(bindings, "LayerTransformAction", QKeySequence("Ctrl+F8"), true);
    f.controller->setShortcutBindings(bindings);
    f.begin(); f.key(Qt::Key_End, Qt::ControlModifier);
    CHECK(f.key(Qt::Key_Return)); CHECK(f.controller->active());
    CHECK(f.content(id).utf8 == "abc\n");
    f.key(Qt::Key_Z, Qt::ControlModifier); CHECK(f.content(id).utf8 == "abc\n"); // No old-key fallback.
    f.key(Qt::Key_F10, Qt::ControlModifier); CHECK(f.content(id).utf8 == "abc");
    f.key(Qt::Key_F10, Qt::ControlModifier | Qt::ShiftModifier); CHECK(f.content(id).utf8 == "abc\n");
    CHECK(!f.key(Qt::Key_F8, Qt::ControlModifier)); // Shared Transform is passed to the editor.
    CHECK(f.key(Qt::Key_F6, Qt::ControlModifier)); CHECK(!f.controller->active());
    CHECK(f.widget<QToolButton>("TextDone")->toolTip().contains("Ctrl+F6"));
    f.begin(); f.key(Qt::Key_Escape); CHECK(!f.controller->active());
}

void creationAndAbandonedPreedit()
{
    Fixture f;
    c::EditorColors colors;
    colors.primary = { 15, 90, 171, 133 };
    colors.secondary = { 211, 30, 40, 255 };
    colors.active = c::ColorSlot::Secondary;
    f.session.setColors(colors);
    const auto revision = f.session.document()->revision();
    f.begin({ 37.25, 88.5 });
    CHECK(f.session.document()->layers().size() == 1);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.controller->insertionStyle().color == colors.secondary);
    CHECK(!f.widget<QWidget>("TextContextOverlay")->isHidden());
    f.controller->finish();
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.activeLayer() == f.rasterId);
    CHECK(!f.controller->active());

    f.begin();
    f.preedit(QString::fromUtf8("候補"), { { QInputMethodEvent::Cursor, 1, 1, { } } });
    CHECK(f.session.document()->layers().size() == 1);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.canvas->scene().document.layersBottomToTop.size() == 2); // Disposable preview only.
    CHECK(f.key(Qt::Key_Escape));
    CHECK(f.controller->active());
    CHECK(f.canvas->scene().document.layersBottomToTop.size() == 1);
    CHECK(f.key(Qt::Key_Escape));
    CHECK(!f.controller->active());
    CHECK(f.session.document()->layers().size() == 1);

    f.begin({ 37.25, 88.5 });
    const auto id = f.controller->layerId();
    f.type(QStringLiteral("Hi"));
    CHECK(f.session.document()->layers().size() == 2);
    CHECK(f.session.activeLayer() == id);
    CHECK(f.content().utf8 == "Hi");
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(f.session.document()->layer(id)->localToDocument.m02 == 37.25);
    CHECK(f.session.document()->layer(id)->localToDocument.m12 == 88.5);
    CHECK(f.content().runs.front().style.color == colors.secondary);
    f.controller->history(false);
    CHECK(f.controller->active());
    CHECK(!f.session.document()->containsLayer(id));
    CHECK(f.session.activeLayer() == f.rasterId);
    f.controller->history(true);
    CHECK(f.controller->active());
    CHECK(f.content().utf8 == "Hi");
    CHECK(f.session.activeLayer() == id);
    CHECK(f.session.colors() == colors);
    CHECK(f.errors.empty());
}

void insertionSelectedRangesAndMixedControls()
{
    Fixture f;
    const auto rasterSelection = c::SelectionMask::rectangle({ 512, 512 }, { 100, 100, 10, 20 });
    CHECK(f.session.document()->setSelection(rasterSelection));
    const auto id = f.add(text("old", 9));
    f.begin();
    CHECK(f.session.activeLayer() == id);
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    const auto baseline = f.content();
    const auto revision = f.session.document()->revision();
    f.patchSize(20);
    CHECK(f.controller->insertionStyle().sizePixels == 20);
    CHECK(f.content() == baseline);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().undoDepth() == 0);
    f.controller->refreshPresentation();
    f.controller->prepareCaches();
    f.type(QStringLiteral("NEW"));
    CHECK(f.content().utf8 == "oldNEW");
    CHECK(c::textStyleAt(f.content(), 0, false).sizePixels == 9);
    CHECK(c::textStyleAt(f.content(), 3, false).sizePixels == 20);
    CHECK(c::textStyleAt(f.content(), 3, false).color == baseline.defaultStyle.color);
    CHECK(c::textStyleAt(f.content(), 3, false).font == baseline.defaultStyle.font);

    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    CHECK(f.controller->cursor().hasSelection());
    auto* sizeLine = f.widget<u::ToolOptionsNumber>("TextSize")->findChild<QLineEdit*>();
    CHECK(sizeLine->text().contains(QStringLiteral("Mixed"))
        || (sizeLine->text().isEmpty()
            && sizeLine->placeholderText().contains(QStringLiteral("Mixed"))));
    if (!sizeLine->text().contains(QStringLiteral("Mixed")) && !sizeLine->text().isEmpty())
        std::cerr << "  mixed size display: " << sizeLine->text().toStdString() << '\n';
    const auto selectionAnchor = f.controller->cursor().anchor();
    const auto selectionPosition = f.controller->cursor().position();
    c::TextStylePatch color;
    color.color = c::Rgba8 { 220, 12, 70, 88 };
    f.controller->patchStyle(color);
    CHECK(f.controller->cursor().anchor() == selectionAnchor);
    CHECK(f.controller->cursor().position() == selectionPosition);
    CHECK(c::textStyleAt(f.content(), 0, false).sizePixels == 9);
    CHECK(c::textStyleAt(f.content(), 3, false).sizePixels == 20);
    CHECK(c::textStyleAt(f.content(), 0, false).color == *color.color);

    f.select(1, 2);
    f.patchSize(31);
    CHECK(c::textStyleAt(f.content(), 0, false).sizePixels == 9);
    CHECK(c::textStyleAt(f.content(), 1, false).sizePixels == 31);
    CHECK(c::textStyleAt(f.content(), 2, false).sizePixels == 9);
    f.select(0, 0);
    CHECK(f.controller->insertionStyle().sizePixels == 9);
    CHECK(f.key(Qt::Key_Right));
    CHECK(f.controller->insertionStyle().sizePixels == 9); // Boundary: preceding character.
    CHECK(f.key(Qt::Key_Right));
    CHECK(f.controller->insertionStyle().sizePixels == 31);
    CHECK(f.session.document()->selection() == rasterSelection);
    CHECK(f.errors.empty());
}

void paragraphAlignmentAndMixedFormats()
{
    Fixture f;
    f.add(text("first\nsecond\n", 18));
    f.begin();
    const auto originalRuns = f.content().runs;
    f.select(6, 12);
    f.widget<QToolButton>("TextAlignCenter")->click();
    CHECK(f.content().paragraphs[0].alignment == c::TextAlignment::Left);
    CHECK(f.content().paragraphs[1].alignment == c::TextAlignment::Center);
    CHECK(f.content().paragraphs[2].alignment == c::TextAlignment::Left);
    CHECK(f.content().runs == originalRuns);
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    CHECK(!f.widget<QToolButton>("TextAlignLeft")->isChecked());
    CHECK(!f.widget<QToolButton>("TextAlignCenter")->isChecked());
    CHECK(!f.widget<QToolButton>("TextAlignRight")->isChecked());
    f.select(0, 5);
    c::TextStylePatch patch;
    patch.family = "Requested Missing Test Family 91524";
    patch.style = "Custom style";
    patch.weight = 700;
    patch.italic = true;
    patch.color = c::Rgba8 { 220, 40, 80, 128 };
    f.controller->patchStyle(patch);
    CHECK(c::textStyleAt(f.content(), 0).font.family == *patch.family);
    CHECK(c::textStyleAt(f.content(), 6, false).font.family == "Noto Sans");
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    CHECK(f.widget<u::FontFamilyPicker>("TextFamily")
            ->currentText()
            .contains(QStringLiteral("Mixed")));
    CHECK(f.widget<QComboBox>("TextStyle")->currentIndex() == -1);
    CHECK(f.widget<QPushButton>("TextColor")->text() == QStringLiteral("Mixed"));
    CHECK(f.widget<QToolButton>("TextBold")->text().contains(QChar(0x00b7)));
    CHECK(f.widget<QToolButton>("TextItalic")->text().contains(QChar(0x00b7)));
    CHECK(f.errors.empty());
}

void popupsAndIndependentColors()
{
    Fixture f;
    f.add(text("neighbor", 9));
    f.begin();
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    f.patchSize(20);
    const auto before = f.content();
    const auto colors = f.session.colors();
    const auto cursor = f.controller->cursor();
    auto* family = f.widget<u::FontFamilyPicker>("TextFamily");
    QFocusEvent focusIn(QEvent::FocusIn, Qt::MouseFocusReason);
    QApplication::sendEvent(family, &focusIn);
    family->showPopup();
    QApplication::processEvents();
    CHECK(f.controller->active());
    family->hidePopup();
    QApplication::processEvents();
    QFocusEvent focusOut(QEvent::FocusOut, Qt::PopupFocusReason);
    QApplication::sendEvent(family, &focusOut);
    f.controller->refreshPresentation();
    CHECK(f.controller->insertionStyle().sizePixels == 20);
    CHECK(f.controller->cursor().position() == cursor.position());
    CHECK(f.content() == before);

    f.widget<QPushButton>("TextColor")->click();
    QApplication::processEvents();
    auto* dialog = f.workspace->findChild<QColorDialog*>(QStringLiteral("TextColorDialog"));
    if (!dialog)
        dialog = f.workspace->panelOverlay()->findChild<QColorDialog*>(
            QStringLiteral("TextColorDialog"));
    CHECK(dialog);
    if (dialog) {
        const QColor chosen(3, 177, 91, 119);
        dialog->setCurrentColor(chosen);
        dialog->accept();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        CHECK(f.controller->active());
        CHECK(f.controller->insertionStyle().sizePixels == 20);
        CHECK(f.controller->insertionStyle().color == c::Rgba8({ 3, 177, 91, 119 }));
    }
    CHECK(f.content() == before);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.session.colors() == colors);
    f.session.setForegroundColor({ 211, 199, 18, 255 });
    CHECK(f.content() == before);
    f.type(QStringLiteral("x"));
    CHECK(c::textStyleAt(f.content(), 8, false).sizePixels == 20);
    CHECK(c::textStyleAt(f.content(), 8, false).color == c::Rgba8({ 3, 177, 91, 119 }));
    CHECK(c::textStyleAt(f.content(), 0, false).color == before.defaultStyle.color);
    CHECK(f.errors.empty());
}

void historyTypingNumericAndRedo()
{
    Fixture f;
    f.add(text("a", 9));
    f.begin();
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    f.type(QStringLiteral("b"));
    f.type(QStringLiteral("c"));
    f.type(QStringLiteral("d"));
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    f.patchSize(20);
    CHECK(f.session.history().undoDepth() == 2);
    f.controller->history(false);
    CHECK(f.content().utf8 == "abcd");
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 9);
    CHECK(f.controller->cursor().hasSelection());
    const auto revision = f.session.document()->revision();
    f.patchSize(9);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().redoDepth() == 1);
    f.controller->history(true);
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 20);

    auto* size = f.widget<u::ToolOptionsNumber>("TextSize");
    const auto depth = f.session.history().undoDepth();
    for (int i = 0; i < 3; ++i) {
        QKeyEvent step(QEvent::KeyPress, Qt::Key_Up, { }, { }, i > 0);
        QApplication::sendEvent(size, &step);
    }
    CHECK(size->interactionActive());
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Up, { });
    QApplication::sendEvent(size, &release);
    CHECK(!size->interactionActive());
    CHECK(f.session.history().undoDepth() == depth + 1);
    // Native spinbox auto-repeat may synthesize an additional initial repeat;
    // the contract here is one grouped adjustment using the displayed value.
    CHECK(size->value() > 20);
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == size->value());
    f.controller->history(false);
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 20);
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    const auto afterUndoRevision = f.session.document()->revision();
    f.patchSize(30); // Insertion style does not clear numeric redo.
    CHECK(f.session.document()->revision() == afterUndoRevision);
    CHECK(f.session.history().redoDepth() == 1);
    f.type(QStringLiteral("!"));
    CHECK(f.session.history().redoDepth() == 0);
    CHECK(c::textStyleAt(f.content(), 4, false).sizePixels == 30);
    CHECK(f.key(Qt::Key_Z, Qt::ControlModifier));
    CHECK(f.content().utf8 == "abcd");
    CHECK(f.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier));
    CHECK(f.content().utf8 == "abcd!");
    CHECK(f.errors.empty());
}

void unicodeClipboardAndInputOwnership()
{
    Fixture f;
    f.add(text(bytes(u8"Ae\u0301👩‍💻Z"), 24));
    f.begin();
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    CHECK(f.key(Qt::Key_Left));
    CHECK(f.key(Qt::Key_Backspace));
    CHECK(f.content().utf8 == bytes(u8"Ae\u0301Z"));
    CHECK(f.key(Qt::Key_Backspace));
    CHECK(f.content().utf8 == "AZ");
    f.controller->history(false);
    CHECK(f.content().utf8
        == bytes(u8"Ae\u0301👩‍💻Z")); // Consecutive grapheme deletions group.
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    CHECK(f.key(Qt::Key_C, Qt::ControlModifier));
    CHECK(QApplication::clipboard()->text() == QString::fromUtf8(f.content().utf8));
    CHECK(f.key(Qt::Key_X, Qt::ControlModifier));
    CHECK(f.content().utf8.empty());
    CHECK(f.key(Qt::Key_V, Qt::ControlModifier));
    CHECK(f.content().utf8 == bytes(u8"Ae\u0301👩‍💻Z"));
    CHECK(f.key(Qt::Key_Return, { }, QStringLiteral("\r")));
    f.type(QStringLiteral("B E T Space"));
    CHECK(f.content().utf8.ends_with("\nB E T Space"));
    CHECK(f.session.activeTool() == c::ToolId::Text);
    const auto before = f.content();
    CHECK(!f.key(Qt::Key_B, { }, QStringLiteral("b"), true));
    CHECK(f.content() == before);
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_A, Qt::ControlModifier);
    CHECK(f.controller->keyEvent(&shortcut, false));
    CHECK(shortcut.isAccepted());
    CHECK(f.errors.empty());
}

void compositionQueriesAndFocusLoss()
{
    Fixture f;
    const auto id = f.add(text(bytes(u8"A😀B"), 24));
    f.begin();
    CHECK(f.key(Qt::Key_Home, Qt::ControlModifier));
    CHECK(f.key(Qt::Key_Right));
    CHECK(f.key(Qt::Key_Right));
    CHECK(f.controller->cursor().position() == 3);
    const auto before = f.content();
    const auto revision = f.session.document()->revision();
    f.preedit(QString::fromUtf8("候補"), { { QInputMethodEvent::Cursor, 1, 1, { } } });
    CHECK(f.content() == before);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.query(Qt::ImEnabled).toBool());
    CHECK(f.query(Qt::ImCursorPosition).toInt() == 3);
    CHECK(f.query(Qt::ImSurroundingText).toString() == QString::fromUtf8(before.utf8));
    CHECK(f.query(Qt::ImTextBeforeCursor).toString() == QString::fromUtf8("A😀"));
    CHECK(f.query(Qt::ImTextAfterCursor).toString() == QStringLiteral("B"));
    const auto candidate = f.query(Qt::ImCursorRectangle).toRectF();
    CHECK(candidate.isValid());
    CHECK(std::isfinite(candidate.x()) && std::isfinite(candidate.y()));
    f.commit(QStringLiteral("x"), -2, 2); // UTF16 replacement removes the complete surrogate pair.
    CHECK(f.content().utf8 == "AxB");
    CHECK(f.session.history().undoDepth() == 1);
    f.controller->history(false);
    CHECK(f.content() == before);
    f.controller->history(true);
    CHECK(f.content().utf8 == "AxB");

    const auto committed = f.content();
    const auto depth = f.session.history().undoDepth();
    QFocusEvent focusLost(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(f.canvas, &focusLost);
    QEvent deactivate(QEvent::ApplicationDeactivate);
    CHECK(!f.controller->inputEvent(&deactivate));
    QApplication::sendEvent(qApp, &deactivate);
    CHECK(f.controller->active());
    CHECK(f.controller->layerId() == id);
    CHECK(f.content() == committed);
    CHECK(f.session.history().undoDepth() == depth);
    f.controller->finish();
    CHECK(f.content(id) == committed);
    CHECK(!f.query(Qt::ImEnabled).toBool());
    CHECK(f.errors.empty());
}

void inputMethodReplacementNeverSplitsScalars()
{
    Fixture f;
    f.add(text(bytes(u8"A😀B"), 24));
    f.begin();
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    // An IME/platform replacement range can straddle a surrogate interior.
    // Resolve it to complete scalars rather than exporting a broken surrogate
    // as '?' or U+FFFD into the authoritative UTF-8 document.
    f.commit(QStringLiteral("x"), -2, 1);
    CHECK(f.content().utf8 == "AxB");
    if (f.content().utf8 != "AxB")
        std::cerr << "  scalar replacement output: " << f.content().utf8 << '\n';
    CHECK(f.errors.empty());
}

void transformedTargetingAndOverlayCacheReuse()
{
    Fixture f;
    const c::AffineTransform transform { 0, -1.2, 200, -1.5, 0, 220 };
    const auto lower = f.add(text("O   O", 24), transform);
    const auto higher = f.add(text("O   O", 24), transform);
    const auto localPoint = c::Vec2d { 10, 10 };
    f.begin(transform.map(localPoint));
    CHECK(f.controller->layerId() == higher);
    CHECK(f.session.activeLayer() == higher);
    const auto* layer = f.session.document()->layer(higher);
    const auto cache = layer->renderCache;
    const auto revision = layer->textRevision;
    const auto documentRevision = f.session.document()->revision();
    f.controller->drag(transform.map({ 40, 10 }));
    CHECK(f.controller->cursor().hasSelection());
    CHECK(!f.canvas->scene().textQuads.empty());
    f.controller->refreshPresentation();
    f.controller->prepareCaches();
    f.controller->viewportChanged();
    CHECK(layer->renderCache == cache);
    CHECK(layer->textRevision == revision);
    CHECK(f.session.document()->revision() == documentRevision);
    CHECK(layer->localToDocument == transform);
    CHECK(f.widget<QWidget>("TextContextOverlay")->parentWidget() == f.workspace->panelOverlay());
    const auto overlayRect = f.widget<QWidget>("TextContextOverlay")->geometry();
    CHECK(f.workspace->panelOverlay()->rect().contains(overlayRect));
    CHECK(f.query(Qt::ImCursorRectangle).toRectF().isValid());
    f.controller->finish();
    CHECK(f.session.document()->setLayerVisibility(higher, false));
    f.session.setActiveTool(c::ToolId::Move);
    CHECK(f.controller->press(transform.map(localPoint), { }, true));
    CHECK(f.controller->layerId() == lower);
    CHECK(f.session.activeTool() == c::ToolId::Text);
    CHECK(f.errors.empty());
}

void documentGridCacheTracksTransformAndFilterSource()
{
    Fixture f;
    f.canvas->resetTo100Percent();
    const c::AffineTransform transform { .8, -.35, 170.25, .25, 1.1, 140.5 };
    const auto id = f.add(text("Native AA", 23), transform);
    auto* layer = f.session.document()->layer(id);
    const auto native = layer->renderCache;
    CHECK(native && native->rasterizedDocumentTransform == transform);
    const auto nativeMapping = c::renderTransform(*layer);
    CHECK(nativeMapping.m00 == 1 && nativeMapping.m11 == 1
        && nativeMapping.m01 == 0 && nativeMapping.m10 == 0);
    CHECK(nativeMapping.m02 == std::floor(nativeMapping.m02)
        && nativeMapping.m12 == std::floor(nativeMapping.m12));
    auto localReference = *layer;
    localReference.renderCache = u::QtTextLayout(f.content(id)).rasterize(1);
    CHECK(c::layerSourceBounds(*layer) == c::layerSourceBounds(localReference));
    f.controller->prepareCaches();
    CHECK(layer->renderCache == native);

    auto translated = transform;
    translated.m02 += 17;
    translated.m12 -= 9;
    CHECK(f.session.document()->setLayerTransform(id, translated));
    f.controller->prepareCaches();
    CHECK(layer->renderCache->surface == native->surface);
    CHECK(layer->renderCache->rasterizedDocumentTransform == translated);
    CHECK(layer->renderCache->documentOrigin == native->documentOrigin + c::Vec2d(17, -9));

    auto moved = transform;
    moved.m02 += .375;
    CHECK(f.session.document()->setLayerTransform(id, moved));
    f.controller->prepareCaches();
    CHECK(layer->renderCache != native && layer->renderCache->rasterizedDocumentTransform == moved);

    auto filters = std::make_shared<c::SpatialFilterStack>();
    filters->items[0].enabled = true;
    filters->items[0].parameters = c::GaussianBlurParameters { 2, 2 };
    CHECK(f.session.document()->setLayerFilters(id, filters));
    f.controller->prepareCaches();
    const auto filteredSource = layer->renderCache;
    CHECK(filteredSource && !filteredSource->rasterizedDocumentTransform
        && filteredSource->density == 1);
    CHECK(filteredSource->pixelsToLocal.m00 == 1 && filteredSource->pixelsToLocal.m11 == 1
        && filteredSource->pixelsToLocal.m01 == 0 && filteredSource->pixelsToLocal.m10 == 0);
    QWheelEvent wheel(QPointF(300, 300), QPointF(300, 300), {}, QPoint(0, 600),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(f.canvas, &wheel);
    CHECK(f.canvas->zoom() > 1);
    f.controller->prepareCaches();
    CHECK(layer->renderCache == filteredSource);
    CHECK(f.errors.empty());
}

void editOnlyClickMarginAndOutsideCompletion()
{
    for (const auto transform : { c::AffineTransform { 1, 0, 160, 0, 1, 180 },
             c::AffineTransform { 0, -1.2, 300, -1.5, 0, 300 } }) {
        for (int zoomDelta : { -600, 0, 600 }) {
            Fixture f;
            const auto id = f.add(text("O  O", 24), transform);
            f.canvas->resetTo100Percent();
            QWheelEvent wheel(QPointF(300, 300), QPointF(300, 300), {}, QPoint(0, zoomDelta),
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(f.canvas, &wheel);
            f.controller->prepareCaches();
            const auto cache = f.session.document()->layer(id)->renderCache;
            const auto original = f.content(id);
            u::QtTextLayout metric(original);
            const auto r = metric.bounds();
            const auto inverse = *transform.inverted();
            const auto xUnit = std::hypot(inverse.m00, inverse.m01) / f.canvas->zoom();
            const auto yUnit = std::hypot(inverse.m10, inverse.m11) / f.canvas->zoom();
            for (int edge = 0; edge < 4; ++edge) {
                CHECK(f.controller->editLayer(id));
                auto point = [&](double pixels) {
                    c::Vec2d p { r.center().x(), r.center().y() };
                    if (edge == 0) p.x = r.left() - pixels * xUnit;
                    if (edge == 1) p.x = r.right() + pixels * xUnit;
                    if (edge == 2) p.y = r.top() - pixels * yUnit;
                    if (edge == 3) p.y = r.bottom() + pixels * yUnit;
                    return transform.map(p);
                };
                CHECK(!c::hitTextBounds(*f.session.document()->layer(id), point(29)));
                CHECK(f.controller->press(point(29), {}, false));
                CHECK(f.controller->layerId() == id);
                CHECK(f.content(id) == original);
                CHECK(f.session.document()->layer(id)->renderCache == cache);
                CHECK(!f.controller->press(point(31), {}, false));
                CHECK(!f.controller->active());
                CHECK(f.session.document()->layers().size() == 2);
                CHECK(f.session.history().undoDepth() == 0);
            }
            CHECK(f.errors.empty());
        }
    }

    Fixture f;
    const auto a = f.add(text("First"));
    const auto b = f.add(text("Second"), { 1, 0, 360, 0, 1, 360 });
    CHECK(f.controller->editLayer(a));
    // Character selection may still drag outside the margin under capture.
    f.controller->drag({ 450, 450 });
    CHECK(f.controller->active());
    CHECK(!f.controller->press({ 365, 365 }, {}, false));
    CHECK(!f.controller->active() && f.session.activeLayer() == a);
    CHECK(f.controller->press({ 365, 365 }, {}, false));
    CHECK(f.controller->layerId() == b); // Only the second click retargets.
    CHECK(!f.controller->press({ -100, -100 }, {}, false));
    CHECK(!f.controller->active()); // Gray workspace also commits.
    CHECK(f.session.document()->layers().size() == 3);
    CHECK(f.session.history().undoDepth() == 0);

    f.begin({ 200, 200 });
    const auto provisional = f.controller->layerId();
    CHECK(f.controller->press({ 190, 195 }, {}, false));
    CHECK(f.controller->layerId() == provisional); // Empty/provisional safe area.
    CHECK(!f.controller->press({ 480, 100 }, {}, false));
    CHECK(!f.controller->active());
    CHECK(f.session.document()->layers().size() == 3);
    CHECK(f.session.history().undoDepth() == 0);
    f.begin({ 200, 200 });
    f.type(QStringLiteral("temporary"));
    f.select(0, 9);
    CHECK(f.key(Qt::Key_Backspace));
    CHECK(!f.controller->press({ 480, 100 }, {}, false));
    CHECK(!f.controller->active());
    CHECK(f.session.document()->layers().size() == 3);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.errors.empty());
}

void numericRoundTripAndCancellationPreserveRedo()
{
    Fixture f;
    f.add(text("abc", 24));
    f.begin();
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    f.patchSize(27);
    f.controller->history(false);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.session.history().redoDepth() == 1);
    const auto original = f.content();
    const auto retained = f.session.history().memoryUsed();
    const auto anchor = f.controller->cursor().anchor();
    const auto position = f.controller->cursor().position();
    f.patchSize(25, 123);
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 25);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.session.history().redoDepth() == 1);
    f.patchSize(24, 123);
    f.controller->finishNumeric();
    CHECK(f.content() == original);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.session.history().redoDepth() == 1);
    CHECK(f.session.history().memoryUsed() == retained);

    f.patchSize(35, 124);
    f.controller->finishNumeric(true);
    CHECK(f.content() == original);
    CHECK(f.controller->cursor().anchor() == anchor);
    CHECK(f.controller->cursor().position() == position);
    CHECK(f.session.history().undoDepth() == 0);
    CHECK(f.session.history().redoDepth() == 1);
    CHECK(f.session.history().memoryUsed() == retained);
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    const auto revision = f.session.document()->revision();
    f.patchSize(40, 125);
    CHECK(f.controller->insertionStyle().sizePixels == 40);
    f.controller->finishNumeric(true);
    CHECK(f.controller->insertionStyle().sizePixels == 24);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().redoDepth() == 1);
    CHECK(f.errors.empty());
}

void provisionalAlignmentSurvivesFirstCommit()
{
    Fixture f;
    const auto revision = f.session.document()->revision();
    f.begin();
    f.widget<QToolButton>("TextAlignCenter")->click();
    CHECK(f.widget<QToolButton>("TextAlignCenter")->isChecked());
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.document()->layers().size() == 1);
    CHECK(f.session.history().undoDepth() == 0);
    f.type(QStringLiteral("Centered"));
    CHECK(f.content().paragraphs.front().alignment == c::TextAlignment::Center);
    CHECK(f.session.history().undoDepth() == 1);
    f.controller->history(false);
    CHECK(f.session.document()->layers().size() == 1);
    f.controller->history(true);
    CHECK(f.content().paragraphs.front().alignment == c::TextAlignment::Center);
    CHECK(f.errors.empty());
}

void compositionSelectionAndVisualRelocation()
{
    Fixture f;
    f.add(text("abcd", 24));
    f.begin();
    f.select(1, 3);
    CHECK(f.controller->cursor().selectedText() == QStringLiteral("bc"));
    f.commit(QStringLiteral("X"), -1, 1);
    CHECK(f.content().utf8 == "Xd");
    f.controller->history(false);
    CHECK(f.content().utf8 == "abcd");
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    const auto before = f.content();
    f.preedit(QStringLiteral("X"),
        { { QInputMethodEvent::Selection, 1, 2, { } }, { QInputMethodEvent::Cursor, 1, 1, { } } });
    CHECK(f.controller->cursor().selectionStart() == 1);
    CHECK(f.controller->cursor().selectionEnd() == 3);
    CHECK(f.content() == before);
    CHECK(f.session.history().redoDepth() == 1);
    u::QtTextLayout expected(text("aXd", 24));
    const auto caret = expected.caret(2);
    const auto transform = f.session.document()->layer(f.controller->layerId())->localToDocument;
    const auto expectedPosition = transform.map({ caret.left(), caret.top() });
    CHECK(!f.canvas->scene().textQuads.empty());
    if (!f.canvas->scene().textQuads.empty()) {
        const auto actual = f.canvas->scene().textQuads.back().corners.front();
        CHECK(std::hypot(actual.x - expectedPosition.x, actual.y - expectedPosition.y) < 0.05);
    }
    CHECK(f.key(Qt::Key_Escape));
    CHECK(f.controller->active());
    CHECK(f.content() == before);
    CHECK(f.errors.empty());
}

void rejectedLargeSpeculativeEditsRestoreState()
{
    Fixture f;
    f.add(text("preserve", 24));
    f.begin();
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    f.type(QStringLiteral("!"));
    f.controller->history(false);
    f.select(2, 5);
    const auto content = f.content();
    const auto anchor = f.controller->cursor().anchor();
    const auto position = f.controller->cursor().position();
    const auto revision = f.session.document()->revision();
    const auto retained = f.session.history().memoryUsed();
    const QString tooManyParagraphs(4100, QLatin1Char('\n'));
    CHECK(tooManyParagraphs.toUtf8().size() < qsizetype(c::kMaximumTextBytes));
    auto unchanged = [&] {
        CHECK(f.controller->active());
        CHECK(f.content() == content);
        CHECK(f.controller->cursor().anchor() == anchor);
        CHECK(f.controller->cursor().position() == position);
        CHECK(f.session.document()->revision() == revision);
        CHECK(f.session.history().undoDepth() == 0);
        CHECK(f.session.history().redoDepth() == 1);
        CHECK(f.session.history().memoryUsed() == retained);
    };
    QApplication::clipboard()->setText(tooManyParagraphs);
    const auto beforePasteErrors = f.errors.size();
    (void)f.key(Qt::Key_V, Qt::ControlModifier);
    CHECK(f.errors.size() == beforePasteErrors + 1);
    unchanged();
    const auto beforeCommitErrors = f.errors.size();
    f.commit(tooManyParagraphs);
    CHECK(f.errors.size() == beforeCommitErrors + 1);
    unchanged();
    const auto beforePreeditErrors = f.errors.size();
    QInputMethodEvent preedit(tooManyParagraphs, { });
    CHECK(f.controller->inputEvent(&preedit));
    CHECK(f.errors.size() == beforePreeditErrors + 1);
    unchanged();
    // A rejected adapter operation does not poison the next normal edit.
    f.type(QStringLiteral("OK"));
    CHECK(f.content().utf8 == "prOKrve");
    CHECK(f.session.history().redoDepth() == 0);
}

void richClipboardRoundTripAndValidation()
{
    auto source = text("head\nsecond\n", 12);
    c::TextStylePatch patch;
    patch.family = "Preserve requested missing font 4509";
    patch.style = "Custom style";
    patch.weight = 650;
    patch.italic = true;
    patch.sizePixels = 29.5;
    patch.color = c::Rgba8 { 211, 42, 86, 97 };
    source = c::formatTextRange(source, 6, 10, patch);
    source.paragraphs[0].alignment = c::TextAlignment::Right;
    source.paragraphs[1].alignment = c::TextAlignment::Center;
    const auto encoded = u::encodeTextClipboard(source);
    const auto decoded = u::decodeTextClipboard(encoded);
    CHECK(decoded && *decoded == source);
    for (const auto& malformed : { QByteArray { }, QByteArray("[1,2,3]"), QByteArray("{broken"),
             QByteArray("null"), QByteArray(8 * 1024 * 1024 + 1, ' ') })
        CHECK(!u::decodeTextClipboard(malformed));
    const auto valid = QJsonDocument::fromJson(encoded).object();
    auto rejects = [&](const QJsonObject& value) {
        CHECK(!u::decodeTextClipboard(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    };
    auto malformed = valid;
    malformed["version"] = 2;
    rejects(malformed);
    malformed = valid;
    malformed["text"] = QString(qsizetype(c::kMaximumTextBytes + 1), QLatin1Char('x'));
    rejects(malformed);
    malformed = valid;
    malformed["runs"] = QStringLiteral("not an array");
    rejects(malformed);
    malformed = valid;
    malformed["paragraphs"] = QStringLiteral("not an array");
    rejects(malformed);
    malformed = valid;
    malformed["text"] = QJsonArray { };
    rejects(malformed);
    malformed = valid;
    auto style = malformed["default"].toObject();
    style["rgba"] = QJsonArray { 0, 1, 2, 256 };
    malformed["default"] = style;
    rejects(malformed);
    malformed = valid;
    auto runs = malformed["runs"].toArray();
    auto run = runs[0].toObject();
    run["start"] = -1;
    runs[0] = run;
    malformed["runs"] = runs;
    rejects(malformed);

    Fixture f;
    f.add(source);
    f.begin();
    f.select(1, 10); // Rebased range crosses the paragraph boundary.
    CHECK(f.key(Qt::Key_C, Qt::ControlModifier));
    const auto* mime = QApplication::clipboard()->mimeData();
    CHECK(mime->hasFormat(u::kTextClipboardMime));
    CHECK(mime->text() == QStringLiteral("ead\nsecon"));
    const auto copied = u::decodeTextClipboard(mime->data(u::kTextClipboardMime));
    CHECK(copied.has_value());
    if (copied) {
        CHECK(copied->utf8 == "ead\nsecon");
        for (std::size_t i = 0; i < copied->utf8.size(); ++i)
            CHECK(c::textStyleAt(*copied, i, false) == c::textStyleAt(source, i + 1, false));
        CHECK(copied->paragraphs[0].alignment == c::TextAlignment::Right);
        CHECK(copied->paragraphs[1].alignment == c::TextAlignment::Center);
    }
    f.controller->finish();
    f.begin({ 300, 300 });
    CHECK(f.key(Qt::Key_V, Qt::ControlModifier));
    CHECK(f.content().utf8 == "ead\nsecon");
    for (std::size_t i = 0; i < f.content().utf8.size(); ++i)
        CHECK(c::textStyleAt(f.content(), i, false) == c::textStyleAt(source, i + 1, false));
    CHECK(f.content().paragraphs[0].alignment == c::TextAlignment::Right);
    CHECK(f.content().paragraphs[1].alignment == c::TextAlignment::Center);
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(f.errors.empty());
}

void pendingNumericFinishesBeforeTypingAndCaretChanges()
{
    Fixture f;
    f.add(text("abc", 24));
    f.begin();
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    f.patchSize(25, 950);
    CHECK(f.session.history().undoDepth() == 0);
    f.type(QStringLiteral("x"));
    f.controller->finishNumeric();
    CHECK(f.content().utf8 == "x");
    CHECK(f.session.history().undoDepth() == 2);
    f.controller->history(false);
    CHECK(f.content().utf8 == "abc");
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 25);
    f.controller->history(false);
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 24);
    CHECK(f.session.history().undoDepth() == 0);
    f.controller->history(true);
    f.controller->history(true);
    CHECK(f.content().utf8 == "x");
    CHECK(c::textStyleAt(f.content(), 0).sizePixels == 25);

    Fixture keyboard;
    keyboard.add(text("abc", 24));
    keyboard.begin();
    CHECK(keyboard.key(Qt::Key_A, Qt::ControlModifier));
    keyboard.patchSize(25, 951);
    CHECK(keyboard.key(Qt::Key_Left));
    CHECK(keyboard.session.history().undoDepth() == 1);
    CHECK(!keyboard.controller->cursor().hasSelection());
    CHECK(keyboard.controller->cursor().position() == 0);

    Fixture pointer;
    pointer.add(text("abc", 24));
    pointer.begin();
    CHECK(pointer.key(Qt::Key_A, Qt::ControlModifier));
    pointer.patchSize(25, 952);
    u::QtTextLayout expected(pointer.content());
    const auto caret = expected.hit({ 3, 3 });
    CHECK(pointer.controller->press({ 33, 83 }, { }, false));
    CHECK(pointer.session.history().undoDepth() == 1);
    CHECK(!pointer.controller->cursor().hasSelection());
    CHECK(pointer.controller->cursor().position() == caret);
    CHECK(pointer.key(Qt::Key_A, Qt::ControlModifier));
    pointer.patchSize(26, 953);
    u::QtTextLayout expectedDrag(pointer.content());
    pointer.controller->drag({ 33, 83 });
    CHECK(pointer.session.history().undoDepth() == 2);
    CHECK(pointer.controller->cursor().position() == expectedDrag.hit({ 3, 3 }));
    CHECK(f.errors.empty() && keyboard.errors.empty() && pointer.errors.empty());
}

void emptyProvisionalOwnsTransformShortcut()
{
    Fixture f;
    f.begin();
    CHECK(f.session.activeLayer() == f.rasterId);
    QKeyEvent shortcut(QEvent::ShortcutOverride, Qt::Key_T, Qt::ControlModifier);
    CHECK(f.controller->keyEvent(&shortcut, false));
    CHECK(shortcut.isAccepted());
    CHECK(f.key(Qt::Key_T, Qt::ControlModifier));
    CHECK(f.controller->active());
    CHECK(f.session.activeLayer() == f.rasterId);
    CHECK(f.session.document()->layers().size() == 1);
    CHECK(f.session.history().undoDepth() == 0);
    f.type(QStringLiteral("committed"));
    CHECK(!f.key(Qt::Key_T, Qt::ControlModifier)); // Existing text permits main transform action.
    CHECK(f.errors.empty());
}

void fontSearchIsDeferredAndPreservesRange()
{
    Fixture f;
    const auto id = f.add(text("abcdef", 9));
    f.begin();
    f.select(1, 4);
    f.patchSize(20);
    f.controller->history(false); // Preserve an existing redo branch while searching.
    f.controller->prepareCaches(); // Fixture callbacks do not run MainWindow::synchronizeUi.
    const auto before = f.content(id);
    const auto revision = f.session.document()->revision();
    const auto cache = f.session.document()->layer(id)->renderCache->surface->id();
    const int anchor = f.controller->cursor().anchor(),
              position = f.controller->cursor().position();
    auto* family = f.widget<u::FontFamilyPicker>("TextFamily");
    family->lineEdit()->selectAll();
    QTest::keyClicks(family->lineEdit(), "Noto Sans");
    family->finishSearch(true);
    CHECK(f.content(id) == before);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().canRedo());
    family->lineEdit()->selectAll();
    QTest::keyClicks(family->lineEdit(), "arabic noto sans");
    CHECK(family->searching());
    CHECK(family->candidate() == QStringLiteral("Noto Sans Arabic"));
    CHECK(f.content(id) == before);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().canRedo());
    CHECK(f.session.document()->layer(id)->renderCache->surface->id() == cache);
    f.controller->refreshPresentation();
    CHECK(family->lineEdit()->text() == QStringLiteral("arabic noto sans"));
    // The click is consumed as a return from search, not a new caret/drag start.
    CHECK(!f.controller->press({ 33, 83 }, { }, false));
    CHECK(f.controller->cursor().anchor() == anchor);
    CHECK(f.controller->cursor().position() == position);
    CHECK(f.controller->layerId() == id);
    CHECK(f.session.document()->layers().size() == 2);
    CHECK(f.session.history().undoDepth() == 1);
    CHECK(!f.session.history().canRedo());
    CHECK(c::textStyleAt(f.content(id), 0, false) == c::textStyleAt(before, 0, false));
    CHECK(c::textStyleAt(f.content(id), 2, false).font.family == "Noto Sans Arabic");
    CHECK(c::textStyleAt(f.content(id), 2, false).sizePixels == 9);
    CHECK(c::textStyleAt(f.content(id), 2, false).color == before.defaultStyle.color);
    f.controller->history(false);
    CHECK(f.content(id) == before);
    const auto restored = family->currentText();
    family->lineEdit()->selectAll();
    QTest::keyClicks(family->lineEdit(), "no such font xyzq");
    CHECK(family->candidate().isEmpty());
    CHECK(!f.controller->press({ 33, 83 }, { }, false));
    CHECK(family->currentText() == restored);
    CHECK(f.content(id) == before);
    CHECK(f.session.history().canRedo());
    CHECK(f.controller->cursor().anchor() == anchor);
    CHECK(f.controller->cursor().position() == position);

    f.select(6, 6);
    f.patchSize(20); // Explicit insertion style must survive query and return.
    family->lineEdit()->selectAll();
    QTest::keyClicks(family->lineEdit(), "arabic noto sans");
    CHECK(!f.controller->press({ 33, 83 }, { }, false));
    CHECK(f.content(id) == before);
    CHECK(f.session.history().canRedo());
    CHECK(f.controller->insertionStyle().sizePixels == 20);
    f.type(QStringLiteral("x"));
    CHECK(c::textStyleAt(f.content(id), 6, false).font.family == "Noto Sans Arabic");
    CHECK(c::textStyleAt(f.content(id), 6, false).sizePixels == 20);
    CHECK(f.errors.empty());
}

void initialEmptySessionRestoresHistoryBranch()
{
    // Clearing committed characters and undoing the initial creation are both
    // abandoned first sessions. Neither may consume the history/redo that was
    // present before the first character, even after formatting actions.
    for (const bool undoCreation : { false, true }) {
        Fixture f;
        CHECK(f.session.execute(std::make_unique<c::SetLayerOpacityCommand>(f.rasterId, .5f)));
        CHECK(f.session.execute(std::make_unique<c::SetLayerOpacityCommand>(f.rasterId, .25f)));
        CHECK(f.session.undo());
        const auto memory = f.session.history().memoryUsed();
        const auto undoDepth = f.session.history().undoDepth();
        const auto redoDepth = f.session.history().redoDepth();
        const std::string undoLabel(f.session.history().undoLabel());
        const std::string redoLabel(f.session.history().redoLabel());
        const auto active = f.session.activeLayer();
        f.begin();
        const auto id = f.controller->layerId();
        f.type(QStringLiteral("draft"));
        CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
        f.patchSize(31);
        if (undoCreation) {
            f.controller->history(false); // Formatting.
            f.controller->history(false); // First creation.
            CHECK(!f.session.document()->containsLayer(id));
            CHECK(f.controller->active());
        } else {
            CHECK(f.key(Qt::Key_Backspace));
            CHECK(f.content(id).utf8.empty());
        }
        f.controller->finish();
        CHECK(!f.controller->active());
        CHECK(!f.session.document()->containsLayer(id));
        CHECK(f.session.document()->layers().size() == 1);
        CHECK(f.session.activeLayer() == active);
        CHECK(f.session.history().undoDepth() == undoDepth);
        CHECK(f.session.history().redoDepth() == redoDepth);
        CHECK(f.session.history().memoryUsed() == memory);
        CHECK(f.session.history().undoLabel() == undoLabel);
        CHECK(f.session.history().redoLabel() == redoLabel);
        CHECK(f.session.document()->layer(f.rasterId)->opacity == .5f);
        CHECK(f.session.redo());
        CHECK(f.session.document()->layer(f.rasterId)->opacity == .25f);
        CHECK(f.session.undo());
        CHECK(f.session.undo());
        CHECK(f.session.document()->layer(f.rasterId)->opacity == 1.f);
        CHECK(f.errors.empty());
    }
}

void initialNonemptySessionPublishesIndividualActions()
{
    Fixture f;
    CHECK(f.session.execute(std::make_unique<c::SetLayerOpacityCommand>(f.rasterId, .5f)));
    CHECK(f.session.execute(std::make_unique<c::SetLayerOpacityCommand>(f.rasterId, .25f)));
    CHECK(f.session.undo()); // Must be replaced by the newly accepted text branch.
    f.begin();
    const auto id = f.controller->layerId();
    f.type(QStringLiteral("kept"));
    const auto plain = f.content(id);
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    f.patchSize(35);
    const auto formatted = f.content(id);
    CHECK(f.key(Qt::Key_End, Qt::ControlModifier));
    f.type(QStringLiteral("!"));
    f.controller->history(false); // Keep branch-local redo across publication.
    CHECK(f.content(id) == formatted);
    f.controller->finish();
    CHECK(f.session.history().undoDepth() == 3); // Prefix + create + format.
    CHECK(f.session.history().redoDepth() == 1);
    CHECK(f.session.redo());
    CHECK(f.content(id).utf8 == "kept!");
    CHECK(f.session.document()->layer(f.rasterId)->opacity == .5f);
    CHECK(f.session.undo());
    CHECK(f.content(id) == formatted);
    CHECK(f.session.undo());
    CHECK(f.content(id) == plain);
    CHECK(f.session.undo());
    CHECK(!f.session.document()->containsLayer(id));
    CHECK(f.session.activeLayer() == f.rasterId);
    CHECK(f.session.undo());
    CHECK(f.session.document()->layer(f.rasterId)->opacity == 1.f);
    CHECK(f.errors.empty());
}

void previouslyCommittedTextCanRemainEmpty()
{
    Fixture f;
    f.begin();
    const auto id = f.controller->layerId();
    f.type(QStringLiteral("kept"));
    const auto valid = f.content(id);
    f.controller->finish();
    CHECK(f.session.history().undoDepth() == 1); // Single Create command publishes too.
    CHECK(f.session.document()->containsLayer(id));
    f.controller->editLayer(id);
    CHECK(f.controller->active());
    CHECK(f.controller->layerId() == id);
    CHECK(f.key(Qt::Key_A, Qt::ControlModifier));
    CHECK(f.key(Qt::Key_Backspace));
    f.controller->finish();
    CHECK(f.session.document()->containsLayer(id));
    CHECK(f.session.document()->layers().size() == 2);
    CHECK(f.content(id).utf8.empty());
    CHECK(f.session.activeLayer() == id);
    CHECK(f.session.history().undoDepth() == 2);
    CHECK(f.session.undo());
    CHECK(f.content(id) == valid);
    CHECK(f.session.redo());
    CHECK(f.content(id).utf8.empty());
    CHECK(f.errors.empty());
}

void explicitEmptyTextLayerCanBeEdited()
{
    Fixture f;
    const auto id = f.add(text("", 19), { 0, -1, 200, 1, 0, 120 });
    const auto transform = f.session.document()->layer(id)->localToDocument;
    const auto revision = f.session.document()->revision();
    f.controller->editLayer(id);
    CHECK(f.controller->active());
    CHECK(f.controller->layerId() == id);
    CHECK(f.session.activeLayer() == id);
    CHECK(f.session.document()->revision() == revision);
    CHECK(f.session.history().undoDepth() == 0);
    f.controller->finish();
    CHECK(f.session.document()->containsLayer(id));
    CHECK(f.session.history().undoDepth() == 0);
    f.controller->editLayer(id);
    f.type(QStringLiteral("restored"));
    f.controller->finish();
    CHECK(f.session.document()->layers().size() == 2);
    CHECK(f.content(id).utf8 == "restored");
    CHECK(f.session.document()->layer(id)->localToDocument == transform);
    CHECK(f.session.undo());
    CHECK(f.session.document()->containsLayer(id));
    CHECK(f.content(id).utf8.empty());
    CHECK(f.session.redo());
    CHECK(f.content(id).utf8 == "restored");
    CHECK(f.errors.empty());
}

void mixedColorOverlayGeometryIsStable()
{
    Fixture f;
    auto rich = text("red blue");
    c::TextStylePatch red, blue;
    red.color = c::Rgba8 { 255, 0, 0, 255 };
    blue.color = c::Rgba8 { 0, 0, 255, 255 };
    rich = c::formatTextRange(rich, 0, 3, red);
    rich = c::formatTextRange(rich, 4, 8, blue);
    f.add(rich);
    f.begin();
    f.select(0, 3);
    auto* overlay = f.widget<QWidget>("TextContextOverlay");
    overlay->ensurePolished();
    overlay->adjustSize();
    const auto size = overlay->size();
    const auto colorSize = f.widget<QPushButton>("TextColor")->size();
    f.select(0, 8);
    overlay->adjustSize();
    CHECK(f.widget<QPushButton>("TextColor")->text() == QStringLiteral("Mixed"));
    CHECK(f.widget<QPushButton>("TextColor")->size() == colorSize);
    CHECK(overlay->size() == size);
    CHECK(f.widget<QPushButton>("TextColor")
            ->styleSheet()
            .contains(QStringLiteral("border-bottom:4px")));
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    const auto fixture = QStringLiteral(IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR)
        + QStringLiteral("/NotoSans-Regular.ttf");
    if (QFontDatabase::addApplicationFont(fixture) < 0) {
        std::cerr << "Cannot load pinned Noto Sans fixture\n";
        return EXIT_FAILURE;
    }
    CHECK(QFontDatabase::addApplicationFont(QStringLiteral(IMAGEEDITOR_TEXT_FONT_FIXTURE_DIR)
              + QStringLiteral("/NotoSansArabic-Regular.ttf"))
        >= 0);
    try {
        configurableTextCommandsPreserveTyping();
        creationAndAbandonedPreedit();
        insertionSelectedRangesAndMixedControls();
        paragraphAlignmentAndMixedFormats();
        popupsAndIndependentColors();
        historyTypingNumericAndRedo();
        unicodeClipboardAndInputOwnership();
        compositionQueriesAndFocusLoss();
        inputMethodReplacementNeverSplitsScalars();
        transformedTargetingAndOverlayCacheReuse();
        documentGridCacheTracksTransformAndFilterSource();
        editOnlyClickMarginAndOutsideCompletion();
        numericRoundTripAndCancellationPreserveRedo();
        provisionalAlignmentSurvivesFirstCommit();
        compositionSelectionAndVisualRelocation();
        rejectedLargeSpeculativeEditsRestoreState();
        richClipboardRoundTripAndValidation();
        pendingNumericFinishesBeforeTypingAndCaretChanges();
        emptyProvisionalOwnsTransformShortcut();
        fontSearchIsDeferredAndPreservesRange();
        mixedColorOverlayGeometryIsStable();
        initialEmptySessionRestoresHistoryBranch();
        initialNonemptySessionPublishesIndividualActions();
        previouslyCommittedTextCanRemainEmpty();
        explicitEmptyTextLayerCanBeEdited();
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception: " << e.what() << '\n';
        ++failures;
    }
    std::cout << (failures ? "Text controller checks failed: " : "Text controller checks passed: ")
              << failures << '\n';
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
