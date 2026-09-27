# Pinned text-test fonts

These unmodified, test-only font fixtures are not installed on the host or included
in the application's resources. They make Text V1 layout/render tests independent
of the user's default fonts. There were no existing font fixtures in the repository.

## Provenance and license

Official upstream: [Noto fonts archive](https://github.com/notofonts/noto-fonts),
commit [`ffebf8c1ee449e544955a7e813c54f9b73848eac`](https://github.com/notofonts/noto-fonts/tree/ffebf8c1ee449e544955a7e813c54f9b73848eac).
Retrieved 2026-09-06. The archived repository is deliberately used for immutable,
already-built static faces, not as a source of current font-release recommendations.

Both files are covered by the **SIL Open Font License 1.1**. The upstream license is
copied unchanged into [OFL.txt](OFL.txt); keep it with any redistributed fixtures.
The embedded font metadata/copyright notices are unchanged. No subsetting,
renaming, conversion, or other modification has been performed.

| Fixture | Bytes | Family/style | Exact pinned source |
| --- | ---: | --- | --- |
| `NotoSans-Regular.ttf` | 569,208 | Noto Sans / Regular | [Raw TTF](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans/NotoSans-Regular.ttf) |
| `NotoSansArabic-Regular.ttf` | 240,456 | Noto Sans Arabic / Regular | [Raw TTF](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSansArabic/NotoSansArabic-Regular.ttf) |
| `OFL.txt` | 4,377 | Upstream license | [Raw license](https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/LICENSE) |

The two fonts total **809,664 bytes** (about 791 KiB). [SHA256SUMS](SHA256SUMS)
records exact SHA-256 hashes for both fonts and the license. Verify from this folder:

```sh
sha256sum --check SHA256SUMS
```

## Loading in Qt tests

After creating the test `QGuiApplication` / `QApplication`, load each needed fixture
once with `QFontDatabase::addApplicationFont(path)`. Fail the test if the returned ID
is negative; obtain the family with `QFontDatabase::applicationFontFamilies(id)`
instead of silently accepting a system substitute. Explicitly request `Regular`.
No OS installation or font-cache update is necessary. Qt describes this process in
its [application-font API documentation](https://doc.qt.io/qt-6/qfontdatabase.html#addApplicationFont).

For glyph/render assertions, load `QRawFont` directly from the fixture as well and
compare the shaped run's identifying font tables (for example `head`, `cmap`, and
`name`) against that reference. This detects accidental use of a same-name system
font. Keep the document-space font size, hinting, Qt/FreeType/HarfBuzz versions,
layout direction, device pixel ratio, and rasterization scale explicit in golden
tests: pinning font bytes alone does not guarantee byte-identical pixels across
different text-stack versions. Prefer geometry/invariant assertions when testing
different monitor scales and zoom levels.

## Intended coverage and limitations

- **Noto Sans Regular:** Latin, Greek, Cyrillic; ASCII O/whitespace hit testing;
  multiline layout; ligatures; composed versus combining accents such as
  `e\u0301`; mixed UTF-8 byte lengths; rich-format boundaries; size-9 / size-20
  insertion tests. Its character map includes U+0300–036F and selected extended
  combining ranges, not every Unicode combining mark.
- **Noto Sans Arabic Regular:** real Arabic joining, right-to-left shaping, and
  combining Arabic marks, e.g. `سَلَام`. Select this family explicitly for that run.
  It has no Latin alphabet, so mixed-script tests should assign the appropriate
  fixture to each script instead of relying on the machine's fallback order.
- These are **Regular faces only**. Bold/italic state, range application, and
  preservation can be tested semantically; exact styled pixel goldens require
  separately pinned actual Bold/Italic faces, not system faces or synthetic styles.
- Neither fixture provides CJK, Indic scripts, or emoji. UTF-8 offset/grapheme tests
  may still use supplementary characters and emoji sequences without claiming
  pinned glyph rendering for them. Test fallback behavior separately and preserve
  requested descriptors; do not assert host-dependent fallback pixels as goldens.
- Font fixtures do not automate real Wayland input-method candidate UI, external
  IME behavior, or manual interaction feel. Those remain explicit hands-on checks.
