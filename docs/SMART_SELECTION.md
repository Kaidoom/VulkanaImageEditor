# Smart Selection behavior and local Object Selection

Quick Selection paints evidence for an image region. Object Selection prompts an
object with a rectangle and correction points. Both produce ordinary temporary
selection masks; neither changes source pixels, creates hidden layers, or sends
images to a service. Magic Wand remains a separate contiguous color operation.

## Quick Selection

Pointer samples use the existing distance-based brush sampler. Brush diameter
defines evidence and influences nearby growth; the result is not a brush stamp.
Positive and negative strokes persist
with selection history; newer explicit evidence wins conflicts. Inferred negative
corrections constrain subsequent additions without becoming appearance-training
samples. Replace and Intersect start new evidence. Add and Subtract retain relevant
earlier evidence. Each request contains the complete current stroke, even when
intermediate requests are coalesced.

The authoritative Active Layer or Merged Visible reference is sampled in document
coordinates without clipping to the temporary selection. Typed content uses its
normal prepared source cache. Adjustment layers remain composition operators.
Invisible RGB cannot create color edges. Sampling and worker publication are
pinned to the runtime document instance, layer, source revision and request
generation. Pan and zoom do not invalidate reference pixels. Preparation uses
bounded row spans through the canonical compositor, including the existing
integer-aligned raster fast path, instead of individual virtual pixel reads.

Region discovery uses competing geodesic edge barriers and learned appearance
prototypes when both evidence classes are present. Growth follows the current
stroke rather than following weakly connected image texture indefinitely. Neither
window edges nor the image border become persistent background evidence.
Edges retains its original 0–100% meaning: higher values
strengthen monotone three-pixel boundary cues, capped relative to the fine edge.
It is not a growth or tolerance control; 100% does not mean maximum expansion.

The local-growth policy uses exact Euclidean distance `d` from the complete
rasterized evidence stroke, using the same distance transform as Refine Selection.
Extra support is `R = clamp(4 × brush diameter, 96, 192)` document pixels. The
inner half has no distance penalty. Beyond it, subtract
`max(1, basinCut) × max(0, 2d/R − 1)²` from the geodesic score. Target propagation
ends at `R`; twelve extra pixels of storage cover graph regularization and
antialiasing. This is a prior in the solver, not cropping a finished selection to
the stroke's rectangle. A bent path does not select the empty middle of its box.

A flat or ambiguous area now stops near the input rather than selecting the
entire image. Continue brushing or add strokes to cover more. Larger brushes
allow broader local work; Object Selection and Magic Wand retain their distinct
behaviors. No inferred locality boundary is stored as a negative correction.
Completed Add strokes preserve existing selection elsewhere; only explicit
Subtract evidence supplies lasting corrective constraints.

The basin score requires inferred support before considering an
escape boundary. A native-resolution contrast-aware Potts graph then regularizes
an eight-pixel band around the proposed boundary. Terminal evidence comes from
the geodesic field; pairwise costs are `3 exp(-squared RGBA difference / 392)` in
encoded premultiplied feature space. Explicit contrary strokes remain hard
constraints. Fixed neighbours enter the band graph as terminal costs. This is
neither whole-mask hole filling nor largest-component cleanup. Every component
connected to fresh positive evidence remains eligible.

The native Dinic solver uses iterative path storage, paired residual edges and
1/256 fixed-point cost units. Exhaustive independent label enumeration validates
its energy on small graphs. The solver does not incorporate reference-library
code. Requests still preflight the 64-million-pixel reference and
16-million-pixel working-window limits; allocation/processing failures are atomic.
Reported workspace size is a conservative bound, not measured RSS.

Workers produce a combined proposed selection once against the gesture's original
selection. The canvas displays that exact proposal, including Subtract and
Intersect. Useful completed prefixes can display during a held stroke; only the
latest complete request can commit on release. One gesture is one nonpersistent
history operation. Insufficient growth reports a limited result rather than a
successful smart region. There is no brush-stamp fallback disguised as inference.

## Object Selection

Choose Object Selection in Smart Select and draw a rough rectangle around the
intended object. The tested model and CPU runtime are bundled by default. Click to
include a missing part, or Alt-click to exclude background. Corrections revise the
same object proposal against its original selection baseline; they do not apply
Add/Subtract repeatedly to previews. Dragging a new rectangle starts another
object. Normal Replace/Add/Subtract/Intersect apply to that new object's result.
Undo and Redo include the rectangle and correction evidence.

The local adapter uses MobileSAM's TinyViT image encoder and prompt decoder with
ONNX Runtime's CPU execution provider. Both encoder and decoder run natively;
Python, PyTorch, CUDA and a system runtime installation are not needed by Vulkana.
It works offline from first launch. No download, model-folder selection or setup
control is needed. Build and runtime checks verify the reviewed input hashes.

