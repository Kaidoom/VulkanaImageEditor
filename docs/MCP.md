# Vulkana MCP

Vulkana exposes a local Model Context Protocol (MCP) interface for editing documents in the running editor. It is disabled by default. The interface controls artwork, not application preferences: there are no theme, language, GPU, update, installation or arbitrary script commands.

## Preferences workflow

Open **Edit → Preferences → MCP**. No command-line flags are needed.

1. Click **Start MCP**. The status changes from **Stopped** to **Listening**, then **Connected · Clients: N** when clients connect. The count measures open local connections, not successful tool discovery or running commands. A failed start shows **Error** and its reason; correct the endpoint/problem and click Start again.
2. In **Connection**, inspect the installed bridge command and endpoint, then click **Copy Configuration · OpenCode V2**. Paste the copied JSON into your OpenCode configuration. Vulkana only copies to the clipboard; it never edits client configuration files.
3. Close Preferences before asking the client to edit a document. While Preferences is open, queries work but editing reports the normal dialog blocker.
4. **Stop MCP** disconnects all clients and closes the listener. You can Start again in the same Vulkana process. Reconnect clients after restarting the server.

**Start automatically with Vulkana** defaults to unchecked. Check it and click Apply/OK to enable future automatic starts. Stop affects the current session, not that saved checkbox. Uncheck it and Apply to disable future automatic starts.

**Advanced Settings** contains the optional custom endpoint. Leave it disabled to use the secure automatic per-user endpoint. Editing or applying endpoint settings never interrupts an active client: the change takes effect on the next Start. Copy Configuration uses the active endpoint while the server is running, otherwise the current draft endpoint. Invalid custom settings are rejected before starting/saving.

Start/Stop are immediate session actions, separate from preference drafts. Cancel discards unapplied settings, but does not undo an explicit Start/Stop. If Stop is requested during a synchronous command, the listener stops accepting work and the UI shows **Stopping…** until the current command finishes. Its completed response is flushed when possible, allowing up to one second for unread output to drain; queued commands are not run. This is not cancellation or rollback of that command.

## Build and command-line connection

Build the editor normally. The `imageeditor` target also builds the small `vulkana-mcp` companion executable. Both are installed into the normal binary directory. No Python, Node.js or MCP SDK is required at runtime.

Start the editor with automation explicitly enabled:

```sh
./build/release/src/app/imageeditor --mcp --skip-new-dialog
```

Connect an MCP client's **stdio** transport to:

```sh
./build/release/src/mcp/vulkana-mcp
```

For OpenCode V2, use **`mcp.servers.vulkana`**, `type: "local"`, and a command array. For example:

```json
{
  "mcp": {
    "servers": {
      "vulkana": {
        "type": "local",
        "command": ["/absolute/path/to/vulkana-mcp", "--socket", "/run/user/1000/vulkana-mcp/editor.sock"]
      }
    }
  }
}
```

