# Localization review build

## Sixteen-language draft delivery — 2026-10-11

All 16 agreed choices are enabled: en, fr, es, de, it, pt-BR, pl, tr, ru, uk,
ar, hi, zh-Hans, zh-Hant, ja and ko. Every non-English catalog has **1,653 of
1,653 active entries translated**, with the correct plural forms. The 13 new
catalogs are AI-assisted first drafts with `reviewPending: true`; they have not
received native-speaker approval. The owner's earlier English/French/Arabic
approval remains recorded separately. Retired contributor entries stay in the
French/Arabic TS files as vanished and are not counted as active UI coverage.

Drafts use concise editing terminology, with full safety meaning retained for
save/discard/overwrite and PSD consolidation/omission choices. A review made
Hindi overwrite prompts explicit, distinguished brush Flow from tablet Pressure
in Russian, and kept contextual Overlay blend names separate from inspection
overlays. Turkish literal percentages use `50%`, not `%50`, which Qt can treat
as an argument placeholder. Small existing-language edits shorten French's
attention-only checkbox and make Arabic Stroke neutral about inside/outside
position. Low-level diagnostics and original license texts are not translated.

Arabic About corrections:

- Arabic-first license wording with LTR-isolated MIT/copyright tokens.
- LTR-isolated version/channel arguments; the running-version caption no longer
  ends with a period that can appear before the version in RTL layout.
- Update results retain a translation resolver instead of a cached English
  string. The normal tested workflow finishes a check, changes language, then
  opens a fresh About panel using that cached result, without another request.
- Contact labels keep their bindings. Queued layout measurement waits for Qt's
  rich-text/script metrics; repeated programmatic binding changes are also
  checked, though switching via Preferences with About open is not a normal UI
  workflow. Test captures use version 0.2.1 as a fixture, not a version bump.

Verification on Fedora 44, Qt 6.11.2, NVIDIA RTX 4080:

- Release build succeeded; **11/11 focused tests passed in 10.98 s**:
  localization runtime/catalogs/authoring/installed lookup, update service/About,
  preferences, text controller/IME, PSD import/export and both interaction suites.
- Repeated switching through all 16 languages keeps numeric controls ASCII,
  combobox identities, document tabs/revisions/history and dock layout unchanged.
- Native widget checks passed on **Wayland 125%** and **XCB 150%**. The repeated
  XCB dropdown assertion was diagnosed: row 26 moved by exactly one 28-pixel
  scroll step (`scroll 10 → 11`) at a popup viewport edge. The test now brings
  each target into view and compares content-space geometry, still checking
  exact row size/position and sufficient script height. No rendering tolerance
  or application behavior was changed to mask this legitimate scrolling.
- Native PSD interaction checks passed on Wayland and XCB: **0 Vulkan validation
  messages**, idle frame delta **0**, upload staging capacity 9,437,184 bytes.
- Fresh CMake install staging at `/tmp/vulkana-languages-stage.yzY5x3/usr`
  includes all 16 application QMs, the pseudo-pack, metadata and 14 matching Qt
  QMs. The translation directory is 6.2 MiB on this filesystem. A relocatable
  installed probe checks all 16 languages without development-path lookup.
- The tested Qt translation package has no Hindi catalog. Vulkana-owned Hindi
  labels/buttons work; some Qt-owned dialogs fall back to English. Matching
  unmodified Qt data provenance/notices were updated; package baselines and
  runtime dependencies are unchanged.

Captures in `temp/languages-review/` cover all languages' workspaces/Preferences,
Arabic About and the existing PSD/tab/dropdown fixtures. Representative German,
Hindi, Japanese, Korean and Traditional Chinese layouts plus Arabic About were
visually inspected. This is not exhaustive linguistic review or a claim to have
manually inspected every effect page. Native-speaker review, actual desktop IME
candidate-window checks and deeper narrow-layout review remain appropriate.

