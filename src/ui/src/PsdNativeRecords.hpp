#pragma once
#include "PsdWriter.hpp"
namespace imageeditor::ui::psdwrite {
// Generated from current native data, never imported opaque descriptors.
QByteArray textRecord(const core::Layer &, QStringList *fonts = nullptr,
                      QStringList *differences = nullptr);
QMap<QByteArray, QByteArray> shapeRecords(const core::Layer &,
                                          core::CanvasSpec);
QByteArray effectRecord(const core::Layer &);
QMap<QByteArray,QByteArray> adjustmentRecord(const core::Layer&);
bool similarity(const core::AffineTransform &, double *scale = nullptr);
} // namespace imageeditor::ui::psdwrite
