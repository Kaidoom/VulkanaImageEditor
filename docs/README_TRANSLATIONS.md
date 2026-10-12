# README translations

The [English README](../README.md) is the source overview. Full translations live
in `docs/readme/README.<language-id>.md`, using the same IDs and native language
names as `localization/languages.json`. The language links at the top of every
README let readers switch directly between all 16 versions.

These are documentation translations, separate from the application's `.ts`/`.qm`
language packs. All language versions are first drafts. Reviews and corrections
from anyone interested are welcome; interface and documentation wording should
be checked separately.

## Editing

- Update English first when a feature or requirement changes, then update the
  corresponding sections in the translated READMEs. Keep wording concise and
  natural; sentence-for-sentence translation is not required.
- Preserve shell commands, example filenames, inline code, package names and
  dependency versions exactly. Commands run from the repository root regardless
  of which README is being read.
- Keep Linux and Windows build instructions in sync with English. The detailed
  [Windows guide](../tools/windows/README.md) is the shared reference for pinned
  dependencies and portable staging; avoid duplicating its version list.
- Adjust relative documentation/image links for the nested directory. Keep all
  16 language links on every page, using native language names.
- Translate the license-description paragraph, not the original license or
  third-party legal texts. Link to the unchanged originals.
- Arabic prose uses an RTL container. Keep the language navigation and shell
  examples LTR; do not insert bidi controls into commands or paths.

From the repository root, validate the documentation without building the app:

```sh
python3 tools/check-readme-translations.py
```

This checks language navigation, local links/images, copyable shell/PowerShell
commands, inline technical tokens, dependency versions and section coverage. It does not certify
linguistic accuracy or replace a reading/layout review. When adding a supported
language, add its README and update every language-navigation block too.

The checker and its regression tests also run through CTest on Linux and Windows:

```sh
ctest --test-dir build/release -R imageeditor_readme --output-on-failure
```

On Windows, replace `build/release` with your `<BuildRoot>/release` directory.