Use the paths copied by Preferences, not the illustrative user ID/path above. OpenCode V2's default classic handshake is compatible with Vulkana's existing `2025-11-25` protocol; do not force the newer discovery-only protocol. See [OpenCode V2 MCP configuration](https://opencode.ai/v2/docs/mcp-servers).

The client starts only the bridge, not Vulkana itself. Enable the server in Preferences, via automatic startup, or with `--mcp`. Forwarding a command-line launch to an existing editor does not silently enable automation; start it through Preferences or restart with the flag. `--skip-new-dialog` avoids startup canvas/language/integration prompts. Explicit `--mcp` starts the configured endpoint; `--mcp-socket` overrides it for that launch without saving the override. Ordinary automatic-start failures leave the editor usable with an Error status; an explicit CLI startup failure returns a failing exit status.

On Linux, the default endpoint is `vulkana-mcp/editor.sock` beneath the user's runtime directory. Preferences displays the effective endpoint. A custom endpoint can also be selected explicitly:

```sh
imageeditor --mcp --mcp-socket /absolute/private-directory/editor.sock --skip-new-dialog
vulkana-mcp --socket /absolute/private-directory/editor.sock
```

The socket's immediate parent must belong to the current user and have permissions `0700`. A missing immediate parent is created with those permissions if its parent already exists. Symlink parents, relative/overlong paths, and conflicting endpoint paths are rejected. Existing socket paths are never removed or replaced automatically; verify that no editor is using an endpoint before manually removing a stale socket after a crash. Stop/normal shutdown removes only the endpoint resources owned by that listener; a replacement belonging to another listener is preserved.

On Windows, MCP uses named pipes with current-user access control. Custom names use letters, numbers, dots, underscores and hyphens, optionally prefixed with `\\.\pipe\`; remote pipe paths are rejected.

The status bar also displays the connected client count and provides Stop MCP. Disconnecting a client, ending its stdin stream or closing its bridge does not close Vulkana, discard documents, or affect other connections. The listener returns to Listening after the last client disconnects. Each new connection has its own initialization state, request-ID namespace and buffers. An idle bridge waiting on client stdin may remain alive after Stop; its editor connection is already closed, and its next request fails rather than silently reconnecting or retrying an edit.

## Multiple clients and diagnostics

Multiple trusted local clients (including multiple bridges launched by the same MCP client application) can initialize and discover tools independently. They share the same editor, active document, selections and undo history. Requests are serialized across all connections on the editor thread, including when a synchronous operation processes nested UI events. Ready connections take turns; there is no transaction spanning multiple tool calls.

Use `expectedRevision` when editing a state you previously queried. If two clients submit different edits against the same revision, the first successful edit advances it and the later call returns `stale_revision` without changing the document. Query again before deciding what to do. `vulkana_get_state.connectedClients` reports the live connection count. Disconnecting during a running edit does not cancel or roll it back; pending requests from that disconnected client are discarded. Stop discards queued requests from every client while allowing the current synchronous command to finish.

The editor's `imageeditor.mcp.server` logging category records listener starts/stops, session IDs, Linux peer PIDs, initialization, disconnect reasons and client counts. Resource-limit rejections are warnings. Enable protocol-rejection codes and bridge lifecycle tracing when diagnosing a connection:

```sh
QT_LOGGING_RULES='imageeditor.mcp.server.debug=true;imageeditor.mcp.bridge.debug=true' imageeditor --mcp --skip-new-dialog
QT_LOGGING_RULES='imageeditor.mcp.bridge.debug=true' imageeditor --mcp-bridge
```

Set the bridge environment in the client configuration when the client launches it. Bridge diagnostics go only to **stderr**, never MCP stdout. Logs do not contain tool arguments, document content, or response bodies. Session IDs distinguish short-lived overlapping connections that a process snapshot can miss; bridge logs include their process IDs. A Connected status alone does not establish that a particular client completed discovery.

Qt may route editor diagnostics to the system journal. Set `QT_FORCE_STDERR_LOGGING=1` on the editor when capturing them in a terminal or redirected log.

## Installed and packaged bridges

Preferences discovers the executable from the running installation, never from a fixed build-directory path:

- **RPM/normal install:** the adjacent `vulkana-mcp` installed in the binary directory is preferred. The CMake install manifest includes both editor and bridge.
- **Windows portable:** `bin/vulkana-mcp.exe` beside `imageeditor.exe`; portable assembly checks that it is present. Copy configuration again if the portable directory moves.
- **AppImage:** the copied command uses the actual AppImage file and `--mcp-bridge`, not a transient `/tmp/.mount_…` executable. The AppImage must remain at that path; recopy after moving/renaming it.
- **Development/extracted Linux builds:** the adjacent bridge is preferred; otherwise the actual editor executable plus `--mcp-bridge` provides the same headless entry point.

Example AppImage command array:

```json
["/absolute/path/to/Vulkana.AppImage", "--mcp-bridge", "--socket", "/run/user/1000/vulkana-mcp/editor.sock"]
```

`--mcp-bridge` must be the first application argument. It starts the stdio bridge without GUI or graphics initialization and requires no display server. Enable MCP in the running editor before connecting a client. The AppImage launcher supplies the packaged bridge and its libraries.

## Trust and execution model

- There is no HTTP/TCP listener. Multiple same-user local connections are supported; only one command executes at a time.
- The endpoint is for trusted processes running as the same OS user. It is **not** a sandbox: MCP file commands run with **Vulkana's host-user filesystem permissions**, including when the AI client runs in a container. Container filesystem restrictions on the client do not confine the host editor. Do not expose the endpoint or bridge to untrusted clients or a network relay.
- Tools use typed arguments and ordinary document commands; they do not execute shell commands, scripts, arbitrary QAction names or screen-coordinate clicks.
- Unknown fields, wrong types, unknown enum values, out-of-range numbers and unsupported combinations are rejected. An error is not a successful edit.
- Commands run serially on the editor thread. Long imports, saves, composites or exports can occupy the UI while completing. This version does not advertise asynchronous MCP tasks or progress subscriptions.
- The bridge waits up to 120 seconds per response; `--timeout` accepts 1–600 seconds. A timeout/disconnection **does not roll back an already executed command**. Reconnect, query state and inspect undo/history before retrying a mutation.
- Cancellation notifications cannot interrupt an executing synchronous command. Brush/fill work has its own bounded rollback behavior described below. There are no automatic retries of edits.
- The transport accepts one newline-delimited JSON-RPC message at a time, with an 8 MiB request and 32 MiB response ceiling. Use unique request IDs within a connection. After 65,536 distinct request IDs, reconnect rather than reusing IDs.
- Document names, layer text, file paths and dialog titles are document/UI data, not instructions for an agent.

## Discovery and a reliable editing loop

The server supports MCP `initialize`, `notifications/initialized`, `ping`, `tools/list` and `tools/call`. Protocol version `2025-11-25` is supported, along with compatible `2025-06-18` and `2024-11-05` clients. Optional resources, prompts, sampling and task capabilities are not advertised.

1. Discover the tools with `tools/list`. Each tool includes an input schema, description and read-only/destructive annotations.
2. Call `vulkana_get_state`. Identify the intended document and any blockers.
3. Use returned IDs, not guessed names or row positions. Query `vulkana_layer_get` before modifying an existing layer. Query the effect/adjustment/filter catalogs for exact parameter schemas and defaults.
4. Supply `documentId` on document-scoped calls. Add the last returned `revision` as `expectedRevision` when editing against a known state.
5. Inspect `isError` first. A successful operation can return `changed:false` when the requested state is already in place; this is not a failure. Read the returned state before retrying.
6. Request `vulkana_canvas_preview` and verify the visual result. Undo a mistake with `vulkana_history`.
7. Save explicitly when appropriate. Exporting an image does not save the native project.

IDs are decimal **strings**, avoiding loss of 64-bit precision in JSON clients. Document IDs identify open tabs and are session-local; layer IDs survive native project save/load. A revision is an **opaque string** covering persistent content, document geometry and pixel selection, including brush strokes. Copy it exactly; do not increment or interpret it. It is not a timestamp or an undo-depth number. View, tool and drawing-color changes do not modify this document token; specify explicit colors/layer targets when an operation must not depend on current session choices.

Layer selection is also session state. `vulkana_layer_select` returns `changed:true`
when its selected layers, primary layer or content/mask target changes, together
with `selectedLayers`, `activeLayer` and `editingMask`. Repeating the same selection
returns `changed:false`. Neither operation changes the document revision or undo
history. Queries and file exports can also return `changed:false` on success;
inspect their image, path or other result fields.

### Recovering from argument errors

Vulkana rejects invalid arguments before executing the tool and returns
`isError:true`, `error.code:invalid_arguments`, and `error.details.changed:false`.
The message identifies the field and the expected type, allowed choices, numeric
bounds, text format or array limits. Missing and unknown fields are reported
together. For example:

- A missing `kind` on `vulkana_layer_create` lists `raster`, `text`, `shape` and `adjustment`.
- Supplying `newName` to `vulkana_layer_rename` reports that `name` is required and `newName` is unknown. Resend with `name`; fields are never silently renamed.
- A `maxSize` above 2048 reports the maximum instead of an unspecified dimension error.

Error text contains the same JSON as `structuredContent`, including codes,
blockers and recovery details, so clients that expose only text retain this
information. Diagnostics are bounded; `tools/list` supplies the complete schema,
and effect/filter/adjustment catalogs supply the parameters for a chosen type.
Clients can also validate schemas locally and reject a request before sending
it; those messages come from the client rather than this server response.

`documentId` must match the active document. The API does not switch tabs implicitly to satisfy a layer edit. `vulkana_document_activate` explicitly changes tabs. Stale revisions and wrong-document requests return errors without modifying the document.

An untouched launch placeholder may be replaced by the first new/opened document, matching the editor's normal behavior. Explicitly created, opened or edited documents remain separate tabs.

## Coordinates, colors and history

- Coordinates are document pixels unless explicitly described as layer-local. Origin is top-left; X increases rightwards and Y downwards. They do not depend on canvas zoom, monitor DPI or dock placement.
- Layer transforms returned by queries are row-major 3×3 matrices. Numeric transforms use the same interaction frame as the editor; query `transformValues` rather than treating the text baseline or a cropped raster's local origin as its center.
- Colors are sRGB `#RRGGBB` or `#RRGGBBAA`, with alpha **last**. Opacity, fill, brush hardness and flow use `0..1`, not percentages. Brush spacing is a percentage of diameter.
- The hierarchy's roots/children are bottom-to-top. Indices refer to sibling insertion positions, not displayed top-to-bottom rows.
- Content edits use the normal undo history. One tool operation normally produces one undo entry; no-op operations do not create a content edit. There is no multi-tool transaction/batch-undo API yet.
- Tool/layer selection, drawing colors, viewport changes and filesystem writes are not document undo actions. Closing a document is not undoable.
- Pixel selection `none` means **unrestricted painting**, not a mask selecting zero pixels. State distinguishes unrestricted, empty and nonempty selections.
- Whole-text property edits apply only supplied formatting properties to all characters. Other mixed properties remain mixed; replacing text content resets its character runs. Rich character-run data can be queried but not written directly in this version.

## Commands

The live `tools/list` schemas are the authoritative argument reference. The tables below group the available tools; common `documentId` and optional `expectedRevision` are omitted from argument summaries.

### State, documents and canvas

| Tool | Purpose and main arguments |
| --- | --- |
| `vulkana_get_state` | Open tabs, active document, IDs/tree, geometry, selected layers, tool, drawing colors, pixel selection, undo/redo depths, snapshots and UI blockers. `includeLayers:false` reduces the response. Safe while editing is blocked. |
| `vulkana_document_create` | New canvas: `width`, `height`, optional `name`, `dpi`, `background`. Creates one transparent raster layer by default. |
| `vulkana_document_open` | Open an absolute `path`: native project, static raster image or animated GIF. GIF imports retain frames and timing. |
| `vulkana_document_activate` | Explicitly activate `documentId`. |
| `vulkana_document_save` | Save native `.vulkana` to absolute `path`. Existing files require `overwrite:true`. |
| `vulkana_document_close` | Close the active document. Unsaved changes require explicit `discardChanges:true`; save first to retain them. |
| `vulkana_import_layer` | Import a static raster image using absolute `path`, optional `name`. Adds and selects a new layer; keeps existing layers and pixel selection. |
| `vulkana_canvas_resize` | `width`, `height`, optional `x`, `y` defining the new boundary in old canvas coordinates. No resampling. |
| `vulkana_canvas_view` | `operation`: `fit`, `reset` (100%), or `set` with `zoom`, `centerX`, `centerY`. View only. |
| `vulkana_canvas_preview` | MCP PNG image, optional `maxSize` (32–2048, default 1024 for both qualities), `quality` (`overview` by default, or `exact`), and `background` (`checkerboard` by default, `transparent`, `white` or `black`). |
| `vulkana_export_image` | `path`, `format` (`png`/`jpeg`/`webp`), optional paired `width`/`height`, `quality`, WebP `lossless`, opaque PNG/JPEG `matte`, `overwrite`. |
| `vulkana_history` | `operation`: `undo` or `redo`, one action. |

Preview is a rendered composite, not a screenshot of the visible viewport. It includes layers, masks, blending and effects but excludes selection outlines, handles and UI panels. `overview` uses the fast navigation sampler; `exact` uses the export compositor and is preferable for checking fine edges. Both support the full `maxSize` range: the longest output side uses that size and the other follows the canvas aspect ratio (at least one pixel). Larger previews require more rendering work. The response includes preview and document dimensions. Neither mode changes document pixels. Export defaults to current canvas dimensions, not preview dimensions.

File destinations must be absolute and their parent directory must already exist. New outputs are staged and published without replacing an existing destination; `overwrite:true` explicitly enables replacement. Native save and image export use the existing atomic writers. Saving marks the native document saved; exporting does not. Project and image formats are not inferred interchangeably from a filename.

### Layers, text and shapes

| Tool | Purpose and main arguments |
| --- | --- |
| `vulkana_layer_get` | Full editable data for `layerId`: text/shape properties, matrix/transform values, mask metadata and effect/adjustment/filter settings. |
| `vulkana_layer_create` | `kind`: `raster`, `text`, `shape`, `adjustment`; optional `name`, `x`, `y`, `parentId`, `index`. Raster uses `width`, `height`, `color`; text/shape use nested `text`/`shape` properties. Adjustment supports `scope`. |
| `vulkana_layer_select` | `layerIds`, optional `target`: `content` or `mask`. Mask target requires one layer with a mask. |
| `vulkana_layer_rename` | `layerId`, `name`. |
| `vulkana_layer_delete` | Delete `layerIds`, including container descendants; undoable. Cannot delete the last content layer. |
| `vulkana_layer_duplicate` | Duplicate `layerIds`; preserves editable payloads and selects the copies. |
| `vulkana_layer_reparent` | Move `layerIds` to optional `parentId` and sibling `index`; omitted parent is the document root. Cycles are rejected. |
| `vulkana_layer_group` | Group `layerIds` with `kind`: `folder`, `group` or `clipping_group`, optional `name`. Structural compatibility is validated. |
| `vulkana_layer_ungroup` | Dissolve container `layerId`, preserving children and effective visibility. |
| `vulkana_layer_set_visibility` | `layerId`, `visible`; supports containers. |
| `vulkana_layer_set_opacity` | `layerId`, `opacity`; affects pixels and effects. |
| `vulkana_layer_set_fill_opacity` | `layerId`, `opacity`; Pixel Opacity/Photoshop Fill. |
| `vulkana_layer_set_blend_mode` | `layerId`, `mode`; supported stable IDs are in the input schema. |
| `vulkana_layer_transform` | `layerIds`, optional `centerX`, `centerY`, absolute `scaleX`, `scaleY`, clockwise `rotation`. Omitted values remain unchanged. |
| `vulkana_font_list` | Installed font families/styles, optional filtering as described by its schema. Does not install fonts. |
| `vulkana_text_set` | `layerId`, `properties`: text, font family/style/size, weight, italic, color, alignment. Missing fonts/styles are rejected. |
| `vulkana_shape_set` | `layerId`, `properties`: kind, width/height, points, corner radius, fill/stroke styling. Geometry is layer-local. |
| `vulkana_layer_mask` | `layerId`, `operation`: `reveal_all`, `hide_all`, `from_selection`, `remove`, `invert`, `enable`, `disable`. Existing masks are not silently replaced. |

### Effects, adjustments and filters

| Tool | Purpose |
| --- | --- |
| `vulkana_effect_catalog` | Every supported effect's stable type ID, parameter schema and defaults. |
| `vulkana_effect_set` | Enable/patch one `type` on `layerId` using `parameters`. Other effects remain unchanged. |
| `vulkana_effect_remove` | Disable one effect, retaining its settings. |
| `vulkana_effect_blending` | Set layer-wide `pixelAlphaShapesEffects` / `maskEffectsAfterRendering`. |
| `vulkana_adjustment_catalog` | Supported adjustment types, parameter schemas and defaults. |
| `vulkana_adjustment_set` | Patch one adjustment's `parameters`; optional `enabled`. Preserves its existing captured-selection mask. |
| `vulkana_adjustment_remove` | Disable one adjustment, retaining settings and mask. |
| `vulkana_adjustment_scope` | Change an adjustment layer's `scope`: `all_below` or `this_group`. |
| `vulkana_filter_catalog` | Supported spatial filters, schemas and defaults. |
| `vulkana_filter_set` | Patch one filter's `parameters`; optional `enabled` / `preserveAlpha`. Preserves captured-selection masks. |
| `vulkana_filter_remove` | Disable one filter, retaining settings and mask. |

Use catalogs instead of assuming that every effect accepts the same fields or units. Arrays replace the specified array. Gradient stops are ordered color/position pairs; at least two are required, or `[]` restores the two-color mode. Container objects are not direct effect targets. Adjustment layers cannot receive unsupported spatial filters/effects.

### Painting and selections

| Tool | Purpose and main arguments |
| --- | --- |
| `vulkana_brush_presets` | Available preset IDs and per-preset defaults. Does not edit the preset library. |
| `vulkana_brush_stroke` | `layerId`, `points:[{x,y,pressure?},…]`; optional `presetId`, `target` (`pixels`/`mask`), `mode` (`paint`/`erase`), `color`, `size`, `hardness`, `opacity`, `flow`, `spacing`, pressure flags. |
| `vulkana_fill` | `layerId`; `mode` (`selection`/`contiguous`), optional `target`, `color`, `tolerance`; contiguous requires `seed`. |
| `vulkana_selection` | `shape`: `rectangle`, `ellipse`, `polygon`, `all`, `none`, `invert`; `from`/`to` or `points` for geometry; optional `operation`: `replace`, `add`, `subtract`, `intersect`. |
| `vulkana_selection_from_alpha` | Replace the selection from `layerId`'s intrinsic pixel/glyph/shape transparency. An empty source leaves the selection and redo branch unchanged. |
| `vulkana_colors` | Set session `primary`, `secondary`, and/or active slot `active`. These are drawing colors, not theme settings. |
| `vulkana_sample_color` | Sample document `point`, from `source:merged` or intrinsic `source:layer` with `layerId`. Does not change drawing colors. |
| `vulkana_tool_select` | Select an ordinary UI `tool`; use the explicit MCP operation for actual editing. |

Strokes use Vulkana's real brush engine, including presets, pressure, transforms, crop and pixel selection. Pixel painting requires a raster layer; mask painting requires an existing mask and preserves original pixels. Erasing a mask paints black. Stroke/fill operations preserve the user's layer selection, active tool and brush settings. The default brush is hard round; color defaults to the active foreground slot unless explicitly supplied.

One stroke is limited to 4,096 supplied points, 65,536 emitted dabs, a total path length of one million document pixels and a five-second cooperative work budget. Exceeding its work budget rolls back journalled changes without adding an undo entry. Fill uses a similar cooperative budget. A single expensive operation may finish its current work unit before the time check. Split large strokes into intentional smaller strokes when necessary.

Alpha selection matches thumbnail Ctrl-click semantics: it respects transform/crop, but excludes layer opacity, fill opacity, masks, effects and clipping-group contributions. Empty alpha never clears an unrelated selection.

### Blocking UI

`vulkana_get_state` remains available while a dialog, text edit, preview, graphics initialization, processing task, rename or pointer/numeric gesture blocks editing. It reports machine-readable blocker codes, user-facing titles/messages and a suggested resolution. Ordinary editing calls return `isError:true` with `error.code:blocked`; they do not dismiss UI automatically.

| Tool | Explicit resolution |
| --- | --- |
| `vulkana_finish_text` | Finish current interactive text editing and retain the text. |
| `vulkana_cancel_interaction` | Cancel a matching `kind`: `workspace_dialog`, `snapshot_preview`, `refine_selection`, `background_removal`, or `transform`. Never accepts a dialog confirmation. |

Native modal dialogs and popup menus must be resolved in the editor before changing their underlying interaction. Running file operations/graphics initialization cannot be cancelled by these tools. Apply of Refine Selection/Remove Background remains interactive. Inspect the returned state after cancelling; a deferred UI transition may not have completed until the next event-loop turn.

## Example calls

After initialization, create a document:

```json
{"name":"vulkana_document_create","arguments":{"width":640,"height":480,"background":"#ffffff","name":"Sketch"}}
```

Read `document.documentId` and `document.revision` from its structured result. Create an editable rectangle (replace the illustrative document ID with the returned value):

```json
{"name":"vulkana_layer_create","arguments":{"documentId":"3","kind":"shape","name":"Blue card","x":40,"y":60,"shape":{"kind":"rectangle","width":180,"height":120,"fillColor":"#2864dc","fillEnabled":true}}}
```

Create a separate raster layer for painting, then use its returned `layerId`:

```json
{"name":"vulkana_brush_stroke","arguments":{"documentId":"3","layerId":"42","color":"#25a54d","size":12,"points":[{"x":260,"y":300,"pressure":1},{"x":300,"y":260,"pressure":0.6},{"x":340,"y":300,"pressure":1}]}}
```

These are `tools/call` parameters, not whole JSON-RPC envelopes. A complete wire request adds `jsonrpc:"2.0"`, a unique `id`, `method:"tools/call"`, and places the object above in `params`. Prefer supplying `expectedRevision` from the preceding state/result in a multi-step client.

## Current boundaries

- This is a typed document-editing API, not complete mouse/keyboard remote control. Selecting a UI tool does not synthesize its interactive gesture.
- No application preferences, theme/language changes, GPU selection, updates, plugin installation, arbitrary scripting or shell execution.
- PSD/PDF review imports and PSD/PDF/GIF export remain interactive. Static raster export and native project round trips are available; GIF import preserves its frames.
- Snapshot metadata is queryable, but timeline capture/reorder/playback/export controls are not exposed yet.
- Object Selection, Remove Background inference, Refine Selection corrections, cloning/healing/local-blur strokes, crop gestures, perspective-corner editing and image resampling are not exposed yet. Existing content using these features still renders normally in previews/exports.
- Text editing is whole-layer/property based; individual rich-text ranges and IME composition are not automated. Shape vertex arrays can be supplied, but there is no pointer-driven vertex editing.
- Mask painting and lifecycle operations are supported; copying masks between layers and writing captured adjustment/filter masks are not yet exposed.
- New canvases use the editor's 16,384-pixel dimension / 40-megapixel limits. MCP raster-layer/mask allocation has a 64-megapixel ceiling; alpha extraction and other operations retain their own engine limits. There is no silent downscaling to fit a request.
- There is no transactional group of multiple tool calls, asynchronous job API or remote notification stream. Query after an error, timeout, reconnect or concurrent human edit; never blindly replay a mutation.

## Extending the framework

The responsibilities are separated so future features do not need their own transport:

- `src/mcp/`: reusable JSON-RPC lifecycle, schema validation, tool registry, private local server and stdio bridge. No editor/UI dependency.
- `McpController`: editor-thread ownership, document/revision checks, blocker reporting, tool results and connection indicator.
- `MainWindow` owns a restartable controller; the MCP Preferences panel observes it. Saved `preferences/mcp` settings are separate from listener state and CLI overrides.
- `McpDocuments.cpp`, `McpLayers.cpp`, `McpPainting.cpp`: domain-specific registrations using existing document commands/renderers.
- `McpToolHelpers.hpp`: schema and value helpers.

To add a feature:

1. Add a typed tool through `McpController::add`, with a stable name, useful description, complete schema and correct access mode. `Document` automatically requires an active `documentId` and accepts `expectedRevision`; `DocumentQuery` adds the same checks with a read-only annotation. `Query` is for safe global/state discovery; `Interaction` is only for explicit, guarded interaction resolution.
2. Validate all arguments and target compatibility **before** modifying the document. Do not silently normalize an unsupported request into a different action.
3. Reuse an existing core command/transaction. Call the shared `execute()` helper, or commit an existing editing transaction and call `changed()` only after success. Preserve unrelated selections, masks, layers and session settings unless changing them is the documented purpose.
4. Return actual outcome/state and stable IDs. Errors after a commit must not pretend that nothing changed. Keep expensive work bounded; add a real job/cancellation design before exposing long model inference.
5. Add focused tests for output pixels/data, undo/redo, wrong/stale IDs, validation without partial edits, blockers, no-op behavior and save/load when relevant. Update this command reference and limitations.

Build and run focused tests:

```sh
cmake --build build/release --target imageeditor imageeditor_mcp_connection_tests imageeditor_mcp_preferences_tests imageeditor_mcp_protocol_tests imageeditor_mcp_editor_tests imageeditor_mcp_layer_tests imageeditor_mcp_painting_tests
ctest --test-dir build/release --output-on-failure -R '^imageeditor_mcp_.*_tests$'
```

Protocol references: [stdio transport](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports), [lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle), [tool schemas and results](https://modelcontextprotocol.io/specification/2025-11-25/server/tools).
