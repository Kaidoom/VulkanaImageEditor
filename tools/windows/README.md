# Windows development

Vulkana uses native x64 MSVC, Qt and Vulkan on Windows 11. Build tooling is kept
separate from Linux packaging; no Python runtime, CUDA installation or model
download is needed by Object Selection. Python is a **build-time** tool for the
language catalogs only.

## Prerequisites

Install PowerShell 7, Git, CMake 3.28+, Ninja, Python 3, Visual Studio's x64 C++ workload,
Qt (MSVC x64), the Vulkan SDK and vcpkg. The reviewed dependency versions are in
`dependencies/versions.json` and `dependencies/vcpkg.json`. Setup rejects version
or checksum mismatches rather than silently changing runtime/model inputs.

The current configuration uses Qt 6.10.3, MSVC tools 14.51.36231, Vulkan SDK
1.4.363.0 and vcpkg revision `e456309491fd875487bf02b8a0dca76c1b1b00cc`.
Qt needs Core, Gui, Widgets, Network, Pdf, Svg, Test, Linguist tools, platform/image
plugins, a TLS backend and translations. The custom `x64-vulkana-windows` triplet
builds Release with the dynamic MSVC CRT. minizip-ng is statically linked, as
required by its vcpkg port, with the same dynamic CRT.

## Setup and build

Clone the repository in PowerShell 7:

