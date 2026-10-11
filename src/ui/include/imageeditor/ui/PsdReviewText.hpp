#pragma once
#include "imageeditor/ui/Localization.hpp"
#include "imageeditor/ui/PsdImport.hpp"
#include "imageeditor/ui/PsdExport.hpp"

namespace imageeditor::ui::i18n {
inline Message psdContentName(PsdContentKind kind, bool exporting = false) {
    switch (kind) {
    case PsdContentKind::Raster: return text("psd.review.type.raster");
    case PsdContentKind::Text: return text("psd.review.type.text");
    case PsdContentKind::Shape: return text("psd.review.type.shape");
    case PsdContentKind::Adjustment: return text("psd.review.type.adjustment");
    case PsdContentKind::Group: return text(exporting ? "psd.review.type.folder" : "psd.review.type.group");
    case PsdContentKind::ClippingGroup: return text("psd.review.type.clipping");
    }
    return {};
}
inline Message psdImportStatus(const PsdLayerInfo& layer) {
    auto result = layer.editable
        ? text(layer.issues.empty() ? "psd.review.editable" : "psd.review.approximate")
        : text(layer.raster ? "psd.review.saved.raster" : "psd.review.review.required");
    if (layer.clippingBase >= 0) result += text("psd.review.clipped.suffix");
    return result;
}
inline Message psdSourceSummary(const PsdInspection& inspection) {
    const auto color = inspection.colorHandling == PsdColorHandling::AssumedSRgb
        ? text("psd.review.color.untagged")
        : inspection.colorHandling == PsdColorHandling::SRgb
            ? Message("sRGB") : text("psd.review.color.converted").arg(inspection.sourceProfile);
    return text("psd.review.source.summary").arg(inspection.size.width()).arg(inspection.size.height())
        .arg(inspection.ppi).arg(Message("\u2068") + color + "\u2069");
}
inline Message psdExportProblem(const PsdExportPlan& plan) {
    switch (plan.problem) {
    case PsdExportProblem::EditableUnavailable: return text("psd.review.export.editable.unavailable").arg(plan.problemLayer);
    case PsdExportProblem::NeedsGroup: return text("psd.review.export.needs.group").arg(plan.problemLayer);
    case PsdExportProblem::MissingBase: return text("psd.review.export.missing.base").arg(plan.problemLayer);
    case PsdExportProblem::BaseStyles: return text("psd.review.export.base.styles").arg(plan.problemLayer);
    case PsdExportProblem::None: return Message(plan.error); // Technical diagnostic stays verbatim.
    }
    return Message(plan.error);
}
}
