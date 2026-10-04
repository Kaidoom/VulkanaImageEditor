# Bevel & Emboss

One independently enabled effect for raster, text and shape layers. Adjustment
layers remain correction operators. The effect uses local pixels; whole-layer
mapping carries its relief and light together, including flips and perspective.
Shape geometry edits do not accumulate scale into Size.

## Controls

Inner Bevel shades inside the silhouette; Outer Bevel shades outside; Emboss
spans both sides. Smooth is the supported technique. Size is the relief width,
Depth is its slope multiplier (100% = 1), and Down reverses height. Soften filters
derived height, never the source. Size is 0–256 px, Depth 0–1000%, Soften 0–64 px.

Angle points toward the light: 0° right, +90° down. Altitude is 0–90°. Light is
per effect, not document-global. Initial settings are Inner, 6 px, 100%, Up,
−135°/30°, white Screen highlight at 65%, black Multiply shadow at 55%.
Existing documents start disabled. Zero Size/Depth, or zero strength for both
shading contributions, bypasses preparation and composition.

Contour stays on this page. Surface Profile shapes relief; Gloss Contour remaps
lighting. The selector is view state. Both curves retain their own enable state,
points, interpolation and corner flags. Presets are generated data, not external
assets. Reset and ordinary edit/cancel/history conventions apply.

## Construction and composition

The common pre-style silhouette is the adjusted/filtered source alpha. Neither
RGB nor previously generated styles enters geometry. As with existing effects,
crop/chamfer and the bitmap layer mask restrict the **completed styled output**;
they are not applied to the silhouette and then applied again. Consequently a
mask cut does not create a new bevel boundary. Clipping groups use their existing
content-coverage domain; a base's exterior relief does not enlarge that domain.

1. Seed the nonzero-alpha support boundary with subpixel distances. A boundary
   texel's interior seed is `0.5 × alpha / localPeak`, clamped to 0.01–0.5;
   exterior seeds are 0.5. `localPeak` is the 5×5 maximum near that boundary.
   Keep source alpha and nearest-boundary peak separately. This retains partial
   coverage, avoids hidden-RGB edges, and gives uniformly translucent content
   the same underlying geometry as its opaque counterpart.
2. A two-pass lower-envelope transform computes squared Euclidean distance to
   these weighted boundary seeds in O(pixel count). Signed distance is positive
   inside. This is a sampled, subpixel-weighted distance field, not an exact
   analytic distance to a reconstructed vector contour.
3. Normalize distance across `[0, Size]` for Inner, `[-Size, 0]` for Outer, and
   `[-Size/2, Size/2]` for Emboss. Clamp the normalized coordinate. With surface
   curve `C`, height is `Size × Depth × smoothstep(C(t))`, negated for Down.
   Four stratified samples integrate the profile over a texel. The existing
   Gaussian filters height with support `3 + Soften` texels at 1× resolution
   (the existing radius = 3σ convention). This reconstructs thin/curved boundaries
   and smooths opposing slopes without changing original pixels.
4. Central height differences produce `normalize(-dx, -dy, 1)`. Cache XY as
   signed normalized 16-bit components and recover positive Z; distance remains
   float. Both CPU and Vulkan consume the same resulting lighting masks.
5. Compute `r = N·L − Lz`, remap `0.5 + 0.5r` through gloss `G`, subtract
   `G(0.5)`, and split twice that signed response into positive highlight and
   negative shadow, each clamped to [0,1]. The subtraction makes a flat surface
   neutral even for edited curves. Enabled gloss uses a 1.5-texel Gaussian
   response filter to antialias sharp transitions. Support fades through the
   reconstruction/Soften halo and remains bounded.

Curve X coordinates run strictly from 0 to 1, with at least 1e−6 separation,
2–16 points, and independently bounded Y values. Linear segments or bounded
piecewise cubic Hermite interpolation allow nonmonotonic profiles without
overshoot. Corner points use separate incoming/outgoing secants. Disabled curves
evaluate as identity while retaining their points. There is no disposable LUT
or preset-name-only serialization.

