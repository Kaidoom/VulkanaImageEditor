# PSD export

Choose **PSD** in Export. Layered output includes the whole hierarchy, including
hidden content. Layer and pixel selections do not restrict it. Flattened output
is an explicit alternative containing one ordinary visible-composite pixel layer.
Keep `.vulkana` as the editable master.

PSD format version **1**, 8-bit RGB, straight layer transparency, an sRGB ICC
profile and document PPI are written. The intended application baseline is modern
desktop Photoshop 2020+; this is not a claim that every release has been tested.

## Supported output and choices

| Native content | Layered output | When conversion is needed |
| --- | --- | --- |
| Raster | Exact samples for integer translations; native-size channels and off-canvas offsets | Other affine/projective transforms baked once from the source at document resolution; original pre-transform resolution is not retained |
| Text | Fresh type/engine records, Unicode, rich font/size/color runs, multiline point text, paragraph alignment and affine placement | Projective text, actual glyph fallback inside a run or synthetic faces become a reviewed pixel choice; Rasterize all text is also available |
| Shapes | Rectangle, ellipse, simple polygon/triangle; solid or no fill, centered solid geometric stroke | Rounded rectangle/line, translucent fill, projective geometry or nonuniform/sheared stroke becomes pixels |
| Bitmap mask | Separate user channel with independent rectangle, black/white outside coverage and enabled state; original covered pixels retained | Non-integer/transformed grids, fractional outside coverage, or combined vector geometry plus bitmap mask are baked together with layer processing |
| Styles | Fresh editable Stroke, Drop/Inner Shadow, Outer/Inner Glow, Color Overlay and two-color Gradient Overlay records | Editable **approximations**, requiring review: edge profiles, softness/spread, gradient interpolation/anchors and clipping differ. Nonuniform/projective style transforms require fallback |
| Adjustments, spatial filters, crop/chamfer | Their result, not fake adjustment layers or Smart Filters | Baked together with the layer mask/styles in native processing order; baked state removed from the exported record |
| Ordinary folders/groups | Pass-through hierarchy, order, visibility and labels | Optional explicit subtree consolidation is isolated, not a sample of outside content |
| Clipping Mask Group | Container plus real clipped-sibling flags and Blend Clipped Layers As Group | Nested containers as members/bases require consolidation; styled bases have an explicit approximation note |

The conversion list sits beside the composite preview, with format options below.
Supported groups/folders and clipping groups map automatically in both directions;
they do not require a conversion choice. Choices remain available for actual
conversion issues, including an enclosing consolidation when necessary.
Attention needed only and Hide groups start enabled. Hiding groups changes only
the list: their children remain available and group membership is still exported.
Visible groups use subdued text. Each conversion option has a short description.
Clicking Export accepts the current choices; invalid plans disable Export until corrected.
The preview is the proposed native composite, not a simulation of another editor's
redraw.
**Enable preview** starts checked. With it off, planning and conversion validation
continue without rendering the on-screen composite. Enabling it reuses a current
preview or renders the latest choices. Export always writes the required fresh
compatibility composite, regardless of this display setting.

Consolidating a folder includes its accepted children in an isolated transparent
backdrop. It can change interactions with layers outside that folder. A
backdrop-dependent exterior effect cannot be silently flattened into a Normal
pixel layer; retain its editable approximation, consolidate an appropriate
explicit group or use Flattened PSD. Removing a clipping base while retaining
members is rejected, including inside a consolidation.

Vulkana's blend math/linear-light alpha contract is unchanged. The 20 named
blend modes have explicit PSD identifiers, but the receiving editor's color and
blending settings affect recomposition. The sRGB profile does not set blend
gamma. No global editor preferences are changed. Flattened PSD is the
appearance-focused route when live recomposition differs.

## Text, geometry and pixels

Text uses the current resolved PostScript font faces, not stale imported
descriptors or missing original requests. Fonts are referenced, **not embedded**.
The receiving computer needs those faces. Strings and style runs are written
with UTF-16 lengths, including surrogate pairs and the engine terminal character.
Sizes are document-pixel em sizes; the baseline and affine matrix come from the
existing text layout. Document PPI is separate from monitor DPI. Multiline leading
is approximate across text engines and explicitly reviewed.

Vector paths are generated from current native geometry, never traced from the
preview. Layer channels omit any mask or style that remains live. User mask
coverage is not ICC-converted. Native mask anchoring becomes an ordinary user mask;
independent unlinked mask movement is outside V1.

The compatibility composite is freshly rendered from the same accepted snapshot.
Omitted content is absent from both records and composite; there is no hidden
full-scene overlay. PSD merged transparency uses white-matted encoded RGB plus
alpha; layer channels remain straight RGBA, including hidden RGB. Unmatting the
8-bit composite can amplify rounding at very low alpha; layer channels retain
their full eligible byte values. All image/channel compression is lossless
PackBits RLE. No source PDF/PSD, project archive, linked files, scripts, private
paths or original document metadata is attached.

## Lifetime, limits and writing

The owner-thread capture freezes source pixels and immutable settings with the
runtime document ID/revision. One background worker plans/previews/writes; obsolete
previews are cancelled. The owned Export overlay follows the existing modal
workspace policy, so switching or closing its owner is refused while it is open.
Successful settings update only that owner. No history, native save association,
selection, visibility or project checkpoint is changed.

One channel spool is shared across layers. Compression keeps one row plus the
current raster in memory; it does not keep a full compressed PSD in RAM. Explicit
consolidations retain their replacement raster until writing completes. Preflight
accounts for the frozen source, retained replacements, rendering/transfer scratch
and the existing renderer's bounded derived resources. Interactive admission uses
75% of available memory, including cgroup constraints; it never silently lowers
quality. The current renderer imposes **64 Mi pixels per output, 1024 layers** and
bounded typed/effect cache limits. These are separate from PSD format limits.

Standard PSD output is limited to 30000 pixels per dimension/channel, signed
32-bit offsets, 32767 layer/group-marker records and less than 2 GiB total file
size. Lengths, RLE row tables and sections are checked again after encoding.
PSB, high-bit-depth/CMYK, Smart Objects/Filters and animation are not emitted.

Writing uses temporary files, then a structural verification walk and atomic
`QSaveFile` publication. Errors/cancellation preserve an existing destination.
Cancel keeps the Export panel available for retry; Close cancels and closes.
Replacing an imported source PSD requires a separate explicit confirmation.
Extra disk headroom is required for the channel spool and atomic output copies.

## Development checks

```sh
cmake --build --preset release
ctest --preset release -R 'psd_(export|import)'
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_psd_export_tests build/psd-export/fixtures
# Optional independent development environment: psd-tools 1.23.0 + Pillow.
python tests/PsdExportIndependent.py build/psd-export/fixtures
QT_QPA_PLATFORM=wayland build/release/tests/imageeditor_psd_export_interaction_tests --validation
QT_QPA_PLATFORM=xcb build/release/tests/imageeditor_psd_export_interaction_tests --validation
```

The native interaction checks require a real compositor/GPU and the validation
layer; ordinary app use does not. XCB on XWayland is not a native X11 test.
No Python, Node, external editor or new library is required by the exporter.
Private PSDs and generated comparison files stay in ignored directories.

Format research: [Adobe PSD specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/),
[ag-psd writer/descriptor documentation](https://github.com/Agamnentzar/ag-psd),
[psd-tools structure reference](https://psd-tools.readthedocs.io/).
The application contains its own scoped C++ writer; these independent projects
are development references, not bundled production code.
