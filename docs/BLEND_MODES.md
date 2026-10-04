# Blend modes

Canonical pixels remain straight RGBA8 sRGB. Filtering and alpha composition
use linear-light premultiplied values; named RGB blend functions operate on
unassociated sRGB. Existing numeric IDs 0–19 and textual IDs are unchanged.
IDs 20–26 append Dissolve, Darker Color, Lighter Color, Vivid Light, Linear Light,
Pin Light, and Hard Mix. Projects using these operations declare
`blend-modes-v3`; effects and non-Normal adjustment operators use the same
capability. An older reader must reject it, not substitute Normal.

## RGB rules

`Cb` is backdrop and `Cs` is source, each normalized to [0,1]. Alpha never enters
the comparison metric.

| Mode | Blend function |
| --- | --- |
| Darker Color | Select the entire RGB triplet with lower `.30R + .59G + .11B`; ties retain the backdrop. |
| Lighter Color | Select the entire triplet with higher weighted lightness; ties retain the backdrop. |
| Vivid Light | Source 0 → 0, source 1 → 1, including opposing backdrop endpoints. Below .5: `1 − min(1,(1−Cb)/(2Cs))`; otherwise `min(1,Cb/(2(1−Cs)))`. |
| Linear Light | `clamp(Cb + 2Cs − 1, 0, 1)`. |
| Pin Light | Below .5: `min(Cb,2Cs)`; otherwise `max(Cb,2Cs−1)`. |
| Hard Mix | Per channel: 1 above `Cb+Cs=1`, 0 below; at the exact tie, 1 only when `Cb>.5`. |

Vivid Light tests denominators before dividing and uses no epsilon. Hard Mix
does not threshold a rounded Vivid Light result. Exact RGBA8 values represent
the rational codes `n/255`; complementary codes remain ties. Continuous float
operands use an error-compensated sum so a neighboring value is not rounded into
a tie. Canonical sample recovery is an exact equality check, not color snapping.
Whole-color comparisons likewise evaluate exact 8-bit weighted sums before
normalization, preserving rational ties such as red 11 versus blue 30.
Partial source alpha and opacity still produce intermediate final colors.

The whole-color metric follows the psd-tools RGB reference, not Adobe's loosely
worded combined-channel sum. The Hard Mix tie follows that reference's observed
ordinary-layer rule. These choices deliberately differ from a channel sum and
from Krita's generic/softer Hard Mix variants. Exact Photoshop-version behavior
still needs the manual fixtures; equivalent names alone are not evidence.

## Dissolve

Dissolve is a spatial coverage operation, not an RGB function. A layer owns a
32-bit `blendSeed`, created once and saved even when another mode is selected.
History, duplication and cross-document copying retain it. It is unrelated to
layer IDs, document instances, paths, worker order or frame time.

The appearance grid is integer **layer-local pixel cells**, initially the source
pixel frame. It moves, scales, rotates, flips and distorts with the layer.
Raster storage expansion/cache padding does not rebase it. Output consumers map
back to that frame; viewport origin, tiles, display DPI and allocation order are
not hash inputs. Explicit rasterization establishes the baked layer's new frame;
its external blend mode remains live. Merge bakes the selected isolated result
and creates a Normal layer, so it never dissolves twice.

The hash uses unsigned 32-bit wraparound, avalanche multipliers `0x7feb352d` and
`0x846ca68b`, and `hash(x XOR hash(y XOR seed))`. Negative cell indices convert
modulo 2^32. Coordinates are floored and bounded to the signed 32-bit float
conversion range. The top 24 bits are compared against `floor(p × 2^24)`;
zero always rejects, one always accepts. A single decision serves all RGB
channels. Raising opacity progressively admits cells without reseeding.

- Ordinary source: `p = source alpha × opacity`, after crop/mask coverage.
  Rejection leaves the backdrop untouched. Acceptance contributes the
  unassociated source color at full coverage, without probability dimming.
- Styles: each effect has a stable salted contribution identity; bevel highlight
  and shadow are independent. Interior styles change color while retaining base
  alpha. Exterior styles gate their own paint coverage against their actual
  backdrop. A Dissolve layer with styles evaluates the complete styled
  contribution and consumes its coverage once, retaining backdrop-dependent RGB
  blending. Emitted color is accumulated separately, avoiding subtraction of
  nearly equal finished composites.
- Clipping groups: upper contributions operate on the stack color; the shared
  base silhouette is restored once. A Dissolve base gates its opacity, leaving
  shared fractional base/mask coverage as a later restriction. It cannot turn a
  half-covered clipping silhouette into opaque specks.
- Adjustments: modes blend original and corrected colors, retaining input alpha.
  Dissolve chooses corrected versus original color with strength/mask probability,
  never erasing or increasing the backdrop's coverage. Bypassed/neutral
  corrections remain identity.

Native-resolution output resolves the pattern before ordinary export resizing;
Pixel Preview displays that resolved image. Interactive sampling uses the same
local field rather than an independent screen-space noise source.

## Exchange and consumers

CPU sampling, exports and Vulkan share the equations and integer hash. Mode
changes invalidate composition, not original image uploads, text/shape geometry
or bevel distance/normal data. The Vulkan parameter buffer includes the small
canonical RGBA8 transfer table used by the CPU; this enumerates the same sRGB
curve, without changing gamma or introducing a new runtime dependency.

PSD layer keys are `diss`, `dkCl`, `lgCl`, `vLit`, `lLit`, `pLit`, `hMix`.
Descriptor enums are independently mapped to `Dslv`, `darkerColor`,
`lighterColor`, `vividLight`, `linearLight`, `pinLight`, `hardMix`.
PSD has no portable representation of Vulkana's seed/grid. Native modes remain
editable; review offers existing consolidation/flatten choices for exact
appearance. Reduced PSD Fill is still a separate unsupported semantic, never
silently multiplied into layer opacity. No native Fill control is added here.
PDF's native text path uses its existing faithful raster fallback for these modes.

References: [Adobe descriptions](https://helpx.adobe.com/photoshop/desktop/repair-retouch/adjust-light-tone/blending-mode-descriptions.html),
[W3C composition](https://www.w3.org/TR/compositing-1/),
[psd-tools RGB equations](https://psd-tools.readthedocs.io/en/latest/_modules/psd_tools/composite/blend.html),
[Krita mode variants](https://docs.krita.org/en/reference_manual/blending_modes/mix.html),
[PSD format](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/).
