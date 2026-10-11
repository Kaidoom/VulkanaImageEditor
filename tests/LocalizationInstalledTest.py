import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

probe, translations, qt_translations = map(Path, sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix='vulkana-installed-languages-') as directory:
    stage = Path(directory)
    binary = stage / 'bin' / probe.name; binary.parent.mkdir()
    shutil.copy2(probe, binary)
    packs = stage / 'share/vulkana-editor/translations'; packs.mkdir(parents=True)
    languages = [v for v in json.loads((translations / 'languages.json').read_text()) if v['enabled']]
    assert len(languages) == 16
    for name in [v['id'] + '.qm' for v in languages] + ['languages.json']:
        shutil.copy2(translations / name, packs)
    for language in languages:
        name = f"qtbase_{language['qtCatalog']}.qm"
        if language['id'] != 'en' and (qt_translations / name).exists():
            shutil.copy2(qt_translations / name, packs)
    subprocess.run([str(binary)], cwd=stage, env=dict(os.environ, QT_QPA_PLATFORM='offscreen'), check=True)
