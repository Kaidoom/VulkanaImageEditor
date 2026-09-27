# Persistent spatial-filter pipeline

Filters are editable per-leaf-layer metadata, not layers or viewport effects.
Filters run in the fixed order **color adjustments → Gaussian → Motion → Lens**,
followed by [layer effects](LAYER_EFFECTS_V1.md) and final crop/opacity composition.
Folders and groups remain pass-through.
Visiting a page never changes that order or enables another filter.

The UI lives in the fourth category of the existing Adjustments workspace:
**Tone | Color | Monochrome | Filters | Effects**. Its filter dropdown selects cached
Gaussian, Motion and Lens pages. Main-menu Filters actions reveal this same
workspace/category, not another dock. The shared target, Before and Reset All
header follows the active category: spatial filters in Filters; the color
adjustment stack in Tone/Color/Monochrome. Captured-region overlays follow the
same ownership, and leaving Filters commits an ongoing numeric edit once.
Category headers share one layout: dropdown and the active effect's Enabled
checkbox, followed by the scope/Capture/Reset row. Per-filter Reset restores the
entire default filter, including disabled state, neutral parameters, no capture
and Preserve Alpha off; undo restores all of it.

## Source, alpha and coordinates

- Original straight-sRGB RGBA8 raster bytes, rich text, shape geometry, and
  layer transforms remain authoritative. Disposable typed render caches are
  shared rather than regenerated for a parameter edit.
- Distances are layer-local pixels. Typed cache density scales kernel sampling,
  not saved parameters. Layer rotation, shear, stretch and flips operate on the
  final layer geometry, including its effect.
- Decoded linear premultiplied RGB and alpha are filtered together. The stack
  stays floating-point between stages. At the final cache boundary only, RGB is
  unassociated, sRGB encoded, and quantized with alpha to straight RGBA8.
  Vulkan and CPU sampling then consume this same image with the established
  alpha-aware filtering and blend contract.
- Preserve Alpha uses alpha-weighted neighboring color but the original
  destination alpha. Zero-alpha colors cannot inject hidden RGB. Each captured
  immutable R8 mask blends original and filtered **output** in premultiplied
  linear space; it does not prevent reading neighbors outside the mask.
- The input beyond actual source bounds is transparent, not edge-clamped.
  Every stage expands support conservatively; the padded cache retains a
  pixels-to-local origin, including negative offsets. Crop and chamfer evaluate
  afterward using the original layer-local coordinates.
- Existing explicitly intrinsic source modes, including Source Layer cloning
  and intrinsic Active Layer color access, remain intrinsic. Merged and rendered
  references include filters. Unprepared eyedropper hover reports unavailable
  until the async cache is ready instead of blocking on full-image processing.

## Preparation, history and limits

The GUI freezes mutable raster input in cancellable row blocks before dispatch.
A serial worker owns that immutable input, floating-point intermediates and
final encoding. A result is admitted only when source identity/revision, source
mapping, adjustments and filters still match. View changes, pan, selection and
caret blinking do not regenerate the effect. No filtering happens inside the
Vulkan frame-recording path. Existing uploaded original textures are retained,
so Before / After and parameter edits do not repeatedly upload the source.

Cheap bounds/working-set preflight runs before accepting a parameter preview.
Default per-job working-set admission is 768 MiB, including a frozen raster and
float intermediates. Published spatial caches have a 768 MiB aggregate
per-document limit; flattened output also applies its existing 256 MiB derived
cache limit. Output dimensions are bounded at 32768 per axis, typed cache
density at 8×, and each kernel enforces its own operation/memory ceiling.
Admission errors are explicit; no partial cache is published.

One completed numeric adjustment or action records filter metadata in history.
Parameter edits remain undoable while preparation runs. Cancelling an active
edit restores its starting metadata; cancelling *processing* after an edit has
completed retains those valid settings and reports that the cache is not
ready. Runtime allocation/processing failures likewise retain completed edits,
with an explicit error, rather than silently undoing user work. Change a filter
or retry an output operation to request preparation again.

### Cache invalidation

An input revision or parameter change currently invalidates the complete padded
effect cache. Jobs are coalesced/cancellable and source snapshots are reused
across parameter-only changes. The new derived image uploads once on completion.
Local Blur's raster transaction uses incremental original-surface updates.
Large radii or heavily magnified typed content can exceed admission limits.

## Other consumers and persistence

Prepared rendered references, merged sampling, group thumbnails, flattened
export and merges all use the same effect cache and skip the color-adjustment
stage that was already evaluated into it. Group thumbnails wait for preparation
rather than convolving during row painting. Export and merge can explicitly
prepare a missing coherent cache with their cooperative cancellation callbacks.
Merge composites only selected content onto transparency, retaining its relative
order and placing the result at the topmost selected position.

Ctrl+J without a raster selection duplicates the original pixels and editable
filter stack. With a raster selection, extraction bakes the adjusted/filtered
appearance *before* applying selection coverage, removes the baked effects from
the result, and keeps layer opacity separate. Thus neither truncated source
neighborhoods nor double filtering changes the extracted appearance.

Projects save the fixed stack, stable string identifiers, algorithm versions,
options and independently owned mask payloads. They declare required capability
`spatial-filters-v1`; older readers reject unsupported essential effects.
Missing filter descriptors mean identity. Unknown types, altered order, unknown
algorithm versions, malformed matrices or missing/oversized mask payloads are
rejected before document admission. Caches and history are not serialized.

## Tests

- `imageeditor_spatial_filter_integration_tests`: exact source preservation,
  transformed cache reuse, alpha/masks, history/cancellation/redo, copies,
  raster/text/shape output, project round-trips and future-version rejection.
- `imageeditor_filters_ui_tests`: cached embedded pages, menu/category routing,
  shared-header scope, captured-region ownership, linked dimensions, numeric
  grouping across category changes and async stale-result handling.
- `imageeditor_wayland_filters`: the same MainWindow/worker interactions on
  native Wayland with actual Vulkan frame submission and validation, including
  application/document destruction.
- `imageeditor_spatial_filter_rendering_tests`: real Vulkan against CPU
  reference, padded bounds, crop, layer blend/opacity, effect uploads and retained
  original textures. Runs the existing offscreen GPU host with validation.

For a compact paired GPU/CPU sheet:

```sh
QT_QPA_PLATFORM=offscreen IMAGEEDITOR_BLEND_REVIEW=/tmp/vulkana-filter-comparison.png \
  build/release/tests/imageeditor_blend_rendering_tests --filters --validation
```

For source-freeze, filtering, encoding and publication timings:

```sh
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_spatial_filter_integration_tests --bench
```
