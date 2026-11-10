# Channel Groups

Channel Groups edit the independent 8-bit values in packed RGB/RGBA textures. Each channel is a real raster layer with compact single-channel storage.

## Open and convert

Use **File → Open data texture…** when the image contains numeric data. This preserves decoded 8-bit channel values without converting the image's colour profile. Higher-precision, premultiplied and CMYK inputs are rejected rather than silently converted.

Right-click a compatible raster layer and choose **Convert to Channel Group**. The conversion preserves its currently stored RGBA bytes, including RGB beneath zero alpha. It cannot recover values previously changed by a colour-managed import or a lossy codec.

Conversion requires a raster without an active mask, crop, non-normal blending, reduced layer opacity, or active adjustments, filters or effects. Bake the appearance explicitly first if that is the intended input.

The group contains R, G, B and A layers. Their roles are fixed independently of their names and display order. Rename them to describe their contents, such as roughness or metallic. All channels share the same pixel grid and transform.

## Inspect and edit

Selecting a channel shows its opaque grayscale values. Selecting the group shows packed RGB without alpha hiding its RGB data. **Show alpha as transparency** enables alpha-aware inspection. These views do not change document pixels or export content.

The Color panel provides a grayscale ramp and a **Value** control from 0 to 255. Ordinary primary and secondary colours are restored when leaving channel editing.

Supported editing includes:

- Brushes, eraser and fill, with pressure, brush strength and soft selections.
- Rectangle/ellipse and freehand selections.
- Ctrl-click a channel thumbnail to load its values as selection coverage. An all-zero result leaves the current selection unchanged.
- Channel cut/copy/paste and undo/redo.
- Clone Stamp, Healing Brush, Spot Healing and Local Blur.
- Non-destructive Levels and Curves using the Value channel, Invert, and Gaussian Blur.

Painting and interpolation operate on numerical values without sRGB conversion. For example, 50% white over black produces approximately 128. Erasing moves values toward zero; zero remains valid data, including in the A channel.

Internal clipboard data stores values separately from selection coverage. Paste applies that copied coverage once and preserves the current selection. External straight-alpha 8-bit grayscale images can also be pasted. Coloured clipboard images require explicit conversion before channel paste.

For cloning or healing, select the source channel and Alt-click the source position. Then select the destination channel and paint. Retouching uses **Source Layer** and the raw channel pixels; it does not sample a packed colour composite or procedural adjustments. Zero-valued pixels remain valid repair context. Rendered-source choices are disabled without overwriting their settings for ordinary colour layers.

Gaussian Blur keeps the channel's original extent and clamps samples at its edges. It does not wrap samples across opposite texture edges. Procedural adjustment/filter masks blend their numerical result through the captured selection.

## Save, export and convert back

Native `.vulkana` projects preserve channel planes, role mappings, labels, geometry and supported processing settings. Older applications that do not understand these project capabilities reject the project instead of discarding channel data.

**Convert Channel Group to Raster** packs the evaluated channels into one numeric RGBA raster. With no processing or pixel edits, conversion back preserves the original pixel bytes exactly. Supported procedural processing is evaluated when packing; undo restores the editable group.

Packed-data rasters remain suitable for lossless storage, duplication and export. Convert them to a Channel Group before pixel editing. This prevents ordinary colour/alpha tools from accidentally changing packed numerical data.

Use **File → Export channel data…** to export the selected channel, Channel Group or packed-data raster. A document consisting of one numeric raster or Channel Group also uses data export through the normal Export command.

Data export uses PNG at the layer's native pixel-grid dimensions. It preserves hidden RGB, omits colour-profile tagging and disables matte replacement, lossy formats and resizing. Canvas placement and transforms are not baked into this native-grid export. Preview shows the actual output. An individual channel exports as opaque grayscale.

Mixed-document visual exports are composited images; use Export channel data for a specific packed texture. PSD export is unavailable while numeric channel layers are present because PSD's ordinary layer/group model does not preserve Channel Groups.

## Limits and implementation

Channels currently use unsigned 8-bit values. Existing image-size limits remain unchanged. Channels cannot be independently resized, cropped, hidden, reparented or assigned blend modes, opacity, masks or decorative effects. Transform or duplicate the whole group. AI, colour-specific adjustments, eyedropper sampling, colour-based selection/refinement and unrelated processing are disabled for numeric targets. Use the Value control or clone from an explicit channel.

Canonical storage uses immutable copy-on-write R8 tiles, shared with the storage primitives used by selection masks. Pixel values, sampling validity and edit coverage remain separate. History retains native R8 patches; Vulkan uploads dirty R8 regions. Native save data does not contain redundant RGBA copies of channel planes.

Retouching uses immutable scalar snapshots and bounded working regions. Gaussian filtering uses one-component float intermediates and an R8 derived cache. A valid cache is reused; changes currently rebuild the affected channel's Gaussian cache. Incremental filter-halo regeneration, sharpen, wraparound retouching, independent channel geometry and higher precision are not implemented.

MCP inspection reports channel roles and storage interpretation. Individual brush/fill calls and supported numeric adjustments work on channel layers. Unsupported operations fail before mutation. Multi-stroke batching and MCP alpha-selection are currently unavailable on scalar channels.
