Vulkana - Windows portable test build

Open bin\imageeditor.exe. Keep bin and share together; move or copy this whole
folder to relocate the application. No installer, administrator permission,
Python installation or model download is required. Object Selection works offline.

Requires 64-bit Windows 11 and a working Vulkan-capable graphics driver.
Graphics drivers and Windows system libraries are not included.

Preferences, custom brushes and recovery data stay in your Windows user profile,
not beside the program. Replacing this folder does not reset those settings.
Do not keep your own artwork inside the application folder.

This is an unsigned test build, not a published Windows release. Automatic
Windows updates are not enabled. Windows may show an unknown-publisher warning.

License: LICENSE.txt (Vulkana, MIT).
Third-party notices: share\vulkana-editor\notices and
share\licenses\vulkana-editor. Model/runtime notices accompany the model in
share\vulkana-editor\object-selection\mobilesam-v1.
Qt is dynamically linked under its LGPL option; its DLLs remain replaceable.
The upstream Qt module inventories are included for review. Final public-release
preparation must also complete the matching Qt source/third-party notice bundle.

portable-manifest.json lists the included files, checksums and DLL dependencies.
It is an integrity inventory, not a digital signature.
