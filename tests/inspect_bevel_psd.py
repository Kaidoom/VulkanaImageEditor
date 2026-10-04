"""Optional independent writer check; psd-tools is development-only.

Run with the directory produced by imageeditor_bevel_effects_tests --psd.
The variants intentionally keep the original compatibility composite unchanged:
an independent editor must redraw the actual live effect to distinguish them.
"""
import sys
import math
from pathlib import Path
from psd_tools import PSDImage

directory = Path(sys.argv[1])
psd = PSDImage.open(directory / "bevel.psd")
effect = list(psd[0].effects)[0]
d = effect.descriptor
assert d[b"bvlT"].enum == b"SfBL"
assert d[b"bvlS"].enum == b"InrB"
assert float(d[b"blur"].value) == 6
assert float(d[b"srgR"].value) == 100
surface = [(float(p[b"Hrzn"].value), float(p[b"Vrtc"].value)) for p in d[b"MpgS"][b"Crv "]]
gloss = [(float(p[b"Hrzn"].value), float(p[b"Vrtc"].value)) for p in d[b"TrnS"][b"Crv "]]
assert surface != gloss and len(surface) == len(gloss) == 5
assert surface[1] == (63.75, 20.400000000000002)
assert gloss[1] == (63.75, 204.0)
psd.topil().save(directory / "native-composite.png")
d[b"lagl"].value = -45.0
psd.save(directory / "bevel-relit.psd")
d[b"enab"].value = False
psd.save(directory / "bevel-disabled.psd")
for name in ("bevel-relit.psd", "bevel-disabled.psd"):
    reopened = PSDImage.open(directory / name)
    assert reopened.topil().tobytes() == psd.topil().tobytes()
for name, kind in (("bevel-text.psd", "type"), ("bevel-shape.psd", "shape")):
    typed = PSDImage.open(directory / name)[0]
    assert typed.kind == kind
    assert math.isclose(float(list(typed.effects)[0].descriptor[b"blur"].value), 7.2)
print("Independent bevel, distinct curves, typed records, and unchanged-composite variants verified.")
