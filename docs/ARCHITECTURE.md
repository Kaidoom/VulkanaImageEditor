# Architecture

## Module boundaries

- `src/core` owns platform-neutral document data, typed layers, selections,
  geometry, image algorithms and undoable transactions. It has no Qt/Vulkan types.
- `src/platform` provides Qt-based resource paths, desktop startup, single-instance
  IPC and optional runtime service configuration.
- `src/render` owns the native Vulkan canvas, composition, overlays and GPU caches.
- `src/ui` connects Qt panels/tools to the core, performs text/shape preparation,
  orchestrates jobs, and handles project/image I/O.
- `src/app` contains the executable entry point and startup coordination.

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

Jobs and deferred callbacks must carry their owning document, target identities,
revisions and cancellation generation. Completion cannot infer its target from
the currently active tab. Explicit repair work can finish on an inactive valid
owner; speculative view work pauses/cancels. Closing invalidates outstanding work.
See [document tabs](DOCUMENT_TABS.md) for transfer and close/save policies.

## Layers, history and persistence

`LayerTree` owns bottom-first hierarchy order and stable item identities.
`Document::layers()` exposes the materialized leaf traversal, not a separate stack.
Containers are pass-through organizational/visibility structures; leaves retain
document-space transforms. Effective visibility includes ancestor gates.

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
