# Localization

The Linux and Windows workspace has 16 local language packs: English, French, Spanish,
German, Italian, Brazilian Portuguese, Polish, Turkish, Russian, Ukrainian,
Modern Standard Arabic, Hindi, Simplified Chinese, Traditional Chinese,
Japanese and Korean. All language versions are first drafts. Reviews and
corrections from contributors are welcome, including English wording.
Choose **Preferences → General → Language**, or use the first-run choice.
Passing technical validation does not certify linguistic accuracy.

## Authoring and preview

`localization/en.ts` is the authoritative English wording. Code refers to stable
message IDs, not English source text. Edit the `<source>` for an existing ID;
do not rename its ID just because its wording changes. The English plural entry
also contains its singular/plural forms. There is no separately maintained
English runtime list. Each other language's TS file contains synchronized source
context and its translations. Qt Linguist can edit these catalogs.

From the repository root:

```sh
# After editing English, preserve translations and mark changed meanings for review.
python3 tools/localization.py sync
python3 tools/localization.py validate

# Compile only packs; does not rebuild or relink the application.
cmake --build build/release --target vulkana_language_packs

# Close an existing instance before these manual launches.
./build/release/src/app/imageeditor --language fr
./build/release/src/app/imageeditor --language ar
./build/release/src/app/imageeditor --language ja
./build/release/src/app/imageeditor --language pt-BR
./build/release/src/app/imageeditor --language qps
```

On Windows, use `python` instead of `python3`, build the same
`vulkana_language_packs` target in `<BuildRoot>/release`, and launch
`<BuildRoot>/stage/bin/imageeditor.exe` with the same language arguments. Re-stage
with `Build.ps1` to update its installed packs, or use `--language-pack-dir`
with `<BuildRoot>/release/translations` to preview newly compiled packs directly.
See the [Windows build guide](../tools/windows/README.md) for build paths.

RPM, DEB, AppImage and Windows staging automatically validate and recompile packs
through the shared CMake install step, including direct `cmake --install` or
CPack use. You do not need to compile packs manually before packaging. Invalid
catalogs or compiler failures stop staging instead of packaging stale packs.
After changing English wording, still run `sync` and review affected translations
before packaging; staging deliberately does not edit the source catalogs.

`qps` is an expanded pseudo-language for clipping/width tests, not a supported
language advertised in Preferences. `--language` is a launch-only override.
`--language system` uses the system UI language with English fallback.

To review a trusted pack without changing the installed application:

```sh
python3 tools/localization.py compile --output /tmp/vulkana-language-review \
  --lrelease /path/to/qt/bin/lrelease
/path/to/vulkana-editor --language fr --language-pack-dir /tmp/vulkana-language-review
```

For an AppImage, use its path in place of `/path/to/vulkana-editor`. The directory
contains `en.qm`, the chosen language's `.qm`, and `languages.json`. Qt's own
`qtbase_*.qm` files accompany installed packs where available; a development
launch may also resolve them from the Qt installation. The tested Qt distribution
has 14 matching non-English catalogs, but no Hindi pack. Vulkana-owned Hindi
controls/buttons are translated; Qt-owned untranslated dialogs fall back to
English. Only load trusted local QM files.
There are no downloads, network translation, runtime Python, or file watchers.
An already running process reloads catalogs when a language is selected again;
restarting is also sufficient. Embedded English is the last-resort fallback.

`--catalog-dir /path/to/catalogs` lets the authoring tool work on a separate copy
of the TS files and metadata. The default is this repository's `localization/`.
The CMake cache setting `VULKANA_LRELEASE` selects the build-time compiler;
point it at your Qt installation's `lrelease` if automatic lookup fails.
The compiler is not shipped with the application.

## Adding languages and submitting corrections

1. Add metadata to `localization/languages.json`: locale ID, native name, RTL
   flag, review status, Qt plural-form count, `enabled` and `qtCatalog`.
   Public IDs use `pt-BR`, `zh-Hans`, `zh-Hant`; corresponding Qt dialog catalog
   names are `pt_BR`, `zh_CN`, `zh_TW`. Start with `enabled: false`.
2. Run `python3 tools/localization.py sync --language <id>` to create its TS file.
3. Translate, validate, then enable the language in a trusted local metadata
   copy to compile and preview using `--language <id>`. Enable the shipping
   metadata when the translation draft is ready for delivery; keep
   `reviewPending` true while language-wide review remains incomplete.
