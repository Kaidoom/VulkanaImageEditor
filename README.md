# Vulkana Image Editor

<!-- readme-languages:start -->
[English](README.md) · [Français](docs/readme/README.fr.md) · [Español](docs/readme/README.es.md) · [Deutsch](docs/readme/README.de.md) · [Italiano](docs/readme/README.it.md) · [Português (Brasil)](docs/readme/README.pt-BR.md) · [Polski](docs/readme/README.pl.md) · [Türkçe](docs/readme/README.tr.md) · [Русский](docs/readme/README.ru.md) · [Українська](docs/readme/README.uk.md) · [العربية](docs/readme/README.ar.md) · [हिन्दी](docs/readme/README.hi.md) · [简体中文](docs/readme/README.zh-Hans.md) · [繁體中文](docs/readme/README.zh-Hant.md) · [日本語](docs/readme/README.ja.md) · [한국어](docs/readme/README.ko.md)
<!-- readme-languages:end -->

A Linux-first image editor for painting, retouching and layered composition,
with native Windows builds, a Qt workspace and Vulkan-accelerated canvas.

![Vulkana workspace with a layered image, brush presets and adjustments](res/Preview.png)

## Features

- **Layered editing:** raster, editable text and shapes; bitmap masks, folders,
  clipping mask groups, multi-selection, blend modes, duplication, merging and
  explicit rasterization.
- **Painting and retouching:** pressure-sensitive brushes, procedural and textured
  presets, erasing, fill, local blur, Clone Stamp, Heal and automatic Spot Heal.
- **Selections:** rectangle, ellipse, freehand/polygon/magnetic lasso, select by
  color and brush-guided Quick Selection, with grow/shrink, inversion and reselect.
- **Object Selection and refinement:** select an object with a rough rectangle
  using the bundled offline model; improve selections and bitmap masks with
  Refine Selection, live previews and manual corrections. No model download or
  Python installation is needed to use Object Selection.
- **Transforms:** move, scale, rotate, flip and corner distortion; transform
  selection boundaries or selected pixels, with snapping and alignment guides.
- **Non-destructive adjustments:** per-layer corrections and masked adjustment
  layers with All Below / This Group scope; exposure, levels, curves, hue/saturation,
  vibrance, color balance, warmth/tint, monochrome conversion and inversion.
- **Filters and layer effects:** Gaussian, Motion and Lens Blur; Stroke, Drop/Inner
  Shadow, Outer/Inner Glow, Color Overlay, Gradient Overlay and Bevel & Emboss.
- **Multiple documents:** tabs with independent history, selections and view state,
  plus editable layer copying between documents.
- **PSD import:** reviewed conversion to native layers, rich text, simple shapes,
  bitmap masks, clipping groups and supported styles, with explicit saved-raster fallbacks.
- **PSD export:** layered or flattened output, editable text/shapes/masks and
  clipping where supported, with reviewed conversions and a fresh composite.
- **PDF import:** selected pages at your chosen PPI, as raster layers or separate
  documents, with transparent or white backgrounds.
- **PDF export:** a composite or layers/groups as pages, custom page selection,
  canvas/fitted sizes and selectable text where supported.
- **Workspace and output:** tabbed dockable panels, a Navigator with pan/zoom
  controls, themes, editable shortcuts, rulers, Pixel Preview, native `.vulkana`
  projects and PNG/JPEG/WebP export.
- **Languages:** 16 interface languages, selectable in Preferences → General →
  Language without restarting. All language versions are first drafts; reviews
  and corrections are welcome.

Actively developed beta. Please keep backups of important projects.

## Build and run

Linux and Windows builds target x86-64. Clone the repository, then run the
commands for your platform from its root:

