# Project layers

Choose **Layers → Add Project as Layer…** or **File → Import as Layer…** and select a saved `.vulkana` file. The new layer shows the source canvas at full resolution, including transparency, masks, adjustments, and effects. You can place the same source more than once. Each placement has its own transform, opacity, blend mode, mask, and effects.

Double-click the layer row, or right-click the layer and choose **Open Source Project**, to edit the artwork in its own tab. When you return to the parent, placements using that source update together, including edits you have yet to save in the source tab. Undo in the source to change the artwork. Undo in the parent to restore its previous appearance or undo a placement edit.

Select the layer’s **content thumbnail** and use the Brush to paint on this placement. Strokes stay within the source image, move with the placement, and survive source updates. If a refreshed source is smaller, paint outside its new bounds remains stored for when the source grows again.

The Bucket’s **Contiguous** mode finds connected colors in the linked image and any existing paint, then fills that region on the placement’s paint overlay. **Selection / Layer** and the foreground/background fill shortcuts write to the same overlay.

The Eraser removes local paint and reveals the linked artwork beneath. Press **Delete** to clear paint inside the selection, or all paint when there is no selection. A small brush icon in the layer row indicates that the placement contains paint when layer info is shown.

To hide parts of the linked artwork itself, add a layer mask and paint on its thumbnail. Use **Delete Selected Layers** to remove the entire placement.

Save the parent to keep its current appearance. If the source moves or goes missing, the parent still displays and exports the saved pixels. Keep the original project to edit its layers again.

- **Relink Project Layer** points a placement to another local project.
- **Refresh Project Layer** reads the latest appearance from the open source or its saved file.
- **Clear Paint Overlay** removes all strokes on this placement in one undo step.
- **Convert to Raster Layer** bakes the source appearance and its paint overlay into ordinary raster pixels. Undo restores the link and editable overlay.

Projects can contain other project layers. A link that would make a project depend on itself is rejected. Exporting a layered PSD writes the Project Layer's appearance as a raster layer.
