#include "imageeditor/core/DocumentGuides.hpp"
#include "imageeditor/core/Document.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace imageeditor::core {
bool validDocumentGuides(const DocumentGuides& guides)
{
    if (!guides.nextId)
        return false;
    std::unordered_set<std::uint64_t> ids;
    for (const auto& guide : guides.items)
        if (!guide.id || guide.id >= guides.nextId || !ids.insert(guide.id).second
            || (guide.orientation != GuideOrientation::Horizontal
                && guide.orientation != GuideOrientation::Vertical)
            || !std::isfinite(guide.position) || std::abs(guide.position) > 1e9)
            return false;
    return true;
}
std::vector<double> guidePlacementPositions(
    GuideOrientation orientation, unsigned count, Extent2u canvas)
{
    if (!count || canvas.empty())
        return { };
    const auto extent = double(orientation == GuideOrientation::Horizontal ? canvas.height : canvas.width);
    std::vector<double> positions;
    positions.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        positions.push_back(extent * (double(i) + 1) / (double(count) + 1));
    return positions;
}
ChangeDocumentGuidesCommand::ChangeDocumentGuidesCommand(
    DocumentGuides before, DocumentGuides after, std::string label)
    : before_(std::move(before))
    , after_(std::move(after))
    , label_(std::move(label))
{
}
bool ChangeDocumentGuidesCommand::apply(Document& document) { return document.setGuides(before_, after_); }
bool ChangeDocumentGuidesCommand::undo(Document& document) { return document.setGuides(after_, before_); }
std::size_t ChangeDocumentGuidesCommand::memoryCost() const noexcept
{
    return sizeof(*this) + label_.capacity()
        + (before_.items.capacity() + after_.items.capacity()) * sizeof(DocumentGuide);
}
}
