Vulkana - Windows portable beta

Extract the entire ZIP to a folder, then open bin\imageeditor.exe.
Keep bin and share together; move or copy this whole
folder to relocate the application. No installer, administrator permission,
Python installation or model download is required. Object Selection and
Remove Background work offline.

Requires 64-bit Windows 11 and a working Vulkan-capable graphics driver.
Graphics drivers and Windows system libraries are not included.

Preferences, custom brushes and recovery data stay in your Windows user profile,
not beside the program. Replacing this folder does not reset those settings.
Do not keep your own artwork inside the application folder.

On first launch, Vulkana offers to register .vulkana project files for this
Windows account: a project icon, Open with entry and saved-canvas thumbnails
in File Explorer. No administrator access is needed. Declining is remembered.
Change this later in Preferences > General > Register Vulkana project files.
After opting in, launch a newer portable version once to update the registered
paths. Older or same-version copies do not take over automatically. Keep the registered folder
in place; after moving the same version, register again from the new location.
Remove registration before deleting the last copy.
To show the prompt again: bin\imageeditor.exe --force-project-integration-prompt
Explorer must allow thumbnails; older projects need re-saving for a preview.

This build is not digitally signed. Windows SmartScreen or Smart App Control
may warn about an unrecognized app/unknown publisher or block it. Only run
copies from a source you trust. Organization security policies may prevent
running unsigned apps; do not disable Windows security to use this build.
Official downloads: https://vulkana.app/downloads/

Vulkana can check for updates and download a ZIP when you choose to do so.
It verifies the published SHA-256, but does not extract, install or run the
download, or restart the application. Close Vulkana and extract a later release
into a new folder yourself. Keep your previous folder until you have tested it.
A checksum detects a changed download; it is not a publisher signature.

This is beta software. Keep backups of important projects.

License: LICENSE.txt (Vulkana, MIT).
Third-party notices: share\vulkana-editor\notices and
share\licenses\vulkana-editor. Model/runtime notices accompany the models in
share\vulkana-editor\object-selection\mobilesam-v1 and
share\vulkana-editor\background-removal\v1.
Qt is dynamically linked under its LGPL option; its DLLs remain replaceable.
The upstream Qt module inventories and original notices are included. Matching
Qt sources and library-replacement instructions are indexed in
share\vulkana-editor\notices\SOURCES.json and LIBRARY-REPLACEMENT.txt, and at
https://vulkana.app/licensing/

portable-manifest.json lists the included files, checksums and DLL dependencies.
It is an integrity inventory, not a digital signature.
