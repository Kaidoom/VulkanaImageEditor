# Vulkana Diffusion

Vulkana Diffusion is a local AI image editor powered by an isolated native worker. This integration currently targets Linux CUDA. Models are supplied by the user; Vulkana does not download or bundle weights.

## AI Image Editor

Open **Edit → AI** and choose:

- **Generate Image:** generate without a target. Optional references are still allowed, and other editing can continue during inference.
- **Edit Current Layer:** capture the selected raster layer as Image 1 and begin an editing session.
- **Generative Fill:** capture the visible canvas and current soft selection, then restrict application of the result to that selection.
- **AI Image Editor:** reopen the panel in its previous mode.

The non-dockable panel occupies the right edge of the workspace. Its tabs are **Configuration**, **Generate** and **Preview**. Configuration contains saved model setups, sampling and performance settings; Generate contains prompts, presets, inputs, size and seed; Preview contains completed variations and their apply/discard actions.

Generation uses immutable input snapshots and never locks canvas editing. Closing the panel leaves the job running. Progress stays in the open panel. When the panel is closed, a dismissible progress card can reopen it. Completion always shows a notification. Closing a source document asks for confirmation; the job can continue without it. Quitting Vulkana asks before cancelling an active job. Results never modify a document before Apply.

Select **Auto**, **Vulkan** or **CUDA** in Configuration. Auto prefers an installed CUDA worker, then Vulkan; explicit choices are checked against the worker's reported backend. An unavailable backend produces a clear error rather than running a different backend silently. Linux CUDA is the currently validated worker; Vulkan still requires a separate worker build and validation. Worker paths are discovered from the installation or development overrides and are not an editable UI setting.

Save named model configurations with explicit component paths; multiple configurations may use the same family. Choose a device from the worker's device list. These are inference device IDs, independent of the canvas renderer's Vulkan IDs.

Essential controls include mode, model, prompt, output dimensions and seed. The compact size-reset button matches the first edit input or the active canvas. Output destinations are chosen in Preview. Open Configuration and expand the sampling and resource sections for technical settings. Generate is disabled until the required model paths have been entered. **Random** chooses a seed for every job; **Fixed** reuses the entered seed; **Increment** advances it after each successful job by the batch size (wrapping to zero at the maximum). The last generated seed is recorded with each result. Failed or cancelled jobs do not advance an Increment seed, and scrolling over numeric controls or closed dropdowns scrolls the panel without changing values. Profile defaults are recommended starting points; explicit overrides are retained. Negative prompts are enabled when CFG exceeds 1, denoising strength is used for image-to-image/fill, and unsupported vision or prefix-cache controls are disabled. Approximate inference caches are experimental and can change results.

### Inputs and variations

Add immutable inputs from a layer's raw pixels, an isolated rendered layer, the visible canvas or a local image file. The list supports inspection, removal and reordering, including an automatically added first image. Edit mode uses the first image; removing or reordering it changes the edit input. Edit and Fill cannot generate with an empty input list. Image files honor orientation and are converted to sRGB; alpha is retained.

An isolated rendered layer includes its mask, opacity and effects against transparency. Backdrop-dependent blending may differ from the complete canvas: use **Visible Canvas** when that context is important. Edit Current Layer uses native raster pixels so existing layer effects, masks and transforms are not baked twice when replacing content.

Generated variations appear in Preview, which opens automatically when a job finishes. Applying a result, changing modes or closing its original document does not remove it from history. Discard removes only the selected result. History is kept in memory and private temporary files until discarded or Vulkana exits; it is not saved in the project. Hover over a preview for its magnifier, then click to inspect its full-resolution image in an owned lightbox, with Fit, 1:1, wheel zoom, middle-button pan and a draggable comparison divider. Regenerate retains earlier successful variations while the next job runs. Select an earlier variation to return to it, or use Previous/Next in the expanded preview to cycle through the history. During generation, a separate Pending item shows the live approximate preview when available, even with completed results in history. It can be inspected and browsed with Previous/Next, but cannot be applied as a final result. Cancellation removes the pending item; completion adds the finished image. Earlier results stay intact.

One model configuration stays loaded between jobs. **Unload all models**, directly below the configuration controls, stops the worker and releases all of its model memory and runtime caches. It is disabled during generation.