Logs: `/tmp/vulkana-languages-tests-ready.log`,
`/tmp/vulkana-languages-validation-final.log`,
`/tmp/vulkana-languages-wayland125-final.log`,
`/tmp/vulkana-languages-xcb150-final.log`,
`/tmp/vulkana-languages-vulkan-wayland.log`,
`/tmp/vulkana-languages-vulkan-xcb.log`,
`/tmp/vulkana-languages-about-final.log`, and
`/tmp/vulkana-languages-install.log`. Initial diagnostic logs were retained.
`./temp/release-test` points to the rebuilt Release executable. No packages,
version bump or additional language families were created in this pass.

## PSD review and planned language catalogs — 2026-10-11

PSD review types/status, source summary, conversion descriptions, actionable
export blockers and import confirmation now have live French/Arabic wording.
Technical reports, codec diagnostics, user names and source profile identities
remain unchanged. Typed presentation codes prevent language-dependent choices;
in particular, a raster conversion eligibility check no longer compares an
English type against a translated label. Export language changes retain the
selected row, group expansion and conversion settings without requesting work.

Filters/Effects headers now look disabled for adjustment layers, have no enabled
hover treatment, and explain eligibility through a translated tooltip. Raster,
text and shape targets retain their existing behavior.

All 16 agreed catalog IDs/native names are prepared. There are **1,653 entries**
in English/French/Arabic. The other 13 languages have unfinished catalogs with
appropriate plural slots, are disabled in metadata, and are neither advertised
nor installed. This follows the owner's decision to translate them in a dedicated
pass. Regional/script matching is tested independently, including `pt_BR`,
`zh-TW`, `zh-HK`, `zh-CN`, `zh-SG` and explicit Chinese script overrides.

Verification on the same Qt 6.11.2/Fedora review environment:

- Release focused suite: **10/10 passed, 11.94 s** — localization runtime,
  catalogs, authoring tools, installed lookup, adjustment layers, preferences/
  theme, PSD import/export and both PSD interaction suites.
- Tests switch existing PSD review/confirmation widgets repeatedly; compare
  unchanged reports, choices, row identity and group expansion; check disabled
  tabs/clicks/tooltips and restore enabled tabs on a raster target.
- Existing Display P3 conversion and PSD clipping blocker tests passed, with
  added assertions for the neutral presentation metadata.
- Wayland widget run at 125% passed. XCB at 150% passed on retry. The first XCB
  attempt had one `visualRect(index) == before` assertion during popup hover;
  its cause is unconfirmed. The unchanged check passed on the isolated retry,
  without changing tolerances. Both logs are retained; this is not a claim of
  fixing a new XCB issue.
- Native PSD interaction runs passed on Wayland and XCB, NVIDIA RTX 4080:
  **0 Vulkan validation messages**; idle frame delta **0** in both.
- Fresh CMake install staging at `/tmp/vulkana-psd-languages.nqeCcK/usr` contains
  only en/fr/ar/qps, metadata and Qt fr/ar packs. The installed probe verifies
  just three advertised languages and English fallback for planned languages.

Captures in `temp/psd-localization-review/` include French/Arabic PSD reviews and
disabled adjustment tabs. The latest visual captures use XCB 150% scaling.
New PSD wording remains available for owner review; no additional full language
translations, packages, version bump, new runtime dependency or notices were
introduced. `./temp/release-test` points to the rebuilt Release executable.

Logs: `/tmp/vulkana-psd-localization-tests-ready.log`,
`/tmp/vulkana-psd-localization-wayland125-final.log`,
`/tmp/vulkana-psd-localization-xcb150.log`,
`/tmp/vulkana-psd-localization-xcb150-retry.log`,
`/tmp/vulkana-psd-localization-vulkan-wayland.log`,
`/tmp/vulkana-psd-localization-vulkan-xcb.log`, and
`/tmp/vulkana-psd-localization-install-final.log`.

## Owner approval — 2026-10-11

The owner approved the first localization pass, including the corrected Arabic
menus/dropdowns and translated wording. French/Arabic language metadata now
records that approval; Preferences no longer shows the pending-review notice.
Future scope is user-facing interface and conversion guidance. Low-level errors
and technical diagnostics intentionally remain English, superseding the broader
diagnostic-localization proposal in the original delivery notes below.

