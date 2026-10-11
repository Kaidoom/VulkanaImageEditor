#!/usr/bin/env python3
"""Authoritative English TS -> validated Qt ID catalogs. Build-time only.

English <source> is the only editable English list. Translation TS <source>
elements are synchronized context, not another source of truth.
"""
import argparse
import collections
import json
import pathlib
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'localization'
TOKENS = re.compile(r'%L?(?:[1-9][0-9]?|n)|\{\{[A-Za-z0-9_]+\}\}')

def languages():
    values = json.loads((CATALOG / 'languages.json').read_text())
    ids = [v['id'] for v in values]
    if len(ids) != len(set(ids)) or 'en' not in ids:
        raise ValueError('Language metadata needs unique IDs including en')
    for v in values:
        if not re.fullmatch(r'[a-z]{2,3}(?:[-_][A-Za-z]{2,4})?', v['id']) or not v.get('nativeName'):
            raise ValueError('Invalid language metadata')
        if not isinstance(v.get('enabled', True), bool) or not isinstance(v.get('pluralForms'), int) or v['pluralForms'] < 1:
            raise ValueError('Invalid language availability/plural metadata')
        if not re.fullmatch(r'[a-z]{2,3}(?:_[A-Za-z]{2,4})?', v.get('qtCatalog', v['id'].replace('-', '_'))):
            raise ValueError('Invalid Qt catalog name')
    return values

def uncovered():
    # Conservative CI guard: directly authored widget/menu properties must
    # enter the catalog. Inventory also records remaining migration debt.
    result = []
    pattern = re.compile(r'\b(?:tr|setText|setToolTip|setWindowTitle|setAccessibleName|addAction|addMenu)\(\s*(?:QStringLiteral\()?"([A-Za-z][^"\n]*)"')
    for file in sorted((ROOT / 'src/ui').rglob('*.cpp')):
        if file.stem == 'Localization': continue
        for match in pattern.finditer(file.read_text()):
            result.append(str(file.relative_to(ROOT)) + ': ' + match[1])
    return sorted(set(result))

def read(path):
    tree = ET.parse(path)
    entries = {}
    for context in tree.getroot().findall('context'):
        for m in context.findall('message'):
            key = m.get('id', '')
            if not re.fullmatch(r'[a-z][a-z0-9]*(?:[._-][a-z0-9]+)+', key):
                raise ValueError(f'{path.name}: invalid semantic ID {key!r}')
            if key in entries:
                raise ValueError(f'{path.name}: duplicate {key}')
            entries[key] = (context.findtext('name'), m)
    return tree, entries

def write(tree, path):
    ET.indent(tree, space='  ')
    tree.write(path, encoding='utf-8', xml_declaration=True)

def write_runtime(tree, path):
    # ID entries have no runtime context. Qt 6.11 no longer converts contextual
    # entries with -idbased; emit the canonical context-free TS representation.
    # Authoring contexts remain in checked-in TS for older Linguist versions.
    root = tree.getroot()
    messages = []
    for context in list(root.findall('context')):
        for message in context.findall('message'):
            message.set('label', context.findtext('name', ''))
            messages.append(message)
        root.remove(context)
    context = ET.SubElement(root, 'context')
    ET.SubElement(context, 'name').text = ''
    context.extend(messages)
    write(tree, path)

def sync(language):
    source, english = read(CATALOG / 'en.ts')
    path = CATALOG / f'{language}.ts'
    existing = read(path)[1] if path.exists() else {}
    root = ET.Element('TS', version='2.1', language=language, sourcelanguage='en')
    contexts = {}
    for key, (context, original) in english.items():
        if context not in contexts:
            contexts[context] = ET.SubElement(root, 'context')
            ET.SubElement(contexts[context], 'name').text = context
        m = ET.SubElement(contexts[context], 'message', original.attrib)
        current = original.findtext('source', '')
        ET.SubElement(m, 'source').text = current
        note = original.findtext('extracomment')
        if note: ET.SubElement(m, 'extracomment').text = note
        old = existing.get(key, (None, None))[1]
        if old is not None and old.find('translation') is not None:
            translation = old.find('translation')
            if old.findtext('source') != current:
                translation.set('type', 'unfinished')
                ET.SubElement(m, 'oldsource').text = old.findtext('source')
            elif old.find('oldsource') is not None:
                m.append(old.find('oldsource'))
            m.append(translation)
            for tag in ('translatorcomment',):
                if old.find(tag) is not None: m.append(old.find(tag))
        else:
            translation = ET.SubElement(m, 'translation', type='unfinished')
            if m.get('numerus') == 'yes':
                count = next(v['pluralForms'] for v in languages() if v['id'] == language)
                for _ in range(count): ET.SubElement(translation, 'numerusform')
    # Retired IDs retain contributor work but never enter runtime packs.
    retired = [m for key, (_, m) in existing.items() if key not in english]
    if retired:
        c = ET.SubElement(root, 'context'); ET.SubElement(c, 'name').text = 'Retired'
        for m in retired:
            m.find('translation').set('type', 'vanished'); c.append(m)
    write(ET.ElementTree(root), path)

