#pragma once

#include <QImage>
#include <QPoint>
#include <QString>
#include <QVector>

namespace imageeditor::ui {

struct DiffusionLayerPart {
    QString name;
    QPoint offset;
    QImage pixels;
};

// Positions are in this stack's pixel grid. Parts are ordered back to front.
struct DiffusionLayerStack {
    QSize extent;
    QVector<DiffusionLayerPart> parts;

    static DiffusionLayerStack fromFullImages(const QVector<QImage>& images);
    QImage inspect(qsizetype index) const;
};

} // namespace imageeditor::ui
