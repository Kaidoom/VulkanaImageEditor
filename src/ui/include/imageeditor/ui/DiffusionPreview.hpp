#pragma once

#include "imageeditor/core/ViewportState.hpp"
#include <QImage>
#include <QRectF>
#include <QTransform>
#include <QVector>
#include <QWidget>
#include <functional>

namespace imageeditor::ui {
struct DiffusionRegion {
  QRectF bounds; // Normalized image coordinates, independent of inference
                 // resolution.
  QString label;
  QString id; // Stable identity when a cropped input view hides or remaps a box.
  QString description;
  QString destinationId; // Stable ID of an output location; empty means no transfer.
  bool targetOnly{}; // Locations never become independent model objects.
};

class DiffusionPreview final : public QWidget {
public:
  explicit DiffusionPreview(QWidget *parent = nullptr);
  enum class Alignment { Canvas, Center, Fit };
  void setImage(QImage image, bool preserveView = false);
  void setComparison(QImage reference, QTransform referenceToDocument = {},
                     QTransform resultToDocument = {});
  void setComparisonEnabled(bool enabled);
  void setComparisonAlignment(Alignment alignment);
  bool comparisonEnabled() const { return comparing_ && !reference_.isNull(); }
  void setComparisonSplit(double fraction);
  double comparisonSplit() const { return split_; }
  QRectF comparisonBounds() const;
  QRectF referenceRect() const;
  const QImage &image() const { return image_; }
  void setEmptyText(const QString &text) {
    emptyText_ = text;
    update();
  }
  void fitView();
  void actualSize();
  double zoom() const { return view_.zoom(); }
  QRectF imageRect() const;
  void setRegionEditing(bool enabled);
  void setRegions(QVector<DiffusionRegion> regions);
  const QVector<DiffusionRegion> &regions() const { return regions_; }
  int selectedRegion() const { return selected_; }
  void setSelectedRegion(int index);
  void removeSelectedRegion();
  void setSelectedRegionLabel(const QString &label);
  void setSelectedRegionDescription(const QString &description);
  void setNewRegionsAreTargets(bool value) { newRegionsAreTargets_ = value; }
  std::function<void()> onViewChanged;
  std::function<void()> onRegionsChanged;
  std::function<void()> onRegionEditFinished;
  std::function<void(int)> onRegionSelected;

protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void keyPressEvent(QKeyEvent *) override;

private:
  QPointF imagePoint(QPointF) const;
  QRectF regionRect(const DiffusionRegion &) const;
  int handleAt(QPointF) const;
  void updateDrag(QPointF);
  void changed();
  QRectF sceneBounds() const;
  QTransform resultPlacement() const;
  QTransform referencePlacement() const;
  QTransform sceneToViewport() const;
  double dividerX() const;
  QImage image_, reference_;
  QString emptyText_{"No image"};
  QTransform referenceToDocument_, resultToDocument_;
  Alignment alignment_{Alignment::Canvas};
  bool comparing_{};
  double split_{.5};
  core::ViewportState view_, oldView_;
  QVector<DiffusionRegion> regions_;
  QVector<DiffusionRegion> oldRegions_;
  int selected_{-1}, resizeCorner_{-1};
  bool editing_{}, fitted_{true};
  bool newRegionsAreTargets_{};
  QPointF anchor_, start_, panStart_;
  QRectF oldBounds_;
  enum class Drag { None, Draw, Move, Resize, Pan, Compare } drag_{Drag::None};
};
} // namespace imageeditor::ui
