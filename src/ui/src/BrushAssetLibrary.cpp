#include "imageeditor/ui/BrushAssetLibrary.hpp"
#include "imageeditor/core/CreativeBrushes.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <span>
#include <utility>

void initializeBrushAssetsResource()
{
    Q_INIT_RESOURCE(brush_assets);
}

namespace {

constexpr auto kRegistryResource = ":/imageeditor/brush/v1/registry.json";
constexpr auto kResourceRoot = ":/imageeditor/brush/v1/";
constexpr int kThumbnailExtent = 64;
constexpr std::uint64_t kMaximumDecodedPixels = 16ULL * 1024ULL * 1024ULL;

QString typeName(imageeditor::core::BrushAssetType type)
{
    return type == imageeditor::core::BrushAssetType::Tip
        ? QStringLiteral("tip") : QStringLiteral("grain");
}

std::optional<imageeditor::core::BrushAssetType> parseType(
    const QString& value)
{
    if (value == QStringLiteral("tip")) {
        return imageeditor::core::BrushAssetType::Tip;
    }
    if (value == QStringLiteral("grain")) {
        return imageeditor::core::BrushAssetType::Grain;
    }
    return std::nullopt;
}

std::optional<imageeditor::core::BrushCoverageChannel> parseCoverage(
    const QString& value)
{
    if (value == QStringLiteral("luminance")) {
        return imageeditor::core::BrushCoverageChannel::Luminance;
    }
    if (value == QStringLiteral("alpha")) {
        return imageeditor::core::BrushCoverageChannel::Alpha;
    }
    if (value == QStringLiteral("luminance-times-alpha")) {
        return imageeditor::core::BrushCoverageChannel::LuminanceTimesAlpha;
    }
    return std::nullopt;
}

QImage legibleGrainTile(QImage tile)
{
    tile = tile.convertToFormat(QImage::Format_Grayscale8);
    if (tile.isNull()) {
        return tile;
    }
    double sum = 0.0;
    double sumSquares = 0.0;
    const auto sampleCount = static_cast<double>(
        tile.width() * tile.height());
    for (int y = 0; y < tile.height(); ++y) {
        const auto* row = tile.constScanLine(y);
        for (int x = 0; x < tile.width(); ++x) {
            const auto value = static_cast<double>(row[x]);
            sum += value;
            sumSquares += value * value;
        }
    }
    const auto mean = sum / sampleCount;
    const auto variance = std::max(0.0,
        sumSquares / sampleCount - mean * mean);
    const auto deviation = std::sqrt(variance);
    // Minification can collapse fine masks into an almost-flat swatch. This
    // preview-only gain preserves polarity and structure while making the
    // selector legible; brush sampling still uses the untouched coverage.
    const auto gain = deviation > 0.0
        ? std::clamp(28.0 / deviation, 1.0, 5.0) : 1.0;
    if (gain <= 1.0) {
        return tile;
    }
    for (int y = 0; y < tile.height(); ++y) {
        auto* row = tile.scanLine(y);
        for (int x = 0; x < tile.width(); ++x) {
            row[x] = static_cast<uchar>(std::clamp(std::lround(
                mean + (static_cast<double>(row[x]) - mean) * gain),
                0L, 255L));
        }
    }
    return tile;
}

QImage coveragePreview(const QImage& source, QSize targetSize, bool tiled)
{
    QImage result(targetSize, QImage::Format_RGBA8888);
    result.fill(QColor(QStringLiteral("#171A22")));
    if (source.isNull()) {
        return result;
    }

    QImage scaled;
    if (tiled) {
        const auto tileSize = QSize(
            std::max(1, targetSize.width() / 2),
            std::max(1, targetSize.height() / 2));
        scaled = source.scaled(tileSize, Qt::IgnoreAspectRatio,
            Qt::SmoothTransformation);
        scaled = legibleGrainTile(std::move(scaled));
    } else {
        scaled = source.scaled(targetSize - QSize(10, 10),
            Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (tiled) {
        for (int y = 0; y < targetSize.height(); y += scaled.height()) {
            for (int x = 0; x < targetSize.width(); x += scaled.width()) {
                painter.drawImage(x, y, scaled);
            }
        }
    } else {
        const QPoint offset {
            (targetSize.width() - scaled.width()) / 2,
            (targetSize.height() - scaled.height()) / 2};
        painter.drawImage(offset, scaled);
    }
    painter.setPen(QColor(QStringLiteral("#4A5369")));
    painter.drawRect(result.rect().adjusted(0, 0, -1, -1));
    return result;
}

QImage noGrainThumbnail()
{
    QImage result(kThumbnailExtent, kThumbnailExtent, QImage::Format_RGBA8888);
    result.fill(QColor(QStringLiteral("#20242F")));
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(QStringLiteral("#8A91A2")), 3));
    painter.drawLine(16, 16, 48, 48);
    painter.drawLine(48, 16, 16, 48);
    return result;
}

} // namespace

