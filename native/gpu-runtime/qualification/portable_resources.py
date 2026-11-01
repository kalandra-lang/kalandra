"""Transfer same-source device bytecode between build hosts; never host executables.

PTX/SPIR-V are architecture-independent device inputs. Source and payload hashes
are checked before CMake consumes them; GPU execution remains a separate gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def source_hash():
    # Git's canonical blobs give the same source identity for LF and Windows
    # autocrlf checkouts. First reject real worktree changes; index-only hashing
    # must never bless bytecode built from modified inputs.
    subprocess.run(['git', 'diff', '--quiet', '--exit-code', '--', 'native', 'CMakeLists.txt'],
                   cwd=ROOT, check=True)
    entries = subprocess.check_output(['git', 'ls-files', '--stage', '-z', '--', 'native', 'CMakeLists.txt'], cwd=ROOT)
    return hashlib.sha256(entries).hexdigest()



def expected():
    types = ('int8', 'uint8', 'int16', 'uint16', 'int32', 'uint32', 'int64', 'uint64', 'float32', 'float64')
    transforms = ['transpose-'+t for t in types] + ['softmaxRows-float32', 'rmsNormRows-float32']
    return ([f'cuda/transforms/{n}.ptx' for n in transforms] +
            [f'cuda/kernels/{n}.ptx' for n in ('matmul-warp64', 'matmul-warp128', 'matmul-catalog')] +
            [f'vulkan/transforms/{n}.spv' for n in transforms] + ['vulkan/add.spv', 'vulkan/mul_mm.spv'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--export', dest='build', type=Path)
    p.add_argument('--directory', type=Path, required=True)
    a = p.parse_args()
    root = a.directory.resolve()
    if a.build:
        root.mkdir(parents=True, exist_ok=False)
        for name in expected():
            target = root/name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(a.build/name, target)
        manifest = {'format': 1, 'sourceSHA256': source_hash(), 'gpuExecution': False,
                    'files': {n: hashlib.sha256((root/n).read_bytes()).hexdigest() for n in expected()}}
        (root/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    manifest = json.loads((root/'manifest.json').read_text())
    if manifest['format'] != 1 or manifest['sourceSHA256'] != source_hash():
        raise RuntimeError('Device resources do not belong to the current source tree')
    if set(manifest['files']) != set(expected()):
        raise RuntimeError('Incomplete or unexpected device resource inventory')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    if actual != set(expected()) | {'manifest.json'}:
        raise RuntimeError('Unexpected file in device-only resource bundle')
    for name, digest in manifest['files'].items():
        path = root/name
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Invalid device resource: '+name)
    print('Verified same-source device bytecode; no host tools, SDK libraries or GPU qualification')


if __name__ == '__main__':
    main()
