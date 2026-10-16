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
from psd_tools.psd.engine_data import Float, Integer


class RejectWarnings(logging.Handler):
    def emit(self, record):
        if record.levelno >= logging.WARNING:
            raise AssertionError(record.getMessage())


logging.getLogger("psd_tools").addHandler(RejectWarnings())
root = Path(sys.argv[1])


def check_modern_text(p):
    layers = [l for l in p.descendants() if l.kind == "type"]
    if not layers:
        assert Tag.TEXT_ENGINE_DATA not in p.tagged_blocks
        return
    global_data = p.tagged_blocks.get_data(Tag.TEXT_ENGINE_DATA)
    assert global_data is not None
    objects = global_data['1']['1']
    fonts = global_data['0']['1']['0']
    frames = global_data['0']['8']['0']
    assert len(objects) == len(frames) == len(layers)
    indices = []
    for l in layers:
        index = l.tagged_blocks.get_data(Tag.TYPE_TOOL_OBJECT_SETTING).text_data[b'TextIndex'].value
        indices.append(index)
        obj = objects[index]
        assert obj['0']['0'].value == l.engine_dict['Editor']['Text'].value
        assert obj['1']['0'][0]['0'].value == index
        assert '2' not in obj['1']  # never retain stale glyph/layout caches
        runs = obj['0']['6']['0']
        legacy = l.engine_dict['StyleRun']
        assert [x['1'].value for x in runs] == [x.value for x in legacy['RunLengthArray']]
        for r, old in zip(runs, legacy['RunArray']):
            assert r['0']['0']['5'].value == 0  # pixel units, not PPI-dependent points
            style = r['0']['0']['6']
            old = old['StyleSheet']['StyleSheetData']
            assert fonts[style['0'].value]['0']['0']['0'].value == l.resource_dict['FontSet'][old['Font'].value]['Name'].value
            for key, old_key in [('1', 'FontSize'), ('6', 'HorizontalScale'), ('7', 'VerticalScale')]:
                assert isinstance(style[key], Float)
                assert style[key].value == old[old_key].value
            assert [v.value for v in style['53']['0']['1']] == [v.value for v in old['FillColor']['Values']]
        paras = obj['0']['5']['0']
        old = l.engine_dict['ParagraphRun']
        assert [x['1'].value for x in paras] == [x.value for x in old['RunLengthArray']]
        assert all(x['0']['0']['6'].value == 0 for x in paras)
        assert [x['0']['0']['5']['0'].value for x in paras] == [x['ParagraphSheet']['Properties']['Justification'].value for x in old['RunArray']]
    assert sorted(indices) == list(range(len(layers)))


def read(name):
    p = PSDImage.open(root / name)
    h = p._record.header
    assert (h.version, h.depth, h.channels, h.color_mode.value) == (1, 8, 4, 3)
    assert p.image_resources.get_data(Resource.ICC_PROFILE)[36:40] == b"acsp"
    check_modern_text(p)
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
    for run in l.engine_dict["StyleRun"]["RunArray"]:
        style = run["StyleSheet"]["StyleSheetData"]
        for key in ("FontSize", "Leading", "HorizontalScale", "VerticalScale", "BaselineShift"):
            assert isinstance(style[key], Float), (key, type(style[key]))
        for key in ("Font", "Tracking", "Kerning"):
            assert isinstance(style[key], Integer), (key, type(style[key]))
        assert all(isinstance(v, Float) for v in style["FillColor"]["Values"])
    for run in l.engine_dict["ParagraphRun"]["RunArray"]:
        assert all(isinstance(v, Float) for v in run["Adjustments"]["Axis"])
        assert all(isinstance(v, Float) for v in run["Adjustments"]["XY"])
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
for dpi in (72, 300):
    modern = read(f"modern-text-{dpi}.psd")
    assert modern.image_resources.get_data(Resource.RESOLUTION_INFO).horizontal == dpi * 65536
    texts = {l.name: l for l in modern if l.kind == "type"}
    assert texts['First'].text == 'Hello Vulkana'
    assert texts['Second'].text == 'Changed café\rSecond line'
    assert texts['First'].engine_dict['StyleRun']['RunArray'][0]['StyleSheet']['StyleSheetData']['FontSize'].value == 32
    assert texts['Second'].engine_dict['StyleRun']['RunArray'][0]['StyleSheet']['StyleSheetData']['FontSize'].value == 21.5
if (root / "astral.psd").exists():
    astral = read("astral.psd")[0]
    assert astral.kind == "type" and astral.text == "🞀🞁"
    assert [int(n.value) for n in astral.engine_dict["StyleRun"]["RunLengthArray"]] == [2, 3]

for i in range(4):
    layer = read(f"transformed-mask-{i}.psd")[0]
    assert layer.has_mask() and layer.kind == "pixel"
    assert layer.mask.disabled == (i == 2)
    fx = layer.tagged_blocks.get_data(Tag.OBJECT_BASED_EFFECTS_LAYER_INFO)
    assert fx[b"OrGl"][b"enab"].value and fx[b"OrGl"][b"Md  "].enum == b"Scrn"

for name, text in (("fallback-text.psd", "Flower"), ("fallback-unicode.psd", "fi é தமிழ்\rA🞀")):
    if not (root / name).exists():
        continue  # Optional installed-font fixture.
    layer = read(name)[0]
    assert layer.kind == "type" and layer.text == text and layer.has_mask()
    assert layer.tagged_blocks.get_data(Tag.OBJECT_BASED_EFFECTS_LAYER_INFO)[b"OrGl"][b"enab"].value
    lengths = [int(n.value) for n in layer.engine_dict["StyleRun"]["RunLengthArray"]]
    assert all(n > 0 for n in lengths) and sum(lengths) == len((text + "\r").encode("utf-16-be")) // 2

styles = read("styles.psd")
assert len(styles) == 8
for layer, key in zip(styles, (b"FrFX", b"DrSh", b"IrSh", b"OrGl", b"IrGl", b"SoFi", b"GrFl", b"ebbl")):
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
