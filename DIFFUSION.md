# Vulkana Diffusion

Vulkana Diffusion is a local AI image editor powered by an isolated native worker. This integration currently targets Linux CUDA. Models are supplied by the user; Vulkana does not download or bundle weights.

## AI Image Editor

Open **Edit → AI** and choose:

- **Generate Image:** generate without a target. References can be added when the selected model supports them, and other editing can continue during inference.
- **Edit Current Layer:** capture the selected raster layer as Image 1 and begin an editing session.
- **Generative Fill:** capture the visible canvas and current soft selection, then restrict application of the result to that selection.
- **AI Image Editor:** reopen the panel in its previous mode.

The AI panel temporarily occupies the right dock’s content area and shares its resize handle. Closing it restores the regular panels, tabs and splitter layout. It starts hidden on each launch and stays in place behind preview overlays. Its tabs are **Configuration**, **Generate** and **Preview**. Configuration contains saved model setups, sampling and performance settings; Generate contains prompts, presets, inputs, size and seed; Preview contains completed variations, image details, and Generate again/Discard actions. Apply controls are in the expanded image preview.

Generation uses immutable input snapshots and never locks canvas editing. Closing the panel leaves the job running. The Generate button changes to Cancel while a job runs; progress and status remain in the panel footer. When the panel is closed, a dismissible progress or completion card offers a magnifier that opens the latest expanded preview directly, without changing tabs. Progress and completion notifications stay hidden while the AI panel is open. Closing a source document asks for confirmation; the job can continue without it. Quitting Vulkana asks before cancelling an active job. Results never modify a document before Apply.

Select **Auto**, **Vulkan** or **CUDA** in Configuration. Auto prefers an installed CUDA worker, then Vulkan; explicit choices are checked against the worker's reported backend. An unavailable backend produces a clear error rather than running a different backend silently. Linux CUDA is the currently validated worker; Vulkan still requires a separate worker build and validation. Worker paths are discovered from the installation or development overrides and are not an editable UI setting.

Choose a **Family**, then a **Variant** where available. Qwen 2.1 Image offers Base and Turbo; Flux Klein offers Flux 2 base/distilled and Flux 3 base/distilled. Families with one choice hide Variant. Click the **+** button beside the configuration dropdown and enter a unique name, then select the model files. The dropdown is disabled until a configuration exists. Edits to files, sampling, backend, performance settings and LoRA defaults save automatically. Multiple configurations may use the same variant. The compact buttons beside the configuration dropdown duplicate the setup under a new name or delete it after confirmation. Name and confirmation cards are centered over the AI panel and block interaction until dismissed. The separate **Session → Loaded models** section shows the worker’s resident models; unloading them does not delete a configuration. Existing saved profile IDs remain compatible. Choose a device from the worker's device list. These are inference device IDs, independent of the canvas renderer's Vulkan IDs.

Essential controls include mode, model, prompt, output dimensions and seed. The compact size-reset button matches the first edit input or the active canvas. Output destinations are chosen beside Apply in the expanded image preview. Open Configuration and expand the sampling and resource sections for technical settings. Generate is disabled until the required model paths have been entered. **Random** chooses a seed for every job; **Fixed** reuses the entered seed; **Increment** advances it after each successful job by the batch size (wrapping to zero at the maximum). The last generated seed is recorded with each result. Failed or cancelled jobs do not advance an Increment seed, and scrolling over numeric controls or closed dropdowns scrolls the panel without changing values. Profile defaults are recommended starting points; explicit overrides are retained. **Negative prompt** is a collapsed section immediately below the positive prompt. Its field is disabled unless the model supports it and CFG exceeds 1; collapsing or disabling it preserves the entered text. Unsupported vision or prefix-cache controls are disabled. Approximate inference caches are experimental and can change results.

**Use image-to-image denoising** and **Denoising strength** are in Generate. Unavailable controls remain visible, with tooltips explaining when they apply:

| Mode | Starting image | Denoising strength |
| --- | --- | --- |
| Generate | None; input images are references only | Disabled |
| Edit layer, image-to-image off (default) | None; Image 1 and other inputs guide reference editing | Disabled |
| Edit layer, image-to-image on | Image 1, scaled to the output size | Enabled |
| Generative Fill | Rendered canvas and the captured selection mask; image-to-image is always on | Enabled |