Preview dimensions are shown in the expanded preview. Output dimensions must be multiples of the model alignment; the size-reset button rounds up when necessary. Reference encoders may internally resample inputs, and the model can recompose details even when input and output dimensions match. **Compare selected layer** captures the current raster, text or shape layer's isolated rendered appearance, including transforms, masks and effects. Groups and adjustment layers are incompatible. The left side shows the selected layer and the right side shows the result. Selection changes update the comparison without editing the document.

- **Canvas Alignment** preserves document positions and scale, including off-canvas origins and transparency around smaller layers. An edited result uses its captured input's placement; a new generation starts at the canvas origin.
- **Center Alignment** aligns image centres at native pixel sizes, without rescaling either image.
- **Fit to Result** proportionally fits the reference within the generated image, preserving aspect ratio and transparent margins.

Zoom and middle-button panning remain available while dragging the divider.

### Apply results

- **Add as New Layer** is the default and uses one undoable layer insertion.
- **Replace selected layer** targets the current raster layer in the active document. Groups and other incompatible selections are disabled. Replacement retains the destination layer's transform, mask, blending and effects and is undoable.
- **Open new document** creates an independent document from the selected result.

For Generative Fill, Vulkana retains the original soft selection and composites through it when applying. Replacement and new-document output preserve pixels outside the selection exactly; a new result layer is transparent outside it. A new layer overlays its existing backdrop, so it cannot erase transparency in the layers below. Replacement is confined to the target raster's existing pixel grid; new-layer output can cover selected regions elsewhere on the canvas.

Replace selected layer captures and validates the destination at Apply time, preserving its current pixels outside a captured Fill selection; it does not use an unrelated current selection. The command checks that revision again before applying. Results do not depend on the original layer remaining available. Editable text/shapes are not automatically flattened: use their rendered appearance as an input and apply to a new raster layer. The Edit Current Layer menu entry initially captures a raster layer, but inputs can subsequently be replaced with other image snapshots.

## Supported models and regions

**Qwen Image 2.1 Turbo:** diffusion GGUF, Qwen3-VL 8B text encoder, Qwen Image 2.1 VAE, and the matching vision projector for reference editing. Recommended: 8 steps, Euler, CFG 1, exact Turbo sigma schedule. The base Qwen profile is available but has not been independently exercised with base weights.

**FLUX.3 Image Klein (distilled):** FLUX.3 diffusion GGUF, Qwen3-VL 4B text encoder and FLUX.2 VAE. Recommended: 16 steps, Euler, CFG 1, discrete flow schedule with shift 6.93. The pinned engine consumes image references as VAE conditioning and does not require a separate vision projector. It supports up to ten references; Qwen's worker profile currently allows eight.

Draw, move, resize, label or remove rectangular regions in the panel's region editor. Region bounds are normalized and independent of a model's prompt syntax:

- FLUX.3 receives the label table appended to its caption, using `[y_min, x_min, y_max, x_max]` on a 0–1000 grid. This follows the reference implementation's non-upsampling path. The pinned native engine has no separate bounding-box tensor input; this is semantic caption guidance, not a spatial hard constraint. [Reference generation path](https://github.com/black-forest-labs/flux-3-image-klein/blob/main/src/flux3/generate.py), [box format](https://github.com/black-forest-labs/flux-3-image-klein/blob/main/src/flux3/bbox.py).
- Qwen receives an additional numbered annotation image and instructions for those regions. One reference slot is reserved for this guide. FLUX-specific box syntax is never sent to Qwen.

