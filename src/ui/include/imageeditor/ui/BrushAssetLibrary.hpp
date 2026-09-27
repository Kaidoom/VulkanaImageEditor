#pragma once

#include "imageeditor/core/BrushAssetRegistry.hpp"
#include "imageeditor/core/BrushTip.hpp"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace imageeditor::ui {

struct BrushComponentItem {
    std::string id;
    QString displayName;
    core::BrushAssetType type {core::BrushAssetType::Tip};
    QImage thumbnail;
    bool available {true};
    bool packaged {false};
    QString unavailableReason;
    std::optional<double> defaultScalePixels;
    std::optional<double> defaultRotationDegrees;
};

struct BrushAssetRuntimeStats {
    std::uint64_t resourceReads {0};
    std::uint64_t imageDecodes {0};
    std::uint64_t mipBuilds {0};
    std::uint64_t resolverHits {0};
    std::uint64_t resolverMisses {0};
    std::size_t residentMaskCount {0};
    std::size_t residentBytes {0};

    friend bool operator==(
        const BrushAssetRuntimeStats&, const BrushAssetRuntimeStats&) = default;
};

// Qt is confined to this application-adapter layer. The resolver side exposes
// only already-prepared immutable masks; createTip/createGrain never read a
// file, decode an image, or build a mip pyramid.
class BrushAssetLibrary final : public core::IBrushAssetResolver {
public:
    [[nodiscard]] static std::shared_ptr<BrushAssetLibrary> createPackaged();
    [[nodiscard]] static std::shared_ptr<BrushAssetLibrary> createBuiltinOnly();

    [[nodiscard]] const core::BrushAssetRegistry& registry() const noexcept
    {
        return *registry_;
    }
    [[nodiscard]] std::vector<BrushComponentItem> components(
        core::BrushAssetType type) const;
    [[nodiscard]] const BrushComponentItem* component(
        std::string_view id) const noexcept;
    [[nodiscard]] bool supports(
        std::string_view id, core::BrushAssetType type) const noexcept;
    [[nodiscard]] bool isPrepared(
        std::string_view id, core::BrushAssetType type) const noexcept;
    bool prepareAsset(std::string_view id, core::BrushAssetType type,
        QString* error = nullptr);
    bool prepareSettings(
        const core::BrushSettings& settings, QString* error = nullptr);
    [[nodiscard]] QString unavailableMessage(
        const core::BrushSettings& settings) const;
    [[nodiscard]] BrushAssetRuntimeStats runtimeStats() const noexcept;
    [[nodiscard]] const QStringList& diagnostics() const noexcept
    {
        return diagnostics_;
    }

    [[nodiscard]] std::unique_ptr<core::IBrushTip> createTip(
        const core::BrushTipDescriptor& descriptor) const override;
    [[nodiscard]] std::unique_ptr<core::IBrushGrain> createGrain(
        const core::BrushGrainDescriptor& descriptor) const override;
    [[nodiscard]] core::BrushAssetCacheStats cacheStats() const noexcept override;

private:
    struct PackagedSource {
        core::BrushAssetRecord record;
        QByteArray encodedBytes;
        // Canonical 0-empty/255-full R8 decoded once during registry load.
        // Selection promotes this immutable plane into the mipmapped core
        // cache; it never asks an image codec to decode the PNG again.
        QImage canonicalCoverage;
    };

    BrushAssetLibrary();
    void loadPackagedRegistry();
    void addBuiltinComponents();
    [[nodiscard]] QImage decodeCoverage(
        const PackagedSource& source, QString* error) const;
    [[nodiscard]] QImage makeAssetThumbnail(
        const QImage& coverage, core::BrushAssetType type) const;
    [[nodiscard]] QImage makeBuiltinTipThumbnail(
        const core::BrushTipDescriptor& descriptor) const;
    [[nodiscard]] QImage makeBuiltinGrainThumbnail(
        const core::BrushGrainDescriptor& descriptor) const;
    void setError(QString* output, const QString& message) const;

    std::shared_ptr<const core::BrushAssetRegistry> registry_;
    std::vector<BrushComponentItem> components_;
    std::map<std::string, PackagedSource, std::less<>> packagedSources_;
    std::map<std::string, std::shared_ptr<const core::GrayscaleMaskAsset>,
        std::less<>> residentMasks_;
    QStringList diagnostics_;
    mutable std::mutex mutex_;
    mutable BrushAssetRuntimeStats stats_;
};

} // namespace imageeditor::ui
