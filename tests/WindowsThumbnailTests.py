"""Native stream/COM checks with generated files; no registry changes."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
writer, probe, provider = map(Path, sys.argv[1:4])


def run(args, expected=0):
    process = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=30)
    assert process.returncode == expected, (args, process.returncode, process.stdout, process.stderr)


with tempfile.TemporaryDirectory(prefix='vulkana-shell-') as folder:
    work = Path(folder)
    old = work / 'Legacy project.vulkana'
    with zipfile.ZipFile(old, 'w') as archive:
        archive.writestr('manifest.json', (root / 'tests/fixtures/projects/v1-manifest.json').read_bytes())
        archive.writestr('rasters/17.rgba', bytes.fromhex((root / 'tests/fixtures/projects/v1-raster.hex').read_text()))
    project = work / 'Saved café Ω.vulkana'
    run([writer, '--resave', old, project])
    original = project.read_bytes()
    run([probe, provider, project])
    run([probe, provider, project])
    assert project.read_bytes() == original
    run([probe, provider, old], expected=1)
    damaged = work / 'Truncated.vulkana'
    damaged.write_bytes(original[:len(original) // 2])
    run([probe, provider, damaged], expected=1)
    stale = work / 'Stale.vulkana'
    with zipfile.ZipFile(project) as source, zipfile.ZipFile(stale, 'w') as archive:
        for name in source.namelist():
            data = source.read(name)
            if name == 'manifest.json':
                manifest = json.loads(data)
                manifest['preview']['contentSha256'] = 'invalid'
                data = json.dumps(manifest).encode()
            archive.writestr(name, data)
    run([probe, provider, stale], expected=1)
print('Native COM thumbnails, Unicode input, repeated extraction and safe fallbacks passed.')