def validate():
    _, english = read(CATALOG / 'en.ts')
    errors = []
    metadata = {v['id']: v for v in languages()}
    for key, (_, message) in english.items():
        if not message.findtext('source', '').strip(): errors.append(f'en.ts:{key}: empty source')
        if message.get('numerus') == 'yes':
            forms = message.findall('translation/numerusform')
            if len(forms) != metadata['en']['pluralForms']:
                errors.append(f'en.ts:{key}: invalid English plural forms')
            for form in forms:
                if collections.Counter(TOKENS.findall(message.findtext('source', ''))) != collections.Counter(TOKENS.findall(''.join(form.itertext()))):
                    errors.append(f'en.ts:{key}: plural placeholder mismatch')
    for file in CATALOG.glob('*.ts'):
        tree, entries = read(file)
        if file.stem == 'en': continue
        translated = 0
        for key, (_, m) in entries.items():
            t = m.find('translation')
            if t is None or t.get('type') in ('unfinished', 'vanished', 'obsolete'): continue
            if key not in english:
                errors.append(f'{file.name}: unknown ID {key}'); continue
            if m.get('numerus') != english[key][1].get('numerus'):
                errors.append(f'{file.name}:{key}: plural declaration mismatch')
            src = english[key][1].findtext('source', '')
            if m.findtext('source') != src: errors.append(f'{file.name}:{key}: stale source; run sync')
            forms = t.findall('numerusform') if m.get('numerus') == 'yes' else [t]
            expected = metadata.get(file.stem, {}).get('pluralForms')
            if m.get('numerus') == 'yes' and expected and len(forms) != expected:
                errors.append(f'{file.name}:{key}: expected {expected} plural forms')
            for form in forms:
                value = ''.join(form.itertext())
                if not value: errors.append(f'{file.name}:{key}: empty translation')
                if collections.Counter(TOKENS.findall(src)) != collections.Counter(TOKENS.findall(value)):
                    errors.append(f'{file.name}:{key}: placeholder mismatch')
                if re.findall(r'</?[A-Za-z][^>]*>', src) != re.findall(r'</?[A-Za-z][^>]*>', value):
                    errors.append(f'{file.name}:{key}: markup mismatch')
                # Ampersands in prose are not accelerators; authored menu IDs are.
                if src.startswith('&') and len(re.findall(r'(?<!&)&(?!&)', src)) != len(re.findall(r'(?<!&)&(?!&)', value)):
                    errors.append(f'{file.name}:{key}: accelerator mismatch')
                if re.findall(r'\*\.[A-Za-z0-9]+', src) != re.findall(r'\*\.[A-Za-z0-9]+', value):
                    errors.append(f'{file.name}:{key}: file extension pattern changed')
            translated += 1
        for key in english.keys() - entries.keys(): errors.append(f'{file.name}: missing ID {key}; run sync')
        if metadata.get(file.stem, {}).get('enabled', True) and translated == 0:
            errors.append(f'{file.name}: an empty language pack must not be enabled')
        print(f'{file.stem}: {translated}/{len(english)} translated; remaining entries fall back to English')
    for file in (ROOT / 'src').rglob('*'):
        if file.suffix not in ('.cpp', '.hpp'): continue
        for key in re.findall(r'i18n::text\("([^"]+)"', file.read_text()):
            if key not in english: errors.append(f'{file.relative_to(ROOT)}: missing {key}')
        if re.search(r'setObjectName\(\s*i18n::text', file.read_text()):
            errors.append(f'{file.relative_to(ROOT)}: object identity must not be translated')
    baseline = CATALOG / 'untranslated-baseline.json'
    if baseline.exists():
        for item in set(uncovered()) - set(json.loads(baseline.read_text())):
            errors.append('New untranslated widget text: ' + item)
    if errors: raise ValueError('\n'.join(errors))
    return english