Interior shadow then highlight alter unassociated color and restore original
coverage. They follow overlays/inner glows and precede inside Stroke. Exterior
shadow then highlight blend against the actual accumulated backdrop after source
composition and before outside Stroke. Exterior coverage is the nearest boundary
peak times `(1 − sourceAlpha)`, within the relief support. Exterior styles can
cover nearby transparent holes; Inner cannot fill them. Overall layer opacity,
crop and layer mask retain their existing once-only contribution boundary.
No existing effect's relative order changes.

The shared CPU/GLSL blend implementation retains linear-light premultiplied
composition and the existing named-mode color convention. There is no flattening
onto an assumed Normal backdrop. Raw-source editing remains raw.

## Caches and persistence

Distance/coverage, normals, lighting masks, and material parameters are separate.
Color, opacity and blend mode reuse all geometry and masks. Angle/Altitude/Gloss
reuse distance and normals. Surface/Depth/Direction rebuild normals, not distance.
Size/Soften can reuse a distance field with sufficient halo; expanding beyond it
prepares a larger field. Padding never changes the logical frame or gradient
anchor. R8 is the existing final CPU/GPU lighting-mask boundary; all profile,
normal and lighting construction precedes that quantization.

The existing bounded serial preparation worker coalesces edits, checks source
revisions and document-instance ownership, and rejects stale/cancelled jobs.
Distance/normal storage is 10 bytes per padded texel, plus compressed lighting
masks. Preparation admission, retained document caches, aggregate inactive-tab
eviction, export/repair budgets and status memory include this storage. Material
edits need no source or mask upload. Lighting changes upload derived masks only.

`.vulkana` retains a version-1 `bevel-emboss` effect record and declares
`bevel-emboss-v1` when the record differs from its disabled defaults. The existing
seven-effect descriptor remains readable. Unknown essential versions are refused.
History and cross-document transfer retain independent immutable settings, not
disposable geometry caches. All canonical consumers, including PDF's raster
fallback for styled text, evaluate this effect. Merge/Rasterize use the existing
selected-only isolated policy and bake active styles once.

## PSD exchange

Modern `ebbl` supports Smooth Inner/Outer/Emboss, direction, depth, size, soften,
separate highlight/shadow settings, angle/altitude, `MpgS` surface and `TrnS` gloss
curves. `Cnty` maps smooth/corner information. Exported disabled Gloss uses an
identity curve because PSD has no corresponding independent enable flag.
Surface enablement uses `useShape`; contour range must be 100%. Local widths and
light vectors are converted through similarity transforms. Other typed mappings
use the review's raster/composite choice. Imported shared light resolves resource
1037/1049 into this effect without changing other layers.

These are editable equivalents with **appearance differences**, not a promise
of identical relief/gloss algorithms. Chisel, texture, Pillow/Stroke Emboss,
unsupported curve/range data and legacy-only `lrFX` remain explicit review
fallbacks. Modern records take precedence over duplicate legacy records; effects
are not applied twice. Exterior non-Normal shading may need a larger chosen
consolidation or Flattened PSD for appearance, rather than an isolated-layer bake.

Record evidence: [Adobe format specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/),
[Krita's style serializer](https://invent.kde.org/graphics/krita/-/blob/master/libs/image/kis_asl_layer_style_serializer.cpp).
No code or contour assets from those projects are incorporated.

## Verification commands

```sh
cmake --build --preset release -j 6
ctest --test-dir build/release --output-on-failure
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_bevel_effects_tests --sheet build/bevel-sheet.png
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_bevel_effects_tests 4096 4096
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_bevel_effects_tests 5120 2880
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_blend_rendering_tests --effects --benchmark --validation
QT_QPA_PLATFORM=wayland build/release/tests/imageeditor_filters_ui_tests --native
QT_QPA_PLATFORM=xcb build/release/tests/imageeditor_filters_ui_tests --native
```

The bevel test also generates native PSD records with `--psd <existing directory>`.
`tests/inspect_bevel_psd.py` optionally checks those with development-only
psd-tools and produces variants with unchanged saved composites for independent
live-effect redraw testing. Receiving-editor visual/editing acceptance and
interactive large-document feel remain manual checks.