Region guidance is experimental and never replaces the pixel-preservation mask. Models may miss a box or alter unrelated areas during generation; Generative Fill's application mask provides the pixel guarantee. Prompt enhancement is not implemented. Detailed prompts can be supplied directly; FLUX.3's reference workflow recommends expanded captions for quality. [Upstream guidance](https://github.com/black-forest-labs/flux-3-image-klein#prompt-upsampling).

## Build and test

The ordinary Vulkana build includes the UI and lightweight QtCore client. It does not require the inference engine or CUDA. The module can also be built and tested independently:

```sh
cmake -S src/diffusion -B build/diffusion-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/diffusion-host
ctest --test-dir build/diffusion-host --output-on-failure
```

The worker is a separate optional build. Use an existing stable-diffusion.cpp checkout at the revision in `src/diffusion/engine-lock.json`, with its recorded GGML and CUTLASS submodules initialized. CMake verifies those revisions and downloads nothing. Downstream patches are allowed; record the patch and its reason alongside the baseline before distributing a changed engine.

Example CUDA build (set the paths and CUDA architecture for the development machine):

```sh
cmake -S src/diffusion/worker -B build/diffusion-worker -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DVULKANA_DIFFUSION_ENGINE_DIR=/absolute/path/to/stable-diffusion.cpp \
  -DSD_CUDA=ON -DSD_VULKAN=OFF -DGGML_NATIVE=OFF \
  -DCMAKE_CUDA_ARCHITECTURES=89-real
cmake --build build/diffusion-worker --target vulkana-diffusion-worker
ctest --test-dir build/diffusion-worker --output-on-failure
```

Choose a host compiler supported by the installed CUDA toolkit. The example targets RTX 40-series development hardware; it is not a multi-generation release binary. Tests use Python's standard library, but neither the worker nor the host client requires Python at runtime. Set `BUILD_TESTING=OFF` to omit worker tests and their Python dependency.

The real-inference smoke executable accepts an absolute worker path, a dedicated private work directory, a load-parameters JSON file, and a generate-parameters JSON file:

```sh
build/diffusion-host/imageeditor_diffusion_worker_smoke \
  /absolute/path/to/vulkana-diffusion-worker \
  /absolute/path/to/private-results \
  /absolute/path/to/load.json /absolute/path/to/generate.json
```

It checks warm jobs, previews, host event-loop responsiveness, cancellation, recovery, output independence, unload and restart. It writes results into the supplied work directory and reports JSON on stdout. Model files are only read. It is deliberately excluded from automatic tests.

## Client and worker lifecycle

`imageeditor::diffusion::WorkerClient` owns one `QProcess`. Call `start()`, wait for `ready`, then send requests. All client methods run on its QObject thread; inference runs in the child process. The worker retains one model context between jobs. Call `unload` to release it or `stop()` to close the worker. Completed output files remain available until the caller removes its work directory.

Load, generate and unload jobs run sequentially. A second job receives `busy`; status, ping, cancellation and shutdown remain available. Validation errors leave a loaded context intact when validation can be completed before loading. Replacing a model releases the old context before constructing the new one to avoid doubling memory use. Construction failure can therefore leave the worker unloaded.

Cancellation is cooperative at engine stage/step boundaries. The client waits for the original job's terminal response, not merely its cancellation acknowledgement. Its configurable grace timeout terminates only the owned worker if a constructor/kernel does not cooperate. Stop has its own grace timeout. There is no generation timeout just because a job is slow. Restart and reload after forced termination, a crash, or an unexpected engine exception. A successfully cancelled job publishes no final result. A cancellation arriving after completion does not revoke a completed result.

Worker crashes, backend assertions and incomplete engine constructors are isolated from the editor. Recoverable generation failures invalidate the model and request a reload. The worker reports recognized allocation failures as `out_of_memory`, managed-budget failures as `memory_limit`, and other failures separately. Unexpected process exits are not assumed to be OOM.

## Protocol 1

Communication is UTF-8 newline-delimited JSON over stdin/stdout, without sockets or network ports. Engine diagnostics, including third-party stdout writes, are redirected to stderr. The worker command is:

```text
vulkana-diffusion-worker --work-dir ABSOLUTE_PRIVATE_DIRECTORY
```

The directory must exist and, on Linux, be owned by the current user with mode `0700`. The client creates it when needed. Requests and output lines are limited to 1 MiB. Use unique request IDs (1–64 ASCII letters, digits, underscores or hyphens). The Qt client generates UUIDs. Reusing an in-flight ID closes the ambiguous session. EOF requests shutdown.

The first message is `{"event":"hello","protocol":1,...}` and includes the engine revision, backend, device IDs, profiles, samplers and schedulers. Requests have this form:

```json
{"protocol":1,"id":"load-1","method":"load","params":{}}
```

Terminal responses are `{"id":"...","ok":true,"result":{...}}` or `{"id":"...","ok":false,"error":{"code":"...","message":"..."}}`. Unknown fields, invalid types and unsupported values are rejected. Methods are `ping`, `status`, `load`, `generate`, `cancel`, `unload`, and `shutdown`.

Events report `state`, `progress`, `preview`, or a fatal protocol error. Progress has `scope:"engineOperation"`: counters may restart between tensor loading and sampling and are not a single overall job percentage. Preview events carry `jobId`, `path`, `step`, `width` and `height`. Previews are approximate latent projections; final images use the VAE. The client ignores expired previews after cancellation and checks that output paths remain within its work directory.

Generated PNGs are independently owned files under `job-<id>/`. Result objects include `images[].path`, dimensions/channels, engine generation metadata, profile, effective resource settings, context generation number and timings. Timings distinguish model construction, engine execution and PNG output; `engineStagesMs` provides conditioning/encoding/sampling/decoding durations reported by the pinned engine. Lazy weight preparation also contributes to the first generation's time. Cooperative cancellation removes that job's partial outputs. Forced process termination can leave partial files; the caller owns cleanup of its private work directory.

## Model profiles and resource settings

The profile registry is `src/diffusion/profiles.json`. Qwen Image 2.1 Turbo has been exercised with local GGUF components. Base Qwen needs independent validation with base weights; the distilled FLUX.3 profile has been exercised with local GGUF components. The engine checks the loaded architecture against the selected family; Turbo/base and distilled/base variants still require the user to choose the matching profile. A GGUF filename alone does not establish compatibility.

Example `load` parameters:

```json
{
  "profile": "qwen-image-2.1-turbo",
  "files": {
    "diffusion": "/models/diffusion.gguf",
    "textEncoder": "/models/text-encoder.gguf",
    "vae": "/models/vae.safetensors",
    "vision": "/models/vision.gguf"
  },
  "settings": {
    "device": "CUDA0",
    "maxVramGiB": 10,
    "threads": 0,
    "weights": "auto",
    "textEncoderCompute": "gpu",
    "vaeCompute": "gpu"
  }
}
```

Copy `device` from `hello.devices`; it is the inference backend's ID, not a Vulkan renderer index. The panel exposes inference device selection directly; automatic mapping from the renderer preference is not implemented. Paths must be absolute readable files. Git LFS pointers are rejected.

| Setting | Values / default |
|---|---|
| `maxVramGiB` | 0–1024; default 10 GiB, 0 uses engine Auto |
| `threads` | 0–1024; 0 uses physical CPU cores |
| `weights` | `auto` (default), `gpu`, `ram`, `disk`, `manual` |
| `placements` | For `manual` only: all of `diffusion`, `textEncoder`, `vae`, each `gpu`/`ram`/`disk` |
| `textEncoderCompute`, `vaeCompute` | `gpu` (default) or `cpu` |
| `flashAttention`, `directConvolution` | `auto` (default), `on`, `off` |
| `sageAttention` | false by default |
| `mmap`, `prefetch`, `segmentedCompute` | true by default |
| `eagerLoad` | false; prepare weights lazily |
| `conditioningCacheSize` | 0–64; default 1, 0 disables |
| `prefixCache` | Qwen only; true by default |
| `prefixCacheType` | Qwen only: `auto`, `f32`, `f16`, `bf16`, `q8_0`, `q4_0`, `q4_1`, `q5_0`, `q5_1`, `q4_K`, `q6_K` |

The memory budget covers managed weights and workspaces, not all driver, library and desktop allocations. It is not a hard physical VRAM cap. Auto placement may use RAM or disk; explicit weight placement disables auto-fit. Moving text weights to RAM still permits GPU computation. `textEncoderCompute:"cpu"` moves the computation itself to CPU. Quantized prefix caches are explicitly lossy options. Persistent contexts retain weights and caches; unload when memory is needed elsewhere.

## Generation and editing

Example `generate` parameters:

```json
{"prompt":"A ceramic teapot on a blue cloth","width":1024,"height":1024,"seed":137,"preview":true}
```

| Setting | Behavior |
|---|---|
| `prompt`, `negativePrompt` | Plain model prompts; the engine applies its model-specific wrapping. Negative prompts require guidance above 1. |
| `width`, `height` | Default 1024; multiples of the profile alignment, 32–8192 each. Combined batch output is limited to 64 megapixels for allocation validation. |
| `steps`, `guidance` | Profile defaults; steps 1–1000, guidance 0–100. Turbo uses 8 / 1. |
| `seed`, `batch` | Seed -1 (random) or an exactly representable non-negative JSON integer; batch 1–8. |
| `sampler`, `scheduler` | Profile sampler / automatic scheduler by default; supported values in `hello`. |
| `schedule`, `sigmas` | `auto`, `turbo`, or `custom`. Turbo requires 8 steps. Custom requires steps+1 strictly descending finite values ending at zero. |
| `referenceImages` | Ordered local images, up to the profile limit (Qwen 8, FLUX.3 10). Qwen requires vision weights loaded. |
| `initialImage`, `mask`, `strength` | Optional image-to-image input, matching-size grayscale mask, and strength 0–1 (default 0.75). Mask requires initialImage. |
| `vaeTiling`, `vaeTileSize`, `vaeTileOverlap` | false / 256 / 0.25; tile size 64–2048 in multiples of 32, overlap 0–0.5. |
| `preview`, `previewInterval` | true / every step by default; latent projection previews. |
| `flowShift` | 0 selects the engine's model default; positive values override it. FLUX.3's default is 6.93. |
| `eta` | -1 selects the sampler default; non-negative values override stochastic sampler noise. |
| `cacheMode`, `cacheReuseThreshold`, `cacheStart`, `cacheEnd` | Experimental `disabled` / `easycache` / `ucache`; defaults disabled, 0.2, 0.15, 0.95. |
| `spatial` | Optional schema `{version:1, regions:[{x,y,width,height,label}], annotationImage?:path}`. Bounds are normalized, finite and inside the image. Qwen requires the annotation image when regions are supplied. |

The Turbo profile supplies its exact eight-step sigma schedule. Select `schedule:"auto"` or provide a custom schedule to experiment with another step count. Reference images retain their own dimensions; initial images/masks must match output dimensions.

The engine's generic latent mask uses binary coverage; it is not model-native inpainting. White permits regeneration and black preserves the initial latent region. It does not guarantee pixel-exact protection through a VAE. Editor integration must retain the original soft selection and composite the result through it when strict preservation is required. Reference editing can alter unrequested areas. Raw generated alpha is preserved, not thresholded or replaced.

## Prompt presets

`PromptPresets` supplies Generate Image, Generative Fill, Replace Background and Remove Background templates. Presets describe intent; they do not grant a model capabilities it lacks or establish a selection by themselves.

`PromptPresetStore` supports custom creation, update, removal and reload. Its default file is `diffusion/prompt-presets.json` under Qt's application-data directory. Writes use `QSaveFile`; invalid input, duplicate IDs/names and malformed files leave existing data intact. Built-ins cannot be overwritten; save a custom copy to customize one.

Templates accept `{instruction}`, `{background}`, `{subject}`, and `{style}`. `{{` and `}}` produce literal braces. Expansion is nonrecursive and cannot execute commands. Selecting a preset in the editor inserts an editable example without opening a parameter prompt. Example values replace template variables, and the inserted text can be changed or undone normally. The lower-level template API still validates explicitly supplied variables. Presets store only prompt/negative-prompt templates and task descriptions, independently of model paths and resource settings.

## Testing and distribution

Run the deterministic host and UI tests with `ctest --test-dir build/release -R imageeditor_diffusion --output-on-failure`. They cover process lifecycle, validation, snapshots, undo, soft selections, preview navigation and comparison. The opt-in real-inference smoke executable described above requires user-provided models.

Document transactions stay in the editor; the worker receives immutable files and returns independent images. Input capture and Apply compositing use the editor thread, so unusually large images can pause briefly during those steps. Inference and model loading remain asynchronous.

Linux CUDA is the supported inference target. Windows inference, Vulkan runtime operation and packaged worker distribution require further validation. Building the editor does not build or bundle an inference worker. A worker distribution must include the applicable engine, GGML and linked dependency notices and comply with the selected backend runtime's redistribution requirements. Model weights are supplied separately by the user.
