# Spot Heal

## Overview

`SpotHealRepair.hpp/.cpp` is a platform-neutral synchronous repair backend. It
owns no document, mutable surface, Qt object, UI event loop or history. It
borrows a bounded reusable CPU executor for independently schedulable work;
every submitted task is joined before the synchronous call returns. The caller
pins the reference, constructs masks on a documented pixel grid, schedules the
outer operation off the UI thread, validates revisions and publishes once
through the raster transaction. Stamp and sampled Heal retain their existing
source-anchor path.

Spot Heal first finds replacement texture, then optionally uses the existing
Heal operator for exterior-tone adaptation. `CloneReference` captures immutable
intrinsic pixels for same-layer raw repair, or the rendered hierarchy for retouch
references, with the existing crop/effect sampling policy.

## Algorithm references

- [Barnes et al., PatchMatch, SIGGRAPH 2009](https://gfx.cs.princeton.edu/pubs/Barnes_2009_PAR/):
  alternating directional propagation and seeded global-to-local random proposals
  for correspondence search.
- [Newson et al., Non-Local Patch-Based Image Inpainting, IPOL 2017](https://www.ipol.im/pub/art/2017/189/):
  conservative donor-patch validity, coarse-to-fine completion, boundary-first
  initialization and exemplar selection that retains texture.
- [Criminisi, Pérez and Toyama, IEEE TIP 2004](https://www.microsoft.com/en-us/research/publication/region-filling-and-object-removal-by-exemplar-based-inpainting/):
  coherent exemplars and ordering trusted context for structure and texture.
  Vulkana uses boundary-distance initialization rather than the paper's full
  confidence/isophote priority scheme.

## Input, alpha and the three masks

All input/output colors are floating-point **premultiplied linear RGBA**.
Stored raster surfaces remain straight sRGB RGBA8. Matching uses unassociated
linear RGB with alpha-aware weights; hidden RGB under zero alpha is never
appearance evidence. Quantization and soft blending belong to the consumer and
happen once after reconstruction.

Spot Heal opts into the shared brush's selection-resolved final publication:
brush influence, opacity and pinned fractional selection are multiplied before
linear color composition and RGBA8 encoding. Its transaction accepts that
already-resolved candidate without another fractional interpolation, while
still enforcing zero selection, document bounds and crop. Ordinary Brush,
Stamp and sampled Heal retain their established write path. Exact independent
double-precision publication tests cover every alpha/selection byte and varied
colors, including partial-opacity retouch and transformed destinations.

The input is an immutable contiguous grid and two binary masks:

1. `unknown`: every marked damaged pixel, including parts outside soft write
   coverage, selection or crop. Any nonzero value excludes original pixels from
   all matching and donor use for the entire operation.
2. `valid`: geometric/sampling-context validity. The consumer must already
   exclude samples whose interpolation footprint touches marked damage. An
   omitted span means geometrically valid everywhere, not alpha-opaque.
3. Final **soft write coverage** is intentionally not an algorithm input. The
   consumer combines brush hardness, opacity/strength, selection and clipping
   once against pre-stroke destination pixels. It must not redefine `unknown`
   using that coverage.

Direct same-layer evaluation on integer raster-local texel centers avoids
resampling native texture and bypasses displayed effects that would otherwise
be baked twice. A rendered-reference consumer may use a document-pixel grid and
map the candidate back to its supported retouch destination. These coordinate
choices must not depend on viewport zoom. The backend itself never rescans live
layers, checkerboard, preview overrides or overlays.

The unknown branch does not inspect original color **or alpha**, even for NaNs.
Candidate invariance is tested by replacing every unknown value with entirely
different RGBA. Direct publishing separately preserves destination alpha by
default; a clean retouch layer needs an explicit nonzero-coverage policy, not
preservation of its empty alpha. The backend does not invert translucent
source-over composition.

## Reconstruction

### Trusted pyramids and support

Known positive-alpha context alone supplies original appearance. A donor center
is eligible only when the **whole matching patch plus a one-pixel square
gradient halo** is trusted. An integral invalid-count grid makes the check
constant-time. Original unknown pixels never become original donors when the
current reconstruction acquires provisional values.

Pyramid pixels use complete 2×2 box support. If any source value is unknown or
invalid, the coarse cell is not trusted; partially available neighborhoods are
not renormalized into supposedly known data. The unknown mask uses union
reduction. Directional absolute-gradient texture evidence is carried from the
fine grid, with its own inherited validity, so downsampling does not make grain
indistinguishable from a smooth region. The coarsest useful level is selected
from the marked region's interior distance, with at most five levels. Levels
without valid donor patches are not used.

The finest matching radius is derived from the marked region's thickness:
interior distance plus one pixel, clamped to 3–7 pixels and image geometry.
This makes a scratch's patches span both banks rather than matching only
provisional interior pixels. Small/limited references may require smaller
valid support. Matching radius is independent of brush size.

### Initialization, search and synthesis

An eight-connected distance from trusted original context establishes a stable
outside-to-inside initialization. Partially known patches compare only available
context; provisional estimates have lower weight than original data. Finer
levels inherit valid coarse displacement proposals, but also search independent
starts to escape a wrong coarse match.

The patch objective combines spatial RGB correspondence, alpha compatibility
and directional texture disagreement. A weak continuous logarithmic distance
penalty prefers nearby equivalent donors without forbidding a better distant
one. Search covers the supplied coherent reference domain using deterministic
strata, restarts, directional offset propagation and exponentially shrinking
random-search windows.

Seven correspondence/reconstruction iterations refine each used level. Coarse
iterations can use weighted votes to stabilize structure. At source resolution
every reconstructed pixel comes from a valid exemplar assignment; incompatible
texture phases are not averaged into a smooth final smear. Translated neighbor
assignments are revalidated for full support before choosing a pixel, and the
final diagnostics identify the valid donor center.

### Smooth-context model and tone adaptation

Exemplar offset ambiguity can add small variations to a truly smooth lighting
gradient. Each connected repair component therefore independently tests a
linear spatial color model against **trusted exterior context only**. It must
have at least 16 samples, unexplained RGB residual RMS no greater than 0.0015
in normalized linear channels, no residual exceeding 0.006, and explain at
least 99% of context variation. Exact decoded sRGB8 samples receive a bounded
quantization-variance allowance: their actual local code-bin step squared over
12, with a 25% finite-sample margin. Arbitrary float samples receive no such
allowance. This prevents a genuine linear ramp from failing solely because
authoritative RGBA8 storage quantizes it. Fine grain, pores, real boundaries and non-affine texture
reject that model and retain patch reconstruction. Diagnostics report
`smoothModelPixels`.

For the remaining exemplar output, surrounding candidate pixels are generated
from the same valid correspondence field. Existing Heal fits illumination
differences only outside the complete unknown mask; destination unknown values
are explicitly zeroed. Its screened correction is optional (`adaptation=0`
provides the texture candidate directly), and failure retains the candidate
rather than presenting a blur as a successful repair. Smooth-model pixels are
not adjusted again. `beforeAdaptation` allows the two stages to be compared.

## Work limits, determinism and diagnostics

The solver is synchronous inside its caller-owned worker. Cancellation is
checked through reference preparation, search, reconstruction and the existing
Heal solve; failed/cancelled results contain no proposed pixel array. The work
budget counts tested patch correspondences. Default limits are 16 megapixels
of supplied input, 768 MiB estimated working memory and 256 million patch
comparisons. Conservative base memory is 256 bytes per input pixel, or 288
when diagnostic maps are requested, plus explicitly counted mandatory fixed
search scratch, including the first target-descriptor workspace. Optional
immutable color caches, extra worker contexts and parallel schedules are
admitted only within the remaining budget. A cache that does not fit uses the
same uncached arithmetic; parallel work that does not fit runs serially. Neither
fallback reduces the donor domain. Inputs without unknown pixels need no search
workspace. Memory normally bounds the usable ROI before the dimension limit;
caller-owned immutable snapshots are additional and budgeted by the integration.

The backend searches its entire supplied context; the consumer owns automatic
ROI/context acquisition and may expand/retry within its snapshot budget when
context is insufficient. Reconstruction retains full pixel resolution.

SplitMix64, integer coordinates, stable scan ordering and explicit tie-breaking
make finalized masks/references/seeds independent of UI event timing. Unknown
pixels may influence geometry and processing order, never original appearance.
Diagnostic capture optionally returns original-donor assignments, forbidden
donor centers and the pre-adaptation candidate, plus support/level/iteration,
work and conservative memory counters. These are development tools, not extra
end-user controls.

## Limits and tests

Spot Heal reconstructs from nearby visible texture. Strong perspective,
nonrepeating structures, few valid donors and large ambiguous holes are difficult.
Evaluate boundary continuity and retained detail alongside pixel error.

`SpotHealRepairTests.cpp` covers corruption/alpha invariance, whole donor and
descriptor validity, deterministic output, exact known-pixel preservation,
partial alpha, true-edge context, multiscale behavior, the validated smooth
model versus fine texture, budget failures and cancellation. Integration tests
must additionally cover selection/crop writes, target identity/revisions,
retouch alpha, exact undo/redo, export and installed-package operation.
