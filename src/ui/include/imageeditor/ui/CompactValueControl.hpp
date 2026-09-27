#pragma once

#include <QDoubleSpinBox>

#include <utility>
#include <functional>

class QMouseEvent;
class QPaintEvent;
class QFocusEvent;
class QWheelEvent;

namespace imageeditor::ui {

// A compact, single-value editor that combines absolute slider interaction,
// direct text entry (double-click or type), and native spin buttons. It keeps
// one Qt input receiver so press/drag/release remains coherent across the
// embedded Vulkan-window boundary.
class CompactValueControl final : public QDoubleSpinBox {
public:
    explicit CompactValueControl(QWidget* parent = nullptr);

    [[nodiscard]] QRect valueFieldRect() const;
    [[nodiscard]] QRect progressTrackRect() const;
    [[nodiscard]] double valueFromPosition(qreal localX) const noexcept;
    [[nodiscard]] bool isSliding() const noexcept { return sliding_; }
    [[nodiscard]] bool isManualEntryActive() const noexcept;
    [[nodiscard]] bool interactionActive() const noexcept { return interactionActive_; }
    // Explicit owner boundary (target change, cancellation, file operation).
    // Ends internal slide/manual ownership so a late release cannot edit a new target.
    // Panel gesture commits can retain focus for subsequent keyboard stepping.
    void finishEditing(bool commit = true, bool keepFocus = false);
    // Optional document-history boundary: one drag, numeric entry, or held
    // stepper is one adjustment. Programmatic setValue does not start one.
    std::function<void()> onInteractionStarted;
    std::function<void()> onInteractionFinished;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void stepBy(int steps) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class EntryMode {
        Slide,
        Manual,
    };

    [[nodiscard]] bool isValueFieldPosition(const QPointF& position) const;
    [[nodiscard]] bool isManualEntryKey(const QKeyEvent& event) const;
    [[nodiscard]] bool shouldClaimEntryShortcut(
        const QKeyEvent& event) const;
    [[nodiscard]] bool canBeginManualEntry(const QString& replacement) const;
    [[nodiscard]] std::pair<int, int> numericTextRange() const;
    void clampTextSelectionToNumber();
    void clampTextCursorToNumber(int position);
    void selectNumericText();
    void clearTextSelection();
    void prepareManualEntryForKey(const QKeyEvent& event);
    void commitAndExitManualEntry();
    void enterManualEntry();
    void enterSlideMode();
    void beginSlide(QMouseEvent* event);
    void updateSlide(const QPointF& position);
    void finishSlide();
    void beginInteraction();
    void finishInteraction();

    bool sliding_ {false};
    bool interactionActive_ {false};
    EntryMode entryMode_ {EntryMode::Slide};
    bool clampingSelection_ {false};
    bool clampingCursor_ {false};
};

} // namespace imageeditor::ui
