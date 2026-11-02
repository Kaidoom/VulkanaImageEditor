#pragma once
#include <QImage>
#include <QJsonArray>
namespace imageeditor::ui {
// A visual reference only. Neither this diagram nor semantic region bounds
// replaces the original soft selection used to composite a fill result.
QImage diffusionRegionAnnotation(const QImage& base, const QJsonArray& regions, QSize outputSize);
}