The encoder receives RGB resized to a 1024-pixel longest side, rounded dimensions,
the model's channel mean/standard deviation, and zero padding on the bottom/right.
Coordinates use the actual rounded resize dimensions. Decoder output is unpadded
and reconstructed at document resolution. Qt's smooth resizing is used instead
of the Python reference's PIL resize; the character comparison before boundary
recovery had 0.9964 mask IoU, not bitwise preprocessing identity.

Near uncertain boundaries, original full-resolution colors classify pixels using
local confident foreground/background donors. The sampling support follows the
encoder footprint, not zoom. With missing or ambiguous two-class evidence, the
model proposal is retained. Output is binary coverage, not model confidence
mislabelled as physical alpha. Fully transparent/invalid reference pixels are
excluded, but partial source alpha is not multiplied into selection coverage.
Use Refine Selection for deliberate matting and soft boundaries.

There is one runtime and one image-feature cache per application window, not one
per open tab. Prompt changes reuse a valid immutable reference's features. Source
edits, transforms, effects, source-mode changes or a new reference invalidate it.
No reference image or features are stored in selection history. Cancellation uses
ONNX run termination; cancelled/stale jobs cannot publish. CPU arenas are disabled
to avoid retaining the encoder's peak working buffers indefinitely.

## Bundled model contract

A Linux x86-64 bundle contains `encoder.onnx`, `decoder.onnx`, `libonnxruntime.so`, their
license/notices, and `vulkana-mobilesam-v1.json`. The manifest has `format: 1` and a
SHA-256 string for each of those three filenames. Each binary must be below
128 MiB. The exact tested ONNX Runtime 1.30.0 binary is used with the 1.20 C API.
Changing model family requires a new contract, not replacing files
under this manifest name.

`tools/prepare-object-selection-pack.py` is a developer-only exporter that takes
explicit local inputs and refuses to overwrite an existing output directory. Run
it in an isolated environment with the supplied upstream MobileSAM checkout,
reviewed checkpoint, runtime library and runtime notices. It exports the complete
encoder plus dynamic-point decoder at ONNX opset 17. MobileSAM's upstream ONNX
script exports only the prompt encoder/decoder and is not a complete application
deployment by itself.

The tested upstream MobileSAM revision is
`f706ad9c4eb7f219c00d9050e46328518ffb65d2`. Model files total about 42.4 MiB;
the tested CPU runtime adds about 28 MiB. Preserve MobileSAM's Apache-2.0 license
and ONNX Runtime's MIT license and third-party notices in any distributed pack.
These ordinary Git-visible files live in `assets/object-selection/mobilesam-v1/`
and install to `share/vulkana-editor/object-selection/mobilesam-v1/`. CMake checks
the pinned bytes; package processing must not strip or rewrite this runtime.
No private research files or Python libraries are installed. The vendored
ONNX C API header has a separate MIT notice in the normal application inventory.
The tested runtime library requires glibc 2.28; the established Vulkana package
baseline is unchanged. Vulkan model acceleration has not been validated. ONNX
Runtime CPU is not a Vulkan backend; ncnn is a separate possible future port.

Development builds use the checked-in bundle. Installed builds resolve resources
relative to the executable through the normal application path service. The old
`VULKANA_OBJECT_SELECTION_PACK` and saved model-folder preference are no longer
read. Missing/corrupt installed files report an installation error, not a download
prompt or a fallback into the research directory.

## Quality comparison and limitations

The private 852 × 1847 character scene reproduces both previous failure modes:
brush-footprint inference, and excessive expansion through weakly connected dark
texture. The local version selects 37,646 pixels from 4,531 evidence pixels in the
floor stroke, compared with 54,763 before locality and 4,653 before the earlier
basin fix. The same floor click falls from about 519 ms to 42 ms, and the knee
stroke from 206 ms to 20 ms (Release core solve, diagnostics disabled). These are
behavior comparisons, not pixel-accurate Photoshop matches. Reflective floors,
highlights and low-contrast seams can still require more strokes or corrections.

Independent OpenCV 5.0.0 GrabCut comparisons use soft rectangle-exterior estimates,
not a hard image-border prior. Box GrabCut included furniture and missed part of
the leg in the character scene. Brush GrabCut depended strongly on initialization
and sometimes selected disconnected skin regions. It was not adopted wholesale.
MobileSAM was compared on the character, the existing flower photograph, a
generated non-person object with a handle/hole, and an object touching the image
border. Positive/negative corrections reuse the same encoded image. Multiple
objects in an ambiguous box may require correction or separate rectangles; no
largest-component rule secretly decides the intended object.

