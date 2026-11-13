#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/DiffusionPreview.hpp"
#include <QUuid>
#include "imageeditor/ui/Theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>

namespace imageeditor::ui {
DiffusionPreview::DiffusionPreview(QWidget *parent) : QWidget(parent) {
  setObjectName("DiffusionPreview");
  setMinimumSize(200, 160);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setLayoutDirection(Qt::LeftToRight);
}
void DiffusionPreview::setImage(QImage image, bool preserveView) {
  const bool retain = preserveView && image_.size() == image.size();
  image_ = std::move(image);
  image_.setDevicePixelRatio(1);
  drag_ = Drag::None;
  if (retain)
    update();
  else
    fitView();
}
void DiffusionPreview::setComparison(QImage reference,
                                     QTransform referenceToDocument,
                                     QTransform resultToDocument) {
  const auto old = sceneBounds();
  reference_ = std::move(reference);
  reference_.setDevicePixelRatio(1);
  referenceToDocument_ = referenceToDocument;
  resultToDocument_ = resultToDocument;
  if (old != sceneBounds() && fitted_)
    fitView();
  else
    update();
}
void DiffusionPreview::setComparisonEnabled(bool enabled) {
  if (comparing_ == enabled)
    return;
  comparing_ = enabled;
  fitView();
}
void DiffusionPreview::setComparisonAlignment(Alignment alignment) {
  if (alignment_ == alignment)
    return;
  alignment_ = alignment;
  fitView();
}
void DiffusionPreview::setComparisonSplit(double fraction) {
  split_ = std::clamp(fraction, 0., 1.);
  update();
}
QTransform DiffusionPreview::resultPlacement() const {
  return comparisonEnabled() && alignment_ == Alignment::Canvas
             ? resultToDocument_
             : QTransform{};
}
QTransform DiffusionPreview::referencePlacement() const {
  if (alignment_ == Alignment::Canvas)
    return referenceToDocument_;
  double scale = 1;
  if (alignment_ == Alignment::Fit && !reference_.isNull())
    scale = std::min(double(image_.width()) / reference_.width(),
                     double(image_.height()) / reference_.height());
  return QTransform(scale, 0, 0, scale,
                    (image_.width() - reference_.width() * scale) / 2,
                    (image_.height() - reference_.height() * scale) / 2);
}
QRectF DiffusionPreview::sceneBounds() const {
  const auto result =
      resultPlacement().mapRect(QRectF(QPointF{}, image_.size()));
  return comparisonEnabled() ? result.united(referencePlacement().mapRect(
                                   QRectF(QPointF{}, reference_.size())))
                             : result;
}
QTransform DiffusionPreview::sceneToViewport() const {
  const auto scene = sceneBounds();
  const auto origin =
      view_.documentToViewport({0, 0}, {scene.width(), scene.height()},
                               {double(width()), double(height())});
  return QTransform(view_.zoom(), 0, 0, view_.zoom(),
                    origin.x - scene.x() * view_.zoom(),
                    origin.y - scene.y() * view_.zoom());
}
QRectF DiffusionPreview::comparisonBounds() const {
  return sceneToViewport().mapRect(sceneBounds());
}
QRectF DiffusionPreview::referenceRect() const {
  return (referencePlacement() * sceneToViewport())
      .mapRect(QRectF(QPointF{}, reference_.size()));
}
double DiffusionPreview::dividerX() const {
  const auto visible = comparisonBounds().intersected(QRectF(rect()));
  return visible.left() + visible.width() * split_;
}
void DiffusionPreview::fitView() {
  if (image_.isNull()) {
    update();
    return;
  }
  fitted_ = true;
  const auto scene = sceneBounds();
  view_.fit({scene.width(), scene.height()},
            {double(width()), double(height())}, 16);
  update();
  if (onViewChanged)
    onViewChanged();
}
void DiffusionPreview::actualSize() {
  fitted_ = false;
  view_.reset100Percent();
  update();
  if (onViewChanged)
    onViewChanged();
}
QRectF DiffusionPreview::imageRect() const {
  if (image_.isNull())
    return {};
  return (resultPlacement() * sceneToViewport())
      .mapRect(QRectF(QPointF{}, image_.size()));
}
QPointF DiffusionPreview::imagePoint(QPointF point) const {
  const auto r = imageRect();
  if (r.isEmpty())
    return {};
  return {std::clamp((point.x() - r.x()) / r.width(), 0., 1.),
          std::clamp((point.y() - r.y()) / r.height(), 0., 1.)};
}
QRectF DiffusionPreview::regionRect(const DiffusionRegion &region) const {
  const auto r = imageRect();
  const auto b = region.bounds;
  return {r.x() + b.x() * r.width(), r.y() + b.y() * r.height(),
          b.width() * r.width(), b.height() * r.height()};
}
void DiffusionPreview::setRegionEditing(bool enabled) {
  editing_ = enabled;
  drag_ = Drag::None;
  update();
}
void DiffusionPreview::setRegions(QVector<DiffusionRegion> regions) {
  regions_ = std::move(regions);
  for (auto &region : regions_) {
    region.bounds = region.bounds.normalized().intersected(QRectF(0, 0, 1, 1));
    if (region.id.isEmpty()) region.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  }
  selected_ = std::min(selected_, int(regions_.size()) - 1);
  update();
}
void DiffusionPreview::setSelectedRegion(int index) {
  selected_ = index >= 0 && index < regions_.size() ? index : -1;
  update();
  if (onRegionSelected)
    onRegionSelected(selected_);
}
void DiffusionPreview::removeSelectedRegion() {
  if (selected_ < 0 || selected_ >= regions_.size())
    return;
  regions_.removeAt(selected_);
  setSelectedRegion(std::min(selected_, int(regions_.size()) - 1));
  changed();
}
void DiffusionPreview::setSelectedRegionLabel(const QString &label) {
  if (selected_ < 0 || selected_ >= regions_.size())
    return;
  regions_[selected_].label = label.trimmed().left(200);
  changed();
}
void DiffusionPreview::setSelectedRegionDescription(const QString &description) {
  if (selected_ < 0 || selected_ >= regions_.size() || regions_[selected_].targetOnly) return;
  regions_[selected_].description = description.left(2000);
  changed();
}
void DiffusionPreview::changed() {
  update();
  if (onRegionsChanged)
    onRegionsChanged();
}
int DiffusionPreview::handleAt(QPointF point) const {
  if (!editing_ || selected_ < 0 || selected_ >= regions_.size())
    return -1;
  const auto r = regionRect(regions_[selected_]);
  const std::array points{r.topLeft(), r.topRight(), r.bottomRight(),
                          r.bottomLeft()};
  for (int i = 0; i < 4; ++i)
    if (QLineF(point, points[std::size_t(i)]).length() < 8)
      return i;
  return -1;
}
void DiffusionPreview::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), themeColor(ThemeColor::Canvas));
  if (image_.isNull()) {
    p.setPen(themeColor(ThemeColor::SecondaryText));
    p.drawText(rect().adjusted(16, 16, -16, -16),
               Qt::AlignCenter | Qt::TextWordWrap, emptyText_);
    return;
  }
  const auto r = comparisonBounds();
  p.save();
  p.setClipRect(r);
  p.fillRect(rect(), themeColor(ThemeColor::CheckerLight));
  for (int y = 0; y < height(); y += 12)
    for (int x = 0; x < width(); x += 12)
      if ((x / 12 + y / 12) % 2)
        p.fillRect(QRect(x, y, 12, 12), themeColor(ThemeColor::CheckerDark));
  p.setRenderHint(QPainter::SmoothPixmapTransform, view_.zoom() < 1);
  const auto draw = [&](const QImage &image, const QTransform &placement,
                        const QRectF &clip) {
    p.save();
    p.setClipRect(clip, Qt::IntersectClip);
    p.setWorldTransform(placement * sceneToViewport());
    p.drawImage(QPointF{}, image);
    p.restore();
  };
  if (comparisonEnabled()) {
    const auto split = dividerX();
    draw(reference_, referencePlacement(),
         QRectF(r.left(), r.top(), split - r.left(), r.height()));
    draw(image_, resultPlacement(),
         QRectF(split, r.top(), r.right() - split, r.height()));
  } else
    draw(image_, resultPlacement(), r);
  p.restore();
  if (comparisonEnabled()) {
    const auto visible = r.intersected(QRectF(rect()));
    const double x = dividerX(), y = visible.center().y();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 180), 3));
    p.drawLine(QPointF(x, visible.top()), QPointF(x, visible.bottom()));
    p.setPen(QPen(themeColor(ThemeColor::Accent), 1));
    p.drawLine(QPointF(x, visible.top()), QPointF(x, visible.bottom()));
    p.setBrush(themeColor(ThemeColor::Surface));
    p.drawRoundedRect(QRectF(x - 11, y - 18, 22, 36), 5, 5);
    p.setPen(QPen(themeColor(ThemeColor::Text), 1.5));
    p.drawPolyline(QPolygonF{{x - 3, y - 5}, {x - 7, y}, {x - 3, y + 5}});
    p.drawPolyline(QPolygonF{{x + 3, y - 5}, {x + 7, y}, {x + 3, y + 5}});
  }
  if (!editing_)
    return;
  for (int i = 0; i < regions_.size(); ++i) {
    const auto box = regionRect(regions_[i]);
    p.setPen(QPen(themeColor(ThemeColor::Accent), i == selected_ ? 2 : 1));
    p.setBrush(QColor(255, 140, 70, 18));
    p.drawRect(box);
    QString title = regions_[i].label.isEmpty()
                              ? QString("Region%1").arg(i + 1)
                              : regions_[i].label;
    if (regions_[i].targetOnly) title += i18n::text("diffusion.ui.target");
    const auto label =
        p.fontMetrics().boundingRect(title).adjusted(-5, -3, 5, 3);
    QRectF labelBox(box.topLeft(), label.size());
    labelBox.moveTop(std::max(0., box.top() - labelBox.height()));
    p.fillRect(labelBox, themeColor(ThemeColor::Surface));
    p.setPen(themeColor(ThemeColor::Text));
    p.drawText(labelBox, Qt::AlignCenter, title);
    if (i == selected_) {
      p.setBrush(themeColor(ThemeColor::Surface));
      p.setPen(themeColor(ThemeColor::Accent));
      for (auto point :
           {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
        p.drawRect(QRectF(point - QPointF(3, 3), QSizeF(6, 6)));
    }
  }
}
void DiffusionPreview::mousePressEvent(QMouseEvent *e) {
  if (image_.isNull() || drag_ != Drag::None) {
    e->ignore();
    return;
  }
  setFocus(Qt::MouseFocusReason);
  if (e->button() == Qt::MiddleButton) {
    drag_ = Drag::Pan;
    oldView_ = view_;
    panStart_ = e->position();
    setCursor(Qt::ClosedHandCursor);
    e->accept();
    return;
  }
  if (e->button() == Qt::LeftButton && comparisonEnabled() &&
      comparisonBounds().contains(e->position())) {
    drag_ = Drag::Compare;
    updateDrag(e->position());
    e->accept();
    return;
  }
  if (e->button() != Qt::LeftButton || !editing_ ||
      !imageRect().contains(e->position())) {
    e->ignore();
    return;
  }
  oldRegions_ = regions_;
  start_ = imagePoint(e->position());
  resizeCorner_ = handleAt(e->position());
  if (resizeCorner_ >= 0 && !e->modifiers().testFlag(Qt::ShiftModifier)) {
    drag_ = Drag::Resize;
    oldBounds_ = regions_[selected_].bounds;
    const std::array corners{oldBounds_.bottomRight(), oldBounds_.bottomLeft(),
                             oldBounds_.topLeft(), oldBounds_.topRight()};
    anchor_ = corners[std::size_t(resizeCorner_)];
  } else {
    int found = -1;
    if (!e->modifiers().testFlag(Qt::ShiftModifier))
      for (int i = int(regions_.size()) - 1; i >= 0; --i)
        if (regions_[i].bounds.contains(start_)) {
          found = i;
          break;
        }
    if (found >= 0) {
      setSelectedRegion(found);
      oldBounds_ = regions_[selected_].bounds;
      drag_ = Drag::Move;
    } else {
      anchor_ = start_;
      int number = 1;
      while (std::any_of(regions_.begin(), regions_.end(), [number](const auto &region) {
        return region.label == QString("Region%1").arg(number);
      }))
        ++number;
      regions_.append({QRectF(start_, QSizeF()),
                       QString("Region%1").arg(number), QUuid::createUuid().toString(QUuid::WithoutBraces), {}, {}, newRegionsAreTargets_});
      setSelectedRegion(int(regions_.size()) - 1);
      drag_ = Drag::Draw;
    }
  }
  e->accept();
}
void DiffusionPreview::updateDrag(QPointF position) {
  if (drag_ == Drag::Compare) {
    const auto visible = comparisonBounds().intersected(QRectF(rect()));
    if (visible.width() > 0)
      setComparisonSplit((position.x() - visible.left()) / visible.width());
    return;
  }
  if (drag_ == Drag::Pan) {
    const auto delta = position - panStart_;
    view_ = oldView_;
    view_.panBy({delta.x(), delta.y()});
    fitted_ = false;
    update();
    if (onViewChanged)
      onViewChanged();
    return;
  }
  if (selected_ < 0 || selected_ >= regions_.size())
    return;
  const auto point = imagePoint(position);
  if (drag_ == Drag::Draw || drag_ == Drag::Resize)
    regions_[selected_].bounds = QRectF(anchor_, point).normalized();
  if (drag_ == Drag::Move) {
    auto moved = oldBounds_.translated(point - start_);
    moved.moveLeft(std::clamp(moved.left(), 0., 1. - moved.width()));
    moved.moveTop(std::clamp(moved.top(), 0., 1. - moved.height()));
    regions_[selected_].bounds = moved;
  }
  changed();
}
void DiffusionPreview::mouseMoveEvent(QMouseEvent *e) {
  if (drag_ != Drag::None) {
    updateDrag(e->position());
    e->accept();
    return;
  }
  if (comparisonEnabled()) {
    setCursor(std::abs(e->position().x() - dividerX()) < 12 ? Qt::SplitHCursor
                                                            : Qt::ArrowCursor);
    return;
  }
  const int handle = handleAt(e->position());
  setCursor(handle >= 0
                ? (handle % 2 ? Qt::SizeBDiagCursor : Qt::SizeFDiagCursor)
            : editing_ ? Qt::CrossCursor
                       : Qt::ArrowCursor);
}
void DiffusionPreview::mouseReleaseEvent(QMouseEvent *e) {
  if ((e->button() == Qt::MiddleButton && drag_ == Drag::Pan) ||
      (e->button() == Qt::LeftButton && drag_ != Drag::None &&
       drag_ != Drag::Pan)) {
    const bool edited = drag_ == Drag::Draw || drag_ == Drag::Move || drag_ == Drag::Resize;
    updateDrag(e->position());
    if (drag_ == Drag::Draw && selected_ >= 0 &&
        (regions_[selected_].bounds.width() * image_.width() < 2 ||
         regions_[selected_].bounds.height() * image_.height() < 2))
      removeSelectedRegion();
    drag_ = Drag::None;
    unsetCursor();
    changed();
    if (edited && onRegionEditFinished) onRegionEditFinished();
    e->accept();
    return;
  }
  QWidget::mouseReleaseEvent(e);
}
void DiffusionPreview::wheelEvent(QWheelEvent *e) {
  if (image_.isNull() || drag_ != Drag::None || e->angleDelta().y() == 0) {
    e->ignore();
    return;
  }
  view_.zoomAround({e->position().x(), e->position().y()},
                   std::pow(1.16, double(e->angleDelta().y()) / 120.),
                   {sceneBounds().width(), sceneBounds().height()},
                   {double(width()), double(height())});
  fitted_ = false;
  update();
  if (onViewChanged)
    onViewChanged();
  e->accept();
}
void DiffusionPreview::resizeEvent(QResizeEvent *e) {
  if (fitted_)
    fitView();
  QWidget::resizeEvent(e);
}
void DiffusionPreview::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Escape && drag_ != Drag::None) {
    if (drag_ == Drag::Pan)
      view_ = oldView_;
    else if (drag_ != Drag::Compare) {
      regions_ = oldRegions_;
      selected_ = std::min(selected_, int(regions_.size()) - 1);
    }
    drag_ = Drag::None;
    unsetCursor();
    changed();
    e->accept();
    return;
  }
  if (editing_ &&
      (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)) {
    removeSelectedRegion();
    e->accept();
    return;
  }
  if (e->key() == Qt::Key_0) {
    fitView();
    e->accept();
    return;
  }
  if (e->key() == Qt::Key_1) {
    actualSize();
    e->accept();
    return;
  }
  QWidget::keyPressEvent(e);
}
} // namespace imageeditor::ui
