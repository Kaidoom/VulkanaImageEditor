"""Catalog workflow/integrity tests; no shipping Python dependency."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
sys.dont_write_bytecode = True

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('localization', root / 'tools/localization.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
executable, lrelease = map(Path, sys.argv[1:3])

def rejects(action, reason=None):
    try: action()
    except ValueError as error:
        if reason: assert reason in str(error), str(error)
        return
    raise AssertionError('Invalid catalog accepted')

with tempfile.TemporaryDirectory(prefix='vulkana-catalog-test-') as work:
    work = Path(work)
    module.CATALOG = work / 'catalog'
    shutil.copytree(root / 'localization', module.CATALOG)
    metadata = module.languages()
    assert [v['id'] for v in metadata] == ['en', 'fr', 'es', 'de', 'it', 'pt-BR', 'pl', 'tr',
        'ru', 'uk', 'ar', 'hi', 'zh-Hans', 'zh-Hant', 'ja', 'ko']
    assert all(v['enabled'] for v in metadata)
    source_ids = set(module.read(module.CATALOG / 'en.ts')[1])
    for value in metadata:
        if value['id'] == 'en': continue
        _, entries = module.read(module.CATALOG / (value['id'] + '.ts'))
        assert source_ids <= entries.keys()
        for key, (_, message) in entries.items():
            if key not in source_ids:
                # Retired entries preserve earlier contributor work, not UI coverage.
                assert message.find('translation').get('type') in ('vanished', 'obsolete')
                continue
            assert message.find('translation').get('type') not in ('unfinished', 'vanished', 'obsolete')
            assert ''.join(message.find('translation').itertext()).strip()
            if message.get('numerus') == 'yes':
                assert len(message.findall('translation/numerusform')) == value['pluralForms']
    metadata_path = module.CATALOG / 'languages.json'
    original_metadata = metadata_path.read_text()
    # Advertising a newly scaffolded/emptied pack must still fail.
    es = module.CATALOG / 'es.ts'
    tree, entries = module.read(es)
    for _, message in entries.values(): message.find('translation').set('type', 'unfinished')
    module.write(tree, es); rejects(module.validate, 'empty language pack')
    shutil.copyfile(root / 'localization/es.ts', es)
    metadata[2]['qtCatalog'] = '../es'
    metadata_path.write_text(json.dumps(metadata)); rejects(module.languages, 'Invalid Qt catalog name')
    metadata_path.write_text(original_metadata)
    en = module.CATALOG / 'en.ts'; fr = module.CATALOG / 'fr.ts'
    before = hashlib.sha256(executable.read_bytes()).digest()
    tree, english = module.read(en)
    key = 'language.label'
    english[key][1].find('source').text = 'Interface language (local pack)'
    module.write(tree, en)
    original_fr = module.read(fr)[1][key][1].findtext('translation')
    for language in metadata:
        if language['id'] != 'en': module.sync(language['id'])
    message = module.read(fr)[1][key][1]
    assert message.findtext('translation') == original_fr
    assert message.find('translation').get('type') == 'unfinished'
    assert message.findtext('oldsource') == 'Language'
    module.sync('fr')
    assert module.read(fr)[1][key][1].findtext('oldsource') == 'Language'
    output = work / 'packs'; module.compile_packs(output, str(lrelease))
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    subprocess.run([str(executable), '--probe-pack', str(output), 'Interface language (local pack)'], env=env, check=True)
    (output / 'ar.qm').write_bytes(b'broken optional catalog')
    subprocess.run([str(executable), '--probe-pack', str(output), 'Interface language (local pack)'], env=env, check=True)
    empty = work / 'empty'; empty.mkdir()
    subprocess.run([str(executable), '--probe-pack', str(empty), 'Language'], env=env, check=True)
    assert hashlib.sha256(executable.read_bytes()).digest() == before
    # Duplicate keys cannot silently overwrite contributor work.
    tree, entries = module.read(en)
    tree.getroot().find('context').append(entries[key][1])
    module.write(tree, en); rejects(lambda: module.read(en))
    for lang in [v['id'] for v in metadata]:
        shutil.copyfile(root / f'localization/{lang}.ts', module.CATALOG / f'{lang}.ts')
    module.validate() # Each negative test starts from a genuinely valid catalog.
    # Placeholder, plural and accelerator damage fail for the intended reason.
    for key, value, reason in [('language.layers.count', '%1', 'plural forms'),
            ('main.window.file', 'Fichier', 'accelerator mismatch'),
            ('main.window.files.replace.1', 'Remplacer', 'placeholder mismatch')]:
        tree, entries = module.read(fr)
        msg = entries[key][1]; msg.remove(msg.find('translation'))
        ET.SubElement(msg, 'translation').text = value
        module.write(tree, fr); rejects(module.validate, reason)
        shutil.copyfile(root / 'localization/fr.ts', fr)
    for lang in ['en', 'fr', 'ar']:
        path = module.CATALOG / f'{lang}.ts'
        tree, _ = module.read(path)
        msg = ET.SubElement(tree.getroot().find('context'), 'message', id='language.test.markup')
        ET.SubElement(msg, 'source').text = '<b>%1</b>'
        if lang != 'en': ET.SubElement(msg, 'translation').text = '<b>%1</b>'
        module.write(tree, path)
    for language in module.languages():
        if language['id'] != 'en': module.sync(language['id'])
    module.validate()
    tree, entries = module.read(fr)
    entries['language.test.markup'][1].find('translation').text = '<i>%1</i>'
    module.write(tree, fr); rejects(module.validate, 'markup mismatch')
print('Catalog workflow and executable-independent pack reload passed')
