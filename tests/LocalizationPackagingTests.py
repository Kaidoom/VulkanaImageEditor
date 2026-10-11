"""Exercise the actual translation install rules without creating any packages."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
cmake, lrelease, qmake = sys.argv[1:4]


def run(*args, succeeds=True):
    result = subprocess.run(list(map(str, args)), text=True, capture_output=True)
    if (result.returncode == 0) != succeeds:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout + result.stderr


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


with tempfile.TemporaryDirectory(prefix='vulkana-language-staging-') as directory:
    work = Path(directory)
    source = work / 'source'
    build = work / 'build'
    stage = work / 'installed'
    shutil.copytree(root / 'localization', source / 'localization')
    for name in ('cmake/Localization.cmake', 'cmake/CompileLanguagePacks.cmake.in',
                 'tools/localization.py'):
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / name, target)
    (source / 'CMakeLists.txt').write_text('''
cmake_minimum_required(VERSION 3.24)
project(LanguageStaging NONE)
add_executable(Qt6::qmake IMPORTED)
set_target_properties(Qt6::qmake PROPERTIES IMPORTED_LOCATION "${TEST_QMAKE}")
set(VULKANA_DATA_DIR share/vulkana-editor)
include(cmake/Localization.cmake)
''')
    run(cmake, '-S', source, '-B', build, f'-DVULKANA_LRELEASE={lrelease}',
        f'-DTEST_QMAKE={qmake}', f'-DPython3_EXECUTABLE={sys.executable}')
    packs = stage / 'share/vulkana-editor/translations'
    # No build first: both a fresh stage and a direct restage must compile packs.
    assert not (build / 'translations/fr.qm').exists()
    run(cmake, '--install', build, '--prefix', stage)
    languages = json.loads((source / 'localization/languages.json').read_text())
    for language in languages:
        if language['enabled']:
            name = language['id'] + '.qm'
            assert (packs / name).read_bytes() == (build / 'translations' / name).read_bytes()
    before = digest(packs / 'fr.qm')
    french = source / 'localization/fr.ts'
    tree = ET.parse(french)
    tree.find('.//message[@id="about.dialog.check.for.updates"]/translation').text = 'Vérifier les mises à jour (test du catalogue modifié)'
    tree.write(french, encoding='utf-8', xml_declaration=True)
    # Like each package run, use a fresh destination but reuse the stale build
    # outputs. Do not depend on filesystem timestamp granularity during restage.
    stage = work / 'edited-install'
    packs = stage / 'share/vulkana-editor/translations'
    output = run(cmake, '--install', build, '--prefix', stage)
    assert digest(build / 'translations/fr.qm') != before, output
    assert digest(packs / 'fr.qm') != before, output
    assert (packs / 'fr.qm').read_bytes() == (build / 'translations/fr.qm').read_bytes()
    good = digest(packs / 'fr.qm')
    # Invalid edits must fail, not reuse the valid older QM in the build folder.
    tree.find('.//message[@id="about.dialog.you.re.running.version.1"]/translation').text = 'Version'
    tree.write(french, encoding='utf-8', xml_declaration=True)
    output = run(cmake, '--install', build, '--prefix', stage, succeeds=False)
    assert 'placeholder mismatch' in output and 'package staging stopped' in output
    assert digest(packs / 'fr.qm') == good
    fresh = work / 'failed-install'
    run(cmake, '--install', build, '--prefix', fresh, succeeds=False)
    assert not list(fresh.rglob('*.qm'))
    # A missing/broken compiler must fail just as explicitly.
    shutil.copyfile(root / 'localization/fr.ts', french)
    run(cmake, '-S', source, '-B', build, f'-DVULKANA_LRELEASE={work / "missing-lrelease"}')
    output = run(cmake, '--install', build, '--prefix', fresh, succeeds=False)
    assert 'package staging stopped' in output
    assert not list(fresh.rglob('*.qm'))

print('Language staging: fresh compile, edited catalog, invalid catalog and missing compiler passed')