Lower strength starts later in the denoising schedule, retaining more of the starting image and running fewer steps. Try **1.0** for object insertion or substantial replacements; it uses the full schedule while references still guide the result. Strength is not a reference-image influence slider. The native engine discretizes strength to sampling steps, so nearby values can produce the same schedule, and zero is not a pixel-exact passthrough. Fill's soft selection is still enforced separately when applying the result.

New Generative Fill configurations start at **1.0** denoising strength, adjustable in Generate. Saved or adjusted strength values survive mode switches and are not replaced by this default. The Edit-layer image-to-image preference is also preserved; Fill's mandatory checked state does not overwrite it. Both values persist across sessions and can be saved in model configurations; existing configurations retain their saved strength. Steps, CFG, sampler, schedules, caching, tiling and resource placement remain in Configuration because they apply across modes. Their availability can depend on the model or another inference setting rather than the editing mode.

### Inputs and variations

Add immutable inputs from a layer's raw pixels, an isolated rendered layer, the visible canvas or a local image file. The list supports inspection, removal and reordering, including an automatically added first image. Edit mode uses the first image; removing or reordering it changes the edit input. Edit and Fill cannot generate with an empty input list. Image files honor orientation and are converted to sRGB; alpha is retained.

Layer inputs have a **Crop to canvas** toggle beside their delete button. It renders the part inside the captured source canvas at document pixel scale, respecting the layer transform. Toggle it off to restore the complete snapshot. Regions follow the cropped image; regions wholly outside the canvas are retained but hidden until restored. Rotated region rectangles use enclosing bounds. A completely off-canvas input must be restored or removed before generation. External files have no document canvas and do not offer this toggle.

**Max input**, below the list, limits each input's longest side proportionally after cropping. It defaults to **1024 px**, is adjustable from 64 to 8192, and persists across sessions. Original snapshots are retained, so raising the limit restores detail. The limit also applies to region crops and output guides in the worker. Small references are not enlarged to the full output area; minimal edge extension aligns them to the encoder grid. Input thumbnails and region editing reflect the effective snapshot. Output resolution is independent: the layer-size button beside Size matches the selected input before this limit, including its canvas crop, or the active document layer if no input is selected. The existing reset button matches the first edit input or active canvas. Both round output dimensions to model alignment.

An isolated rendered layer includes its mask, opacity and effects against transparency. Backdrop-dependent blending may differ from the complete canvas: use **Visible Canvas** when that context is important. Edit Current Layer uses native raster pixels so existing layer effects, masks and transforms are not baked twice when replacing content.

A live draft appears beneath Generate while inference runs, then shows the latest completed image. Click it to inspect the image without changing tabs. Completed variations also appear in Preview; finishing a job leaves the current tab selected. Applying a result, changing modes or closing its original document does not remove it from history. Previews are listed in generation order. You can discard any completed variation while another is generating. History is kept in memory and private temporary files until discarded or Vulkana exits; it is not saved in the project. Hover over a preview for its magnifier, then click to inspect its full-resolution image in an owned lightbox, with Fit, 1:1, wheel zoom, middle-button pan and a draggable comparison divider. The expanded preview keeps canvas edits blocked and owns keyboard input, so browsing with Left/Right cannot move layers underneath. The centered preview blocks interaction with the editor underneath, including its Layers panel. Comparison uses a searchable chooser attached to the left of the preview, independent of the dock layout. Its thumbnails reuse the Layers panel’s cached content icons without rendering them again. Its Apply controls offer the output destination; applying closes the preview and performs the existing undoable operation. Preview-tab details retain the configuration, model filename, output resolution, copyable actual seed and completion date for each image, independently of current controls. Regenerate retains earlier successful variations while the next job runs. Select an earlier variation to return to it, or use Previous/Next in the expanded preview to cycle through the history. During generation, a separate Pending item shows the live approximate preview when available, even with completed results in history. It can be inspected and browsed with Previous/Next, but cannot be applied as a final result. Cancellation removes the pending item; completion adds the finished image. Earlier results stay intact.

