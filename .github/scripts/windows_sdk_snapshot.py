"""Prepare CI-only MSVC link inputs from the installed Windows SDK.

The plaintext output must never be uploaded as an artifact or shipped in a
Kalandra package. windows_sdk_exchange.py encrypts it for individual CI jobs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(output):
    if platform.system() != 'Windows' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('SDK snapshots are restricted to disposable Windows CI')
    machine = platform.machine().lower()
    if machine not in ('amd64', 'x86_64', 'arm64', 'aarch64'): raise RuntimeError('Unsupported native architecture')
    architecture = 'arm64' if machine in ('arm64', 'aarch64') else 'x86_64'
    triple = ('aarch64' if architecture == 'arm64' else 'x86_64') + '-pc-windows-msvc'
    llvm = Path(os.environ['KALANDRA_LLVM_ROOT'])
    clang, readobj = llvm / 'bin/clang++.exe', llvm / 'bin/llvm-readobj.exe'
    directories = [Path(v) for v in os.environ['LIB'].split(';') if v]
    candidates = {}
    for directory in directories:
        for file in directory.glob('*.lib'):
            candidates.setdefault(file.name.lower(), file)
    output.mkdir(parents=True)
    libraries = output / 'lib'
    libraries.mkdir()
    pending = {'libcmt.lib', 'libcpmt.lib', 'libucrt.lib', 'libvcruntime.lib', 'oldnames.lib', 'kernel32.lib'}
    for optional in ('legacy_stdio_definitions.lib', 'synchronization.lib', 'uuid.lib', 'ntdll.lib'):
        if optional in candidates: pending.add(optional)
    copied = set()
    while pending:
        name = pending.pop()
        if name in copied: continue
        if name not in candidates: raise RuntimeError('Selected SDK lacks required library ' + name)
        source = candidates[name]
        shutil.copyfile(source, libraries / name)
        copied.add(name)
        directives = subprocess.check_output([str(readobj), '--coff-directives', str(source)], text=True)
        for a, b in re.findall(r'[-/]defaultlib:(?:"([^"]+)"|([^\s]+))', directives, re.IGNORECASE):
            dependency = (a or b).strip().lower()
            if not dependency.endswith('.lib'): dependency += '.lib'
            if Path(dependency).name != dependency: raise RuntimeError('Unexpected SDK dependency path')
            pending.add(dependency)
    objects = {}
    for mode in ('-O0', '-O2'):
        path = output / ('foundation' + mode + '.obj')
        subprocess.run([str(clang), '--no-default-config', '--target=' + triple, '-std=c++23',
                        '-ffp-contract=off', '-fms-runtime-lib=static', mode,
                        '-ffile-prefix-map=' + str(ROOT) + '=/kalandra/src',
                        '-c', str(ROOT / 'native/tests/CallFoundationRuntimeTests.cpp'), '-o', str(path)], check=True)
        objects[mode] = dict(file=path.name, sha256=digest(path))
    manifest = dict(format=1, platform='windows-' + architecture, triple=triple,
                    checkoutCommit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
                    foundationObjects=objects, libraries=sorted(copied),
                    versions={k: os.environ[k] for k in ('VCToolsVersion', 'WindowsSDKVersion')},
                    clang=subprocess.check_output([str(clang), '--version'], text=True))
    (output / 'sdk.json').write_text(json.dumps(manifest, indent=2) + '\n')
    sums = {p.relative_to(output).as_posix(): digest(p) for p in sorted(output.rglob('*')) if p.is_file()}
    (output / 'SHA256SUMS').write_text(''.join(v+'  '+k+'\n' for k, v in sums.items()))
    print(f'Prepared {len(copied)} {architecture} SDK link libraries for encrypted CI transfer only')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    prepare(p.parse_args().output)
