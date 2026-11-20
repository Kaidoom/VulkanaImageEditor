#pragma once

#include "imageeditor/core/LayerPixelCleanup.hpp"
#include <QWidget>

namespace imageeditor::ui {
class CompactValueControl;
class LayerCleanupPreview final : public QWidget {
public:
    LayerCleanupPreview(const core::Document&, const core::LayerSelectionState&, QWidget* parent);
    [[nodiscard]] int threshold() const;

private:
    CompactValueControl* threshold_;
};
} // namespace imageeditor::ui
