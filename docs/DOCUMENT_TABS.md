# Document tabs

## Product behavior

- One shared workspace/canvas; a normal, closable tab remains visible with one
  document. Tabs have 120–250 logical-pixel natural widths, elided names, full-path
  tooltips, modified markers, an 8 px close-button inset, drag reorder and Qt
  overflow navigation. Same filenames show parent-directory/ordinal qualifiers.
- Tabs occupy a dedicated opaque 33 logical-pixel row between
  the contextual toolbar and the entire workspace, not the canvas overlay.
  Rulers, dock columns and the tool rail retain their original workspace-relative
  top alignment. The row stays the same height with zero/one/many documents.
  Theme → Custom includes independent Active document tab / Inactive document
  tab backgrounds; the full-width row uses Panel / toolbar background. Existing
  custom palettes inherit Control / Surface colors for the new roles until edited.
- New, Open, Recents and command-line files create tabs in supplied order.
  An already-open canonical `.vulkana` path focuses its existing context without
  reloading it or clearing undo. Reopening an ordinary image creates another tab.
  Failed/cancelled input does not replace a document or leave a placeholder.
- At ordinary startup, New Canvas is presented with no hidden editable document.
  Cancel leaves an empty welcome area with New/Open actions. Closing the last tab
  presents the same card; Cancel does not create an unsolicited blank document.
  The explicit development `--skip-new-dialog` path retains its starter canvas.
- Dropping files on the strip, empty workspace or New Canvas starter opens
  documents. While New Canvas is visible, the entire owning application accepts
  local-file drops as Open requests, including the canvas, panels and toolbar.
  Without that starter overlay, dropping on an existing canvas remains Import
  as Layer. Resize and other modal cards do not opt into this routing.
- Ctrl+W closes the active document; Ctrl+Tab / Ctrl+Shift+Tab cycle tabs. These
  use the existing configurable shortcut registry and do not become text input.
- Tab activation retains committed typing and completed tool transactions.
  A live pointer stroke/drag or uncommitted selection preview is cancelled against
  its original document; completed transform/crop/numeric edits are finalized.
  No pointer capture continues into the next document. IME follows the existing
  TextController completion convention.
- Closing an inactive clean tab does not activate it. Closing the active tab
  selects its right neighbor, otherwise its left. Save/Discard/Cancel applies to
  the named owner. Quit/restart preflights every dirty document before destroying
  any; a later Cancel retains all contexts (completed disk saves remain saved).
  Save As rejects a destination owned by another open project.

Inactive tabs activate through `PointerPressPreparation` before pointer capture
and Qt drag initialization. The New Canvas drop handler covers the owner's
widget/native surfaces and opens files after the modal loop exits; remote URLs
are rejected.

## Copy between tabs

Dragging layer rows captures a source-owned typed payload before the 450 ms
intentional hover switch. Moving canvas content over the strip starts the same
copy transfer after rolling back the source move preview. Hover alone makes no
edit. Tab reorder is Qt's separate tab drag, not a layer payload.

Selected ancestors/descendants are normalized, relative stacking and effective
visibility preserved. Folders currently have no inherited geometry, so leaf
document transforms remain valid when detached. Raster bytes are copied; text,
shapes, crop, effects, adjustment/filter settings and immutable masks survive.
Destination IDs are fresh, including container child references. PPI does not
scale content. Destination-only undo removes/restores the entire insertion and
selection in one command. The existing 256 MiB retained-layer history admission
limit also bounds a transfer; exceeding it fails before modifying documents.

A tab drop retains original document coordinates. A canvas drop places the
genuine Move grab point at the destination document point; a Layers-panel drag
uses the combined logical bounds center (origin for empty content). Relative
placement of multiple roots is unchanged. Default insertion is above the target
active item, or at root top when none; dropping in Layers uses its insertion rule.
Closed source/target owners invalidate the payload instead of redirecting it.

## Ownership and resources

`DocumentContext` owns independent content, hierarchy, history/checkpoint,
selection/primary layer, inference evidence, project/source name and metadata,
export settings, clone anchor, measurement, folder expansion and pan/zoom.
Brush assets, colors, appropriate tool settings, shortcuts and workspace layout
remain shared. Runtime instance IDs cannot collide when projects repeat local
layer IDs and revisions.

Activation reuses panels/models; it does not copy pixels or reload files. Effects,
adjustments and opacity edits are finished before target rebinding. Histogram
targets are cleared before binding even when the next layer has the same ID.
Queued thumbnail callbacks carry a session generation. Speculative selection,
filter and view-density work stops when leaving a context. Completed Spot Heal
work may finish on an inactive owner; it cannot repaint a different document or
survive closing its owner. There is still only one repair worker coordinator.

Loading/saving retain the existing synchronous, cancellation-aware owner-thread
progress path with activation excluded during I/O. Export retains its initiating
context through its modal coordinator and immutable codec/write tasks. Neither
completion selects whichever document happens to be current. Update restart
uses the all-document guard and forwards saved project paths.

Resource bounds:

- Disposable text/shape/filter/effect surfaces across contexts: 512 MiB target;
  least-recent inactive caches are discarded first. Active working sets may
  exceed this target within the evaluator's existing per-operation limits.
- GPU source-texture LRU: 512 MiB target; active textures are protected. Closing
  owners queues retirement through the existing frame-safe resource path, not
  a new device-wide idle wait. Shared device/pipelines/swapchain are retained.
- Pixel Preview native outputs: 256 MiB / 16 contexts, plus the existing bounded
  single worker/frozen-source working set. Result keys do not pin disposable
  inactive filter/effect caches. Text layout maps retain at most eight sessions.
- Authoritative pixels and undo histories are **not** disposable caches. Their
  aggregate surface/history charges are reported by `documentMemory()`; there is
  no new global cap that silently erases edits or history. These counters are not
  a process RSS estimate and omit allocator/layout/widget overhead. Many large
  documents can therefore still exhaust available system RAM.

## Tests

`imageeditor_document_tabs_tests` covers tab layout, activation, independent
document state, file opening, closing and shortcut routing.
`imageeditor_document_transfer_tests` covers typed-layer copying, fresh identities,
placement and destination-only history.

```sh
ctest --test-dir build/release --output-on-failure -R 'imageeditor_(document_tabs|document_transfer)_tests$'
```
