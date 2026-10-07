#include "imageeditor/ui/ColorPicker.hpp"
#include <QEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace imageeditor::ui {
class ColorField final : public QWidget {
public:
    ColorField(ColorPicker& picker, bool hue, QWidget* parent)
        : QWidget(parent), picker_(picker), hue_(hue) {
        setObjectName(hue ? "ColorHueStrip" : "ColorSaturationValue");
        setAccessibleName(hue ? tr("Hue") : tr("Saturation and brightness"));
        setToolTip(hue ? tr("Hue") : tr("Saturation left to right; brightness bottom to top"));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::CrossCursor);
        if (hue) setFixedWidth(30);
        else setMinimumWidth(90);
        setMinimumHeight(100);
        setSizePolicy(hue ? QSizePolicy::Fixed : QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QSize sizeHint() const override { return {hue_ ? 30 : 220, 210}; }
protected:
    QRectF fieldRect() const {
        return QRectF(rect()).adjusted(hue_ ? 6 : 5, 5, hue_ ? -6 : -5, -5);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto area = fieldRect();
        QPainterPath clip; clip.addRoundedRect(area, 5, 5);
        painter.save(); painter.setClipPath(clip);
        if (hue_) {
            QLinearGradient gradient(area.topLeft(), area.bottomLeft());
            for (int i = 0; i <= 6; ++i)
                gradient.setColorAt(i / 6.0, QColor::fromHsvF(float(1.0 - i / 6.0), 1, 1));
            painter.fillRect(area, gradient);
        } else {
            QLinearGradient saturation(area.topLeft(), area.topRight());
            saturation.setColorAt(0, Qt::white);
            saturation.setColorAt(1, QColor::fromHsvF(float(picker_.hue_), 1, 1));
            painter.fillRect(area, saturation);
            QLinearGradient value(area.topLeft(), area.bottomLeft());
            value.setColorAt(0, QColor(0, 0, 0, 0)); value.setColorAt(1, Qt::black);
            painter.fillRect(area, value);
        }
        painter.restore();
        painter.setPen(QPen(palette().color(hasFocus() ? QPalette::Highlight : QPalette::Mid), 1));
        painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(area, 5, 5);
        if (hue_) {
            const auto y = area.top() + (1.0 - picker_.hue_) * area.height();
            painter.setPen(QPen(QColor(0, 0, 0, 190), 1)); painter.setBrush(Qt::white);
            painter.drawPolygon(QPolygonF{{0, y - 4}, {5, y}, {0, y + 4}});
            painter.drawPolygon(QPolygonF{{qreal(width()), y - 4}, {qreal(width() - 5), y}, {qreal(width()), y + 4}});
        } else {
            const QPointF point(area.left() + picker_.saturation_ * area.width(),
                                area.top() + (1 - picker_.value_) * area.height());
            painter.setPen(QPen(QColor(0, 0, 0, 160), 4)); painter.drawEllipse(point, 5, 5);
            painter.setPen(QPen(Qt::white, 2)); painter.drawEllipse(point, 5, 5);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) { QWidget::mousePressEvent(event); return; }
        setFocus(Qt::MouseFocusReason);
        start_ = {picker_.hue_, picker_.saturation_, picker_.value_};
        startColor_ = picker_.color_;
        dragging_ = true; applyPoint(event->position()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_) { QWidget::mouseMoveEvent(event); return; }
        applyPoint(event->position()); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !dragging_) { QWidget::mouseReleaseEvent(event); return; }
        applyPoint(event->position()); dragging_ = false; event->accept();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Escape && dragging_) { cancel(); event->accept(); return; }
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
        auto h = picker_.hue_, s = picker_.saturation_, v = picker_.value_;
        switch (event->key()) {
        case Qt::Key_Left: if (hue_) h -= step / 360; else s -= step / 255; break;
        case Qt::Key_Right: if (hue_) h += step / 360; else s += step / 255; break;
        case Qt::Key_Up: if (hue_) h += step / 360; else v += step / 255; break;
        case Qt::Key_Down: if (hue_) h -= step / 360; else v -= step / 255; break;
        default: QWidget::keyPressEvent(event); return;
        }
        picker_.setHsv(h, s, v); event->accept();
    }
    bool event(QEvent* event) override {
        if (event->type() == QEvent::ShortcutOverride && dragging_
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            event->accept(); return true;
        }
        if (event->type() == QEvent::TouchCancel || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::FocusOut) cancel();
        return QWidget::event(event);
    }