Removed the duplicate Edit-menu New Adjustment Layer entry (Layers and the
other creation controls remain) and added an original scalable Preferences gear.
Release rebuilt; localization runtime/catalog and preferences/theme tests passed
3/3. The menu regression checks one top-menu adjustment entry and a Preferences
icon. Approval-review captures are in `temp/localization-approval-review/`.

## Manual-review fixes — 2026-10-11

- Fixed one-time string snapshots in Fill, Layer/Mask, visibility, grouping,
  duplication, merge/rasterize, New Adjustment Layer and document-navigation
  actions. The top and context menus share the corrected live actions.
- Bound ruler-position submenus and their directional choices, plus the Layer
  Mask submenu. Fixed additional one-time New Document/export/refinement labels.
- Application-owned OK/Apply/Cancel/Close/Create buttons now use our catalogs,
  retaining Qt roles/default-button behavior. They no longer depend on the
  desktop theme's language or lose bindings to queued Qt language events.
- Reproduced the Arabic dropdown geometry error: 28-pixel Arabic rows had cached
  positions only 18 pixels apart after switching from English. Allowing model
  display-change notifications invalidates the view's cached layout. Control
  edit signals remain blocked; display-only changes cannot toggle effects.

Release regressions: **8/8 passed** (localization runtime, catalogs, authoring
tools, installed packs, shortcuts, preferences/themes, Bevel effects, Color).
The expanded runtime test checks both English-created and Arabic-created
workspaces across repeated switches, the listed actions and ruler menus, existing
Preferences/About/New Document/Canvas Size/Color buttons, unchanged document
state and effect-edit counts, and non-overlapping dropdown rows during hover.
Native widget runs passed on **XCB (100%, 150%)** and **Wayland (125%)**.
Arabic popup captures were inspected in `temp/localization-fix-review/`.
No renderer change, package rebuild, or new Vulkan validation run was needed.

Catalogs now contain **1,633 English/French/Arabic entries**. All earlier coverage
limitations and human translation-review requirements still apply.

Logs: `/tmp/localization-fix-tests.log`, `/tmp/localization-fix-xcb.log`,
`/tmp/localization-fix-xcb150.log`, `/tmp/localization-fix-wayland125.log`.

## Original foundation delivery — 2026-10-10

## Scope and environment

Current Linux branch; no Windows work, release packages, version change or
language downloads. Release is available through `./temp/release-test`, pointing
to `build/release/src/app/imageeditor`.

Tested on Fedora 44 x86-64, Qt 6.11.2, GCC 16.2.1, CMake 4.3.0 and an NVIDIA
GeForce RTX 4080. The local build-only `lrelease` is from
`qt6-linguist-6.11.2-1.fc44.x86_64`; Qt dialog catalogs are from
`qt6-qttranslations-6.11.2-1.fc44.noarch`. The existing Ubuntu/glibc package
baseline was not changed or rebuilt in this pass.

## Automated results

| Check | Result |
| --- | --- |
| Focused Release suite | 12/12 passed, 9.20 s |
| Focused Debug suite | 5/5 passed, 3.08 s |
| Catalog validation | 1,627 English IDs; 1,627 French and 1,627 Arabic draft translations |
| Fractional-scale widget runs | 125% and 150% passed |
| Native Wayland / Arabic | Smoke assertions passed; Vulkan validation: 0 warnings, 0 errors |
| Native XCB / French | Smoke assertions passed; Vulkan validation: 0 warnings, 0 errors |
| CMake install staging | Successful; English/French/Arabic/pseudo QMs, metadata, Qt dialog QMs and notices installed |

The Release selection includes localization runtime/catalog/tooling/installed
resource tests, shortcuts, preferences/themes, text controller, color panel,
dock workspace, Bevel effects and PSD import/export interaction tests. Debug
covers the four localization tests plus the text controller. These are focused
regressions, not a claim that the entire application's test suite was rerun.

