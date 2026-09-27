# Layer effects

Effect settings use data version 1.

## Editing

The fifth Adjustments category, Effects, follows the primary renderable layer.
Its seven persistent pages each own an independent Enabled state. Selecting a
page does not enable it. Parameters can be prepared while disabled. The dropdown
also lists the enable states. Colors belong to the effect, not foreground or
background swatches. Folders remain organizational/pass-through containers.

Reset beside Enabled restores just that effect's defaults (disabled). Reset Effects
resets all seven styles without changing adjustments, filters, crop, source
pixels, or geometry. Hold for Before bypasses styles only, including native
Pixel Preview. Comparison/navigation are not project edits. Numeric gestures
and color-dialog previews use one stable-target transaction; Escape restores
the starting settings. The color picker is hosted in the existing workspace
dialog, not a separate desktop window.

## Authoritative construction

`LayerEffects.cpp` prepares geometry; `detail/LayerEffectMath.inc` is shared by
CPU sampling and the Vulkan compositor. Existing `BlendMath.inc` supplies every
blend mode, including its established unassociated-sRGB named-mode stage and
linear-light premultiplied alpha composition. There are no new blend formulas.

1. Prepare source content and the existing adjustments/Gaussian/Motion/Lens stack.
2. Derive **all** style masks from this one pre-style alpha silhouette, before
   crop. Effects do not feed their generated pixels into other effects' masks.
3. Composite Drop Shadow, then Outer Glow against the actual document backdrop.
4. Modify base color, in order: Color Overlay, Gradient Overlay, Inner Shadow,
   Inner Glow, inside Stroke. These operations retain the original base alpha.
5. Composite that base with its external layer blend mode.
6. Composite outside Stroke with its own blend mode.
7. Interpolate the original backdrop toward the complete styled result by the
   layer opacity times final crop coverage. Overall opacity/crop apply once.

Crop/chamfer remains an **output restriction**, not destructive source trimming.
Retained content can influence a filter or shadow inside the crop; generated
pixels outside the crop are hidden. Fractional crop coverage is never applied
once to the silhouette and again to its result. Document clipping remains final.

Drop Shadow uses the full source silhouette behind the content: opaque content
occludes it, translucent content deliberately permits it to show through. It is
not additionally multiplied by `(1-alpha)` before source-over, which would double
the occlusion. Outer Glow uses positive excess of the expanded mask over source
alpha. Exterior stroke likewise uses the expanded-minus-original coverage;
inside stroke uses `(alpha-eroded)/alpha` where alpha is positive. Inner effects
use `(alpha-offset/softened eroded alpha)/alpha`, clamped to 0–1. The inside
color operation then restores the original alpha, preventing alpha-squared edges.

### Geometry and units

Widths and distances use local pixels. Shape geometry resizing changes shape
dimensions, not style widths. Explicit whole-layer affine transforms carry the
source and styles together, including rotation, shear, flips and scaling.
Styled visual bounds are separate from logical bounds used for handles/snapping.

Morphology is grayscale circular max/min, not binary thresholding or a square
kernel. A max pyramid accelerates the exact circular query. Its radial coverage
kernel is `clamp(radius + 1 - distanceBetweenTexelCenters, 0, 1)`; grayscale dilation
takes `max(min(inputAlpha, kernel))`, and erosion is the dual. Radius zero is an
exact identity; positive subpixel radii contribute partial coverage. An exhaustive
double-precision test checks this definition independently of the pyramid.

Stroke uses Size inside/outside or half Size on each side for Center. For shadows
and glows, morphology radius is `Size × Spread/Choke`; remaining Gaussian support
is `Size × (1-Spread/Choke)` (the existing radius = 3σ convention). Thus 0% is all
softening; 100% is solid circular morphology, with no Gaussian pass. Zero size
does no morphology/blur; a Drop Shadow can still have an offset. Positive angles
are clockwise, 0° right and 90° down. Fractional offsets use alpha interpolation.

