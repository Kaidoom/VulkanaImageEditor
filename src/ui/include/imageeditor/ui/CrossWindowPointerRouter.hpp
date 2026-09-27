#pragma once

#include <QObject>
#include <QPointer>

#include <cstdint>

class QEvent;
class QMouseEvent;
class QWidget;
class QWindow;

namespace imageeditor::ui {

// Opt-in preparation for controls that finish text editing or switch document
// before a pointer action. Run before capture begins so cleanup of the previous
// interaction cannot cancel the new pointer gesture.
class PointerPressPreparation {
public:
    virtual ~PointerPressPreparation() = default;
    virtual void preparePointerPress(const QMouseEvent& event) = 0;
};

// QWidget's implicit press grab is maintained inside QWidgetWindow. An embedded
// QWindow is a separate native surface, so a platform may deliver the next move
// or release directly to that child window and bypass QWidgetWindow entirely.
// This router bridges that one event-routing seam in both directions. It never
// takes a platform grab of its own.
class CrossWindowPointerRouter final : public QObject {
public:
    enum class CaptureDomain {
        None,
        Widget,
        NativeWindow,
    };

    CrossWindowPointerRouter(QWidget* widgetRoot, QWindow* nativeWindow,
        QWidget* nativeContainer, QObject* parent = nullptr);
    ~CrossWindowPointerRouter() override;

    [[nodiscard]] CaptureDomain captureDomain() const noexcept { return captureDomain_; }
    [[nodiscard]] QObject* captureOwner() const noexcept;
    [[nodiscard]] std::uint64_t routedEventCount() const noexcept { return routedEventCount_; }
    [[nodiscard]] std::uint64_t retainedUngrabCount() const noexcept
    {
        return retainedUngrabCount_;
    }
    [[nodiscard]] std::uint64_t dragTakeoverCount() const noexcept
    {
        return dragTakeoverCount_;
    }
    [[nodiscard]] std::uint64_t repairedButtonStateCount() const noexcept
    {
        return repairedButtonStateCount_;
    }
    [[nodiscard]] std::uint64_t popupTakeoverCount() const noexcept
    {
        return popupTakeoverCount_;
    }

    void cancelCapture();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] bool widgetBelongsToRoot(const QWidget* widget) const noexcept;
    [[nodiscard]] bool windowBelongsToRoot(const QWindow* window) const noexcept;
    [[nodiscard]] QWindow* dispatchWindowForWidget(const QWidget* widget) const noexcept;
    [[nodiscard]] bool eventComesFromWidgetSide(const QObject* watched) const noexcept;
    [[nodiscard]] bool objectBelongsToCaptureScope(const QObject* watched) const noexcept;
    [[nodiscard]] bool eventCrossesCaptureBoundary(const QObject* watched) const noexcept;
    [[nodiscard]] bool popupBelongsToRoot(const QWidget* popup) const noexcept;
    [[nodiscard]] QWidget* activeRootPopup() const noexcept;
    [[nodiscard]] bool isCaptureCancellationEvent(
        const QObject* watched, const QEvent* event) const noexcept;
    void beginWidgetCapture(QWidget* receiver, const QMouseEvent& event);
    void beginNativeCapture(const QMouseEvent& event);
    bool routeMouseEvent(QObject* sourceObject, QWindow* destination, QMouseEvent& source);
    void cancelWidgetCapture();
    void cancelNativeCapture();
    void releaseExplicitWidgetGrabIfOwned(QWidget* expectedGrabber = nullptr);
    void finishCapture(const char* reason);
    void logRoute(const QObject* source, const QWindow* destination,
        const QMouseEvent& event) const;

    QPointer<QWidget> widgetRoot_;
    QPointer<QWindow> nativeWindow_;
    QPointer<QWidget> nativeContainer_;
    QPointer<QWidget> widgetCaptureOwner_;
    QPointer<QWindow> widgetCaptureWindow_;
    CaptureDomain captureDomain_ {CaptureDomain::None};
    Qt::MouseButtons pressedButtons_ {Qt::NoButton};
    bool retainedSeamUngrab_ {false};
    bool notifyingWidgetCancellation_ {false};
    std::uint32_t routingDepth_ {0};
    std::uint64_t routedEventCount_ {0};
    std::uint64_t retainedUngrabCount_ {0};
    std::uint64_t dragTakeoverCount_ {0};
    std::uint64_t repairedButtonStateCount_ {0};
    std::uint64_t popupTakeoverCount_ {0};
};

} // namespace imageeditor::ui