**Loaded models**, directly below the configuration controls, lists the configuration actually retained by the worker and its component files. Selecting or editing a saved configuration does not change this list until that configuration loads. Use the entry's **Unload** action to release its memory and runtime caches; generated images are kept. Unloading is disabled during active work.

The worker retains one configuration at a time. Its diffusion model, text encoder, VAE and optional vision component belong to one context and unload together; the current engine cannot unload these components independently. Switching configurations releases the previous context before loading the next one.

**Resolution Selector**, below Size, calculates dimensions from an aspect ratio, an approximate megapixel target (1 MP = 1024 × 1024) and a rounding multiple. Manual size edits select Custom; changing the target or multiple in Custom leaves those dimensions untouched. **Multiple** defaults to **Auto**, using **8** for Krea 2, **16** for Flux Klein, **32** for Qwen Image 2.1, or **64** for Stable Diffusion. Explicit multiples remain available. The selected model's requirement is displayed in the selector. Incompatible dimensions are rejected before model loading or generation, with nearby valid sizes suggested. Changing models recalculates a named preset when Auto is selected; Custom dimensions and explicit multiples remain unchanged. Worker discovery and mode switches preserve the chosen dimensions. The explicit size-matching buttons still round up to model alignment when necessary.

Preview dimensions are shown in the expanded preview. Reference encoders may internally resample inputs, and the model can recompose details even when input and output dimensions match. **Compare with layer** opens a searchable list of compatible raster, text and shape layers in the current document, including hidden layers. Choose a layer to capture its isolated rendered appearance, including transforms, masks and effects. Groups, adjustment layers and rasters without pixel data are excluded. The left side shows the chosen layer and the right side shows the result. Choosing a comparison never changes the document or its active layer; turn comparison off using the same button or the chooser’s close icon.

- **Canvas Alignment** preserves document positions and scale, including off-canvas origins and transparency around smaller layers. An edited result uses its captured input's placement; a new generation starts at the canvas origin.
- **Center Alignment** aligns image centres at native pixel sizes, without rescaling either image.
- **Fit to Result** proportionally fits the reference within the generated image, preserving aspect ratio and transparent margins.

Zoom and middle-button panning remain available while dragging the divider.

### Apply results

- **Add as New Layer** is the default and uses one undoable layer insertion.
- **Replace selected layer** targets the current raster layer in the active document. Groups and other incompatible selections are disabled. Replacement retains the destination layer's transform, mask, blending and effects and is undoable.
- **Open new document** creates an independent document from the selected result.

For Generative Fill, Vulkana retains the original soft selection and composites through it when applying. Replacement and new-document output preserve pixels outside the selection exactly; a new result layer is transparent outside it. A new layer overlays its existing backdrop, so it cannot erase transparency in the layers below. Replacement is confined to the target raster's existing pixel grid; new-layer output can cover selected regions elsewhere on the canvas.

Fill uses the full canvas dimensions for its starting image and selection mask, including margins beyond the selected layer. In temporary inference inputs, fully transparent selected canvas pixels receive an opaque white placeholder so RGBA-aware models do not interpret those missing areas as transparency to retain. This also applies to matching visible-canvas references from the same document. Opaque and partly transparent content, unselected pixels, other references, and the original document are retained; final application still uses the original soft mask and the generated alpha.

Outpainting quality remains model-, prompt-, resolution- and seed-dependent. The supported Qwen and FLUX profiles use general image editing with a sampling mask, not dedicated mask-conditioned inpainting. The mask restricts denoising but does not itself tell the model what the words “selected area” mean. Describe the intended continuation and its location explicitly; region guidance can supply additional semantic placement hints. Use Add as New Layer or Open new document for margins outside the original raster's bounds. Visible-canvas references are snapshots taken when added; re-add one if the source canvas has changed.

Replace selected layer captures and validates the destination at Apply time, preserving its current pixels outside a captured Fill selection; it does not use an unrelated current selection. The command checks that revision again before applying. Results do not depend on the original layer remaining available. Editable text/shapes are not automatically flattened: use their rendered appearance as an input and apply to a new raster layer. The Edit Current Layer menu entry initially captures a raster layer, but inputs can subsequently be replaced with other image snapshots.

