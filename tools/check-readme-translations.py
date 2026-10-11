#!/usr/bin/env python3
"""Check README navigation, local links and copyable build instructions.

This validates structure and technical tokens, not translation quality.
Uses only the Python standard library; run from any working directory.
"""
from collections import Counter
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parent.parent
START = '<!-- readme-languages:start -->'
END = '<!-- readme-languages:end -->'
LINK = re.compile(r'!?\[([^\]]*)\]\(([^\s)]+)\)')
CODE = re.compile(r'^```sh\n(.*?)^```$', re.M | re.S)
INLINE = re.compile(r'(?<!`)`([^`\n]+)`(?!`)')
VERSION = re.compile(r'\*\*(\d+(?:\.\d+)*(?:\+|\.x))\*\*')


def check():
    languages = [item for item in json.loads((ROOT / 'localization/languages.json').read_text())
                 if item['enabled']]
    source = (ROOT / 'README.md').read_text()
    blocks = CODE.findall(source)
    inline = Counter(INLINE.findall(CODE.sub('', source)))
    versions = Counter(VERSION.findall(source))
    errors = []
    for language in languages:
        code = language['id']
        relative = Path('README.md') if code == 'en' else Path('docs/readme') / f'README.{code}.md'
        path = ROOT / relative
        if not path.is_file():
            errors.append(f'{relative}: missing README')
            continue
        text = path.read_text()
        if text.count(START) != 1 or text.count(END) != 1:
            errors.append(f'{relative}: expected one language-navigation block')
        else:
            navigation = text.split(START, 1)[1].split(END, 1)[0]
            expected = []
            for item in languages:
                target = ('README.md' if code == 'en' else '../../README.md') if item['id'] == 'en' else (
                    ('docs/readme/' if code == 'en' else '') + f'README.{item["id"]}.md')
                expected.append((item['nativeName'], target))
            if LINK.findall(navigation) != expected:
                errors.append(f'{relative}: language names, order or destinations differ from the registry')
        for _, target in LINK.findall(text):
            url = urlsplit(target)
            if url.scheme or url.netloc or not url.path:
                continue
            destination = (path.parent / unquote(url.path)).resolve()
            if not destination.is_relative_to(ROOT) or not destination.is_file():
                errors.append(f'{relative}: broken local link: {target}')
        if CODE.findall(text) != blocks:
            errors.append(f'{relative}: shell examples differ from English')
        tokens = Counter(INLINE.findall(CODE.sub('', text)))
        if tokens != inline:
            errors.append(f'{relative}: inline technical tokens differ: '
                          f'missing {list((inline - tokens).elements())}, extra {list((tokens - inline).elements())}')
        if Counter(VERSION.findall(text)) != versions:
            errors.append(f'{relative}: dependency version requirements differ')
        if len(re.findall(r'^## ', text, re.M)) != len(re.findall(r'^## ', source, re.M)):
            errors.append(f'{relative}: missing or extra major section')
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'PASS: {len(languages)} READMEs; language navigation, local links/images, '
          'shell commands, technical tokens and version requirements match.')
    return 0


if __name__ == '__main__':
    sys.exit(check())
