# PSD import

PSD import converts reviewed content into ordinary Vulkana layers. The source
file is never modified or needed after saving the result as `.vulkana`.
Open, recent files, file-manager requests and document-opening drops create a
new tab. Import as Layer and canvas drops insert the chosen content at its
original document coordinates, above the active layer, in one undo action.
The destination canvas/PPI is unchanged. Source-image imports are not deduplicated.

## Review and fallback choices

The list shows detected types and conversion status. Select a layer to choose
its conversion beneath the list, with a short description of that option.
Attention needed only and Hide groups start enabled. Hidden group rows do not
hide their layers or change membership; shown groups use subdued text.
Editable conversion is the default where supported. Missing fonts show the
original request and an installed replacement; changing that replacement only
affects runs requesting that face. “Import all text as raster” sets a starting
policy; individual choices can override it.

Choose settings first, then click **Validate Import**. Validation prepares the
chosen layers and canonical preview once, in the background. An owned
confirmation overlay shows the preview, layer/pixel memory and estimated working
memory. **Confirm** imports that prepared result; **Cancel** (or Escape) returns
to the settings. Choices do not trigger rendering. Revalidating unchanged choices
reuses the prepared result; changing a choice discards it.
**Enable preview** starts checked. Turn it off to prepare/import layers without
rendering a preview. Turning it on reuses a current image, or renders from the
prepared layers without decoding them again. If the choices have changed, enabling
preview explicitly prepares the new choices first; it does not confirm the import.
Layer preparation and preview rendering have separate progress stages. The preview
integrates the native-resolution composite in linear premultiplied color, reusing
bounded scanline buffers without reducing imported pixel quality.

- **Import editable:** native raster, rich text, supported vector shape or folder.
- **Import editable** can include a font/layout substitution where indicated.
- **Import as raster:** full-resolution saved layer channels, not a thumbnail.
  Supported mask, layer opacity, blending and modern styles remain separate.
- **Base pixels only:** explicit recovery without unsupported composition,
  masks or effects. This is not a claim to preserve the original appearance.
- **Don't import:** excludes the item and, for containers, its subtree. Visible
  unsupported items require a deliberate action before Validate Import is enabled.
- **Saved composite as one image:** when validated full-resolution compatibility
  pixels exist, an alternative to layered conversion. It cannot honor layer
  exclusions or recover editing structure. No hidden duplicate is added.

No fallback crops the compatibility composite into purported isolated layers.
Corrupt channels report the affected layer; excluding it can recover the rest.

## Supported subset

PSD version 1, 8-bit RGB; raw, PackBits RLE, ZIP and ZIP-with-prediction layer
channels. Saved compatibility composites currently support raw/RLE. PSB,
16/32-bit and non-RGB files are explicitly rejected; there is no untested
depth/mode conversion advertised as a fallback.

Merged transparency is decoded from its white-matted RGB representation before
ICC conversion. This does not affect the separate straight-RGBA layer channels.

Layer names, bottom-to-top stacking, bounds (including off-canvas), visibility,
opacity and resolution are read from records. Decoded saved pixels already
contain their raster geometry; original Smart Object resolution is not invented
and transformation metadata is not applied again.

| Feature | Native conversion |
| --- | --- |
| Raster content/brush artwork | Straight RGBA8 pixels at the stored extent and offset |
| Point text | Unicode, multiline strings, per-run face/size/RGB/alpha, faux bold/italic, left/center/right paragraphs, affine text matrix and baseline |
| Solid vector shapes | One closed polygon/rectangle or recognized four-cubic ellipse; filled or hollow; centered solid stroke; miter/round/bevel joins and butt/round/square caps |
| Layer styles | One modern solid Stroke and Color Overlay; separate enabled state, color, opacity, blend, Stroke position/size and style scale |
| Bitmap mask | Separate grayscale coverage, its own origin/extent/outside coverage, enabled state; remains paintable and undoable |
| Groups | Unstyled fully opaque pass-through hierarchy; visibility retained |
| Adjustment layers | Native Invert and Exposure with zero offset/unit gamma; supported masks and strength. A Normal folder with a direct native adjustment becomes an explicit local domain |
| Clipping chains | Sibling-scoped native Clipping Mask Groups when Blend Clipped Layers As Group (`clbl`) is enabled/default |

The 20 blend mappings are explicit in `PsdImport.cpp`: Normal, Multiply, Screen,
Overlay, Soft/Hard Light, Darken/Lighten, Difference, Exclusion, Hue, Saturation,
Color, Luminosity, Color/Linear Dodge, Color/Linear Burn, Subtract and Divide.
Vulkana's existing linear-light composition is unchanged. Editable style edges,
typography and blend results can differ from the saved composition.

Unsupported items include other isolated/styled groups, unsupported adjustment equations, ungrouped clipping (`clbl=false`), Blend
If, knockout, non-default Fill opacity, paragraph-box/warped/vertical text,
explicit tracking/leading/baseline shifts and unsupported decorations,
other curved/compound vector paths, sheared ellipse strokes, non-centered/dashed vector strokes, other active
styles, and bitmap-plus-vector masks. Mask density/feather, rendered/inverted
mask flags and combined effective/component masks require explicit fallback.
Masks use Vulkana's stable layer-local anchoring; independent unlinked-mask
movement is not introduced by this importer.

