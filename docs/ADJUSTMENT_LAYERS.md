# Adjustment layers

An adjustment layer is a stack operator, not a raster surface. Add one through
the Layers menu, context menu, or slider icon in the Layers footer. The existing
Tone, Color and Monochrome controls edit its ordered correction collection.
Filters and Effects are unavailable for this layer type. The eye bypasses the
correction; Strength controls its contribution. Normal is the only layer blend
mode in V1.

## Composition and scope

The shared CPU/Vulkan composition plan evaluates bottom to top. At an adjustment,
the input is the lower composite after its masks, effects, opacity and blending.
The existing adjustment equations operate on that float input. With coverage
`a`, original premultiplied RGB `C`, adjusted RGB `A`, and strength
`s = opacity × layer-mask coverage`, the result is `(C + s × (A − C), a)`.
It is not a second source-over image. Transparent input contributes nothing.
Disabled/neutral corrections and zero strength bypass without color conversion.
Corrections on one layer retain the order in [Adjustment algorithms](ADJUSTMENT_ALGORITHMS.md).

- **All Below** processes the lower accumulated image in the current domain.
  Ordinary pass-through folders do not limit it; clipping or local domains do.
- **This Group** processes lower content in the immediate container, including
  nested subtrees. It establishes a transparent local domain for that container;
  the completed container composites normally over its outside backdrop.
  This changes how backdrop-sensitive children blend, separately from changing
  the correction. The settings panel identifies this behavior.

The serialized scope explicitly owns the local-domain policy
`scope-owned-isolation-v1`. A hidden, neutral or zero-strength scoped adjustment
still retains its container's local domain. Removing it, reparenting it, or
changing it to All Below releases that boundary unless another scoped adjustment
still requires it. These structural/scope changes are undoable. There is no
parent ID to become stale or isolation flag left behind after removal.
At the root both scopes mean All Below; the UI offers only All Below there.

Clipping groups retain their existing composition contract. An upper adjustment
corrects the working stack color while the base coverage/opacity is applied
once. It cannot escape the group. An adjustment has no clipping silhouette:
as the bottom child it makes a multi-child clipping group empty. A lone
adjustment inside a clipping group has no lower input and also contributes
nothing. Ordinary groups without scoped adjustments are unchanged.

## Masks, editing and storage

Creation uses the current selection as an owned mask, or compact Reveal All.
White applies the correction; black protects the input; gray mixes strengths.
Mask thumbnail editing reuses regional brush/fill history. Delete, invert,
enable/disable and mask-to-selection work without modifying lower pixels.
Apply Mask is unavailable: the operator has no independent pixels to bake.

The mask frame is layer-local, initially the document pixel grid. Moving or
transforming the adjustment moves its mask/captured regions, not the geometry
of the lower layers. Canvas resize and reparenting do not rescale that frame.
Ordinary painting requires selecting the mask or a real raster layer.

`.vulkana` stores the adjustment payload, scope, ordered parameters and optional
R8 mask with required capability `adjustment-layer-v1`. There is no canvas-sized
RGBA placeholder. Old files retain their existing behavior; readers lacking the
capability reject the project rather than dropping the operator. Duplication,
cross-document copy and history retain independent editable state.

## Evaluation and caches

CPU point/row sampling, Pixel Preview, thumbnails of containing groups, native
baking and exports use the structural composition plan. The operator's own
thumbnail is its icon, paired with the ordinary mask thumbnail.

Vulkan evaluates the same float equations directly on the composition target.
It retains the lower input before the first active operator in each eligible
domain, using exact document-instance, structure, geometry and source-revision
keys. Pass-through nesting is normalized for this purpose. Changes above that
operator or to its parameters/mask reuse the input; lower edits invalidate it.
Multiple corrections share this prefix rather than allocating a complete canvas
for every operator. Prefix cache admission is capped at 256 MiB; combined
clipping/local-domain scratch retains the existing 512 MiB limit. A domain that
cannot admit another cached input evaluates without that optional cache.
Resources also obey the existing device storage-buffer limit.

Levels/Curves histograms sample the pre-stage lower input in bounded GUI slices.
Their keys exclude above-stack artwork and the current/later corrections.
Tab activation clears target identity; inactive panels do not run histogram work.
Pixel Preview freezes mutable visible sources for its worker, keeps hierarchy,
and rejects stale document-instance results. Source pixels are not uploaded again
for a parameter-only edit, and unchanged scenes remain demand-driven.

## Merge and exchange

Selected-only Merge evaluates operators against only the included lower content.
It never pulls in unselected artwork. Select the intended lower layers together
with an adjustment and Merge, or rasterize a containing group. Rasterizing an
adjustment alone is rejected with that explanation.

PDF page entries exclude adjustment operators. Each selected drawable/group
page receives the applicable operators at their original positions/domains,
without adding unselected drawable content. Hidden scope owners retain the
domain without applying their correction. A page with an active adjustment uses
canonical raster rendering rather than emitting uncorrected native PDF text.

PSD native mappings currently cover **Invert** and **Exposure** with zero offset
and unit gamma, Normal blending and supported bitmap-mask geometry. A local
domain is represented as a Normal PSD folder; ordinary pass-through folders
remain pass-through. Fractional strength/masks and non-sRGB imported working
spaces are reviewed as possible appearance differences. Unsupported correction
types, per-correction masks, or several combined corrections require an explicit
group consolidation, omission, or Flattened PSD choice. The exporter never
repeats a combined mask/opacity across separate correction layers. Compatibility
composites are freshly rendered from the accepted plan.

Native record/independent-parser checks are not a claim that a particular
Photoshop version has been manually verified. Receiving-editor redraw, masks,
fractional opacity and saved/reopened editability remain manual checks.

## Focused verification

```sh
cmake --build --preset release -j 6
ctest --test-dir build/release --output-on-failure -R 'adjustment|clipping|layer_mask|pdf_export|psd_export'
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_blend_rendering_tests --benchmark --adjustment-layers --validation
QT_QPA_PLATFORM=wayland build/release/tests/imageeditor_adjustment_layer_tests --native
QT_QPA_PLATFORM=xcb build/release/tests/imageeditor_adjustment_layer_tests --native
```

Native checks require a working display and Vulkan validation layer. XCB under
XWayland is not native X11 coverage. Test fixtures are generated locally; private
PSD samples are not required or packaged.
