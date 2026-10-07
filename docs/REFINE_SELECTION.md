# Refine Selection

Select → **Refine Selection…** improves an existing nonempty selection. A mask
thumbnail's **Refine Mask…** opens the same temporary workspace, independently
of the document selection. Both actions are configurable in Shortcuts, with no
default binding. Even coverage below the marching-ants threshold is eligible.

The workspace temporarily hides docked panels and floating panel groups without
moving the canvas. Hidden panels have no input footprint. Tools and rulers remain
visible; the refinement card sits inside their available area. Apply/Cancel
restores the original panel visibility, tab groups, current tabs and placements.
This is transient workspace state, not a saved-layout or document-tab preference.
Inspect
Overlay, Black, White, Transparency or raw Grayscale Mask; hold Original to
compare. These display choices do not edit paint colors or document content.
Output and Apply/Cancel remain visible while the controls scroll.

## Processing contract

The platform-neutral service starts from immutable R8 input, settings and
distance-sampled brush strokes. It evaluates in this order:

1. **Image-guided refinement.** Radius (0–250 px) defines a document-pixel band around
   coverage boundaries. Refine Edge strokes union additional uncertain regions;
   they do not paint foreground. A zero Radius still permits brushed regions.
2. **Smooth.** Relaxed 3×3 median filtering reduces boundary irregularities,
   with fractional passes. This is a rank operation, not Gaussian feathering.
   Larger amounts can remove small details.
3. **Shift Edge.** Scalar maximum/minimum morphology over Euclidean lattice disks
   expands/contracts coverage. Fractional radii interpolate neighboring disk
   results, without scaling object bounds or making square-kernel corners.
4. **Feather.** The shared pixel-integrated Gaussian uses sigma = radius / 3 and
   finite support ceil(radius), in document pixels.
5. **Contrast.** Coverage becomes `a^p / (a^p + (1-a)^p)`, where
   `p = 1 + 15 × contrast` and contrast is normalized to [0,1]. Zero is identity;
   endpoints stay fixed and the maximum is finite.
6. **Manual corrections.** Add/Subtract strokes mix toward white/black last,
   so automatic/global settings cannot overwrite deliberate corrections.

Intermediate coverage is floating point; final publication quantizes to R8.
Each stroke unions its dabs rather than repeatedly depositing opacity at the
same point. Edge-region repainting is idempotent; separate Add/Subtract strokes
may intentionally build strength. The existing brush engine owns spacing,
pressure and document-coordinate input; refinement uses a round tip, unit Flow
(pressure still modulates it), and keeps its controls local to the session.
The configured brush-size shortcuts (`[` / `]` by default) resize that local
brush in all three modes, without changing the editor's normal brush. Numeric
entry retains keyboard ownership; merely focusing a slider does not block them.

Selection processing extends edge samples at the canvas boundary, rather than
inventing a black border. Constant all-zero/all-one input therefore remains
constant under global operations. Mask sessions include the stored mask's
off-canvas extent, canvas and 378 document pixels of analysis/filter context
(250 px Radius plus the existing 64 px Shift Edge and 64 px Feather limits),
sampling the mask's explicit outside coverage. Quantized no-ops reuse the
original mask/selection; unchanged tiles are shared.

## Image guidance

This is a native local color-pair sampling matte with confidence-weighted cleanup,
not a flood fill, neural model or foreground-color recovery. Primary samples lie
outside the uncertain region: coverage ≥0.98 for foreground and ≤0.02 for
background. Intermediate gray is an estimate, not an automatic hard label.
Image borders are not assumed to be background.

A wide Radius or Refine Edge stroke must not erase all foreground evidence.
For each 8-connected definite-foreground component entirely covered by the
analysis region, additional appearance samples come from its inner half-depth
core (Euclidean distance to coverage below 0.98). Its deepest valid sample is
always retained, even for a tiny island. These are color proposals, **not fixed
alpha constraints**: the solver can still correct those pixels. Components with
trusted foreground outside the region keep the ordinary trimap sampling path.

Spatial k-d trees supply four nearby samples of each class. Pairs fit
`I = aF + (1-a)B` in linear RGB; reconstruction error, spatial distance and a small
input prior choose a candidate. If the spatial fit is not already accurate,
color/spatial trees add eight candidates per class, within the same bounded
sampling context. Uniform-color sets use the cheaper spatial path. For compact
foreground color clusters (summed channel variance below 0.01), nearby matching
colors are excluded from background training (squared RGB distance below 0.01).
This prevents an incomplete selection from teaching the solver that more of the
same foreground is background. Broad/multicolor sets do not use that exclusion.

