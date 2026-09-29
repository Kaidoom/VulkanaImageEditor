# PDF import

Open or drop a PDF to choose pages and rasterization settings. Import as Layer
defaults to the current document; Open defaults to a new document. Pages can
also open in separate tabs. Only successful imports update recent files and
remember PPI/background/annotation preferences.

## Page geometry and pixels

Qt PDF renders the intersection of CropBox and MediaBox, with inherited page
rotation. qpdf supplies precise box values and `/UserUnit` scaling (ignored by
Qt PDF 6.11.2); its geometry must agree with the renderer before import proceeds.
No whitespace, artwork or print marks are trimmed. Output dimensions are
`max(1, floor(points × PPI / 72 + 0.5))` independently for each axis and page.
PPI changes resolution, not aspect ratio. Device DPI and canvas zoom do not enter
this calculation. Ranges use one-based physical page numbers; custom labels are
shown separately.

White paper composites the page over white. Transparent retains unpainted areas;
it does not remove painted white. Output uses straight RGBA8, sRGB device color,
top-left origin, and ordinary antialiasing rather than LCD-subpixel text.
Embedded fonts are rendered by PDFium. This is RGB artwork import, not CMYK
proofing, spot-color or print-separation preservation.

Render Annotations controls ordinary stored annotation appearances. Interactive
form widgets are not rendered by this Qt path, even when an appearance is
present. There is no form filling, JavaScript, link/action activation, embedded
file opening or document-requested networking.

## Documents and history

Each page becomes an ordinary visible Normal/100% raster layer at its own size.
New multipage documents use the independent maximum selected width and height;
all pages align top-left, page 1 of the chosen set is topmost and active. Separate
tabs use each page's exact size and the requested PPI. Current-document imports
use top-left placement above the active item, leaving canvas size/PPI unchanged.
File drops currently provide no placement point to the file-import route, so
the same top-left rule applies. Imported pages never inherit existing effects.

The PDF path is source provenance, not a project-save path. Save asks for a
`.vulkana` destination. All pixels are retained in the project; reopening it or
redoing an import never needs the PDF again.

The owned modal overlay pins the destination instance, revision, selection and
insertion point. Tab activation/closing is blocked while it is open; unexpected
external target mutation rejects publication. Rendering and document creation
are staged before any existing tree changes. Current-document insertion is one
atomic undo action, including selection restoration. Failed/cancelled imports
publish no layers or tabs.

## Worker and budgets

Qt PDF and qpdf objects are created, used and destroyed on one shared worker;
immutable input bytes and an ephemeral password never enter canonical document
data. Up to four jobs can be queued. The UI polls only while work is outstanding.
Thumbnails are visible-page requests, at most 256 pixels per side, with a
32-entry LRU. Background/annotation changes cancel obsolete requests. Final
pages always render from PDF content, never enlarged thumbnails.

Preflight limits input to 128 MiB/4096 pages, output to 40 megapixels per page,
16384 pixels per side (or the smaller GPU limit), 256 MiB aggregate raster data,
and 1.5 GiB estimated working memory. The estimate includes existing document
data/history, GPU storage, conversion buffers, source bytes and a decoder
reserve. Linux additionally reserves desktop headroom using MemAvailable.
Current-document history/layer limits are checked separately. Resolution is
never silently lowered.

Qt's page render cannot be interrupted midway. Cancel closes the overlay
immediately and discards the current page and earlier staged results once the
renderer returns. Shutdown waits safely for engine ownership to finish. These
are bounded scheduling/allocation checks, not a decoder sandbox or a hard cap
on internal PDF-engine scratch allocations; use maintained Qt/qpdf packages.

## Tests

`imageeditor_pdf_import_tests` covers geometry, ranges, alpha, annotations/forms,
password errors, limits, cancellation, source independence and project/export
round-trips. `imageeditor_pdf_import_interaction_tests` covers destinations,
ordering, undo/redo and pinned-target UI behavior; `--native` adds real Vulkan
rendering and validation. Tests use original small fixtures under
`tests/fixtures/pdf`; `generate_pdf_fixtures.py` is a maintainer utility, not an
application dependency. Optional PDF arguments to the service test measure
private references without adding them to the test or install requirements.

Backend references: [Qt PDF API](https://doc.qt.io/qt-6/qpdfdocument.html),
[render options](https://doc.qt.io/qt-6/qpdfdocumentrenderoptions.html),
[qpdf API](https://qpdf.readthedocs.io/en/stable/library.html).
