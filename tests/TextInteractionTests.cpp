#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/MainWindow.hpp"
#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include "imageeditor/ui/Theme.hpp"
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QVulkanInstance>
#include <iostream>

namespace core = imageeditor::core;
namespace ui = imageeditor::ui;
namespace render = imageeditor::render;
int failures = 0;
#define CHECK(e)                                                                                   \
    do {                                                                                           \
        if (!(e)) {                                                                                \
            std::cerr << "FAIL " << __LINE__ << ": " << #e << '\n';                                \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)
void settle()
{
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
}
bool waitFor(const std::function<bool()>& f)
{
    QElapsedTimer timer;
    timer.start();
    do {
        settle();
        if (f())
            return true;
        QTest::qWait(5);
    } while (timer.elapsed() < 5000);
    return f();
}
void key(QObject* target, int code, Qt::KeyboardModifiers mods = { }, QString text = { })
{
    QKeyEvent event(QEvent::KeyPress, code, mods, text);
    QCoreApplication::sendEvent(target, &event);
    settle();
}
void mouse(render::CanvasWindow* canvas, QEvent::Type type, QPointF pos, Qt::MouseButton button,
    Qt::MouseButtons buttons)
{
    QMouseEvent e(
        type, pos, pos, QPointF(canvas->mapToGlobal(pos.toPoint())), button, buttons, { });
    QCoreApplication::sendEvent(canvas, &e);
    settle();
}
void run(QVulkanInstance* instance, bool native)
{
    ui::MainWindow window(instance, false, false);
    window.setUnsavedPromptEnabled(false);
    window.resize(1700, 980);
    window.show();
    settle();
    render::CanvasWindow* canvas = nullptr;
    ui::TextController* editor = nullptr;
    for (auto* w : QGuiApplication::allWindows())
        if (auto* c = dynamic_cast<render::CanvasWindow*>(w))
            canvas = c;
    for (auto* c : window.children())
        if (auto* t = dynamic_cast<ui::TextController*>(c))
            editor = t;
    CHECK(canvas && editor);
    if (!canvas || !editor)
        return;
    auto action = [&](const char* shortcut) {
        for (auto* a : window.findChildren<QAction*>())
            if (a->shortcuts().contains(QKeySequence(QString::fromLatin1(shortcut)))) {
                a->trigger();
                settle();
                return;
            }
        CHECK(false);
    };
    auto logical = [&](core::Vec2d p) {
        const auto& s = canvas->scene();
        const auto e = s.document.canvas.extent;
        const auto v = s.viewport.documentToViewport(
            p, { double(e.width), double(e.height) }, s.logicalViewport);
        return QPointF(v.x, v.y);
    };
    auto click = [&](core::Vec2d p) {
        const auto q = logical(p);
        mouse(canvas, QEvent::MouseButtonPress, q, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, q, Qt::LeftButton, { });
    };
    auto number = [&](const char* name, double v) {
        auto* w = window.findChild<QDoubleSpinBox*>(QString::fromLatin1(name));
        CHECK(w);
        if (w)
            w->setValue(v);
        settle();
    };
    auto button = [&](const char* name) {
        auto* w = window.findChild<QToolButton*>(QString::fromLatin1(name));
        CHECK(w);
        if (w)
            w->click();
        settle();
    };
    const auto& session = window.editorSession();
    const auto initial = session.document()->layers().size();
    if (native)
        CHECK(waitFor([&] { return canvas->rendererStats().framesSubmitted > 1; }));
    action("T");
    CHECK(session.activeTool() == core::ToolId::Text);
    click({ 180, 180 });
    CHECK(editor->active());
    CHECK(session.document()->layers().size() == initial);
    if (native) {
        auto* inputBridge = window.findChild<QWidget*>(QStringLiteral("VulkanCanvasContainer"));
        const bool focused = waitFor([&] {
            return QGuiApplication::focusObject() == canvas
                || QGuiApplication::focusObject() == inputBridge;
        });
        if (!focused) {
            auto describe = [](QObject* o) {
                return o ? std::string(o->metaObject()->className()) + ":"
                        + o->objectName().toStdString()
                         : "null";
            };
            std::cerr << "Text focus diagnostic object=" << describe(QGuiApplication::focusObject())
                      << " window=" << describe(QGuiApplication::focusWindow())
                      << " widget=" << describe(QApplication::focusWidget()) << " canvas=" << canvas
                      << '\n';
        }
        CHECK(focused);
        QInputMethodQueryEvent focusQuery(Qt::ImQueryAll);
        QCoreApplication::sendEvent(QGuiApplication::focusObject(), &focusQuery);
        CHECK(focusQuery.value(Qt::ImEnabled).toBool());
    }
    number("TextSize", 48);
    key(canvas, Qt::Key_T, { }, QStringLiteral("Text V1"));
    const auto id = editor->layerId();
    const auto* layer = session.document()->layer(id);
    CHECK(layer);
    if (!layer)
        return;
    CHECK(std::get<core::TextLayer>(layer->payload).utf8 == "Text V1");
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    CHECK(editor->active());
    CHECK(!session.document()->containsLayer(id));
    CHECK(session.document()->layers().size() == initial);
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(editor->active());
    CHECK(session.document()->containsLayer(id));
    CHECK(session.activeTool() == core::ToolId::Text);
    CHECK(session.document()->layers().size() == initial + 1);
    CHECK(session.document()->selection() == canvas->scene().document.selection);
    auto* overlay = window.findChild<QWidget*>(QStringLiteral("TextContextOverlay"));
    CHECK(overlay && overlay->isVisible());
    if (overlay)
        CHECK(overlay->parentWidget()->rect().contains(overlay->geometry()));
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    number("TextSize", 64);
    if (native) {
        auto* font = dynamic_cast<ui::FontFamilyPicker*>(
            window.findChild<QComboBox*>(QStringLiteral("TextFamily")));
        CHECK(font);
        // QTextCursor copies track subsequent document changes too; snapshot
        // integer positions so resetting the layout cannot alter our baseline.
        const auto selectedAnchor = editor->cursor().anchor();
        const auto selectedPosition = editor->cursor().position();
        if (font) {
            auto* panelWindow = overlay->parentWidget()->windowHandle();
            CHECK(panelWindow);
            auto nativeClick = [&](QWidget* widget) {
                QTest::mouseClick(panelWindow, Qt::LeftButton, {},
                    panelWindow->mapFromGlobal(widget->mapToGlobal(widget->rect().center())));
                settle();
            };
            auto checkFontPopupAnchor = [&] {
                // Use the configured native popup geometry: QWidget's requested
                // position alone misses Wayland's subsurface-parent anchoring.
                const bool anchored = waitFor([&] {
                    auto* popup = font->completionPopup()->windowHandle();
                    if (!popup || !popup->isExposed())
                        return false;
                    const auto fieldBottom = font->lineEdit()->mapToGlobal(
                        QPoint(0, font->lineEdit()->height())).y();
                    const auto top = popup->mapToGlobal(QPoint {}).y();
                    return top >= fieldBottom && top <= fieldBottom + 8;
                });
                if (!anchored) qWarning() << "Font popup anchor:" << font->completionPopup()->geometry()
                    << "native:" << font->completionPopup()->windowHandle()->geometry()
                    << "field:" << font->mapToGlobal(QPoint()) << font->size()
                    << "host:" << window.windowHandle()->geometry()
                    << "owner:" << font->completionPopup()->windowHandle()->transientParent();
                CHECK(anchored);
            };
            auto nativeQuery = [&](const QString& query) {
                nativeClick(font->lineEdit());
                CHECK(waitFor([&] { return QGuiApplication::focusObject() == font && font->hasFocus(); }));
                QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_A, Qt::ControlModifier);
                for (const QChar c : query)
                    QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key(c.toUpper().unicode()));
                settle();
            };
            const auto textBefore
                = std::get<core::TextLayer>(session.document()->layer(id)->payload);
            const auto revisionBefore = session.document()->revision();
            const auto depthBefore = session.history().undoDepth();
            // Exercise actual QWidgetWindow routing, not keys delivered directly
            // to the font widget (which concealed the native focus regression).
            nativeQuery(QStringLiteral("mono"));
            CHECK(font->searching());
            CHECK(!font->candidate().isEmpty());
            CHECK(session.activeTool() == core::ToolId::Text);
            CHECK(session.document()->revision() == revisionBefore);
            CHECK(session.history().undoDepth() == depthBefore);
            CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == textBefore);
            CHECK(font->lineEdit()->text() == QStringLiteral("mono"));
            checkFontPopupAnchor(); // Typing, before any explicit showPopup().
            font->hidePopup();
            CHECK(waitFor([&] { return !QApplication::activePopupWidget()
                && QGuiApplication::focusWindow() == window.windowHandle()
                && QGuiApplication::focusObject() == font && font->hasFocus(); }));
            QInputMethodQueryEvent fontQuery(Qt::ImSurroundingText | Qt::ImCursorRectangle);
            QCoreApplication::sendEvent(QGuiApplication::focusObject(), &fontQuery);
            CHECK(fontQuery.value(Qt::ImSurroundingText).toString() == QStringLiteral("mono"));
            const auto fieldOffset = QGuiApplication::focusWindow()->mapFromGlobal(
                font->lineEdit()->mapToGlobal(QPoint {}));
            const auto expectedCandidate = fontQuery.value(Qt::ImCursorRectangle).toRectF().translated(fieldOffset);
            const bool alignedCandidate = waitFor([&] { return QLineF(QGuiApplication::inputMethod()->cursorRectangle().topLeft(),
                expectedCandidate.topLeft()).length() < 2; });
            if (!alignedCandidate) {
                const auto actual = QGuiApplication::inputMethod()->cursorRectangle();
                const auto transform = QGuiApplication::inputMethod()->inputItemTransform();
                std::cerr << "Font IME actual " << actual.x() << ',' << actual.y()
                    << " expected " << expectedCandidate.x() << ',' << expectedCandidate.y()
                    << " input transform " << transform.dx() << ',' << transform.dy()
                    << " field origin " << fieldOffset.x() << ',' << fieldOffset.y()
                    << " current field origin " << QGuiApplication::focusWindow()->mapFromGlobal(
                        font->lineEdit()->mapToGlobal(QPoint {})).x() << ','
                    << QGuiApplication::focusWindow()->mapFromGlobal(
                        font->lineEdit()->mapToGlobal(QPoint {})).y() << '\n';
            }
            CHECK(alignedCandidate);
            // Reproduce Qt's late, overlay-relative geometry installation
            // after focus restoration; no new user input should be necessary.
            QGuiApplication::inputMethod()->setInputItemTransform(QTransform::fromTranslate(2, 3));
            CHECK(waitFor([&] { return QLineF(QGuiApplication::inputMethod()->cursorRectangle().topLeft(),
                expectedCandidate.topLeft()).length() < 2; }));
            // Geometry-only overlay movement must update the IME anchor even
            // without a keystroke, cursor change or another focus transition.
            const auto originalOverlayPosition = overlay->pos();
            for (const auto offset : {QPoint(11, 7), QPoint()}) {
                overlay->move(originalOverlayPosition + offset);
                const auto movedFieldOrigin = QGuiApplication::focusWindow()->mapFromGlobal(
                    font->lineEdit()->mapToGlobal(QPoint {}));
                const auto movedCandidate = fontQuery.value(Qt::ImCursorRectangle).toRectF()
                    .translated(movedFieldOrigin);
                CHECK(waitFor([&] { return QLineF(QGuiApplication::inputMethod()->cursorRectangle().topLeft(),
                    movedCandidate.topLeft()).length() < 2; }));
            }
            QInputMethodEvent searchCommit;
            searchCommit.setCommitString(QStringLiteral(" mono"));
            QCoreApplication::sendEvent(QGuiApplication::focusObject(), &searchCommit);
            CHECK(font->lineEdit()->text() == QStringLiteral("mono mono"));
            CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == textBefore);
            font->showPopup();
            settle();
            CHECK(waitFor([&] { return font->completionPopup()->windowHandle()
                && font->completionPopup()->windowHandle()->isExposed(); }));
            checkFontPopupAnchor(); // Explicit arrow/reopen path after IME input.
            if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEXT_SCREENSHOT");
                !output.isEmpty()) {
                QTest::qWait(150);
                QProcess capture;
                capture.start(QStringLiteral("spectacle"),
                    { QStringLiteral("--background"), QStringLiteral("--activewindow"),
                        QStringLiteral("--nonotify"), QStringLiteral("--output"),
                        output + QStringLiteral(".fonts.png") });
                CHECK(capture.waitForFinished(10000) && capture.exitCode() == 0);
                CHECK(font->completionPopup()->grab().save(output + QStringLiteral(".font-list.png")));
            }
            QTest::keyClick(font->completionPopup(), Qt::Key_Return);
            CHECK(waitFor([&] { return !QApplication::activePopupWidget(); }));
            settle();
            CHECK(!font->hasFocus());
            CHECK(!font->lineEdit()->hasFocus());
            CHECK(editor->cursor().anchor() == selectedAnchor);
            CHECK(editor->cursor().position() == selectedPosition);

            // Qt sends the first outside click to the grabbing popup. It must
            // accept a canvas return on that click, without requiring a second.
            nativeQuery(QStringLiteral("noto sans"));
            font->showPopup();
            settle();
            const auto selectedFont = font->candidate();
            auto* popup = font->completionPopup();
            const auto global = canvas->mapToGlobal(logical({ 390, 205 }).toPoint());
            const auto outside = popup->mapFromGlobal(global);
            CHECK(!popup->rect().contains(outside));
            QMouseEvent outsidePress(
                QEvent::MouseButtonPress, outside, global, Qt::LeftButton, Qt::LeftButton, { });
            QCoreApplication::sendEvent(popup, &outsidePress);
            settle();
            CHECK(!font->searching() && !popup->isVisible());
            CHECK(editor->cursor().anchor() == selectedAnchor);
            CHECK(editor->cursor().position() == selectedPosition);
            CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload)
                      .runs.front()
                      .style.font.family
                == selectedFont.toStdString());

            // No-match click-back must not target another point or start a drag.
            const auto accepted = std::get<core::TextLayer>(session.document()->layer(id)->payload);
            nativeQuery(QStringLiteral("unknown xyzq font"));
            CHECK(font->searching() && font->candidate().isEmpty());
            font->hidePopup();
            click({ 390, 205 });
            CHECK(!font->searching());
            CHECK(editor->cursor().anchor() == selectedAnchor);
            CHECK(editor->cursor().position() == selectedPosition);
            CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == accepted);
            CHECK(waitFor([&] { return !font->hasFocus() && !font->lineEdit()->hasFocus(); }));
            // Native focus and IME must follow a second editable overlay field,
            // not remain bound to the preceding search. Preserve the text range.
            nativeQuery(QStringLiteral("mono"));
            font->hidePopup();
            CHECK(waitFor([&] { return !QApplication::activePopupWidget(); }));
            auto* sizeField = window.findChild<QDoubleSpinBox*>(QStringLiteral("TextSize"));
            nativeClick(sizeField->findChild<QLineEdit*>());
            CHECK(waitFor([&] { return QGuiApplication::focusObject() == sizeField; }));
            QInputMethodQueryEvent sizeQuery(Qt::ImSurroundingText);
            QCoreApplication::sendEvent(QGuiApplication::focusObject(), &sizeQuery);
            CHECK(sizeQuery.value(Qt::ImSurroundingText).toString().contains(QStringLiteral("64")));
            QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_Escape);
            font->finishSearch(false);
            button("EditText");

            // Arrows are pointer actions, not editable-field focus. Exercise
            // the real QWidgetWindow path, including its queued focus handoff.
            auto* size = dynamic_cast<ui::ToolOptionsNumber*>(sizeField);
            auto* router = dynamic_cast<ui::CrossWindowPointerRouter*>(
                window.findChild<QObject*>(QStringLiteral("CrossWindowPointerRouter")));
            CHECK(size && router);
            if (size && router) {
                const auto before = std::get<core::TextLayer>(session.document()->layer(id)->payload);
                const auto baseSize = size->value();
                const auto depth = session.history().undoDepth();
                const auto anchor = editor->cursor().anchor();
                const auto position = editor->cursor().position();
                auto arrowPoint = [&](bool up) {
                    return panelWindow->mapFromGlobal(size->mapToGlobal(
                        QPoint(size->width() - 8, up ? 7 : size->height() - 7)));
                };
                auto checkPointerMode = [&] {
                    CHECK(!size->hasFocus());
                    CHECK(!size->findChild<QLineEdit*>()->hasSelectedText());
                    CHECK(QGuiApplication::focusObject() != size);
                    CHECK(editor->cursor().anchor() == anchor);
                    CHECK(editor->cursor().position() == position);
                };
                auto undoAdjustment = [&] {
                    // A broken/rolled-back arrow must not undo pre-existing
                    // typing and eventually remove this test's target layer.
                    if (session.history().undoDepth() != depth + 1) {
                        CHECK(session.history().undoDepth() == depth + 1);
                        return;
                    }
                    QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_Z, Qt::ControlModifier);
                    settle();
                    CHECK(session.history().undoDepth() == depth);
                    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == before);
                };
                for (bool focused : { false, true }) {
                    for (bool up : { true, false }) {
                        if (focused) {
                            nativeClick(size->findChild<QLineEdit*>());
                            CHECK(waitFor([&] { return QGuiApplication::focusObject() == size; }));
                        }
                        const auto point = arrowPoint(up);
                        QTest::mousePress(panelWindow, Qt::LeftButton, {}, point);
                        QTest::qWait(40);
                        CHECK(size->value() == baseSize + (up ? 1 : -1));
                        CHECK(router->captureOwner() == size);
                        CHECK(size->interactionActive());
                        CHECK(session.history().undoDepth() == depth);
                        checkPointerMode();
                        QTest::mouseRelease(panelWindow, Qt::LeftButton, {}, point);
                        settle();
                        CHECK(!size->interactionActive());
                        CHECK(!router->captureOwner());
                        CHECK(session.history().undoDepth() == depth + 1);
                        undoAdjustment();
                    }
                }
                const auto point = arrowPoint(true);
                QTest::mousePress(panelWindow, Qt::LeftButton, {}, point);
                QTest::qWait(700);
                CHECK(size->value() >= baseSize + 2);
                CHECK(size->interactionActive() && router->captureOwner() == size);
                CHECK(session.history().undoDepth() == depth);
                checkPointerMode();
                QTest::mouseRelease(panelWindow, Qt::LeftButton, {}, point);
                settle();
                CHECK(!size->interactionActive() && !router->captureOwner());
                CHECK(session.history().undoDepth() == depth + 1);
                const auto held = std::get<core::TextLayer>(session.document()->layer(id)->payload);
                undoAdjustment();
                QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_Z,
                    Qt::ControlModifier | Qt::ShiftModifier);
                settle();
                CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == held);
                undoAdjustment();
            }
        }
    }
    const auto clickAwayDepth = session.history().undoDepth();
    const auto clickAwayText = std::get<core::TextLayer>(session.document()->layer(id)->payload);
    const auto clickAwayLayers = session.document()->layers().size();
    click({ 180 - 20 / canvas->zoom(), 205 });
    CHECK(editor->active() && editor->layerId() == id);
    click({ 1400, 800 });
    CHECK(!editor->active());
    CHECK(session.document()->layers().size() == clickAwayLayers);
    if (session.history().undoDepth() != clickAwayDepth) {
        const auto after = std::get<core::TextLayer>(session.document()->layer(id)->payload);
        std::cerr << "Click-away history: " << clickAwayDepth << " -> " << session.history().undoDepth()
            << "; text bytes " << clickAwayText.utf8.size() << " -> " << after.utf8.size() << '\n';
        for (const auto& run : clickAwayText.runs) std::cerr << " before " << run.style.font.family << ' ' << run.style.sizePixels << '\n';
        for (const auto& run : after.runs) std::cerr << " after " << run.style.font.family << ' ' << run.style.sizePixels << '\n';
    }
    CHECK(session.history().undoDepth() == clickAwayDepth);
    CHECK(canvas->scene().textQuads.empty());
    button("EditText");
    CHECK(editor->active() && editor->layerId() == id);
    key(canvas, Qt::Key_End);
    number("TextSize", 28);
    key(canvas, Qt::Key_Return, { }, QStringLiteral("\n"));
    key(canvas, Qt::Key_T, { }, QStringLiteral("Editable • UTF-8 • مرحبا"));
    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload).runs.size() >= 2);
    QInputMethodQueryEvent query(Qt::ImQueryAll);
    QCoreApplication::sendEvent(canvas, &query);
    CHECK(query.value(Qt::ImEnabled).toBool());
    CHECK(query.value(Qt::ImCursorRectangle).toRectF().isValid());
    if (native) {
        QInputMethodQueryEvent focusQuery(Qt::ImQueryAll);
        QCoreApplication::sendEvent(QGuiApplication::focusObject(), &focusQuery);
        CHECK(focusQuery.value(Qt::ImSurroundingText) == query.value(Qt::ImSurroundingText));
        const auto globalOrigin = canvas->mapToGlobal(QPoint(0, 0));
        const auto focusOrigin = QGuiApplication::focusWindow()->mapToGlobal(QPoint(0, 0));
        const auto expected
            = query.value(Qt::ImCursorRectangle).toRectF().translated(globalOrigin - focusOrigin);
        const auto actual = QGuiApplication::inputMethod()->cursorRectangle();
        CHECK(QLineF(actual.topLeft(), expected.topLeft()).length() < 2);
    }
    CHECK(!canvas->scene().textQuads.empty());
    const auto beforeSelectionCache = session.document()->layer(id)->renderCache->surface->id();
    if (native) {
        CHECK(waitFor([&] { return canvas->rendererStats().fullUploads >= 2; }));
        QTest::qWait(200);
    }
    const auto stats = canvas->rendererStats();
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    QTest::qWait(650);
    settle();
    CHECK(session.document()->layer(id)->renderCache->surface->id() == beforeSelectionCache);
    if (native) {
        CHECK(canvas->rendererStats().uploadedBytes == stats.uploadedBytes);
        CHECK(canvas->rendererStats().framesSubmitted > stats.framesSubmitted);
    }
    QEvent deactivate(QEvent::ApplicationDeactivate);
    QCoreApplication::sendEvent(qApp, &deactivate);
    settle();
    CHECK(editor->active());
    const auto content = std::get<core::TextLayer>(session.document()->layer(id)->payload);
    if (native) {
        if (const auto output = qEnvironmentVariable("IMAGEEDITOR_TEXT_SCREENSHOT");
            !output.isEmpty()) {
            window.raise();
            window.activateWindow();
            QTest::qWait(150);
            QProcess capture;
            capture.start(QStringLiteral("spectacle"),
                { QStringLiteral("--background"), QStringLiteral("--activewindow"),
                    QStringLiteral("--nonotify"), QStringLiteral("--output"), output });
            CHECK(capture.waitForFinished(10000) && capture.exitCode() == 0);
        }
    }
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    CHECK(!editor->active());
    action("V");
    action("Ctrl+T");
    CHECK(canvas->scene().transformOverlay.has_value());
    number("TransformAngleControl", 32);
    button("TransformFlipHorizontal");
    button("TransformApply");
    layer = session.document()->layer(id);
    CHECK(std::get<core::TextLayer>(layer->payload) == content);
    CHECK(layer->localToDocument.m00 < 0 || layer->localToDocument.m11 < 0);
    const auto center = layer->localToDocument.map({ core::layerGeometryExtent(*layer).width * .5,
        core::layerGeometryExtent(*layer).height * .5 });
    // Canvas double-click through the actual Move routing enters text editing.
    mouse(canvas, QEvent::MouseButtonDblClick, logical(center), Qt::LeftButton, Qt::LeftButton);
    CHECK(editor->active());
    CHECK(editor->layerId() == id);
    key(canvas, Qt::Key_End);
    key(canvas, Qt::Key_T, { }, QStringLiteral("!"));
    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload).utf8 != content.utf8);
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    CHECK(editor->active());
    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload) == content);
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(editor->active());
    const auto oldRevision = session.document()->revision();
    QInputMethodEvent preedit(QStringLiteral("あ"), { });
    QCoreApplication::sendEvent(native ? QGuiApplication::focusObject() : canvas, &preedit);
    settle();
    CHECK(session.document()->revision() == oldRevision);
    key(canvas, Qt::Key_Escape);
    CHECK(editor->active());
    CHECK(session.document()->revision() == oldRevision);
    key(canvas, Qt::Key_Escape);
    CHECK(!editor->active());
    // The cached toolbar action can reopen an accepted, subsequently emptied
    // text layer, for which glyph/layout hit testing has no clickable content.
    auto* editButton = window.findChild<QToolButton*>(QStringLiteral("EditText"));
    CHECK(editButton && editButton->isVisible() && editButton->isEnabled());
    button("EditText");
    CHECK(editor->active() && editor->layerId() == id);
    key(canvas, Qt::Key_A, Qt::ControlModifier);
    key(canvas, Qt::Key_Backspace);
    button("TextDone");
    CHECK(!editor->active());
    CHECK(session.document()->containsLayer(id));
    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload).utf8.empty());
    button("EditText");
    CHECK(editor->active() && editor->layerId() == id);
    if (native)
        QTest::keyClick(QGuiApplication::focusWindow(), Qt::Key_A);
    else
        key(canvas, Qt::Key_A, {}, QStringLiteral("a"));
    CHECK(std::get<core::TextLayer>(session.document()->layer(id)->payload).utf8 == "a");
    CHECK(window.findChild<QToolButton*>(QStringLiteral("EditText")) == editButton);
    button("TextDone");
    if (native) {
        const auto frame = canvas->rendererStats().framesSubmitted;
        canvas->scheduleFrame();
        CHECK(waitFor([&] { return canvas->rendererStats().framesSubmitted > frame; }));
    }
    window.close();
    settle();
}
int main(int argc, char** argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ImageEditorTests"));
    QCoreApplication::setApplicationName(QStringLiteral("TextInteractions"));
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ui::applyEditorTheme(app);
    const bool native = qEnvironmentVariableIsSet("IMAGEEDITOR_TEXT_NATIVE");
    QVulkanInstance instance;
    int warnings = 0, errors = 0;
    if (native) {
        instance.setApiVersion(QVersionNumber(1, 2));
        instance.setLayers({ "VK_LAYER_KHRONOS_validation" });
        instance.installDebugOutputFilter([&](QVulkanInstance::DebugMessageSeverityFlags flags,
                                              QVulkanInstance::DebugMessageTypeFlags type,
                                              const void* message) {
            if (!type.testFlag(QVulkanInstance::ValidationMessage))
                return false;
            if (flags.testFlag(QVulkanInstance::ErrorSeverity))
                ++errors;
            else if (flags.testFlag(QVulkanInstance::WarningSeverity))
                ++warnings;
            else
                return false;
            const auto* data = static_cast<const VkDebugUtilsMessengerCallbackDataEXT*>(message);
            std::cerr << (data ? data->pMessage : "Vulkan validation") << '\n';
            return false;
        });
        CHECK(instance.create());
        if (!instance.isValid())
            return 1;
    }
    run(native ? &instance : nullptr, native);
    instance.destroy();
    settle();
    CHECK(warnings == 0 && errors == 0);
    std::cout << "Text interaction assertions: " << failures << " failed; Vulkan " << warnings
              << " warnings, " << errors << " errors\n";
    return failures ? 1 : 0;
}
