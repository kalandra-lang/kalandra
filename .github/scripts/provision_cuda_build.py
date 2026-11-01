"""Fetch two hash-pinned CUDA build dependencies into ephemeral hosted CI only.

No installer, driver, system PATH, package-manager or global configuration change.
These SDK files are build inputs, never Kalandra package contents.
Source: NVIDIA CUDA 13.2.2 redistrib manifest (SHA256 below).
"""
import hashlib
import json
import os
from pathlib import Path
import tarfile
import urllib.request

BASE = 'https://developer.download.nvidia.com/compute/cuda/redist/'
MANIFEST_SHA256 = '89d93e6de640d47eb43c63437fe1be426ed21dd58cc79933a29acbca33c6eeb8'
PACKAGES = {
    'cuda_cudart': ('d6be2b3546fa93e5dec9e3d118d31f9540d0b7af5319ee3c6726f202dfc5b5d4', 1558760),
    'cuda_nvrtc': ('6e0661e31a43c22ad030637adeb881822249ffb2a14a19691828f117f5ab2de9', 60704976),
}


def provision():
    if os.environ.get('GITHUB_ACTIONS') != 'true' or not os.environ.get('RUNNER_TEMP'):
        raise RuntimeError('This provisioner is restricted to ephemeral hosted CI')
    root = Path(os.environ['RUNNER_TEMP']) / 'kalandra-cuda-build'
    root.mkdir(parents=True, exist_ok=False)
    manifest = urllib.request.urlopen(BASE+'redistrib_13.2.2.json', timeout=90).read()
    if hashlib.sha256(manifest).hexdigest() != MANIFEST_SHA256:
        raise RuntimeError('CUDA redistribution manifest hash changed')
    (root/'redistrib.json').write_bytes(manifest)
    entries = json.loads(manifest)
    paths = {}
    for name, (expected, size) in PACKAGES.items():
        entry = entries[name]['linux-x86_64']
        if entry['sha256'] != expected or int(entry['size']) != size:
            raise RuntimeError('CUDA dependency does not match the reviewed pin')
        relative = f'{name}/linux-x86_64/{name}-linux-x86_64-13.2.86-archive.tar.xz'
        if entry['relative_path'] != relative:
            raise RuntimeError('Unexpected CUDA dependency URL')
        archive = root/(name+'.tar.xz')
        with urllib.request.urlopen(BASE+relative, timeout=180) as response:
            archive.write_bytes(response.read(size+1))
        if archive.stat().st_size != size or hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
            raise RuntimeError('CUDA dependency checksum mismatch')
        with tarfile.open(archive) as tar:
            prefix = f'{name}-linux-x86_64-13.2.86-archive'
            if any(Path(m.name).parts[0] != prefix or '..' in Path(m.name).parts for m in tar.getmembers()):
                raise RuntimeError('Unsafe CUDA archive paths')
            tar.extractall(root, filter='data')
        paths[name] = root/prefix
    headers = paths['cuda_cudart']/'include'
    nvrtc = paths['cuda_nvrtc']/'lib/libnvrtc.so'
    if not (headers/'cuda.h').is_file() or not nvrtc.is_file():
        raise RuntimeError('CUDA build dependencies incomplete')
    with open(os.environ['GITHUB_ENV'], 'a') as environment:
        environment.write(f'KALANDRA_CUDA_HEADERS={headers}\nKALANDRA_CUDA_NVRTC={nvrtc}\n')
    print('Verified CUDA 13.2.86 headers and NVRTC; isolated build inputs only')


if __name__ == '__main__':
    provision()
