"""Produce link-only macOS interfaces from Kalandra's own runtime imports.

No Apple SDK files, headers, libraries or implementations are copied. Every
listed name must resolve through the destination OS's system-library exports.
The accompanying C++ test objects are compiled from this repository on a Mac.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LIBRARIES = {'libSystem': '/usr/lib/libSystem.B.dylib', 'libc++': '/usr/lib/libc++.1.dylib'}
MATH = ('acos', 'asin', 'atan', 'atan2', 'ceil', 'copysign', 'cos', 'cosh', 'exp', 'exp2',
        'expm1', 'fabs', 'fdim', 'floor', 'fma', 'fmax', 'fmin', 'fmod', 'hypot', 'ldexp',
        'log', 'log10', 'log1p', 'log2', 'nearbyint', 'pow', 'remainder', 'rint', 'round',
        'sin', 'sinh', 'sqrt', 'tan', 'tanh', 'trunc')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    return subprocess.check_output(list(map(str, command)), text=True).strip()


def external_symbols(text):
    defined, undefined = set(), set()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) > 1 and len(fields[1]) == 1:
            (undefined if fields[1] == 'U' else defined).add(fields[0])
    # These refer to generated user code or linker-synthesized section limits.
    # They must never become fake operating-system imports.
    return {s for s in undefined - defined if not s.startswith(
        ('__mlir_ciface_', '_kalandra_', 'section$')) and s not in ('___dso_handle', '__mh_execute_header')}


def classify(symbols, resolver):
    result = {key: [] for key in LIBRARIES}
    for symbol in sorted(symbols):
        if not symbol.startswith('_'):
            raise ValueError('Unexpected Mach-O import: ' + symbol)
        for library in LIBRARIES:
            if resolver(library, symbol[1:]):
                result[library].append(symbol)
                break
        else:
            raise ValueError('Import is not exported by the supported OS libraries: ' + symbol)
    return result


def produce(a):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise RuntimeError('Import validation requires native macOS ARM64')
    os_version = platform.mac_ver()[0]
    if os.environ.get('GITHUB_ACTIONS') == 'true' and os_version.split('.')[0] != '26':
        raise RuntimeError('CI must validate the macOS 26 support baseline')
    metadata = json.loads((a.package / 'share/kalandra-native-tools/toolchain.json').read_text())
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    if (metadata['platform'] != 'macos-arm64' or metadata['version'] != (ROOT / 'VERSION').read_text().strip()
            or metadata['llvmVersion'] != lock['llvmVersion']):
        raise ValueError('Import interfaces require a matching macOS runtime package')
    sums_file = a.package / 'SHA256SUMS'
    if not sums_file.is_file() and not a.staged:
        raise ValueError('Runtime package inventory is required')
    inventory = (dict((line.split('  ', 1)[1], line.split('  ', 1)[0])
                      for line in sums_file.read_text().splitlines()) if sums_file.is_file() else None)
    archives = sorted(p for p in (a.package / 'lib/kalandra/native').glob('*.a')
                      if p.name == 'libkalandra-native-runtime.a' or p.name.endswith('-entry-debug.a'))
    if not archives: raise ValueError('Missing normal CPU runtime archives')
    archive_hashes = {}
    for archive in archives:
        name, checksum = archive.relative_to(a.package).as_posix(), digest(archive)
        if inventory is not None and inventory.get(name) != checksum: raise ValueError('Runtime checksum mismatch: ' + name)
        archive_hashes[name] = checksum
    a.output.mkdir(parents=True)
    objects = {}
    for mode in (() if a.interfaces_only else ('-O0', '-O2')):
        obj = a.output / ('foundation' + mode + '.o')
        subprocess.run([str(a.clang), '--no-default-config', '--target=arm64-apple-macosx26.0.0',
                        '-mmacosx-version-min=26.0', '-std=c++23', '-ffp-contract=off', mode,
                        '-ffile-prefix-map=' + str(ROOT) + '=/kalandra/src',
                        '-fdebug-compilation-dir=/kalandra/build', '-c',
                        str(ROOT / 'native/tests/CallFoundationRuntimeTests.cpp'), '-o', str(obj)], check=True)
        objects[mode] = dict(file=obj.name, sha256=digest(obj))
    symbols = external_symbols(run([a.nm, '--format=posix', *archives,
                                    *(a.output / v['file'] for v in objects.values())]))
    # LLVM may emit standard C memory/math calls directly from user code.
    symbols.update('_' + name for name in ('memset', 'memmove', 'memcmp', 'memcpy'))
    for name in MATH: symbols.update(('_' + name, '_' + name + 'f'))
    handles = {key: ctypes.CDLL(path) for key, path in LIBRARIES.items()}
    def resolves(library, name):
        try: getattr(handles[library], name)
        except AttributeError: return False
        return True
    imports = classify(symbols, resolves)
    sysroot = a.output / 'interfaces'
    (sysroot / 'usr/lib').mkdir(parents=True)
    for key, install in LIBRARIES.items():
        text = ('--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64-macos ]\ninstall-name: ' + json.dumps(install)
                + '\ncurrent-version: 1.0.0\ncompatibility-version: 1.0.0\nexports:\n'
                + '  - targets: [ arm64-macos ]\n    symbols: ' + json.dumps(imports[key]) + '\n...\n')
        (sysroot / 'usr/lib' / (key + '.tbd')).write_text(text)
    (sysroot / 'README.txt').write_text(
        'Kalandra link-only interfaces for macOS ARM64. Generated from the Kalandra runtime import set '
        'and standard C math/memory names; validated against macOS system-library exports. '
        'Contains no Apple SDK files, headers or library implementations.\n')
    commit = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True)
    result = dict(format=1, platform='macos-arm64', minimumMacOS='26.0', validatedOnMacOS=os_version,
                  checkoutCommit=commit.stdout.strip() if commit.returncode == 0 else None, toolchain=metadata,
                  clang=run([a.clang, '--version']), runtimeArchives=archive_hashes,
                  foundationObjects=objects, imports={LIBRARIES[k]: v for k, v in imports.items()})
    (a.output / 'imports.json').write_text(json.dumps(result, indent=2) + '\n')
    sums = {p.relative_to(a.output).as_posix(): digest(p) for p in sorted(a.output.rglob('*')) if p.is_file()}
    (a.output / 'SHA256SUMS').write_text(''.join(v + '  ' + k + '\n' for k, v in sums.items()))
    print(f'Validated {sum(map(len, imports.values()))} OS imports on macOS {os_version}; no Apple SDK copied')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--package', type=Path, required=True)
    p.add_argument('--clang', type=Path, required=True)
    p.add_argument('--nm', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--interfaces-only', action='store_true', help='Package only link metadata, without test objects')
    p.add_argument('--staged', action='store_true', help='Packaging owns this fresh staging tree before its inventory is written')
    produce(p.parse_args())
