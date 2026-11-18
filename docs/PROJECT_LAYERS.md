# Project layers

Choose **Layers → Add Project as Layer…** or **File → Import as Layer…** and select a saved `.vulkana` file. The new layer shows the source canvas at full resolution, including transparency, masks, adjustments, and effects. You can place the same source more than once. Each placement has its own transform, opacity, blend mode, mask, and effects.

Double-click the layer row, or right-click the layer and choose **Open Source Project**, to edit the artwork in its own tab. When you return to the parent, placements using that source update together, including edits you have yet to save in the source tab. Undo in the source to change the artwork. Undo in the parent to restore its previous appearance or undo a placement edit.

Save the parent to keep its current appearance. If the source moves or goes missing, the parent still displays and exports the saved pixels. Keep the original project to edit its layers again.

- **Relink Project Layer** points a placement to another local project.
- **Refresh Project Layer** reads the latest appearance from the open source or its saved file.
- **Convert to Raster Layer** makes the placement independently paintable. Undo restores its link.

Projects can contain other project layers. A link that would make a project depend on itself is rejected. Exporting a layered PSD writes the Project Layer's appearance as a raster layer.
