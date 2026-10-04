"""Optional independent PSD record check; psd-tools is a development dependency.

Generate fixtures with VULKANA_ADJUSTMENT_FIXTURES=/path/to/output and run
imageeditor_adjustment_layer_tests, then pass that directory to this script.
Nothing here is needed by the application or packaged runtime.
"""
import sys
from pathlib import Path

import numpy as np
import psd_tools
from psd_tools import PSDImage
from psd_tools.constants import BlendMode, Tag

root = Path(sys.argv[1])
global_doc = PSDImage.open(root / "global.psd")
scoped_doc = PSDImage.open(root / "scoped.psd")
invert_doc = PSDImage.open(root / "invert.psd")
for doc in (global_doc, scoped_doc, invert_doc):
    assert doc.size == (8, 8) and doc.depth == 8
    assert doc.image_resources.get_data(1039)  # Current sRGB ICC profile.
    for layer in doc.descendants():
        if layer.kind == "exposure":
            assert layer.exposure == 1 and layer.exposure_offset == 0
            assert layer.gamma == 1
            assert Tag.EXPOSURE in layer._record.tagged_blocks
            assert layer.blend_mode == BlendMode.NORMAL
            assert not layer.has_pixels()  # Real operator, not a scene image.
        elif layer.kind == "pixel":
            assert layer.topil() is not None

assert [layer.kind for layer in global_doc] == ["pixel", "pixel", "exposure"]
group = next(layer for layer in scoped_doc if layer.is_group())
assert group.blend_mode == BlendMode.NORMAL
assert [layer.kind for layer in group] == ["pixel", "exposure"]
assert invert_doc[-1].kind == "invert"
assert Tag.INVERT in invert_doc[-1]._record.tagged_blocks

# The independent parser decodes both fresh compatibility composites. Invert
# acts in encoded RGB, preserves coverage, and must agree within one final
# quantization step. No psd-tools approximate Exposure compositor is an oracle.
before = np.asarray(global_doc.topil().convert("RGBA"), dtype=np.int16)
after = np.asarray(invert_doc.topil().convert("RGBA"), dtype=np.int16)
assert np.array_equal(before[..., 3], after[..., 3])
assert np.max(np.abs(after[..., :3] - (255 - before[..., :3]))) <= 1
print(f"psd-tools {psd_tools.__version__}: native Exposure/Invert records, "
      "local domain, pixel channels and fresh composite inversion passed")
