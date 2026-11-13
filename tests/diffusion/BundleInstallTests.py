"""Exercise the optional worker's real CMake configure/install boundary without a GPU."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
CMAKE = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else shutil.which('cmake')
spec = importlib.util.spec_from_file_location('prepare_bundle', ROOT / 'tools/diffusion/prepare_bundle.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleInstallTests(unittest.TestCase):
    def setUp(self):
        if not CMAKE:
            self.skipTest('CMake is unavailable')
        self.work = tempfile.TemporaryDirectory(prefix='vulkana bundle install-')
        self.addCleanup(self.work.cleanup)
        self.directory = Path(self.work.name)
        self.prefix = self.directory / 'worker payload'
        self.source = self.directory / 'fixture source'
        self.build = self.directory / 'fixture build'
        self.destination = self.directory / 'installed app'
        self.source.mkdir()
        self.platform = 'windows' if sys.platform == 'win32' else 'linux'
        self.write_bundle(self.platform)
        # No Qt, CUDA, compiler or engine checkout is needed to test installation.
        (self.source / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.25)\n'
            'project(DiffusionBundleInstallFixture NONE)\n'
            f'set(PROJECT_SOURCE_DIR [==[{ROOT.as_posix()}]==])\n'
            f'set(Python3_EXECUTABLE [==[{Path(sys.executable).as_posix()}]==])\n'
            f'include([==[{(ROOT / "cmake/DiffusionBundle.cmake").as_posix()}]==])\n',
            encoding='utf-8')

    def write_bundle(self, platform):
        if self.prefix.exists():
            shutil.rmtree(self.prefix)
        self.data = {'schemaVersion': 1, 'backend': 'cuda', 'platform': platform,
            'appVersion': bundle.app_version(),
            'engine': json.loads((ROOT / 'src/diffusion/engine-lock.json').read_text()),
            'build': {'cpuBaseline': 'avx2-fma-f16c-bmi2',
                'cudaNativeImages': ['86', '89', '120a'], 'cudaPtxImages': ['90']},
            'files': []}
        if platform == 'windows':
            binary = bytearray(512)
            binary[:2] = b'MZ'
            struct.pack_into('<I', binary, 60, 128)
            binary[128:132] = b'PE\0\0'
            struct.pack_into('<H', binary, 132, 0x8664)
            struct.pack_into('<H', binary, 134, 1)  # One section, no imports.
            struct.pack_into('<H', binary, 148, 240)
            struct.pack_into('<H', binary, 152, 0x20b)
            struct.pack_into('<I', binary, 212, 512)  # SizeOfHeaders.
            binary[392:397] = b'.text'
            self.worker = 'bin/vulkana-diffusion-worker-cuda.exe'
            libraries = ['bin/cudart64_13.dll', 'bin/cublas64_13.dll', 'bin/cublasLt64_13.dll']
        else:
            binary = bytearray(64)
            binary[:6] = b'\x7fELF\x02\x01'
            struct.pack_into('<H', binary, 18, 62)
            self.worker = 'bin/vulkana-diffusion-worker-cuda'
            libraries = ['lib/vulkana-diffusion/cuda/' + name for name in
                ('libcudart.so.13', 'libcublas.so.13', 'libcublasLt.so.13')]
        self.libraries = libraries
        for name in [self.worker, *libraries]:
            self.add(name, binary, 'binary')
        for name in ('THIRD-PARTY-NOTICES.txt', 'licenses/stable-diffusion-MIT.txt',
                     'licenses/ggml-MIT.txt', 'licenses/CUDA-EULA.txt',
                     'licenses/CUDA-cccl-LICENSE.txt'):
            self.add(bundle.META + '/' + name, b'Fixture license text\n', 'notice')
        if platform == 'windows':
            self.add(bundle.META + '/licenses/Microsoft-Redist.txt', b'Fixture license text\n', 'notice')
        notices = {'format': 1, 'version': bundle.app_version(), 'acknowledgements': [],
                   'components': [], 'texts': {}, 'notice_text_keys': []}
        self.add(bundle.META + '/notices.json', json.dumps(notices).encode(), 'notice')
        self.save_manifest()

    def add(self, name, content, kind):
        path = self.prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        if kind == 'binary':
            path.chmod(0o755)
        self.data['files'].append(bundle.record(self.prefix, name, kind))

    def save_manifest(self):
        (self.prefix / bundle.MANIFEST).write_text(json.dumps(self.data), encoding='utf-8')

    def run_cmake(self, *args, success=True):
        result = subprocess.run([CMAKE, *map(str, args)], text=True, encoding='utf-8',
            errors='replace', stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def configure(self, success=True):
        return self.run_cmake('-S', self.source, '-B', self.build,
            '-DVULKANA_DIFFUSION_BUNDLE=' + str(self.prefix), success=success)

    def install(self, success=True):
        return self.run_cmake('--install', self.build, '--prefix', self.destination, success=success)

    def test_install_preserves_payload_and_executable_permission(self):
        self.configure()
        self.install()
        for entry in self.data['files']:
            self.assertEqual((self.destination / entry['path']).read_bytes(),
                             (self.prefix / entry['path']).read_bytes())
        self.assertEqual((self.destination / bundle.MANIFEST).read_bytes(),
                         (self.prefix / bundle.MANIFEST).read_bytes())
        self.assertTrue(os.access(self.destination / self.worker, os.X_OK))
        bundle.verify(self.destination)

    def test_changed_runtime_fails_before_any_copy(self):
        self.configure()
        with (self.prefix / self.libraries[0]).open('ab') as stream:
            stream.write(b'tampered')
        output = self.install(success=False)
        self.assertIn('integrity mismatch', output)
        self.assertFalse((self.destination / self.worker).exists())

    def test_changed_valid_manifest_requires_reconfigure(self):
        self.configure()
        added = bundle.META + '/licenses/additional-attribution.txt'
        self.add(added, b'Additional attribution\n', 'notice')
        self.save_manifest()
        bundle.verify(self.prefix)  # A valid new payload still changes the install list.
        output = self.install(success=False)
        self.assertIn('manifest changed', output)
        self.assertFalse((self.destination / self.worker).exists())
        self.configure()
        self.install()
        self.assertEqual((self.destination / added).read_bytes(), b'Additional attribution\n')
        bundle.verify(self.destination)

    def test_wrong_target_platform_is_rejected(self):
        self.write_bundle('linux' if self.platform == 'windows' else 'windows')
        bundle.verify(self.prefix)
        output = self.configure(success=False)
        self.assertIn('does not match this target platform', output)


if __name__ == '__main__':
    unittest.main()
