# Brush Symmetry

Brush Symmetry paints a reflected copy of each brush mark on the same target. It works with Brush, its erase mode, and layer-mask painting.

Enable the symmetry button in the Brush toolbar, then click or drag on the canvas to place the target. Placement snaps to canvas edges and center, visible layer bounds, and guides using the existing snapping settings. A mint indicator shows the snapped position. Hold **Ctrl** to place freely. Placement does not paint. **Esc** cancels placement. The target and axis remain visual overlays and do not appear in exported images.

The options button beside **Symmetry** offers:

- **Vertical:** mirror left/right across a vertical line through the target.
- **Horizontal:** mirror top/bottom across a horizontal line through the target.
- **Reposition target:** move the symmetry target without painting.

During ordinary painting, the target does not intercept strokes. A second brush cursor previews the reflected position. Toggle the toolbar button off to paint normally; enabling it again restores the saved position. Each document retains its own symmetry setup, including after saving and reopening a native project.

Both sides form one undoable brush stroke and use the same pressure, spacing, smoothing and brush settings. Existing selections, masks, crop bounds and layer transforms still control where painting can occur. An asymmetric selection can clip one side independently. Brush Symmetry does not make other layers editable or bypass mirrored-layer restrictions.

Changing the symmetry setup and repositioning its target are undoable independently of painting. Symmetry is not applied to Clone Stamp, healing, local blur, selections or refinement brushes. The first version supports one vertical or horizontal reflection; diagonal, radial and multi-axis modes are not included.