namespace imageeditor::ui {

BrushAssetLibrary::BrushAssetLibrary()
    : registry_(std::make_shared<core::BrushAssetRegistry>(
          core::BrushAssetRegistry::SupportedVersion,
          std::vector<core::BrushAssetRecord> {}))
{
    addBuiltinComponents();
}

std::shared_ptr<BrushAssetLibrary> BrushAssetLibrary::createBuiltinOnly()
{
    return std::shared_ptr<BrushAssetLibrary>(new BrushAssetLibrary);
}

std::shared_ptr<BrushAssetLibrary> BrushAssetLibrary::createPackaged()
{
    static const bool initialized = [] {
        initializeBrushAssetsResource();
        return true;
    }();
    (void)initialized;
    auto result = std::shared_ptr<BrushAssetLibrary>(new BrushAssetLibrary);
    result->loadPackagedRegistry();
    return result;
}

void BrushAssetLibrary::addBuiltinComponents()
{
    const auto addTip = [this](std::string_view id, const QString& name,
                            double rotation = 0.0) {
        core::BrushTipDescriptor descriptor;
        descriptor.assetId = id;
        components_.push_back({std::string(id), name,
            core::BrushAssetType::Tip, makeBuiltinTipThumbnail(descriptor),
            true, false, {}, {}, rotation});
    };
    addTip(core::BrushAssetIds::ProceduralRoundTip,
        QStringLiteral("Round"));
    addTip(core::BrushAssetIds::ProceduralEllipseTip,
        QStringLiteral("Ellipse / Chisel"));
    addTip(core::BrushAssetIds::DryInkMaskTip,
        QStringLiteral("Generated Dry Ink"));
    for (const auto& preset : core::creativeBrushPresets())
        addTip(preset.settings.tip.assetId, QString::fromStdString(preset.displayName), preset.settings.tip.angleDegrees);

    core::BrushGrainDescriptor none;
    none.assetId = core::BrushAssetIds::NoGrain;
    components_.push_back({std::string(core::BrushAssetIds::NoGrain),
        QStringLiteral("None"), core::BrushAssetType::Grain,
        noGrainThumbnail(), true, false, {}, 96.0, 0.0});
    core::BrushGrainDescriptor paper;
    paper.assetId = core::BrushAssetIds::DryInkPaperGrain;
    paper.strength = 1.0;
    components_.push_back({std::string(core::BrushAssetIds::DryInkPaperGrain),
        QStringLiteral("Generated Paper"), core::BrushAssetType::Grain,
        makeBuiltinGrainThumbnail(paper), true, false, {}, 72.0, 7.0});
}

void BrushAssetLibrary::loadPackagedRegistry()
{
    QFile manifest(QString::fromLatin1(kRegistryResource));
    if (!manifest.open(QIODevice::ReadOnly)) {
        diagnostics_.push_back(QStringLiteral("Packaged brush registry is unavailable"));
        return;
    }
    const auto document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isObject()) {
        diagnostics_.push_back(QStringLiteral("Packaged brush registry is malformed"));
        return;
    }
    const auto root = document.object();
    const auto schema = root.value(QStringLiteral("schema"));
    const auto version = root.value(QStringLiteral("version"));
    const auto convention = root.value(QStringLiteral("coverageConvention"));
    const auto assetValue = root.value(QStringLiteral("assets"));
    if (!schema.isString()
        || schema.toString() != QStringLiteral("imageeditor.brush-assets")
        || !version.isDouble()
        || version.toInteger(-1)
            != static_cast<qint64>(core::BrushAssetRegistry::SupportedVersion)
        || !convention.isString()
        || convention.toString()
            != QStringLiteral("zero-is-empty-255-is-full")) {
        diagnostics_.push_back(
            QStringLiteral("Unsupported packaged brush registry schema or convention"));
        return;
    }
    if (!assetValue.isArray() || assetValue.toArray().isEmpty()) {
        diagnostics_.push_back(
            QStringLiteral("Packaged brush registry has no asset array"));
        return;
    }

