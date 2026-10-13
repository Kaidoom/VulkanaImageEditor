# Project icons and saved thumbnails

Save a `.vulkana` project to embed a small overview of its visible canvas. Existing
files without an embedded preview use the Vulkana document icon until re-saved.
Previews remain embedded when files are renamed or moved between machines.

## Linux desktop integration

The normal Linux install manifest includes:

- `bin/vulkana-thumbnailer`, a small headless preview extractor;
- `share/thumbnailers/vulkana.thumbnailer`, registered for `application/x-vulkana`;
- a one-time KDE settings migration in `share/kconf_update/`;
- the MIME definition and `vulkana-project` fallback icon in the hicolor theme.

The RPM installs these files automatically. MIME and icon caches use the
distribution's normal transaction triggers. Vulkana previews are enabled by
default in Dolphin. For existing profiles with a saved nonempty preview-provider
list, a one-time KDE migration appends Vulkana without changing any other entries.
Profiles without a saved list already use KDE's enabled-by-default behavior. An
explicitly empty list (all providers disabled) remains empty.

KDE runs the migration in each user's session when it notices the installed update
file, or at the next KDE login. Reopen Dolphin after upgrading if an existing
window still uses its old settings. The migration's stable ID ensures that later
unchecking **Vulkana Image Editor project** is respected across upgrades and
reinstallation. No package script scans home directories or edits settings as
root; no KDE runtime dependency is added to Vulkana.

The default application, icon theme, global **Show Previews** setting and file-size
limits remain unchanged. Dolphin must have **Show Previews** enabled, and its local
file-size limit must allow the project to be previewed.

The explicit icon name `vulkana-project` avoids KDE's dashed-name fallback from
`application-x-vulkana` to the current theme's `application-x-generic` before it
searches hicolor. The conventional `application-x-vulkana` icon is still installed
for other consumers. No theme-specific overrides are needed.

The standard thumbnailer registration is independent of KDE libraries. Browsing
a project folder does not start Vulkana or require a GPU/display connection.
Only the saved preview is decoded; the full document is not rendered on demand.
AppImage desktop integration is not installed automatically. Its temporary mount
cannot be registered as a permanent thumbnail-helper location. Use the native
Linux package for the integration above. The embedded preview format is shared
with Windows.

## Windows portable integration

The first normal launch offers **Register Vulkana project files?**. Registering
adds a project icon, an **Open with** entry and saved-canvas thumbnails in File
Explorer, for the current Windows account only. No administrator access or
external installer is needed. Existing default-app choices and Explorer's own
thumbnail visibility settings stay unchanged. Choosing **No thanks** (or closing
the prompt) is remembered; portable updates do not reset this choice.

Use **Preferences → General → Register Vulkana project files…** to register again
or remove the registration. After opting in, launch a newer portable version once to refresh
the icon, Open with and thumbnail-provider paths to its folder. Versions are
compared numerically; older or same-version copies do not take over automatically.
Keep the newly registered folder in place. If moving the same version or choosing
an older build deliberately, register it through Preferences from its new location.
Downloading or extracting a build alone does not update registration. Choosing
**No thanks** or removing registration disables automatic refresh.

Only paths still matching Vulkana's saved registration are refreshed; removed
or externally changed values are left untouched. No shortcut or shared runtime
copy is installed. Removing registration
restores prior registry values only where Vulkana's values are still present;
later changes by another application are preserved. Remove registration before
deleting the last portable copy.

With Vulkana closed, these commands are also available from the portable folder:

```powershell
# Show the prompt again; this does not register anything without confirmation.
.\bin\imageeditor.exe --force-project-integration-prompt

# Explicit noninteractive registration/removal for the current account.
.\bin\imageeditor.exe --register-project-files
.\bin\imageeditor.exe --unregister-project-files
```

The native `IThumbnailProvider` uses `IInitializeWithStream`, so Windows' normal
out-of-process thumbnail isolation remains enabled. The shell-facing DLL has
only OS dependencies; it loads the matching private codec DLL by absolute sibling
path with a restricted dependency search. That codec uses the same bounded
ZIP/manifest/PNG validation as Linux. It does not launch Vulkana, load a document,
create a Qt application/window, initialize Vulkan or extract source pixels to
temporary files. Returned bitmaps are top-down premultiplied BGRA with alpha.
Missing, stale or malformed previews fall back to the project icon.

Unsigned-build policies can still prevent a Shell extension from loading. Do not
disable Windows security to work around an organization policy. A visual **Preview
pane** handler is not included; these are file-list thumbnails.

## Testing

```sh
cmake --build build/release --target imageeditor_project_file_tests vulkana-thumbnailer
ctest --test-dir build/release --output-on-failure -R 'project_(file_tests|thumbnail_integration)'
build/release/src/project/vulkana-thumbnailer --size 256 example.vulkana preview.png
```

The extractor exits 0 after writing a PNG, 1 when no usable preview is available
or extraction fails, and 2 for invalid arguments. It refuses to overwrite the
input project. A desktop's preallocated empty temporary output is filled in place,
preserving its inode and security label; the desktop publishes the final cached
thumbnail. Symlink, multiply linked empty and nonregular outputs are refused.
Other output paths use atomic replacement. Preview validation and its
freshness digest are documented in [the project format](VULKANA_FORMAT.md).

The integration test uses disposable XDG data/config/cache directories to check
Unicode paths, headless extraction, legacy-file fallback, MIME metadata and KDE
icon lookup when `kiconfinder6` is available. It never edits desktop preferences.
It also checks the preallocated-output contract without requiring SELinux. RPM
desktop testing must use the installed helper with its `thumb_exec_t` label: an
extracted executable does not exercise Fedora's `thumb_t` restrictions.
The KDE migration test uses isolated profiles to check first installation,
existing provider lists, inherited settings and preservation of later opt-outs.

Windows CTest includes `imageeditor_windows_project_integration_tests` (a
disposable registry root covering numeric upgrades, older/equal versions, legacy
registration, missing components, opt-outs, external changes and removal after
upgrades) and `imageeditor_windows_thumbnail_provider` (native
COM loading, Unicode filenames, legacy/corrupt/stale inputs and source
preservation). The decoder is tested from an executable that does not import Qt.
For a real Shell-cache check, stage the two Windows test executables beside the
portable app and run this from a normal, non-elevated desktop session:

```powershell
.\bin\imageeditor_windows_project_integration_tests.exe --shell-cache C:\Artwork\example.vulkana
```

This explicit probe temporarily registers that folder and restores prior
associations afterward; it does not change the app's remembered first-run choice.
The test executables are not included in portable releases.
