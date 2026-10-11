#pragma once

namespace imageeditor::ui {
// Presentation identifiers travel with worker results. English technical
// reports remain separate; UI decisions never inspect translated wording.
enum class PsdContentKind { Raster, Text, Shape, Adjustment, Group, ClippingGroup };
enum class PsdColorHandling { AssumedSRgb, SRgb, ConvertedToSRgb };
enum class PsdExportProblem { None, EditableUnavailable, NeedsGroup, MissingBase, BaseStyles };
}
