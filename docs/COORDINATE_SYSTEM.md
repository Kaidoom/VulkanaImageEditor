# Coordinate system

ImageEditor uses one top-left, Y-down convention from canonical pixels through canvas interaction:

- A `RasterSurface` stores unpremultiplied RGBA8 rows in display order. Pixel `(0, 0)` is the top-left pixel, rows increase downward, columns increase rightward, and integer pixel rectangles are half-open.
- Layer-local and document coordinates start at the top-left and increase right and down. An identity raster-layer transform places its top-left texel at document `(0, 0)` without changing CPU row order.
- Qt logical viewport positions, including pointer events and Vulkan overlay inputs, start at the canvas window's top-left and increase right and down. `ViewportState` maps between those logical positions and document positions.
- The renderer scales logical viewport positions to physical framebuffer positions per axis. Checkerboard bounds, canvas borders, transform overlays, tool cursors, and raster-layer geometry all use this same mapping.
- The document canvas is a visibility boundary, not a destructive crop. Raster layers are clipped against the half-open document rectangle `[0, width) × [0, height)` in framebuffer space. A conservative Vulkan scissor bounds layer work and the fragment shader applies the exact floating-point edge. Pixels and layer transforms outside that rectangle remain unchanged and can become visible after panning a layer or enlarging the canvas.
- CPU rows are copied to the Vulkan image unchanged: source row `y` is uploaded to image offset `y`. Layer UV `(0, 0)` samples the top-left texel and UV `(1, 1)` reaches the bottom-right edge.
- With the renderer's positive-height Vulkan viewport, NDC `(-1, -1)` maps to framebuffer `(0, 0)`. Therefore the document-to-clip boundary preserves Y direction; it does not apply an OpenGL-style Y inversion. Vulkan `gl_FragCoord` is likewise treated as top-left, Y-down.

`CanvasCoordinateMapping` owns the renderer-side conversion, exact canvas framebuffer rectangle, and conservative clipped scissor. The four-color `tests/assets/orientation-corners-reference.png` fixture and `imageeditor_raster_orientation_tests` guard production image loading plus the top-left/top-right/bottom-left/bottom-right geometry, overlay, pointer mapping, and canvas clipping without mutating `RasterSurface` data. Open the same fixture on the Vulkan canvas to check shader sampling visually.

## Selection coordinates

Selection is a document-space R8 plane, never a layer. Rectangle endpoints snap independently to the nearest document-pixel edge after clipping to the canvas, yielding half-open rectangles in any drag direction. A stationary click creates active-empty coverage; it does not mean deselected. Zoom and display scaling do not affect mask pixels or modifier semantics. Cached Vulkan contour endpoints use those same pixel edges; ant width and dash length use logical screen pixels.

Exception for an existing selection: in unmodified Replace, a press over nonzero coverage moves the mask; a stationary press/release there is a no-op. Holes and gaps are not draggable interiors. Movement rounds the displacement from the original press to integer document pixels, independently of zoom/DPR. The starting mask survives off-canvas previews; committing clips the result, and undo restores the original. Grow/shrink X acts along document left/right, Y along top/bottom, with signed amounts per side. The rectangular R8 max/min kernel is applied horizontally then vertically for mixed signs, with zero coverage beyond the canvas.

Positive selection rotation is clockwise in the Y-down document convention. The pivot is the center of the starting mask's nonzero bounds (possibly half-pixel), held constant throughout one numeric adjustment. Inverse bilinear filtering preserves partial R8 coverage, with zero extension outside the canvas. Exact quarter turns avoid trigonometric drift; quarter turns are lossless when the pivot maps pixel centers onto pixel centers. Every repeated step samples the original adjustment mask, not the preceding preview. Rotate by is relative and resets to 0° after completion; the next adjustment takes a new baseline/pivot.

The optional Ctrl+T selection gizmo uses entry bounds as its local rectangle. Its matrix maps bounds-local coordinates to document coordinates; subtracting the entry bounds origin before that matrix yields the original-document-to-new-document mask mapping. Preview only maps contours. Apply inverse-bilinearly samples original R8 pixel centers, preserving holes/partial coverage and clipping once; integer translations are exact. Whole-layer transforms and source pixel grids are unchanged. Heavy minification can lose thin coverage with this bilinear filter.

Brush/Erase evaluates selection at `floor(localToDocument(sourcePixelCenter))`. The layer's own pixel grid remains authoritative, including flips and rotations. Magnified native pixels can extend visually across a selection boundary; no selection rendering mask is added to the layer. Layer via Copy instead samples document-pixel centers into a newly cropped/document-aligned surface and multiplies alpha by R8 coverage once, retaining source opacity separately. Canvas resizing is top-left anchored and undo restores the prior mask and extent together.

## Layer transforms

Layer Crop adds an optional rectangle in **stable layer-local pixels**. It never
rebases the source, transforms it, or changes the document canvas. Crop X/Y are
the local frame origin (unlike Move's document-space center); W/H are local
dimensions. The output pixel footprint is mapped through the layer inverse and
analytically intersected with the crop for antialiased visibility after
adjustments. Source/cache bounds, effective visible bounds and the crop frame
are distinct. Move/Ctrl+T uses the cropped effective frame, applies its affine
delta to the unchanged source transform, and therefore carries the crop with
the source. Group frames ignore empty-cropped members when measuring bounds,
but still transform those members so later uncropping preserves placement.

Transform position is the layer's **center in document pixels**, not its screen-space bounding-box minimum. Scale is relative to the original raster dimensions, with independent signed X/Y scales; positive rotation is clockwise. A preserved QR shear term keeps pre-existing affine geometry representable without adding a shear control. The decomposition is not unique: reopening a horizontally flipped layer can express the same matrix as a 180° rotation plus a negative Y scale. The pixels and local handle identities do not change.

Within an active transform session, local Undo/Redo restores signed numeric checkpoints as well as exact matrices, so horizontal-flip fields do not unexpectedly switch to a different equivalent decomposition. Ordinary Move auto-selection uses continuous document coordinates (not the Eyedropper's containing-pixel center) and bounded alpha filtering in transformed layer-local space. This prevents thin or flipped visible content from selecting an adjacent transparent document pixel.

Corner/edge identities remain clockwise in **layer-local** space through flips. Affine scaling uses the frozen press-time inverse, handle and opposite anchor; Alt chooses the layer center instead. Projective resizing follows forward handle trajectories. Both resolve from the gesture baseline, with aspect-constrained motion projected onto its axis/diagonal. The transform is resolved in document space before the Vulkan quad and overlay are built. Shift rotation snaps to absolute document axes in 15° increments. Rotation accumulates shortest-arc pointer-angle differences across ±180°.

Exact zero scale is intentionally excluded: signed magnitudes clamp to `[1e-6, 1000]`, retaining the previous sign at exactly zero and switching sign as the pointer crosses. This permits flips without undefined inverse mapping. Pointer hit testing uses 9-logical-pixel handle radii and outside-corner rotation zones extending to 25 logical pixels, independently of viewport zoom or DPR. The 10-logical-pixel handle squares and bounding lines render in Vulkan after the canvas-clipped layer pass; they remain visible over the gray workspace outside the document. Middle/Space pan and wheel zoom remain available between transform gestures to reach off-screen handles.
