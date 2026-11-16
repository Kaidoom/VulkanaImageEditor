#pragma once
#include "imageeditor/core/Command.hpp"
#include "imageeditor/core/Geometry.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace imageeditor::core {
enum class GuideOrientation { Horizontal, Vertical };
struct DocumentGuide {
    std::uint64_t id { };
    GuideOrientation orientation { GuideOrientation::Horizontal };
    double position { }; // Document pixels, independent of zoom and device scale.
    friend bool operator==(const DocumentGuide&, const DocumentGuide&) = default;
};
struct DocumentGuides {
    std::vector<DocumentGuide> items;
    std::uint64_t nextId { 1 };
    friend bool operator==(const DocumentGuides&, const DocumentGuides&) = default;
};
[[nodiscard]] bool validDocumentGuides(const DocumentGuides&);
[[nodiscard]] std::vector<double> guidePlacementPositions(
    GuideOrientation, unsigned count, Extent2u canvas);
class ChangeDocumentGuidesCommand final : public Command {
public:
    ChangeDocumentGuidesCommand(DocumentGuides before, DocumentGuides after, std::string label);
    bool apply(Document&) override;
    bool undo(Document&) override;
    std::string_view label() const noexcept override { return label_; }
    std::size_t memoryCost() const noexcept override;

private:
    DocumentGuides before_, after_;
    std::string label_;
};
}
