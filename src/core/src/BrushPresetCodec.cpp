#include "imageeditor/core/BrushEngine.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace imageeditor::core {
namespace {

constexpr auto kHeader = "ImageEditorBrushPreset";
constexpr int kCurrentVersion = 2;

std::string_view rotationToken(BrushTipRotationMode mode) noexcept
{
    switch (mode) {
    case BrushTipRotationMode::Fixed: return "fixed";
    case BrushTipRotationMode::FollowStrokeDirection: return "follow-stroke";
    }
    return "fixed";
}

std::string_view smoothingToken(BrushSmoothingMode mode) noexcept
{
    switch (mode) {
    case BrushSmoothingMode::None: return "none";
    case BrushSmoothingMode::Weighted: return "weighted";
    }
    return "none";
}

bool finite(double value) noexcept
{
    return std::isfinite(value);
}

bool validRotation(BrushTipRotationMode mode) noexcept
{
    return mode == BrushTipRotationMode::Fixed
        || mode == BrushTipRotationMode::FollowStrokeDirection;
}

bool validSmoothing(BrushSmoothingMode mode) noexcept
{
    return mode == BrushSmoothingMode::None
        || mode == BrushSmoothingMode::Weighted;
}

bool validStableId(std::string_view id) noexcept
{
    return !id.empty() && id.size() <= 192U
        && std::all_of(id.begin(), id.end(), [](unsigned char character) {
               return std::isalnum(character) || character == '.'
                   || character == '_' || character == '-'
                   || character == ':';
           });
}

bool validDisplayName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 320U) {
        return false;
    }
    bool hasVisibleCharacter = false;
    for (const auto raw : name) {
        const auto character = static_cast<unsigned char>(raw);
        if (character < 0x20U || character == 0x7fU) {
            return false;
        }
        hasVisibleCharacter = hasVisibleCharacter
            || std::isspace(character) == 0;
    }
    return hasVisibleCharacter
        && std::isspace(static_cast<unsigned char>(name.front())) == 0
        && std::isspace(static_cast<unsigned char>(name.back())) == 0;
}

bool validPreset(const BrushPresetRecord& preset) noexcept
{
    const auto& settings = preset.settings;
    return validStableId(preset.id) && validDisplayName(preset.displayName)
        && validStableId(settings.tip.assetId)
        && validStableId(settings.grain.assetId)
        && finite(settings.tip.aspectRatio)
        && settings.tip.aspectRatio >= 0.05
        && settings.tip.aspectRatio <= 1.0
        && finite(settings.tip.angleDegrees)
        && validRotation(settings.tip.rotationMode)
        && finite(settings.grain.scalePixels)
        && settings.grain.scalePixels >= 1.0
        && settings.grain.scalePixels <= 4096.0
        && finite(settings.grain.angleDegrees)
        && finite(settings.grain.strength)
        && settings.grain.strength >= 0.0
        && settings.grain.strength <= 1.0
        && finite(settings.sizePixels) && settings.sizePixels >= 0.25
        && settings.sizePixels <= 4096.0
        && finite(settings.hardness) && settings.hardness >= 0.0
        && settings.hardness <= 1.0
        && finite(settings.opacity) && settings.opacity >= 0.0
        && settings.opacity <= 1.0
        && finite(settings.flow) && settings.flow >= 0.0
        && settings.flow <= 1.0
        && finite(settings.spacingPercent) && settings.spacingPercent >= 1.0
        && settings.spacingPercent <= 200.0
        && finite(settings.smoothingTimeMilliseconds)
        && validSmoothing(settings.smoothing)
        && settings.smoothingTimeMilliseconds >= 1.0
        && settings.smoothingTimeMilliseconds <= 250.0;
}

template <typename Value>
bool readField(std::istream& stream, std::string_view expected, Value& value)
{
    std::string key;
    return static_cast<bool>(stream >> key >> value) && key == expected;
}

bool readQuotedField(std::istream& stream, std::string_view expected,
    std::string& value)
{
    std::string key;
    return static_cast<bool>(stream >> key >> std::quoted(value))
        && key == expected;
}

void setError(std::string* error, std::string message)
{
    if (error) {
        *error = std::move(message);
    }
}

} // namespace

std::string serializeBrushPreset(const BrushPresetRecord& preset)
{
    if (!validPreset(preset)) {
        return {};
    }
    const auto& settings = preset.settings;
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<double>::max_digits10);
    stream << kHeader << ' ' << kCurrentVersion << '\n'
           << "id " << std::quoted(preset.id) << '\n'
           << "name " << std::quoted(preset.displayName) << '\n'
           << "tip.asset " << std::quoted(settings.tip.assetId) << '\n'
           << "tip.aspect " << settings.tip.aspectRatio << '\n'
           << "tip.angle " << settings.tip.angleDegrees << '\n'
           << "tip.rotation " << rotationToken(settings.tip.rotationMode) << '\n'
           << "grain.asset " << std::quoted(settings.grain.assetId) << '\n'
           << "grain.scale " << settings.grain.scalePixels << '\n'
           << "grain.angle " << settings.grain.angleDegrees << '\n'
           << "grain.strength " << settings.grain.strength << '\n'
           << "grain.invert " << (settings.grain.invert ? 1 : 0) << '\n'
           << "size " << settings.sizePixels << '\n'
           << "hardness " << settings.hardness << '\n'
           << "opacity " << settings.opacity << '\n'
           << "flow " << settings.flow << '\n'
           << "spacing " << settings.spacingPercent << '\n'
           << "foreground " << static_cast<int>(settings.foreground.red) << ' '
           << static_cast<int>(settings.foreground.green) << ' '
           << static_cast<int>(settings.foreground.blue) << ' '
           << static_cast<int>(settings.foreground.alpha) << '\n'
           << "pressure.size " << (settings.pressureToSize ? 1 : 0) << '\n'
           << "pressure.flow " << (settings.pressureToFlow ? 1 : 0) << '\n'
           << "smoothing " << smoothingToken(settings.smoothing) << '\n'
           << "smoothing.ms " << settings.smoothingTimeMilliseconds << '\n'
           << "seed " << settings.deterministicSeed << '\n';
    return stream.str();
}