Pairs with squared color separation below 0.0025 are inconclusive. Fit confidence
is `exp(-normalized residual / 0.02)`, with residual ≤0.0001 treated as reliable.
Reliable fits stay unchanged. Others combine nearby proposals in a 5×5 working-
pixel neighborhood, weighted by confidence, spatial distance and reference-color
similarity. This removes the former abrupt residual cutoff and speckled retained
pixels without globally feathering the mask. Insufficient aggregate evidence
still retains the input locally. Transparent source pixels provide no evidence.

Only uncertain pixels are modified. An explicitly broad brush region expands the
sampling context without changing trustworthy coverage outside that region.
Duplicate region strokes remain idempotent; cleanup is evaluated once from the
immutable fit, not accumulated across previews. There is no dense image-wide
linear system.

The method needs trustworthy samples on both sides. Closely matching colors,
complex textures, tiny isolated features or a badly misplaced starting selection
can remain imperfect. Refine Edge can expose gaps hidden inside a broad initial
selection; Add/Subtract handles areas without useful color evidence. Existing
source transparency is **not** copied into the coverage mask. Samples below
250/255 source alpha are conservatively left unresolved. Global/manual operations
remain available without a useful image reference.

Research references: [PyMatting's algorithm overview](https://pymatting.github.io/alpha.html)
and [closed-form natural image matting](https://doi.org/10.1109/TPAMI.2007.1177).
The implementation is original C++; no reference code, Python runtime, model or
new dependency is incorporated.

## Reference, preview and native masks

Merged Visible uses the canonical compositor. Active Layer evaluates the target
in isolation, without final layer visibility/opacity/blend, retaining its content,
transforms, crop and processing. Adjustment layers use Merged Visible because
they have no independent source image. Refine Mask bypasses **only** its target
bitmap mask in an owned reference snapshot, never in the actual document.
Other masks and composition remain intact; temporary selections never clip
reference context. Fully transparent RGB is not color evidence.

Selection inspection uses the captured reference and proposed coverage. Mask
inspection replaces the target mask in a transient canonical render snapshot,
including normal styles, clipping and adjustment-layer semantics. Grayscale
shows raw working coverage. No second compositor or permanent live effect is
introduced. Existing bitmap masks currently contain coverage, mapping, outside
value and enablement; refinement retains those supported properties.

Mask processing uses a document-aligned grid at least as dense as its native
grid, bounded using the mapping Jacobian. Modified coverage **deltas** are mapped
back once into the original native frame. This avoids resampling unchanged stored
coverage. Required storage extension changes the mapping offset in compensation,
not the document position. Unchanged sessions retain the original object exactly.
Severely minified or near-horizon projective masks can exceed the explicit limits
and are rejected rather than silently downsampled.

RGB, source alpha, text and shape data remain unchanged. Background-colored
fringes already present in mixed source pixels can remain: alpha accuracy and
edge-color contamination are separate concerns.

## Output, history and lifetime

- **Selection** replaces temporary selection coverage without dirtying persistent
  image content. Refine Mask only changes the temporary selection if this output
  is explicitly chosen.
- **Layer Mask / Replace Layer Mask** targets the pinned compatible layer,
  preserving typed content. It selects the mask thumbnail and retains the
  temporary document selection. Undo restores the previous thumbnail target too.
- **New Layer with Mask** duplicates the actual native target immediately above
  it, with independent content/mask identity and the same structural membership.
  The original stays visible, as stated in the output control; a clipping base
  is never silently hidden. The preview shows this combined result.

Completed parameter gestures and strokes have workspace-local Undo/Redo. Reset
is undoable; Escape cancels the active gesture/stroke, otherwise the workspace.
Apply publishes one normal atomic operation. Cancel/no-op leaves ordinary history,
redo and saved checkpoints intact. Each document tab owns its unfinished session.
Switching tabs pauses refinement, restores ordinary panels and controls, and
resumes that tab's settings, brush edits and local history on return. Other tabs
can be edited, saved, closed or refined independently. The active refinement
tab gates editing menus and shortcuts, not File or Help. New/Open (including
their shortcuts) suspend refinement; cancelling the dialog returns to it unchanged.
Navigation, saving and closing remain usable.
Save or another operation requiring completed content asks before discarding
that tab's refinement. Closing a refining tab asks for that tab only; quitting
asks about all unfinished sessions, then follows the normal unsaved-document
prompts. Declining keeps the sessions. No provisional mask is saved or exported.

One asynchronous coordinator and the existing bounded executor process immutable
inputs. Runtime document identity, target/selection revisions and generations
guard publication. Requests coalesce, cancelled/stale results are discarded, and
Cancel releases the workspace immediately while an owned, cooperatively cancelled
worker drains without publishing. Suspended tabs stop scheduling work; returning
reuses completed results or restarts the latest pending request. Final destruction
joins retired workers before UI teardown. Pan/zoom and inspection choices
do not rerun the solver.

Reference pixels and typed sources are reused. Radius/Refine Edge changes rebuild
analysis; global/manual changes reuse valid guided coverage. The canvas uses the
existing compressed mask atlas and revisioned source textures, without reuploading
unchanged source RGBA. No inactive-document refinement is scheduled.

## Limits and verification

The working grid is limited to 24 Mi pixels, 32768 pixels per side and 16 samples
per document pixel on each axis, with a conservative 64 bytes/pixel solve budget
(1.5 GiB maximum).
Reference rendering, derived caches, mask GPU storage and duplication also retain
their existing subsystem limits. Offset work is preflighted; stroke history is
bounded to 4096 strokes / 262144 dabs. Requests fail explicitly, never at a secretly
reduced resolution. These bounds are additional to existing open-document memory.
Workspace Undo metadata has a 64 MiB retention budget; oldest local checkpoints
can expire, while Reset and whole-session Cancel always retain the original input.

Release tests cover identity/return-to-neutral, trusted coverage, local correction
precedence, independent color/alpha fixtures, event cadence, cancellation,
history/checkpoints, transformed masks and storage expansion, text/shape/adjustment
targets, clipping-base duplication, project reopen and CPU/Vulkan mask rendering.
Native workspace tests run on Wayland and XCB with Vulkan validation.

`imageeditor_refinement_quality_tests [output-directory] [owned-image]` generates
known-alpha geometry, text holes, strands, feathered edges, textured/ambiguous
backgrounds and border/island fixtures. It compares refinement against the initial
mask and Feather-only, retains limited-result cases, and renders black/white/color
inspection sheets. Ground truth stays in the test harness. Private images and
derived sheets are not installed or included in public source bundles.

`imageeditor_refinement_quality_tests <output-directory> --flower <Flower.png>`
reproduces a small Magic Wand selection and compares Radius 0/5/26/53/250 plus two
Refine Edge sizes on black, white and as coverage. It is a visual regression
fixture, not a ground-truth alpha reference. Generated tests separately verify
large-radius recovery from a tiny island, disconnected details, holes, exact
trusted-region preservation and repeatability. Existing quality limits remain
checked for strands, soft edges and textured backgrounds.

`imageeditor_selection_refinement_tests --profile` measures the core;
`imageeditor_refinement_workspace_tests --native --profile` includes preparation,
publication and a submitted Vulkan frame. A representative Fedora 44/Qt 6.11.2
Release run with validation measured:

| Canvas | Reference/initial workspace | Subsequent Radius / Feather / Contrast |
| --- | ---: | ---: |
| 3840×2160 | 466 ms | 253 / 214 / 253 ms |
| 5120×2880 | 782 ms | 525 / 387 / 476 ms |

Feather and Contrast reuse the cached guided result; Radius reuses the reference
image but recalculates analysis. Peak process RSS was
about 1.17 GiB after both canvases and earlier test tabs remained open, including
UI/Vulkan resources; it is not the solver's allocation alone. Frame timings end
at submission, not physical display scanout. Complexity, effects and mask mapping
change these costs.

In a separate core-only run, Smooth 2, Shift Edge +3 px and Feather 6 px took
173 / 184 / 141 ms at 4K and 306 / 334 / 256 ms at 5K. Radius analysis plus
publication took 219 / 467 ms, including the additional foreground-core analysis.
The Flower fixture's 5–53 px radii and 75/130 px Refine Edge dabs took roughly
30–50 ms for analysis at its native 1122×1402 resolution; 250 px Radius took
about 353 ms in the same fixture. Its larger recovered
regions are still estimates: petal contamination and ambiguous mixed edges may
need Add/Subtract correction. Large radii and repeated
smoothing passes cost more; they remain cancellable between scanlines.
