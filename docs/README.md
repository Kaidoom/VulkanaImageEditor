# Developer documentation

Build/run/test instructions are in the [repository README](../README.md).
The guides below describe the architecture, behavior and algorithms.

- [Architecture](ARCHITECTURE.md): module boundaries, document ownership,
  rendering, transactions and resource lifetime.
- [Coordinates](COORDINATE_SYSTEM.md): pixel grids, transforms and input mapping.
- [Project format](VULKANA_FORMAT.md): persistence, limits and compatibility.
- [Document tabs](DOCUMENT_TABS.md): ownership, activation and cross-tab copying.
- [Refine Selection](REFINE_SELECTION.md): guided edges, coverage controls, mask output and session safety.
- [Smart Selection](SMART_SELECTION.md): brush-local regions, bundled offline object selection and quality checks.
- [PDF import](PDF_IMPORT.md): page geometry, rasterization, destinations and limits.
- [PSD import](PSD_IMPORT.md): editable conversion, review choices, saved-raster scope and limits.
- [PDF export](PDF_EXPORT.md): page ordering, physical sizes, hybrid text and safety.
- [Shortcuts](SHORTCUTS.md): bindings, focus ownership and configuration.
- [Single instance](SINGLE_INSTANCE.md): startup arbitration and file forwarding.
- [Adjustments](ADJUSTMENT_ALGORITHMS.md): color/alpha equations and masks.
- [Adjustment layers](ADJUSTMENT_LAYERS.md): lower-stack operators, scope and exchange.
- [Layer effects](LAYER_EFFECTS_V1.md): style order, bounds and compositing.
- [Clipping groups](CLIPPING_GROUPS.md): structural membership, alpha math and baking.
- [Spatial filters](SPATIAL_FILTER_PIPELINE.md) and [kernels](spatial-filter-kernels.md).
- [Local blur](LOCAL_BLUR.md), [Heal](HEALING_ALGORITHM.md), and
  [Spot Heal](SPOT_HEAL_ALGORITHM.md): algorithms and integration contracts.
- [Optional online services](../config/SERVICES.md): private config and offline builds.
- [Source license](../LICENSE) and [third-party notices](../THIRD_PARTY.md).
