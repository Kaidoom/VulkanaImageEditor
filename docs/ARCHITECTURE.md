# Architecture

Raster interaction bounds are cached per surface revision in 64-pixel tiles.
Only dirty tiles are rescanned; masks, layer visibility, opacity and effect
padding do not redefine raw content bounds. Storage origins and effect frames
remain distinct from that interaction rectangle. Ordinary brush strokes retain
the incremental journal path until storage must grow, then use the existing
regional COW surface and a single storage/pixel history action. Gesture mapping
stays anchored to the original pixel frame; left/up growth does not rebase
crops, masks or gradients. The native canvas snapshot is rebound only on a
storage-geometry change, not for every dab.

## Memory readout

The status bar samples whole-process resident RAM and cached layer-texture bytes
once per second. RAM includes all tabs/history/CPU caches; the GPU-cache figure
is not total VRAM. Hover details include reserved upload staging, whose backing
may overlap resident RAM. The figures are not additive. Sampling reads OS and
existing renderer counters only, pauses while hidden/minimized, and never
requests a canvas frame or walks pixel data. Narrow layouts prioritize RAM.

## Performance preferences

Preferences → Performance stores resource policy under
`preferences/performance` in per-user QSettings, never in projects. Auto keeps
the established defaults: 256 MiB of undo retention per document, 512 MiB of
derived document caches per editor window, and 512 MiB of layer textures per
renderer. Custom memory values are bounded to 64–8192 MiB. These targets do not
cap process RAM, total VRAM, source pixels, operation scratch or active inputs.
Import/export and tool-specific admission limits remain independent.

Memory policy is applied to existing editor windows only after successful Apply;
new documents inherit it. Reducing undo retention requires confirmation, updates
the budget without deleting existing undo/redo, and trims at the next normal
history publication. The latest command is kept even if it exceeds the target.
No document revision, saved checkpoint, selection or view changes on Apply.
Clear Inactive Caches is a separate immediate action: it drops inactive tabs'
derived caches and retained preview results, then requests inactive GPU texture
eviction through the normal frame-fence deferral path. It does not clear history,
source pixels or required active textures. Worker-held immutable data lives until
its owning work completes. Lower targets/clearing may make later tab activation
recompute or reupload those disposable resources.
An explicit GPU clear or target reduction drains at most one extra frame-slot
cycle to reclaim retired textures promptly, then returns to idle rendering.

GPU and thread choices use an immutable startup snapshot and require a restart.
The core parallel executor supports an automatic or 1–8 participant limit,
including the calling thread, bounded by detected hardware capacity. Nested work
stays serial, interactive brushes retain the nonblocking busy-pool fallback,
and no pool is resized while work is live. Object Selection's separate CPU
inference sessions support Auto (half the logical CPU count, capped at six) or a
1–6 thread limit, also hardware-bounded. These are not a cap on all application
threads; job coordinators, import workers and the renderer remain independent.

