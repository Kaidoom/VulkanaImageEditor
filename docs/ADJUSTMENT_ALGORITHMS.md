# Adjustment algorithms — version 1

Adjustment equations live in `core/detail/AdjustmentMath.inc`, shared by the
CPU evaluator and Vulkan. Settings are defined in `Adjustments.hpp`;
`AdjustmentCoreTests.cpp` checks them against an independent double-precision
reference.

## Evaluation and storage contract

One fixed ordered collection is attached to each renderable leaf: Exposure,
Brightness/Contrast, Levels, Curves, Hue/Saturation, Vibrance, Color Balance,
Warmth/Tint, Black & White, Invert. Stable string identifiers and algorithm
version `1` are persisted. Page navigation never changes this order. Containers
remain pass-through; their leaf children retain independent collections.

Raster RGBA8, editable text runs and shape geometry remain untouched. Source
texels are decoded and associated **before** alpha-aware bilinear filtering.
The filtered sample is then unassociated, adjusted using float intermediates,
reassociated and passed to the existing opacity/blend compositor. Adjustments
are not baked into source mipmaps, and no RGBA8 quantization occurs between
stages. A resolution change can regenerate an existing typed source cache for
the same reasons as before; changing adjustments is not such a reason.

Each stage starts/ends with unassociated **linear-light RGB**, bounded to
`[0,1]`. Some stages deliberately encode to sRGB internally, as detailed below.
Each stage clips its output to the SDR range. For example, +20 stops followed by -20 stops cannot recover the
display-clipped intermediate, but the original layer remains intact and is
recovered by disabling/resetting those stages. Alpha remains unchanged.

At alpha zero, a non-neutral evaluation returns transparent black without
interpreting hidden RGB. Exact associated white is unassociated to exactly one,
not a potentially rounded `alpha / alpha`. Whole-layer masks assign the stage
result directly; fractional masks mix **linear RGB** once. Finite input samples
remain finite, including exact endpoints. Invalid parameter values are rejected
before publication/loading rather than silently reinterpreted. Defensive CPU
handling of nonfinite color channels maps them to zero; ordinary stored RGBA8
and source-over output cannot contain them.

Disabled and neutral stages compile out. A fully neutral collection returns the
original filtered sample **bit-for-bit**, without color-space round trips or
mask sampling. Disabled states and unchanged values still retain editable data.
All stages default disabled. Reset Current restores its default parameters,
removes its mask and disables it. Reset All restores the pristine unadjusted
collection. Black & White and Invert require explicit enabling.

## Operations

### Exposure

`outLinear = clamp(inLinear × 2^stops, 0, 1)`. Stops range `[-20,+20]`; zero
is exact identity. This is linear-light exposure, not brightness or gamma.
The scalar multiplier is compiled once when parameters change.

### Brightness / Contrast

Both controls range `[-1,+1]`. In **encoded sRGB**:

`out = clamp((in - 0.5) × 2^(4 × contrast) + 0.5 + brightness, 0, 1)`.

The contrast pivot is 50% encoded gray; contrast spans gain `1/16` to `16`.
Brightness is an additive encoded-channel offset, deliberately independent of
contrast. Both zero is exact identity. The final result is decoded to linear.

### Levels

Composite RGB is applied first, then the corresponding individual R/G/B
channel, all in **encoded sRGB**. Every channel owns input black `b`, input
white `w`, gamma `g`, output black `ob`, and output white `ow`:

`t = clamp((input - b)/(w-b), 0, 1)`;
`output = ob + (ow-ob) × t^(1/g)`.

Input/output endpoints range `[0,1]`, gamma `[0.1,10]`. Input white must be at
least input black. Coincident input endpoints are supported as an exact step:
input `<= b` produces `ob`, input `> b` produces `ow`. Reversed input intervals
are rejected. Reversed **output** endpoints are valid and intentionally invert
the range. No epsilon shifts an endpoint or the threshold. The neutral channel
is `(0,1,1,0,1)` in field order black/gamma/white/output-black/output-white.

### Curves

