#pragma once
#include <QImage>
#include <QJsonArray>
#include "imageeditor/ui/DiffusionPreview.hpp"
namespace imageeditor::ui {
struct DiffusionInputCrop {
    QImage image;
    QTransform toDocument;
};
// Document-scale pixels clipped to the source canvas; never mutates the snapshot.
DiffusionInputCrop diffusionCropToCanvas(const QImage&, const QTransform&, QSize canvas);
QVector<DiffusionRegion> diffusionMapInputRegions(const QVector<DiffusionRegion>&,
    QSize fromSize, const QTransform& fromDocument, QSize toSize, const QTransform& toDocument);
// A visual reference only. Neither this diagram nor semantic region bounds
// replaces the original soft selection used to composite a fill result.
QImage diffusionRegionAnnotation(const QImage& base, const QJsonArray& regions, QSize outputSize);
QImage diffusionOutputLayout(const QImage& scene, QSize outputSize);
}