For a canvas-cropped Edit input, replacement uses the captured document placement and preserves destination pixels outside that rectangle. The active selection remains unchanged, and the operation supports undo/redo. Add as New Layer retains the result's captured placement.

## Supported models and regions

**Qwen Image 2.1 Turbo:** diffusion weights, Qwen3-VL 8B text encoder and Qwen Image 2.1 VAE. For image editing, vision weights may be embedded in the text encoder or supplied as a separate matching projector. Leave Vision empty for a combined encoder; a text-only encoder, such as a split GGUF, needs its projector. The backend checks the loaded tensors rather than inferring this from INT8/GGUF filenames. Recommended: 8 steps, Euler, CFG 1, exact Turbo sigma schedule. The base Qwen profile is available but has not been independently exercised with base weights.

**Flux Klein:** choose the matching generation and variant. Flux 2 uses its diffusion weights, a matching Qwen3 text encoder (4B for Klein 4B, 8B for Klein 9B), and the FLUX.2 VAE. Flux 3 uses its diffusion weights, Qwen3-VL 4B and the FLUX.2 VAE. Neither needs a separate vision projector. Both support image references; native structured region guidance is available for Flux 3 only.

| Variant | Steps | CFG | Sampler | Scheduler |
|---|---:|---:|---|---|
| Flux 2 base | 20 | 4 | Euler | Flux2 |
| Flux 2 distilled | 4 | 1 | Euler | Flux2 |
| Flux 3 base | 20 | 3.5 | Euler | Discrete |
| Flux 3 distilled | 16 | 1 | Euler | Discrete |

These are starting defaults, not fixed limits. Flux 3's default flow shift is 6.93. Up to ten image references are supported; Qwen's worker profile currently allows eight.

**Stable Diffusion:** select a complete SD 1.5 or SDXL-compatible `.safetensors` checkpoint, including compatible Illustrious derivatives. The engine detects the architecture and uses embedded CLIP/text encoders and VAE. Leave **VAE override (optional)** blank to use the embedded VAE, or supply a compatible external VAE. Encoder-less diffusion files require a complete checkpoint instead. This family supports text-to-image only; image inputs, Edit Layer, Generative Fill and region guidance are unavailable.

Stable Diffusion reuses the prompt, negative prompt, seed, resolution and LoRA controls. In **Advanced generation**, adjust Sampler, Scheduler, Steps and Guidance (CFG); defaults are DPM++ 2M, Karras, 25 steps and CFG 7. The worker advertises sampling options from the pinned engine. Flow shift is disabled because it does not apply to this family. Checkpoint architecture is returned in `effectiveSettings.architecture`.

Choose an image in the **Regions** dropdown, or double-click an input image, to draw, move, resize, label or remove its rectangular regions. New labels are **Region1**, **Region2**, and so on. Regions belong to that input snapshot: reordering images moves their regions with them, and removing an image removes only its regions. Image numbers follow the current input-list order.

Use **`<Image1>`** for an input image and **`<Image1/Region1>`** for one of its named regions. With exactly one input, **`<Region1>`** is also supported. Region names match the labels in the region editor. With multiple inputs, qualify region names with their image; the active UI selection does not change alias meaning. Use **`<Output/Region1>`** for output-layout regions. Existing unbracketed `Image1/Region1` prompts remain accepted for compatibility, but bracketed aliases are preferred. Input bounds use the effective snapshot dimensions after canvas cropping and input-size limiting.