    std::vector<core::BrushAssetRecord> records;
    const auto values = assetValue.toArray();
    records.reserve(static_cast<std::size_t>(values.size()));
    for (qsizetype index = 0; index < values.size(); ++index) {
        if (!values[index].isObject()) {
            diagnostics_.push_back(QStringLiteral("Registry entry %1 is not an object")
                .arg(index));
            return;
        }
        const auto object = values[index].toObject();
        const auto id = object.value(QStringLiteral("id"));
        const auto name = object.value(QStringLiteral("name"));
        const auto typeValue = object.value(QStringLiteral("type"));
        const auto path = object.value(QStringLiteral("path"));
        const auto coverageValue = object.value(QStringLiteral("coverage"));
        const auto invert = object.value(QStringLiteral("invert"));
        const auto revision = object.value(QStringLiteral("revision"));
        const auto sha256 = object.value(QStringLiteral("sha256"));
        if (!id.isString() || !name.isString() || !typeValue.isString()
            || !path.isString() || !coverageValue.isString()
            || !invert.isBool() || !revision.isDouble()
            || !sha256.isString()) {
            diagnostics_.push_back(QStringLiteral("Registry entry %1 has missing or mistyped required fields")
                .arg(index));
            return;
        }
        const auto revisionNumber = revision.toDouble();
        if (!std::isfinite(revisionNumber)
            || revisionNumber < 1.0
            || std::floor(revisionNumber) != revisionNumber
            // JSON numbers are IEEE-754 doubles; reject values beyond the
            // exact-integer range rather than accepting a rounded revision.
            || revisionNumber > 9007199254740991.0) {
            diagnostics_.push_back(QStringLiteral("Registry entry %1 has an invalid revision")
                .arg(index));
            return;
        }
        if ((object.contains(QStringLiteral("seamless"))
                && !object.value(QStringLiteral("seamless")).isBool())
            || (object.contains(QStringLiteral("defaultScale"))
                && !object.value(QStringLiteral("defaultScale")).isDouble())
            || (object.contains(QStringLiteral("defaultRotation"))
                && !object.value(QStringLiteral("defaultRotation")).isDouble())) {
            diagnostics_.push_back(QStringLiteral("Registry entry %1 has mistyped optional fields")
                .arg(index));
            return;
        }
        const auto type = parseType(typeValue.toString());
        const auto coverage = parseCoverage(coverageValue.toString());
        if (!type || !coverage) {
            diagnostics_.push_back(QStringLiteral("Registry entry %1 has an unknown type or coverage channel")
                .arg(index));
            return;
        }
        core::BrushAssetRecord record;
        record.id = id.toString().toStdString();
        record.displayName = name.toString().toStdString();
        record.type = *type;
        record.relativePackagedPath = path.toString().toStdString();
        record.coverageChannel = *coverage;
        record.invert = invert.toBool();
        if (object.contains(QStringLiteral("seamless"))) {
            record.seamless = object.value(QStringLiteral("seamless")).toBool();
        }
        if (object.contains(QStringLiteral("defaultScale"))) {
            record.defaultScalePixels
                = object.value(QStringLiteral("defaultScale")).toDouble();
        }
        if (object.contains(QStringLiteral("defaultRotation"))) {
            record.defaultRotationDegrees
                = object.value(QStringLiteral("defaultRotation")).toDouble();
        }
        record.revision = static_cast<std::uint64_t>(revisionNumber);
        record.sha256 = sha256.toString().toStdString();
        records.push_back(std::move(record));
    }

