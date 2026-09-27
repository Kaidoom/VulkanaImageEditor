#include "imageeditor/ui/TextController.hpp"
#include "imageeditor/core/LayerGeometry.hpp"
#include "imageeditor/render/CanvasCoordinateMapping.hpp"
#include "imageeditor/render/CanvasWindow.hpp"
#include "imageeditor/ui/FontFamilyPicker.hpp"
#include "imageeditor/ui/OverlayDockWorkspace.hpp"
#include "imageeditor/ui/TextClipboard.hpp"
#include "imageeditor/ui/ToolOptionsButton.hpp"
#include "imageeditor/ui/ToolOptionsNumber.hpp"
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextDocumentFragment>
#include <cmath>
#include <stdexcept>

namespace imageeditor::ui {
void TextController::setSession(core::EditorSession& session)
{
    if (session_ == &session) return;
    Q_ASSERT(!active());
    blink_.stop(); densityTimer_.stop();
    if (session_->document()) inactiveLayouts_[session_] = std::move(layouts_);
    layouts_.clear();
    session_ = &session;
    if (auto node = inactiveLayouts_.extract(session_); !node.empty()) layouts_ = std::move(node.mapped());
    while (inactiveLayouts_.size() > 8) inactiveLayouts_.erase(inactiveLayouts_.begin());
    lastZoom_ = lastDpr_ = -1;
}
void TextController::forgetSession(core::EditorSession& session)
{
    inactiveLayouts_.erase(&session);
}
namespace {
// Style choices return to the canvas; font searching has its own explicit
// acceptance boundary rather than committing on every combo value change.
class TextStyleBox final : public QComboBox {
public:
    using QComboBox::QComboBox;
    std::function<void()> onPopupClosed;
    void hidePopup() override
    {
        QComboBox::hidePopup();
        if (onPopupClosed)
            onPopupClosed();
    }
};
QColor qtColor(core::Rgba8 c) { return { c.red, c.green, c.blue, c.alpha }; }
core::Rgba8 rgba(QColor c)
{
    return { std::uint8_t(c.red()), std::uint8_t(c.green()), std::uint8_t(c.blue()),
        std::uint8_t(c.alpha()) };
}
render::CanvasCoordinateMapping mapping(const render::CanvasWindow& canvas)
{
    const auto& s = canvas.scene();
    return { { double(s.document.canvas.extent.width), double(s.document.canvas.extent.height) },
        { double(canvas.width()), double(canvas.height()) },
        { std::uint32_t(canvas.width()), std::uint32_t(canvas.height()) }, s.viewport };
}
QString utf8(const std::string& s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
}
TextController::TextController(core::EditorSession& session, render::CanvasWindow* canvas,
    OverlayDockWorkspace* workspace, QObject* parent)
    : QObject(parent)
    , session_(&session)
    , canvas_(canvas)
    , workspace_(workspace)
{
    // Wayland grants keyboard focus to the top-level surface, not a Vulkan
    // subsurface. Reuse its existing QWidget container as the IME focus bridge;
    // it draws no text, owns no undo stack and introduces no extra child window.
    workspace_->canvasContainer()->installEventFilter(this);
    createOverlay();
    // Overlay fields can move after native popup focus is restored (for
    // example while the compositor configures the owner). Keep the IME anchor
    // current even when the text cursor itself has not changed.
    for (auto* widget : {overlay_, static_cast<QWidget*>(family_), static_cast<QWidget*>(family_->lineEdit()),
             static_cast<QWidget*>(size_), workspace_->panelOverlay(), workspace_->window()})
        widget->installEventFilter(this);
    updateShortcutHints(overlay_, shortcuts_);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        if (workspace_->hasModalOverlay()) return;
        auto* proxy = workspace_->canvasContainer()->focusProxy();
        if (active() && proxy && now) {
            QWidget* owner = (now == family_ || family_->isAncestorOf(now)) ? family_
                : (now == size_ || size_->isAncestorOf(now)) ? static_cast<QWidget*>(size_)
                : (now == styles_ || styles_->isAncestorOf(now)) ? styles_ : nullptr;
            if (owner && owner != proxy)
                setOverlayInput(owner);
        }
        if (active() && !now && proxy != family_)
            QTimer::singleShot(0, this, [this] { returnCanvasFocus(); });
    });
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (workspace_->hasModalOverlay()) return;
            auto* proxy = workspace_->canvasContainer()->focusProxy();
            if (active() && proxy && !QApplication::activePopupWidget()
                && !QApplication::activeModalWidget()
                && QGuiApplication::focusWindow() == workspace_->window()->windowHandle()
                && QGuiApplication::applicationState() == Qt::ApplicationActive)
                setOverlayInput(proxy);
        });
    });
    const auto updateInput = [this] {
        QTimer::singleShot(0, this, [this] { updateInputGeometry(); });
    };
    connect(qApp, &QGuiApplication::focusObjectChanged, this, updateInput);
    // QWidget may install an overlay-surface-relative IME transform *after*
    // popup focus restoration. The native focus owner is the host window, so
    // reconcile that later change too (not only focus/cursor/move events).
    connect(QGuiApplication::inputMethod(), &QInputMethod::cursorRectangleChanged, this, updateInput);
    connect(family_->lineEdit(), &QLineEdit::cursorPositionChanged, this, updateInput);
    connect(size_->findChild<QLineEdit*>(), &QLineEdit::cursorPositionChanged, this, updateInput);
    blink_.setInterval(std::max(150, QApplication::cursorFlashTime() / 2));
    connect(&blink_, &QTimer::timeout, this, [this] {
        if (!active() || !canvas_->isExposed()
            || QGuiApplication::applicationState() != Qt::ApplicationActive)
            return;
        caretVisible_ = !caretVisible_;
        publishOverlays();
    });
    densityTimer_.setSingleShot(true);
    densityTimer_.setInterval(100);
    connect(&densityTimer_, &QTimer::timeout, this, [this] {
        prepareCaches();
        refreshPresentation();
    });
}
TextController::~TextController()
{
    workspace_->canvasContainer()->removeEventFilter(this);
    canvas_->onViewportUpdated = { };
    delete overlay_;
}
bool TextController::eventFilter(QObject* watched, QEvent* event)
{
    if (workspace_->hasModalOverlay()) return false;
    if (active() && (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show))
        QTimer::singleShot(0, this, [this] { updateInputGeometry(); });
    if (watched == workspace_->canvasContainer()
        && (event->type() == QEvent::InputMethod || event->type() == QEvent::InputMethodQuery))
        return inputEvent(event);
    return QObject::eventFilter(watched, event);
}
void TextController::createOverlay()
{
    overlay_ = new QWidget(workspace_->panelOverlay());
    overlay_->setObjectName(QStringLiteral("TextContextOverlay"));
    overlay_->setAttribute(Qt::WA_StyledBackground);
    overlay_->setStyleSheet(QStringLiteral(
        "#TextContextOverlay {background:palette(button);border:1px solid palette(mid);border-radius:7px;}"));
    auto* row = new QHBoxLayout(overlay_);
    row->setContentsMargins(8, 7, 8, 7);
    row->setSpacing(5);
    family_ = new FontFamilyPicker(overlay_);
    family_->onReturnToText
        = [this] { QTimer::singleShot(0, this, [this] { returnCanvasFocus(); }); };
    family_->onOutsidePress = [this](QPoint global) {
        if (!active() || !family_->searching())
            return false;
        const auto local = canvas_->mapFromGlobal(global);
        if (!QRect(QPoint { }, canvas_->size()).contains(local))
            return false;
        auto* panels = workspace_->panelOverlay();
        if (panels->isVisible() && panels->mask().contains(panels->mapFromGlobal(global)))
            return false; // Other panels/overlay controls do not accept a search.
        const auto p = mapping(*canvas_).logicalViewportToDocument(
            { double(local.x()), double(local.y()) });
        press(p, Qt::NoModifier, false);
        return true;
    };
    family_->setObjectName(QStringLiteral("TextFamily"));
    family_->setFixedWidth(164);
    family_->setToolTip(QStringLiteral("Font family • selected characters or insertion style"));
    row->addWidget(family_);
    auto* styleBox = new TextStyleBox(overlay_);
    styles_ = styleBox;
    styleBox->onPopupClosed = family_->onReturnToText;
    styles_->setObjectName(QStringLiteral("TextStyle"));
    styles_->setFixedWidth(110);
    row->addWidget(styles_);
    size_ = new ToolOptionsNumber(overlay_);
    size_->setObjectName(QStringLiteral("TextSize"));
    size_->setRange(.25, 2048);
    size_->setDecimals(2);
    size_->setSuffix(QStringLiteral(" px"));
    size_->setFixedWidth(100);
    size_->setToolTip(QStringLiteral("Font size in document pixels"));
    row->addWidget(size_);
    auto button = [&](const char* name, const QString& label, const QString& hint) {
        auto* b = new ToolOptionsButton(
            QString::fromLatin1(name), label, hint, ToolOptionsButton::Kind::Toggle, overlay_);
        b->setFocusPolicy(Qt::NoFocus);
        row->addWidget(b);
        return b;
    };
    bold_ = button("TextBold", QStringLiteral("B"), QStringLiteral("Bold"));
    italic_ = button("TextItalic", QStringLiteral("I"), QStringLiteral("Italic"));
    alignLeft_ = button(
        "TextAlignLeft", QStringLiteral("≡←"), QStringLiteral("Align current paragraphs left"));
    alignCenter_ = button(
        "TextAlignCenter", QStringLiteral("≡"), QStringLiteral("Align current paragraphs center"));
    alignRight_ = button(
        "TextAlignRight", QStringLiteral("→≡"), QStringLiteral("Align current paragraphs right"));
    color_ = new QPushButton(overlay_);
    color_->setObjectName(QStringLiteral("TextColor"));
    color_->setFixedSize(76, 30);
    color_->setFocusPolicy(Qt::NoFocus);
    color_->setToolTip(QStringLiteral("Text color • independent of the global colors"));
    row->addWidget(color_);
    auto* done = new ToolOptionsButton(QStringLiteral("TextDone"), QStringLiteral("Done"),
        QStringLiteral("Finish editing ({{FinishTextAction}}). Enter inserts a new line."),
        ToolOptionsButton::Kind::Action, overlay_);
    done->setFocusPolicy(Qt::NoFocus);
    row->addWidget(done);
    connect(done, &QToolButton::clicked, this, [this] { finish(); });
    family_->onFamilyChosen = [this](const QString& font) {
        if (syncing_ || !active())
            return;
        const auto requested = font.toStdString();
        const auto text = layout_->text();
        const auto h = hint();
        const auto a = std::min(h.anchor, h.position), b = std::max(h.anchor, h.position);
        const bool same = a == b ? insertion_.font.family == requested
            : std::all_of(text.runs.begin(), text.runs.end(), [&](const auto& run) {
                  return run.start >= b || run.start + run.length <= a
                      || run.style.font.family == requested;
              });
        if (same)
            return; // Choosing the current family must not clear its style or redo.
        core::TextStylePatch p;
        p.family = requested;
        p.style = std::string { };
        patchStyle(p);
    };
    connect(styles_, &QComboBox::activated, this, [this](int i) {
        if (syncing_ || !active() || i < 0)
            return;
        core::TextStylePatch p;
        p.style = styles_->itemText(i).toStdString();
        const auto family = styles_->property("textFontFamily").toString();
        const auto style = styles_->itemText(i);
        p.weight = QFontDatabase::weight(family, style);
        p.italic = QFontDatabase::italic(family, style);
        patchStyle(p);
    });
    connect(size_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        if (syncing_ || !active())
            return;
        if (!numericGroup_)
            numericGroup_ = ++group_;
        core::TextStylePatch p;
        p.sizePixels = v;
        patchStyle(p, numericGroup_);
        if (!size_->interactionActive())
            finishNumeric();
    });
    size_->onInteractionFinished = [this] { finishNumeric(); };
    size_->onInteractionCancelled = [this] { finishNumeric(true); };
    connect(bold_, &QToolButton::clicked, this, [this](bool b) {
        core::TextStylePatch p;
        p.weight = b ? 700 : 400;
        patchStyle(p);
    });
    connect(italic_, &QToolButton::clicked, this, [this](bool b) {
        core::TextStylePatch p;
        p.italic = b;
        patchStyle(p);
    });
    connect(alignLeft_, &QToolButton::clicked, this,
        [this] { applyAlignment(core::TextAlignment::Left); });
    connect(alignCenter_, &QToolButton::clicked, this,
        [this] { applyAlignment(core::TextAlignment::Center); });
    connect(alignRight_, &QToolButton::clicked, this,
        [this] { applyAlignment(core::TextAlignment::Right); });
    connect(color_, &QPushButton::clicked, this, [this] { showColor(); });
    overlay_->hide();
}
void TextController::returnCanvasFocus()
{
    if (!active() || workspace_->hasModalOverlay()
        || (family_->searching() && workspace_->canvasContainer()->focusProxy() == family_)
        || QApplication::activeModalWidget()
        || QApplication::activePopupWidget()
        || QGuiApplication::applicationState() != Qt::ApplicationActive)
        return;
    auto* focus = QApplication::focusWidget();
    if (!focus || focus == family_ || family_->isAncestorOf(focus) || focus == styles_
        || styles_->isAncestorOf(focus)
        || workspace_->canvasContainer()->focusProxy() == family_
        || workspace_->canvasContainer()->focusProxy() == styles_) {
        setOverlayInput(nullptr);
        workspace_->canvasContainer()->setFocus(Qt::OtherFocusReason);
        canvas_->requestActivate();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
}
void TextController::prepareOverlayInteraction(QWidget* target, const QMouseEvent& event)
{
    if (active() && target && (target == overlay_ || overlay_->isAncestorOf(target))) {
        guardEdit([this] { commitComposition(); });
        if (target == family_ || family_->isAncestorOf(target))
            setOverlayInput(family_);
        else if (target == size_ || size_->isAncestorOf(target)) {
            // Arrows deliberately release text focus. Restore the canvas bridge
            // NOW, before the shared router captures the press; doing it from a
            // queued focus callback deactivates the overlay mid-hold and cancels
            // the numeric preview. Clicking the number still owns text/IME input.
            setOverlayInput(target == size_ && size_->isStepButtonPress(event) ? nullptr : size_);
        }
        else if (target == styles_ || styles_->isAncestorOf(target))
            setOverlayInput(styles_);
    }
}
void TextController::setOverlayInput(QWidget* target)
{
    auto* bridge = workspace_->canvasContainer();
    auto* nativeFocus = QGuiApplication::focusWindow();
    auto* before = QGuiApplication::focusObject();
    // QWidget and QWindow ancestry intentionally differ for the stable ARGB
    // panel plane. Wayland activates the host, never its child surface. Keep
    // native focus there, but delegate keys/IME to the real QWidget editor.
    bridge->setFocusProxy(target);
    // activateWindow() only requests *native* activation and cannot establish
    // the logical QWidget window here. This public Qt6 compatibility call is
    // deliberately contained; no private APIs, keyboard grabs or fake events.
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    QApplication::setActiveWindow(target ? target->window() : bridge->window());
    QT_WARNING_POP
    (target ? target : bridge)->setFocus(Qt::OtherFocusReason);
    auto* after = QGuiApplication::focusObject();
    if (nativeFocus && nativeFocus == QGuiApplication::focusWindow() && before != after)
        Q_EMIT nativeFocus->focusObjectChanged(after);
    updateInputGeometry();
}
void TextController::updateInputGeometry()
{
    if (!active() || workspace_->hasModalOverlay())
        return;
    auto* target = qobject_cast<QWidget*>(QGuiApplication::focusObject());
    auto* nativeFocus = QGuiApplication::focusWindow();
    if (!target || !nativeFocus || target != workspace_->canvasContainer()->focusProxy()
        || (target != family_ && target != size_ && target != styles_))
        return;
    // These controls forward their internal line editor's IME query unchanged.
    if (auto* combo = qobject_cast<QComboBox*>(target); combo && combo->isEditable())
        target = combo->lineEdit();
    else if (qobject_cast<QAbstractSpinBox*>(target))
        target = target->findChild<QLineEdit*>();
    if (!target)
        return;
    const auto offset = nativeFocus->mapFromGlobal(target->mapToGlobal(QPoint { }));
    const auto transform = QTransform::fromTranslate(offset.x(), offset.y());
    auto* input = QGuiApplication::inputMethod();
    // Updating IME geometry emits cursorRectangleChanged. Do not create an
    // idle notification loop once the cross-surface mapping is already right.
    if (input->inputItemTransform() == transform && input->inputItemRectangle() == target->rect())
        return;
    input->setInputItemTransform(transform);
    input->setInputItemRectangle(target->rect());
    input->update(Qt::ImQueryAll);
}
core::AffineTransform TextController::transform() const
{
    const auto* layer = session_->document() ? session_->document()->layer(id_) : nullptr;
    return layer       ? layer->localToDocument
        : provisional_ ? provisional_->localToDocument
                       : core::AffineTransform { };
}
bool TextController::containsEditingPoint(core::Vec2d p) const
{
    if (!active() || !displayLayout())
        return false;
    const auto inverse = transform().inverted();
    if (!inverse)
        return false;
    // Offset each transformed layout edge by 30 logical screen pixels. Inverse
    // row lengths convert edge-normal distances to local units, even for flips,
    // nonuniform scale and shear. This changes no layer/cache/widget geometry.
    constexpr double margin = 30.0;
    const double documentMargin = margin / canvas_->zoom();
    const auto derivatives=inverse->derivatives(p);
    const double dx = documentMargin * std::hypot(derivatives[0].x,derivatives[1].x);
    const double dy = documentMargin * std::hypot(derivatives[0].y,derivatives[1].y);
    const auto local = inverse->map(p);
    return displayLayout()->bounds().adjusted(-dx, -dy, dx, dy).contains(QPointF(local.x, local.y));
}
bool TextController::press(core::Vec2d p, Qt::KeyboardModifiers modifiers, bool doubleClick)
{
    auto* doc = session_->document();
    if (!doc)
        return false;
    if (active() && family_->searching()) {
        // Accept/abandon search without relocating the saved character range.
        // Returning near text resumes editing; clicking away finishes the edit.
        family_->finishSearch(true);
        if (!containsEditingPoint(p))
            finish();
        else {
            returnCanvasFocus();
            publishOverlays();
        }
        return false;
    }
    if (active()) {
        size_->interpretText();
        finishNumeric();
        if (!containsEditingPoint(p)) {
            finish();
            return false; // This press is Done, never creation/retargeting too.
        }
    }
    prepareCaches();
    const core::Layer* hit = nullptr;
    for (auto it = doc->layers().rbegin(); it != doc->layers().rend(); ++it)
        if (doc->isEffectivelyVisible(it->id) && core::hitTextBounds(*it, p)) {
            hit = &*it;
            break;
        }
    if (doubleClick && session_->activeTool() == core::ToolId::Move && !hit)
        return false;
    const auto extent = doc->canvas().extent;
    if (!active() && (p.x < 0 || p.y < 0 || p.x >= extent.width || p.y >= extent.height))
        return false;
    const auto hitId = active() ? id_ : (hit ? hit->id : 0);
    commitComposition();
    if (!active() || hitId != id_) {
        finish();
        if (active())
            return false;
        if (onActivateText)
            onActivateText();
        previousActive_ = session_->activeLayer();
        if (hitId) {
            id_ = hitId;
            session_->setActiveLayer(id_);
            layout_ = std::make_unique<QtTextLayout>(
                std::get<core::TextLayer>(doc->layer(id_)->payload));
        } else {
            core::TextLayer text;
            text.defaultStyle.color = session_->foregroundColor();
            text.defaultStyle.sizePixels = 24;
            provisional_ = core::Layer::text("Text", text);
            provisional_->localToDocument.m02 = p.x;
            provisional_->localToDocument.m12 = p.y;
            id_ = provisional_->id;
            layout_ = std::make_unique<QtTextLayout>(text);
        }
        cursor_ = QTextCursor(&layout_->document());
    }
    size_->interpretText();
    size_->clearFocus();
    setOverlayInput(nullptr);
    workspace_->canvasContainer()->setAttribute(Qt::WA_InputMethodEnabled, true);
    workspace_->canvasContainer()->setFocus(Qt::OtherFocusReason);
    canvas_->requestActivate();
    canvas_->setSpacePanHeld(false);
    const auto inverse = transform().inverted();
    if (!inverse)
        return false;
    cursor_.setPosition(layout_->hit(inverse->map(p)),
        modifiers.testFlag(Qt::ShiftModifier) ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    if (doubleClick)
        cursor_.select(QTextCursor::WordUnderCursor);
    deliberateCaret();
    showEditing();
    return true;
}
bool TextController::editLayer(core::LayerId id)
{
    auto* doc = session_->document();
    const auto* layer = doc ? doc->layer(id) : nullptr;
    if (!layer || !std::holds_alternative<core::TextLayer>(layer->payload))
        return false;
    if (active() && id_ == id) {
        family_->finishSearch(true);
        finishNumeric();
    } else {
        finish();
        if (active())
            return false;
        if (onActivateText)
            onActivateText();
        layer = doc->layer(id);
        if (!layer)
            return false;
        id_ = id;
        previousActive_ = session_->activeLayer();
        session_->setActiveLayer(id);
        layout_ = std::make_unique<QtTextLayout>(std::get<core::TextLayer>(layer->payload));
        cursor_ = QTextCursor(&layout_->document());
        cursor_.movePosition(QTextCursor::End);
        deliberateCaret();
    }
    setOverlayInput(nullptr);
    workspace_->canvasContainer()->setAttribute(Qt::WA_InputMethodEnabled, true);
    workspace_->canvasContainer()->setFocus(Qt::OtherFocusReason);
    canvas_->requestActivate();
    canvas_->setSpacePanHeld(false);
    showEditing();
    return true;
}
void TextController::showEditing()
{
    blink_.start();
    overlay_->show();
    overlay_->raise();
    if (onChanged)
        onChanged();
    refreshPresentation();
}
void TextController::drag(core::Vec2d p)
{
    if (!active() || !layout_)
        return;
    finishNumeric();
    if (const auto inverse = transform().inverted()) {
        const int pos = layout_->hit(inverse->map(p));
        if (pos != cursor_.position()) {
            cursor_.setPosition(pos, QTextCursor::KeepAnchor);
            deliberateCaret();
        }
    }
}
bool TextController::settleCreationHistory()
{
    if (historyBeforeCreation_) {
        auto* doc = session_->document();
        const auto* layer = doc ? doc->layer(id_) : nullptr;
        const bool abandoned = !layer || std::get<core::TextLayer>(layer->payload).utf8.empty();
        if (!abandoned) {
            // Preserve all typing/formatting undo steps; never collapse the edit.
            bool published = false;
            try {
                published = historyBeforeCreation_->publishAppliedBranch(*doc, session_->history());
            } catch (const std::exception& e) {
                if (onError)
                    onError(QString::fromUtf8(e.what()));
            }
            if (!published) {
                if (onError)
                    onError(QStringLiteral("Unable to finish text history; keep editing and retry."));
                return false;
            }
        } else if (layer) {
            (void)doc->takeLayer(id_);
        }
        session_->history() = std::move(*historyBeforeCreation_);
        historyBeforeCreation_.reset();
        if (abandoned) {
            if (doc) doc->restoreContentState(contentBeforeCreation_);
            session_->setActiveLayer(previousActive_);
        }
    }
    return true;
}
void TextController::finish()
{
    if (!active())
        return;
    family_->finishSearch(false);
    commitComposition();
    setOverlayInput(nullptr);
    size_->interpretText();
    size_->finishInteraction();
    finishNumeric();
    if (!settleCreationHistory())
        return;
    if (colorDialog_)
        colorDialog_->reject();
    canvas_->cancelTextInput();
    blink_.stop();
    id_ = 0;
    workspace_->canvasContainer()->setAttribute(Qt::WA_InputMethodEnabled, false);
    cursor_ = QTextCursor { };
    layout_.reset();
    preeditLayout_.reset();
    provisional_.reset();
    preedit_.clear();
    overlay_->hide();
    workspace_->setContextOverlayInteractionRegion({ });
    canvas_->setTextQuads({ });
    resetGroup();
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    if (onChanged)
        onChanged();
}
core::TextEditHint TextController::hint() const
{
    if (!layout_)
        return { id_, 0, 0 };
    const auto t = layout_->text();
    const core::Utf8TextIndex index(t.utf8);
    return { id_, index.byteOffset(std::size_t(cursor_.anchor())),
        index.byteOffset(std::size_t(cursor_.position())) };
}
void TextController::restoreCursor(core::TextEditHint h)
{
    const core::Utf8TextIndex index(layout_->text().utf8);
    cursor_ = QTextCursor(&layout_->document());
    cursor_.setPosition(int(index.utf16Offset(h.anchor)));
    cursor_.setPosition(int(index.utf16Offset(h.position)), QTextCursor::KeepAnchor);
}
void TextController::resetGroup()
{
    ++group_;
    typingKind_.clear();
    typingClock_.invalidate();
}
void TextController::guardEdit(const std::function<void()>& operation)
{
    core::TextEditHint saved { id_, 0, 0 };
    try {
        if (active())
            saved = hint();
        operation();
    } catch (const std::exception& error) {
        // Limits and allocation failures cover speculative Qt mutation too,
        // not just history admission. Never leave an invalid adapter alive.
        try {
            if (numericEdit_)
                finishNumeric(true);
            const auto* layer = session_->document() ? session_->document()->layer(id_) : nullptr;
            if (active() && layout_ && (layer || provisional_)) {
                layout_->reset(
                    std::get<core::TextLayer>((layer ? layer : &*provisional_)->payload));
                restoreCursor(saved);
                preedit_.clear();
                preeditLayout_.reset();
                refreshPresentation();
                synchronizeControls();
            }
        } catch (const std::exception&) {
            // A failed Qt adapter recovery must not strand the original history.
            // Publication capacity was reserved before each admitted command.
            if (!settleCreationHistory())
                return;
            setOverlayInput(nullptr);
            id_ = 0;
            cursor_ = QTextCursor { };
            layout_.reset();
            preeditLayout_.reset();
            blink_.stop();
            overlay_->hide();
            workspace_->setContextOverlayInteractionRegion({ });
            canvas_->setTextQuads({ });
        }
        if (onError)
            onError(QString::fromUtf8(error.what()));
    }
}
void TextController::finishNumeric(bool cancel)
{
    numericGroup_ = 0;
    if (!numericEdit_)
        return;
    auto edit = std::move(*numericEdit_);
    numericEdit_.reset();
    resetGroup();
    auto* doc = session_->document();
    auto* layer = doc ? doc->layer(id_) : nullptr;
    if (!edit.selected) {
        if (cancel) {
            insertion_ = edit.insertion;
            synchronizeControls();
        }
        return;
    }
    if (!layer || !std::holds_alternative<core::TextLayer>(layer->payload))
        return;
    const auto& after = std::get<core::TextLayer>(layer->payload);
    if (!cancel && after != edit.before) {
        try {
            if (historyBeforeCreation_)
                historyBeforeCreation_->reserveForPublication(session_->history(), 1);
            if (!session_->adoptApplied(std::make_unique<core::TextEditCommand>(
                    id_, edit.before, after, edit.cursor, hint(), 0, "Format text")))
                cancel = true;
        } catch (const std::exception& e) {
            cancel = true;
            if (onError)
                onError(QString::fromUtf8(e.what()));
        }
    }
    if (cancel) {
        doc->setLayerText(id_, std::move(edit.before));
        insertion_ = edit.insertion;
    }
    layout_->reset(std::get<core::TextLayer>(layer->payload));
    restoreCursor(edit.cursor);
    if (onChanged)
        onChanged();
    prepareCaches();
    refreshPresentation();
    synchronizeControls();
}
std::uint64_t TextController::typingGroup(const QString& kind)
{
    if (typingKind_ != kind || !typingClock_.isValid() || typingClock_.elapsed() > 900)
        ++group_;
    typingKind_ = kind;
    typingClock_.restart();
    return group_;
}
void TextController::mutate(core::TextLayer next, core::TextEditHint before,
    core::TextEditHint after, const QString& label, std::uint64_t key)
{
    auto* doc = session_->document();
    if (!doc || !active())
        return;
    try {
        next = core::normalizedText(std::move(next));
        const auto* existing = doc->layer(id_);
        if(existing && !existing->localToDocument.isAffine())
            (void)QtTextLayout(next).documentBounds(existing->localToDocument);
        bool changed = false;
        if (!existing && provisional_ && next.utf8.empty()) {
            // Empty paragraph alignment is editor state until first commit.
            provisional_->payload = next;
        } else if (!existing && provisional_ && !next.utf8.empty()) {
            if (!historyBeforeCreation_) {
                contentBeforeCreation_ = doc->contentState();
                historyBeforeCreation_.emplace(std::move(session_->history()));
                session_->history() = core::History { };
            }
            historyBeforeCreation_->reserveForPublication(session_->history(), 1);
            auto layer = *provisional_;
            layer.payload = next;
            changed = session_->execute(std::make_unique<core::TextEditCommand>(
                std::move(layer), doc->layers().size(), previousActive_, after, key));
        } else if (existing) {
            if (historyBeforeCreation_)
                historyBeforeCreation_->reserveForPublication(session_->history(), 1);
            changed = session_->execute(std::make_unique<core::TextEditCommand>(id_,
                std::get<core::TextLayer>(existing->payload), next, before, after, key,
                label.toStdString()));
        }
        // Qt layout reflects admitted content, not a competing undo stack.
        const auto* admitted = doc->layer(id_);
        layout_->reset(admitted ? std::get<core::TextLayer>(admitted->payload)
                                : std::get<core::TextLayer>(provisional_->payload));
        const bool noEffect = layout_->text() == next;
        restoreCursor(changed || noEffect ? after : before);
        if (changed && onChanged)
            onChanged();
        caretVisible_ = true;
        prepareCaches();
        synchronizeControls();
        refreshPresentation();
    } catch (const std::exception& e) {
        const auto* layer = doc->layer(id_);
        layout_->reset(layer ? std::get<core::TextLayer>(layer->payload)
                             : std::get<core::TextLayer>(provisional_->payload));
        restoreCursor(before);
        if (onError)
            onError(QString::fromUtf8(e.what()));
        refreshPresentation();
    }
}
void TextController::replaceSelection(const QString& text, const QString& label, bool grouped)
{
    if (!active())
        return;
    const auto before = hint();
    const auto original = layout_->text();
    const auto removed
        = std::max(before.anchor, before.position) - std::min(before.anchor, before.position);
    if (text.toUtf8().size() + qsizetype(original.utf8.size() - removed)
        > qsizetype(core::kMaximumTextBytes)) {
        if (onError)
            onError(QStringLiteral("Text limit: 256 KiB per layer"));
        return;
    }
    cursor_.insertText(text, QtTextLayout::format(insertion_));
    auto next = layout_->text();
    const auto after = hint();
    mutate(std::move(next), before, after, label, grouped ? typingGroup(label) : 0);
}
void TextController::deliberateCaret()
{
    if (!layout_)
        return;
    finishNumeric();
    resetGroup();
    const auto text = layout_->text();
    insertion_ = core::textStyleAt(text, hint().position);
    caretVisible_ = true;
    synchronizeControls();
    publishOverlays();
    placeOverlay();
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
}
void TextController::patchStyle(core::TextStylePatch patch, std::uint64_t grouping)
{
    guardEdit([&] { applyStyle(std::move(patch), grouping); });
}
void TextController::applyStyle(core::TextStylePatch patch, std::uint64_t grouping)
{
    if (!active() || syncing_)
        return;
    commitComposition();
    if (!grouping) {
        finishNumeric();
        resetGroup();
    } else if (!numericEdit_)
        numericEdit_ = NumericEdit { layout_->text(), hint(), insertion_, cursor_.hasSelection() };
    if (!cursor_.hasSelection()) {
        insertion_ = core::patchedTextStyle(insertion_, patch);
        synchronizeControls();
        QGuiApplication::inputMethod()->update(Qt::ImFont);
        return;
    }
    const auto h = hint();
    auto next = core::formatTextRange(
        layout_->text(), std::min(h.anchor, h.position), std::max(h.anchor, h.position), patch);
    if (grouping) {
        const auto* target=session_->document()->layer(id_);
        if(target && !target->localToDocument.isAffine())
            (void)QtTextLayout(next).documentBounds(target->localToDocument);
        session_->document()->setLayerText(id_, next);
        layout_->reset(next);
        restoreCursor(h);
        prepareCaches();
        refreshPresentation();
        synchronizeControls();
    } else
        mutate(std::move(next), h, h, QStringLiteral("Format text"));
    insertion_ = core::patchedTextStyle(insertion_, patch);
}
void TextController::applyAlignment(core::TextAlignment alignment)
{
    guardEdit([&] { alignParagraphs(alignment); });
}
void TextController::alignParagraphs(core::TextAlignment alignment)
{
    if (!active() || syncing_)
        return;
    commitComposition();
    finishNumeric();
    resetGroup();
    auto text = layout_->text();
    const auto h = hint();
    const auto a = std::min(h.anchor, h.position), b = std::max(h.anchor, h.position);
    for (std::size_t i = 0; i < text.paragraphs.size(); ++i) {
        auto& p = text.paragraphs[i];
        const auto end
            = i + 1 < text.paragraphs.size() ? text.paragraphs[i + 1].start : text.utf8.size() + 1;
        if (end > a && (p.start < b || (a == b && p.start <= a)))
            p.alignment = alignment;
    }
    mutate(std::move(text), h, h, QStringLiteral("Align text"));
}
void TextController::synchronizeControls()
{
    if (!active() || !layout_)
        return;
    const QScopedValueRollback guard(syncing_, true);
    auto shown = insertion_;
    bool familyMixed = false, styleMixed = false, sizeMixed = false, colorMixed = false,
         weightMixed = false, italicMixed = false;
    const auto text = layout_->text();
    const auto h = hint();
    const auto a = std::min(h.anchor, h.position), b = std::max(h.anchor, h.position);
    bool first = true;
    for (const auto& run : text.runs)
        if (a != b && run.start < b && run.start + run.length > a) {
            if (first) {
                shown = run.style;
                first = false;
                continue;
            }
            familyMixed |= shown.font.family != run.style.font.family;
            styleMixed |= shown.font.style != run.style.font.style;
            sizeMixed |= shown.sizePixels != run.style.sizePixels;
            colorMixed |= shown.color != run.style.color;
            weightMixed |= shown.font.weight != run.style.font.weight;
            italicMixed |= shown.font.italic != run.style.font.italic;
        }
    family_->setDisplayedFamily(QString::fromStdString(shown.font.family), familyMixed);
    const auto family = QString::fromStdString(shown.font.family);
    styles_->setProperty("textFontFamily", family);
    styles_->clear();
    styles_->addItems(QFontDatabase::styles(family));
    const auto requested = QString::fromStdString(shown.font.style);
    if (styleMixed || familyMixed || weightMixed || italicMixed) {
        styles_->setCurrentIndex(-1);
        styles_->setPlaceholderText(QStringLiteral("Mixed styles"));
    } else {
        int i = styles_->findText(requested);
        if (requested.isEmpty())
            for (int candidate = 0; candidate < styles_->count(); ++candidate) {
                const auto name = styles_->itemText(candidate);
                if (QFontDatabase::weight(family, name) == shown.font.weight
                    && QFontDatabase::italic(family, name) == shown.font.italic) {
                    i = candidate;
                    break;
                }
            }
        if (i < 0 && !requested.isEmpty()) {
            styles_->addItem(requested);
            i = styles_->count() - 1;
        }
        styles_->setCurrentIndex(i < 0 ? 0 : i);
    }
    size_->setValue(shown.sizePixels);
    size_->setIndeterminate(sizeMixed);
    bold_->setChecked(!weightMixed && shown.font.weight >= 600);
    italic_->setChecked(!italicMixed && shown.font.italic);
    bold_->setText(weightMixed ? QStringLiteral("B·") : QStringLiteral("B"));
    italic_->setText(italicMixed ? QStringLiteral("I·") : QStringLiteral("I"));
    bool hasBold = false, hasItalic = false;
    for (const auto& s : QFontDatabase::styles(family)) {
        hasBold |= QFontDatabase::bold(family, s);
        hasItalic |= QFontDatabase::italic(family, s);
    }
    bold_->setEnabled(hasBold && !familyMixed);
    italic_->setEnabled(hasItalic && !familyMixed);
    std::optional<core::TextAlignment> alignment;
    bool mixedAlignment = false;
    for (std::size_t i = 0; i < text.paragraphs.size(); ++i) {
        const auto& p = text.paragraphs[i];
        const auto end
            = i + 1 < text.paragraphs.size() ? text.paragraphs[i + 1].start : text.utf8.size() + 1;
        if (end > a && (p.start < b || (a == b && p.start <= a))) {
            if (alignment && alignment != p.alignment)
                mixedAlignment = true;
            else
                alignment = p.alignment;
        }
    }
    alignLeft_->setChecked(!mixedAlignment && alignment == core::TextAlignment::Left);
    alignCenter_->setChecked(!mixedAlignment && alignment == core::TextAlignment::Center);
    alignRight_->setChecked(!mixedAlignment && alignment == core::TextAlignment::Right);
    const auto c = qtColor(shown.color);
    color_->setText(colorMixed ? QStringLiteral("Mixed") : c.name().toUpper());
    // Keep the identical stylesheet box model in uniform and mixed states.
    // A neutral split stripe communicates mixed values without hiding its 4px.
    color_->setStyleSheet(QStringLiteral("QPushButton {border-bottom:4px solid %1;}")
            .arg(colorMixed
                    ? QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #a2a8b8,stop:0.49 "
                                     "#a2a8b8,stop:0.5 #525967,stop:1 #525967)")
                    : c.name(QColor::HexArgb)));
}
void TextController::showColor()
{
    if (!active())
        return;
    commitComposition();
    resetGroup();
    if (colorDialog_) {
        colorDialog_->raise();
        return;
    }
    QWidget* owner = overlay_;
    while (owner->parentWidget())
        owner = owner->parentWidget();
    auto* dialog = new QColorDialog(qtColor(insertion_.color), owner);
    colorDialog_ = dialog;
    dialog->setWindowTitle(QStringLiteral("Text color"));
    dialog->setObjectName(QStringLiteral("TextColorDialog"));
    dialog->setOptions(QColorDialog::DontUseNativeDialog | QColorDialog::ShowAlphaChannel);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::ApplicationModal);
    (void)owner->winId();
    (void)dialog->winId();
    dialog->windowHandle()->setTransientParent(owner->windowHandle());
    connect(dialog, &QColorDialog::colorSelected, this, [this](QColor c) {
        core::TextStylePatch p;
        p.color = rgba(c);
        patchStyle(p);
    });
    connect(dialog, &QColorDialog::finished, this, [this] {
        if (active()) {
            QTimer::singleShot(0, this, [this] { returnCanvasFocus(); });
            publishOverlays();
        }
    });
    dialog->show();
}
void TextController::history(bool redo)
{
    if (!active())
        return;
    family_->finishSearch(false);
    commitComposition();
    finishNumeric();
    resetGroup();
    if (!(redo ? session_->redo() : session_->undo()))
        return;
    const auto saved = hint();
    const auto h = session_->history().textEditHint();
    if (h && h->layer != id_) {
        finish();
        if (onChanged)
            onChanged();
        return;
    }
    const auto* layer = session_->document()->layer(id_);
    if (layer && std::holds_alternative<core::TextLayer>(layer->payload))
        layout_->reset(std::get<core::TextLayer>(layer->payload));
    else if (provisional_)
        layout_->reset(std::get<core::TextLayer>(provisional_->payload));
    else {
        finish();
        if (onChanged)
            onChanged();
        return;
    }
    restoreCursor(h.value_or(saved));
    deliberateCaret();
    if (onChanged)
        onChanged();
    refreshPresentation();
}
void TextController::setShortcutBindings(const ShortcutBindings& bindings)
{
    shortcuts_ = bindings;
    updateShortcutHints(overlay_, shortcuts_);
}
bool TextController::keyEvent(QKeyEvent* e, bool fieldOwnsInput)
{
    bool result = false;
    guardEdit([&] { result = handleKey(e, fieldOwnsInput); });
    return result;
}
bool TextController::handleKey(QKeyEvent* e, bool fieldOwnsInput)
{
    if (!active() || fieldOwnsInput || QApplication::activePopupWidget()
        || QApplication::activeModalWidget())
        return false;
    for(const auto* action:{"CloseDocumentAction","NextDocumentAction","PreviousDocumentAction"})
        if(shortcutMatches(shortcuts_,action,*e))return false;
    // The geometry command is the one editor shortcut allowed through while
    // editing; tool letters themselves always remain textual input.
    if (shortcutMatches(shortcuts_, "LayerTransformAction", *e)) {
        // An empty provisional text edit has no geometry yet. Do not let its
        // shortcut accidentally transform the previously active raster layer.
        if (session_->document()->containsLayer(id_))
            return false;
        e->accept();
        return true;
    }
    if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return true;
    }
    if (e->type() == QEvent::KeyRelease) {
        e->accept();
        return true;
    }
    // Admit the live numeric baseline before typing/deletion or moving the
    // caret. Flushing after either would reverse command order or restore the
    // old range over the navigation that just happened.
    finishNumeric();
    const auto key = e->key();
    const auto mods = e->modifiers();
    const bool ctrl = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);
    if (key == Qt::Key_Escape) {
        if (!preedit_.isEmpty()) {
            QGuiApplication::inputMethod()->reset();
            clearComposition();
        } else
            finish();
    } else if (shortcutMatches(shortcuts_, "FinishTextAction", *e))
        finish();
    else if (shortcutMatches(shortcuts_, "UndoAction", *e))
        history(false);
    else if (shortcutMatches(shortcuts_, "RedoAction", *e))
        history(true);
    else if (ctrl && key == Qt::Key_A) {
        commitComposition();
        cursor_.select(QTextCursor::Document);
        deliberateCaret();
    } else if (ctrl && (key == Qt::Key_C || key == Qt::Key_X)) {
        commitComposition();
        if (cursor_.hasSelection()) {
            auto selected = cursor_.selectedText();
            selected.replace(QChar(0x2029), QLatin1Char('\n'));
            auto* mime = new QMimeData;
            mime->setText(selected);
            QtTextLayout copied(core::TextLayer { });
            QTextCursor copy(&copied.document());
            copy.insertFragment(QTextDocumentFragment(cursor_));
            QTextCursor firstParagraph(&copied.document());
            firstParagraph.setBlockFormat(
                layout_->document().findBlock(cursor_.selectionStart()).blockFormat());
            mime->setData(kTextClipboardMime, encodeTextClipboard(copied.text()));
            QApplication::clipboard()->setMimeData(mime);
            if (key == Qt::Key_X) {
                resetGroup();
                replaceSelection({ }, QStringLiteral("Cut text"), false);
            }
        }
    } else if (ctrl && key == Qt::Key_V) {
        commitComposition();
        resetGroup();
        const auto* mime = QApplication::clipboard()->mimeData();
        const auto rich = mime ? decodeTextClipboard(mime->data(kTextClipboardMime)) : std::nullopt;
        if (rich) {
            const auto before = hint();
            QtTextLayout copied(*rich);
            QTextCursor copy(&copied.document());
            copy.select(QTextCursor::Document);
            const auto pasteStart = cursor_.selectionStart();
            cursor_.insertFragment(QTextDocumentFragment(copy));
            QTextCursor firstParagraph(&layout_->document());
            firstParagraph.setPosition(pasteStart);
            firstParagraph.setBlockFormat(copied.document().begin().blockFormat());
            auto next = layout_->text();
            const auto after = hint();
            mutate(std::move(next), before, after, QStringLiteral("Paste text"));
        } else
            replaceSelection(
                QApplication::clipboard()->text(), QStringLiteral("Paste text"), false);
    } else if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
        commitComposition();
        const auto before = hint();
        if (!cursor_.hasSelection()) {
            if (ctrl)
                cursor_.movePosition(
                    key == Qt::Key_Backspace ? QTextCursor::PreviousWord : QTextCursor::NextWord,
                    QTextCursor::KeepAnchor);
            else {
                const auto s = utf8(layout_->text().utf8);
                QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, s);
                boundary.setPosition(cursor_.position());
                const auto p = key == Qt::Key_Backspace ? boundary.toPreviousBoundary()
                                                        : boundary.toNextBoundary();
                if (p >= 0)
                    cursor_.setPosition(int(p), QTextCursor::KeepAnchor);
            }
        }
        if (cursor_.hasSelection()) {
            cursor_.removeSelectedText();
            auto next = layout_->text();
            const auto after = hint();
            mutate(std::move(next), before, after, QStringLiteral("Delete text"),
                typingGroup(key == Qt::Key_Backspace ? QStringLiteral("Backspace")
                                                     : QStringLiteral("Delete")));
        }
    } else {
        QTextCursor::MoveOperation op = QTextCursor::NoMove;
        switch (key) {
        case Qt::Key_Left:
            op = ctrl ? QTextCursor::PreviousWord : QTextCursor::Left;
            break;
        case Qt::Key_Right:
            op = ctrl ? QTextCursor::NextWord : QTextCursor::Right;
            break;
        case Qt::Key_Up:
            op = QTextCursor::Up;
            break;
        case Qt::Key_Down:
            op = QTextCursor::Down;
            break;
        case Qt::Key_Home:
            op = ctrl ? QTextCursor::Start : QTextCursor::StartOfLine;
            break;
        case Qt::Key_End:
            op = ctrl ? QTextCursor::End : QTextCursor::EndOfLine;
            break;
        default:
            break;
        }
        if (op != QTextCursor::NoMove) {
            commitComposition();
            cursor_.movePosition(op, shift ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
            deliberateCaret();
        } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            commitComposition();
            replaceSelection(QStringLiteral("\n"), QStringLiteral("Type text"), true);
        } else if (!e->text().isEmpty() && (!ctrl || mods.testFlag(Qt::AltModifier))
            && e->text().at(0).unicode() >= 0x20)
            replaceSelection(e->text(), QStringLiteral("Type text"), true);
    }
    e->accept();
    return true;
}
void TextController::prepareCaches()
{
    auto* doc = session_->document();
    if (!doc)
        return;
    std::erase_if(layouts_, [&](const auto& p) { return !doc->containsLayer(p.first); });
    for (const auto& item : doc->layers()) {
        if (!std::holds_alternative<core::TextLayer>(item.payload))
            continue;
        auto* layer = doc->layer(item.id);
        auto& record = layouts_[item.id];
        if (!record.second || record.first != layer->textRevision) {
            record.second
                = std::make_unique<QtTextLayout>(std::get<core::TextLayer>(layer->payload));
            record.first = layer->textRevision;
        }
        const auto& t = layer->localToDocument;
        const auto bounds=record.second->bounds();
        const auto scale = t.maximumScaleOver({bounds.x(),bounds.y(),bounds.width(),bounds.height()});
        const bool filtered = core::hasActiveSpatialFilters(layer->filters)||core::hasActiveLayerEffects(layer->effects);
        const auto viewScale = canvas_->zoom() * canvas_->devicePixelRatio();
        const double density = filtered ? 1.0 : std::clamp(
            std::pow(2.0,
                std::ceil(std::log2(
                    std::max(.125, scale * viewScale)))),
            .125, 8.0);
        constexpr std::size_t pixelBudget = 16 * 1024 * 1024;
        bool documentGrid = !filtered && viewScale <= 1;
        bool reducedPreview = false;
        if (documentGrid) {
            const bool current = layer->renderCache && layer->renderCache->contentRevision == layer->textRevision
                && layer->renderCache->rasterizedDocumentTransform == t;
            core::RectD bounds;
            try {
                if(!current) bounds=record.second->documentBounds(t);
            } catch(const std::exception& error) {
                if(onError) onError(QString::fromUtf8(error.what()));
                continue; // Retain editable data and the last valid cache.
            }
            const auto width = current ? layer->renderCache->surface->extent().width
                : std::ceil(bounds.right()) - std::floor(bounds.x);
            const auto height = current ? layer->renderCache->surface->extent().height
                : std::ceil(bounds.bottom()) - std::floor(bounds.y);
            reducedPreview = width > 8192 || height > 8192 || width * height > double(pixelBudget);
            documentGrid = !reducedPreview;
        }
        const bool modeChanged = layer->renderCache
            && layer->renderCache->rasterizedDocumentTransform.has_value() != documentGrid;
        if (documentGrid && layer->renderCache && layer->renderCache->contentRevision == layer->textRevision
            && !canvas_->transformDragging())
            if (auto moved = core::translatedDocumentRenderCache(layer->renderCache, t))
                layer->renderCache = std::move(moved);
        const bool transformChanged = documentGrid && !canvas_->transformDragging() && layer->renderCache
            && layer->renderCache->rasterizedDocumentTransform != t;
        if (!layer->renderCache || layer->renderCache->contentRevision != layer->textRevision
            || modeChanged || transformChanged
            || layer->renderCache->requestedDensity != (documentGrid ? 1.0 : density)) {
            try {
                // Native-size presentation shares the merge rasterizer. Filters
                // retain a canonical local grid because their radii are local.
                const auto source = documentGrid
                    ? record.second->rasterizeDocument(t, pixelBudget)
                    : record.second->rasterize(density, filtered ? std::optional<std::size_t>(pixelBudget) : std::nullopt);
                if (filtered && (source->surface->extent().width > 8192 || source->surface->extent().height > 8192))
                    throw std::runtime_error("Filtered text source exceeds the display dimension limit");
                auto cache = std::make_shared<core::LayerRenderCache>(*source);
                cache->contentRevision = layer->textRevision;
                cache->requestedDensity = documentGrid ? 1.0 : density;
                layer->renderCache = std::move(cache);
                if (reducedPreview && onError)
                    onError(QStringLiteral("This text exceeds the full-resolution preview budget. Its editable data is retained."));
            } catch (const std::exception&) {
                if (filtered && layer->renderCache && layer->renderCache->rasterizedDocumentTransform)
                    layer->renderCache.reset();
                if (onError)
                    onError(QStringLiteral("Not enough memory to render this text. Its editable data is retained."));
            }
        }
    }
}
QtTextLayout* TextController::displayLayout() const
{
    return preeditLayout_ ? preeditLayout_.get() : layout_.get();
}
void TextController::refreshPresentation()
{
    if (publishing_ || !session_->document())
        return;
    const QScopedValueRollback guard(publishing_, true);
    auto snapshot = session_->document()->snapshot();
    if (active() && preeditLayout_) {
        auto cache = preeditLayout_->rasterize(canvas_->zoom() * canvas_->devicePixelRatio());
        bool found = false;
        for (auto& layer : snapshot.layersBottomToTop)
            if (layer.id == id_) {
                layer.renderCache = cache;
                found = true;
                break;
            }
        if (!found && provisional_) {
            core::LayerSnapshot layer;
            layer.id = id_;
            layer.payload = std::get<core::TextLayer>(provisional_->payload);
            layer.localToDocument = provisional_->localToDocument;
            layer.renderCache = cache;
            snapshot.layersBottomToTop.push_back(std::move(layer));
        }
    }
    canvas_->setDocument(std::move(snapshot), false);
    publishOverlays();
    placeOverlay();
}
void TextController::viewportChanged()
{
    const auto origin = mapping(*canvas_).documentToLogicalViewport({ 0, 0 });
    if (lastOrigin_ == origin && lastZoom_ == canvas_->zoom()
        && lastDpr_ == canvas_->devicePixelRatio() && lastViewport_ == canvas_->size())
        return;
    const bool density = lastZoom_ != canvas_->zoom() || lastDpr_ != canvas_->devicePixelRatio();
    lastOrigin_ = origin;
    lastZoom_ = canvas_->zoom();
    lastDpr_ = canvas_->devicePixelRatio();
    lastViewport_ = canvas_->size();
    if (density)
        densityTimer_.start();
    if (active()) {
        publishOverlays();
        placeOverlay();
    }
}
void TextController::publishOverlays()
{
    if (!active() || !displayLayout())
        return;
    std::vector<render::CanvasScene::TextQuad> quads;
    const auto t = transform();
    const auto* layer = session_->document() ? session_->document()->layer(id_) : nullptr;
    if (layer && (!session_->document()->isEffectivelyVisible(id_) || layer->opacity <= 0)) {
        canvas_->setTextQuads({ });
        return;
    }
    auto add = [&](QRectF rect, core::Rgba8 color) {
        if (layer && layer->crop && layer->crop->hasChamfer()) {
            const auto p = core::clippedCropPolygon(*layer->crop, { rect.x(), rect.y(), rect.width(), rect.height() });
            for (std::size_t i = 2; i < p.size; ++i) {
                const auto a = t.map(p.vertices[0]), b = t.map(p.vertices[i - 1]), c = t.map(p.vertices[i]);
                quads.push_back({ { a, b, c, c }, color }); // Triangle in the existing quad stream.
            }
            return;
        }
        if (layer && layer->crop) {
            const auto r = *layer->crop;
            rect = rect.intersected(QRectF(r.x, r.y, r.width, r.height));
            if (rect.isEmpty())
                return;
        }
        quads.push_back(
            { { t.map({ rect.left(), rect.top() }), t.map({ rect.right(), rect.top() }),
                  t.map({ rect.right(), rect.bottom() }), t.map({ rect.left(), rect.bottom() }) },
                color });
    };
    if (cursor_.hasSelection() && !preeditLayout_)
        for (const auto& r : layout_->selection(cursor_.selectionStart(), cursor_.selectionEnd()))
            add(r, { 101, 119, 243, 95 });
    if (preeditLayout_) {
        for (const auto& r : preeditLayout_->selection(
                 cursor_.selectionStart(), cursor_.selectionStart() + int(preedit_.size())))
            add({ r.left(), r.bottom() - 1, r.width(), 1 }, { 255, 255, 255, 230 });
    }
    if (caretVisible_ && (!preeditLayout_ || preeditCaretVisible_)) {
        const auto r = displayLayout()->caret(
            preeditLayout_ ? cursor_.selectionStart() + preeditCursor_ : cursor_.position());
        const auto d=t.derivatives({r.x(),r.y()});
        const auto width = 1.0 / std::max(.001, canvas_->zoom() * std::hypot(d[0].x,d[0].y));
        add(r.adjusted(-width, -width, width, width), { 10, 12, 18, 230 });
        auto inner = r;
        inner.setWidth(width);
        add(inner, { 255, 255, 255, 255 });
    }
    canvas_->setTextQuads(std::move(quads));
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}
void TextController::placeOverlay()
{
    if (!active() || !layout_)
        return;
    overlay_->adjustSize();
    const auto map = mapping(*canvas_);
    const auto t = transform();
    const auto r = layout_->bounds();
    std::array<core::Vec2d, 4> corners { t.map({ 0, 0 }), t.map({ r.width(), 0 }),
        t.map({ 0, r.height() }), t.map({ r.width(), r.height() }) };
    double x = 1e20, y = 1e20;
    for (const auto& p : corners) {
        auto q = map.documentToLogicalViewport(p);
        x = std::min(x, q.x);
        y = std::min(y, q.y);
    }
    const auto area = workspace_->panelOverlay()->rect().adjusted(8, 8, -8, -8);
    overlay_->move(std::clamp(int(std::floor(x)), area.left(),
                       std::max(area.left(), area.right() - overlay_->width())),
        std::clamp(int(std::floor(y)) - overlay_->height() - 10, area.top(),
            std::max(area.top(), area.bottom() - overlay_->height())));
    overlay_->raise();
    workspace_->setContextOverlayInteractionRegion(overlay_->geometry());
    updateInputGeometry();
}
void TextController::clearComposition()
{
    preedit_.clear();
    preeditLayout_.reset();
    caretVisible_ = true;
    refreshPresentation();
}
void TextController::commitComposition()
{
    if (preedit_.isEmpty())
        return;
    QGuiApplication::inputMethod()->commit();
    // A platform context may not deliver synchronous commit. Never promote
    // speculative preedit ourselves; discard only that uncommitted part.
    if (!preedit_.isEmpty()) {
        QGuiApplication::inputMethod()->reset();
        clearComposition();
    }
}
void TextController::composition(QInputMethodEvent* e)
{
    if (!active())
        return;
    finishNumeric();
    // Incoming data is bounded before Qt allocation; normalizedText below
    // validates the resulting replacement (not the bytes it removes).
    if (e->commitString().toUtf8().size() + e->preeditString().toUtf8().size()
        > qsizetype(core::kMaximumTextBytes))
        throw std::length_error("Text composition exceeds 256 KiB limit");
    if (!e->commitString().isEmpty() || e->replacementLength() != 0) {
        const auto before = hint();
        const int base = cursor_.selectionStart();
        cursor_.removeSelectedText();
        if (e->replacementStart() != 0 || e->replacementLength() != 0) {
            const int start = std::clamp(
                base + e->replacementStart(), 0, layout_->document().characterCount() - 1);
            cursor_.setPosition(start);
            cursor_.setPosition(std::clamp(start + e->replacementLength(), 0,
                                    layout_->document().characterCount() - 1),
                QTextCursor::KeepAnchor);
        }
        cursor_.insertText(e->commitString(), QtTextLayout::format(insertion_));
        auto next = layout_->text();
        const auto after = hint();
        preedit_.clear();
        preeditLayout_.reset();
        mutate(std::move(next), before, after, QStringLiteral("Compose text"),
            typingGroup(QStringLiteral("Compose")));
    }
    preedit_ = e->preeditString();
    preeditLayout_.reset();
    preeditCursor_ = int(preedit_.size());
    preeditCaretVisible_ = true;
    for (const auto& a : e->attributes())
        if (a.type == QInputMethodEvent::Selection) {
            cursor_.setPosition(std::clamp(a.start, 0, layout_->document().characterCount() - 1));
            cursor_.setPosition(
                std::clamp(a.start + a.length, 0, layout_->document().characterCount() - 1),
                QTextCursor::KeepAnchor);
        }
    if (!preedit_.isEmpty()) {
        preeditLayout_ = std::make_unique<QtTextLayout>(layout_->text());
        QTextCursor visual(&preeditLayout_->document());
        visual.setPosition(cursor_.selectionStart());
        visual.setPosition(cursor_.selectionEnd(), QTextCursor::KeepAnchor);
        visual.insertText(preedit_, QtTextLayout::format(insertion_));
        (void)preeditLayout_
            ->text(); // Apply the same bounded layout limits before publishing preedit.
        for (const auto& a : e->attributes()) {
            if (a.type == QInputMethodEvent::Cursor) {
                preeditCursor_ = std::clamp(a.start, 0, int(preedit_.size()));
                preeditCaretVisible_ = a.length != 0;
            }
            if (a.type == QInputMethodEvent::TextFormat && a.start >= 0 && a.length > 0) {
                visual.setPosition(std::clamp(cursor_.selectionStart() + a.start, 0,
                    preeditLayout_->document().characterCount() - 1));
                visual.setPosition(std::clamp(cursor_.selectionStart() + a.start + a.length, 0,
                                       preeditLayout_->document().characterCount() - 1),
                    QTextCursor::KeepAnchor);
                visual.mergeCharFormat(qvariant_cast<QTextFormat>(a.value).toCharFormat());
            }
        }
    }
    caretVisible_ = true;
    refreshPresentation();
    QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    e->accept();
}
bool TextController::inputEvent(QEvent* event)
{
    if (event->type() == QEvent::InputMethodQuery) {
        auto* e = static_cast<QInputMethodQueryEvent*>(event);
        e->setValue(Qt::ImEnabled, active());
        if (!active())
            return true;
        const auto text = utf8(layout_->text().utf8);
        e->setValue(Qt::ImSurroundingText, text);
        e->setValue(Qt::ImCursorPosition, cursor_.position());
        e->setValue(Qt::ImAnchorPosition, cursor_.anchor());
        e->setValue(Qt::ImAbsolutePosition, cursor_.position());
        e->setValue(Qt::ImCurrentSelection, cursor_.selectedText());
        e->setValue(Qt::ImTextBeforeCursor, text.left(cursor_.position()));
        e->setValue(Qt::ImTextAfterCursor, text.mid(cursor_.position()));
        e->setValue(Qt::ImFont, QtTextLayout::format(insertion_).font());
        e->setValue(Qt::ImHints, int(Qt::ImhMultiLine));
        const auto caret = displayLayout()->caret(
            preeditLayout_ ? cursor_.selectionStart() + preeditCursor_ : cursor_.position());
        const auto map = mapping(*canvas_);
        const auto t = transform();
        QRectF rect;
        bool first = true;
        for (const auto& p :
            { caret.topLeft(), caret.topRight(), caret.bottomLeft(), caret.bottomRight() }) {
            const auto q = map.documentToLogicalViewport(t.map({ p.x(), p.y() }));
            const QRectF point(q.x, q.y, 1, 1);
            rect = first ? point : rect.united(point);
            first = false;
        }
        e->setValue(Qt::ImCursorRectangle, rect);
        e->setValue(Qt::ImAnchorRectangle, rect);
        event->accept();
        return true;
    }
    if (event->type() == QEvent::InputMethod && active()) {
        guardEdit([&] { composition(static_cast<QInputMethodEvent*>(event)); });
        return true;
    }
    return false;
}
} // namespace imageeditor::ui
