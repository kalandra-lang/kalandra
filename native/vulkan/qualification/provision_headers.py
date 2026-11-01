"""Fetch verified public Vulkan headers into a private qualification directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--directory', type=Path, required=True)
a = p.parse_args()
lock = json.loads(Path(__file__).with_name('vulkan-headers.json').read_text())
root = a.directory.resolve()
root.mkdir(parents=True, exist_ok=True)
archive = root/(lock['commit']+'.tar.gz')
if not archive.exists():
    pending = archive.with_suffix('.download')
    request = urllib.request.Request(lock['url'], headers={'User-Agent': 'Kalandra-Vulkan-qualification'})
    with urllib.request.urlopen(request, timeout=120) as response, pending.open('wb') as output:
        while block := response.read(1024 * 1024):
            output.write(block)
    with pending.open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != lock['sha256']:
            raise RuntimeError('Vulkan header archive checksum mismatch')
    pending.replace(archive)
with archive.open('rb') as source:
    if hashlib.file_digest(source, 'sha256').hexdigest() != lock['sha256']:
        raise RuntimeError('Cached Vulkan header archive checksum mismatch')
name = 'Vulkan-Headers-'+lock['commit']
destination = root/name
if destination.exists():
    raise RuntimeError('Choose a fresh qualification directory; existing headers are never overwritten')
with tarfile.open(archive) as package:
    for member in package.getmembers():
        parts = Path(member.name).parts
        if (not (member.isdir() or member.isfile()) or not parts or parts[0] != name or
                '..' in parts or Path(member.name).is_absolute()):
            raise RuntimeError('Unsafe Vulkan header archive member')
    package.extractall(root, filter='data')
assert (destination/'include/vulkan/vulkan.h').is_file()
assert (destination/'LICENSE.md').is_file() and (destination/'LICENSES').is_dir()
if 'GITHUB_ENV' in os.environ:
    with Path(os.environ['GITHUB_ENV']).open('a') as environment:
        environment.write('KALANDRA_VULKAN_HEADERS_ROOT='+str(destination)+'\n')
print(destination, flush=True)
