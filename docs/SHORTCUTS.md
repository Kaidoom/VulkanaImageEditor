# Configurable shortcuts and focus routing

## Preferences

Edit → Preferences → Shortcuts provides searchable commands, Change, Clear binding,
and Restore defaults. Select a command or double-click its row, then press one key
or chord. This V1 captures single chords, not multi-step sequences.

Escape cancels capture or its pending conflict prompt; a subsequent Escape closes
Preferences. Escape is never assignable. A conflicting assignment shows the
affected commands inline. Confirming assigns the new chord and clears their
bindings; cancelling changes nothing. Mutually exclusive operation contexts may
share a chord (for example Apply operation and Finish text).
Shift variants of nudges and construction completion/removal are included in
conflict detection.

Changes preview immediately. Apply saves them; OK saves and closes. Cancel
restores the last applied state, including menu hints and tool help. Bindings are
stored in per-user QSettings under `shortcuts/v1/<stable command ID>`; an explicit
empty list disables a command. Missing entries use defaults. They are not project
content and do not affect document history. Existing settings are preserved.

## Ownership and fixed keys

- Meaningful focused controls get first use of their editing/navigation keys:
  text fields type/edit text, dropdowns use arrows/Enter, sliders/spin controls
  step, and Layers arrows navigate rows or expand/collapse containers.
- Completing an adjustment keeps the clicked slider/curve focused. This only
  reserves local editing keys, not every shortcut: global commands still work,
  and Save/Save as also finish pending numeric entry before saving.
- Clicking canvas returns ownership to canvas movement. Nudges move layers or
  pixel-selection masks by 1 document pixel; Shift uses the General preference
  (default 10 px). One binding per direction covers both contexts.
- Ctrl+T remains one contextual command for layers and selection boundaries.
  Shift+T independently starts **Transform Selected Pixels** on the primary raster;
  Select also exposes **Transform Selection** without a default binding.
  Ctrl+drag on an explicit transform corner distorts that corner; Ctrl inside the
  frame bypasses snapping during translation. Ctrl can be pressed or released
  during a corner drag.
  Finish is one command
  for Ctrl+T, Crop, and lasso/polygon completion. Right-click completion is unchanged.
- Remove point is **Backspace**, not Delete: it removes a lasso anchor, polygon
  vertex, or focused curve point. Delete on a focused curve does not erase artwork.
- Delete in the canvas erases selected raster pixels, or clears a compatible raster
  layer when there is no selection. It never deletes editable text/shapes/containers.
- Plain Enter in text inserts a newline. Finish text is its own configurable
  command (default Ctrl+Enter). Standard character editing, selection, clipboard,
  and navigation keys stay local to text; conflicting text-command assignments
  are rejected. Escape retains the existing IME-cancel / finish-text behavior.
- Tab/Shift+Tab do not cycle control focus, including in dialog cards. They can
  be assigned as shortcuts. Literal tabs in text editors and OS chords are unchanged.
- Escape, inline Rename Enter/Escape, and gesture modifiers
  (Shift constraints/add selection, Alt duplicate/source/picker, Ctrl snap bypass,
  Ctrl/Shift layer-click selection) are fixed and not exposed as bindings.
- Menu mnemonics and Alt-to-menu focus are disabled; explicit Alt chords such as
  reveal layers or fill remain available.
- Hold Alt in Brush, Eraser or Fill for temporary eyedropper sampling. A sampling
  drag keeps its ownership until release, even if Alt is released first.
- Persistent Measure (Shift+R) and temporary Measure (hold R) are independent
  commands. Held access tracks the physical key until release, even if Shift
  changes or focus moves. Changing tool bindings does not alter gesture modifiers.
- Brackets are one shared size pair for Brush/Erase, Cloning, Local Blur, and
  Quick Selection. Pan is also configurable as a hold command.
- Native canvas and panel-overlay input use the same dispatcher and actions.
  QWidget controls keep local editing; rebinding removes the old editor fallback.

