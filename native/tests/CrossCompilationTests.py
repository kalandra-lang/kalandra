"""Cross-link using installed host/target packages; execute only on native destination runners."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import sys
import subprocess
import tarfile

from CrossTargetTests import SOURCES

ROOT = Path(__file__).resolve().parents[2]
TRIPLES = {
    'linux': {'x86_64': 'x86_64-linux-gnu', 'arm64': 'aarch64-linux-gnu'},
    'windows': {'x86_64': 'x86_64-pc-windows-msvc', 'arm64': 'aarch64-pc-windows-msvc'},
}
SYSTEM = {'Linux': 'linux', 'Windows': 'windows'}
sys.path.insert(0, str(ROOT / 'native/packaging'))
from WindowsPlatform import pe_machine, MACHINES


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=240, **kwargs)
    assert r.returncode == 0, (command, r.returncode, r.stdout[-1000:], r.stderr[-4000:])
    return r.stdout


def extract(archive, destination):
    assert digest(archive) == archive.with_suffix('.gz.sha256').read_text().split()[0]
    destination.mkdir(parents=True)
    with tarfile.open(archive) as package:
        assert all(m.isdir() or m.isfile() for m in package.getmembers())
        package.extractall(destination, filter='data')
    prefix, = destination.iterdir()
    return prefix


def build(a):
    host = 'arm64' if platform.machine().lower() in ('aarch64', 'arm64') else 'x86_64'
    assert SYSTEM.get(platform.system()) == a.system and host != a.target
    windows = a.system == 'windows'
    suffix = '.exe' if windows else ''
    a.output.mkdir(parents=True, exist_ok=True)
    host_root = extract(a.host_archive, a.output / 'host package')
    target_root = extract(a.target_archive, a.output / 'target package')
    compiler = host_root / ('bin/kalandra' + suffix)
    clang = host_root / ('libexec/kalandra/clang++' + suffix)
    triple = TRIPLES[a.system][a.target]
    documentation = (ROOT / 'apps/website/src/content/native/cross-compilation.tiramisu').read_text()
    templates = re.findall(r'codeblock \{ language = json, """([\s\S]*?)""" \}', documentation)
    documented_profile = json.loads(templates[0])
    assert documented_profile['format'] == 1 and documented_profile['target'] == 'aarch64-unknown-linux-gnu'
    documented_source = re.findall(r'codeblock \{ language = kalandra, """([\s\S]*?)""" \}', documentation)[0]
    assert documented_source == SOURCES['calls'], 'Cross-compiled documentation source drift'
    profile = dict(format=1, target=triple, runtimePackage=str(target_root.resolve()),
                   linker=str(a.linker.absolute()))
    if windows:
        # Capture the selected destination's MSVC SDK paths explicitly. The
        # compiler child then drops ambient LIB/INCLUDE and uses this profile.
        profile.update(sysroot=os.environ['WindowsSdkDir'],
                       libraryDirectories=[v for v in os.environ['LIB'].split(';') if v])
    else:
        # Ubuntu cross-development packages form an architecture-aware / root;
        # /usr/<triple> alone is incomplete and libc scripts use absolute paths.
        profile.update(sysroot='/', gccDirectory='/usr/lib/gcc-cross/' + triple + '/13')
        profile = dict(documented_profile, **profile)
    profile_file = a.output / 'cross toolchain.json'
    profile_file.write_text(json.dumps(profile))
    # Hostile ambient toolchain hints must not replace the explicit target.
    environment = dict(os.environ, CCC_OVERRIDE_OPTIONS='+--target=' + TRIPLES[a.system][host],
                       LIBRARY_PATH='/not-a-target-library-directory')
    products = a.output / 'products'
    products.mkdir()
    cases = {name: dict(source=text, output=expected) for name, text, expected in
             [(name, text, {'calls': '1144\n', 'captures': '3000\n', 'tasks': '[2, 4, 6]\n',
                            'records': '7\n', 'floating': None}[name]) for name, text in SOURCES.items()]}
    numeric_values = {'int8': '-42', 'uint8': '255', 'int16': '-30000', 'uint16': '65535',
                      'int32': '-2000000000', 'uint32': '4294967295',
                      'int64': '-9223372036854775807', 'uint64': '18446744073709551615',
                      'float32': '-1.5', 'float64': '-1.5'}
    for numeric, value in numeric_values.items():
        cases[numeric] = dict(source=f'fn main()->{numeric}:\n  {value}\n', output=value+'\n')
    cases['bool'] = dict(source='fn main()->Bool:\n  true\n', output='true\n')
    for name, fixture, expected in (
        ('recursive-array', 'recursive-callables/array-result.k', '42\n'),
        ('owned-error', 'recursive-callables/owned-error.k', '42\n'),
        ('resumable', 'resumable-callables/recursive.k', '42\n')):
        cases[name] = dict(source=(ROOT / 'native/tests/fixtures' / fixture).read_text(), output=expected)
    cases['fault'] = dict(source='fn crash(n:int)->int:\n  1/(n-n)\nfn main()->int:\n  crash(0)\n',
                         status=86, stderr='ArithmeticFault')
    records = []
    for name, case in cases.items():
        source = a.output / (name + '.k')
        source.write_text(case['source'])
        for mode in ('-O0', '-O2'):
            binary = products / (name + mode + suffix)
            run([compiler, source, '--toolchain', profile_file, mode, '-o', binary], env=environment)
            # A second independent invocation must reproduce the same bytes.
            repeat = a.output / (name + mode + '-repeat' + suffix)
            run([compiler, source, '--toolchain', profile_file, mode, '-o', repeat], env=environment)
            assert digest(binary) == digest(repeat), (name, mode, 'non-reproducible')
            repeat.unlink()
            if windows:
                assert pe_machine(binary) == MACHINES['windows-' + a.target]
            else:
                data = binary.read_bytes()
                assert data[:6] == b'\x7fELF\x02\x01'
                assert int.from_bytes(data[18:20], 'little') == (62 if a.target == 'x86_64' else 183)
            records.append(dict(file=binary.name, sha256=digest(binary), case=case))
    # Compile the existing deep-call/cancellation/stack-safety runtime harness
    # for the destination, using the identical source shared by native tests.
    source = ROOT / 'native/tests/fixtures/call-foundation.k'
    for mode in ('-O0', '-O2'):
        ir = a.output / ('foundation' + mode + '.ll')
        run([compiler, source, '--toolchain', profile_file, '--test-entry', 'run',
             '--emit-llvm', ir, mode, '-o', a.output / ('foundation-main' + mode + suffix)], env=environment)
        binary = products / ('foundation' + mode + suffix)
        flags = ['--no-default-config', '--target=' + triple, '--sysroot=' + profile['sysroot'],
                 '--ld-path=' + profile['linker']]
        if windows:
            resources = target_root / 'lib/kalandra/toolchain/clang'
            flags += ['-resource-dir=' + str(resources), '-fms-runtime-lib=static', '--rtlib=compiler-rt',
                      '-nostdlib', '-Xlinker', '/defaultlib:libcmt', '-Xlinker', '/defaultlib:oldnames',
                      '-Xlinker', '/defaultlib:kernel32', '-Xlinker', '/Brepro']
            flags += ['-L' + directory for directory in profile['libraryDirectories']]
            for directory in os.environ['INCLUDE'].split(';'):
                if directory: flags += ['-isystem', directory]
            matches = []
            for library in resources.rglob('*builtins*.lib'):
                headers = run([a.linker.parent / 'llvm-readobj.exe', '--file-headers', library])
                machines = re.findall(r'Machine: .*\(0x([0-9A-Fa-f]+)\)', headers)
                if machines and {int(v, 16) for v in machines} == {MACHINES['windows-' + a.target]}:
                    matches.append(library)
            assert len(matches) == 1, matches
            libraries = [target_root / 'lib/kalandra/native/kalandra-native-runtime.lib', matches[0]]
        else:
            flags += ['--gcc-install-dir=' + profile['gccDirectory'], '-pthread']
            libraries = [target_root / 'lib/kalandra/native/libkalandra-native-runtime.a']
        run([clang, *flags, '-std=c++23', '-ffp-contract=off', '-Wno-override-module', mode,
             ROOT / 'native/tests/CallFoundationRuntimeTests.cpp', ir, *libraries, '-o', binary])
        records.append(dict(file=binary.name, sha256=digest(binary), case=dict(contains='passed')))
    if not windows:
        # Execute the documented compiler command with SDK paths substituted in
        # the actual profile template, then run its product on the destination.
        (a.output / 'compute.k').write_text(documented_source)
        (a.output / 'cross.json').write_text(json.dumps(profile))
        run([compiler.resolve(), 'compute.k', '--toolchain', 'cross.json', '-O2', '-o', 'program-arm64'], cwd=a.output)
        binary = products / 'documentation-O2'
        binary.write_bytes((a.output / 'program-arm64').read_bytes())
        records.append(dict(file=binary.name, sha256=digest(binary), case=dict(output='1144\n')))
    # Misconfigured targets must preserve every requested output.
    source = a.output / 'int64.k'
    protected = a.output / 'protected'
    protected.write_text('keep executable')
    protected_ir = a.output / 'protected.ll'
    protected_ir.write_text('keep LLVM')
    for change in ({'target': 'armv7-linux-gnueabihf'}, {'target': TRIPLES[a.system][host]},
                   {'sysroot': '/missing/sysroot'}, {'format': 2}, {'surprise': True}):
        profile_file.write_text(json.dumps(dict(profile, **change)))
        r = subprocess.run(list(map(str, [compiler, source, '--toolchain', profile_file,
                           '-o', protected, '--emit-llvm', protected_ir])), capture_output=True, text=True)
        assert r.returncode == 2, (change, r.returncode, r.stderr)
        assert protected.read_text() == 'keep executable' and protected_ir.read_text() == 'keep LLVM'
    profile_file.write_text(json.dumps(profile))
    # Even a self-consistent inventory must not admit runtime code for the
    # source architecture. A damaged archive must fail its original digest.
    runtime_name = 'kalandra-int64-entry-debug.lib' if windows else 'libkalandra-int64-entry-debug.a'
    library = target_root / 'lib/kalandra/native' / runtime_name
    inventory = target_root / 'SHA256SUMS'
    original, original_inventory = library.read_bytes(), inventory.read_bytes()
    try:
        old_digest = digest(library)
        library.write_bytes((host_root / 'lib/kalandra/native' / runtime_name).read_bytes())
        inventory.write_text(original_inventory.decode().replace('\r\n', '\n').replace(old_digest, digest(library)))
        r = subprocess.run(list(map(str, [compiler, source, '--toolchain', profile_file,
                           '-o', protected, '--emit-llvm', protected_ir])), capture_output=True, text=True)
        assert r.returncode == 2 and 'object ABI mismatch' in r.stderr, (r.returncode, r.stderr)
        assert protected.read_text() == 'keep executable' and protected_ir.read_text() == 'keep LLVM'
        library.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        inventory.write_bytes(original_inventory)
        r = subprocess.run(list(map(str, [compiler, source, '--toolchain', profile_file,
                           '-o', protected, '--emit-llvm', protected_ir])), capture_output=True, text=True)
        assert r.returncode == 2 and 'checksum mismatch' in r.stderr, (r.returncode, r.stderr)
        assert protected.read_text() == 'keep executable' and protected_ir.read_text() == 'keep LLVM'
    finally:
        library.write_bytes(original)
        inventory.write_bytes(original_inventory)
    (products / 'qualification.json').write_text(json.dumps(dict(
        sourceSystem=a.system, destinationSystem=a.system, sourceArchitecture=host, destinationArchitecture=a.target, compilerSHA256=digest(compiler),
        hostArchiveSHA256=digest(a.host_archive), targetArchiveSHA256=digest(a.target_archive),
        linker=run([a.linker, '--version']).strip(), clang=run([clang, '--version']).strip(),
        toolchain=profile, sdkVersions={k: os.environ[k] for k in ('VCToolsVersion', 'WindowsSDKVersion') if k in os.environ},
        records=records), indent=2) + '\n')
    print(f'{host} -> {a.target}: {len(records)} reproducible target programs prepared; not executed on source host')


def execute(a):
    host = 'arm64' if platform.machine().lower() in ('aarch64', 'arm64') else 'x86_64'
    report = json.loads((a.output / 'qualification.json').read_text())
    assert SYSTEM.get(platform.system()) == report['destinationSystem'] and report['destinationArchitecture'] == host
    for record in report['records']:
        binary = (a.output / record['file']).resolve()
        assert digest(binary) == record['sha256']
        binary.chmod(0o755)
        r = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
        case = record['case']
        assert r.returncode == case.get('status', 0), (record['file'], r.returncode, r.stdout, r.stderr)
        if case.get('stderr'):
            assert case['stderr'] in r.stderr and not r.stdout
        else:
            assert not r.stderr, (record['file'], r.stderr)
            if case.get('contains'):
                assert case['contains'] in r.stdout, r.stdout
            elif case['output'] is None:
                expected = 1.0
                for _ in range(100): expected = expected * 1.000000001 + 0.25
                assert math.isclose(float(r.stdout), expected, rel_tol=1e-13, abs_tol=1e-13)
            else:
                assert r.stdout == case['output'], (record['file'], r.stdout, case['output'])
    print(f"{report['sourceArchitecture']} -> {host}: {len(report['records'])} cross-built programs passed on native destination hardware")


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['build', 'execute'])
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--system', choices=TRIPLES)
    p.add_argument('--target', choices=['x86_64', 'arm64'])
    p.add_argument('--host-archive', type=Path)
    p.add_argument('--target-archive', type=Path)
    p.add_argument('--linker', type=Path)
    a = p.parse_args()
    build(a) if a.mode == 'build' else execute(a)
