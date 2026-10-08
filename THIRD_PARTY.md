# Third-party notices

Vulkana-owned code, documentation and assets use the [MIT License](LICENSE).
Keep the copyright and license notice with copies or substantial portions.
This source license is separate from the terms accompanying older official
application packages.

Third-party material, including license copies and notices, retains its original terms.

## Included test fonts

The repository includes two unmodified fonts for repeatable text tests:

| Files | Attribution | License |
| --- | --- | --- |
| `tests/assets/fonts/NotoSans-Regular.ttf`, `NotoSansArabic-Regular.ttf` | Copyright 2018 The Noto Project Authors; additional embedded font credits are retained | SIL Open Font License 1.1 |

The full [OFL notice](tests/assets/fonts/OFL.txt) accompanies the fonts.
Keep it and the embedded credits with redistributed copies. Font modifications
remain under OFL; its reserved-name and naming conditions apply.

[Provenance and checksums](tests/assets/fonts/README.md) identify the exact
upstream revision and files. These fixtures are used by tests, not installed as
application fonts.

## External dependencies

Object Selection includes the unmodified ONNX Runtime
1.20.1 C API header at `src/ui/third_party/onnxruntime/onnxruntime_c_api.h`
(SHA-256 `573a117ae6b83ead7f53da6cedaa0d7b7d9cdb45c0ba4726bac9081e67696a82`).
Copyright Microsoft Corporation, [MIT license](src/ui/third_party/onnxruntime/LICENSE).
The default Linux x86-64 build bundles the tested ONNX Runtime CPU 1.30.0 library
and MobileSAM vit_t ONNX encoder/decoder in `assets/object-selection/mobilesam-v1`.
MobileSAM uses Apache-2.0; the ONNX Runtime library retains its MIT license and
complete upstream third-party notices. The bundle includes all license copies
and attribution/modification notes. It needs no Python installation, model
download or network connection. See [exact input provenance](assets/object-selection/README.md)
and [Object Selection behavior](docs/SMART_SELECTION.md).

The source build finds these libraries on the developer's system. Their library
source and binaries are not vendored in the application source tree.

| Component | Use | Upstream terms |
| --- | --- | --- |
| Qt 6.8+ Core, Gui, Widgets, Network, DBus; Qt Test for tests | Workspace, text, networking, desktop color picking and tests | LGPL-3.0 option for these modules; [Qt licensing](https://doc.qt.io/qt-6/licensing.html). Qt tools and embedded third-party code have their own notices. |
| Qt PDF 6.8+ / PDFium | One-time PDF page rasterization | Qt PDF's LGPL-3.0 option; PDFium uses BSD-3-Clause with separately licensed embedded components. [Qt PDF notices](https://doc.qt.io/qt-6/qtpdf-licensing.html) and the installed library's notices identify its actual build inputs. No WebEngine browser is linked by Vulkana. |
| qpdf 11+ | PDF import geometry; export page assembly and shaped-text Unicode mappings | [Apache-2.0](https://github.com/qpdf/qpdf/blob/v12.3.2/LICENSE.txt), with embedded-code notices in [NOTICE](res/notices/licenses/qpdf-NOTICE.txt). Library objects stay inside PDF services; the command-line program is not used. |
| Vulkan headers and loader, API 1.2+ | GPU rendering | [Khronos headers](https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.4.341/LICENSE.md): Apache-2.0/MIT per file; [loader](https://github.com/KhronosGroup/Vulkan-Loader/blob/v1.4.341/LICENSE.txt): Apache-2.0 with permissive file exceptions. |
| minizip-ng 4.x | Project-file ZIP reading/writing | [Zlib license](https://github.com/zlib-ng/minizip-ng/blob/4.1.0/LICENSE); Gilles Vollant, Mathias Svensson and Nathan Moinvaziri. |
| zlib-compatible runtime | PSD ZIP channel decoding | [Zlib license](https://zlib.net/zlib_license.html); Jean-loup Gailly, Mark Adler and implementation contributors. Uses the selected system implementation (zlib or zlib-ng compatibility API), not vendored source. |
| libwebp / libwebpmux 1.2+ | WebP export | [BSD-3-Clause](https://github.com/webmproject/libwebp/blob/v1.6.0/COPYING), Google Inc. and contributors; [patent grant](https://github.com/webmproject/libwebp/blob/v1.6.0/PATENTS). |

Qt platform/image plugins, font libraries, system C/C++ libraries and Vulkan
drivers come from the selected development/runtime environment. Qt SVG and image
format plugins retain their module and embedded-code licenses.
CMake, Ninja, pkg-config, the compiler and shader compiler are build tools, not
vendored dependencies. GCC's runtime uses its
[Runtime Library Exception](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html);
this does not change Vulkana's MIT license.

Keep the upstream terms for the versions you install. Distributing compiled
libraries or a linked application adds obligations under the licenses of those
build inputs; MIT covers Vulkana-owned code, not the dependency code.

## Algorithms and research

The healing, selection, filtering and blending implementations are project code.
Their mathematical references are documented in the [developer guides](docs/README.md).
Optional comparison scripts obtain reference programs and photographs separately
under ignored `research/` or `private/`; those inputs are not part of this source distribution.
OpenCV, PyMaxflow and Python/PyTorch were used only for selection comparisons.
Their implementation code is not incorporated in the native graph-cut solver.
License copies for the direct dependencies are in `res/notices/licenses/`.
They belong to their named components, not to Vulkana's MIT grant.