Clipping chains never cross PSD group boundaries. The original base is retained
even when hidden. Skipping it requires dependent members to be skipped or
explicitly imported as Base pixels only; another clipped member is never silently
promoted. Converted children retain their own pixels, masks and styles. Review
records the native style ordering when styled chains are involved; see
[clipping groups](CLIPPING_GROUPS.md).

Modern effect records take precedence over duplicate legacy representations.
Disabled dormant effects do not generate warnings. Legacy-only effects are not
mistaken for effects already present in the saved base raster.

## Text, color and geometry

UTF-16 engine strings/run lengths are validated, including surrogate boundaries,
before conversion to native UTF-8 byte ranges. Only the terminal engine carriage
return is removed; other line breaks are retained. Inherited character/paragraph
defaults are resolved. Engine font sizes are local pixel em sizes, with the TySh
matrix applied once; neither monitor DPI nor document PPI scales them again.
Layout starts at the recorded baseline through the existing text service.
Unsupported manual kerning choices are identified as native-layout substitution.

Installed faces are checked against actual font PostScript names, not merely
accepted by `QFont`. The selected native font and original request are persisted
in text metadata, including clipboard/project round-trips. Raster text uses the
saved channels and does not require the missing font. No font is downloaded.

Supported embedded RGB ICC profiles are converted to sRGB through Qt's existing
color conversion. Untagged RGB is assumed sRGB. Masks are coverage, never color
converted. Non-square/out-of-range resolution and unsupported ICC profiles fail
explicitly. Stored shape coordinates are normalized against document dimensions;
live-shape metadata is not reapplied to already positioned vector paths.

## Ownership and limits

Raster import removes empty outer RGBA padding, retaining every nonzero byte
(including hidden RGB and alpha 1) and a one-pixel transparent sampling guard
where it existed. It does not trim white paper, use masks/opacity to discard
hidden artwork, or clip to the canvas. Original layer-local coordinates and
the gradient reference frame remain fixed; mask origins and effects stay live.
Fully empty rasters use a minimal transparent surface and remain paintable.

Transform handles, Move coordinates and snapping use cached nonzero-alpha
content bounds, independently of storage padding. Empty layers retain an editing
frame. Painting extends raster storage as needed within the existing canvas,
crop, selection and allocation limits; it never scales the old pixels. Growth
and painting form one undo action. Erasing, mask painting and repair tools keep
their existing bounded-reference behavior. Existing projects get tight handles
without automatically deleting or repacking their source data.

`PsdReader` is an import-only checked binary reader, with zlib for DEFLATE.
Parser descriptors never enter canonical document data. Inspection produces an
immutable input/capability list; each plan stages a complete native document,
then publication revalidates its intended runtime document, revision, selection
and insertion point. Cancelling does not publish partial content. Switching or
closing tabs is blocked by the existing owned modal import policy.

One shared worker runs bounded/coalesced jobs. Cancellation is cooperative at
read, channel/row, layer and canonical-render boundaries. Old preview generations
are discarded. The source is read once, checked for concurrent changes, and
retained immutably until the review closes. The shipping importer has no Python,
Node, SDK executable, external editor, linked-file fetch or script execution.

Memory admission uses 75% of currently available RAM, respecting finite Linux
cgroup-v2 limits and leaving headroom for the desktop. If available memory
cannot be queried, a 1.5 GiB additional-working-memory allowance is used. There
is no fixed 256 MiB aggregate layer cap. The estimate includes selected retained
raster/mask pixels, one layer's decoder/color-conversion scratch, publication
staging, source/metadata and applicable preview caches. Skipped layers and saved
text/shape rasters replaced by editable models are not charged as retained pixels.
Raster layers are decoded sequentially and admitted at their compact retained
size, with scratch reserved for the largest original layer rectangle.
The review reports estimates; allocation failures still cancel safely. This is
a preflight, not an OS memory reservation.

Structural insertion shares retained surfaces with its undo command. The history
retention target is not a maximum import size; the normal history policy keeps
the latest action and can retire older actions to meet its target.

Format/resource limits remain: 512 MiB input, 4096 layer records, 64 group levels,
48 descriptor levels, and 16,384 pixels per side (or lower GPU limits) / 40 Mi
pixels per raster. Core rendering limits also apply. No automatic resolution
reduction occurs. Text/shape caches are reconstructible; source pixels remain native.

Project persistence streams compressed raster/mask payloads without a fixed
1 GiB aggregate or 2 GiB archive-size cap. Reopening admits new data against
available memory before decoding. Per-surface and format validation still apply;
see [project format](VULKANA_FORMAT.md).

Non-round geometric strokes use the `shape-stroke-v2` project capability so an
older reader rejects unsupported appearance rather than silently rounding it.

## Development checks

`imageeditor_psd_import_tests` builds independent in-memory fixtures; the
interaction target tests tabs, atomic insertion, cancellation and stale targets.
Both run without private files. Optional sample inspection:

```sh
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_psd_import_tests \
  --sample private/reference/Photoshop.psd build/psd-review
VULKANA_PSD_REFERENCE=private/reference/Photoshop.psd \
  build/release/tests/imageeditor_psd_import_interaction_tests --native
```

The private fixture and generated comparisons are never installation inputs.
The Adobe PSD specification, psd_sdk, psd-tools and ag-psd were research references.
No SDK/parser source or research runtime is incorporated in the application.
