#pragma once

class QWidget;
class QMimeData;
class QPixmap;
class QPoint;

namespace imageeditor::ui {
// Layer payloads reference live document sessions and are application-local.
// Keep ordinary pointer/wheel delivery rather than entering a native OS drag.
void dragLayers(QWidget* source, const QMimeData*, const QPixmap&, const QPoint& globalPosition);
}
