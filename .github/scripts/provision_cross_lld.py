"""Build a host-native LLD from the existing pinned LLVM source on macOS.

The pinned Homebrew LLVM installation supplies Clang/MLIR but omits LLD. Keep
this optional cross linker in a separate build directory; never replace the
system linker or modify the installed compiler package.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen

from provision_native_llvm import save_verified, sha256

ROOT = Path(__file__).resolve().parents[2]


def provision(directory, llvm):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise RuntimeError('This provisioner supports macOS ARM64 build hosts')
    lock = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())
    version, source = lock['llvmVersion'], lock['mlirSource']
    reported = subprocess.check_output([str(llvm / 'bin/clang++'), '--version'], text=True)
    match = re.search(r'\bclang version ([0-9.]+)', reported)
    if not match or match[1] != version:
        raise RuntimeError('Cross linker requires the pinned LLVM SDK')
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / source['name']
    if not archive.is_file() or sha256(archive) != source['sha256']:
        url = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{version}/{source['name']}"
        with urlopen(url, timeout=120) as response:
            save_verified(response, archive, source['sha256'])
    sources = directory / ('source-' + source['sha256'][:12])
    if not sources.exists():
        with tempfile.TemporaryDirectory(dir=directory) as staging:
            with tarfile.open(archive) as package:
                package.extractall(staging, filter='data')
            candidate, = Path(staging).iterdir()
            if not (candidate / 'lld/CMakeLists.txt').is_file():
                raise RuntimeError('LLVM source archive has no LLD project')
            candidate.rename(sources)
    build = directory / ('build-' + source['sha256'][:12])
    subprocess.run(['cmake', '-S', str(sources / 'lld'), '-B', str(build), '-G', 'Ninja',
                    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0', '-DLLVM_DIR=' + str(llvm / 'lib/cmake/llvm'),
                    '-DLLVM_INCLUDE_TESTS=OFF', '-DLLVM_INCLUDE_BENCHMARKS=OFF'], check=True)
    subprocess.run(['cmake', '--build', str(build), '--target', 'lld', '-j2'], check=True)
    binary = build / 'bin/lld'
    for name in ('ld.lld', 'ld64.lld', 'lld-link'):
        alias = binary.with_name(name)
        if alias.is_symlink():
            if alias.resolve() != binary.resolve(): raise RuntimeError('Unexpected linker alias: ' + str(alias))
        elif alias.exists():
            # LLVM may create copied/hard-linked driver aliases at build time.
            if not alias.is_file() or sha256(alias) != sha256(binary):
                raise RuntimeError('Unexpected linker file: ' + str(alias))
        else:
            alias.symlink_to(binary.name)
        reported = subprocess.check_output([str(alias), '--version'], text=True)
        if version not in reported: raise RuntimeError('Cross linker version mismatch: ' + reported)
    if os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a') as environment:
            environment.write(f'KALANDRA_CROSS_LLD_ROOT={build / "bin"}\n')
    return build / 'bin'


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--directory', type=Path, required=True)
    p.add_argument('--llvm', type=Path, required=True)
    a = p.parse_args()
    print('Verified host cross linkers: ' + str(provision(a.directory.resolve(), a.llvm.resolve())))