```sh
ctest --test-dir build/release --output-on-failure \
  -R 'imageeditor_(localization|shortcut|theme_preferences|text_controller|psd_export_interaction|psd_import_interaction|color_panel|overlay_dock_workspace|bevel_effects)'
ctest --test-dir build/debug --output-on-failure \
  -R 'imageeditor_(localization|text_controller)'
```

One XCB smoke attempt launched concurrently with the Wayland lifecycle exercise
failed its presentation-resume assertions. Vulkan validation still reported
zero warnings/errors. The isolated XCB retry passed. The failed log is retained
at `/tmp/vulkana-localization-xcb-concurrent.log`; this was not resolved by
weakening an assertion. Run native lifecycle tests separately to avoid competing
window-state exercises; a recurrence in isolated use still needs investigation.

### What the localization tests prove

- Existing controls, menu actions/default-action buttons, combo values/data and
  cached tooltips update without triggering parameter/action edits.
- Repeated language switches preserve open document identities, revisions,
  history depth, active tab, window geometry, saved dock state and tab groups.
- Arabic form direction is scoped; the application's workspace remains LTR.
  ASCII negative/decimal input and all six Arabic plural categories are checked.
- Chinese/Japanese/Korean preedit, commit, UTF-16 surrogate replacement and undo
  work through a field and the canvas text controller. Language changes defer
  until composition or a guarded interaction is complete.
- Editing/compiling English packs changes wording without rebuilding or changing
  the executable's hash. Reselecting English reloads its pack. Missing/corrupt
  optional packs fall back to readable embedded English.
- Sync retains contributor text and old-source review flags. Tests deliberately
  damage duplicate IDs, placeholders, plurals, markup and menu accelerators and
  check the corresponding validation errors.
- The installed-resource probe has **no development-resource lookup**. Its
  temporary `bin` + `share/vulkana-editor/translations` layout loads local packs
  and Qt's standard dialog translations without an explicit path override.

The normal Release executable was also staged under
`/tmp/vulkana-localization-stage-9ZVRpz/usr`. This validates the install manifest,
not an actual RPM/AppImage/DEB upgrade. The separate relocatable probe validates
installed lookup; do not mistake a development executable's source-tree lookup
for that test.

Logs: `/tmp/vulkana-localization-regressions.log`,
`/tmp/vulkana-localization-debug-tests.log`,
`/tmp/vulkana-localization-wayland.log`,
`/tmp/vulkana-localization-xcb.log`, and
`/tmp/vulkana-localization-install.log`.

## Visual review and remaining work

Generated English, French, Arabic and expanded-pseudo workspace/panel/Preferences
captures are in `temp/localization-review/` (not shipped). French Preferences and
Arabic panel shaping/spacing were inspected at 125%. Docks and artwork are not
mirrored. Numeric values retain ASCII digits. This is representative inspection,
not human approval of every phrase or every narrow-panel configuration.

**Catalog completeness is not application-wide coverage.** Detailed PSD/PDF
capability reports, some service/model/codec errors, core-generated operation
notifications and bootstrap/CLI text still contain English. Some dynamically
generated descriptions refresh only on their next normal update. They need the
typed-diagnostic/presentation migration described in [the guide](LOCALIZATION.md).
No arbitrary user text, names or paths are translated to conceal these gaps.

Pending owner review:

1. French/Arabic wording, especially Save/Discard/Delete/Replace/Apply and
   overwrite/unsaved-document decisions.
2. Narrow panels, all adjustment/effect pages, PSD/PDF review and mixed Arabic/
   Latin filenames, punctuation, keyboard shortcuts and units.
3. Real desktop IME candidate-window placement and Enter/Escape behavior using
   installed Chinese/Japanese/Korean input methods. Synthetic event tests do
   not replace that desktop check.
4. Next package build: test installed RPM/AppImage/DEB offline loading and record
   the exact Qt translation catalog sources/notices in the release inventory.

No renderer/model work was added to language switching. This pass does not
provide a measured GPU/cache performance benchmark for repeated live switches.
Remaining language catalogs should wait for this first owner review.
