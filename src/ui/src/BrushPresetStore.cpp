#include "imageeditor/ui/BrushPresetStore.hpp"
#include "imageeditor/platform/ApplicationPaths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace imageeditor::ui {
namespace {

constexpr qint64 kMaximumPresetBytes = 1024 * 1024;

QString normalizedName(const QString& name)
{
    return name.trimmed().toCaseFolded();
}

bool hasName(std::span<const core::BrushPresetRecord> records,
    const QString& displayName)
{
    const auto normalized = normalizedName(displayName);
    return std::any_of(records.begin(), records.end(),
        [&normalized](const core::BrushPresetRecord& record) {
            return normalizedName(QString::fromStdString(record.displayName))
                == normalized;
        });
}

} // namespace

BrushPresetStore::BrushPresetStore(QString directoryPath)
    : directoryPath_(std::move(directoryPath))
{
    if (directoryPath_.isEmpty()) {
        directoryPath_ = platform::userPresetDirectory();
    }
}

std::vector<core::BrushPresetRecord> BrushPresetStore::loadOnce(
    std::span<const core::BrushPresetRecord> reservedPresets,
    QStringList* diagnostics)
{
    if (loaded_) {
        return loadedPresets_;
    }
    loaded_ = true;
    QDir directory(directoryPath_);
    if (!directory.exists()) {
        return loadedPresets_;
    }

    std::unordered_set<std::string> ids;
    std::vector<QString> names;
    for (const auto& preset : reservedPresets) {
        ids.insert(preset.id);
    }
    // Newly introduced factory names must not hide existing user presets.
    // Identity remains protected; duplicate user files are still rejected.
    // Save Copy continues to require a unique name when creating a new preset.
    const auto files = directory.entryInfoList(
        {QStringLiteral("*.iebrush")}, QDir::Files, QDir::Name);
    for (const auto& info : files) {
        if (info.size() <= 0 || info.size() > kMaximumPresetBytes) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("Skipped oversized or empty preset: %1")
                    .arg(info.fileName()));
            }
            continue;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("Unable to read preset %1: %2")
                    .arg(info.fileName(), file.errorString()));
            }
            continue;
        }
        const auto bytes = file.readAll();
        std::string error;
        const auto decoded = core::deserializeBrushPreset(
            std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())),
            &error);
        if (!decoded || decoded->id.starts_with("builtin.")) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("Skipped invalid preset %1: %2")
                    .arg(info.fileName(), decoded
                            ? QStringLiteral("reserved built-in ID")
                            : QString::fromStdString(error)));
            }
            continue;
        }
        const auto normalized = normalizedName(
            QString::fromStdString(decoded->displayName));
        if (!ids.insert(decoded->id).second
            || std::find(names.begin(), names.end(), normalized) != names.end()) {
            if (diagnostics) {
                diagnostics->push_back(QStringLiteral("Skipped duplicate preset: %1")
                    .arg(info.fileName()));
            }
            continue;
        }
        names.push_back(normalized);
        loadedPresets_.push_back(*decoded);
    }
    return loadedPresets_;
}

BrushPresetSaveResult BrushPresetStore::saveCopy(const QString& displayName,
    const core::BrushSettings& settings,
    std::span<const core::BrushPresetRecord> existingPresets)
{
    // The store owns one startup scan. Calling save before the application
    // explicitly loads presets is still safe and cannot cause the same file
    // to be appended again by a later loadOnce().
    if (!loaded_) {
        (void)loadOnce(existingPresets);
    }
    const auto trimmed = displayName.trimmed();
    const auto containsControl = std::any_of(trimmed.begin(), trimmed.end(),
        [](QChar character) {
            const auto value = character.unicode();
            return value < 0x20U || value == 0x7fU;
        });
    if (trimmed.isEmpty() || trimmed.size() > 80 || containsControl) {
        return {{}, QStringLiteral("Preset name must contain 1 to 80 characters.")};
    }
    if (hasName(existingPresets, trimmed)
        || hasName(loadedPresets_, trimmed)) {
        return {{}, QStringLiteral("A brush preset named ‘%1’ already exists.")
            .arg(trimmed)};
    }
    if (!QDir().mkpath(directoryPath_)) {
        return {{}, QStringLiteral("Unable to create the brush preset directory: %1")
            .arg(directoryPath_)};
    }

    const auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    core::BrushPresetRecord preset {
        .id = QStringLiteral("user.preset.%1").arg(uuid).toStdString(),
        .displayName = trimmed.toStdString(),
        .settings = settings,
    };
    const auto encoded = core::serializeBrushPreset(preset);
    if (encoded.empty()) {
        return {{}, QStringLiteral("The current brush settings cannot be serialized.")};
    }
    const auto path = QDir(directoryPath_).filePath(
        QStringLiteral("preset-%1.iebrush").arg(uuid));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {{}, QStringLiteral("Unable to save preset: %1")
            .arg(file.errorString())};
    }
    const auto written = file.write(encoded.data(),
        static_cast<qint64>(encoded.size()));
    if (written != static_cast<qint64>(encoded.size()) || !file.commit()) {
        return {{}, QStringLiteral("Unable to atomically save preset: %1")
            .arg(file.errorString())};
    }
    loadedPresets_.push_back(preset);
    return {std::move(preset), {}};
}

} // namespace imageeditor::ui
