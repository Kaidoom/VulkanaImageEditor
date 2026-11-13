#!/usr/bin/env python3
"""Audit an already staged CUDA worker and write its offline distribution inventory.

Build-time only; downloads nothing and never copies model weights. The resulting
manifest checks integrity and provenance, not a signature or legal approval.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
META = 'share/vulkana-editor/diffusion/cuda'
MANIFEST = META + '/manifest.json'
SCHEMA = 1
LINUX_LIBS = {'libcudart.so.13', 'libcublas.so.13', 'libcublasLt.so.13',
              'libstdc++.so.6', 'libgcc_s.so.1', 'libgomp.so.1'}
WINDOWS_LIBS = {'cudart64_13.dll', 'cublas64_13.dll', 'cublaslt64_13.dll',
                'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
                'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
                'vcruntime140.dll', 'vcruntime140_1.dll', 'vcomp140.dll',
                'concrt140.dll'}
CUDA_NAMES = {'libcudart.so.13', 'libcublas.so.13', 'libcublaslt.so.13',
              'cudart64_13.dll', 'cublas64_13.dll', 'cublaslt64_13.dll'}
WINDOWS_SYSTEM = {'kernel32.dll', 'ntdll.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll',
                  'bcrypt.dll', 'bcryptprimitives.dll', 'crypt32.dll', 'ole32.dll',
                  'oleaut32.dll', 'shell32.dll', 'shlwapi.dll', 'setupapi.dll',
                  'version.dll', 'winmm.dll', 'ws2_32.dll', 'msvcrt.dll', 'ucrtbase.dll',
                  'psapi.dll', 'dbghelp.dll', 'dxcore.dll', 'nvcuda.dll', 'nvapi64.dll'}
LINUX_SYSTEM = {'libcuda.so.1', 'libstdc++.so.6', 'libgcc_s.so.1', 'libgomp.so.1',
                'libm.so.6', 'libc.so.6', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
                'ld-linux-x86-64.so.2'}
REQUIRED_CPU_ON = ('GGML_AVX', 'GGML_AVX2', 'GGML_FMA', 'GGML_F16C', 'GGML_BMI2')
REQUIRED_CPU_OFF = ('GGML_NATIVE', 'GGML_CPU_ALL_VARIANTS', 'GGML_AVX512',
                    'GGML_AVX512_BF16', 'GGML_AVX512_VBMI', 'GGML_AVX512_VNNI',
                    'GGML_AVX_VNNI', 'GGML_AMX_TILE', 'GGML_AMX_INT8', 'GGML_AMX_BF16')


def fail(message):
    raise ValueError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def run(*args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                            encoding='utf-8', errors='replace', check=False,
                            env=dict(os.environ, LC_ALL='C', LANG='C'))
    if result.returncode:
        fail(f'{Path(str(args[0])).name} failed: {result.stderr.strip()}')
    return result.stdout


def version_tuple(text):
    return tuple(int(v) for v in text.split('.'))


def app_version():
    return re.search(r'set\(VULKANA_VERSION "([^"]+)"\)',
                     (ROOT / 'CMakeLists.txt').read_text()).group(1)


def read_cache(path):
    return dict(re.findall(r'^([^/#\n][^:\n]*):[^=\n]*=(.*)$',
                           path.read_text(encoding='utf-8'), re.M))


def check_global_cpu_flags(cache):
    audited = {}
    for key, value in cache.items():
        if not re.fullmatch(r'CMAKE_(C|CXX|CUDA)_FLAGS(?:_[A-Z0-9_]+)?', key): continue
        # Also inspect nvcc -Xcompiler arguments; quoting does not change their ISA.
        flags = value.replace('"', '').replace("'", '')
        found = []
        for match in re.finditer(r'-(march|mcpu)(?:=|\s+)([A-Za-z0-9_.+-]+)', flags, re.I):
            option, target = match[1].lower(), match[2].lower()
            if option == 'mcpu' or target not in ('x86-64', 'x86-64-v2', 'x86-64-v3'):
                fail(f'{key} exceeds the portable CPU policy: {match[0]}')
            found.append(match[0])
        advanced = re.search(r'(?<![\w-])-m(?:avx512[\w-]*|avxvnni[\w-]*|avx10[\w.-]*|amx[\w-]*|avxifma|vaes|vpclmulqdq|gfni|sha|aes|clwb)\b', flags, re.I)
        if advanced: fail(f'{key} exceeds the portable CPU policy: {advanced[0]}')
        for match in re.finditer(r'/arch:([A-Za-z0-9_.-]+)', flags, re.I):
            if match[1].upper() not in ('SSE', 'SSE2', 'AVX', 'AVX2'):
                fail(f'{key} exceeds the portable CPU policy: {match[0]}')
            found.append(match[0])
        if found: audited[key] = found
    return audited


def compiler_identity(build):
    records = list(build.glob('CMakeFiles/*/CMakeCXXCompiler.cmake'))
    if not records: fail('CMake compiler identity is missing')
    path = max(records, key=lambda p: version_tuple(p.parent.name))
    text = path.read_text(encoding='utf-8')
    result = {}
    for name, key in [('ID', 'id'), ('VERSION', 'version')]:
        match = re.search(r'set\(CMAKE_CXX_COMPILER_' + name + r' "([^"]+)"\)', text)
        if not match: fail('CMake compiler identity is incomplete')
        result[key] = match[1]
    return result


def check_build(cache):
    if cache.get('CMAKE_BUILD_TYPE') != 'Release' or cache.get('SD_CUDA') != 'ON':
        fail('A Release CUDA worker build is required')
    check_global_cpu_flags(cache)
    for option in REQUIRED_CPU_ON:
        if cache.get(option) != 'ON': fail(f'Release CPU baseline requires {option}=ON')
    for option in REQUIRED_CPU_OFF:
        if cache.get(option) != 'OFF': fail(f'Release CPU baseline requires {option}=OFF')
    for option in ('SD_VULKAN', 'SD_BUILD_SHARED_LIBS', 'SD_BUILD_SHARED_GGML_LIB',
                   'SD_WEBP', 'SD_WEBM', 'GGML_RPC', 'GGML_OPENMP_FETCH'):
        if cache.get(option) not in ('OFF', None): fail(f'Unsupported release option: {option}')
    arch = set(cache.get('CMAKE_CUDA_ARCHITECTURES', '').split(';'))
    if arch not in ({'86-real', '89-real', '120-real', '90-virtual'},
                    {'86-real', '89-real', '120a-real', '90-virtual'}):
        fail('Release CUDA targets must contain 86/89/120[a] native images and generic 90 PTX')
    return sorted(arch)


def safe_file(prefix, relative):
    p = PurePosixPath(relative)
    if (not relative or '\\' in relative or p.is_absolute() or
            any(part in ('', '.', '..') or ':' in part for part in relative.split('/'))):
        fail(f'Unsafe bundle path: {relative}')
    target = prefix / p
    current = prefix
    for part in p.parts:
        current = current / part
        if current.is_symlink() or getattr(current, 'is_junction', lambda: False)():
            fail(f'Bundle symlinks or junctions are not allowed: {relative}')
    if not target.is_file(): fail(f'Missing bundle file: {relative}')
    if not target.resolve().is_relative_to(prefix.resolve()): fail(f'Bundle path escapes prefix: {relative}')
    return target


def bundle_files(prefix, directory=None):
    """Visit regular payload files without descending into aliases or junctions."""
    for root, dirs, files in os.walk(directory or prefix, followlinks=False):
        for name in dirs:
            path = Path(root) / name
            if path.is_symlink() or getattr(path, 'is_junction', lambda: False)() or not path.resolve().is_relative_to(prefix):
                fail(f'Bundle directory aliases are not allowed: {path.relative_to(prefix)}')
        for name in files:
            path = Path(root) / name
            safe_file(prefix, path.relative_to(prefix).as_posix())
            yield path


def binary_kind(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if header.startswith(b'\x7fELF'):
            if len(header) < 64 or header[4:6] != b'\x02\x01' or struct.unpack_from('<H', header, 18)[0] != 62:
                fail(f'Expected an x86-64 ELF binary: {path.name}')
            return 'elf-x86_64'
        if header.startswith(b'MZ') and len(header) == 64:
            stream.seek(struct.unpack_from('<I', header, 60)[0])
            pe = stream.read(26)
            if (len(pe) == 26 and pe[:4] == b'PE\0\0' and
                    struct.unpack_from('<H', pe, 4)[0] == 0x8664 and
                    struct.unpack_from('<H', pe, 24)[0] == 0x20b):
                return 'pe-x86_64'
    fail(f'Expected a native x86-64 executable or library: {path.name}')


def pe_imports(path):
    """Read normal and delayed imports without executing a staged binary."""
    with path.open('rb') as stream:
        def read(offset, count):
            if offset < 0: fail(f'Invalid PE offset: {path.name}')
            stream.seek(offset)
            data = stream.read(count)
            if len(data) != count: fail(f'Truncated PE binary: {path.name}')
            return data
        head = read(0, 64)
        pe_offset = struct.unpack_from('<I', head, 60)[0]
        pe = read(pe_offset, 24)
        sections, optional_size = struct.unpack_from('<H', pe, 6)[0], struct.unpack_from('<H', pe, 20)[0]
        if pe[:4] != b'PE\0\0' or not 0 < sections <= 128 or not 112 <= optional_size <= 4096:
            fail(f'Invalid PE header: {path.name}')
        optional = read(pe_offset + 24, optional_size)
        image_base = struct.unpack_from('<Q', optional, 24)[0]
        header_size = struct.unpack_from('<I', optional, 60)[0]
        table = read(pe_offset + 24 + optional_size, sections * 40)
        ranges = []
        for i in range(sections):
            virtual_size, address, raw_size, raw_offset = struct.unpack_from('<IIII', table, i * 40 + 8)
            ranges.append((address, raw_size, raw_offset))
        def offset(rva):
            if rva < header_size: return rva
            for address, size, raw in ranges:
                if address <= rva < address + size: return raw + rva - address
            fail(f'Invalid PE import address: {path.name}')
        def name(rva):
            start = offset(rva)
            stream.seek(start)
            raw = stream.read(256).split(b'\0', 1)[0]
            if not re.fullmatch(rb'[A-Za-z0-9_.-]+\.dll', raw, re.I):
                fail(f'Invalid PE import name: {path.name}')
            return raw.decode('ascii').lower()
        result = set()
        directory_count = min(struct.unpack_from('<I', optional, 108)[0], (optional_size - 112) // 8)
        for index, size in ((1, 20), (13, 32)):
            if directory_count <= index: continue
            rva = struct.unpack_from('<I', optional, 112 + index * 8)[0]
            if not rva: continue
            start = offset(rva)
            for i in range(1024):
                row = read(start + i * size, size)
                if not any(row): break
                if index == 1:
                    name_rva = struct.unpack_from('<I', row, 12)[0]
                else:
                    attributes, name_rva = struct.unpack_from('<II', row)
                    if attributes == 0: name_rva -= image_base
                    elif attributes != 1: fail(f'Unsupported PE delay import attributes: {path.name}')
                result.add(name(name_rva))
            else: fail(f'Unterminated PE import directory: {path.name}')
        return sorted(result)


def check_windows_imports(prefix, files):
    binaries = [entry for entry in files if entry['kind'] == 'binary']
    available = {PurePosixPath(entry['path']).name.lower() for entry in binaries}
    for entry in binaries:
        imports = pe_imports(safe_file(prefix, entry['path']))
        for name in imports:
            if name in available or name in WINDOWS_SYSTEM or name.startswith(('api-ms-', 'ext-ms-')): continue
            fail(f'Missing bundled dependency {name} required by {entry["path"]}')
        if 'imports' in entry and entry['imports'] != imports:
            fail(f'PE import inventory mismatch: {entry["path"]}')


def inspect_elf(path, is_worker, minimum_glibc):
    dynamic = run('readelf', '--dynamic', path)
    imports = sorted(set(re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)))
    paths = re.findall(r'\((RPATH|RUNPATH)\).*?\[([^\]]*)\]', dynamic)
    if is_worker and paths not in ([('RPATH', '$ORIGIN/../lib/vulkana-diffusion/cuda')],
                                  [('RPATH', '$ORIGIN/../lib/vulkana-diffusion/cuda:$ORIGIN/../lib')]):
        fail('Worker must use the relative, transitive private CUDA RPATH')
    for _, search in paths:
        if any(value and not value.startswith('$ORIGIN') for value in search.split(':')):
            fail(f'Non-relocatable ELF search path in {path.name}')
    symbols = run('readelf', '--version-info', path)
    # Dependencies' own exported symbol versions are not requirements.
    needs = symbols.split('Version needs section', 1)[-1] if 'Version needs section' in symbols else ''
    maxima = {}
    for namespace, baseline in [('GLIBC', minimum_glibc), ('GLIBCXX', '3.4.33'), ('CXXABI', '1.3.15')]:
        values = re.findall(r'\b' + namespace + r'_(\d+\.\d+(?:\.\d+)?)', needs)
        if values:
            value = max(values, key=version_tuple)
            if version_tuple(value) > version_tuple(baseline):
                fail(f'{path.name} requires {namespace}_{value}, beyond the release baseline {baseline}')
            maxima[namespace] = value
    return imports, maxima


def check_linux_imports(files):
    binaries = [entry for entry in files if entry['kind'] == 'binary']
    available = {PurePosixPath(entry['path']).name for entry in binaries}
    for entry in binaries:
        for name in entry.get('imports', []):
            if name not in available and name not in LINUX_SYSTEM:
                fail(f'Missing bundled dependency {name} required by {entry["path"]}')


def payload_allowed(name, platform):
    p = PurePosixPath(name)
    if platform == 'windows':
        return p.parent == PurePosixPath('bin') and (p.name.lower() in WINDOWS_LIBS or
                    p.name == 'vulkana-diffusion-worker-cuda.exe')
    return (name == 'bin/vulkana-diffusion-worker-cuda' or
            p.parent == PurePosixPath('lib/vulkana-diffusion/cuda') and p.name in LINUX_LIBS)


def metadata_allowed(name):
    p = PurePosixPath(name)
    return name in (META + '/THIRD-PARTY-NOTICES.txt', META + '/notices.json') or (
        p.parent == PurePosixPath(META + '/licenses') and p.suffix in ('.txt', '.md'))


def record(prefix, name, kind):
    path = safe_file(prefix, name)
    return {'path': name, 'sha256': digest(path), 'size': path.stat().st_size, 'kind': kind}


def verify(prefix):
    prefix = prefix.resolve()
    data = json.loads(safe_file(prefix, MANIFEST).read_text(encoding='utf-8'))
    if data.get('schemaVersion') != SCHEMA or data.get('backend') != 'cuda': fail('Unsupported worker manifest')
    platform = data.get('platform')
    if platform not in ('windows', 'linux'): fail('Unsupported worker platform')
    if data.get('appVersion') != app_version(): fail('Worker bundle version does not match Vulkana')
    lock = json.loads((ROOT / 'src/diffusion/engine-lock.json').read_text())
    if data.get('engine') != lock: fail('Worker bundle uses a different engine or patch baseline')
    if data.get('build', {}).get('cpuBaseline') != 'avx2-fma-f16c-bmi2': fail('Unsupported worker CPU baseline')
    native = set(data.get('build', {}).get('cudaNativeImages', []))
    if not {'86', '89'} <= native or not native.intersection({'120', '120a'}): fail('Missing native CUDA images')
    if '90' not in data.get('build', {}).get('cudaPtxImages', []): fail('Missing generic forward-compatible PTX')
    seen = set()
    for entry in data.get('files', []):
        name = entry.get('path', '')
        if name.casefold() in seen: fail(f'Duplicate bundle entry: {name}')
        seen.add(name.casefold())
        if entry.get('kind') == 'binary':
            if not payload_allowed(name, platform): fail(f'Unexpected worker payload: {name}')
        elif entry.get('kind') == 'notice':
            if not metadata_allowed(name): fail(f'Unexpected notice path: {name}')
        else: fail(f'Unsupported bundle entry: {name}')
        path = safe_file(prefix, name)
        if path.stat().st_size != entry.get('size') or digest(path) != entry.get('sha256'):
            fail(f'Worker bundle integrity mismatch: {name}')
        if entry['kind'] == 'binary':
            expected = 'pe-x86_64' if platform == 'windows' else 'elf-x86_64'
            if binary_kind(path) != expected: fail(f'Wrong platform: {name}')
            if platform == 'linux' and name.startswith('bin/') and not os.access(path, os.X_OK):
                fail('Worker executable is not executable')
    required = {'bin/vulkana-diffusion-worker-cuda' + ('.exe' if platform == 'windows' else ''),
                META + '/THIRD-PARTY-NOTICES.txt', META + '/notices.json', META + '/licenses/stable-diffusion-MIT.txt',
                META + '/licenses/ggml-MIT.txt', META + '/licenses/CUDA-cccl-LICENSE.txt'}
    runtime_prefix = 'bin/' if platform == 'windows' else 'lib/vulkana-diffusion/cuda/'
    required.update(runtime_prefix + v for v in
                    (('cublas64_13.dll', 'cublasLt64_13.dll') if platform == 'windows'
                     else ('libcudart.so.13', 'libcublas.so.13', 'libcublasLt.so.13')))
    if not {v.casefold() for v in required} <= seen: fail('Worker or required runtime/notices are missing from manifest')
    cuda_eula = (META + '/licenses/CUDA-EULA.txt').casefold()
    cuda_components = {(META + '/licenses/CUDA-' + c + '-LICENSE.txt').casefold()
                       for c in ('cuda_cudart', 'libcublas')}
    if cuda_eula not in seen and not cuda_components <= seen: fail('CUDA runtime licensing texts are missing')
    if platform == 'windows' and (META + '/licenses/Microsoft-Redist.txt').casefold() not in seen:
        fail('Microsoft runtime redistribution notice is missing')
    if platform == 'windows': check_windows_imports(prefix, data['files'])
    else: check_linux_imports(data['files'])
    for directory in (prefix / META, prefix / 'lib/vulkana-diffusion/cuda'):
        if directory.exists():
            for path in bundle_files(prefix, directory):
                name = path.relative_to(prefix).as_posix()
                if name != MANIFEST and name.casefold() not in seen:
                    fail(f'Untracked worker bundle file: {name}')
    return data


def read_text(path):
    raw = path.read_bytes()
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'
    text = raw.decode(encoding, errors='strict')
    if not text.strip() or '\0' in text: fail(f'Invalid legal text: {path.name}')
    return text


def legal_comments(path):
    text = read_text(path)
    blocks = re.findall(r'/\*.*?\*/', text, re.S)
    result = [v for v in blocks if ('Permission is hereby granted' in v or
                                    'This software is available under 2 licenses' in v or
                                    'This is free and unencumbered software' in v or
                                    'granted a perpetual, irrevocable license' in v)]
    if not result: fail(f'No embedded notice in {path.name}')
    return '\n\n'.join(dict.fromkeys(result)) + '\n'


def notices(engine, cuda, msvc_license, platform):
    files = {
        'stable-diffusion-MIT.txt': engine / 'LICENSE', 'ggml-MIT.txt': engine / 'ggml/LICENSE',
        'CUTLASS-BSD.txt': engine / 'thirdparty/cutlass/LICENSE.txt',
        'SageAttention-Apache-2.0.txt': engine / 'src/core/cuda/sageattention/LICENSE',
        'darts-clone-BSD.txt': engine / 'thirdparty/LICENSE.darts_clone.txt',
        'Oniguruma-BSD.txt': engine / 'thirdparty/oniguruma/COPYING',
        'utf8proc-MIT-Unicode.md': engine / 'thirdparty/utf8proc/LICENSE.md',
        'nlohmann-json-MIT.txt': ROOT / 'tools/diffusion/licenses/nlohmann-json-MIT.txt',
        'zip-MIT.txt': ROOT / 'tools/diffusion/licenses/zip-MIT.txt',
        'GCC-GPL-3.0.txt': ROOT / 'res/notices/licenses/GPL-3.0.txt',
        'GCC-runtime-exception.txt': ROOT / 'res/notices/licenses/GCC-exception-3.1.txt'}
    if platform == 'windows':
        files = {k: v for k, v in files.items() if not k.startswith('GCC-')}
    out = {name: read_text(path) for name, path in files.items()}
    for name in ('stb_image.h', 'stb_image_resize.h', 'stb_image_write.h', 'miniz.h'):
        out[name + '-licenses.txt'] = legal_comments(engine / 'thirdparty' / name)
    # Keep embedded contributors' attributions as well as the library's MIT text.
    json_header = read_text(engine / 'thirdparty/json.hpp')
    authors = sorted(set(re.findall(r'SPDX-FileCopyrightText: (.*)', json_header)))
    out['nlohmann-json-attributions.txt'] = '\n'.join(authors) + '\n'
    eula = cuda / 'EULA.txt'
    if not eula.is_file(): eula = cuda / 'LICENSE'
    if eula.is_file(): out['CUDA-EULA.txt'] = read_text(eula)
    for component in ('cuda_cudart', 'libcublas', 'cccl', 'cuda_crt'):
        path = cuda / 'licenses' / component / 'LICENSE'
        if path.is_file(): out[f'CUDA-{component}-LICENSE.txt'] = read_text(path)
    if 'CUDA-cccl-LICENSE.txt' not in out:
        # Some Linux SDK layouts omit the standalone CCCL notice. These two
        # CUDA 13.3 header versions have the same upstream license collection.
        headers = [cuda / 'include/cccl', cuda / 'targets/x86_64-linux/include/cccl']
        header = next((p / 'thrust/version.h' for p in headers if (p / 'thrust/version.h').is_file()), None)
        if not header or not re.search(r'#define THRUST_VERSION (300301|300304)\b', read_text(header)):
            fail('Supply the selected CUDA SDK CCCL license collection under licenses/cccl/LICENSE')
        out['CUDA-cccl-LICENSE.txt'] = read_text(ROOT / 'tools/diffusion/licenses/CCCL-LICENSE.txt')
    if not any(name.startswith('CUDA-') for name in out): fail('CUDA SDK licensing texts are missing')
    if msvc_license:
        out['Microsoft-Redist.txt'] = read_text(msvc_license)
    return out


def sdk_original(cuda, basename):
    for folder in ('bin', 'bin/x64', 'lib64', 'targets/x86_64-linux/lib'):
        path = cuda / folder
        if path.is_dir():
            for candidate in path.iterdir():
                if candidate.name.casefold() == basename.casefold() and candidate.is_file():
                    return candidate
    fail(f'Cannot find original CUDA runtime in SDK: {basename}')


def prepare(args):
    prefix, engine, cuda, build = (p.resolve() for p in (args.prefix, args.engine, args.cuda_root, args.build))
    lock = json.loads((ROOT / 'src/diffusion/engine-lock.json').read_text())
    if run('git', '-C', engine, 'rev-parse', 'HEAD').strip() != lock['engine']['revision']: fail('Wrong engine revision')
    for name, revision in lock['dependencies'].items():
        if run('git', '-C', engine / name, 'rev-parse', 'HEAD').strip() != revision: fail(f'Wrong dependency: {name}')
    for patch in lock['patches']:
        path = ROOT / 'src/diffusion' / patch['path']
        if digest(path) != patch['sha256']: fail('Recorded patch checksum mismatch')
        run('git', '-C', engine, 'apply', '--reverse', '--check', path)
    cache = read_cache(build / 'CMakeCache.txt')
    arch = check_build(cache)
    if args.platform == 'linux' and (not args.minimum_glibc or not re.fullmatch(r'\d+\.\d+', args.minimum_glibc)):
        fail('--minimum-glibc is required')
    exe = 'bin/vulkana-diffusion-worker-cuda' + ('.exe' if args.platform == 'windows' else '')
    executable = safe_file(prefix, exe)
    cuobjdump = cuda / 'bin' / ('cuobjdump.exe' if args.platform == 'windows' else 'cuobjdump')
    native = sorted(set(re.findall(r'\.sm_([0-9]+a?)\.cubin', run(cuobjdump, '--list-elf', executable))))
    ptx = sorted(set(re.findall(r'\.sm_([0-9]+a?)\.ptx', run(cuobjdump, '--list-ptx', executable))))
    if not {'86', '89'} <= set(native) or not set(native).intersection({'120', '120a'}) or '90' not in ptx:
        fail('Worker binary does not contain the required CUDA native/PTX images')
    files = []
    for path in sorted(bundle_files(prefix)):
        name = path.relative_to(prefix).as_posix()
        if name.startswith(META + '/'): continue
        if not payload_allowed(name, args.platform): fail(f'Unexpected staged file: {name}')
        binary_kind(path)
        entry = record(prefix, name, 'binary')
        if args.platform == 'windows': entry['imports'] = pe_imports(path)
        else:
            entry['imports'], entry['requiredSymbolVersions'] = inspect_elf(path, name == exe, args.minimum_glibc)
        if path.name.casefold() in CUDA_NAMES:
            if digest(sdk_original(cuda, path.name)) != entry['sha256']:
                fail(f'CUDA runtime differs from the original SDK file: {name}')
            entry['origin'] = 'NVIDIA CUDA redistributable'
        files.append(entry)
    if args.platform == 'windows' and not args.msvc_license: fail('--msvc-license is required for a Windows payload')
    if args.platform == 'windows': check_windows_imports(prefix, files)
    else: check_linux_imports(files)
    versions = {}
    if (cuda / 'version.json').is_file():
        versions = {k: v.get('version') for k, v in json.loads((cuda / 'version.json').read_text()).items() if isinstance(v, dict)}
    elif args.cuda_manifest:
        source = json.loads(args.cuda_manifest.read_text())
        versions = {k: v['version'] for k, v in source.items() if isinstance(v, dict) and 'version' in v}
        versions['cuda'] = source.get('release_label')
    nvcc = cuda / 'bin' / ('nvcc.exe' if args.platform == 'windows' else 'nvcc')
    nvcc_version = re.search(r'\bV(\d+\.\d+\.\d+)', run(nvcc, '--version'))
    if not nvcc_version or not nvcc_version[1].startswith('13.3.'): fail('Release inventory currently supports CUDA 13.3')
    legal = notices(engine, cuda, args.msvc_license, args.platform)
    metadata = prefix / META
    if metadata.exists(): fail('Worker metadata already exists; stage into a new prefix or use --verify')
    (metadata / 'licenses').mkdir(parents=True)
    text = ('Vulkana Diffusion CUDA worker — third-party notices\n\n'
            'Vulkana and the inference engine are open source. Bundled third-party\n'
            'runtime components retain their own terms; Vulkana\'s MIT license does not\n'
            'relicense these files. CUDA runtimes are supplied for use with Vulkana.\n'
            'The NVIDIA graphics driver and all model weights are supplied by the user.\n\n'
            'Engine: ' + lock['engine']['url'] + ' at ' + lock['engine']['revision'] + '\n'
            'Recorded downstream patches: ' + ', '.join(p['path'] for p in lock['patches']) + '\n\n')
    for name, content in sorted(legal.items()):
        path = metadata / 'licenses' / name
        path.write_text(content, encoding='utf-8')
        files.append(record(prefix, path.relative_to(prefix).as_posix(), 'notice'))
        text += '\n--- ' + name + ' ---\n\n' + content + '\n'
    (metadata / 'THIRD-PARTY-NOTICES.txt').write_text(text, encoding='utf-8')
    files.append(record(prefix, META + '/THIRD-PARTY-NOTICES.txt', 'notice'))
    requirements = {'cpu': 'x86-64 with AVX2, FMA, F16C and BMI2',
        'nvidiaDriver': 'CUDA 13 compatible driver (R580+ for native code; PTX JIT requires a driver supporting this toolkit)',
        'driverReference': 'https://docs.nvidia.com/cuda/archive/13.3.1/cuda-toolkit-release-notes/'}
    if args.platform == 'linux':
        requirements['minimumGlibc'] = args.minimum_glibc
        requirements['systemLibraries'] = sorted({name for entry in files for name in entry.get('imports', []) if name in LINUX_SYSTEM})
        requirements['requiredSymbolVersions'] = {}
        for entry in files:
            for name, value in entry.get('requiredSymbolVersions', {}).items():
                old = requirements['requiredSymbolVersions'].get(name, '0')
                if version_tuple(value) > version_tuple(old): requirements['requiredSymbolVersions'][name] = value
    app_notices = make_app_notices(legal, lock, args.platform, versions, nvcc_version[1])
    (metadata / 'notices.json').write_text(json.dumps(app_notices, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    files.append(record(prefix, META + '/notices.json', 'notice'))
    data = {'schemaVersion': SCHEMA, 'appVersion': app_version(), 'backend': 'cuda',
        'platform': args.platform, 'engine': lock,
        'build': {'configuration': 'Release', 'cpuBaseline': 'avx2-fma-f16c-bmi2',
            'cudaArchitectures': arch, 'cudaNativeImages': native, 'cudaPtxImages': ptx,
            'cudaCompilerVersion': nvcc_version[1], 'cudaComponents': versions,
            'cxxCompiler': compiler_identity(build),
            'globalCpuTargetFlags': check_global_cpu_flags(cache)},
        'runtimeRequirements': requirements, 'files': sorted(files, key=lambda e: e['path'])}
    (prefix / MANIFEST).write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    return verify(prefix)


def make_app_notices(legal, lock, platform, versions, compiler):
    prefix = 'diffusion-cuda-'
    components = []

    def component(name, version, license_name, keys, scope):
        components.append({'vulkana_diffusion_component': True, 'notice': {
            'name': name, 'version': version, 'license': license_name, 'scope': scope,
            'copyright': 'Original copyright and attribution statements are retained in the accompanying texts.'},
            'texts': [prefix + key for key in keys]})

    component('Vulkana Diffusion / stable-diffusion.cpp', lock['engine']['revision'], 'MIT',
              ['stable-diffusion-MIT.txt'], 'Compiled into the optional CUDA worker; downstream engine patches are recorded in its manifest.')
    component('Vulkana Diffusion / GGML', lock['dependencies']['ggml'], 'MIT',
              ['ggml-MIT.txt'], 'Tensor computation library compiled into the worker.')
    component('Vulkana Diffusion / CUTLASS', lock['dependencies']['thirdparty/cutlass'], 'BSD-3-Clause',
              ['CUTLASS-BSD.txt'], 'CUDA kernel templates compiled into the worker.')
    component('Vulkana Diffusion / SageAttention', 'Engine-pinned sources', 'Apache-2.0',
              ['SageAttention-Apache-2.0.txt'], 'Optional attention kernels compiled into the worker.')
    utilities = [key for key in legal if key.startswith(('stb_', 'miniz.', 'darts-', 'Oniguruma-', 'utf8proc-', 'nlohmann-', 'zip-'))]
    component('Vulkana Diffusion / image and tokenizer utilities', 'Engine-pinned sources',
              'MIT, BSD and Unicode licenses; see component texts', utilities,
              'Image I/O, archive parsing, JSON and tokenization code compiled into the worker.')
    component('Vulkana Diffusion / NVIDIA CUDA runtimes', versions.get('cuda') or compiler,
              'NVIDIA CUDA Toolkit EULA and incorporated third-party terms',
              [key for key in legal if key.startswith('CUDA-')],
              'Compiled CUDA support headers and redistributed runtime/cuBLAS components for use by Vulkana. The graphics driver is supplied by the user.')
    if platform == 'windows':
        component('Vulkana Diffusion / Microsoft Visual C++ runtime', 'Build-selected REDIST files; hashes in worker manifest',
                  'Microsoft Visual Studio redistribution terms', ['Microsoft-Redist.txt'],
                  'Redistributed C++ and OpenMP runtime DLLs from the Visual Studio REDIST directory.')
    else:
        component('Vulkana Diffusion / GCC runtime', 'Build-selected system runtime',
                  'GPL-3.0-or-later WITH GCC-exception-3.1',
                  ['GCC-GPL-3.0.txt', 'GCC-runtime-exception.txt'],
                  'Runtime/header code used by the worker. Separate runtime libraries are system dependencies unless included by the enclosing application package.')
    texts = {prefix + name: content for name, content in legal.items()}
    return {'format': 1, 'profile': 'diffusion-cuda', 'version': app_version(),
            'acknowledgements': ['Vulkana Diffusion: the optional native worker includes open-source inference code and CUDA runtime components under their own terms. Model weights and the NVIDIA driver are not included.'],
            'components': components, 'texts': texts, 'notice_text_keys': sorted(texts)}


def merge_notices(base, prefix, output):
    verify(prefix)
    if base.resolve() == output.resolve(): fail('Merged notices must use a separate output directory')
    data = json.loads((base / 'notices.json').read_text(encoding='utf-8'))
    worker = json.loads((prefix / META / 'notices.json').read_text(encoding='utf-8'))
    if data.get('format') != 1 or data.get('version') != app_version(): fail('Base notice catalog does not match Vulkana')
    data['components'] = [c for c in data['components'] if not c.get('vulkana_diffusion_component')] + worker['components']
    data['texts'] = {k: v for k, v in data['texts'].items() if not k.startswith('diffusion-cuda-')}
    data['texts'].update(worker['texts'])
    # Downstream inventories may also contain build/audit records. Preserve
    # their existing reader projection rather than publishing every raw text.
    data['notice_text_keys'] = sorted({k for k in data.get('notice_text_keys', data['texts'])
                                      if not k.startswith('diffusion-cuda-')} | set(worker['notice_text_keys']))
    data['acknowledgements'] = [a for a in data['acknowledgements'] if not a.startswith('Vulkana Diffusion:')] + worker['acknowledgements']
    encoded = json.dumps(data, indent=2, ensure_ascii=False) + '\n'
    if len(encoded.encode('utf-8')) > 16 * 1024 * 1024: fail('Merged notices exceed the application reader limit')
    output.mkdir(parents=True, exist_ok=True)
    lines = ['Vulkana ' + data['version'] + ' — third-party notices', *data['acknowledgements']]
    for c in data['components']:
        n = c.get('notice')
        if not n:
            continue
        lines += ['', n['name'] + ' ' + n['version'], n['scope'], n['license'], n['copyright']]
        for key in c['texts']:
            if key not in data['texts']: fail('Missing notice text: ' + key)
    for key in data['notice_text_keys']:
        if key not in data['texts']: fail('Missing notice text: ' + key)
        lines += ['', '--- ' + key + ' ---', data['texts'][key]]
    (output / 'notices.json').write_text(encoded, encoding='utf-8')
    (output / 'THIRD-PARTY-NOTICES.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    # Preserve the enclosing package's source-access and library replacement records.
    for name in ('SOURCES.json', 'SOURCE-SHA256SUMS', 'LIBRARY-REPLACEMENT.txt'):
        if (base / name).is_file(): (output / name).write_bytes((base / name).read_bytes())
    print('Merged CUDA worker notices into', output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path, metavar='PREFIX')
    parser.add_argument('--merge-notices', type=Path, metavar='BASE_DIRECTORY')
    parser.add_argument('--output', type=Path)
    for name in ('engine', 'build', 'prefix', 'cuda-root', 'cuda-manifest', 'msvc-license'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--platform', choices=('linux', 'windows'))
    parser.add_argument('--minimum-glibc')
    args = parser.parse_args()
    try:
        if args.merge_notices:
            prefix = args.prefix or args.verify
            if not prefix or not args.output: parser.error('--merge-notices requires --verify PREFIX (or --prefix) and --output')
            merge_notices(args.merge_notices, prefix, args.output)
            return
        if args.verify:
            data = verify(args.verify)
        else:
            if not all((args.engine, args.build, args.prefix, args.cuda_root, args.platform)):
                parser.error('prepare requires --engine, --build, --prefix, --cuda-root and --platform')
            data = prepare(args)
        print(f'Validated {data["platform"]} CUDA worker bundle: {len(data["files"])} files, Vulkana {data["appVersion"]}')
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        parser.exit(1, f'Diffusion bundle: {error}\n')


if __name__ == '__main__':
    main()
