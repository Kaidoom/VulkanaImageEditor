"""Bundle integrity and release-policy checks; uses small synthetic binaries."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
sys.dont_write_bytecode = True
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_bundle', ROOT / 'tools/diffusion/prepare_bundle.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.prefix = Path(self.work.name)
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', elf, 18, 62)
        self.data = {'schemaVersion': 1, 'backend': 'cuda', 'platform': 'linux',
            'appVersion': bundle.app_version(),
            'engine': json.loads((ROOT / 'src/diffusion/engine-lock.json').read_text()),
            'build': {'cpuBaseline': 'avx2-fma-f16c-bmi2',
                      'cudaNativeImages': ['86', '89', '120a'], 'cudaPtxImages': ['90']},
            'files': []}
        for name in ('bin/vulkana-diffusion-worker-cuda',
                     'lib/vulkana-diffusion/cuda/libcudart.so.13',
                     'lib/vulkana-diffusion/cuda/libcublas.so.13',
                     'lib/vulkana-diffusion/cuda/libcublasLt.so.13'):
            self.add(name, elf, 'binary')
        for name in ('THIRD-PARTY-NOTICES.txt', 'licenses/stable-diffusion-MIT.txt',
                     'licenses/ggml-MIT.txt', 'licenses/CUDA-EULA.txt', 'licenses/CUDA-cccl-LICENSE.txt'):
            self.add(bundle.META + '/' + name, b'fixture legal text\n', 'notice')
        notice = {'format': 1, 'version': bundle.app_version(), 'profile': 'diffusion-cuda',
                  'acknowledgements': ['Vulkana Diffusion: fixture'],
                  'components': [{'vulkana_diffusion_component': True, 'notice': {
                      'name': 'Fixture worker', 'version': '1', 'scope': 'Test',
                      'license': 'MIT', 'copyright': 'Fixture'},
                      'texts': ['diffusion-cuda-fixture.txt']}],
                  'texts': {'diffusion-cuda-fixture.txt': 'fixture legal text'},
                  'notice_text_keys': ['diffusion-cuda-fixture.txt']}
        self.add(bundle.META + '/notices.json', json.dumps(notice).encode(), 'notice')
        self.save()

    def tearDown(self):
        self.work.cleanup()

    def add(self, name, data, kind):
        path = self.prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        if kind == 'binary': path.chmod(0o755)
        self.data['files'].append(bundle.record(self.prefix, name, kind))

    def save(self):
        (self.prefix / bundle.MANIFEST).write_text(json.dumps(self.data))

    def test_standalone_and_merged_app_prefix(self):
        bundle.verify(self.prefix)
        (self.prefix / 'bin/imageeditor').write_bytes(b'other application')
        bundle.verify(self.prefix)

    def test_binary_tampering(self):
        (self.prefix / 'bin/vulkana-diffusion-worker-cuda').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'integrity mismatch'): bundle.verify(self.prefix)

    def test_no_undeclared_private_payload(self):
        (self.prefix / 'lib/vulkana-diffusion/cuda/model.gguf').write_bytes(b'no weights')
        with self.assertRaisesRegex(ValueError, 'Untracked'): bundle.verify(self.prefix)

    def test_manifest_cannot_authorize_models(self):
        self.add('bin/model.gguf', b'weights', 'binary'); self.save()
        with self.assertRaisesRegex(ValueError, 'Unexpected worker payload'): bundle.verify(self.prefix)

    def test_paths_cannot_escape_prefix(self):
        self.data['files'][0]['path'] = '../worker'; self.save()
        with self.assertRaises(ValueError): bundle.verify(self.prefix)
        with self.assertRaises(ValueError): bundle.safe_file(self.prefix, 'C:/worker')
        with self.assertRaises(ValueError): bundle.safe_file(self.prefix, 'bin/./worker')

    def test_source_and_version_must_match(self):
        original = copy.deepcopy(self.data)
        self.data['engine']['engine']['revision'] = '0' * 40; self.save()
        with self.assertRaisesRegex(ValueError, 'baseline'): bundle.verify(self.prefix)
        self.data = original; self.data['appVersion'] = '0.0.0'; self.save()
        with self.assertRaisesRegex(ValueError, 'version'): bundle.verify(self.prefix)

    def test_forward_compatibility_not_feature_specific_ptx(self):
        self.data['build']['cudaPtxImages'] = ['120a']; self.save()
        with self.assertRaisesRegex(ValueError, 'forward-compatible'): bundle.verify(self.prefix)

    def test_notices_required(self):
        self.data['files'] = [v for v in self.data['files'] if not v['path'].endswith('CUDA-EULA.txt')]
        self.save()
        with self.assertRaisesRegex(ValueError, 'licensing'): bundle.verify(self.prefix)

    def test_symlink_rejected(self):
        path = self.prefix / 'bin/vulkana-diffusion-worker-cuda'
        original = self.prefix / 'real-worker'
        path.rename(original)
        try: path.symlink_to(original)
        except OSError: self.skipTest('Filesystem symlink creation unavailable')
        with self.assertRaisesRegex(ValueError, 'symlinks'): bundle.verify(self.prefix)

    def test_architecture_and_cpu_policy(self):
        cache = dict(CMAKE_BUILD_TYPE='Release', SD_CUDA='ON',
                     CMAKE_CUDA_ARCHITECTURES='86-real;89-real;120a-real;90-virtual')
        cache.update({k: 'ON' for k in bundle.REQUIRED_CPU_ON})
        cache.update({k: 'OFF' for k in bundle.REQUIRED_CPU_OFF})
        bundle.check_build(cache)
        cache['CMAKE_CUDA_ARCHITECTURES'] = '86-real;89-real;120-real;90-virtual'
        bundle.check_build(cache)
        cache['GGML_NATIVE'] = 'ON'
        with self.assertRaisesRegex(ValueError, 'GGML_NATIVE'): bundle.check_build(cache)
        cache['GGML_NATIVE'] = 'OFF'; cache['CMAKE_CUDA_ARCHITECTURES'] = 'native'
        with self.assertRaisesRegex(ValueError, 'CUDA targets'): bundle.check_build(cache)

    def test_merge_is_idempotent_and_preserves_source_access(self):
        base, first, second = (self.prefix / n for n in ('base', 'merged', 'merged-again'))
        base.mkdir()
        original = {'format': 1, 'version': bundle.app_version(), 'profile': 'custom',
                    'source_url': 'https://example.invalid/sources', 'source_url_published': True,
                    'acknowledgements': ['Existing notice'], 'components': [], 'texts': {}, 'notice_text_keys': []}
        source = json.dumps(original)
        (base / 'notices.json').write_text(source)
        (base / 'SOURCES.json').write_text('{"archives":[]}')
        bundle.merge_notices(base, self.prefix, first)
        bundle.merge_notices(first, self.prefix, second)
        merged = json.loads((first / 'notices.json').read_text())
        self.assertEqual(merged['profile'], original['profile'])
        self.assertEqual(merged['source_url'], original['source_url'])
        self.assertEqual(len(merged['components']), 1)
        self.assertEqual((first / 'notices.json').read_bytes(), (second / 'notices.json').read_bytes())
        self.assertEqual((base / 'notices.json').read_text(), source)
        self.assertEqual((base / 'SOURCES.json').read_bytes(), (second / 'SOURCES.json').read_bytes())

    @staticmethod
    def pe_fixture(imports=(), delayed=()):
        data = bytearray(2048)
        data[:2] = b'MZ'; struct.pack_into('<I', data, 60, 0x80)
        data[0x80:0x84] = b'PE\0\0'
        struct.pack_into('<HH', data, 0x84, 0x8664, 1)
        struct.pack_into('<H', data, 0x94, 240)
        optional = 0x98
        struct.pack_into('<H', data, optional, 0x20b)
        struct.pack_into('<Q', data, optional + 24, 0x140000000)
        struct.pack_into('<I', data, optional + 60, 512)
        struct.pack_into('<I', data, optional + 108, 16)
        struct.pack_into('<IIII', data, optional + 240 + 8, 1536, 0x1000, 1536, 512)
        for names, directory, start, descriptor in ((imports, 1, 512, 20), (delayed, 13, 768, 32)):
            if not names: continue
            struct.pack_into('<II', data, optional + 112 + directory * 8, start - 512 + 0x1000,
                             (len(names) + 1) * descriptor)
            for i, name in enumerate(names):
                offset = 1024 + directory * 32 + i * 64
                rva = offset - 512 + 0x1000
                if directory == 1: struct.pack_into('<I', data, start + i * descriptor + 12, rva)
                else: struct.pack_into('<II', data, start + i * descriptor, 1, rva)
                value = name.encode() + b'\0'; data[offset:offset + len(value)] = value
        return data

    def test_windows_imports_include_delay_loaded_runtime(self):
        name = 'bin/vulkana-diffusion-worker-cuda.exe'
        self.add(name, self.pe_fixture(('KERNEL32.dll', 'VCOMP140.DLL'), ('cublas64_13.dll',)), 'binary')
        path = self.prefix / name
        self.assertEqual(bundle.binary_kind(path), 'pe-x86_64')
        self.assertEqual(bundle.pe_imports(path), ['cublas64_13.dll', 'kernel32.dll', 'vcomp140.dll'])
        files = [self.data['files'][-1]]
        with self.assertRaisesRegex(ValueError, 'Missing bundled dependency'): bundle.check_windows_imports(self.prefix, files)
        for name in ('bin/VCOMP140.DLL', 'bin/cublas64_13.dll'):
            self.add(name, self.pe_fixture(), 'binary'); files.append(self.data['files'][-1])
        bundle.check_windows_imports(self.prefix, files)

    def test_elf_audit_checks_runtime_abi_and_relative_loader_paths(self):
        dynamic = ('  (NEEDED) Shared library: [libcublas.so.13]\n'
                   '  (RPATH) Library rpath: [$ORIGIN/../lib/vulkana-diffusion/cuda:$ORIGIN/../lib]\n')
        symbols = 'Version needs section\n Name: GLIBC_2.34\n Name: GLIBCXX_3.4.32\n Name: CXXABI_1.3.13\n'
        with patch.object(bundle, 'run', side_effect=[dynamic, symbols]):
            imports, versions = bundle.inspect_elf(Path('worker'), True, '2.39')
        self.assertEqual(imports, ['libcublas.so.13'])
        self.assertEqual(versions['GLIBCXX'], '3.4.32')
        with patch.object(bundle, 'run', return_value=dynamic.replace('RPATH', 'RUNPATH')):
            with self.assertRaisesRegex(ValueError, 'transitive'): bundle.inspect_elf(Path('worker'), True, '2.39')
        with patch.object(bundle, 'run', side_effect=['', symbols.replace('GLIBCXX_3.4.32', 'GLIBCXX_3.4.34')]):
            with self.assertRaisesRegex(ValueError, 'GLIBCXX'): bundle.inspect_elf(Path('runtime.so'), False, '2.39')
        with patch.object(bundle, 'run', side_effect=['', symbols.replace('GLIBC_2.34', 'GLIBC_2.40')]):
            with self.assertRaisesRegex(ValueError, 'GLIBC_2.40'): bundle.inspect_elf(Path('runtime.so'), False, '2.39')

    def test_elf_closure_does_not_allow_missing_private_libraries(self):
        files = [{'kind': 'binary', 'path': 'bin/vulkana-diffusion-worker-cuda',
                  'imports': ['libcuda.so.1', 'libcublas.so.13']}]
        with self.assertRaisesRegex(ValueError, 'libcublas'): bundle.check_linux_imports(files)
        files.append({'kind': 'binary', 'path': 'lib/vulkana-diffusion/cuda/libcublas.so.13',
                      'imports': ['libc.so.6']})
        bundle.check_linux_imports(files)

    def test_global_flags_cannot_override_portable_cpu_policy(self):
        for key, value in [('CMAKE_CXX_FLAGS', '-march=native'),
                           ('CMAKE_C_FLAGS_RELEASE', '-O3 -march=x86-64-v4'),
                           ('CMAKE_CXX_FLAGS_RELWITHDEBINFO', '-march=znver5'),
                           ('CMAKE_CUDA_FLAGS', '-Xcompiler="-march=native"'),
                           ('CMAKE_CUDA_FLAGS_RELEASE', '-Xcompiler=/arch:AVX512'),
                           ('CMAKE_CXX_FLAGS', '-mavx512f -mavx512bw'),
                           ('CMAKE_C_FLAGS', '-mcpu=native'),
                           ('CMAKE_CXX_FLAGS', '-mamx-tile')]:
            with self.subTest(key=key, value=value):
                with self.assertRaisesRegex(ValueError, 'portable CPU policy'):
                    bundle.check_global_cpu_flags({key: value})
        self.assertEqual(bundle.check_global_cpu_flags({'CMAKE_CXX_FLAGS': '-O3 -DNDEBUG'}), {})
        self.assertEqual(bundle.check_global_cpu_flags({'CMAKE_CUDA_FLAGS': '-Xcompiler=-march=x86-64-v3'}),
                         {'CMAKE_CUDA_FLAGS': ['-march=x86-64-v3']})
        bundle.check_global_cpu_flags({'CMAKE_CXX_FLAGS_RELEASE': '/O2 /arch:AVX2',
                                       'CMAKE_C_FLAGS': '-mtune=native -mno-avx512f'})


if __name__ == '__main__': unittest.main()
