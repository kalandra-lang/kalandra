"""Explicitly authorized, project-local test dependency; never installs packages.

Uses the official LLVM GitHub release asset digest. System Clang remains selected
explicitly when configuring Kalandra; the production toolchain lock is unchanged.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request

VERSION = '23.1.4'
NAME = f'LLVM-{VERSION}-Linux-X64.tar.zst'
SHA256 = 'efdd1dbd089b99bfcfcde7b8eb1236163f2e88df5a981cca4f783ec9cb5e0378'
URL = f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{VERSION}/{NAME}'

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        while data := source.read(8 * 1024 * 1024):
            h.update(data)
    return h.hexdigest()

def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: provision_llvm.py <explicit project-local dependencies directory>')
    destination = Path(sys.argv[1]).resolve()
    if not destination.is_relative_to(Path.home()) or destination == Path.home():
        raise SystemExit('project-local path under the current user home required')
    if not shutil.which('zstd'):
        raise SystemExit('zstd is missing; ask the machine owner before installing anything')
    destination.mkdir(parents=True, exist_ok=True)
    marker = destination / f'llvm-{VERSION}.verified'
    if marker.exists():
        root = Path(marker.read_text().strip())
        if not root.is_relative_to(destination) or not (root/'lib/cmake/mlir/MLIRConfig.cmake').is_file():
            raise SystemExit('invalid completed dependency marker')
        print(f'Reusing verified local MLIR: {root}', flush=True)
        return
    if shutil.disk_usage(destination).free < 12 * 1024**3:
        raise SystemExit('need 12 GiB free for archive and extracted development files')
    archive = destination / NAME
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + '.partial')
        if partial.exists():
            raise SystemExit('partial download exists; inspect active jobs before restarting')
        print(f'Downloading official {URL}', flush=True)
        with urllib.request.urlopen(URL, timeout=120) as response, partial.open('xb') as output:
            shutil.copyfileobj(response, output, 8 * 1024 * 1024)
        partial.rename(archive)
    if digest(archive) != SHA256:
        raise SystemExit('official LLVM archive digest mismatch; extraction refused')
    print(f'Verified SHA-256: {SHA256}', flush=True)
    unpacked = destination / f'llvm-{VERSION}'
    if unpacked.exists():
        raise SystemExit('incomplete extraction exists; inspect it before retrying')
    unpacked.mkdir()
    with subprocess.Popen(['zstd', '--long=31', '-dc', str(archive)], stdout=subprocess.PIPE) as decoder:
        with tarfile.open(fileobj=decoder.stdout, mode='r|') as package:
            package.extractall(unpacked, filter='data')
        if decoder.wait() != 0:
            raise SystemExit('archive decompression failed')
    configs = list(unpacked.rglob('MLIRConfig.cmake'))
    if len(configs) != 1:
        raise SystemExit(f'expected one MLIRConfig.cmake, found {len(configs)}')
    root = configs[0].parents[3]
    marker.write_text(str(root) + '\n')
    print(f'Local LLVM/MLIR root: {root}', flush=True)
    print('Configure with -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++', flush=True)

if __name__ == '__main__':
    main()