private:
    void applyPoint(QPointF point) {
        const auto area = fieldRect();
        const auto x = std::clamp((point.x() - area.left()) / area.width(), 0.0, 1.0);
        const auto y = std::clamp((point.y() - area.top()) / area.height(), 0.0, 1.0);
        picker_.setHsv(hue_ ? 1 - y : picker_.hue_, hue_ ? picker_.saturation_ : x,
                      hue_ ? picker_.value_ : 1 - y);
    }
    void cancel() {
        if (!dragging_) return;
        dragging_ = false;
        picker_.hue_ = start_[0]; picker_.saturation_ = start_[1]; picker_.value_ = start_[2];
        picker_.publish(startColor_);
    }
    ColorPicker& picker_;
    bool hue_, dragging_ {};
    std::array<qreal, 3> start_ {};
    QColor startColor_;
};

ColorPicker::ColorPicker(QWidget* parent) : QWidget(parent) {
    setObjectName("ColorPicker");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(6);
    auto* fields = new QHBoxLayout;
    fields->setSpacing(4);
    plane_ = new ColorField(*this, false, this); hueStrip_ = new ColorField(*this, true, this);
    fields->addWidget(plane_, 1); fields->addWidget(hueStrip_);
    layout->addLayout(fields, 1);
    manualInputs_ = new QWidget(this);
    manualInputs_->setObjectName("ColorManualInputs");
    auto* manualLayout = new QVBoxLayout(manualInputs_);
    manualLayout->setContentsMargins(0, 0, 0, 0); manualLayout->setSpacing(6);
    auto* numbers = new QGridLayout; numbers->setHorizontalSpacing(4); numbers->setVerticalSpacing(4);
    for (int row = 0; row < 2; ++row) for (int i = 0; i < 3; ++i) {
        const auto label = QString(row ? "HSV" : "RGB").mid(i, 1);
        auto* spin = new QSpinBox(this);
        spin->setObjectName("Color" + label);
        spin->setAccessibleName(row ? QStringList{tr("Hue"), tr("Saturation"), tr("Brightness")}[i]
                                   : QStringList{tr("Red"), tr("Green"), tr("Blue")}[i]);
        spin->setRange(0, row ? (i == 0 ? 360 : 100) : 255);
        spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        (row ? hsv_ : rgb_)[size_t(i)] = spin;
        numbers->addWidget(new QLabel(label, this), row, i * 2);
        numbers->addWidget(spin, row, i * 2 + 1);
        numbers->setColumnStretch(i * 2 + 1, 1);
        connect(spin, &QSpinBox::valueChanged, this, [this, row] {
            if (row) setHsv(hsv_[0]->value() / 360.0, hsv_[1]->value() / 100.0, hsv_[2]->value() / 100.0);
            else {
                const QColor color(rgb_[0]->value(), rgb_[1]->value(), rgb_[2]->value(), color_.alpha());
                const auto before = color_; setColor(color);
                if (color_ != before && onColorChanged) onColorChanged(color_);
            }
        });
    }
    manualLayout->addLayout(numbers);
    auto* last = new QHBoxLayout; last->setSpacing(4);
    hex_ = new QLineEdit(this); hex_->setObjectName("ColorHex"); hex_->setAccessibleName(tr("Hex color"));
    hex_->setToolTip(tr("#RRGGBB or #RRGGBBAA"));
    hex_->setMaxLength(9); hex_->setMinimumWidth(0);
    last->addWidget(hex_, 1);
    alphaRow_ = new QWidget(this);
    auto* alphaLayout = new QHBoxLayout(alphaRow_); alphaLayout->setContentsMargins(0, 0, 0, 0); alphaLayout->setSpacing(4);
    alpha_ = new QSpinBox(alphaRow_); alpha_->setObjectName("ColorAlpha"); alpha_->setAccessibleName(tr("Alpha"));
    alpha_->setRange(0, 255);
    alphaLayout->addWidget(new QLabel(tr("A"), alphaRow_)); alphaLayout->addWidget(alpha_);
    last->addWidget(alphaRow_); manualLayout->addLayout(last);
    layout->addWidget(manualInputs_);
    connect(alpha_, &QSpinBox::valueChanged, this, [this](int alpha) {
        auto color = color_; color.setAlpha(alpha); publish(color);
    });
    connect(hex_, &QLineEdit::editingFinished, this, [this] {
        const auto text = hex_->text().trimmed();
        bool valid = false;
        const auto rgba = text.mid(1).toULongLong(&valid, 16);
        if (text.startsWith('#') && valid && (text.size() == 7 || (alphaEnabled_ && text.size() == 9))) {
            const auto rgb = text.size() == 9 ? rgba >> 8 : rgba;
            const QColor color(int(rgb >> 16 & 255), int(rgb >> 8 & 255), int(rgb & 255),
                               text.size() == 9 ? int(rgba & 255) : color_.alpha());
            const auto before = color_; setColor(color);
            if (color_ != before && onColorChanged) onColorChanged(color_);
        }
        refreshControls();
    });
    refreshControls();
}
void ColorPicker::setColor(QColor color) {
    if (!color.isValid()) return;
    color = color.toRgb();
    if (!alphaEnabled_) color.setAlpha(255);
    if (color.rgba() == color_.rgba()) return;
    color_ = color;
    // Achromatic colors have no hue; black has no recoverable saturation.
    // Retain those UI coordinates so choosing a hue while black/gray is useful.
    if (color.hsvHueF() >= 0) hue_ = color.hsvHueF();
    if (color.valueF() > 0) saturation_ = color.hsvSaturationF();
    value_ = color.valueF(); refreshControls();
}
void ColorPicker::setAlphaEnabled(bool enabled) {
    alphaEnabled_ = enabled;
    alphaRow_->setVisible(enabled);
    hex_->setToolTip(enabled ? tr("#RRGGBB or #RRGGBBAA") : tr("#RRGGBB"));
    auto color = color_; if (!enabled) color.setAlpha(255); setColor(color); refreshControls();
}
void ColorPicker::setManualInputsVisible(bool visible) { manualInputs_->setVisible(visible); }
void ColorPicker::setHsv(qreal hue, qreal saturation, qreal value) {
    hue_ = std::clamp(hue, 0.0, 1.0); saturation_ = std::clamp(saturation, 0.0, 1.0);
    value_ = std::clamp(value, 0.0, 1.0);
    publish(QColor::fromHsvF(float(hue_), float(saturation_), float(value_), color_.alphaF()).toRgb());
}
void ColorPicker::publish(QColor color) {
    const bool changed = color.rgba() != color_.rgba();
    color_ = color; refreshControls();
    if (changed && onColorChanged) onColorChanged(color_);
}
void ColorPicker::refreshControls() {
    const std::array<int, 3> rgb {color_.red(), color_.green(), color_.blue()};
    const std::array<int, 3> hsv {int(std::lround(hue_ * 360)), int(std::lround(saturation_ * 100)), int(std::lround(value_ * 100))};
    for (size_t i = 0; i < 3; ++i) {
        const QSignalBlocker r(rgb_[i]), h(hsv_[i]);
        rgb_[i]->setValue(rgb[i]); hsv_[i]->setValue(hsv[i]);
    }
    const QSignalBlocker a(alpha_), hex(hex_);
    alpha_->setValue(color_.alpha());
    auto text = color_.name(QColor::HexRgb).toUpper();
    if (alphaEnabled_ && color_.alpha() != 255) text += QStringLiteral("%1").arg(color_.alpha(), 2, 16, QChar('0')).toUpper();
    hex_->setText(text);
    plane_->update(); hueStrip_->update();
}
} // namespace imageeditor::ui
