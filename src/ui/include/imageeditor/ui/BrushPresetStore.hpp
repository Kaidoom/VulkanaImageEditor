#pragma once

#include "imageeditor/core/BrushEngine.hpp"

#include <QString>
#include <QStringList>

#include <optional>
#include <span>
#include <vector>

namespace imageeditor::ui {

struct BrushPresetSaveResult {
    std::optional<core::BrushPresetRecord> preset;
    QString error;
};

class BrushPresetStore final {
public:
    explicit BrushPresetStore(QString directoryPath = {});

    [[nodiscard]] const QString& directoryPath() const noexcept
    {
        return directoryPath_;
    }
    [[nodiscard]] std::vector<core::BrushPresetRecord> loadOnce(
        std::span<const core::BrushPresetRecord> reservedPresets,
        QStringList* diagnostics = nullptr);
    [[nodiscard]] BrushPresetSaveResult saveCopy(const QString& displayName,
        const core::BrushSettings& settings,
        std::span<const core::BrushPresetRecord> existingPresets);

private:
    QString directoryPath_;
    bool loaded_ {false};
    std::vector<core::BrushPresetRecord> loadedPresets_;
};

} // namespace imageeditor::ui