The GPU picker lists detected devices and their eligibility using best-effort Vulkan
device UUID plus vendor/device IDs, never a saved enumeration index or name
alone. Auto retains the dedicated/integrated/software preference order. A missing
or no-longer-compatible saved identity falls back to a compatible device.
Adapters whose device/initial swapchain creation fails are excluded from further
startup attempts and the next compatible candidate is tried; this covers hybrid
drivers that advertise presentation support but cannot create that surface.
On Wayland, previously unverified explicit choices and Auto candidates present
a tiny cleared swapchain image on an unmapped native surface in an isolated
process using the same executable and temporary settings. The helper never
shows a desktop window, but still exercises compositor buffer import and commit.
Successful results are reused across launches for the same GPU/driver inventory,
Qt/kernel version, display configuration, relevant
loader environment and presentation policy. Ordinary restarts and cold shader
caches do not rerun the helper. The helper shares the canvas's device, swapchain format,
color-space policy and synchronization path, but never initializes artwork
shaders. Cold driver shader compilation happens in the real canvas after
verification and cannot trip the compatibility deadline.
The compatibility-check wait processes GUI events without a separate progress
popup. Once a device is verified, the canvas compiles its compute compositor
pipeline on a worker. The GUI polls completion without blocking. An animated
initialization card is installed before the main window is shown, on both warm
and cold starts, and remains in place until the first canvas frame is presented.
The empty-workspace actions remain hidden behind this startup gate. Command-line
document imports wait for it too, so import-review dialogs cannot bypass it.
All startup dialogs and update notices wait for the first presented canvas frame,
including the interval before shader compilation starts and when using Auto.
The initialization card has no cancel action:
shader compilation cannot safely be interrupted. During compilation, the window's close control
immediately changes the card to “Aborting…” and requests shutdown after the
driver call finishes. Underlying startup dialogs cannot dismiss that card, and
normal startup presentation does not resume during shutdown. Device/layout
handles stay alive until the worker is joined, including when the native surface
is recreated during initialization.
The helper waits for presentation and compositor confirmation, cancelling its
deadline when a frame has been queued. The parent bounds each attempt to 15
seconds. A timeout or inability to launch the helper is inconclusive, retried
once, then stops startup with an explanation without changing the preference.
A definite rejection tries the next ranked candidate, which must pass its own
probe before use. This also gates replacements after device/swapchain creation
fails in the real canvas. No available verified device means startup stops;
there is no unchecked Auto fallback. Only successful checks enter the bounded
16-entry settings cache; rejection and inconclusive results are not persisted.
An initialization marker is cleared after the first real canvas presentation
is confirmed, or on intentional application exit. An interrupted startup or
observed device/swapchain initialization failure invalidates that configuration,
so a cached result cannot cause repeated unchecked crash loops. Changes to the
WSI contract must bump the cache policy revision in `GraphicsDeviceProbe.cpp`.
Exclusion affects rendering eligibility, not the device inventory: detected
adapters remain listed by name with a disabled startup-check/unsupported status.
After successful presentation, the working device becomes the saved selection
and the process's resolved choice. One app-owned startup information card names
the failed and replacement GPUs; OK resumes the normal startup flow. Preferences
has no persistent fallback warning. Recovery preserves other resource settings
and never overwrites a newer explicit GPU choice made while startup was pending.
Changing this selection does not move Object Selection inference onto the GPU.
The `imageeditor_performance_tests` suite covers ordered fallback, all-device
rejection, timeout retry, unchanged preferences and cache invalidation. When Wayland is available,
`ctest -R 'wayland_gpu_' --output-on-failure` also checks cold-cache startup,
cross-process cache reuse, an unmapped helper, startup-dialog ordering,
responsive GUI heartbeats, close requests and native-surface recreation,
exact-device rejection, missing-device fallback, helper timeouts and a missing
helper. The cold-cache checks disable the
NVIDIA/Mesa shader disk caches for that child process only; they never delete
caches or modify global driver settings. Native checks use temporary preferences.
For manual startup testing, close Vulkana and launch its executable with
`--cold-cache`. The flag sets `__GL_SHADER_DISK_CACHE=0` and
`MESA_SHADER_CACHE_DISABLE=true` before Qt initializes, including for GPU probe
children. This bypasses NVIDIA/Mesa shader disk caches for that process tree;
it does not delete caches, clear document data or save a preference. A normal
launch uses the normal cache policy. Shader compilation can make this diagnostic
launch noticeably slower.
Performance usage polling runs only while the page is visible, uses existing
renderer counters and document accounting, and never requests a canvas frame.
The displayed cache totals are not additive with whole-process resident RAM.

## Document compositing

Document composition is selected by `CanvasSpec::compositingSpace`, defaulting
to the historical linear-light path. PSD opening opts into encoded sRGB. The
same `BlendMath.inc` equations serve CPU output and Vulkan, retaining linear
premultiplied input/output so filtering, adjustments and texture storage keep
their existing contracts. Group/style opacity and clipping-container restoration
use the selected space too. Snapshots and frozen documents retain the setting;
GPU/thumbnail/adjustment-input cache keys include it. Source RGBA uploads remain
reusable. Persistence and compatibility are specified in `VULKANA_FORMAT.md`.

## Bitmap layer masks

Raster, text, shape and adjustment layers can own one immutable tiled R8 `LayerMask`.
Its pixel frame is layer-local and moves with the layer. It is separate from
the document selection and from captured adjustment/filter masks. Coverage is
applied to the adjusted, filtered and styled result after crop, before final
layer opacity/blending; it never changes source pixels during ordinary editing.