**FLUX.3:** Vulkana translates image aliases to zero-based `<ref_image_N>` references and region aliases to unique `<obj_N>` references. The caption ends with the native JSON box table, using `[y_min, x_min, y_max, x_max]` on a 0–1000 grid. With one input and no output-layout objects, rows retain the established `id`, `bbox`, `desc` format. Description defaults to empty. Multiple-input source rows use `from:"ref_image_N"`, `src_bbox`, `tgt_bbox` and `desc`; without a linked destination, `tgt_bbox` is null. Ordinary output-layout objects use `bbox`. Input labels identify regions, rather than inventing subject descriptions. Reference padding is accounted for when mapping box coordinates. Full original input snapshots are sent; regions do not create additional image references by default. [Upstream box schema](https://github.com/black-forest-labs/flux-3-image-klein/blob/main/src/flux3/bbox.py).

For FLUX, each region has a separate **Description (optional)** field. The name identifies the prompt alias; only the text entered in Description supplies semantic guidance. Vulkana does not infer descriptions from the region name or image. Target locations have no independent object description.

With multiple inputs, use **Output scene** to choose the destination backdrop. `<Scene>` expands to that input's current native reference number; inputs are not reordered and no scene instructions are added to the prompt. This choice does not change the initial image used by Edit Layer/Generative Fill or the destination selected when applying a result.

To link a transfer:

1. Open the source image's region editor and select a region. Enter a description if useful.
2. Choose **Destination → New target…**, or select an existing output-layout box. The existing owned preview opens over the output scene; draw the desired destination. **Edit…** reopens an assigned target.
3. Write the instruction using `<Scene>` and the source alias, for example: `Use <Scene> as the output scene. Transfer <Image2/Region1> into its target position.`

A transfer produces one native object row containing the source and destination boxes. Linked locations are marked **Target** and never become separate objects. `<Output/Name>` for a linked target expands to the target position of its source object. Unlinking or deleting a source keeps the target as a reusable location, not an object to generate. Delete unused targets through the region editor.

Destination coordinates belong to the requested output frame. The scene is fitted proportionally into that frame as a drawing guide, with transparent margins when aspect ratios differ; it is not stretched or sent as an extra reference. Changing output dimensions retains relative box positions. Sources continue to use their own cropped/resized reference coordinates, including encoder padding. Stable IDs preserve links across renaming and reordering. Removing a destination or scene, or cropping a linked source entirely away, blocks generation until the missing relationship is repaired or unlinked.

**Extra crop guidance (experimental)** is a Vulkana option for FLUX, off by default. It supplements native boxes with cropped detail references and short links to their object IDs. It can change results, increases memory use, and consumes one reference slot per input region. The setting is explicit and persists across sessions. Native boxes alone do not consume reference-image slots.

Fully native FLUX captions containing `<ref_image_N>`, `<obj_N>` and a trailing JSON box table can be used directly after clearing editor regions. Their text is passed through without adding another box table. Supplying both a hand-written table and editor regions produces an explanatory error. No crop, visual annotation, prompt enhancement, or separate vision encoder is implicitly added for native FLUX regions.

**Qwen:** input regions retain their crop-reference path. The worker resolves bracketed aliases to the supplied image/crop numbers and supplies a short source relationship. Each input region consumes one extra reference slot; output-layout regions also require a numbered annotation guide. FLUX-specific box syntax is not sent to Qwen. The selected model's total reference limit includes original inputs and all optional/additional images: eight for Qwen and ten for FLUX.

Regions are semantic guidance, not pixel-preservation masks. Cross-image object transfer remains model- and prompt-dependent; inspect the preview before applying. A successful single-image box edit does not establish reliable multi-image transfer. Editing both source images requires separate results, and original document layers are not changed before Apply.

Region guidance is experimental and never replaces the pixel-preservation mask. Models may miss a box or alter unrelated areas during generation; Generative Fill's application mask provides the pixel guarantee. Prompt enhancement is not implemented. Detailed prompts can be supplied directly; FLUX.3's reference workflow recommends expanded captions for quality. [Upstream guidance](https://github.com/black-forest-labs/flux-3-image-klein#prompt-upsampling).

## Build and test

The ordinary Vulkana build includes the UI and lightweight QtCore client. It does not require the inference engine or CUDA. The module can also be built and tested independently:

```sh
cmake -S src/diffusion -B build/diffusion-host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/diffusion-host
ctest --test-dir build/diffusion-host --output-on-failure
```

The worker is a separate optional build. Use an existing stable-diffusion.cpp checkout at the revision in `src/diffusion/engine-lock.json`, with its recorded GGML and CUTLASS submodules initialized. Apply the recorded [engine patches](src/diffusion/patches/README.md) before configuring. CMake verifies the revisions, patch checksums and applied patches; it downloads nothing and does not modify the engine checkout.

The current baseline is `fc350be2ceeeb859bcc201ac435591ed7d46821d`. It includes Krea LoRA name-matching fixes, corrected FLUX scheduler token counts and reduced temporary GPU buffers. Scheduling corrections can change FLUX results for an identical seed; saved generation settings remain unchanged.

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

ComfyUI INT8 ConvRot Safetensors with groups 1024 and 4096 are supported on CPU and CUDA through a recorded engine patch. Weights remain INT8. Unsupported group sizes or widths incompatible with the group are rejected while loading, with `unsupported_model_format` identifying the file and tensor. The worker remains available to load another configuration. This support does not imply compatibility with every quantization format or with arbitrary model architectures; GGUF remains an alternative for supported models.

FLUX.3 base and distilled checkpoints can use fused `qkv` projections or separate Q/K/V projections. The engine detects the layout automatically, including fused ComfyUI INT8 checkpoints, for generation and reference editing. An unrecognized architecture produces a recoverable error naming the diffusion weights file.

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

`status` responses and `state` events include `loadedModels`, an array of resident contexts with their `contextGeneration`, `profile`, `files`, `backend` and `effectiveSettings`. The array is empty when unloaded and currently contains at most one entry. Consumers must scope context identities to the worker process, since generation counters restart with a new worker. Loading failures may retain the previous context or leave none, so use reported state rather than assuming the requested configuration became resident.

Events report `state`, `progress`, `preview`, or a fatal protocol error. Progress has `scope:"engineOperation"`: counters may restart between tensor loading and sampling and are not a single overall job percentage. Preview events carry `jobId`, `path`, `step`, `width` and `height`. Previews are approximate latent projections; final images use the VAE. The client ignores expired previews after cancellation and checks that output paths remain within its work directory.

Generated PNGs are independently owned files under `job-<id>/`. Result objects include `images[].path`, dimensions/channels, engine generation metadata, profile, effective resource settings, context generation number and timings. Timings distinguish model construction, engine execution and PNG output; `engineStagesMs` provides conditioning/encoding/sampling/decoding durations reported by the pinned engine. Lazy weight preparation also contributes to the first generation's time. Cooperative cancellation removes that job's partial outputs. Forced process termination can leave partial files; the caller owns cleanup of its private work directory.

## LoRAs

In **Configuration → Model files → LoRAs (optional)**, add local adapter files and give each a friendly name, default strength and enabled-by-default state. Save the model configuration to retain the list. The section starts collapsed between Vision and Backend. Strength ranges from −4 to 4, with 1 as the default and 0 producing no effect.

**Generate → LoRAs**, below Negative prompt, shows only entries from the current configuration. Toggle entries and adjust strengths for the next job. The header shows the enabled count, and the reset icon restores configuration defaults. These overrides last for the editor session and follow their configuration across mode/configuration switches. Saving a configuration always stores its configured defaults. Generation overrides are never written into the saved defaults. An empty list hides the Generate section.

Enabled, nonzero adapters are passed together to the native generation API. The complete effective set is supplied on every job, including an empty set when all are off. Runtime application keeps base weights intact, allows changes without a model reload and invalidates conditioning caches when necessary. Additional adapters can increase memory use and inference time. Enter any recommended trigger words in the prompt yourself.

Choose adapters trained for the selected model family and compatible variant. A missing file is rejected before inference. With the pinned engine, Vulkana checks runtime tensor-application reports and rejects results from adapters that failed to load, matched no tensors or skipped incompatible tensors. The error names the affected entry. Such failures release the model context for a clean reload on the next job. Existing successful previews remain available.

Saved configuration entries contain `id`, `path`, `name`, `strength` and `enabled`. The editor sends only the effective adapters as optional `generate.loras` entries containing `path`, `name` and `strength`. Model `load` parameters exclude the adapter list. The worker advertises `loraParameters:true`; an older worker produces an update-required message. Duplicate enabled paths are rejected, and strengths must be finite and within −4 to 4.

```json
{"loras":[{"path":"/models/portrait-style.safetensors","name":"Portrait style","strength":0.8}]}
```

## Model profiles and resource settings

The shared profile registry declares supported modes, image references, region guidance, negative prompts and sampling defaults. The panel disables unavailable modes and controls. Selecting a generation-only model switches the panel to Generate. Existing input images and regions stay available for switching back to a compatible model, and are omitted from generation-only requests. The worker also validates these capabilities before inference.

**Krea 2 Turbo** supports text-to-image generation. Configure its diffusion weights, a compatible Qwen3-VL 4B text encoder and the Wan 2.1 VAE. Recommended starting settings are **1024 × 1024, 8 steps, Euler, Simple scheduler, Guidance 1 and Flow shift Auto (1.15)**. Dimensions must be multiples of 8; the engine pads and crops its internal transformer patches without changing the output size. The Simple scheduler reproduces the published Turbo timestep spacing at eight steps. Krea's reference implementation uses CFG 0 to disable guidance, which corresponds to Guidance 1 in this worker. Higher guidance and alternative sampling settings remain adjustable for experimentation. Negative prompts become available above Guidance 1. [Official Turbo configuration and sampler](https://github.com/krea-ai/krea-2).

Krea 2 Turbo's editing, image-input and region controls are disabled. Input files, initial images, masks and spatial requests are rejected by the worker for this profile. Generated results use the usual preview, comparison and undoable Apply workflow. Krea 2 Raw is not included.

The profile registry is `src/diffusion/profiles.json`. Qwen Image 2.1 Turbo has been exercised with local GGUF components. Base Qwen needs independent validation with base weights. Flux 2 base/distilled (9B), Flux 3 base/distilled and Krea 2 Turbo have been exercised with GGUF components. Stable Diffusion has been tested with a complete SDXL-derived checkpoint; SD 1.5 uses the same engine loading path but has not had separate inference validation. The engine checks the loaded architecture against the selected family; Turbo/base and distilled/base variants still require the user to choose the matching profile. A GGUF filename alone does not establish compatibility.

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

For `profile:"stable-diffusion"`, `files.diffusion` is the complete checkpoint and is the only required model file. `files.vae` is an optional override; `files.textEncoder` and `files.vision` are omitted. Component-based families continue to require diffusion, text encoder and VAE paths.

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
| `referenceImages` | Ordered local images, up to the profile limit (Qwen 8, FLUX.3 10). Qwen requires vision weights, embedded in the text encoder or loaded from a separate projector. |
| `referenceMaxSize` | Longest-side reference limit, 64–8192. Omitted or 0 retains the engine's automatic sizing for existing protocol callers. Applies to full references, region details and guides; does not change output or initial-image size. |
| `initialImage`, `mask`, `strength` | Optional image-to-image input, matching-size grayscale mask, and strength 0–1 (default 0.75). Mask requires initialImage. |
| `vaeTiling`, `vaeTileSize`, `vaeTileOverlap` | false / 256 / 0.25; tile size 64–2048 in multiples of 32, overlap 0–0.5. |
| `preview`, `previewInterval` | true / every step by default; latent projection previews. |
| `flowShift` | 0 selects the engine's model default; positive values override it. FLUX.3's default is 6.93; Krea 2 Turbo uses 1.15. |
| `eta` | -1 selects the sampler default; non-negative values override stochastic sampler noise. |
| `cacheMode`, `cacheReuseThreshold`, `cacheStart`, `cacheEnd` | Experimental `disabled` / `easycache` / `ucache`; defaults disabled, 0.2, 0.15, 0.95. |
| `spatial` | Optional output-layout schema `{version:1, regions:[{x,y,width,height,label}], annotationImage?:path}`. Version 2 additionally accepts `inputRegions:[{image:1, regions:[...]}]`, where `image` is a one-based entry in `referenceImages`, and optional FLUX-only `cropGuidance:boolean` (default false). Use `regions:[]` when there is no output layout. Bounds are normalized, finite and inside the relevant image. Qwen requires an annotation image only for output regions. |

The Turbo profile supplies its exact eight-step sigma schedule. Select `schedule:"auto"` or provide a custom schedule to experiment with another step count. Reference images retain their own dimensions; initial images/masks must match output dimensions.

The engine's generic latent mask uses binary coverage; it is not model-native inpainting. White permits regeneration and black preserves the initial latent region. It does not guarantee pixel-exact protection through a VAE. Editor integration must retain the original soft selection and composite the result through it when strict preservation is required. Reference editing can alter unrequested areas. Raw generated alpha is preserved, not thresholded or replaced.

The worker advertises `spatialVersions:[1,2,3]` and `flux3NativeRegions:true`. The editor rejects a legacy worker for FLUX instead of silently falling back to crop guidance. Schema 3 adds FLUX region `id`, `description`, source `targetId`, output `targetOnly`, and a one-based `outputScene` input number. Source `targetId` refers to an output region ID marked `targetOnly:true`. Validation rejects duplicate IDs, missing targets, unsupported roles and invalid scene indices before inference. Schemas 1 and 2 remain accepted. Spatial validation also checks input indices, unique group ownership, region names and the effective reference budget. FLUX results include `fluxRegionRows` and `fluxRegionBindings` linking editor names to native object IDs. The final caption is also available in generation metadata. `inputRegionReferences` reports actual additional crops for Qwen or opt-in FLUX crop guidance, including the source image, detail-image index and crop rectangle. The pinned engine receives native FLUX boxes through its existing caption input; no engine patch or separate bounding-box tensor is required.

`hello.referenceSizeLimit:true` advertises reference sizing support. Limited references are proportionally resized, edge-extended to the encoder grid and passed with automatic VAE reference enlargement disabled. Qwen's separate vision path also receives the longest-side limit. Results report `referenceSizes` with the submitted and encoder dimensions. An older worker produces an update-required message instead of silently ignoring the limit. Smaller inputs reduce reference-processing cost, but model weights, output resolution and reference count still contribute to memory use. Semantic object targeting remains dependent on the model, prompt and reference composition; use Generative Fill when pixels outside a selection must be protected.

## Prompt presets

`PromptPresets` supplies Generate Image, Generative Fill, Replace Background and Remove Background templates. Presets describe intent; they do not grant a model capabilities it lacks or establish a selection by themselves.

`PromptPresetStore` supports custom creation, update, removal and reload. Its default file is `diffusion/prompt-presets.json` under Qt's application-data directory. Writes use `QSaveFile`; invalid input, duplicate IDs/names and malformed files leave existing data intact. Built-ins cannot be overwritten; save a custom copy to customize one.

Templates accept `{instruction}`, `{background}`, `{subject}`, and `{style}`. `{{` and `}}` produce literal braces. Expansion is nonrecursive and cannot execute commands. Selecting a preset in the editor inserts an editable example without opening a parameter prompt. Example values replace template variables, and the inserted text can be changed or undone normally. The lower-level template API still validates explicitly supplied variables. Presets store only prompt/negative-prompt templates and task descriptions, independently of model paths and resource settings.

## Testing and distribution

Run the deterministic host and UI tests with `ctest --test-dir build/release -R imageeditor_diffusion --output-on-failure`. They cover process lifecycle, validation, snapshots, undo, soft selections, preview navigation, comparison, reversible input cropping, region remapping, size-limit persistence and cropped replacement placement. Worker tests cover the spatial protocol and bounded reference resizing, including alpha and edge extension, native FLUX box formats, reference/object/scene aliases, source-target pairing, description escaping, invalid links and explicit crop-guidance selection. UI tests cover rename/reorder/crop stability, output-aspect mapping, destination drawing/cancellation and native keyboard/IME entry. The opt-in real-inference smoke executable described above requires user-provided models.

Document transactions stay in the editor; the worker receives immutable files and returns independent images. Input capture and Apply compositing use the editor thread, so unusually large images can pause briefly during those steps. Inference and model loading remain asynchronous.

Linux CUDA is the supported inference target. Windows inference, Vulkan runtime operation and packaged worker distribution require further validation. Building the editor does not build or bundle an inference worker. A worker distribution must include the applicable engine, GGML and linked dependency notices and comply with the selected backend runtime's redistribution requirements. Model weights are supplied separately by the user.
