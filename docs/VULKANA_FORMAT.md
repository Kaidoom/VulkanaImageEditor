# Vulkana project format — schema 1

The public extension is `.vulkana`; the format identity is `org.vulkana.project`. The schema version is independent of the application version. ZIP Store and Deflate, including ZIP64, are accepted. The writer uses Deflate level 3 once per payload. No archive entry is extracted to the filesystem.

## Container

`manifest.json` is UTF-8 JSON. Raster payloads are `rasters/<decimal-layer-id>.rgba`: exactly width × height × 4 bytes, top-to-bottom tightly packed rows, RGBA channel order. These are unassociated/straight RGBA8, sRGB color values. All four bytes round-trip exactly, including RGB under zero alpha. Layer adjustments, opacity and blend-mode composition are evaluated for rendered output and are never baked into these source payloads during serialization.

```json
{
  "format": "org.vulkana.project",
  "version": 1,
  "required": ["rgba8", "rich-text-v1"],
  "canvas": {
    "width": 1920, "height": 1080, "ppi": 96,
    "colorSpace": "srgb", "pixelFormat": "rgba8-straight"
  },
  "layers": [{
    "id": "42", "type": "raster", "name": "Layer 1",
    "visible": true, "opacity": 1,
    "transform": [1, 0, 0, 0, 1, 0],
    "raster": {"width": 1920, "height": 1080, "path": "rasters/42.rgba"}
  }]
}
```

Layers are ordered **bottom to top**. IDs are canonical positive decimal strings up to signed 63-bit maximum: strings avoid the JSON/JavaScript 53-bit integer precision trap and leave allocator headroom. The six affine doubles are `[m00,m01,m02,m10,m11,m12]`, mapping layer-local to document coordinates. Negative scales, rotation, shear and off-canvas translation are retained. Surface extent is independent of canvas extent; invisible and clipped pixels are still saved. JSON doubles preserve the existing in-memory precision; opacity remains the core's float property.

The existing initial Transparent/White/Black background is actual raster content, not a second document background property.

### Projective mapping and native raster origins

`projective-transform-v1` extends the schema-1 required-capability list. Affine
projects continue writing six transform numbers. A projective layer writes nine,
ordered `[m00,m01,m02,m10,m11,m12,m20,m21,m22]`; the first two row products are
divided by `m20*x + m21*y + m22`. Captured adjustment/filter `localToMask` mappings
use the same six-or-nine representation and declare the capability when needed.
Missing bottom rows default to `[0,0,1]`. Readers reject undeclared or unsupported
projective state, nonfinite/singular matrices and raster support crossing a
projective horizon. Typed models remain authoritative and editable.

`raster-local-frame-v1` permits an optional raster-layer field:

```json
"rasterLocalFrame": {
  "origin": [-24, -12],
  "effectReference": [0, 0, 800, 600]
}
```

The integral signed origin locates stored raster pixel `(0,0)` in the unchanged
layer-local coordinate system. This preserves external transforms and crop/mask
coordinates when selected-pixel edits extend storage. The optional positive-size
effect reference rectangle preserves gradient anchors when bounds grow. Without
the field, origin is zero and the natural raster bounds define that reference.
The field is forbidden on typed layers. Actual raster bytes remain the ordinary
width×height RGBA payload, including off-canvas content and hidden RGB; no session,
fragment, history or copy-on-write storage implementation is serialized.

These required capabilities do not change the application/package version or
the schema-1 archive envelope. Older readers reject them explicitly rather than
silently flattening or misplacing the content.

## Editable text

A `type: "text"` layer has `text` instead of `raster`. Its object uses the shared explicit rich-text codec:

```json
{
  "version": 1,
  "text": "Hello\nworld",
  "default": {
    "family": "Sans Serif", "style": "", "weight": 400, "italic": false,
    "size": 24, "rgba": [255, 255, 255, 255]
  },
  "runs": [],
  "paragraphs": [{"start": 0, "alignment": 0}]
}
```

Runs contain `start`, `length`, and `format` (same fields as `default`). Offsets are UTF-8 byte offsets at Unicode scalar boundaries, **not Qt UTF-16 positions**. Gaps inherit the default style; adjacent equivalent runs are canonicalized. Paragraphs contain newline-boundary byte `start` and alignment 0=left, 1=center, 2=right; absent paragraph records inherit left/preceding alignment during normalization. Font size is document pixels, independent of viewport/monitor DPI. Requested font descriptors are preserved, even if unavailable. Font files are not embedded; missing families produce a warning and render through Qt fallback.

Text data is authoritative; font layout, raster caches, caret and character selection are excluded. The temporary document selection, active layer, tools, global colors, brushes, view/UI state and history are also excluded. A selection explicitly captured as an adjustment mask is persistent layer data, as described below.

