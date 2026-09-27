# Spatial filter kernels

## Mathematical contract

The shared platform-neutral primitives are in `SpatialFilters.hpp/.cpp`. Persistent filter order is fixed: **Gaussian → Motion → Lens**, after color adjustments and before crop/chamfer coverage, layer opacity and named-mode compositing. Visiting a UI page never changes that order. Algorithm version is `1`; identifiers are `gaussian-blur`, `motion-blur`, and `lens-blur`. Unrecognized essential versions/types are not silently interpreted as identity.

Distances use layer-local pixels for persistent filters. A cache's `pixelScale` converts those stable units into its evaluation grid without modifying document parameters. Local Blur chooses its document-pixel reference grid separately and consumes the identical Gaussian primitive. The kernel code has no Qt, Vulkan, screen, or document ownership dependencies.

Four-channel planes are **linear-light premultiplied RGBA floats**; authoritative surfaces remain straight sRGB RGBA8. Decode/unassociate only at the existing boundaries. Do not filter straight RGB or quantize between stages. Scalar planes use the same kernels for future mask effects.

Outside the real source extent is transparent zero. There is no edge extension or per-edge renormalization: an opaque image can soften into transparent padding. Input patches must include `requiredSpatialInputBounds`; a tile edge is not an image edge. `expandedSpatialOutputBounds` propagates support through the ordered chain. Preserve Alpha keeps output bounds unchanged but still requires the full input neighborhood. Bounds remain integer grids with pixel centers `(x+.5,y+.5)`; padded origin must be retained by consumers.

Let `B=(b,ab)` be the original premultiplied pixel and `F=(f,af)` the convolution. Alpha-changing mode uses `F` directly. Preserve Alpha computes `F'=(f*ab/af,ab)` for positive `af`, otherwise zero color and original alpha. An original zero-alpha destination stays transparent. The ratio derives color only from alpha-weighted neighbors: hidden RGB cannot bleed into edges. Captured-mask coverage `m` then produces `B + m*(F'-B)` (or `F` when changing alpha). The mask restricts **output contribution**, never the available neighboring source.

## Gaussian

Radius `r` means **three standard deviations**, `sigma=r/3`, with continuous support `[-r,r]`. A tap at integer offset `i` is the integral of `exp(-x²/(2 sigma²))` over `[i-.5,i+.5] ∩ [-r,r]`. The erf integral is normalized after truncation. The integer allocation support is `ceil(r)` in each axis; weights can be zero at its outermost cells. Radius zero is exact identity. Sub-half-pixel radii remain identity under this explicit area-sampled discrete contract. Fractional support enters continuously, avoiding a jump when `ceil(r)` increases.

The kernel is separable. Horizontal rows are cached in a bounded rolling buffer, then vertically accumulated. Double accumulation and float intermediate storage agree with the independent direct double reference within `1.2e-7` for the regression fixtures.

The finite separable method and boundary conditions are discussed in [Getreuer, IPOL 2013](https://www.ipol.im/pub/art/2013/87/).

## Motion

Distance is the **full trail length**, centered on the original pixel. Angles follow document axes: 0° right, 90° down, positive clockwise. A trail averages the bilinearly reconstructed source along its line. The integration is split exactly where the line crosses the integer sampling lattice. On each interval the tent weights are quadratic, so two-point Gauss–Legendre quadrature integrates them exactly. This avoids sparse repeated-image copies, fixed-step angular brightness bias, and endpoint translation. Axis-aligned and rotated lines have equal total mass; zero distance is exact identity. The first moment is zero to floating-point precision.

For reconstruction and prefiltering, see [PBRT, Image Reconstruction](https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Image_Reconstruction).

## Lens / Bokeh

Lens blur applies one uniform aperture across the image; it does not use a depth map. Radius is the aperture circumradius. Blade count `0` is a true circle; `3..12` is a regular polygon, with the first vertex pointing right at zero rotation. Positive rotation is clockwise.

Each tap is the area of intersection between its pixel square and the aperture. Circular intersection is evaluated analytically from the disk integral; polygon intersection uses convex clipping and a local-coordinate shoelace area. The resulting nonnegative taps are normalized. Small and fractional radii have deterministic area coverage.

For convolution, exactly equal adjacent weights become horizontal spans. Double-precision row prefix sums evaluate each span in constant time; partially covered boundary cells remain their genuine individual weights. The implementation holds one prefix row plus a 16-row output stripe, not an image-sized summed-area table. Typical circle/polygon work grows with boundary length rather than aperture area. This preserves the discrete aperture convolution exactly. Odd blade counts retain their actual shape and orientation.

Related aperture-filtering methods: [McGraw, Fast Bokeh Effects Using Low-Rank Linear Filters](https://web.ics.purdue.edu/~tmcgraw/papers/dof_mcgraw_2014.pdf) and [Moersch and Hamilton, FLAG](https://www2.cs.uregina.ca/~hamilton/projects/FLAG/index.html).

## Limits, cancellation, and reproducibility

Document parameters permit radius `0..256`, motion distance `0..512`, blade count `0` or `3..12`; cache density supports `(0,8]`. The UI can offer a smaller practical range. Requests preflight output, kernel/scratch memory and estimated sampling work. The default primitive budget is 512 MiB working memory excluding caller-owned input and 64 Gi sample operations. Callers may lower these budgets. Overflow/nonfinite coordinates and invalid parameters fail predictably.

Kernel creation checks cancellation by aperture row or motion interval; convolution checks at bounded input/output scanline cadence. Progress is monotonic. No cancelled or failed result contains a partially computed output. Work is synchronous within the primitive; callers own revision-keyed caching, scheduling/coalescing, and stale-result rejection. No idle kernel generation, texture upload, or timer is part of this core.

`imageeditor_spatial_filter_tests` covers independent numeric Gaussian integration, dense continuous motion sampling, independent scanline aperture-area integration, normalization/symmetry, fractional radii, scalar/RGBA reference convolution, 256-column/16-row seam checks, preserved/spread alpha, soft masks and holes, input-context versus write coverage, expansion, algorithm validation, no-ops, budgets, progress and cancellation. The comparison/benchmark helper uses shipping primitives, not a separate implementation.

Repeatable commands (from repository root):

```sh
cmake --build --preset release --target imageeditor_spatial_filter_tests imageeditor_spatial_filter_benchmark -j4
build/release/tests/imageeditor_spatial_filter_tests
build/release/tests/imageeditor_spatial_filter_benchmark --benchmark 3840 2160
build/release/tests/imageeditor_spatial_filter_benchmark --benchmark 5120 2880
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_spatial_filter_benchmark --sheet build/test-artifacts/spatial-filter-comparison.png
```

The comparison sheet shows translucent primitives, fine lines and text above the
impulse responses. Impulse alpha is multiplied by 128 for visibility. Benchmarks
report kernel/application timing and working memory separately from input memory.