`LayerMaskEdit` adapts the existing regional brush/fill transactions to grayscale
coverage, sharing unchanged tiles. Each gesture publishes one undo command.
White reveals, black hides, and gray is numeric partial coverage (not gamma
converted). Selection coverage constrains editing without replacing the selection.
Eraser/Delete hide; brushes and Fill use the grayscale value of their chosen color.
The row thumbnail and status identify the per-document content/mask editing target.
Other destructive pixel tools require returning to the content thumbnail.

Reveal All creates a mask over full local source/styled bounds, with white outside;
From Selection captures the selection in that frame, with black outside.
Later text/shape edits retain the original pixel grid. When editing needs a larger
frame, integer padding extends it to the uncropped local source/style bounds using
the mask's outside coverage, never scaling or resampling existing pixels. Preparing
the brush/fill proxy alone does not change the document; padding is committed with
the actual edit, and cancel/no-op retains the original mask and redo history.
Apply explicitly rasterizes the masked appearance, baking transforms/effects too,
while retaining layer identity, opacity and blend mode; undo restores the typed
original. A disabled mask must be enabled before Apply. Canvas, Pixel Preview,
rendered references, merge and export share the coverage contract. Native PDF text
falls back to raster output when an enabled layer mask cannot be represented.

Copy/Paste layer mask uses a versioned, size-bounded R8 clipboard format with
the original mask frame, source layer transform, enabled state and outside
coverage. Paste keeps the same document-pixel position without resampling:
`destinationLocalToMask = sourceLocalToMask × inverse(sourceLocalToDocument) × destinationLocalToDocument`.
The bitmap is integer-padded to include the destination's full local source/style
frame, retaining off-frame samples and outside coverage. The mapping receives the
opposite integer offset, so neither its appearance nor its position changes. Masks
saved with smaller frames also expand when brush/fill edits require more room.
The pasted mask subsequently moves with its destination layer normally. It is
independent of later source edits and does not consume the document selection.
Selected raster/text/shape/adjustment layers receive independent mask states
sharing unchanged tiles where aligned; containers are not implicit paste targets.
`PasteLayerMasksCommand` preflights all replacements, preserves typed content,
and restores the previous masks, layer selection and mask-editing target in one
undo step. Identical pastes preserve redo and the saved checkpoint. Clipboard
payloads contain no source RGB, layer IDs or document pointers.

## Adjustment operators

`AdjustmentLayer` owns an ordered correction collection, scope and optional mask,
not a source image. `CompositionPlan` defines its lower-stack input and local or
clipping domain. CPU row/point sampling and Vulkan both mix corrected float RGB
with the original while retaining alpha. Scope-owned local isolation persists
while the correction is bypassed and is released undoably with the scope owner.
See [Adjustment layers](ADJUSTMENT_LAYERS.md) for masking, cache keys and exchange
policies. No lower pixels are rewritten for parameter/history changes.

## Module boundaries

- `src/core` owns platform-neutral document data, typed layers, selections,
  geometry, image algorithms and undoable transactions. It has no Qt/Vulkan types.
- `src/platform` provides Qt-based resource paths, desktop startup, single-instance
  IPC and optional runtime service configuration.
- `src/render` owns the native Vulkan canvas, composition, overlays and GPU caches.
- `src/ui` connects Qt panels/tools to the core, performs text/shape preparation,
  orchestrates jobs, and handles project/image I/O.
- `src/app` contains the executable entry point and startup coordination.

PSD conversion stays at the UI/IO boundary: checked binary inspection produces
an immutable source and capability list, the owned review dialog supplies a
plan, and a bounded worker constructs a standalone native document. Publication
pins the runtime destination/revision and uses existing tab or atomic structure
insertion. No PSD descriptors, executable content or external source dependencies
enter `.vulkana`. See [PSD import](PSD_IMPORT.md).

Keep authoritative document content separate from reconstructible render caches
and UI state. Panels and the renderer consume the same document model; they must
not introduce another editable stacking order or save display caches as source.

## Workspace and documents

One `MainWindow` owns a shared workspace and native canvas/device/swapchain.
Each runtime `DocumentContext` retains an independent `EditorSession`, history
and saved checkpoint, project/source paths, export state, layer/pixel selections,
clone anchors and pan/zoom. Runtime document-instance IDs are distinct from paths,
tab indices and serialized layer IDs.