## Editable shapes (`shape-v1`)

Shape layers extend schema 1 through its existing required-capability mechanism. A document containing any shape includes `"shape-v1"` in the root `required` array. A pre-shape reader rejects that capability explicitly before constructing a document; it cannot silently flatten or discard shapes. Raster/text-only saves do not require this capability, so they remain compatible with the original schema-1 reader. The app/package version remains independent of both schema and capability versions.

A layer with `type: "shape"` contains the following authoritative descriptor and **no raster payload**:

```json
{
  "version": 1,
  "kind": "rounded-rectangle",
  "size": [160.5, 90.25],
  "points": [],
  "cornerRadius": 12.5,
  "fillEnabled": true,
  "fillRgba": [35, 80, 170, 128],
  "strokeEnabled": true,
  "strokeRgba": [240, 240, 255, 200],
  "strokeWidth": 2.5,
  "strokePlacement": "center",
  "cap": "round",
  "join": "round"
}
```

Every descriptor field above is required. Kinds are `rectangle`, `rounded-rectangle`, `ellipse`, `triangle`, `line`, and `polygon`. Size, radius, stroke width and vertices are doubles measured in **layer-local document pixels** (+X right, +Y down), unaffected by monitor DPI or viewport zoom. `points` is empty for primitives, exactly two `[x,y]` pairs for lines, and at least three for polygons; each vertex lies in the inclusive local frame `[0,width] × [0,height]`. Polygon filling is even-odd, including self-intersections. Structurally valid degenerate geometry can be reopened and edited even though empty creation gestures do not create layers.

Fill and stroke each retain their own straight-sRGB RGBA8 color, including hidden RGB at zero alpha. Disabled fill means no fill. Disabled stroke or zero stroke width means **no stroke**, never a device-pixel hairline. Strokes are centered with round joins/caps; these explicit descriptor fields prevent a later implementation from silently changing their meaning. Stroke and geometry share the layer's affine transform, including stretching, rotation and negative scales. Corner radius is stored without permanently clamping it to the current size; rendering applies the geometric clamp.

Caches, antialiasing outsets and cache density are disposable and excluded. On publication, the common shape-cache preparation path rebuilds them from the descriptor, including stroke/antialiasing bounds; load does not allocate up to 1024 derived full-resolution images during untrusted archive decoding. Existing metadata preservation applies inside `shape` as well. Unknown shape versions, kinds, essential stroke semantics, or missing `shape-v1` declarations fail explicitly.

## Non-destructive crop (`layer-crop-v1`, `layer-crop-chamfer-v1`)

An optional layer-level `crop` object stores `{ "version": 1, "x": 12.5,
"y": -2, "width": 80, "height": 60 }` in stable layer-local pixels. All five
fields are required. Positions, dimensions and endpoints must be finite and
within ±1e9; dimensions cannot be negative. Zero dimensions represent an empty
visible region, not absence of crop. Absence of this object means unrestricted
source visibility. Source payloads, text/shape descriptors, transforms and
adjustments are preserved completely; crop never truncates saved content.

A file containing any crop must declare `layer-crop-v1` in `required`, including
an empty crop. Unsupported essential versions and malformed descriptors reject
the load. Old files with no crop load unchanged. Resaving after Remove Crop
drops both the obsolete owned descriptor and the capability if no other layer
needs it. The editor-only retained-source preview is not serialized.

Chamfered crops write `version: 2`, the same frame fields, and a required
`corners: [TL, TR, BR, BL]` array. Each finite value in [0, 1e9] specifies an
equal-axis cut distance in local pixels. Resolve each independently to at most
half the shorter frame dimension; preserve the raw value when saving. Both
`layer-crop-v1` and `layer-crop-chamfer-v1` are required, so unsupported readers
cannot silently replace the chamfer with a rectangle. Version 1 forbids the
`corners` field. Files with all-zero cuts may use version 2 when read, but write
version 1 without the chamfer capability. See [Coordinates](COORDINATE_SYSTEM.md)
for crop geometry and coverage.

## Non-destructive adjustments (`adjustments-v1`)

A raster, text or shape layer may contain an `adjustments` object. Any serialized collection requires `"adjustments-v1"` in the root `required` array, including a collection whose entries are currently disabled. Older projects with no adjustment descriptor load as unadjusted. Pass-through folders and groups have no adjustment collection of their own; their children retain their individual settings.

The collection has required numeric `version: 1`, numeric `algorithmVersion: 1`, and an `items` array containing exactly ten records in the order below. Every record requires its stable string `type`, numeric `algorithmVersion: 1`, Boolean `enabled`, and typed `parameters` object. The array order, descriptor version and algorithm version are explicit persisted semantics. Unknown types, unsupported versions, duplicate or reordered types, wrong parameter types, and invalid ranges reject the project rather than silently dropping an effect. The version-one evaluation contract is documented in [Adjustment algorithms](ADJUSTMENT_ALGORITHMS.md).

