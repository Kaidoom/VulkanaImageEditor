# Build Vulkana on Windows

Build an x64 Release application for Windows 11, run its tests, or assemble a
portable folder.

## Prerequisites

Install the following tools. The dependency versions are recorded in
[versions.json](dependencies/versions.json) and the
[vcpkg manifest](dependencies/vcpkg.json).

| Tool | Version or components |
| --- | --- |
| PowerShell | Windows PowerShell 5.1 or PowerShell 7 |
| Build tools on PATH | Git, CMake 3.28+, Ninja and Python 3 |
| Visual Studio | x64 C++ workload with MSVC tools 14.51.36231 |
| Qt | 6.10.3 for MSVC x64; Core, Gui, Widgets, Network, Pdf, Svg, Test, Linguist tools, translations, platform/image plugins and a TLS backend |
| Vulkan SDK | 1.4.363.0 |
| vcpkg | Revision `e456309491fd875487bf02b8a0dca76c1b1b00cc` |

Check out the listed vcpkg revision and run its `bootstrap-vcpkg.bat`.
Install a Vulkan-capable graphics driver to run Vulkana and its rendering tests.
Dependency setup needs internet access.

## Build and run

Open Windows PowerShell and clone the repository:

```powershell
git clone https://github.com/Kaidoom/VulkanaImageEditor.git
cd VulkanaImageEditor
```

Build and launch the application:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1
C:\Dev\Vulkana\stage\bin\imageeditor.exe
```

`Build.ps1` prepares dependencies, compiles the app and language packs, and
assembles the runtime files under `C:\Dev\Vulkana\stage`. Re-run the same
command after source changes.

## Configuration

Use these options with `Build.ps1` to match your tool locations and available
build resources:

| Option | Default |
| --- | --- |
| `-BuildRoot` | `C:\Dev\Vulkana` |
| `-VcpkgRoot` | `C:\Dev\vcpkg` |
| `-QtRoot` | `C:\Qt\6.10.3\msvc2022_64` |
| `-VulkanRoot` | `C:\VulkanSDK\1.4.363.0` |
| `-Jobs` | 6 |

For example, choose a build directory and limit compilation to four jobs:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1 -BuildRoot D:\Builds\Vulkana -Jobs 4
D:\Builds\Vulkana\stage\bin\imageeditor.exe
```

Use a separate build directory for each repository checkout. Reuse that directory
when rebuilding the same checkout.

Pass the same `-BuildRoot` to subsequent test and portable commands. Setup,
Build and Portable accept the dependency-path options above; OverlaySmoke also
accepts `-QtRoot`.

To use PowerShell 7, replace `powershell.exe` with `pwsh` in the commands.
Run each script as a separate process so its compiler environment stays scoped
to that invocation.

## Testing

From the repository root, run the dependency checks and application regressions:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Setup.ps1
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Build.ps1 -Tests
```

Setup tests image/PDF/SVG/TLS support, Vulkan and the offline MobileSAM
encoder/decoder. Build runs the application tests through CTest. Find their
logs under `<BuildRoot>/dependency-smoke/Testing/Temporary` and
`<BuildRoot>/release/Testing/Temporary`.

For standalone script checks:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tests/WindowsBuildScriptTests.ps1
```

For native canvas and overlay checks, run these commands from an interactive
desktop after `Build.ps1 -Tests`, with the SDK's Vulkan validation layer installed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Smoke.ps1 -Relocate
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/OverlaySmoke.ps1
```

Inspect logs and captures under `<BuildRoot>/validation`. Smoke checks window
lifecycle, canvas interaction and relocation; OverlaySmoke checks dock
transparency, modal dimming and input handling against actual desktop pixels.

## Portable folder

After building, create a portable folder at a destination that does not exist:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Portable.ps1 -Destination C:\Dev\VulkanaPortable
C:\Dev\VulkanaPortable\bin\imageeditor.exe
```

Keep the entire folder together when copying it to another location. Preferences,
custom brushes and recovery data stay in the Windows user profile. Store artwork
outside the application folder, and replace the application folder when updating.

To validate the portable folder, run the dependency checks in the Testing
section first, then:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/PortableSmoke.ps1 -PortableDirectory C:\Dev\VulkanaPortable
```

This checks the file inventory, relocated launch, codecs and offline model using
a temporary copy. Inspect `portable-manifest.json` for file hashes and DLL
dependencies; it is an integrity inventory rather than a digital signature.

Portable builds are unsigned. Before publishing a release, audit the final
DLL/plugin inventory and complete the matching Qt source/notices bundle and
MSVC redistribution material.

## Troubleshooting

- **A prerequisite is missing or its version differs:** check the versions above
  and the dependency-path options. Install those tools before running Build.
- **CMake reports a different source directory:** choose a separate `-BuildRoot`
  for this checkout.
- **Scripts are blocked:** run scripts from a trusted checkout using the shown
  `RemoteSigned` command. It sets the execution policy for that process;
  organization-managed policies take precedence.
- **Rendering checks fail:** verify the graphics driver and Vulkan validation
  layer, then inspect the logs in `<BuildRoot>/validation`. Run window tests in
  an interactive desktop session.
- **A custom executable has black dock gaps or incorrect modal dimming:** embed
  [Vulkana.manifest](../../res/windows/Vulkana.manifest) in the executable target
  to enable layered child-window transparency.
- **Language changes are missing from the staged app:** rerun Build to compile
  and install the packs. See [localization](../../docs/LOCALIZATION.md) for
  pack-only compilation and preview.

## Manual checks

Before distributing a build:

- Paint, erase, pan, zoom and undo; test pressure with a pen tablet.
- Float, resize, tab and redock panels; check canvas input around overlays.
- Resize, maximize, minimize and move the window between displays at different DPI.
- Test text editing, Arabic/CJK input, language changes and native file dialogs.
- Save, reopen and overwrite Unicode-named projects; check cancellation.
- Export and reopen PNG/WebP, layered PSD and PDF; exercise masks, adjustments
  and offline Object Selection.
- Move the application folder and relaunch; confirm preferences and custom brushes
  remain available.
