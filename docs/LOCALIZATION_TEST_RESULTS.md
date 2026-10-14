# Localization validation

All 16 language choices are enabled: en, fr, es, de, it, pt-BR, pl, tr, ru, uk,
ar, hi, zh-Hans, zh-Hant, ja and ko. Each non-English catalog contains a first
translation of all **1,731 active English entries**, including the required
plural forms. Retired contributor entries remain in the catalogs but do not
count toward active coverage.

All language versions are first drafts, including the English wording.
Contributors are welcome to review terminology, clarity and layout. Automated
validation and targeted fixes are not a substitute for language-wide review.
See the [localization guide](LOCALIZATION.md) for editing and preview commands.

The 0.3.3 catalog update adds 68 translations per non-English language for
project registration, canvas clipboard copying, screenshot capture/cropping,
and Performance preferences. Existing completed translations are retained.
Technical GPU names remain isolated left-to-right in Arabic messages; numeric
values, units, source pixels and selection coordinates remain unchanged.

## Recorded technical checks

These results describe specific regression runs, not a guarantee for every
desktop configuration or a claim that every page has been visually reviewed.

| Environment and date | Result |
| --- | --- |
| Fedora 44 x86-64, Qt 6.11.2, NVIDIA RTX 4080; 2026-10-11 | 11/11 focused Release tests passed: catalogs, authoring, runtime/installed lookup, updates, preferences, text/IME, PSD import/export and interaction |
| Same Linux environment; 2026-10-10 | 5/5 focused Debug localization and text-controller tests passed |
| Native Wayland at 125% and XCB at 150%; 2026-10-11 | Widget checks passed; PSD interaction checks reported zero Vulkan validation messages and no idle frame growth |
| Relocatable Linux install; 2026-10-11 | All 16 application QMs, pseudo-pack, metadata and 14 matching Qt QMs loaded without development-path lookup |
| Windows 11 x64, Qt 6.10.3, MSVC 19.51; 2026-10-12 | 16/16 focused Release tests passed, including localization runtime and catalog validation |
| Linux, Qt 6.11.2; 2026-10-14 | 9/9 focused Release checks passed: catalogs, authoring, installed lookup/staging, runtime, preferences, Performance, screenshots and updates; 2/2 focused Debug checks passed |
| Native Wayland at 125% and XCB at 150%; 2026-10-14 | Widget/localization checks passed, including new Performance, screenshot and registration layouts, repeated language switches and unchanged crop/settings |
| Windows 11 x64, Qt 6.10.3; 2026-10-14 | Localization, screenshot and Performance tests passed with the 0.3.3 test build and compiled packs |

The tested Qt translation distributions have no Hindi Qt dialog catalog.
Vulkana-owned Hindi controls are translated; some Qt-owned dialogs fall back to
English. Qt translation data retains its original licensing and notices.

## Regression coverage

- Existing menus, controls, combo values and cached tooltips refresh without
  triggering editing actions. Repeated switches preserve document identities,
  revisions, history, selections, dock groups and view state.
- Arabic uses scoped RTL forms while artwork, coordinates and docks stay LTR.
  Numeric controls retain ASCII digits, decimal points and negative signs.
  All six Arabic plural categories are covered.
- Application-owned OK/Apply/Cancel/Close/Create buttons retain their bindings
  through queued Qt language events. Menu actions, rulers and mask actions use
  the same live translation path as other controls.
- Dropdown tests check sufficient script height and non-overlapping rows after
  language changes and repeated hover. Display-change notifications invalidate
  cached view geometry without generating parameter edits.
- About text isolates MIT/copyright and version tokens in mixed-direction prose.
  Version captions avoid trailing punctuation that can appear before a number.
  Cached update results translate when About is reopened after changing language.
- PSD review summaries, conversion guidance and confirmation text update without
  changing row identity, choices, group expansion or requesting new rendering.
  Technical reports and user-authored names remain unchanged. Filters/Effects
  headers show their disabled state for adjustment-layer targets.
- Chinese/Japanese/Korean preedit, commit, surrogate-pair replacement, undo and
  finite candidate geometry are tested through text fields and the canvas text
  controller. Language changes wait for composition or guarded interactions.
- Editing and compiling an external English pack changes wording without
  rebuilding the executable. Missing or corrupt optional packs fall back to
  readable embedded English. Installed-resource probes disable source-tree lookup.
- Catalog checks cover IDs, placeholders, plural forms, markup, accelerators,
  contributor corrections and changed-source review flags. Turkish literal
  percentages use `50%`, not `%50`, to avoid Qt argument-token ambiguity.

## Reproducing focused checks

Configure and build with tests enabled as described in the
[main README](../README.md). From the repository root on Linux:

```sh
python3 tools/localization.py validate
python3 tools/check-readme-translations.py
ctest --test-dir build/release --output-on-failure \
  -R 'imageeditor_(localization|theme_preferences|text_controller|psd_export_interaction|psd_import_interaction)'
```

On Windows, `tools/windows/Build.ps1 -Tests` builds and runs the available native
Release regression suite. See the [Windows guide](../tools/windows/README.md)
for prerequisites, separate build roots and interactive smoke tests.

Run native window-lifecycle tests separately: simultaneous Wayland/XCB exercises
can compete for focus and window state. A concurrent XCB lifecycle run previously
failed presentation-resume assertions; the isolated retry passed with zero Vulkan
messages. A recurrence during isolated use still needs investigation.

The native workspace test waits for initial window-manager placement to settle
before recording its geometry baseline. An XCB run otherwise recorded the
pre-placement origin and later mistook the initial configure event for a language
change. Geometry equality remains strict throughout the language-switch loop.

Popup geometry must be compared in content coordinates when a menu scrolls.
A recorded XCB row-position difference was one legitimate 28-pixel scroll step,
not a change in row height. Tests bring rows into view and account for scrolling
without weakening size or overlap assertions.

## Remaining review

- Review every language's wording, especially Save, Discard, Delete, Replace,
  Apply and overwrite/unsaved-document choices. README translations need their
  own review; catalog completeness does not certify documentation quality.
- Check narrow panels, all adjustment/effect pages, PSD/PDF reviews, mixed-script
  filenames and fractional display scales. Representative layouts have been
  inspected; coverage is not exhaustive.
- Test real desktop CJK IME candidate windows, placement and Enter/Escape
  behavior. Synthetic composition tests cannot certify each installed input method.
- Verify offline catalog loading in each final package and keep matching Qt
  translation sources/notices with the release materials. Install-staging tests
  are not package-upgrade tests.

Catalog completeness is not application-wide string coverage. Some PDF review
summaries and dynamic descriptions still use English. Low-level codec/model
errors, technical reports, logs and CLI diagnostics intentionally remain English;
user artwork, names and paths must never be translated. Track user-facing gaps
separately from these deliberate exclusions.
