#!/usr/bin/env python3
"""Optional independent validation of PsdExportTests outputs.

Development-only dependencies: psd-tools 1.23.0 and Pillow. No private artwork,
external editor, or Python runtime is needed by Vulkana or its CTest suite.
Run the C++ fixture generator first; pass its output directory here.
"""
import logging
import sys
from pathlib import Path

import psd_tools
from PIL import Image
from psd_tools import PSDImage
from psd_tools.constants import Resource, Tag


class RejectWarnings(logging.Handler):
    def emit(self, record):
        if record.levelno >= logging.WARNING:
            raise AssertionError(record.getMessage())


logging.getLogger("psd_tools").addHandler(RejectWarnings())
root = Path(sys.argv[1])


def read(name):
    p = PSDImage.open(root / name)
    h = p._record.header
    assert (h.version, h.depth, h.channels, h.color_mode.value) == (1, 8, 4, 3)
    assert p.image_resources.get_data(Resource.ICC_PROFILE)[36:40] == b"acsp"
    return p


exact = read("exact.psd")
l = next(x for x in exact if x.name == "Samples α")
assert l.bbox == (-4, 3, 256, 8) and not l.visible and l.opacity == 153
assert l.blend_mode.value == b"mul "
expected = bytearray((n * 73 + n // 101) % 256 for n in range(260 * 5 * 4))
expected[128 * 4:259 * 4] = bytes([37]) * (131 * 4)
assert l.topil(apply_icc=False).tobytes() == expected
resolution = exact.image_resources.get_data(Resource.RESOLUTION_INFO)
assert resolution.horizontal == resolution.vertical == 300 * 65536

masked = read("mask.psd")[0]
m = masked._record.mask_data
assert (m.left, m.top, m.width, m.height, m.background_color) == (7, 2, 6, 7, 255)
assert m.flags.mask_disabled
assert masked.mask.topil().tobytes() == bytes([128]) * 42
assert masked.topil(apply_icc=False).getpixel((0, 0)) == (200, 100, 20, 255)

p = read("editable.psd")
layers = {l.name: l for l in p.descendants()}
assert layers["Editable ember"].kind == "type"
assert layers["Editable ember"].text == "Ember café"
mixed = layers["Mixed words"]
assert mixed.text == "Mixed words"
runs = mixed.engine_dict["StyleRun"]
assert [int(n.value) for n in runs["RunLengthArray"]] == [6, 6]
assert [float(r["StyleSheet"]["StyleSheetData"]["FontSize"].value)
        for r in runs["RunArray"]] == [28, 21]
for l in (layers["Editable ember"], mixed):
    for f in l.resource_dict["FontSet"]:
        assert f["Name"].value and not f["Name"].value.startswith("/")
    assert sum(int(n.value) for n in l.engine_dict["StyleRun"]["RunLengthArray"]) == len((l.text + "\r").encode("utf-16-be")) // 2
for name in ("Rectangle", "Hollow triangle", "Ellipse"):
    assert layers[name].kind == "shape" and layers[name].has_vector_mask()
stroke = layers["Live Stroke"].tagged_blocks.get_data(Tag.OBJECT_BASED_EFFECTS_LAYER_INFO)
assert float(stroke[b"FrFX"][b"Sz  "].value) == 5
assert stroke[b"FrFX"][b"enab"].value
base = layers["Half alpha base"]
assert not base.clipping and base.opacity == 204
assert base.topil(apply_icc=False).getpixel((0, 0))[3] == 128
for name in ("Upper Multiply", "Upper Normal"):
    l = layers[name]
    assert l.clipping and l.parent == base.parent
    assert l.topil(apply_icc=False).getpixel((0, 0))[3] == 255  # no baked base
    assert l.tagged_blocks.get_data(Tag.BLEND_CLIPPING_ELEMENTS)
assert p.topil(apply_icc=False).tobytes() == Image.open(root / "editable-reference.png").convert("RGBA").tobytes()
flat = read("flattened.psd")
assert len(flat) == 1 and flat[0].kind == "pixel"
assert flat[0].topil(apply_icc=False).tobytes() == p.topil(apply_icc=False).tobytes()
assert all(l.kind != "type" for l in read("raster-text.psd").descendants())
assert not any(l.clipping for l in read("consolidated.psd").descendants())
assert not any(l.name == "Half alpha base" for l in read("omitted.psd").descendants())
assert read("multiline.psd")[0].text == "Café\rSecond line"
if (root / "astral.psd").exists():
    astral = read("astral.psd")[0]
    assert astral.kind == "type" and astral.text == "🞀🞁"
    assert [int(n.value) for n in astral.engine_dict["StyleRun"]["RunLengthArray"]] == [2, 3]

styles = read("styles.psd")
for layer, key in zip(styles, (b"FrFX", b"DrSh", b"IrSh", b"OrGl", b"IrGl", b"SoFi", b"GrFl")):
    effects = layer.tagged_blocks.get_data(Tag.OBJECT_BASED_EFFECTS_LAYER_INFO)
    assert key in effects and effects[key][b"enab"].value
    assert len(layer.effects) == 1
    assert layer.topil(apply_icc=False).size == (12, 12)  # no double-baked style
read("empty.psd")
assert len(read("empty-folder.psd")[0]) == 0
assert read("empty-folder-consolidated.psd")[0].kind == "pixel"
assert read("empty-text.psd")[0].text == ""
disabled = read("disabled-style.psd")[0].tagged_blocks.get_data(Tag.OBJECT_BASED_EFFECTS_LAYER_INFO)
assert not disabled[b"FrFX"][b"enab"].value and float(disabled[b"FrFX"][b"Sz  "].value) == 11
read("projective-text.psd")
print(f"Independent PSD structure/channels/composites passed (psd-tools {psd_tools.__version__})")
