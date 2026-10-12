"""README authoring checks, including Windows commands and UTF-8 text."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('readme_check', ROOT / 'tools/check-readme-translations.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class ReadmeChecks(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix='vulkana-readme-')
        self.addCleanup(self.workspace.cleanup)
        self.root = Path(self.workspace.name).resolve()
        (self.root / 'localization').mkdir()
        (self.root / 'docs/readme').mkdir(parents=True)
        (self.root / 'tools/windows').mkdir(parents=True)
        (self.root / 'tools/windows/README.md').write_text('# Windows\n', encoding='utf-8')
        (self.root / 'localization/languages.json').write_text(json.dumps([
            {'id': 'en', 'nativeName': 'English', 'enabled': True},
            {'id': 'fr', 'nativeName': 'Français', 'enabled': True},
        ], ensure_ascii=False), encoding='utf-8')
        for language in ('en', 'fr'):
            english = language == 'en'
            navigation = ('[English](README.md) · [Français](docs/readme/README.fr.md)' if english else
                          '[English](../../README.md) · [Français](README.fr.md)')
            guide = 'tools/windows/README.md' if english else '../../tools/windows/README.md'
            heading = 'Build' if english else 'Compilation'
            content = (f'# Vulkana\n\n{checker.START}\n{navigation}\n{checker.END}\n\n'
                       f'## {heading}\n\nCMake **3.28+**\n\n### Linux\n\n'
                       '```sh\ncmake --preset release\n```\n\n### Windows\n\n'
                       f'[Windows]({guide})\n\n'
                       '```powershell\npwsh -File tools/windows/Build.ps1 -BuildRoot C:\\Dev\\VulkanaClean -Tests\n```\n')
            path = self.root / ('README.md' if english else 'docs/readme/README.fr.md')
            path.write_text(content, encoding='utf-8')
        self.translation = self.root / 'docs/readme/README.fr.md'

    def run_check(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = checker.check(self.root)
        return result, output.getvalue()

    def change(self, old, new):
        text = self.translation.read_text(encoding='utf-8')
        self.assertIn(old, text)
        self.translation.write_text(text.replace(old, new), encoding='utf-8')

    def test_matching_utf8_documents(self):
        self.assertEqual(self.run_check()[0], 0)

    def test_rejects_changed_powershell_command(self):
        self.change('-Tests', '-SkipSmoke')
        status, output = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn('shell/PowerShell examples differ', output)

    def test_rejects_changed_shell_command(self):
        self.change('--preset release', '--preset debug')
        self.assertEqual(self.run_check()[0], 1)

    def test_rejects_wrong_command_language(self):
        self.change('```powershell', '```sh')
        self.assertEqual(self.run_check()[0], 1)

    def test_rejects_missing_platform_subsection(self):
        self.change('### Windows', 'Windows')
        status, output = self.run_check()
        self.assertEqual(status, 1)
        self.assertIn('missing or extra subsection', output)


if __name__ == '__main__':
    unittest.main()