    try {
        std::vector<std::string_view> reservedIds;
        reservedIds.reserve(components_.size());
        for (const auto& component : components_) {
            reservedIds.emplace_back(component.id);
        }
        registry_ = std::make_shared<core::BrushAssetRegistry>(
            core::BrushAssetRegistry::SupportedVersion, std::move(records),
            reservedIds);
    } catch (const std::exception& exception) {
        diagnostics_.push_back(QStringLiteral("Brush registry validation failed: %1")
            .arg(QString::fromUtf8(exception.what())));
        registry_ = std::make_shared<core::BrushAssetRegistry>(
            core::BrushAssetRegistry::SupportedVersion,
            std::vector<core::BrushAssetRecord> {});
        return;
    }

    for (const auto& record : registry_->assets()) {
        const auto resourcePath = QString::fromLatin1(kResourceRoot)
            + QString::fromStdString(record.relativePackagedPath);
        QFile file(resourcePath);
        PackagedSource source {
            .record = record, .encodedBytes = {}, .canonicalCoverage = {}};
        QString unavailable;
        if (!file.open(QIODevice::ReadOnly)) {
            unavailable = QStringLiteral("Packaged resource is missing: %1")
                              .arg(resourcePath);
        } else {
            source.encodedBytes = file.readAll();
            ++stats_.resourceReads;
            const auto digest = QCryptographicHash::hash(
                source.encodedBytes, QCryptographicHash::Sha256).toHex();
            if (digest != QByteArray::fromStdString(record.sha256)) {
                unavailable = QStringLiteral("Digest mismatch for %1")
                                  .arg(QString::fromStdString(record.id));
                source.encodedBytes.clear();
            }
        }

        QImage thumbnail;
        if (unavailable.isEmpty()) {
            QString decodeError;
            const auto coverage = decodeCoverage(source, &decodeError);
            if (coverage.isNull()) {
                unavailable = decodeError;
            } else {
                thumbnail = makeAssetThumbnail(coverage, record.type);
                source.canonicalCoverage = coverage;
                // The verified compressed resource remains embedded in the
                // application, so retaining a second QByteArray copy serves
                // no purpose after canonical decoding.
                source.encodedBytes.clear();
            }
        }
        if (!unavailable.isEmpty()) {
            diagnostics_.push_back(unavailable);
        }
        components_.push_back({record.id,
            QString::fromStdString(record.displayName), record.type,
            std::move(thumbnail), unavailable.isEmpty(), true, unavailable,
            record.defaultScalePixels, record.defaultRotationDegrees});
        packagedSources_.emplace(record.id, std::move(source));
    }
}

