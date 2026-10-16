#pragma once
#include <QApplication>
#include <QMouseEvent>

class LayerDragTestApplication final : public QApplication {
public:
    using QApplication::QApplication;
    bool scriptedDrag {false};

    bool notify(QObject* receiver, QEvent* event) override
    {
        // Scripted Qt presses do not hold the physical mouse button. Native
        // exposure/grab notifications can therefore insert an unrelated hover
        // (buttons=None) into the test gesture. Ignore only those platform
        // hovers; all scripted moves, cancellation, wheels and releases still
        // traverse the normal application filters and widget/native surfaces.
        if (scriptedDrag && event->spontaneous() && event->type() == QEvent::MouseMove
            && !(static_cast<QMouseEvent*>(event)->buttons() & Qt::LeftButton)) return true;
        return QApplication::notify(receiver,event);
    }
};
