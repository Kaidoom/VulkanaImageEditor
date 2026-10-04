#pragma once

#include <QString>
#include <QStringList>
#include <QWidget>

namespace imageeditor::ui {

// Per-panel values are data; every regular panel still uses the same
// WorkspacePanel/QSplitter implementation.
struct PanelHeightRange {
    int minimum {160};
    int maximum {1200};

    friend bool operator==(
        const PanelHeightRange&, const PanelHeightRange&) = default;
};

struct UiLayoutConfig {
    PanelHeightRange color {120, QWIDGETSIZE_MAX};
    PanelHeightRange layers {260, QWIDGETSIZE_MAX};
    PanelHeightRange properties {200, QWIDGETSIZE_MAX};
    PanelHeightRange adjustments {220, QWIDGETSIZE_MAX};

    [[nodiscard]] static UiLayoutConfig loadFromIni(
        const QString& filePath, QStringList* diagnostics = nullptr);
};

} // namespace imageeditor::ui
