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

The first launch offers per-user project icons and Explorer thumbnails. Manage
this in **Preferences → General → Register Vulkana project files…**, or launch with
`--force-project-integration-prompt` to ask again. After opting in, launching a
newer version updates the registered paths automatically. Re-register through
Preferences when moving the same version or deliberately choosing an older one.
See [project thumbnail integration](../../docs/PROJECT_THUMBNAILS.md)
for removal, command-line setup and troubleshooting.

To assemble from another configured Release directory, pass `-BuildDirectory`
to `Portable.ps1`; `-BuildRoot` still locates the prepared dependencies.

To validate the portable folder, run the dependency checks in the Testing
section first, then:

```powershell
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/PortableSmoke.ps1 -PortableDirectory C:\Dev\VulkanaPortable
```

This checks the file inventory, relocated launch, codecs and offline model using
a temporary copy. Inspect `portable-manifest.json` for file hashes and DLL
dependencies; it is an integrity inventory rather than a digital signature.

## Release ZIP

Portable builds are unsigned. The included README explains Windows warnings,
manual extraction, user-data storage and the download-only update workflow.
Vulkana never extracts, installs or launches a downloaded Windows update.

Prepare distribution notices before archiving. Download the five exact Qt source
archives listed in [qt-sources.json](dependencies/qt-sources.json) from its
`baseUrl` into `C:\Dev\QtSources`. Qt PDF's source is in the larger QtWebEngine
archive; the browser runtime is not shipped. Keep these archives available for
source access. Generate notices from them and the installed dependency metadata:

```powershell
python tools/windows/PrepareNotices.py --sources C:\Dev\QtSources --base C:\Dev\Vulkana\release\notices --qt C:\Qt\6.10.3\msvc2022_64 --vcpkg C:\Dev\Vulkana\installed\x64-vulkana-windows --output C:\Dev\WindowsNotices
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/Portable.ps1 -Destination C:\Dev\Vulkana-0.3.2-win64 -NoticesDirectory C:\Dev\WindowsNotices
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/PortableSmoke.ps1 -PortableDirectory C:\Dev\Vulkana-0.3.2-win64
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File tools/windows/ZipPortable.ps1 -PortableDirectory C:\Dev\Vulkana-0.3.2-win64 -Destination C:\Dev\Vulkana-0.3.2-win64.zip
```

Use new output paths for each release. Notice preparation verifies source hashes;
ZIP creation checks every file against the portable inventory, then verifies the
compressed members. Neither script uploads files. Review the DLL/plugin inventory,
notices and source-access links before distribution; retain Microsoft's REDIST
material and the model/runtime notices.

Official builds with configured service credentials read the `windowsportable`
entry in the update feed. Set its HTTPS `url` and `sha256` to the final ZIP's values:

```json
"windowsportable": {
  "sha256": "<SHA-256 printed by ZipPortable.ps1>",
  "url": "https://vulkana.app/downloads/Vulkana-0.3.2-win64.zip"
}
```

Publish the artifact before changing the feed. Keep the existing Linux entries.
Builds without service credentials do not contact the update service.

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