Float intermediates retain antialiasing and partial alpha. Finished geometry is
quantized once into the existing compressed R8 mask representation. Linear mask
sampling includes the half-texel fringe in visual bounds. Empty masks do not
expand bounds. Widths range up to 256 px and distances up to 512 px. Noise is
not supported.

### Gradients

The reference rectangle is raster intrinsic extent, shape geometry size, or text
layout logical extent, in local coordinates starting at (0,0). Filter/style cache
padding, pan/zoom, and crop never shift this frame. Linear gradients project onto
the specified angle across that rectangle; radial gradients use its centered
elliptical normalized radius. Scale changes the span/radius; Reverse swaps the
parameter direction. Endpoints clamp outside the span.

Endpoint RGB is decoded to linear light and premultiplied by endpoint alpha
before interpolation. Both associated color and alpha interpolate as floats;
transparent endpoint RGB is irrelevant. The result goes through the normal
blend function, using its alpha as color-effect strength, while base coverage
remains unchanged. No intermediate gradient bitmap or RGBA8 quantization is used.

## Consumers and destructive operations

Canvas, prepared rendered references, native output, Pixel Preview, and relevant
thumbnails use the same program and geometry. Typed sources with active spatial
styles use a canonical 1× local source, as spatial filters do, independent of
viewport DPI/zoom. Text and shape models remain editable; styled previews are
rasterized at this local resolution.

Raw-source Brush/Erase/Heal/clone paths remain raw. Rendered cloning uses the
styled reference; painting it into another styled destination can apply the
destination's styles again. The existing retouch-layer warning and Spot Heal
admission rules now include styles. Source Layer remains available for raw repair.

Merge Selected still normalizes roots, isolates selected content on transparency,
preserves selected ordering, and consolidates at the topmost selected placement.
Unselected backdrops are excluded.

Rasterize and selected Ctrl+J extraction bake styles on transparency **before**
the layer's external blend mode and opacity, which remain separate properties.
Exterior effects are flattened against transparency, so their interaction with
a backdrop can differ from the editable Multiply/Screen effects.
Active baked style state is removed once; Rasterize keeps disabled/unbaked
settings. Unselected Ctrl+J and Duplicate preserve editable effect settings.

Projects declare required capability `layer-effects-v1` and stable effect IDs.
Old projects have no styles. Unknown essential types/versions are rejected on
load, not silently dropped on resave. History retains settings/source models but
discards disposable effect caches. Group/Ungroup leaves child settings intact.

## Work and memory

The existing serial, cancellable spatial-preparation worker is reused with
immutable source snapshots, coalesced requests, and revision-checked publication.
Source/filter results and unchanged per-effect masks are reused. Effect color,
opacity, blend, and gradient parameters do not rebuild masks. Vulkan uses the
existing compressed mask atlas and parameter buffers, not new source textures.
Cursor/pan/zoom do not re-run morphology or blur; idle rendering is unchanged.

Admission includes a 768 MiB per-job working budget, 384 MiB document mask-cache
budget, and existing 128 MiB GPU mask/16 MiB parameter budgets. Oversized work
fails visibly without modifying source data. Expensive geometry previews finish
asynchronously; material-only changes are immediate. Masks remain native quality.

## Tests

`imageeditor_layer_effects_tests` and `imageeditor_layer_effects_vulkan` cover
circular morphology, alpha/holes, disabled identity, blend modes, geometry reuse,
crop/transforms, opacity, history, save/load and rasterization. Filters UI and
native baking tests cover transactions, navigation and Pixel Preview.

Generate the comparison sheet or run native interaction checks:

```sh
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_layer_effects_tests --sheet build/layer-effects-comparison.png
QT_QPA_PLATFORM=wayland build/release/tests/imageeditor_filters_ui_tests --native
QT_QPA_PLATFORM=xcb build/release/tests/imageeditor_filters_ui_tests --native
```

Run native suites sequentially to avoid focus interference.