QImage BrushAssetLibrary::decodeCoverage(
    const PackagedSource& source, QString* error) const
{
    if (source.encodedBytes.isEmpty()) {
        setError(error, QStringLiteral("Brush asset data is unavailable: %1")
                            .arg(QString::fromStdString(source.record.id)));
        return {};
    }
    QBuffer buffer;
    buffer.setData(source.encodedBytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    reader.setAutoTransform(false);
    const auto dimensions = reader.size();
    if (!dimensions.isValid()
        || dimensions.width() <= 0 || dimensions.height() <= 0
        || static_cast<std::uint64_t>(dimensions.width())
                * static_cast<std::uint64_t>(dimensions.height())
            > kMaximumDecodedPixels) {
        setError(error, QStringLiteral("Brush asset dimensions are invalid or too large: %1")
                            .arg(QString::fromStdString(source.record.id)));
        return {};
    }
    auto rgba = reader.read().convertToFormat(QImage::Format_RGBA8888);
    ++stats_.imageDecodes;
    if (rgba.isNull()) {
        setError(error, QStringLiteral("Unable to decode brush asset %1: %2")
                            .arg(QString::fromStdString(source.record.id),
                                reader.errorString()));
        return {};
    }
    QImage result(rgba.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* input = rgba.constScanLine(y);
        auto* output = result.scanLine(y);
        for (int x = 0; x < rgba.width(); ++x) {
            const auto offset = x * 4;
            const auto red = static_cast<unsigned>(input[offset]);
            const auto green = static_cast<unsigned>(input[offset + 1]);
            const auto blue = static_cast<unsigned>(input[offset + 2]);
            const auto alpha = static_cast<unsigned>(input[offset + 3]);
            const auto luminance = (54U * red + 183U * green + 19U * blue + 128U)
                >> 8U;
            unsigned coverage = 0;
            switch (source.record.coverageChannel) {
            case core::BrushCoverageChannel::Luminance:
                coverage = luminance;
                break;
            case core::BrushCoverageChannel::Alpha:
                coverage = alpha;
                break;
            case core::BrushCoverageChannel::LuminanceTimesAlpha:
                coverage = (luminance * alpha + 127U) / 255U;
                break;
            }
            if (source.record.invert) {
                coverage = 255U - coverage;
            }
            output[x] = static_cast<uchar>(coverage);
        }
    }
    return result;
}

QImage BrushAssetLibrary::makeAssetThumbnail(
    const QImage& coverage, core::BrushAssetType type) const
{
    return coveragePreview(coverage,
        QSize(kThumbnailExtent, kThumbnailExtent),
        type == core::BrushAssetType::Grain);
}

QImage BrushAssetLibrary::makeBuiltinTipThumbnail(
    const core::BrushTipDescriptor& descriptor) const
{
    auto tip = core::builtinBrushAssetResolver().createTip(descriptor);
    QImage coverage(kThumbnailExtent, kThumbnailExtent,
        QImage::Format_Grayscale8);
    coverage.fill(0);
    if (!tip) {
        return coveragePreview(coverage, coverage.size(), false);
    }
    core::BrushDab dab;
    dab.documentCenter = {kThumbnailExtent * 0.5, kThumbnailExtent * 0.5};
    dab.diameterPixels = 50.0;
    dab.tipAspectRatio = descriptor.assetId
            == core::BrushAssetIds::ProceduralEllipseTip
        ? 0.35 : 0.72;
    dab.tipAngleDegrees = descriptor.assetId
            == core::BrushAssetIds::ProceduralEllipseTip
        ? -35.0 : -20.0;
    for (const auto& preset : core::creativeBrushPresets()) if (preset.settings.tip.assetId == descriptor.assetId) {
        dab.tipAspectRatio = preset.settings.tip.aspectRatio;
        // Show the canonical tip, not an arbitrarily tilted/squashed heart or
        // star. Directional paint nibs retain their actual initial angle.
        dab.tipAngleDegrees = preset.settings.tip.angleDegrees;
        dab.deterministicSeed = preset.settings.deterministicSeed;
        if (preset.displayName == "Precision Ink") dab.diameterPixels = 9;
        break;
    }
    tip->prepareDab(dab, 0.75, 1.0);
    for (int y = 0; y < coverage.height(); ++y) {
        auto* output = coverage.scanLine(y);
        for (int x = 0; x < coverage.width(); ++x) {
            output[x] = static_cast<uchar>(std::clamp(std::lround(
                tip->coverage({x + 0.5, y + 0.5}) * 255.0), 0L, 255L));
        }
    }
    return coveragePreview(coverage, coverage.size(), false);
}

QImage BrushAssetLibrary::makeBuiltinGrainThumbnail(
    const core::BrushGrainDescriptor& descriptor) const
{
    auto grain = core::builtinBrushAssetResolver().createGrain(descriptor);
    QImage coverage(kThumbnailExtent, kThumbnailExtent,
        QImage::Format_Grayscale8);
    coverage.fill(255);
    if (grain) {
        core::BrushDab dab;
        dab.grainScalePixels = 32.0;
        dab.grainStrength = 1.0;
        dab.deterministicSeed = 1;
        grain->prepareDab(dab, 1.0);
        for (int y = 0; y < coverage.height(); ++y) {
            auto* output = coverage.scanLine(y);
            for (int x = 0; x < coverage.width(); ++x) {
                output[x] = static_cast<uchar>(std::clamp(std::lround(
                    grain->modulation({x + 0.5, y + 0.5}) * 255.0),
                    0L, 255L));
            }
        }
    }
    return coveragePreview(coverage, coverage.size(), true);
}

std::vector<BrushComponentItem> BrushAssetLibrary::components(
    core::BrushAssetType type) const
{
    std::vector<BrushComponentItem> result;
    std::copy_if(components_.begin(), components_.end(),
        std::back_inserter(result), [type](const BrushComponentItem& item) {
            return item.type == type;
        });
    return result;
}

const BrushComponentItem* BrushAssetLibrary::component(
    std::string_view id) const noexcept
{
    const auto found = std::find_if(components_.begin(), components_.end(),
        [id](const BrushComponentItem& item) { return item.id == id; });
    return found == components_.end() ? nullptr : &*found;
}

bool BrushAssetLibrary::supports(
    std::string_view id, core::BrushAssetType type) const noexcept
{
    const auto* item = component(id);
    return item && item->type == type && item->available;
}

bool BrushAssetLibrary::isPrepared(
    std::string_view id, core::BrushAssetType type) const noexcept
{
    if (!supports(id, type)) {
        return false;
    }
    if (!component(id)->packaged) {
        return true;
    }
    const std::lock_guard lock(mutex_);
    return residentMasks_.contains(id);
}

bool BrushAssetLibrary::prepareAsset(std::string_view id,
    core::BrushAssetType type, QString* error)
{
    if (error) {
        error->clear();
    }
    const auto* item = component(id);
    if (!item || item->type != type) {
        setError(error, QStringLiteral("Unknown %1 asset: %2")
                            .arg(typeName(type), QString::fromUtf8(id)));
        return false;
    }
    if (!item->available) {
        setError(error, item->unavailableReason.isEmpty()
                ? QStringLiteral("Brush asset is unavailable: %1")
                      .arg(QString::fromUtf8(id))
                : item->unavailableReason);
        return false;
    }
    if (!item->packaged) {
        return true;
    }

    const std::lock_guard lock(mutex_);
    if (residentMasks_.contains(id)) {
        return true;
    }
    const auto source = packagedSources_.find(id);
    if (source == packagedSources_.end()) {
        setError(error, QStringLiteral("Brush asset data is unavailable: %1")
                            .arg(QString::fromUtf8(id)));
        return false;
    }
    const auto coverage = source->second.canonicalCoverage;
    if (coverage.isNull()) {
        setError(error, QStringLiteral("Canonical brush coverage is unavailable: %1")
                            .arg(QString::fromUtf8(id)));
        return false;
    }
    std::vector<std::uint8_t> pixels;
    pixels.resize(static_cast<std::size_t>(coverage.width())
        * static_cast<std::size_t>(coverage.height()));
    for (int y = 0; y < coverage.height(); ++y) {
        const auto* row = coverage.constScanLine(y);
        std::copy_n(row, coverage.width(), pixels.begin()
            + static_cast<std::ptrdiff_t>(y) * coverage.width());
    }
    try {
        auto mask = std::make_shared<core::GrayscaleMaskAsset>(
            source->second.record.id, source->second.record.revision,
            static_cast<std::uint32_t>(coverage.width()),
            static_cast<std::uint32_t>(coverage.height()), pixels);
        residentMasks_.emplace(source->second.record.id, std::move(mask));
        source->second.canonicalCoverage = {};
        ++stats_.mipBuilds;
    } catch (const std::exception& exception) {
        setError(error, QStringLiteral("Unable to prepare brush asset %1: %2")
                            .arg(QString::fromUtf8(id),
                                QString::fromUtf8(exception.what())));
        return false;
    }
    return true;
}

bool BrushAssetLibrary::prepareSettings(
    const core::BrushSettings& settings, QString* error)
{
    QString detail;
    if (!prepareAsset(settings.tip.assetId, core::BrushAssetType::Tip, &detail)) {
        setError(error, detail);
        return false;
    }
    if (!prepareAsset(settings.grain.assetId,
            core::BrushAssetType::Grain, &detail)) {
        setError(error, detail);
        return false;
    }
    if (error) {
        error->clear();
    }
    return true;
}

QString BrushAssetLibrary::unavailableMessage(
    const core::BrushSettings& settings) const
{
    if (!supports(settings.tip.assetId, core::BrushAssetType::Tip)) {
        return QStringLiteral("Brush tip is unavailable: %1")
            .arg(QString::fromStdString(settings.tip.assetId));
    }
    if (!isPrepared(settings.tip.assetId, core::BrushAssetType::Tip)) {
        return QStringLiteral("Brush tip is not prepared: %1")
            .arg(QString::fromStdString(settings.tip.assetId));
    }
    if (!supports(settings.grain.assetId, core::BrushAssetType::Grain)) {
        return QStringLiteral("Brush grain is unavailable: %1")
            .arg(QString::fromStdString(settings.grain.assetId));
    }
    if (!isPrepared(settings.grain.assetId, core::BrushAssetType::Grain)) {
        return QStringLiteral("Brush grain is not prepared: %1")
            .arg(QString::fromStdString(settings.grain.assetId));
    }
    return {};
}

std::unique_ptr<core::IBrushTip> BrushAssetLibrary::createTip(
    const core::BrushTipDescriptor& descriptor) const
{
    if (auto builtin = core::builtinBrushAssetResolver().createTip(descriptor)) {
        const std::lock_guard lock(mutex_);
        ++stats_.resolverHits;
        return builtin;
    }
    const std::lock_guard lock(mutex_);
    const auto found = residentMasks_.find(descriptor.assetId);
    if (found == residentMasks_.end()
        || !registry_->contains(descriptor.assetId, core::BrushAssetType::Tip)) {
        ++stats_.resolverMisses;
        return {};
    }
    ++stats_.resolverHits;
    return std::make_unique<core::BitmapMaskTip>(found->second);
}

std::unique_ptr<core::IBrushGrain> BrushAssetLibrary::createGrain(
    const core::BrushGrainDescriptor& descriptor) const
{
    if (auto builtin = core::builtinBrushAssetResolver().createGrain(descriptor)) {
        const std::lock_guard lock(mutex_);
        ++stats_.resolverHits;
        return builtin;
    }
    const std::lock_guard lock(mutex_);
    const auto found = residentMasks_.find(descriptor.assetId);
    if (found == residentMasks_.end()
        || !registry_->contains(descriptor.assetId, core::BrushAssetType::Grain)) {
        ++stats_.resolverMisses;
        return {};
    }
    ++stats_.resolverHits;
    return std::make_unique<core::DocumentAnchoredMaskGrain>(found->second);
}

core::BrushAssetCacheStats BrushAssetLibrary::cacheStats() const noexcept
{
    const auto builtin = core::builtinBrushAssetResolver().cacheStats();
    const std::lock_guard lock(mutex_);
    auto result = builtin;
    result.grayscaleMaskCount += residentMasks_.size();
    for (const auto& [id, mask] : residentMasks_) {
        (void)id;
        result.retainedBytes += mask->retainedBytes();
    }
    return result;
}

BrushAssetRuntimeStats BrushAssetLibrary::runtimeStats() const noexcept
{
    const std::lock_guard lock(mutex_);
    auto result = stats_;
    result.residentMaskCount = residentMasks_.size();
    result.residentBytes = 0;
    for (const auto& [id, mask] : residentMasks_) {
        (void)id;
        result.residentBytes += mask->retainedBytes();
    }
    return result;
}

void BrushAssetLibrary::setError(
    QString* output, const QString& message) const
{
    if (output) {
        *output = message;
    }
}

} // namespace imageeditor::ui
