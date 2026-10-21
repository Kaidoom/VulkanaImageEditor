# Remove Background

Remove Background generates an editable layer mask using bundled, offline CPU
models. It does not upload images, download models or erase the original pixels.

## Use

Select one raster layer and choose **Layers → Remove Background…**. The preview
starts automatically with **U²-NetP**, the small, fast default. Choose
**BiRefNet-lite INT8** for a second result with a larger model and working-memory
requirement. The better result depends on the image; inspect fine edges before
applying either mask.

The bundled files are:

| Choice | Path relative to the model directory |
|---|---|
| U²-NetP | `u2netp.onnx` |
| BiRefNet-lite INT8 | `birefnet-lite-512/onnx/model_int8.onnx` |

These are the only supported models. U²-NetP uses a 320×320 input and
BiRefNet-lite uses 512×512. Both return a full-resolution mask, but small details
can still be lost during inference. Hair, translucent objects and low-contrast
boundaries may need manual correction or Refine Mask after Apply.

The canvas previews the isolated raw raster, retaining its transform, raster
origin, crop, original transparency and enabled existing mask. Other layers,
opacity, adjustments and effects are omitted from this isolated preview.
Those document properties are not changed by Apply.

Switching the model regenerates the preview. **Retry** is available after a
failed attempt. Hold **Original** to compare with the
layer before removal. Transparency, black and white backgrounds inspect the
cutout; **Mask Only** shows the raw generated mask before source transparency
and any existing layer mask. Middle mouse pans; the wheel zooms; Space+drag
also pans.

Apply combines the generated coverage with an existing enabled mask, or creates
a mask when none exists. It does not erase source pixels or apply source alpha
twice. One undo restores the exact previous mask. Unrelated selections stay
unchanged. Disabled existing masks must be enabled or removed first, avoiding
silent loss or reactivation of their contents. Empty/failed results cannot be
applied. Cancel returns immediately without document changes.

## Bundled assets

The model files, checksums, provenance and licenses are in
[`assets/background-removal/v1`](../assets/background-removal/v1/). The normal
build and installation use those files; no development model directory or
Python environment is needed. ONNX Runtime CPU is shared with Object Selection,
whose MobileSAM model is independent of Remove Background.

U²-NetP is distributed unchanged from rembg's ONNX release under the upstream
U-2-Net Apache-2.0 terms, with rembg's MIT reference attribution retained.
BiRefNet-lite is derived from Studio Ludens' MIT-licensed 512×512 ONNX conversion
of ZhengPeng's BiRefNet-lite. Complete terms and the modification notice accompany
the models and are included in Vulkana's offline third-party notices.

BiRefNet-lite uses per-channel signed INT8 weights and calibrated activation
ranges for convolution and dense operations (ONNX QDQ). Other operations remain
floating-point; graph inputs and outputs remain FP32. A subsequent graph rewrite
reduces intermediate layout conversions without changing the quantized weights
or scales. INT8 reduces the file size, not necessarily inference time or peak
memory in the same proportion. It is a derived model, not an upstream INT8
release. Quantization can change soft edges; it does not guarantee FP32 parity.

## Adapter contract

`BackgroundRemovalEngine` owns one CPU ONNX session. The `profiles` table in
`BackgroundRemoval.cpp` declares file location, input normalization/resampling,
dynamic-size fallback and output interpretation. Input/output names, dimensions
and element types come from the graph; unsupported metadata fails locally for
that model. Both supported graphs use FP32 input/output tensors; INT8 is
internal to BiRefNet's graph. Git LFS pointers, invalid files and missing
external tensor data produce errors, not downloads.

- U²-NetP uses RGB Lanczos resizing, division by the resized
  image maximum, ImageNet normalization, and first-output min/max scaling.
  Quantized coverage is Lanczos-resized back to the original dimensions.
- BiRefNet-lite uses RGB bilinear resizing, `/255` plus ImageNet normalization,
  then sigmoid on output logits and bilinear resizing of floating-point coverage.

No threshold, morphological cleanup, feathering, colour decontamination or
automatic refinement is added. Final masks use the editor's native 8-bit
coverage and always match the raster source dimensions. Existing Refine Mask
remains available after Apply.

Add another adapter by extending the profile table and its synthetic-graph
tests. Models needing a different tensor contract require an explicit adapter,
not guesses from filenames or tensor confidence. Runtime/session creation is
separate from preprocessing, output decoding, mask composition and UI state;
a future GPU execution provider can be investigated at that boundary without
rewriting the workspace. No GPU inference provider is enabled.

## Responsiveness and memory

Source pixels are copied on the UI thread in bounded row batches under an edit
barrier. Workers only read owned copies, not mutable document surfaces.
Inference and postprocessing run on a worker. Model switching cancels the old
request and discards stale results before loading the next model.

Only one model session is retained. Previous sessions are released when
switching; allocators may retain freed memory, so process RSS need not fall
immediately. Source copies, full-resolution masks and preview buffers add memory
on top of model inference. The source limit is 64 megapixels / 32768 per side,
not a guarantee that every machine has sufficient working memory.

The **Object Selection CPU threads** setting in Performance preferences also
controls background removal. The panel shows total preparation time; its timing
tooltip separates model initialization, preprocessing, inference and postprocessing.
Mask composition and initial source copying are included in the total, but GPU
display latency is not.

ONNX inference supports cooperative termination. Session construction does not
support interruption through the bundled C API: it stays on the worker and a
cancelled initialization is discarded afterward. Cancel restores the editor
immediately; reopening this feature waits for the retired worker to finish.
Application process shutdown joins outstanding work and can therefore wait for
model initialization. Never force-terminate a worker thread.

## Verification and benchmarking

Backend unit tests use small synthetic graphs and malformed temporary fixtures.
Interaction and relocated-install tests exercise both bundled models offline,
including missing/damaged files and recovery. No private assets are required.

```sh
ctest --test-dir build/release --output-on-failure \
  -R 'imageeditor_(background_removal_|bundled_background_removal_)'
```

Run actual-model measurements with the bundled directory:

```sh
./build/release/tests/imageeditor_background_removal_probe \
  ./assets/background-removal/v1 /absolute/path/to/image.png all \
  /absolute/path/to/results 2
```

Arguments after the image are model ID (`all` by default), output directory
(`-` to skip writing masks), repetition count, and optional cancellation delay
in milliseconds. Each JSON line records graph metadata, stage timings, mask
statistics and Linux peak process RSS. For meaningful per-model memory peaks,
run each model ID in a fresh process; peak RSS is cumulative within a process.

```sh
QT_QPA_PLATFORM=offscreen \
  ./build/release/tests/imageeditor_background_removal_interaction_tests \
  --image /absolute/path/to/image.png
```

Use `--native` with `QT_QPA_PLATFORM=wayland` or `xcb` for native UI/Vulkan checks.
The interaction test switches between both bundled models, discards stale work,
applies/undoes an INT8 mask, and closes while model initialization is pending.
`tests/BackgroundRemovalReferenceTests.py` optionally compares adapter output
against Pillow on synthetic portrait/landscape inputs. Pillow/Python are test
tools only; neither is an application dependency.

The application verifies bundled model checksums and does not accept an
environment override. The standalone benchmark accepts an explicit trusted
model directory for adapter development; do not give it untrusted ONNX files.
