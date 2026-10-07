#pragma once
#include "imageeditor/core/LayerTree.hpp"
#include <QColor>
#include <QIcon>
#include <QPalette>
namespace imageeditor::ui {
enum class ToolGlyph {
    Replace, Add, Subtract, Intersect, Transform, SelectionAdjustments, MergedVisible, ActiveLayer,
    FillSelection, FillContiguous, UnderMouse, Measure, LassoFreehand, LassoPolygonal, LassoMagnetic,
    SelectRectangle, SelectEllipse, ShapeRectangle, ShapeRoundedRectangle,
    ShapeEllipse, ShapeTriangle, ShapeLine, ShapePolygon, ShapeFill, ShapeStroke, Folder, Group, ClippingGroup, LayerRaster,
    SelectByColor, Crop, ShowSource, Chamfer, ResetCrop, AspectLock, FlipHorizontal, FlipVertical,
    QuickSelection, MagicWand, Cloning, CloneStamp, CloneHeal, SpotHeal, CloneSource,
    CurrentAndBelow, CloneAligned, FollowStrokeDirection, LocalBlur,
    Person, SocialX, Mail, Globe, ExternalLink, Refresh, Download, CheckCircle, InfoCircle, Close,
    Repository, LayerMask, Trash, NewLayer, AdjustmentLayer, Plus, Effects
};
// Original code-native glyphs, rendered and cached at the requested size/DPR.
QIcon toolGlyph(ToolGlyph glyph);
QIcon toolGlyph(ToolGlyph glyph, const QColor& ink);
// One palette for layer-label stripes and all labelled layer/folder glyphs.
QColor layerLabelColor(core::ColorLabel label);
// Cached, palette-aware tint of an existing code/SVG/PNG icon. No asset reload
// during normal repaint; existing QAction instances follow live theme changes.
QIcon themedIcon(const QString& resource, QPalette::ColorRole role = QPalette::Text);
QIcon themedIcon(const QString& resource, const QColor& ink);
}