On a Ryzen 9 9900X, six CPU threads, the private native test measured approximately
0.36–0.37 s cold object evaluation, 29–36 ms repeated box evaluation and 0.23 s for
the first changed correction shape. Peak process RSS was typically 466–474 MiB;
one repeated-source/cancellation run under concurrent testing reached 659 MiB.
These measurements include runtime allocations, not just Vulkana's feature cache,
and are not a guaranteed memory cap. The actual native canvas gesture test
measured 0.31–0.38 s for the short Quick Selection stroke and 0.53–0.54 s for
rectangle Object Selection including reference preparation. These
are sample timings, not cross-machine guarantees. Native ONNX versus PyTorch
with identical prepared tensors produced zero differing thresholded mask pixels;
maximum image-feature error was about 1.1e-6.

For a generated textured region in 4K/5K documents, the warm core Quick Selection
solver took about 13 ms and measured process peaks were 51/83 MiB (including the
39.6/70.3 MiB immutable reference). A short input on a uniform 4K/5K image now
evaluates 49,729 pixels in about 9 ms, instead of repeatedly expanding across
13.9/32.2 million working pixels in 2.04/4.98 seconds. That case's peak process RSS
falls from 273/482 MiB to 47/79 MiB. It intentionally produces a local region,
not the previous full-canvas result.

Batched cold reference preparation measured 461/813 ms for one aligned layer at
4K/5K, down from 805/1428 ms. Two-layer transformed/blended preparation measured
1159/2059 ms, down from 1523/2742 ms. The coherent whole-canvas reference is still
prepared once when source content changes; subsequent strokes reuse it. This
remaining cold cost is distinct from local solving and stays cooperatively
sliced on the owner thread. Larger documents or complex effects can cost more.

The generated 4K/5K canvas interaction test measured 513/735 ms from the cold
gesture to idle, and 22/23 ms for the warm correction. Its longest observed UI
event-loop gap was 7 ms. Native character-scene gestures took 277 ms on Wayland
and 302 ms on XCB, including initial reference preparation and held-stroke preview.

Fine gaps/strands absent from the model proposal can still require corrections or
Refine Selection. Model boundaries are not recovered foreground colors, and
residual edge tint is not removed. Private images, masks, model experiments and
comparison artifacts stay under ignored directories and outside releases.

## Verification and references

Focused Release tests cover graph energy, textured regions, thin features, holes,
correction precedence, cancellation, source sampling, history and exact preview
publication. `imageeditor_smart_selection_review image prompts.json output`
captures source, explicit evidence, edge costs, barrier field, connected/regularized
inference, incoming mask and published mask without application-side file writes.
The local-growth revision additionally tests bent paths without bounding-box
filling, distant pixels remaining unchanged, progressive extension, repeat-input
identity, and stable local work on larger canvases. The Euclidean prior is checked
against a brute-force distance oracle. Batched reference pixels match independent
single-pixel sampling exactly across multiple chunk sizes, including masks,
projective transforms, Dissolve, clipping groups and adjustment layers. The eight
focused Release suites and native Wayland/XCB canvas checks pass; Object Selection
masks on the private fixture are unchanged by the shared sampling optimization.
`imageeditor_object_selection_tests --bundled image output` exercises the bundled
native model, cached prompts, source invalidation and in-flight cancellation.
The interaction test's `--private-quality image output` exercises actual canvas
pointer gestures, undo/redo and tab lifetime; private images are not test assets.
The focused Release run passed 12 tests, including existing Refine Selection and
document-tab regressions. Native Wayland and XCB Vulkan checks passed with zero
validation warnings/errors. Both display systems passed the private canvas gesture
checks; a native screenshot also verifies the displayed object boundary. The local
pack exporter reproduced both tested ONNX files byte-for-byte. No installed
Photoshop version was available for this pass: the supplied screenshots are
behavioral references, not asserted pixel ground truth.

Bundling validation also runs the complete encoder/decoder in the Ubuntu 24.04
baseline container with networking disabled, and runs default resource lookup
from a relocated installation with development paths compiled out. Bundled-model
outputs for the private image and corrected prompt are byte-identical to the
quality-tested optional implementation. No model/runtime conversion was performed
when promoting the feature to the default installation.

- [GrabCut paper and authors](https://www.microsoft.com/en-us/research/publication/grabcut-interactive-foreground-extraction-using-iterated-graph-cuts/)
- [OpenCV segmentation API](https://docs.opencv.org/4.13.0/d3/d47/group__imgproc__segmentation.html)
- [MobileSAM source and model](https://github.com/ChaoningZhang/MobileSAM)
- [MobileSAM ONNX export scope](https://github.com/ChaoningZhang/MobileSAM/blob/master/scripts/export_onnx_model.py)
- [ncnn CPU and Vulkan runtime](https://github.com/Tencent/ncnn)