The central activation path settles pending operations against their original
owner, rebinds existing panels/models, and restores the incoming view. Switching
must not copy all pixels, reload a project, replay history or recreate the canvas.
Programmatic panel updates are guarded against emitting edits.

Workspace panels can share a tabbed frame. A middle drop joins the target frame;
top/bottom thirds retain vertical docking, and tab-strip gaps set tab order. The
header moves the whole frame; dragging a tab moves only its original panel.
Tab and docked-panel drags use a compact title chip beside the pointer. Floating
panels can be parked below the workspace while their full header stays reachable.
Ownership changes after pointer release, retaining controls and the native canvas.
Requested visibility is separate from the active tab. Group membership, active
tabs and floating geometry live in UI settings (`window/panel-tabs-v1`), not projects;
`--panel-default` ignores this saved layout without overwriting it. The Layers
`fx` shortcut reveals the selected effect's settings without enabling it.

The default workspace has Layers/tools on the left (370 logical pixels), with
Color/Swatches above Properties/Adjustments on the right (551 logical pixels).
The two right frames share height in a 315:889 ratio, respecting panel minimums;
Color and Properties are initially active. Saved layouts take precedence.
Color and Swatches are separate panels, tabbed together by default. Swatches
retain the existing `editor/custom-swatches-v1` preferences. The shared color
picker edits sRGB through HSV (hue strip, saturation/value field). The Color panel
shows only those interactive fields; fixed-size app-owned color cards also offer
independent alpha and RGB/HSV/hex entry. They use the same workspace presenter as
About, restoring the parent card when opened from Preferences or Export. No picker
creates a separate desktop window. Acceptance/cancellation retains each caller's
existing preview/history policy; the renderer's color math is unchanged.
Screen picking uses the desktop PickColor portal in Wayland sessions (including
XWayland) and Qt screen capture on X11. Custom palettes belong only to Swatches,
not to individual dialogs.

### Screenshot capture

File → Take Screenshot opens an app-owned workspace card with a shared preview
and pixel-aligned crop. Capture minimizes Vulkana, then restores its previous
window state for review. Retake retains the previous image until a new capture
succeeds. Open as New Document publishes an unsaved raster document through the
normal image loader. Add as Layer instead uses the existing undoable layer-import
path, pinned to the originating document; it preserves canvas size and view and
uses the normal refinement-discard guard when needed. Cancelling leaves existing
tabs, history and clipboard unchanged. Cropping copies source pixels without
resampling the preview. Wheel zoom uses the canvas's cursor-anchored mapping and
limits; middle-mouse dragging pans. Reset Zoom fits and centers the image without
changing the crop. Windows capture defaults to a user-adjustable one-second delay.

On Linux, the interactive XDG Screenshot portal owns capture permissions and
the available screen/window/region choices. A generation-scoped request subscribes
before dispatch, handles cancellation and late replies, and times out. Only a
bounded local image returned by that request is decoded; no remote URI is fetched
and portal-owned files are not deleted. A working desktop screenshot portal is
required, including on X11.

On Windows, GDI captures visible pixels from the virtual desktop or the foreground
window at the end of a configurable delay. Active Window excludes Vulkana, uses
DWM frame bounds clipped to the desktop and physical-pixel DPI coordinates, and
does not request hidden/minimized window content. BGRX becomes an owned opaque
image in memory. Protected content, occluded regions and HDR/wide-gamut color
fidelity are subject to the desktop capture API; the result is treated as SDR,
not an HDR or color-managed screen export. No helper executable, network service
or additional runtime dependency is required.

`imageeditor_screenshot_tests` covers crop geometry, stale callbacks, cancellation,
new-document publication and existing history. `imageeditor_screenshot_portal_tests`
uses an isolated D-Bus fixture, never the real desktop. The opt-in Windows test
`imageeditor_windows_screenshot_tests --native` captures only its own separate
fixture window on an interactive desktop. Monitor arrangements, live portal
chooser behavior and protected/HDR content still need desktop-specific testing.
The screenshot widget test's `--native` mode exercises the workspace with Vulkan;
on Wayland it needs real pointer input to authorize restoring a minimized window
and returns 77 if the compositor input prerequisite is unavailable.

