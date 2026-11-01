"""Fetch only pinned BSD-3 C++ headers into a build-local cache; install nothing."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent
PIN = json.loads((ROOT / 'upstream.json').read_text())
def sha(data):
    return hashlib.sha256(data).hexdigest()

def provision(output, archive=None):
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / 'receipt.json'
    if receipt.exists():
        saved = json.loads(receipt.read_text())
        if saved['revision'] != PIN['revision'] or saved['archiveSHA256'] != PIN['archiveSHA256']:
            raise RuntimeError('CUTLASS cache has a different source pin')
        for relative, digest in saved['files'].items():
            if sha((output / relative).read_bytes()) != digest:
                raise RuntimeError('CUTLASS cache modified: ' + relative)
        return output / 'include'
    if archive is None:
        archive = output / 'source.tar.gz'
        if not archive.exists():
            temporary = archive.with_suffix('.download')
            url = 'https://codeload.github.com/NVIDIA/cutlass/tar.gz/' + PIN['revision']
            with urllib.request.urlopen(url, timeout=120) as response, temporary.open('wb') as target:
                while data := response.read(1024 * 1024):
                    target.write(data)
            temporary.rename(archive)
    if sha(archive.read_bytes()) != PIN['archiveSHA256']:
        raise RuntimeError('CUTLASS archive hash mismatch')
    prefix = 'cutlass-' + PIN['revision']
    files = {}
    with tarfile.open(archive) as source:
        # Do not extract or execute the separately licensed Python/CuTeDSL tree.
        for member in source.getmembers():
            parts = PurePosixPath(member.name).parts
            if not parts or parts[0] != prefix or '..' in parts:
                raise RuntimeError('Invalid CUTLASS archive path')
            selected = len(parts) >= 2 and (parts[1] == 'include' or parts[1:] == ('LICENSE.txt',))
            if not selected or member.isdir():
                continue
            if not member.isfile():
                raise RuntimeError('Non-regular CUTLASS header')
            data = source.extractfile(member).read()
            if parts[1] == 'include' and b'SPDX-License-Identifier: BSD-3-Clause' not in data:
                raise RuntimeError('Unexpected C++ header license: ' + member.name)
            if parts[1:] == ('LICENSE.txt',) and sha(data) != PIN['licenseSHA256']:
                raise RuntimeError('Unexpected upstream license')
            relative = '/'.join(parts[1:])
            target = output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            files[relative] = sha(data)
    if len(files) != 839 or 'include/cutlass/gemm/kernel/default_gemm.h' not in files:
        raise RuntimeError('Incomplete pinned C++ header tree')
    receipt.write_text(json.dumps(dict(revision=PIN['revision'], archiveSHA256=PIN['archiveSHA256'], files=files), sort_keys=True) + '\n')
    return output / 'include'

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--archive', type=Path)
    p.add_argument('--audit-only', action='store_true')
    a = p.parse_args()
    if a.audit_only and not (a.output / 'receipt.json').is_file():
        raise RuntimeError('Build the pinned catalog before auditing its header cache')
    print(provision(a.output, a.archive))
