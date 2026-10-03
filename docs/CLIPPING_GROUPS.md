# Clipping Mask Groups

A clipping mask group is an expandable container. Its bottommost direct child
is the **Base**; all other children share that child's coverage. Reordering or
removing children changes the base immediately. Hidden bases are not replaced
by the next visible child. Empty groups render nothing; one-child groups render
that child normally.

**Add to Clipping Mask Group** collects selected roots at the topmost selected
position, in document order. It never takes an unselected neighbor. An existing
group can be converted in place. Conversion back and Ungroup release clipping
without rasterizing. Each operation is one structural undo action.

Normal Add and import operations targeting the container insert at its top;
targeting a child inserts above that child. Explicit tree drops can choose the
bottom and thus change the base. Children keep their own document-space
transforms, editable data, masks, styles and visibility.

## Composition

All calculations use the existing linear-light premultiplied-alpha contract.
The structural plan is shared by CPU sampling/export and Vulkan dispatch.

For a renderable base, let `P` be its adjusted/filtered premultiplied content,
and `a = P.alpha`. Crop/chamfer and bitmap-mask coverage are `m`.

1. Initialize the stack color to `P.rgb / a` with working alpha 1 when `a > 0`.
2. Evaluate upper children in bottom-to-top order, with their own blend modes,
   opacity, masks, crops and styles. Containers evaluate their subtrees.
3. Restore the base coverage: `P' = (working.rgb * a, a)`.
4. Evaluate the base's styles on `P'`, then its crop/mask and final opacity/blend
   through the existing style/composition boundary, exactly once.

Zero alpha contributes no hidden RGB. Without exterior styles, an alpha-0.5 base
remains alpha 0.5 regardless of the number of opaque Normal upper members.
Base opacity multiplies the completed contribution, not each member separately.
Base RGB never determines clipping coverage. Exterior base shadows/glows do not
expand the silhouette. Upper styles stay constrained by it.

Base style geometry still comes from the original pre-style silhouette. Interior
base overlays are applied after stack colors. This order is a native policy;
PSD review identifies styled clipping conversions that can differ.

A container base has no external opacity/blend property. Evaluate its subtree
over transparency twice: with styles (`S`) and without styles (`C`, the content
silhouette). Initialize upper composition from `S`'s unassociated color. Change
only its content-covered color:

`result.rgb = S.rgb + (working.rgb - S.rgb/S.alpha) * min(C.alpha, S.alpha)`

`result.alpha = S.alpha`

Composite that result as Normal. Thus base-subtree exterior styles stay present
but do not reveal upper members. This local isolation applies only when a
container is the base of a multi-child clipping group. Ordinary groups remain
pass-through elsewhere; one-child clipping groups introduce no isolation.

## Editing, references and baking

Clipping restricts output, not storage. Painting outside the silhouette retains
pixels that may become visible after movement. Layer-panel targeting and mask
editing remain possible even when clipping hides everything. Canvas hit testing
checks ancestor clipping coverage. Explicit intrinsic sampling remains raw;
rendered references, Pixel Preview and exports evaluate clipping.
An active-layer rendered reference keeps the existing unblended single-layer
color policy and applies ancestor coverage; a merged reference evaluates the
complete clipping stack and its blends.

Content/mask row thumbnails retain their existing source-editing role. Composite
group thumbnails evaluate the hierarchy. The clipping container has a distinct
scalable icon, expandable children and a Base badge.

Whole-group Merge or Rasterize commits its isolated appearance once to a Normal
raster. Exterior backdrop interactions are evaluated against transparency, as
with other selected-only bakes. A partial Merge includes no unselected base
pixels: it retains clipping only when the original base is included. A child
copied out of the group is independent. Rasterizing an individual child commits
its own internal appearance and leaves its structural membership intact.

PDF Layers as Pages treats the group as one item. Interacting native text falls
back to canonical raster composition. Project files declare the essential
`clipping-mask-group-v1` capability and store `kind: "clipping-mask-group"`;
there is no separately persisted base ID or duplicated child mask.

## Resources

CPU native output composes bounded scanlines, retaining the aligned raster read
path. Vulkan allocates tight framebuffer-space working rectangles, not a
document-sized mask for each child. Base source and crop/mask coverage are cached
per frame slot with document-instance, content, geometry and mask dependencies.
Upper-only edits reuse them; unchanged frames reuse the final composition and
source textures. Retained buffers follow frame-fence ownership.

The clipping work limit is the smaller of 512 MiB and the device's storage-buffer
range. A leaf-base stack uses 36 bytes per evaluated framebuffer pixel (RGBA
working/source plus scalar coverage). Nested container bases need additional
bounded subtree work. Buffer capacity is included in renderer memory accounting.

## Tests

```
ctest --test-dir build/release -R 'clipping_group|psd_import' --output-on-failure
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_blend_rendering_tests --clipping --validation
QT_QPA_PLATFORM=offscreen build/release/tests/imageeditor_blend_rendering_tests --clipping --benchmark --validation
```

Independent alpha fixtures cover fractional coverage, hidden/zero-opacity bases,
masking, partial bakes, hierarchy changes, undo, cross-document copy and project
round-trips. Vulkan comparisons cover all supported blend modes, projective
mapping, nested containers, styles, typed content, fractional display scale,
unchanged-source upload reuse and cached base reuse.