std::optional<BrushPresetRecord> deserializeBrushPreset(
    std::string_view serialized, std::string* error)
{
    if (error) {
        error->clear();
    }
    std::istringstream stream {std::string(serialized)};
    stream.imbue(std::locale::classic());
    std::string header;
    int version = 0;
    if (!(stream >> header >> version) || header != kHeader
        || version != kCurrentVersion) {
        setError(error, "Unsupported brush preset header or version");
        return std::nullopt;
    }

    BrushPresetRecord result;
    std::string rotation;
    std::string smoothing;
    int grainInvert = 0;
    int pressureSize = 0;
    int pressureFlow = 0;
    int red = 0;
    int green = 0;
    int blue = 0;
    int alpha = 0;
    std::string foregroundKey;
    if (!readQuotedField(stream, "id", result.id)
        || !readQuotedField(stream, "name", result.displayName)
        || !readQuotedField(stream, "tip.asset", result.settings.tip.assetId)
        || !readField(stream, "tip.aspect", result.settings.tip.aspectRatio)
        || !readField(stream, "tip.angle", result.settings.tip.angleDegrees)) {
        setError(error, "Malformed or incomplete brush preset");
        return std::nullopt;
    }

    // Rotation is optional within schema v2 so records that omit it receive the
    // documented Fixed default. Keep the parser positional after this one
    // extension so malformed and trailing data remain errors.
    std::string nextKey;
    if (!(stream >> nextKey)) {
        setError(error, "Malformed or incomplete brush preset");
        return std::nullopt;
    }
    if (nextKey == "tip.rotation") {
        if (!(stream >> rotation)
            || !readQuotedField(stream, "grain.asset",
                result.settings.grain.assetId)) {
            setError(error, "Malformed or incomplete brush preset");
            return std::nullopt;
        }
    } else if (nextKey == "grain.asset") {
        rotation = "fixed";
        if (!(stream >> std::quoted(result.settings.grain.assetId))) {
            setError(error, "Malformed or incomplete brush preset");
            return std::nullopt;
        }
    } else {
        setError(error, "Malformed or incomplete brush preset");
        return std::nullopt;
    }

    if (!readField(stream, "grain.scale", result.settings.grain.scalePixels)
        || !readField(stream, "grain.angle", result.settings.grain.angleDegrees)
        || !readField(stream, "grain.strength", result.settings.grain.strength)
        || !readField(stream, "grain.invert", grainInvert)
        || !readField(stream, "size", result.settings.sizePixels)
        || !readField(stream, "hardness", result.settings.hardness)
        || !readField(stream, "opacity", result.settings.opacity)
        || !readField(stream, "flow", result.settings.flow)
        || !readField(stream, "spacing", result.settings.spacingPercent)
        || !(stream >> foregroundKey >> red >> green >> blue >> alpha)
        || foregroundKey != "foreground"
        || !readField(stream, "pressure.size", pressureSize)
        || !readField(stream, "pressure.flow", pressureFlow)
        || !readField(stream, "smoothing", smoothing)
        || !readField(stream, "smoothing.ms",
            result.settings.smoothingTimeMilliseconds)
        || !readField(stream, "seed", result.settings.deterministicSeed)) {
        setError(error, "Malformed or incomplete brush preset");
        return std::nullopt;
    }

    if (rotation == "fixed") {
        result.settings.tip.rotationMode = BrushTipRotationMode::Fixed;
    } else if (rotation == "follow-stroke") {
        result.settings.tip.rotationMode =
            BrushTipRotationMode::FollowStrokeDirection;
    } else {
        setError(error, "Unknown tip rotation mode");
        return std::nullopt;
    }
    if (smoothing == "none") {
        result.settings.smoothing = BrushSmoothingMode::None;
    } else if (smoothing == "weighted") {
        result.settings.smoothing = BrushSmoothingMode::Weighted;
    } else {
        setError(error, "Unknown smoothing mode");
        return std::nullopt;
    }
    if ((grainInvert != 0 && grainInvert != 1)
        || (pressureSize != 0 && pressureSize != 1)
        || (pressureFlow != 0 && pressureFlow != 1)
        || red < 0 || red > 255 || green < 0 || green > 255
        || blue < 0 || blue > 255 || alpha < 0 || alpha > 255) {
        setError(error, "Brush preset contains an out-of-range discrete value");
        return std::nullopt;
    }
    result.settings.grain.invert = grainInvert != 0;
    result.settings.pressureToSize = pressureSize != 0;
    result.settings.pressureToFlow = pressureFlow != 0;
    result.settings.foreground = {
        static_cast<std::uint8_t>(red),
        static_cast<std::uint8_t>(green),
        static_cast<std::uint8_t>(blue),
        static_cast<std::uint8_t>(alpha),
    };
    stream >> std::ws;
    if (!stream.eof() || !validPreset(result)) {
        setError(error, "Brush preset contains invalid or trailing data");
        return std::nullopt;
    }
    return result;
}

} // namespace imageeditor::core
