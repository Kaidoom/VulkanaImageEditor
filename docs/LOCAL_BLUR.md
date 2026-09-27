# Local Blur

Local Blur (`K`) is a destructive, undoable **raster source-color edit**, not a
layer effect. It never samples displayed color adjustments, persistent spatial
filters, layer opacity, or blending back into those same effects. Raster bytes
are edited through the shared brush transaction; text and shape payloads are
never flattened.

## Controls and coordinates

- Size: document-space brush diameter, shared with Brush.
- Hardness: brush edge coverage, shared with Brush.
- Strength: independent `[0,1]` stroke influence ceiling. The shared brush Flow
  and pressure-to-flow settings determine accumulation below that ceiling.
- Blur Radius: document-pixel Gaussian neighborhood support, `0..64`; the shared
  Gaussian has `sigma = radius / 3`, pixel-integrated normalized taps, and
  integer support `ceil(radius)`. Zero is identity.

Unlike persistent filters, whose radius is layer-local, the Local Blur radius
stays in document pixels on rotated, scaled, stretched, and flipped layers.
The existing brush engine continues to own tips, grain, spacing, smoothing,
pressure, distance resampling, and Shift-constrained paths. `[` / `]` adjusts
size. Foreground color and the Brush opacity control do not tint or gate blur;
Local Blur uses its own Strength instead.

## Source, alpha, and reconstruction

Every stroke captures coherent intrinsic straight-RGBA8 source bytes before any
write. A prepared raw sampler maps document points through the frozen inverse
layer transform, using established alpha-aware linear-light filtering and
crop/chamfer visibility. Other displayed layer effects are intentionally absent.
Sampling may read off-canvas layer content and beyond the brush or selection
write footprint. Outside the source/crop is transparent; internal tile borders
are never image borders.

Shared separable Gaussian convolution operates on linear-premultiplied RGBA
document-pixel tiles. Filtered and original document-grid values are bilinearly
reconstructed together. Their unassociated **color difference** is applied to
the original source color at the target texel. Subtracting the identically
reconstructed baseline prevents neutral kernels from resampling transformed
artwork merely because the document grid differs from the source texel grid.
Reconstruction uses the document pixel grid, which limits detail when sampling
highly minified sources.

The brush interpolates original-to-filtered linear color by accumulated Flow ×
Strength. RasterEditTransaction applies the pinned selection coverage once.
The original destination alpha byte is retained exactly. Fully transparent
destination bytes (including hidden RGB) remain untouched; fully transparent
input RGB contributes nothing. Alpha-weighted neighbors cannot produce opaque
patches or dark color fringes in this color-only workflow.

Repeated events at a stationary point do not add dabs. Overlapping or revisited
regions always reuse pre-stroke source, including cache misses first encountered
after other regions were painted. Separate committed strokes intentionally can
build up additional softening. One stroke is one history action; cancellation
restores exact bytes, and no-effect strokes preserve redo and produce no upload.

## Bounds and costs

- Immutable source: at most **256 MiB**, enough for the RGBA8 source of a
  5120×2880 canvas (56.25 MiB). Larger sources fail before writing pixels.
- Gaussian output: **128×128 document-pixel tiles**, bounded **32 MiB LRU**.
  Eviction changes work only, never results, because the source is immutable.
- Each tile requests its complete shared-kernel halo. Gaussian output/scratch
  has a **16 MiB working cap**, in addition to its sampled input patch.
- The inherited brush/transaction has its own bounded touched-tile budget.
- No idle convolution or frame-driven mutation. Source snapshots and tile caches
  are discarded when the stroke ends or is cancelled.
- Cooperative filtering services paint/progress and Escape events. Mouse/tablet
  moves and releases arriving during a calculation are cloned into one FIFO and
  posted after the current input handler can install/retain pointer capture.
  Turns, pressure, timestamps, and modifiers are retained, not coalesced away.
  The FIFO is bounded at 4096 events; overflow cancels and restores the stroke
  rather than silently dropping trace points. Escape discards deferred input.

`imageeditor_local_blur_tests --bench` reports 4K/5K stroke timings, source/cache/
scratch memory, affected tiles and incremental upload bytes.

## Automated checks

`imageeditor_local_blur_tests` covers independent double-precision Gaussian
reference agreement, fractional selections and holes, neighborhood access beyond
the write mask, alpha preservation and hidden RGB, affine sampling/crop,
adjustment/opacity isolation, tiny-radius identity, event-rate and duplicate-event
consistency, late overlap, progressive separate strokes, cancellation, exact
undo/redo, source invalidation, and no-op upload/history behavior.

`imageeditor_local_blur_ui_tests` covers cached controls/manual numeric input,
independence from shared Brush color/opacity, `K` routing and text-field ownership,
live strokes, alpha/history, Escape/focus cancellation, the no-op path, exact
bent-trace replay through the busy-input barrier, and queue-overflow cancellation. Its
`--native` variant runs the same interaction on Wayland with Vulkan validation.