| Order / stable type | Required parameter fields |
| --- | --- |
| 1. `exposure` | `stops` in [−20, 20] |
| 2. `brightness-contrast` | `brightness`, `contrast`, each in [−1, 1] |
| 3. `levels` | `channels`: four objects in composite RGB, R, G, B order; each has `inputBlack`, `gamma`, `inputWhite`, `outputBlack`, `outputWhite` |
| 4. `curves` | `channels`: four arrays in composite RGB, R, G, B order, each containing 2–16 `[input, output]` control points |
| 5. `hue-saturation` | `ranges`: seven `{hue, saturation, lightness}` objects in Master, Reds, Yellows, Greens, Cyans, Blues, Magentas order; Boolean `colorize`; `colorizeHue`; `colorizeSaturation` |
| 6. `vibrance` | `amount` in [−1, 1] |
| 7. `color-balance` | `tones`: three arrays of three numbers, shadows/midtones/highlights × red/green/blue; Boolean `preserveLuminosity` |
| 8. `warmth-tint` | `warmth`, `tint`, each in [−1, 1] |
| 9. `black-white` | `contributions`: six values in Red, Yellow, Green, Cyan, Blue, Magenta order; `tintRgba`: four integer bytes; `tintStrength` in [0, 1] |
| 10. `invert` | Empty parameters object |

All parameter numbers must be finite. Levels black/white/output values lie in [0, 1], `inputWhite >= inputBlack`, and gamma lies in [0.1, 10]; coincident input points are valid and have defined threshold behavior. Curve coordinates lie in [0, 1] with input coordinates strictly increasing by at least 10⁻⁶; output coordinates may increase or decrease, so intentionally non-monotonic curves round-trip. Hue-range rotation lies in [−180, 180] degrees, range saturation/lightness in [−1, 1], colorize hue in [0, 360], and colorize saturation in [0, 1]. Color-balance components and monochrome contributions lie in [−1, 1]. The monochrome tint's alpha byte is retained, but does not change source alpha when evaluated.

Each record can additionally contain a `mask` descriptor. For example, this is one exposure record within the complete ten-record collection:

```json
{
  "type": "exposure",
  "algorithmVersion": 1,
  "enabled": true,
  "parameters": {"stops": 1.25},
  "mask": {
    "width": 1920,
    "height": 1080,
    "path": "adjustments/42/exposure.r8",
    "localToMask": [1, 0, 120, 0, 1, 80]
  }
}
```

Mask payloads use the canonical path `adjustments/<decimal-layer-id>/<stable-type>.r8` and contain exactly width × height coverage bytes, top-to-bottom tightly packed rows. Zero means no effect and 255 means full effect; intermediate values, holes, and disconnected regions are preserved. An omitted mask means whole-layer scope. A present all-zero plane remains an explicitly captured empty mask and never becomes unrestricted.

`localToMask` uses the same six-coefficient layout as a layer transform but maps current layer-local coordinates into the frozen capture plane. It must be finite, invertible, and within the affine coefficient bounds below. Mask dimensions belong to that captured plane; they need not equal the current canvas or current text/shape bounds. Moving or transforming a layer, resizing the canvas, or editing text/shape bounds does not rewrite or normalize its saved coverage. Ctrl+J raster extraction retains editable adjustments and composes this mapping into the new output-local frame; only extraction pixels and selection alpha are baked. Successful merge instead writes already-adjusted raster content into a fresh layer with no adjustment collection.

Original raster bytes, text content/formatting and shape geometry/style stay authoritative and unchanged. Compiled parameter blocks, display caches, histograms, panel navigation and temporary before/after bypass are not serialized. Adjustment data is an owned subtree: the writer replaces it with the current authoritative collection, and removes it when that collection is absent. Resetting therefore cannot resurrect old settings or mask paths retained in source metadata. Masks are currently stored per record, without deduplicating shared immutable coverage across records or layers; each payload counts toward the aggregate byte limit.

## Evolution and validation

Schema dispatch happens before constructing a document. Only version 1 currently exists; other versions are rejected without rewriting the source. Frozen compatibility samples are in `tests/fixtures/projects`; tests wrap those independently in Store/Deflate/ZIP64 archives.

Required fields: format/version, canvas width/height, nonempty layers, each layer's ID/type/name, and its raster dimensions/path, rich-text descriptor or shape descriptor. Optional defaults: ppi=96, visible=true, opacity=1, transform=identity, colorSpace=srgb, pixelFormat=rgba8-straight, required=[] (base schema remains required; shapes additionally require `shape-v1`). All recognized supplied fields are validated. Invalid types/ranges, unsupported color pipelines, unknown layer types or required capabilities fail explicitly.