```sh
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

### Linux

Requires Linux, a C++20 compiler, Ninja, pkg-config, and these development libraries:

- CMake **3.28+** for the supplied presets.
- Qt **6.8+**: Core, Gui, Widgets, Network, DBus and PDF; Qt Test for tests.
- Qt Linguist `lrelease` and Python 3 for building language packs (build tools only).
- qpdf **11+** development library (PDF import/export; not the command-line tool).
- zlib-compatible development library (PSD ZIP channels).
- Vulkan **1.2+** headers/loader, a working Vulkan driver, and `glslc`.
- minizip-ng **4.x** (the `minizip` pkg-config module, not legacy minizip).
- libwebp and libwebpmux **1.2+**.

Install Qt's platform plugins for your desktop, SVG support and image-format
plugins. Point `CMAKE_PREFIX_PATH` at your Qt installation if it is outside the
system search path.
Qt PDF is a separate development package on many distributions (for example,
`qt6-qtpdf-devel` on Fedora, `qt6-pdf-dev` on Ubuntu). It does not require a
WebEngine browser in Vulkana's runtime.
Qt Linguist tools are provided by `qt6-linguist` on Fedora or
`qt6-tools-dev-tools` on Ubuntu. Set `-DVULKANA_LRELEASE=/path/to/lrelease` if
needed. Qt's standard-dialog catalogs come from `qt6-qttranslations` on Fedora
or `qt6-translations-l10n` on Ubuntu. See [localization](docs/LOCALIZATION.md)
for pack-only builds and review.

From the repository root:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=OFF
cmake --build --preset release --parallel
./build/release/src/app/imageeditor
```

Projects and images can also be passed as arguments:

```sh
./build/release/src/app/imageeditor "project.vulkana" "photo.webp"
```

Qt selects Wayland or X11 from the session. Preferences and custom presets live
in per-user storage. Online services are optional; without
[service configuration](config/SERVICES.md), updates and feedback stay disabled.

For Release tests, enable the test targets and rebuild:

```sh
cmake --preset release -DIMAGEEDITOR_BUILD_TESTS=ON
cmake --build --preset release --parallel 6
ctest --preset release
```

The debug preset is also available. Native rendering checks need Vulkan
validation layers. Some interaction tests require a running Wayland/X11 desktop.

### Windows

Install PowerShell 7, Git, CMake, Ninja, Python 3, Visual Studio's x64 C++ tools,
Qt for MSVC x64, the Vulkan SDK and vcpkg. Use the pinned versions and component
list in the [Windows build guide](tools/windows/README.md).

From PowerShell 7 in the repository root:

```powershell
pwsh -NoProfile -File tools/windows/Setup.ps1 -BuildRoot C:\Dev\VulkanaClean
pwsh -NoProfile -File tools/windows/Build.ps1 -BuildRoot C:\Dev\VulkanaClean -Tests
C:\Dev\VulkanaClean\stage\bin\imageeditor.exe
```

Choose a new build directory for a fresh-clone test; do not reuse a directory
configured for another checkout. Setup verifies dependencies and runs prerequisite
checks; Build compiles language packs, builds/tests the app and stages its runtime
files. Initial setup needs internet access for build dependencies, but the app
and bundled Object Selection run locally without Python or model downloads.

See the Windows guide for custom dependency paths, native smoke tests and
creating a clean portable folder. Preferences remain in per-user storage,
separate from the application folder.

## Architecture

- `src/core` — platform-neutral documents, layers, geometry, image algorithms
  and undoable transactions.
- `src/render` — shared Vulkan canvas, composition, overlays and bounded GPU caches.
- `src/ui` — Qt panels, tools, document tabs, text/shape preparation and file I/O.
- `src/platform` — resource lookup, desktop integration and optional services.
- `src/app` — application entry point; `shaders` — Vulkan shader sources.

Each document owns its content and history; tabs share one workspace and Vulkan
renderer. Editable source data stays separate from render caches. Canvas,
Pixel Preview and export share the same composition rules.

See the [developer docs](docs/README.md) and [architecture guide](docs/ARCHITECTURE.md).

## License

Vulkana-owned code, documentation and assets are [MIT licensed](LICENSE).
Third-party material retains its own licenses; see [third-party notices](THIRD_PARTY.md).
