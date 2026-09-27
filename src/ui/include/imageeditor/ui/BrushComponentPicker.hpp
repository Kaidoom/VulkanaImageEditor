#pragma once

#include "imageeditor/core/BrushAssetRegistry.hpp"
#include "imageeditor/ui/BrushAssetLibrary.hpp"

#include <QWidget>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QListWidget;
class QMenu;
class QToolButton;

namespace imageeditor::ui {

class BrushComponentPicker final : public QWidget {
public:
    explicit BrushComponentPicker(core::BrushAssetType type,
        QWidget* parent = nullptr);

    void setItems(std::vector<BrushComponentItem> items);
    void setCurrentAssetId(std::string_view assetId);
    [[nodiscard]] const std::string& currentAssetId() const noexcept
    {
        return currentAssetId_;
    }
    void openPopup();

    std::function<void(const std::string&)> onAssetSelected;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuildGrid();
    void updateButton();

    core::BrushAssetType type_ {core::BrushAssetType::Tip};
    QToolButton* button_ {nullptr};
    QMenu* menu_ {nullptr};
    QListWidget* grid_ {nullptr};
    std::vector<BrushComponentItem> items_;
    std::string currentAssetId_;
};

} // namespace imageeditor::ui
