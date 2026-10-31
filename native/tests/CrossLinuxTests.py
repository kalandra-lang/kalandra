"""Cross-link using installed host/target packages; execute only on native destination runners."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

from CrossTargetTests import SOURCES

ROOT = Path(__file__).resolve().parents[2]
TRIPLES = {'x86_64': 'x86_64-linux-gnu', 'arm64': 'aarch64-linux-gnu'}


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
    assert platform.system() == 'Linux' and host != a.target
    a.output.mkdir(parents=True, exist_ok=True)
    host_root = extract(a.host_archive, a.output / 'host package')
    target_root = extract(a.target_archive, a.output / 'target package')
    compiler = host_root / 'bin/kalandra'
    clang = host_root / 'libexec/kalandra/clang++'
    triple = TRIPLES[a.target]
    # Ubuntu cross-development packages form an architecture-aware / sysroot;
    # /usr/<triple> alone is not a complete root (its libc linker scripts use
    # absolute /usr/<triple>/lib paths). Select the target GCC installation explicitly.
    profile = dict(format=1, target=triple, sysroot='/',
                   runtimePackage=str(target_root.resolve()), linker=str(a.linker.resolve()),
                   gccDirectory='/usr/lib/gcc-cross/' + triple + '/13')
    profile_file = a.output / 'cross toolchain.json'
    profile_file.write_text(json.dumps(profile))
    # Hostile ambient toolchain hints must not replace the explicit target.
    environment = dict(os.environ, CCC_OVERRIDE_OPTIONS='+--target=' + TRIPLES[host],
                       LIBRARY_PATH='/not-a-target-library-directory')
    products = a.output / 'products'
    products.mkdir()
    cases = {name: dict(source=text, output=expected) for name, text, expected in
             [(name, text, {'calls': '1144\n', 'captures': '3000\n', 'tasks': '[2, 4, 6]\n',
                            'records': '7\n', 'floating': None}[name]) for name, text in SOURCES.items()]}
    for numeric in ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64'):
        cases[numeric] = dict(source=f'fn main()->{numeric}:\n  {numeric}(42)\n', output='42\n')
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
            binary = products / (name + mode)
            run([compiler, source, '--toolchain', profile_file, mode, '-o', binary], env=environment)
            # A second independent invocation must reproduce the same bytes.
            repeat = a.output / (name + mode + '-repeat')
            run([compiler, source, '--toolchain', profile_file, mode, '-o', repeat], env=environment)
            assert digest(binary) == digest(repeat), (name, mode, 'non-reproducible')
            repeat.unlink()
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
             '--emit-llvm', ir, mode, '-o', a.output / ('foundation-main' + mode)], env=environment)
        binary = products / ('foundation' + mode)
        run([clang, '--no-default-config', '--target=' + triple, '--sysroot=' + profile['sysroot'],
             '--gcc-install-dir=' + profile['gccDirectory'], '--ld-path=' + profile['linker'],
             '-std=c++23', '-ffp-contract=off', '-pthread', mode,
             ROOT / 'native/tests/CallFoundationRuntimeTests.cpp', ir,
             target_root / 'lib/kalandra/native/libkalandra-native-runtime.a', '-o', binary])
        records.append(dict(file=binary.name, sha256=digest(binary), case=dict(contains='passed')))
    # Misconfigured targets must preserve every requested output.
    source = a.output / 'int64.k'
    protected = a.output / 'protected'
    protected.write_text('keep executable')
    protected_ir = a.output / 'protected.ll'
    protected_ir.write_text('keep LLVM')
    for change in ({'target': 'armv7-linux-gnueabihf'}, {'target': TRIPLES[host]},
                   {'sysroot': '/missing/sysroot'}, {'format': 2}, {'surprise': True}):
        profile_file.write_text(json.dumps(dict(profile, **change)))
        r = subprocess.run(list(map(str, [compiler, source, '--toolchain', profile_file,
                           '-o', protected, '--emit-llvm', protected_ir])), capture_output=True, text=True)
        assert r.returncode == 2, (change, r.returncode, r.stderr)
        assert protected.read_text() == 'keep executable' and protected_ir.read_text() == 'keep LLVM'
    profile_file.write_text(json.dumps(profile))
    (products / 'qualification.json').write_text(json.dumps(dict(
        sourceArchitecture=host, destinationArchitecture=a.target, compilerSHA256=digest(compiler),
        hostArchiveSHA256=digest(a.host_archive), targetArchiveSHA256=digest(a.target_archive),
        linker=run([a.linker, '--version']).strip(), records=records), indent=2) + '\n')
    print(f'{host} -> {a.target}: {len(records)} reproducible target programs prepared; not executed on source host')


def execute(a):
    host = 'arm64' if platform.machine().lower() in ('aarch64', 'arm64') else 'x86_64'
    report = json.loads((a.output / 'qualification.json').read_text())
    assert platform.system() == 'Linux' and report['destinationArchitecture'] == host
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
    p.add_argument('--target', choices=TRIPLES)
    p.add_argument('--host-archive', type=Path)
    p.add_argument('--target-archive', type=Path)
    p.add_argument('--linker', type=Path)
    a = p.parse_args()
    build(a) if a.mode == 'build' else execute(a)