Jobs and deferred callbacks must carry their owning document, target identities,
revisions and cancellation generation. Completion cannot infer its target from
the currently active tab. Explicit repair work can finish on an inactive valid
owner; speculative view work pauses/cancels. Closing invalidates outstanding work.
See [document tabs](DOCUMENT_TABS.md) for transfer and close/save policies.

## Layers, history and persistence

`LayerTree` owns bottom-first hierarchy order and stable item identities.
`Document::layers()` exposes the materialized leaf traversal, not a separate stack.
Ordinary containers are pass-through organizational/visibility structures; leaves
retain document-space transforms. Effective visibility includes ancestor gates.
Explicit [clipping groups](CLIPPING_GROUPS.md) use a shared structural composition
plan, with the bottommost direct child's stable coverage and final opacity/blend.

Raster bytes, editable text/shape models, transforms, crop, adjustments, filters
and styles are authoritative. Whole-layer transforms retain source detail and
typed editability. Merge and Rasterize are explicit destructive operations at
document resolution; they must evaluate authoritative sources, not viewport caches.
Merge isolates selected roots over transparency, preserves their relative order,
and consolidates at the topmost selected position without importing unselected
backdrop pixels.

Edits use existing raster, geometry or structure transactions. Admit bounded
allocations and validate all targets before publication. One completed gesture
is one history action; cancellation and exact no-ops preserve redo/checkpoints.
Selection restoration belongs to the operation, not a later panel side effect.
Project writing is atomic; see [the format contract](VULKANA_FORMAT.md).

Large brush dabs share the bounded core worker pool for immutable tip/grain
sampling and independent tile accumulation. Dabs remain ordered; surface reads,
regional writes and history stay on the originating thread. Small dabs, custom
samplers without a concurrency contract, and sampled repair/filter callbacks
use the serial path. Painting never queues behind a busy background worker batch.

## Coordinates and transforms

Use the shared [coordinate convention](COORDINATE_SYSTEM.md): top-left origin,
X right, Y down, with explicit conversions between source pixels, document pixels,
Qt logical coordinates and framebuffer pixels. Never use monitor DPI or zoom to
determine stored raster resolution.

`ProjectiveTransform` supplies a shared 3×3 forward/inverse mapping and a fast
affine path. Explicit corner distortion retains a projective residual through
subsequent scale/rotate/move. Visible side handles use projected-edge midpoints.
Projective resizing follows forward handle trajectories rather than inverse-
projecting a free pointer across a horizon. Invalid support/geometry preserves
the last accepted state; resource limits remain enforced.

Selected-pixel transforms capture immutable native pixels plus coverage and
evaluate remaining source and transformed content as one provisional raw layer.
Effects then run once. Copy-on-write regional storage retains unchanged bytes
and supports pixel-aligned growth without moving crop/mask coordinates. Apply
commits ordinary pixels; Cancel restores the original source and selection.

## Composition and caches

CPU evaluation, Vulkan, Pixel Preview, rendered sampling, export and raster baking
must share alpha, blend, transform and effect semantics. Treat the checkerboard,
rulers, handles and selection outlines as view-only overlays. Pixel Preview caches
the native-resolution document output; pan/zoom alone must not recompose it.

Cache keys include document/content identity and relevant revisions. Retain warm
textures/derived data within bounded budgets; evict reconstructible inactive data
before authoritative pixels or undo history. Retire GPU resources after in-flight
use, without introducing a device-wide wait on every tab change.

Rendering is demand-driven. Inactive tabs do not own render loops, caret timers
or repeated filter evaluation. Pointer changes should not upload unchanged source
pixels. Shared algorithm contracts are indexed in [the documentation index](README.md).

## Input and host integration

The native canvas remains stationary beneath the Qt overlay workspace. Reuse
existing popup ownership, workspace dialogs and cross-panel capture; ordinary
panels are not independent desktop windows. Text/numeric controls retain local
editing keys while unrelated global shortcuts remain available. See
[shortcuts](SHORTCUTS.md) and [single-instance coordination](SINGLE_INSTANCE.md).

Preferences and installed brushes are shared; document-relative targets are not.
Packaged resources use installed/executable-relative paths, never the working
directory or development checkout. Development fallbacks are explicitly gated.
Online services load [optional runtime configuration](../config/SERVICES.md);
missing configuration disables those actions without affecting editing.
