# PDF export

Adjustment layers are operators, not page entries. Each selected page includes
the applicable corrections over its included lower content in original stack
order, without adding unselected artwork or a blank adjustment page. Scope-owned
group isolation remains in force even for hidden corrections. Active operators
use the canonical raster page path; see [adjustment layers](ADJUSTMENT_LAYERS.md).

Choose PDF in **File → Export**. Single Composite combines the checked items;
Layers as Pages gives each chosen layer or group its own page. This exports
rendered artwork and, where supported, real selectable text. It does not export
editable PDF layers or embed the project. Imported PDF pages remain raster images.

## Items and order

The list follows top-to-bottom Layers-panel order. Ordinary folders are traversed
recursively; an intentional Group is one item containing its subtree. Empty
folders produce no item. Folder expansion/collapse has no effect. Current layer
selection includes exportable descendants and normalizes overlapping selections.

Source numbers stay fixed before hidden filtering and reversal. Ranges such as
`1-3, 6, 9-12` select those numbers, reject invalid input and deduplicate overlaps.
The preview selector shows the separate final page order. Reverse Order changes
pages only, never their internal stacking. Empty selections disable Export.

Ignore Hidden respects visibility throughout the hierarchy. With it off, selected
hidden content and group descendants are included using immutable export-only
overrides. Opacity is never forced to 100%; zero opacity can produce a blank page.

Composite uses the chosen content's original relative stacking and pass-through
semantics. With all visible items it matches normal document export. Each separate
page instead composites its own content onto transparency, including its real
blend modes and effects, without unrelated background layers.

## Geometry, color and resolution

Canvas pages preserve placement and clip at the canvas. Fit Each Layer/Group uses
the evaluated document-space bounds, including transforms, crop and effect/filter
support. Supported off-canvas content is retained. Empty bounds use a previewed
canvas-size blank page. Imported page margins are not automatically trimmed.

For the chosen integer-grid document rectangle:

```
physical points = rectangle size × 72 / document PPI
raster pixels   = max(1, floor(rectangle size × export PPI / document PPI + 0.5))
```

Export PPI defaults to document PPI. Changing it changes raster detail, not physical
page size, text layout or document settings. Pages have exact custom sizes and
zero margins; there is no printer or implicit paper-size matching.

Raster pages use the canonical source compositor: straight RGBA8 sRGB after
linear-light composition, no viewport textures, selection clip or editor overlays.
The PDF contains lossless Flate RGB images and soft alpha masks, with an sRGB output
intent. Transparency is not painted white unless requested. Replace Transparency
first renders onto transparency, then uses the existing linear-light final matte;
the matte is not an earlier backdrop for blend modes. Its color is export-only.
This is RGB artwork output, not CMYK proofing, PDF/X or spot-color separation.

## Preserved text

Preserve text where possible uses the existing Qt text-layout service's resolved
fonts, rich-text formatting, shaped glyph positions and Unicode clusters. Fonts
are embedded/subset only when their reported embedding permissions allow it.
Qt supplies the font subsets; qpdf maps shaped glyphs back to the original Unicode
clusters (including ligatures and contextual Arabic). Auxiliary marks in a
multi-glyph cluster can be outlines, while its actual PDF text glyph carries the
whole cluster. There is no invisible duplicate text. Affine rotation, scaling and
flips do not reflow the layout or depend on raster PPI.

The compact summary counts preserved/rasterized TextLayers per output page;
Text details explains fallbacks. V1 conservatively rasterizes cropped/projective,
translucent, adjusted or styled text; unsupported/restricted/color/synthetic fonts;
and interacting blend/style spans or raster content above preserved text. Final
matte output also uses canonical raster composition. Ordinary opaque photo-plus-
title pages retain real text. Unexpected Qt font encoding triggers a reported
affected-page raster retry, never missing text or a failed export just for that
limitation. Rasterize all text always uses the fully canonical page image.

PDF readers differ in antialiasing and reading order. Rotated/mirrored text has
logical-reading-order metadata as well as glyph Unicode mappings. Poppler honors
this; Qt PDF/PDFium currently ignores that form-level metadata and may reorder
transformed words when copying/searching. Appearance remains correct. Rasterized
fallback text is not selectable/searchable.

## Lifetime and resources

The owned export overlay pins its document and resolves unfinished editing using
the existing export policy. Tab switching/closing is blocked while it is open.
One frozen snapshot keeps all pages on the same revision; export never changes
selection, history, project association, PPI or visibility. Successful options
belong to the originating document; general preferences omit runtime IDs/ranges
and PPI. A source PDF cannot be used as its own export destination.

One worker serializes planning, previews, lazy thumbnails and writing. Preview
requests coalesce and obsolete results are discarded. At most one full page and
bounded compositor scratch are evaluated at once; 32 small icons are retained.
Qt writes compressed page resources to a temporary PDF; qpdf streams the final
assembly into QSaveFile. There is no in-memory list of full page images.

Limits: 1024 source layers, 512 MiB frozen raster source, 16 MiB text/geometry
metadata, 32768 pixels/side, 32 megapixels/page, 128 megapixels reconstruction
support, 0.01–14400 points/side and 1 GiB encoded output. The 2 GiB working estimate
includes open-document source/history/cache memory, the snapshot, page buffers
and renderer/writer reserve; Linux additionally checks available-memory headroom.
These are preflight budgets, not a hard sandbox around Qt/font/codec allocations.
Resolution is never silently reduced. Closing other documents or reducing export
PPI can resolve a working-budget rejection.

Cancel appears during export and stops the operation without closing the panel.
The header X (or Escape) closes the panel, safely cancelling any active work first.
Cancellation is checked between pages, compositor rows, text runs and output writes.
An in-progress font/compression call must finish before worker shutdown; no thread
is forcibly killed. The UI stays responsive. All painter/writer finalization and
device checks finish before atomic destination replacement. Cancellation or failure
leaves the previous file intact.

## Development checks

`imageeditor_pdf_export_tests` covers plans, rendering, exact embedded image bytes,
native text/search, font fallbacks, project round-trips, cancellation and limits.
`imageeditor_pdf_export_interaction_tests` exercises the existing export overlay,
range controls, previews, per-document settings and blocked retargeting;
`--validation` adds native Vulkan rendering/validation.

Set `VULKANA_PDF_EXPORT_OUTPUT` to retain diagnostic outputs. The optional
`tests/check_pdf_export.py` uses Poppler, Pillow, NumPy and pypdf for independent
render/extraction checks; none are application dependencies. It does not update
goldens. Private PDFs may be passed to the C++ service test for import → edit →
project reopen → export checks without adding them to the repository or packages.
