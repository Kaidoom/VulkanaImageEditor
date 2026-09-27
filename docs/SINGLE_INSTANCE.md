# Single workspace per user

## Ownership and second launches

RPM, AppImage (mounted or extracted), and development executables all use the
same `vulkana-editor` directory inside Qt's per-user RuntimeLocation. The
AppImage launcher preserves that location and forwards arguments unchanged.
No executable basename, PID-liveness heuristic, package version, mount path,
project path, display backend, or preferences file determines ownership.

A Linux kernel `flock` elects the primary **before Vulkan/workspace creation**.
The private directory is owner-only (0700); the lock is an owner-only regular
file (0600), opened without following symlinks and close-on-exec. It is never
unlinked: contenders must continue to lock the same inode. Process exit/crash
releases ownership automatically. Only the lock owner removes a stale local
socket and starts the user-only Qt local server. A live but unresponsive owner
does not permit a second workspace.

Second launches forward an activation request and optional ordered file list.
Paths are made absolute in the launching process's working directory; spaces,
Unicode, and multiple files survive JSON IPC. The protocol has version/type/
path validation, 64 files / 64 KiB per request, a bounded startup queue and
connection deadlines. Normal forwarding waits at most five seconds and reports
an error on failure. Ambiguous delivery is not retried (which could
open duplicate image tabs). No shell commands or document bytes travel over IPC.

The existing workspace is shown/restored and activation is requested; the
Wayland compositor retains final control over focus-stealing prevention.
Files use the ordinary Open/new-tab path: existing canonical projects focus
their tab, images may open repeatedly. Requests defer around modal dialogs,
popups and active gestures. Startup New Canvas yields to incoming files;
explicit New Canvas and other user dialogs do not get dismissed. No-file
activation changes no document, selection, history or saved checkpoint.

The server stops accepting on application quit, but keeps the lock until after
window and Vulkan teardown. Update Restart Now uses the internal
`--wait-for-instance-exit` option: the new executable waits up to 15 seconds for
the same lock instead of forwarding back into the closing process. It does
not bypass ownership, and the existing Save/Discard/Cancel guards still run.
Failed child launch leaves documents/windows available as before.

Help/version exit before coordination. Explicit `--smoke-test` integration
probes are exempt so a test cannot forward its scripted actions into an editor
with real unsaved documents.

## Tests

Multiprocess tests cover concurrent launches, differently named executables,
path forwarding, malformed requests, timeouts, crash recovery, restart handoff
and runtime-directory permissions. Project lifecycle tests cover modal deferral,
startup-chooser dismissal and preservation of unsaved tabs.

```sh
ctest --test-dir build/release --output-on-failure -R 'imageeditor_(single_instance|platform_startup|project_lifecycle)_tests$'
```