Composite RGB followed by R/G/B, in **encoded sRGB**. Each channel stores 2–16
control points with inputs/outputs in `[0,1]`. Inputs must be increasing, with a
minimum separation `0.000001` to keep the float representation well resolved.
Duplicate/too-close inputs are rejected; editing UI must prevent crossing or
coalesce a proposed duplicate rather than create an ambiguous curve. Endpoints
are editable; outside their input interval, the nearest endpoint output is
held constant. Removing/moving a point does not change other stored points.

Interpolation is PCHIP: local monotone cubic Hermite interpolation, with
Fritsch–Butland weighted harmonic interior slopes and limited one-sided end
slopes. A local extremum has zero derivative. Thus an intentional rise/fall
stays a rise/fall rather than being globally sorted or forced increasing.
Derivatives are compiled in double and sent as float point/tangent records to
both evaluators. Preview and output evaluate each interval analytically. A final
segment-range clamp suppresses floating-point roundoff. Identity collinear points covering `[0,1]` compile out.

References: [SciPy PchipInterpolator equations and bibliography](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html),
[Fritsch and Butland, 1984](https://doi.org/10.1137/0905021),
[Moler, Numerical Computing with MATLAB, interpolation chapter](https://www.mathworks.com/content/dam/mathworks/mathworks-dot-com/moler/interp.pdf).

### Hue / Saturation

Use standard HSL derived from **encoded sRGB**, not linear RGB. Hue wraps over
360° and lightness is `(max+min)/2`. The stable saturation denominator is
`max+min` below the midpoint and `(1-max)+(1-min)` above it, avoiding a rounded
near-white average becoming a zero divisor. Exact gray has saturation zero and
conventional hue zero; hue rotation alone cannot colorize it.

There are Master, Red, Yellow, Green, Cyan, Blue and Magenta control triplets.
Hue offsets range ±180°, saturation/lightness offsets ±1. The six range centers
are `0,60,120,180,240,300` degrees. For circular distance `d` to a center, range
weight is `1-smoothstep(clamp(d/60,0,1))`; adjacent weights sum to one. Named
range weights also receive `smoothstep(clamp(chroma × 255,0,1))`, so a barely
colored gray does not abruptly acquire one named hue's lightness correction.
This one-code-value chroma confidence ramp applies only to named ranges, not
Master.

Weighted offsets are added to Master once, using the original stage hue.
Hue is rotated by the resulting offset. Saturation is multiplied by
`1 + totalSaturation`, then clamped. Total lightness offset is clamped to ±1:
negative values multiply lightness by `1+offset`; positive values mix lightness
toward one by that amount. Colorize replaces the resulting hue/saturation with
explicit absolute hue `[0,360]` and saturation `[0,1]`, retaining the resulting
lightness. Unused Colorize values do not defeat a neutral stage.

Reference for the color model and conversion: [W3C CSS Color 4, HSL](https://www.w3.org/TR/css-color-4/#the-hsl-notation).

### Vibrance

In encoded-sRGB HSL, preserve hue and lightness and apply
`S' = clamp(S + amount × S × (1-S), 0, 1)`, with amount in `[-1,+1]`.
Positive amount increases muted saturation proportionally more than already
saturated colors; a fully saturated color stays saturated, and exact gray stays
gray. Negative values reduce saturation with the same weighting; -1 does not
fully desaturate. Use Hue/Saturation for uniform saturation changes.

### Color Balance

Three encoded-channel offset triplets (Cyan/Red, Magenta/Green, Yellow/Blue),
each `[-1,+1]`, target Shadows/Midtones/Highlights. Compute linear luminance
`Y = .2126 R + .7152 G + .0722 B`, then `q = sRGBEncode(Y)`. Smooth Bernstein
weights are `(1-q)²`, `2q(1-q)`, `q²`; they sum to one with no tonal seams.
Each encoded output channel adds half the corresponding weighted controls,
then clips. The result is decoded back to linear.

Preserve Luminosity (default on) means preserving that **linear Rec.709 Y**,
not HSL lightness or the older nonseparable-blend luma coefficients. First add
the difference between original/new Y to all three linear channels. If this
leaves the RGB cube, contract chroma toward the target neutral gray just enough
to fit. This preserves target Y while keeping channels bounded, including black
and white. Zero triplets are neutral regardless of this option's stored state.

### Warmth / Tint

Relative channel-gain correction in **linear sRGB**.
Warmth `w` and magenta tint `t` both range `[-1,+1]`. Channel gains are:

`R = 2^(.5w + .25t)`; `G = 2^(-.5t)`; `B = 2^(-.5w + .25t)`.

Multiply each linear channel by its gain and clip. Positive warmth increases
red relative to blue; positive tint increases red/blue relative to green.
Both zero is exact identity. This intentionally can change luminance.

### Black & White

Default conversion is **linear Rec.709 luminance**. Six source-hue controls
range `[-1,+1]`, using the same smooth hue partition as above. They are encoded
gray offsets, weighted by source chroma (so neutral gray is unchanged):

`grayEncoded = clamp(encode(Y) + chroma × sum(rangeWeight × contribution),0,1)`.

Zero contributions are a predictable neutral luminance conversion rather than
six redundant RGB multipliers. Positive values brighten source colors in that
range; negative values darken them. Decode the gray to linear. Optional tint
owns an independent color and strength `[0,1]`; tint alpha is intentionally
ignored because no color adjustment changes source alpha. Project the decoded
tint to the gray's luminance with the same gamut-safe Preserve Luminosity rule,
then mix gray/tint by strength in linear light. Defaults: disabled, all range
offsets zero, tint strength zero.

### Invert

`out = decode(1 - encode(inLinear))`. This is photographic **encoded RGB**
inversion with unchanged alpha, not inversion of linear energy or alpha. Black
and white exchange exactly. Disabled by default.

## Masks and history

An explicit mask capture requires an active selection; a null selection is not
silently captured as unrestricted. The immutable R8 selection snapshot is
retained together with the capture-time local-to-document affine matrix. At
evaluation, a layer-local point is mapped through that frozen matrix into the
old selection coordinate plane and coverage is bilinearly sampled at pixel
centers with zero exterior. This preserves fractional coverage, holes and
disconnected regions. Later layer movement/rotation/flips carry the mask;
later text or shape bounds changes do not normalize or stretch it. Changing or
deselecting the document selection cannot mutate the captured snapshot.

For each stage independently, `result = original + coverage × (adjusted-original)`
in linear RGB. Coverage one assigns `adjusted` directly, preserving exact white
and black algebraic endpoints. Coverage zero skips the stage. An active empty
mask compiles that stage out but remains an editable/persisted empty mask.

`AdjustmentEditTransaction` pins LayerId, starting parameters, adjustment
revision and content-state token. Live writes touch only adjustment/document
revisions, not source raster or text/shape cache revisions. Commit adopts one
before/after command; returning to the starting state/cancellation preserves
redo. Removed/replaced targets and foreign history changes invalidate ownership,
so a stale callback cannot apply to a different row/layer. Owned cancellation
restores the exact starting immutable collection. The command uses existing
history saved-state tracking, not a separate undo stack.

History memory charges retained stack objects, curve-vector capacities and
owned mask memory. The same mask shared within a stack or a command's before/
after pair is counted once. Mask snapshots can share immutable tiles internally,
following the existing SelectionMask memory convention. Reset/duplicate copies
share only immutable storage; edits replace a layer's collection independently.

## Compiled ABI and histogram input

The initial bounded parameter block is ten records × 256 floats (10 KiB).
Record slots 0/1 hold active/type; parameters start at slot 8. Curves use four
49-float subrecords: count plus up to sixteen `(x,y,tangent)` triplets; count
zero is an exact per-channel identity sentinel (no Hermite-roundoff drift). All
unused storage is zeroed. No filesystem access, decoding, heap allocation or
curve reconstruction occurs per sample/dab. Mask resources are separate and
indexed by stable stage index. Shader/CPU loops use the same fixed order.

The evaluator accepts an exclusive `stopBefore` stage index. This exposes
authoritative input immediately before Levels/Curves without allowing a curve
to feed its own histogram. Histogram consumers should use alpha-weighted source
samples, ignore alpha zero, and key their cache by source plus **upstream**
parameters/masks; downstream or the current stage's changes are irrelevant.