```powershell
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

Use a new build root when testing a fresh clone. CMake build directories are tied
to their source checkout; do not share one between different clones.

```powershell
pwsh -NoProfile -File tools/windows/Setup.ps1 -BuildRoot C:\Dev\VulkanaClean
pwsh -NoProfile -File tools/windows/Build.ps1 -BuildRoot C:\Dev\VulkanaClean -Tests
C:\Dev\VulkanaClean\stage\bin\imageeditor.exe
```

Both scripts accept `-VcpkgRoot`, `-QtRoot`, `-VulkanRoot`, `-BuildRoot` and `-Jobs`.
Defaults are conventional developer directories under `C:\Dev` and `C:\Qt`;
change the arguments to match your installation. Do not dot-source the scripts.
They enter the x64 compiler environment without changing the machine-wide PATH.
Pass the same `-BuildRoot` to subsequent smoke and portable commands. Setup,
Build and Portable also need matching dependency-path overrides; OverlaySmoke
accepts `-QtRoot` for a nondefault Qt location. The examples below continue using
`C:\Dev\VulkanaClean`.

Setup does not install Visual Studio, Qt, Vulkan drivers, the SDK or vcpkg itself.
Use a clean vcpkg checkout at the revision in `dependencies/versions.json` and
run its `bootstrap-vcpkg.bat` before setup. A working Vulkan graphics driver is
required to launch the app and run GPU checks, but not to compile C++ or shaders.
Initial dependency setup needs internet access; no network is needed for editing
or Object Selection once the runtime has been staged.

Setup installs missing manifest dependencies and verifies the official ONNX
Runtime 1.30.0 Windows x64 CPU archive and DLL. Licenses are retained unmodified.
It then builds/runs a standalone dependency/core smoke suite. `-SkipSmoke`
prepares dependencies without tests. The unchanged encoder and decoder already
in the repository are verified before use. No model conversion is performed.

Build configures Release and stages a relocatable application under
`<BuildRoot>/stage`. Run `stage/bin/imageeditor.exe`. `-Tests` additionally builds
and runs the Windows regression targets. No ZIP, installer or published release
is created by this command. Startup/download services require the separate public
service configuration; Windows update delivery is not yet enabled and cannot
fall back to a Linux package.

Build outputs, dependency archives and runtime SDKs stay outside the source tree.
vcpkg uses its normal downloads/build trees and binary cache. Settings, custom
brushes and recovery data use per-user Qt storage rather than the executable
directory, so replacing the application folder does not replace user settings.

## Verification and scope

For a clean, unsigned portable test folder after configuring with `Build.ps1`:

```powershell
pwsh -NoProfile -File tools/windows/Portable.ps1 -BuildRoot C:\Dev\VulkanaClean -Destination "$env:USERPROFILE/Desktop/Vulkana Portable"
pwsh -NoProfile -File tools/windows/PortableSmoke.ps1 -BuildRoot C:\Dev\VulkanaClean -PortableDirectory "$env:USERPROFILE/Desktop/Vulkana Portable"
```

The destination must not exist. The script updates the executable and language
packs, installs into a fresh directory, deploys Qt, resolves native DLL imports,
includes dependency notices/inventories and publishes only after checks pass.
It excludes test executables, symbols, SDKs, unused software OpenGL/D3D compilers
and source trees. Run `bin/imageeditor.exe` and move the entire folder together.
This is a review build, not a signed or license-audited public release.
The portable smoke requires the probes built by `Setup.ps1`. It verifies the
inventory, a clean-PATH relocated native launch, image/PDF/SVG/TLS support and
both offline model stages; probes are added only to a disposable test copy.

The prerequisite suite compiles the actual shared core and existing selection
tests; it exercises Qt image/PDF/SVG/TLS dependencies, Vulkan device creation and
the composite shader, and the complete offline MobileSAM encoder/decoder.
Language packs use the same validation/compiler as Linux. CTest logs are under
`<BuildRoot>/dependency-smoke/Testing/Temporary` and `<BuildRoot>/release/Testing/Temporary`.

Headless checks cannot certify native window presentation, tablet input, IME,
HiDPI or dialogs. Run the application on an interactive Windows desktop with a
working Vulkan driver, including `--smoke-test`, before release. The reproducible
native lifecycle and clean-PATH relocation check is:

```powershell
pwsh -NoProfile -File tools/windows/Smoke.ps1 -BuildRoot C:\Dev\VulkanaClean -Relocate
pwsh -NoProfile -File tools/windows/OverlaySmoke.ps1 -BuildRoot C:\Dev\VulkanaClean
```

It requires the SDK's Vulkan validation layer, exercises canvas/panel input,
window transitions, painting and undo, and fails on any validation warning/error.
Logs remain under `<BuildRoot>/validation`; a successful temporary relocated copy
is removed. Failed copies remain available for inspection. Test relocation
without developer directories on PATH, Unicode filenames, project overwrite and
cancel, concurrent launches, masks, text, PSD/PDF export and Object Selection.

`OverlaySmoke.ps1` requires `Build.ps1 -Tests`. It captures actual desktop pixels
over the Vulkan canvas, verifies transparent dock gaps and modal dimming, and
checks restoration after repeated dismissal. Captures and logs remain under
`<BuildRoot>/validation/overlay`. This cannot be replaced by QWidget screenshots
or a session-0 test, which do not inspect native alpha composition.
The application and Windows test executables embed `res/windows/Vulkana.manifest`:
its Windows 10/11 compatibility declaration is required for layered child-window
transparency. Keep it when adding or replacing executable build targets.

Windows instance coordination uses a user-restricted named mutex and named pipe,
scoped to the logon session and independent of the executable's location. Project
saving stages beside the destination, flushes and validates the archive, then
performs a same-volume replacement without a direct-write fallback. Existing
Linux coordination and file-saving semantics remain in their platform paths.

Developer staging is not a license-audited distribution. A release must inventory
the final DLL/plugin closure, retain upstream notices and Qt redistribution
requirements, and include the appropriate MSVC redistribution material. Do not
copy runtime SDK symbol files or private source/reference material into a release.

## Manual UI review

- Open several images, paint/erase, pan/zoom and undo; try a pen tablet if available.
- Float, resize, tab and redock panels; verify that overlays neither hide the
  canvas nor intercept clicks outside their visible controls.
- Resize, maximize, minimize/restore and move between displays at different DPI.
- Create text, switch languages, and test Arabic/CJK typing and native file dialogs.
- Save/reopen a project with a Unicode filename, overwrite it, and cancel a save.
- Export PNG/WebP, layered PSD and PDF; reopen the results. Exercise masks,
  adjustments and rectangle-based Object Selection without a network connection.
- Close and relaunch from a different application folder: user preferences and
  custom brushes should remain available without writing into the application.
