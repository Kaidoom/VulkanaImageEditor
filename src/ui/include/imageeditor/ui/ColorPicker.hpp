#pragma once
#include <QColor>
#include <QWidget>
#include <array>
#include <functional>

class QLineEdit;
class QSpinBox;
namespace imageeditor::ui {
class ColorField;

// Shared sRGB/HSV editor. Alpha is independent coverage; HSV is UI-only and
// never changes the document's linear-light compositing contract.
class ColorPicker final : public QWidget {
public:
    explicit ColorPicker(QWidget* parent = nullptr);
    void setColor(QColor color);
    [[nodiscard]] QColor color() const { return color_; }
    void setAlphaEnabled(bool enabled);
    [[nodiscard]] bool alphaEnabled() const { return alphaEnabled_; }
    void setManualInputsVisible(bool visible);
    std::function<void(QColor)> onColorChanged;
private:
    friend class ColorField;
    void setHsv(qreal hue, qreal saturation, qreal value);
    void publish(QColor color);
    void refreshControls();
    QColor color_ {Qt::white};
    qreal hue_ {}, saturation_ {}, value_ {1};
    bool alphaEnabled_ {true};
    ColorField* plane_ {};
    ColorField* hueStrip_ {};
    std::array<QSpinBox*, 3> rgb_ {}, hsv_ {};
    QSpinBox* alpha_ {};
    QWidget* alphaRow_ {};
    QWidget* manualInputs_ {};
    QLineEdit* hex_ {};
};
} // namespace imageeditor::ui