def compile_packs(output, lrelease):
    validate()
    output.mkdir(parents=True, exist_ok=True)
    files = []
    # English runtime text is derived, never edited or checked in twice.
    tree, entries = read(CATALOG / 'en.ts')
    # Compatibility adapter for existing Qt-free command labels. Generated only;
    # never use this lookup for user-authored document data.
    history = {m.findtext('source'): key for key, (_, m) in entries.items()}
    (output / 'history-ids.json').write_text(json.dumps(history, ensure_ascii=False))
    for _, m in entries.values():
        t = m.find('translation')
        if m.get('numerus') == 'yes':
            if t is None: raise ValueError(f"English plural missing: {m.get('id')}")
            continue
        if t is not None: m.remove(t)
        t = ET.SubElement(m, 'translation')
        t.text = m.findtext('source')
    english = output / 'en.ts'; write_runtime(tree, english); files.append(english)
    for p in CATALOG.glob('*.ts'):
        if p.stem == 'en': continue
        generated = output / p.name
        write_runtime(read(p)[0], generated); files.append(generated)
    for path in files:
        subprocess.run([lrelease, '-idbased', '-nounfinished', str(path), '-qm', str(output / (path.stem + '.qm'))], check=True)
    # Expanded pseudo pack preserves placeholders, identifiers and markup.
    tree, entries = read(CATALOG / 'en.ts'); tree.getroot().set('language', 'en')
    for _, m in entries.values():
        if m.find('translation') is not None: m.remove(m.find('translation'))
        t = ET.SubElement(m, 'translation')
        text = m.findtext('source', '')
        expanded = '[!! ' + text + ' ' + '·' * max(4, len(text) // 3) + ' !!]'
        if m.get('numerus') == 'yes':
            for _ in range(2): ET.SubElement(t, 'numerusform').text = expanded
        else: t.text = expanded
    pseudo = output / 'qps.ts'; write_runtime(tree, pseudo)
    subprocess.run([lrelease, '-idbased', str(pseudo), '-qm', str(output / 'qps.qm')], check=True)
    shutil.copyfile(CATALOG / 'languages.json', output / 'languages.json')

def main():
    global CATALOG
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['validate', 'compile', 'sync', 'inventory'])
    parser.add_argument('--language')
    parser.add_argument('--catalog-dir', type=pathlib.Path, default=CATALOG, help='Trusted authoring catalogs, including languages.json')
    parser.add_argument('--write-baseline', action='store_true', help='Explicitly record migration debt; review changes')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'build/localization')
    parser.add_argument('--lrelease', default=shutil.which('lrelease-qt6') or shutil.which('lrelease') or '/usr/lib64/qt6/bin/lrelease')
    args = parser.parse_args()
    CATALOG = args.catalog_dir.resolve()
    if args.command == 'sync':
        choices = [x['id'] for x in languages() if x['id'] != 'en']
        for lang in [args.language] if args.language else choices:
            if lang not in choices: raise ValueError('Add language metadata before synchronizing a new catalog')
            sync(lang)
    elif args.command == 'compile': compile_packs(args.output, args.lrelease)
    elif args.command == 'inventory':
        if args.write_baseline:
            (CATALOG / 'untranslated-baseline.json').write_text(json.dumps(uncovered(), ensure_ascii=False, indent=2) + '\n')
        validate()
        for file in sorted((ROOT / 'src').rglob('*.cpp')):
            if 'third_party' in file.parts: continue
            for n, line in enumerate(file.read_text().splitlines(), 1):
                if re.search(r'\btr\("|(?:setText|setToolTip|setWindowTitle|addAction|addMenu)\(\s*(?:QStringLiteral\()?"[A-Za-z]', line):
                    print(f'{file.relative_to(ROOT)}:{n}: {line.strip()}')
    else: validate()

if __name__ == '__main__':
    try: main()
    except (ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr); sys.exit(1)