4. Submit the TS/metadata diff and screenshots for any layout problem. Do not
   submit generated QM files or change document/serialization identifiers.

No C++ language branches are required. Disabled, empty, missing or invalid packs
are not advertised and do not prevent startup. Qt locale matching understands
language, territory and script: `pt_BR → pt-BR`, `zh-TW`/`zh-HK → zh-Hant`,
`zh-CN`/`zh-SG → zh-Hans`. Explicit scripts take precedence. `fr-CA` uses `fr`;
`pt-PT` does not silently become Brazilian Portuguese. Missing matches use
English. ASCII numbers remain independent of the chosen UI locale.

The 16-language lineup is translated and enabled in `localization/`:
`en`, `fr`, `es`, `de`, `it`, `pt-BR`, `pl`, `tr`, `ru`, `uk`, `ar`, `hi`,
`zh-Hans`, `zh-Hant`, `ja`, `ko`. The initial 1,731-message translation pass includes
the appropriate plural forms. New messages may fall back to English until the
next translation pass; run `validate` for current coverage. All language metadata retains `reviewPending: true`;
targeted corrections do not imply a complete review of a language.
CMake installs enabled packs plus the test pseudo-pack.

The sync command retains contributor translations/comments, preserves retired
entries as vanished, and marks changed English sources unfinished with their
old source for review. Unfinished translations use readable English. The
current draft comments identify AI assistance; remove/update those comments
as entries receive human review. Set a language's `reviewPending` false only
after a documented language-wide review, not merely because validation passes.

Keep `%1`, `%2`, `%n`, shortcut tokens such as `{{UndoAction}}`, markup and file
extension patterns intact. `%n` entries need the language's real plural forms
(English/French: two; Arabic: six). Arabic controls deliberately retain ASCII
digits and decimal points; translating the interface does not change numeric
parsing, file formats, color codes, fonts or artwork.
Do not put a literal percent sign immediately before digits (for example the
usual Turkish percentage order): Qt can interpret `%50` as an argument token.
Use `50%` for numeric percentages in translated UI copy.

### Shared terminology

| Meaning | French | Arabic |
| --- | --- | --- |
| Save | Enregistrer | حفظ |
| Discard edits | Abandonner / ne pas enregistrer | تجاهل التغييرات |
| Delete | Supprimer | حذف |
| Replace | Remplacer | استبدال |
| Apply | Appliquer | تطبيق |
| Layer / mask | Calque / masque | طبقة / قناع |
| Brush stroke | Trait de pinceau | ضربة فرشاة |
| Stroke effect | Contour | حدود |

Contexts distinguish brush marks from the Stroke style. Existing user layer
names, paths, text content, preset IDs and original license texts are not
translated. Contributors can propose glossary corrections alongside catalog edits.

## Runtime contract

`src/ui/src/Localization.cpp` loads Qt QM catalogs with `QTranslator`. The small
presentation helper binds IDs/arguments to existing widget properties and
combobox entries. Switching refreshes labels in place, with control edit signals
blocked so translation is not a parameter edit. Model `dataChanged` signals must
remain enabled: views need them to refresh fallback-font metrics and row positions.
Checkable item handlers must ignore display-only changes. Document models, history and Vulkan/model
resources are not recreated. Core stays Qt-free; a generated compatibility
adapter presents existing core undo labels without translating stored commands.

Retain `i18n::Message` in label helpers/collections and use `i18n::make`,
`addAction`, `addMenu` or `setText` for persistent controls. Converting a message
to `QString` before binding loses its resolver and leaves the previous language
visible. Application-owned standard buttons use `i18n::ButtonBox` so queued Qt
language events or the desktop theme cannot overwrite their catalog bindings.
Custom button labels such as Create still use `i18n::setText`.
Cached update-service messages also retain their resolver: changing language
before reopening About must not show the old language's network status. Use
`i18n::ltrToken` for version numbers and other known Latin technical tokens in
mixed-direction prose. This is presentation-only; never insert bidi controls
into canonical filenames, paths, URLs or saved document data. Arabic About
uses an Arabic-first license line and isolates MIT/copyright tokens; its running
version label has no trailing period to drift across the number.

Application/workspace direction remains LTR. Arabic opts into RTL on form and
dialog content; it never reorders docks, layers, coordinates or artwork. Qt
handles Unicode shaping and bidi text. Numeric fields retain the C locale.
The menu bar reserves fallback-script height to avoid shifting the canvas
when Arabic is activated. A requested change waits for mouse gestures and IME
preedit to end; it does not forcibly commit text or finish an editing operation.

