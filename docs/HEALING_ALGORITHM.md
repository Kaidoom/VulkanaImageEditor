# Cloning Heal

## Method

Heal transfers the sampled source image's texture and reconstructs a spatially
varying illumination correction from the unpainted destination surroundings.
It operates on the accumulated stroke footprint. A patch contains corresponding
source samples, coherent pre-stroke destination context, and the final footprint
including selection holes. It does not repeatedly blur or recolor individual
dabs.

The starting point is the correction-field formulation in Pérez, Gangnet, and
Blake, [*Poisson Image Editing*, SIGGRAPH 2003, equation 5](https://www.cs.jhu.edu/~misha/ReadingSeminar/Papers/Perez03.pdf).
Their source-gradient reconstruction can be written as source plus a harmonic
interpolation of the boundary mismatch. That preserves source detail while
changing its tone. Their discussion of nearby objects also identifies boundary
color bleeding as a failure mode of ordinary Poisson cloning.

The screened formulation is motivated by Bhat, Curless, Cohen, and Zitnick,
[*Fourier Analysis of the 2D Screened Poisson Equation for Gradient Domain Problems*, ECCV 2008](https://grail.cs.washington.edu/projects/screenedPoissonEq/).
A value-fidelity term combined with gradient fidelity gives a screened Poisson
system. The implementation here uses an irregular spatial graph rather than
their Fourier solution. The robust boundary model, parameter values, memory
policy, and alpha integration below are this implementation's choices.

## Domain and illumination model

The array grid consists of destination layer texels. The caller maps each texel
center through the destination transform into document coordinates, adds the
document-space clone offset, and samples that corresponding source position.
Tip rotation affects coverage only. The operator does not perform a second
geometric mapping.

1. Any positive finite coverage with reliable source alpha seeds the repair
   domain. Coverage magnitude is deliberately ignored during reconstruction.
2. A six-texel four-connected dilation adds solver support outside the write
   footprint. Invalid or transparent source samples interrupt support. The caller
   should supply at least a six-texel context halo where available.
3. Each connected component of the expanded support gets an independent model.
   Nearby disconnected footprints whose support overlaps share a model; distant
   islands do not. Holes with zero coverage can supply context and are never
   proposed as writes.
4. Only uncovered support pixels with reliable source and destination alpha
   supply destination color data. Destination pixels inside even a very soft
   painted edge never provide defect gradients or tone observations.

Let `s` and `d` be straight **linear-light RGB**, obtained by safely unpremultiplying
the supplied premultiplied values. At context samples, fit the RGB mismatch
`d - s` with an affine plane per channel:

```text
p_c(x,y) = b_c + u_c x + v_c y
```

Coordinates are centered and scaled per component. A median initialization and
eight Tukey iteratively reweighted least-squares steps reject large boundary
outliers jointly across RGB. Confidence includes `min(source alpha, destination
alpha)`. The Tukey cutoff is `max(0.025, 4.685 * 1.4826 * median RGB-RMS residual)`.
The 0.025 floor is in straight linear RGB. A tiny slope-only ridge handles one-row
and one-column domains. At most 8,192 regularly spaced context samples enter the
fit; all context pixels participate in the subsequent reconstruction.

This model transfers different brightness and color casts and also follows a
spatial lighting gradient. The residual reconstruction below handles non-affine
illumination.

## Screened reconstruction and boundary conditions

Write the output RGB as

```text
f_i = s_i + adaptation * (p_i + q_i)
```

For each RGB channel, solve the strictly convex quadratic energy on the support
graph:

```text
E(q) = sum_(i,j in support edges) (q_i - q_j)^2
     + lambda * sum_(i in support) q_i^2
     + sum_(i in uncovered valid context) w_i * (q_i - (d_i - s_i - p_i))^2

lambda = 1/256
w_i = 4 * TukeyWeight(RGB residual) * min(source alpha, destination alpha)
```

The resulting sparse linear system is

```text
(degree_i + lambda + w_i) q_i - sum_(neighbor j) q_j
    = w_i * (d_i - s_i - p_i)
```

Context observations are soft boundary constraints, with zero weight for rejected
outliers. Missing neighbors at image edges, source transparency, and the outer
support boundary impose a zero outward residual flux. There is no wraparound or
border-pixel replication. The screening term gives every unknown a positive
diagonal contribution, including isolated nodes. Its nominal residual diffusion
length is 16 target texels; the affine plane extends across the whole component.

Because the algorithm adds a smooth correction to the original source samples,
it retains their high-frequency texture. It never estimates texture by blurring
the already painted result. Destination detail inside the defect does not enter
the energy, so a scratch is not retained just because its gradient is stronger
than the source. Exterior object colors that disagree sharply with the robust
model are rejected rather than propagated as mandatory boundary values.

`adaptation` is the actual fraction of the reconstructed illumination correction:
zero is exactly Stamp, one requests full adaptation, and the default is 0.8.
The parameter does not change brush hardness, mask feathering, or opacity.

## Working space and alpha policy

The public arrays and output use the existing `PremultipliedColor` type:
premultiplied linear RGB plus linear alpha. The RGB solve uses visible straight
linear colors; output is clipped to the representable `[0,1]` straight RGB gamut
and repremultiplied by **the original source alpha**. There is no intermediate
8-bit encoding and no alpha reconstruction that could make a patch opaque.

Zero-alpha source samples become transparent black, regardless of hidden RGB.
Zero-alpha destination samples are not context. Non-finite samples cannot provide
context. Alpha at or below `1e-6` is too small for reliable unpremultiplication;
positive source alpha that small is still preserved and passed through without
adaptation. Ordinary partially transparent source and context pixels are
supported; their alpha is not discarded or applied as a second brush mask.

The returned pixel array contains unmasked source colors with repaired RGB on
the footprint. The caller applies its accumulated brush coverage, flow/opacity
ceiling, selection and source-over composition exactly once. Outside the
footprint, the array contains source pass-through values and does not authorize
any write. The solver support must never be substituted for the write mask.

For an empty Normal retouch layer, `destination` must contain the appropriate
underlying visible content, not the empty target's transparent black. The output
still carries source alpha and can be composited on that empty layer normally.
Translucent source or partial final coverage leaves some original destination
visible.

The caller chooses raw versus rendered references consistently with the clone
workflow. The solver does not invert adjustments or blend modes. Writing rendered
appearance into a layer with additional effects can alter that appearance again.
The operator is not aware of live documents, QObject objects, layer revisions,
transactions, or undo; those remain the caller's responsibility.

## Quality, bounds, and cancellation

Each channel uses Jacobi-preconditioned conjugate gradients, with float vectors
and double reductions. The default maximum is 400 iterations, clamped to an
absolute maximum of 1,000. Relative tolerance defaults to `1e-4`, with a permitted
range from `1e-7` to `1e-2`. Convergence requires the actual final matrix residual
to satisfy

```text
||Aq - b||^2 <= max(tolerance^2 * ||b||^2, support_pixel_count * 1e-16)
```

The second term is a `1e-8` absolute RMS floor for nearly zero residual fields.
The final residual is recomputed from the matrix rather than trusting only the
recursive CG residual. Iteration count, maximum relative residual, and actual
absolute residual RMS are returned as diagnostics. When a fitted affine plane
already explains the mismatch to float precision, the remaining right-hand side
can be nearly zero: its relative residual ratio can then be large even though
the absolute RMS meets the `1e-8` floor. Report the absolute RMS alongside the
ratio; the ratio alone does not determine convergence. A failed channel discards every channel's repair for its entire
component, so a mixed partially solved RGB result is never published.

The default patch budget is 1,048,576 pixels, including halo and unused rectangle
pixels. The absolute ceiling is 4,194,304. The conservative operator-owned scratch
estimate is `64 * patch_pixels + 524288` bytes and includes its output, sparse-grid
indices, reused component queue, solver vectors, and fit samples; caller-owned
input arrays are additional. There is no full-document allocation inside the
operator, unbounded queue, background thread, or task backlog. Components are
processed sequentially, and their channel buffers are reused.

Cancellation is checked before allocation, every 16,384 elements in grid,
initialization, CG-vector, residual-check and output passes, between fit steps,
between channels, and on every CG iteration. Cancelled results
have an empty pixel array and must not be published. The invocation owns only
its arrays and scratch data; the callback runs on the invoking thread.

At least three accepted surrounding samples are required per component. Missing
context or nonconvergence produces `StampFallback` or `PartialFallback` with
explicit component/pixel counts and a human-readable diagnostic message. A budget
or allocation failure returns `LimitExceeded` with no result pixels. The UI must
show this condition and use its documented fallback or cancel the operation;
these cases must not be described as successful healing.

## Tests

`tests/HealingTests.cpp` checks independent analytical lighting fields, transferred
source-detail Laplacians, distinct source/destination texture, destination-defect
invariance, strong exterior edges, disconnected coverage, selection holes,
image-edge and single-row domains, translucent context/source output, retouch
composition alpha, zero adaptation, absent context, memory refusal, cancellation,
and forced nonconvergence. It also verifies that changing soft mask magnitudes
does not apply coverage inside the solver.


Generate a comparison image or benchmark the solver:

```sh
build/release/tests/imageeditor_healing_tests --comparison build/healing-comparison.ppm
build/release/tests/imageeditor_healing_tests --benchmark
```

The image compares Source, Damaged destination, Hard Stamp, Feather-only Stamp
and Soft Heal on lighting-mismatch and adjacent-object fixtures.

## Practical limits

Whole-frame 4K/5K solves exceed the default patch budget. Large repairs use the
application's cancellable cooperative or background finalization path.

The robust context fit protects against minority outliers; it cannot identify
objects semantically. If a different surface occupies most of the context, the
model can fit that surface. A clean source and footprint contained within the
intended surface remain necessary. Occluded or sharply discontinuous illumination
cannot always be inferred from a surrounding ring. Screening attenuates
non-affine lighting influence far from the boundary, and gamut clipping can lose
texture in severely mismatched highlights/shadows. Very large curved-lighting
repairs may therefore benefit from several appropriately placed strokes.

The PDE metric uses destination layer texels. The caller handles geometric
sampling; nonuniform scaling or shear changes the apparent document-space
diffusion width.
