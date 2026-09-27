# Optional online services

The editor builds and runs without online-service credentials. Without a valid
configuration and nonempty client token, Check for Updates and Submit Feedback
are disabled, and automatic startup checks do not run (even with
`--force-update-check`). Editing, saving and exporting remain available.

For a local developer build, copy `config/services.example.json` to
`private/services.json`, then supply these fields:

- `version`: `1` (configuration schema, not the application/project version).
- `domain`: the HTTPS origin, such as `https://updates.example`; no path, query,
  fragment, credentials or nonstandard port.
- `apiKey`: the service's client token. Empty or invalid tokens disable both
  online actions. Tokens are never logged or stored in project/preferences data.

`private/` is ignored by Git. The optional `appimagekey` field in the same JSON
holds the GPG signing-key **public fingerprint** (40 or 64 hexadecimal characters).
It is used only by the packaging tools, which default to this file and use the
user's existing keyring. An explicit `--sign-key-file` can still name a plain
fingerprint file. Never put private key material in this repository.

## Runtime lookup and distribution

1. `VULKANA_SERVICE_CONFIG=/absolute/path/services.json`, when set, is the sole
   source. A missing/invalid file (or an empty/relative override) disables the
   services; it never falls back to another configuration.
2. Developer builds read `private/services.json` directly, without falling back
   to a separately installed application. Change the file and restart; no
   recompilation is needed.
3. Non-development builds use the existing installed/relocatable resource
   lookup for `share/vulkana-editor/services.json`. The AppImage stays within
   its own bundle. The working directory is never searched.

The normal CMake installation optionally includes **only** `private/services.json`
as `share/vulkana-editor/services.json`. Its optional public signing fingerprint
is harmless distribution metadata, ignored by the application. No separate
signing files or other private files are installed. Build a fresh package/stage
after changing this file. An open-source
checkout without `private/` installs no configuration and remains offline by
default. Each service holds an immutable configuration snapshot so a file change
cannot redirect an in-flight report or change a pending download's trusted origin.

The paths relative to the configured origin are `/downloads/latest.json`,
`/#download`, and `/api/feedback-add`; About's website link uses the origin.
Update artifacts must still use HTTPS on that same origin under `/downloads/`,
pass filename and SHA-256 validation, and comply with the existing redirect rules.
The token is sent only as `X-Vulkana-Token` on feedback POSTs, never on update GETs.
Missing tokens nevertheless disable updates as a distribution-policy choice.

This is an extractable client identifier, **not a secret or user authentication**.
Moving it to JSON separates distribution configuration from public source; it
does not replace server-side authorization, validation or rate limiting.
Third-party notices/source attribution URLs are separate publication metadata and
are intentionally not rewritten by this service configuration.

## Focused checks

```sh
cmake --preset release
cmake --build --preset release --target imageeditor imageeditor_update_tests imageeditor_feedback_tests -j6
ctest --test-dir build/release --output-on-failure -R '^imageeditor_(update|feedback)_tests$'
```

Normal tests inject a temporary JSON file with an `.example` origin and synthetic
token plus an offline network transport. They neither need the private file nor
send requests to the live service. Missing/malformed/oversized configuration,
invalid URLs/tokens, disabled controls, startup continuation, trusted-origin
validation and request lifetime are covered.
