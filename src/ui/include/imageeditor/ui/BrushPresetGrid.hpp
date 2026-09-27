#pragma once

#include <QImage>
#include <QListWidget>
#include <QString>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QResizeEvent;

namespace imageeditor::ui {

struct BrushPresetGridItem {
    std::string id;
    QString displayName;
    QImage thumbnail;
    bool available {true};
    QString unavailableReason;
};

// A permanent, preset-ID keyed brush shelf. The fixed-size cells reflow with
// the Properties panel while this view grows to show every row; the enclosing
// Properties scroll area remains the sole vertical scroller.
class BrushPresetGrid final : public QListWidget {
public:
    explicit BrushPresetGrid(QWidget* parent = nullptr);

    void setItems(std::vector<BrushPresetGridItem> items);
    void setCurrentPresetId(std::string_view presetId);
    void setCurrentPresetModified(bool modified);
    [[nodiscard]] const std::string& currentPresetId() const noexcept
    {
        return currentPresetId_;
    }

    std::function<void(const std::string&)> onPresetSelected;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void rebuild();
    void updateCurrentItem();
    void updateGridHeight();

    std::vector<BrushPresetGridItem> items_;
    std::string currentPresetId_;
    bool currentPresetModified_ {false};
    bool updatingSelection_ {false};
};

} // namespace imageeditor::ui
