# Mirrored layers, effect copying and guides

## Mirrored layers

Right-click a color raster, text or shape layer and choose **Create mirrored layer**. The new layer follows its source's content and initially shares its position and appearance. Move or transform the mirror independently; later source transforms do not move it.

The layer row shows **Mirrored**. Its content, crop, mask, adjustments and filters follow the source and are read-only. Use **Select source** to edit that content, or **Promote to full layer** to make an independent editable layer. Painting and other content-changing actions are disabled on mirrors; selection tools, sampling, effects and whole-layer transforms remain available.

Layer opacity, pixel opacity and blend mode follow the source until changed on the mirror. The chain button next to the opacity control restores the source link for the displayed opacity type. **Sync with source** clears all appearance overrides while preserving the mirror's transform, name and visibility.

Effects inherit independently:

- Changing an effect's settings creates an override for that effect.
- Turning off an overridden effect restores the source's effect, which may still be enabled.
- Turning off an inherited effect disables it on the mirror. Re-enabling it restores inheritance when the source's effect is enabled and its settings were not changed.

Deleting a source promotes surviving mirrors to independent layers, preserving their current appearance. Creating, promoting and synchronizing mirrors, changing overrides, and deleting sources support undo/redo.

Native projects save source identities and overrides without duplicating mirrored raster pixels. The renderer shares the source's pixel storage and uploads; each layer retains its own transform and appearance caches. PSD export writes ordinary layers with the resolved appearance. Mirror relationships require Vulkana's native format; older versions that do not support mirrors reject these projects instead of dropping linked content.

## Copy and paste effects

Use **Copy Effects** and **Paste Effects** in a layer's context menu. Paste replaces the target's complete effect stack, including disabled effects and the layer-wide effects options, in one undoable operation. It leaves the target's pixels, masks, adjustments, filters, transform, opacity and blend mode unchanged.

Pasting onto a mirror overrides its effect stack. Use **Sync with source** to restore inherited appearance, or turn off an individual overridden effect to restore that effect's source settings.

## Guides

Select **Guides** in the tools panel. Choose **Horizontal** or **Vertical** and set **Count** in the top toolbar. The mouse wheel over the canvas adjusts the same count; changing orientation preserves it.

- One preview guide sits at the canvas center. Multiple guides divide the canvas into evenly spaced sections. Move placed guides individually as needed.
- Left-click empty canvas to place the preview. Placement remains active afterward.
- Hover near an existing guide to highlight it, then left-drag to move it or right-click to remove it. Existing guides take priority over placement.
- Press **Esc** to cancel the current placement or drag without removing saved guides.

Use **Clear guides** in the Guide toolbar to remove all guides in one undoable step. **View → Guides** also offers show, hide and clear controls. Enable the **Guides** target under **View → Snap to** to use them with the existing snapping controls. Dragging a guide snaps to visible layer edges and centers, and canvas edges and center, using the same snapping settings as layer movement. A mint-green indicator marks the snapped position. Hold **Ctrl** to move freely. Visibility and snapping are separate preferences. Other tools can snap to guides but cannot drag or delete them.

Guide positions use document coordinates, so zooming and panning do not change their placement. Native projects retain the guides; placing, moving, removing and clearing them is undoable. Guides are canvas overlays and are excluded from rendered snapshots and image exports.
