#!/usr/bin/env python3
"""Optional independent PDF QA. Developer tools: Poppler, Pillow, NumPy, pypdf.

Run the C++ PDF export test with VULKANA_PDF_EXPORT_OUTPUT first, then pass that
directory here. This never updates a reference and is not an app dependency.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import numpy as np
from PIL import Image, ImageFilter
from pypdf import PdfReader


def run(*args, env=None):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=env).stdout


def check(directory):
    for reference in sorted(directory.glob("*-reference*.png")):
        name = reference.stem
        if name.startswith("alpha"):
            # Its RGB+SMask bytes are checked exactly in C++; a viewer's chosen
            # paper backdrop is not Vulkana's linear-light matte operation.
            continue
        if "-reference-" in name:
            stem, page = name.split("-reference-")
            pdf = directory / (stem + "-export.pdf")
        else:
            stem, page = name.removesuffix("-reference"), "1"
            pdf = directory / (stem + ".pdf")
        if not pdf.exists():
            continue
        expected = Image.open(reference).convert("RGB")
        output = directory / (stem + "-independent-" + page)
        native_text = stem.startswith("text-transform") or stem == "hybrid"
        # Splash (pdftoppm) bilinearly resamples some large images even on a
        # nominal native grid. Cairo respects the unfiltered image transform;
        # compare its decoded raster exactly AND independently extract pixels.
        renderer = "pdftoppm" if native_text else "pdftocairo"
        if not native_text:
            images = PdfReader(pdf).pages[int(page)-1].images
            assert len(images) == 1, (pdf, "unexpected embedded image count")
            assert images[0].image.convert("RGB").tobytes() == expected.tobytes(), (pdf, "embedded pixels changed")
        args = (renderer, "-f", page, "-l", page, "-scale-to-x", str(expected.width),
                "-scale-to-y", str(expected.height), "-singlefile", "-png", str(pdf), str(output))
        run(*args)
        actual = Image.open(output.with_suffix(".png")).convert("RGB")
        a, b = np.array(actual).astype(int), np.array(expected).astype(int)
        assert a.shape == b.shape, (pdf, a.shape, b.shape)
        delta = np.abs(a - b).max(axis=2)
        if native_text:
            # Native-density font hinting and transformed raster reconstruction
            # can differ by two pixels. Every mixed/affine case ALSO has a 4x
            # density check with the same two-pixel reconstruction support: this
            # distinguishes hinting from changed outlines (and caught Qt faux
            # bold, which required six high-density pixels and now falls back).
            # At 4x this permits at most half a document pixel around an edge.
            radius = 1 if stem == "hybrid" else 2
            hi = np.array(expected.filter(ImageFilter.MaxFilter(radius*2+1))).astype(int)
            lo = np.array(expected.filter(ImageFilter.MinFilter(radius*2+1))).astype(int)
            edges = (hi - lo).max(axis=2) > 1
            # Two 8-bit levels account for different AA tail quantization.
            outside = int(np.count_nonzero((delta > 2) & ~edges))
            assert outside == 0, (pdf, "changed away from glyph edges", outside)
            # Embedded fonts must render identically without any installed fonts.
            with tempfile.TemporaryDirectory(prefix="vulkana-pdf-font-check-") as temporary:
                root = Path(temporary)
                (root / "fonts").mkdir()
                config = root / "fonts.conf"
                config.write_text(f'<fontconfig><dir>{root}/fonts</dir><cachedir>{root}/cache</cachedir></fontconfig>')
                environment = {**os.environ, "FONTCONFIG_FILE": str(config), "FONTCONFIG_PATH": str(root)}
                no_fonts = root / "render"
                run(*args[:-1], str(no_fonts), env=environment)
                assert Image.open(no_fonts.with_suffix(".png")).convert("RGB").tobytes() == actual.tobytes(), (pdf, "host font dependency")
        else:
            assert int(delta.max()) == 0, (pdf, "raster changed", int(delta.max()))
        print(f"{pdf.name} page {page}: max={delta.max()}, mean={np.abs(a-b).mean():.6f}, changed={np.count_nonzero(delta)}")
        inventory = run("pdfimages", "-list", str(pdf)).decode()
        assert " jpeg " not in inventory and " jpx " not in inventory, (pdf, "lossy image encoding")
        run("pdfinfo", str(pdf))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    for directory in parser.parse_args().directories:
        check(directory.resolve())
