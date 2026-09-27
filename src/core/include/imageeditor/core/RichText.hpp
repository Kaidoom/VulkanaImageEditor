#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Layer.hpp"
#include <optional>

namespace imageeditor::core {
inline constexpr std::size_t kMaximumTextBytes = 256 * 1024;
inline constexpr std::size_t kMaximumTextRuns = 16384;

// Maps Unicode scalar boundaries explicitly. A UTF-16 surrogate interior maps
// to the preceding scalar boundary; navigation separately uses graphemes.
class Utf8TextIndex final {
public:
    explicit Utf8TextIndex(std::string_view utf8);
    [[nodiscard]] std::size_t byteOffset(std::size_t utf16) const noexcept;
    [[nodiscard]] std::size_t utf16Offset(std::size_t byte) const noexcept;
    [[nodiscard]] bool isBoundary(std::size_t byte) const noexcept;
    [[nodiscard]] std::size_t utf16Length() const noexcept { return bytes_.size() - 1; }

private:
    std::vector<std::size_t> bytes_;
};
struct TextStylePatch {
    std::optional<std::string> family, style;
    std::optional<int> weight;
    std::optional<bool> italic;
    std::optional<double> sizePixels;
    std::optional<Rgba8> color;
};
[[nodiscard]] TextStyle patchedTextStyle(TextStyle, const TextStylePatch&);
[[nodiscard]] TextStyle textStyleAt(const TextLayer&, std::size_t byte, bool preceding = true);
// Canonical full run coverage, coalesced equal adjacent styles; LF paragraphs.
[[nodiscard]] TextLayer normalizedText(TextLayer);
[[nodiscard]] TextLayer formatTextRange(
    const TextLayer&, std::size_t first, std::size_t last, const TextStylePatch&);
[[nodiscard]] std::size_t textMemoryCost(const TextLayer&) noexcept;

class TextEditCommand final : public Command {
public:
    TextEditCommand(LayerId, TextLayer before, TextLayer after, TextEditHint beforeCursor,
        TextEditHint afterCursor, std::uint64_t mergeKey = 0, std::string label = "Edit text");
    // First committed insertion creates the layer and content atomically.
    TextEditCommand(Layer created, std::size_t index, std::optional<LayerId> previousActive,
        TextEditHint afterCursor, std::uint64_t mergeKey);
    bool apply(Document&) override;
    bool undo(Document&) override;
    bool canAdoptApplied(const Document&) const noexcept override;
    bool mergeWith(const Command&) override;
    [[nodiscard]] std::string_view label() const noexcept override { return label_; }
    [[nodiscard]] std::size_t memoryCost() const noexcept override;
    [[nodiscard]] std::optional<std::uint64_t> activeLayerAfter(bool undo) const noexcept override;
    [[nodiscard]] std::optional<TextEditHint> textEditAfter(bool undo) const noexcept override;

private:
    LayerId id_;
    TextLayer before_, after_;
    TextEditHint beforeCursor_, afterCursor_;
    std::uint64_t mergeKey_;
    std::string label_;
    std::optional<Layer> created_;
    std::size_t index_ { 0 };
    std::optional<LayerId> previousActive_;
};
}