Qt 6.11 changed ID-catalog handling. The compiler creates a context-free runtime
TS bridge before `lrelease -idbased`; authoring contexts remain intact in the
checked-in TS files. This supports the older supported Qt tool syntax and the
tested Qt 6.11.2 compiler. See [Qt ID-based translation](https://doc.qt.io/qt-6/linguist-id-based-i18n.html)
and [lrelease](https://doc.qt.io/qt-6/linguist-lrelease.html). No new runtime library
was incorporated. Unmodified Qt dialog translation data is installed separately;
its provenance and license inventory are in `QtTranslations-NOTICE.txt`. The
binary packages must include the matching Qt translation sources/notices required
for their distribution.

## Coverage and known remaining work

The catalogs have **1,731 English entries**, with translations in all 15 other
languages for every cataloged entry. It covers menus/tools, Properties, Adjustments/Effects,
layer/mask actions, preferences, shortcuts, update/feedback UI, Refine Selection,
selection tools, undo presentation, and the primary save/import/export controls
and prompts, project-file registration, canvas clipboard copying, screenshot
capture/cropping, and Performance preferences. This is **not a claim of complete
application-string coverage**.

The scope is the interface people use to edit and make decisions, not
translation of every string in the program. PSD content types, status, source
summary, conversion guidance, actionable export blockers and the final import
confirmation now update live. Worker results use presentation identifiers;
language switches do not replan, rerender or change the chosen conversions.
Technical report bodies remain English. A focused follow-up may cover:

- User-facing PDF review summaries, layer status and brief conversion reasons
  still supplied as English by the import/export services.
- Some model-generated row descriptions and dynamically composed labels can
  remain English or retain their current wording until their next refresh.

Low-level archive/codec/solver/model errors, developer logs, technical reports
and bootstrap/CLI diagnostics intentionally remain English. Do not migrate them
merely to raise translation coverage, and never translate arbitrary user text.

`python3 tools/localization.py inventory` reports directly authored untranslated
UI calls. Validation rejects newly introduced direct strings beyond the reviewed
`untranslated-baseline.json` (currently empty). This conservative guard is not
a C++ semantic analyzer: raw strings returned from services/helper functions
still need code review. Do not expand the baseline to hide new user-facing text.

Where a service supplies a user-facing conversion reason, use stable codes and
arguments and translate at the presentation boundary, rather than putting Qt
into core or using translated strings as program decisions. Technical diagnostic
details stay verbatim beneath a localized, actionable summary where needed.

## Validation and contributor review

```sh
python3 tools/localization.py validate
ctest --test-dir build/release -R 'localization|text_controller' --output-on-failure
```

The tests cover runtime binding/index stability, document IDs/revisions/history
and dock state, repeated switching, Arabic plurals and ASCII numeric input,
English fallback, contributor-source review flags, invalid packs, and changing
English in an external pack without changing the executable. Installed-resource
tests use a relocatable probe without development-path lookup and check Qt's
standard dialog translations. CJK tests send Chinese/Japanese/Korean preedit,
commit and surrogate-pair replacement through both a field and the canvas text
controller, including undo and finite candidate geometry. These are not a claim
that every desktop IME candidate window has been manually verified.

Manual language-review checklist for every language:

- Switch language with multiple edited documents open. Confirm names, changes,
  selections, zoom and dock/tab positions stay intact.
- Review Save, Discard, Delete, Replace and overwrite prompts particularly
  carefully, including unsaved-tab and app closure.
- Open every Tone/Color/Monochrome/Filter/Effect page, including Bevel contours,
  at narrow widths. Check controls, tooltips and no unexpected parameter edits.
- Test PSD/PDF reviews and raster export; note English technical report gaps
  separately from mistranslated UI labels.
- Open filenames mixing Arabic, French, Latin digits, punctuation and emoji.
  Check clipping and punctuation, not just whether all characters appear.
- Enter negative/decimal numbers and hex colors. Try 125% and 150% display scales.
- Type Arabic artwork plus Chinese/Japanese/Korean using a real desktop IME:
  candidate position, replacement, Enter/Escape ownership and undo.
- Preview `qps`; check expanded menus, dock labels, buttons and long help text.

Technical verification and known limits are recorded in
[Localization validation](LOCALIZATION_TEST_RESULTS.md). All language versions
remain open to review; report awkward terminology separately from layout or
runtime bugs.