## Default command inventory

### File

| Command | Default |
| --- | --- |
| New document | Ctrl+N |
| Open document | Ctrl+O |
| Close document | Ctrl+W |
| Next document tab | Ctrl+Tab |
| Previous document tab | Ctrl+Shift+Tab |
| Save | Ctrl+S |
| Save as | Ctrl+Shift+S |
| Import image as layer | Unassigned |
| Export image | Ctrl+Shift+E |
| Export again | Unassigned |
| Quit | Ctrl+Q |

New/Open add tabs without discarding the active document. Close applies to the
active tab; Quit checks all dirty tabs before destroying any. Tab shortcuts also
work while editing text: committed typing is retained when switching documents.

### Edit

| Command | Default |
| --- | --- |
| Undo | Ctrl+Z |
| Redo | Ctrl+Shift+Z or Ctrl+Y |
| Paste image as layer | Ctrl+V |
| Preferences | Unassigned |
| Transform layers / selection | Ctrl+T |
| Transform Selection (boundary only) | Unassigned |
| Transform Selected Pixels | Shift+T |
| Erase selection / clear raster layer | Delete |
| Fill with foreground | Alt+Backspace |
| Fill with background | Ctrl+Backspace |

### Editing operations

| Command | Default |
| --- | --- |
| Apply / finish canvas operation | Return |
| Remove point / last anchor | Backspace |
| Finish text editing | Ctrl+Return |
| Nudge layers / selection left | Left |
| Nudge layers / selection right | Right |
| Nudge layers / selection up | Up |
| Nudge layers / selection down | Down |

### Layers

| Command | Default |
| --- | --- |
| New raster layer | Ctrl+Shift+N |
| Delete selected layers | Shift+Delete |
| Duplicate selected layers | Shift+D |
| Rename layer / folder / group | F2 |
| New folder | Unassigned |
| Group selected | Unassigned |
| Ungroup | Unassigned |
| Merge selected | Unassigned |
| Rasterize layers | Unassigned |
| Hide selected | H |
| Reveal selected | Alt+H |
| Isolate selected | Shift+H |
| Show all layers | Shift+Alt+H |

### Selection

| Command | Default |
| --- | --- |
| Select all pixels | Ctrl+A |
| Deselect | Ctrl+D |
| Invert selection | Ctrl+Shift+I |
| Reselect last selection | Ctrl+Shift+D |
| Layer via copy | Ctrl+J |

### Tools

| Command | Default |
| --- | --- |
| Move | V |
| Layer crop | C |
| Select | M |
| Lasso | L |
| Select by color | Shift+O |
| Smart select | W |
| Brush | B |
| Toggle erase mode | E |
| Cloning | S |
| Local blur | K |
| Fill | G |
| Text | T |
| Shape | U |
| Measure tool | Shift+R |
| Temporary Measure (hold) | R |
| Eyedropper | I |
| Decrease brush / tool size | [ |
| Increase brush / tool size | ] |
| Switch active color | X |

### View & canvas

| Command | Default |
| --- | --- |
| Change canvas size | Ctrl+Alt+C |
| Fit canvas | Ctrl+0 |
| 100% zoom | Ctrl+1 |
| Pan canvas (hold) | Space |
| Pixel preview | Shift+P |
| Selected-layer outlines | O |
| Toggle snapping | Unassigned |
| Snap to canvas | Unassigned |
| Snap to layers | Unassigned |
| Horizontal ruler | Unassigned |
| Vertical ruler | Unassigned |

## Tests

Shortcut and widget tests cover binding conflicts, persistence, menu hints,
focused-control ownership, slider stepping, text editing, curve-point deletion,
Tab capture and Escape cancellation.

```sh
ctest --test-dir build/release --output-on-failure -R '^imageeditor_(shortcut|adjustment_ui|filters_ui|widget_polish|workspace_dialog)_tests$'
```

`imageeditor_wayland_shortcuts` exercises native routing with Vulkan validation.
