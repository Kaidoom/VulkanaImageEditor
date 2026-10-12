#!/usr/bin/env python3
"""Prepare offline Windows notices from verified sources and SDK inventories.

No network access or executable code from input archives. Run after Build.ps1.
The output is an input to Portable.ps1 -NoticesDirectory, not a release approval.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile

HERE = Path(__file__).resolve().parent


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def source_notices(path, prefix):
    """Conservative reference appendix; not a claim every source component ships."""
    parts = []
    # PDFium's embedded AGG permission is in a header, not a LICENSE file.
    agg = 'src/3rdparty/chromium/third_party/pdfium/third_party/agg23/agg_basics.h'
    with tarfile.open(path, 'r|xz') as archive:
        for member in archive:
            p = PurePosixPath(member.name)
            if (p.is_absolute() or '..' in p.parts
                    or (member.name != prefix and not member.name.startswith(prefix + '/'))):
                raise ValueError('Unsafe source member: ' + member.name)
            name = p.name.upper()
            legal = (re.match(r'^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE|OFL|FTL)([._-]|$)', name)
                     or 'LICENSES' in p.parts or name == 'QT_ATTRIBUTION.JSON'
                     or name in ('README.IJG', 'README.CHROMIUM', 'README.PDFIUM')
                     or member.name == prefix + '/' + agg)
            if not member.isfile() or not legal:
                continue
            if member.size > 4 * 1024 * 1024:
                raise ValueError('Oversized legal text: ' + member.name)
            data = archive.extractfile(member).read()
            if b'\0' in data:
                continue
            text = data.decode('utf-8', errors='replace')
            if member.name.endswith('/' + agg):
                text = text.split('#ifndef', 1)[0]
            parts.append((member.name, text))
    if not parts:
        raise ValueError('No legal texts in ' + path.name)
    return '\n'.join('\n--- ' + name + ' ---\n' + text for name, text in sorted(parts))


def prepare(args):
    lock = json.loads((HERE / 'dependencies/qt-sources.json').read_text())
    versions = json.loads((HERE / 'dependencies/versions.json').read_text())
    if lock['version'] != versions['qtVersion']:
        raise ValueError('Qt notice source lock does not match the runtime lock')
    manifest = json.loads((args.base / 'notices.json').read_text(encoding='utf-8'))
    manifest.update(profile='windowsportable', source_url='https://vulkana.app/licensing/',
                    source_url_published=False)
    # The source-build inventory's Qt translations refer to another platform.
    manifest['components'] = [c for c in manifest['components']
                              if not c['notice']['name'].startswith(('Qt', 'GCC runtime'))]
    manifest['texts'] = {k: v for k, v in manifest['texts'].items()
                         if not k.startswith('qttranslations-')}
    for item in manifest['components']:
        notice = item['notice']
        notice['scope'] = notice['scope'].replace('System-provided', 'Bundled Windows dependency')
        if notice['name'] == 'Vulkan headers and loader':
            notice['scope'] = 'Headers compiled into the application; Vulkan loader/driver supplied by the graphics driver, not bundled'
    records = []
    for module, expected in lock['archives'].items():
        prefix = f"{module}-everywhere-src-{lock['version']}"
        source = args.sources / (prefix + '.tar.xz')
        if digest(source) != expected:
            raise ValueError('Source checksum mismatch: ' + source.name)
        records.append(dict(name=source.name, sha256=expected, bytes=source.stat().st_size,
                            url=lock['baseUrl'] + source.name, delivery='upstream'))
        key = module + '-source-notices.txt'
        manifest['texts'][key] = source_notices(source, prefix)
        manifest['components'].append(dict(notice=dict(
            name='Qt PDF / PDFium source notice collection' if module == 'qtwebengine' else module,
            version=lock['version'],
            scope=('Bundled Qt shared libraries/plugins/translations. Original source notice appendix '
                   'also covers optional components not linked into Vulkana. Qt PDF uses PDFium; '
                   'the WebEngine browser runtime is not included.'),
            license='LGPL-3.0-only for Qt libraries; BSD-3-Clause for translations; third-party terms below',
            copyright='The Qt Company Ltd. and contributors; original third-party holders retained below'),
            texts=['LGPL-3.0.txt', 'GPL-3.0.txt', key]))
        print('Collected', module, flush=True)
    # Include the actual SDK's target/third-party attribution records, not a
    # different distribution's metadata. The source appendix complements PDF's
    # incomplete embedded-dependency inventory.
    for module in ('qtbase', 'qtimageformats', 'qtpdf', 'qtsvg', 'qttranslations'):
        path = args.qt / 'sbom' / f"{module}-{lock['version']}.spdx.json"
        raw = path.read_text(encoding='utf-8')
        sbom = json.loads(raw)
        if not any(p.get('versionInfo') == lock['version'] for p in sbom['packages']):
            raise ValueError('Unexpected SDK inventory version: ' + path.name)
        manifest['texts'][path.name] = raw
        manifest['components'].append(dict(notice=dict(name=module + ' SDK inventory', version=lock['version'],
            scope='Upstream build provenance and third-party attribution metadata',
            license='See individual package licenseConcluded fields and source notices',
            copyright='See individual package copyrightText fields'), texts=[path.name]))
    for module in ('qpdf', 'libwebp', 'libjpeg-turbo', 'libspng', 'minizip-ng', 'zlib'):
        directory = args.vcpkg / 'share' / module
        # A prepared evidence directory may consist of share's contents directly.
        if not directory.exists():
            directory = args.vcpkg / module
        text = (directory / 'copyright').read_text(encoding='utf-8')
        key = module + '-windows-copyright.txt'
        manifest['texts'][key] = text
        sbom = json.loads((directory / 'vcpkg.spdx.json').read_text(encoding='utf-8'))
        package = next(p for p in sbom['packages'] if p['name'] == module)
        manifest['components'].append(dict(notice=dict(name=module + ' Windows package',
            version=package['versionInfo'], scope='Redistributed vcpkg library / compiled code',
            license=package.get('licenseConcluded', 'See preserved copyright file'),
            copyright='See original copyright file'), texts=[key]))
    manifest['acknowledgements'] += [
        'Qt shared libraries are used under LGPL v3. Libraries remain replaceable; see LIBRARY-REPLACEMENT.txt.',
        'The Qt source appendix preserves additional upstream notices conservatively; it is not a list of installed features.',
        'Microsoft Visual C++ runtime files are redistributed from the Visual Studio REDIST directory. '
        'See share/licenses/vulkana-editor/Microsoft-Redist.txt. Windows system libraries and graphics drivers are not bundled.']
    manifest['notice_text_keys'] = sorted(manifest['texts'])
    for component in manifest['components']:
        for key in component['texts']:
            if key not in manifest['texts']:
                raise ValueError('Missing notice: ' + key)
    encoded_manifest = json.dumps(manifest, ensure_ascii=False, indent=2) + '\n'
    if len(encoded_manifest.encode('utf-8')) > 16 * 1024 * 1024:
        raise ValueError('Notice catalog exceeds the application reader limit')
    args.output.mkdir(parents=True, exist_ok=False)
    def write(name, text):
        (args.output / name).write_text(text, encoding='utf-8', newline='\n')
    write('notices.json', encoded_manifest)
    lines = ['Vulkana ' + manifest['version'] + ' - Windows portable third-party notices',
             *manifest['acknowledgements']]
    for component in manifest['components']:
        n = component['notice']
        lines += ['', n['name'] + ' ' + n['version'], n['scope'], n['license'], n['copyright'],
                  'Texts: ' + ', '.join(component['texts'])]
    for key, value in sorted(manifest['texts'].items()):
        lines += ['', '--- ' + key + ' ---', value]
    write('THIRD-PARTY-NOTICES.txt', '\n'.join(lines) + '\n')
    write('SOURCES.json', json.dumps(dict(schema=1, archives=records), indent=2) + '\n')
    write('SOURCE-SHA256SUMS', ''.join(f"{r['sha256']}  {r['name']}\n" for r in records))
    write('LIBRARY-REPLACEMENT.txt', (HERE / 'LIBRARY-REPLACEMENT.txt').read_text(encoding='utf-8'))
    print('Prepared offline notices:', args.output)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True, help='Verified Qt source .tar.xz archives')
    parser.add_argument('--base', type=Path, required=True, help='Release build/notices directory')
    parser.add_argument('--qt', type=Path, required=True, help='Qt SDK root containing sbom/')
    parser.add_argument('--vcpkg', type=Path, required=True, help='Installed target prefix or copied share directory')
    parser.add_argument('--output', type=Path, required=True, help='New output directory')
    prepare(parser.parse_args())