Unknown **noncritical JSON** fields are retained at document/canvas/layer/nested-object levels, except inside the owned `adjustments` subtree described above. Known fields override them on save; layer objects match by ID, not current ordering. Nested structured arrays retain optional metadata at corresponding entries while known fields/lengths are updated. Removed layer metadata is not retained. Opaque metadata must not carry essential semantics: producers must declare any essential extension in the root `required` list. Unknown archive payloads are rejected instead of silently dropped.

Hard bounds on reading and writing:

- 1–1024 layers; 1–32768 pixels per axis; 64 × 1024² pixels per canvas or raster.
- Aggregate raw raster plus adjustment-mask bytes ≤1 GiB; manifest ≤8 MiB; archive file ≤2 GiB. The aggregate limit uses inflated bytes, independently of ZIP compression ratio.
- Entries ≤11265 (one manifest, up to 1024 raster payloads and up to ten masks per layer), names ≤256 bytes; cumulative ZIP filename/extra/comment metadata ≤16 MiB.
- Each adjustment mask: 1–32768 pixels per axis, at most 64 × 1024² coverage bytes. All descriptor versions, parameter ranges, exact entry lengths, canonical paths and aggregate bounds are validated before any mask coverage plane is allocated.
- Existing rich-text limits: UTF-8 ≤256 KiB per text layer, ≤16384 runs, ≤4096 paragraphs; valid finite fonts/styles and ordered scalar-boundary ranges.
- Shapes: ≤100000 vertices per layer; finite local dimensions in [0, 1000000] and radius/stroke width in [0, 100000]; vertices within their local frame. The aggregate 8 MiB manifest limit also bounds total shape data before descriptor allocation.
- PPI 1–1200; finite affine coefficients with absolute value ≤10¹² and a valid inverse.
- Canonical relative paths only, no duplicate entries, links/directories, encryption, multipart archives or other compression codecs.

Directory declarations are bounded before payload allocation. Actual inflated lengths, compressed-byte counts and independently calculated CRC32 must agree with ZIP metadata. Cancellation is checked per bounded payload chunk. CRC detects corruption, not malicious authenticity; projects are not cryptographically signed.

## Safe writes and memory

The adapter uses the installed minizip-ng 4.x low-level stream API (zlib license), behind a private seekable QIODevice wrapper. It does not use a high-level reader that silently expands a compressed central directory. Read/write errors are sticky even if compressor shutdown misses a callback error.

On Linux, save creates a uniquely named `QTemporaryFile` **beside the destination**, streams the manifest and ≤256 KiB raster/mask row chunks, finishes ZIP, flushes and fsyncs the temporary file, then independently reads and verifies every entry. Verification decompresses into a bounded discard buffer, not another document. There is one compression pass, no full-raster clone or GPU readback. A final same-directory POSIX `rename` atomically replaces the destination; no direct-write fallback exists. Failure/cancellation before rename removes the temporary file and preserves the previous destination. A destination directory/permission/rename failure also leaves the previous file intact.

The parent directory is not fsynced, so the rename may be lost after sudden power failure even though the file data was flushed.

Loading allocates each validated raw surface once, fills it in bounded chunks and moves it into a temporary document. Each validated mask is read into one bounded R8 buffer, converted to the immutable tiled selection representation, and then releases that buffer; its decoded coverage and conversion buffer coexist temporarily. The current and candidate documents coexist until success; this is the necessary cost of failed-open safety. GPU/text/shape caches are rebuilt after publication. Service limits bound the candidate, not total process memory including the existing document/history/render caches.

## Saved state and interactions

Successful save updates association, title, recents and checkpoint without clearing history. Opening an image never adopts its path as a save target. New explicit canvases start modified; the untouched startup placeholder and newly opened files start clean. Content tokens exactly recognize undo/redo to saved endpoints, including branches, merges and budget eviction. A separate later edit that recreates equivalent bytes/properties can conservatively remain modified; the implementation does not hash entire documents to discover semantic equality between independent actions.

File operations finish committed typing/numeric edits and publish completed transform actions. Save/Open reject an active stroke/fill/held geometry drag until it finishes. Closing cancels unfinished stroke/fill/drag previews first, while retaining completed actions, then asks Save/Discard/Cancel. Cancelling a destination chooser does not call the destructive edit-cancellation path. Discard permits replacement but leaves the current document alive until the new one is fully decoded. A failed save/open does not change its association or claim a successful checkpoint.

All operations run under controlled editor-input exclusion on the owning thread. Progress/cancel/expose events remain serviced; timers do not edit document content during serialization. The callback API is also an explicit deterministic cancellation/failure-injection seam for tests. Recent paths are application preferences, capped at 10 normalized, deduplicated MRU entries.
