"""Headless thumbnail and isolated MIME/icon registration checks; no user settings."""
import configparser
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

root = Path(__file__).resolve().parents[1]
codec, helper = map(Path, sys.argv[1:3])


def run(args, env, expected=0):
    result = subprocess.run(list(map(str, args)), env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == expected, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout.strip()


with tempfile.TemporaryDirectory(prefix='vulkana-thumbnails-') as directory:
    work = Path(directory)
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', XDG_DATA_HOME=str(work / 'data'),
               XDG_CONFIG_HOME=str(work / 'config'), XDG_CACHE_HOME=str(work / 'cache'))
    old = work / 'Old project Ω.vulkana'
    with zipfile.ZipFile(old, 'w') as archive:
        archive.writestr('manifest.json', (root / 'tests/fixtures/projects/v1-manifest.json').read_bytes())
        archive.writestr('rasters/17.rgba', bytes.fromhex((root / 'tests/fixtures/projects/v1-raster.hex').read_text()))
    project = work / 'Saved project Ω.vulkana'
    run([codec, '--resave', old, project], env)
    before = hashlib.sha256(project.read_bytes()).digest()
    output = work / 'Preview image Ω.png'
    # No display/platform plugin may be required by the small extractor.
    headless = dict(env, QT_QPA_PLATFORM='nonexistent', DISPLAY='', WAYLAND_DISPLAY='')
    descriptor = configparser.ConfigParser(interpolation=None)
    descriptor.read(root / 'res/linux/vulkana.thumbnailer')
    entry = descriptor['Thumbnailer Entry']
    assert entry['MimeType'] == 'application/x-vulkana;'
    args = shlex.split(entry['Exec'])
    assert args[0] == helper.name == entry['TryExec']
    replace = {'%i': str(project), '%o': str(output), '%s': '128'}
    run([helper, *[replace.get(arg, arg) for arg in args[1:]]], headless)
    png = output.read_bytes()
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    width, height = struct.unpack('>II', png[16:24])
    assert 0 < width <= 128 and 0 < height <= 128
    # Dolphin preallocates a temporary output and retains its open descriptor.
    # Fedora's installed thumbnailer domain may write, but not rename over it.
    # Assert in-place publication even on systems without SELinux enforcement.
    preallocated = work / 'Desktop temporary output.png'
    with preallocated.open('w+b') as desktop_file:
        identity = os.fstat(desktop_file.fileno()).st_ino
        run([helper, '--size', '128', project, preallocated], headless)
        assert preallocated.stat().st_ino == identity
        assert desktop_file.read() == png
    # Reject aliases and nonregular files without altering their contents.
    empty = work / 'Do not modify'
    empty.touch()
    hardlink = work / 'Linked temporary output.png'
    os.link(empty, hardlink)
    run([helper, project, hardlink], headless, expected=1)
    assert empty.read_bytes() == b'' and hardlink.stat().st_ino == empty.stat().st_ino
    symlink = work / 'Symlink temporary output.png'
    symlink.symlink_to(empty)
    run([helper, project, symlink], headless, expected=1)
    assert symlink.is_symlink() and empty.read_bytes() == b''
    fifo = work / 'Not a regular output.png'
    os.mkfifo(fifo)
    run([helper, project, fifo], headless, expected=1)
    # An invalid input must leave a caller-provided temporary file untouched.
    untouched = work / 'Unused temporary output.png'
    untouched.touch()
    run([helper, old, untouched], headless, expected=1)
    assert untouched.read_bytes() == b''
    # Replacing a standalone previous thumbnail remains supported.
    run([helper, '--size', '128', project, output], headless)
    assert output.read_bytes() == png
    run([helper, old, work / 'missing.png'], headless, expected=1)
    assert not (work / 'missing.png').exists()
    run([helper, project, project], headless, expected=2)
    assert hashlib.sha256(project.read_bytes()).digest() == before

    # Use the installed theme's lookup rules with a disposable MIME/icon prefix.
    mime = work / 'data/mime/packages'
    mime.mkdir(parents=True)
    xml = root / 'res/linux/vulkana-editor.xml'
    shutil.copyfile(xml, mime / xml.name)
    icon_name = ET.parse(xml).find('.//{*}icon').attrib['name']
    assert icon_name == 'vulkana-project'  # Avoid application-x-generic fallback.
    icon = work / f'data/icons/hicolor/512x512/mimetypes/{icon_name}.png'
    icon.parent.mkdir(parents=True)
    shutil.copyfile(root / 'assets/Vulkana512.png', icon)
    if shutil.which('update-mime-database'):
        run(['update-mime-database', work / 'data/mime'], env)
    if shutil.which('kiconfinder6'):
        (work / 'config').mkdir(exist_ok=True)
        (work / 'config/kdeglobals').write_text('[Icons]\nTheme=Papirus-Dark\n')
        found = Path(run(['kiconfinder6', icon_name], env))
        assert found == icon, found
        print('KDE resolves the project icon instead of the generic document icon.')
    else:
        print('KDE icon lookup unavailable; MIME and helper checks passed.')
print('Headless extraction, desktop temporary outputs, old-file fallback, Unicode paths and source preservation passed.')
