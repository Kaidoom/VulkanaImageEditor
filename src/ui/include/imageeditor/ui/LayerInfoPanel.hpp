#pragma once
#include "imageeditor/core/BoundsSnapping.hpp"
#include "imageeditor/core/Document.hpp"
#include "imageeditor/core/RichText.hpp"
#include <QWidget>
#include <functional>
class QLabel;
class QPushButton;
namespace imageeditor::ui {
class FontFamilyPicker;
class ToolOptionsNumber;
// One shared inspector follows the selected layer independently of the tool.
// Only explicit control acceptance publishes edits; refresh never changes data.
class LayerInfoPanel final : public QWidget {
public:
    explicit LayerInfoPanel(QWidget* parent = nullptr);
    void setLayer(const core::Layer*, const core::LayerContainer* = nullptr,
        std::optional<core::DocumentBounds> groupBounds = { });
    std::function<void(core::LayerId, core::TextStylePatch)> onTextFormat;
    std::function<void(core::LayerId)> onTextColor;

private:
    QLabel *size_, *angle_, *position_, *name_;
    QWidget *transform_, *text_;
    FontFamilyPicker* family_;
    ToolOptionsNumber* fontSize_;
    QPushButton* color_;
    core::LayerId target_ { };
    bool updating_ { };
    bool numericDirty_ { };
    double displayedFontSize_ {24};
    bool displayedSizeMixed_ {};
};
}
