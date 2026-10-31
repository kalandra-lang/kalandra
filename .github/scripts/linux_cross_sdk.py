"""Lock Ubuntu SDK downloads on Linux; materialize them on another build host.

Only a URL/version/digest manifest and Kalandra-owned test objects are uploaded.
Vendor packages are downloaded directly from Ubuntu, never republished by CI.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import posixpath
import re
import shlex
import shutil
import subprocess
import tarfile
from urllib.parse import urlsplit
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ('libc6', 'libc6-dev', 'libgcc-s1', 'libgcc-13-dev',
            'libstdc++6', 'libstdc++-13-dev', 'gcc-13-base')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def safe_name(name):
    value = PurePosixPath(name)
    if value.is_absolute() or '..' in value.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe SDK archive path: ' + name)
    return value.as_posix()


def deb_data(data):
    if not data.startswith(b'!<arch>\n'):
        raise ValueError('Invalid Debian archive')
    offset = 8
    found = None
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:] != b'`\n':
            raise ValueError('Invalid Debian member')
        size = int(header[48:58])
        if size < 0 or offset + 60 + size > len(data):
            raise ValueError('Truncated Debian member')
        name = header[:16].decode('ascii').strip().rstrip('/')
        if name.startswith('data.tar'):
            if found is not None: raise ValueError('Duplicate Debian data archive')
            found = data[offset + 60:offset + 60 + size]
        offset += 60 + size + (size % 2)
    if found is None: raise ValueError('Missing Debian data archive')
    return found


def unpack(data, root, links):
    with tarfile.open(fileobj=io.BytesIO(deb_data(data))) as archive:
        for member in archive:
            name = safe_name(member.name)
            # This sysroot is for linking LLVM IR and project-owned C++ test
            # objects. Exclude compiler programs, headers and unused sanitizer
            # dependencies; retain package copyright notices with the inputs.
            base = PurePosixPath(name).name
            wanted = (re.fullmatch(r'(?:[Sr]?crt[0-9int].*|crt(?:begin|end).*|lib(?:gcc(?:_s|_eh)?|stdc\+\+|c(?:_nonshared)?|m(?:vec)?|pthread|dl|rt)\.(?:a|so(?:\.[0-9.]+)?)|ld-linux[^/]+)', base)
                      or name.startswith('usr/share/doc/') and base == 'copyright')
            if member.isdir() or not wanted: continue
            destination = root / name
            if member.isfile():
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.extractfile(member) as source, destination.open('wb') as output:
                    shutil.copyfileobj(source, output)
                links.pop(name, None)
            elif member.issym() or member.islnk():
                # Resolve within the SDK namespace, never on the build host.
                target = member.linkname
                if member.issym() and not target.startswith('/'):
                    target = posixpath.join(posixpath.dirname(name), target)
                target = safe_name(posixpath.normpath(target.lstrip('/')))
                links[name] = target
            else:
                raise ValueError('Unsupported SDK archive entry: ' + name)


def materialize_links(root, links):
    # Ubuntu's usr-merged libraries still have /lib names in linker scripts.
    # Materialize files rather than symlinks: no Windows symlink privilege needed.
    pending = dict(links)
    while pending:
        progress = False
        for name, target in list(pending.items()):
            source = root / target
            if target in pending or not source.is_file(): continue
            destination = root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            del pending[name]
            progress = True
        if not progress:
            raise ValueError('Unresolved SDK links: ' + repr(pending))
    for name in ('lib', 'lib64'):
        source = root / 'usr' / name
        if source.is_dir(): shutil.copytree(source, root / name, dirs_exist_ok=True)


def lock(a):
    if platform.system() != 'Linux' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('SDK locking requires a disposable native Linux CI runner')
    architecture = run('dpkg', '--print-architecture')
    if architecture not in ('amd64', 'arm64'): raise ValueError(architecture)
    triple = 'aarch64-linux-gnu' if architecture == 'arm64' else 'x86_64-linux-gnu'
    a.output.mkdir(parents=True)
    packages = []
    for name in PACKAGES:
        version = run('dpkg-query', '-W', '-f=${Version}', name + ':' + architecture)
        selector = name + ':' + architecture + '=' + version
        # APT authenticates repository metadata; request SHA256 explicitly.
        lines = run('apt-get', '-o', 'Acquire::ForceHash=SHA256', '--print-uris', 'download', selector).splitlines()
        fields, = [shlex.split(line) for line in lines if line.startswith("'")]
        url, filename, size, checksum = fields
        if not re.fullmatch(r'SHA256:[a-f0-9]{64}', checksum):
            raise ValueError('APT did not supply SHA256: ' + repr(fields))
        parsed = urlsplit(url)
        if parsed.hostname and parsed.hostname.endswith('.archive.ubuntu.com'):
            parsed = parsed._replace(netloc='archive.ubuntu.com')
        if parsed.hostname not in ('archive.ubuntu.com', 'security.ubuntu.com', 'ports.ubuntu.com'):
            raise ValueError('Unexpected Ubuntu package origin: ' + url)
        url = parsed._replace(scheme='https').geturl()
        packages.append(dict(package=name, version=version, architecture=architecture,
                             url=url, size=int(size), sha256=checksum.split(':')[1]))
    objects = {}
    for mode in ('-O0', '-O2'):
        output = a.output / ('foundation' + mode + '.o')
        subprocess.run([str(a.clang), '-std=c++23', '-ffp-contract=off', '-pthread', mode,
                        '-ffile-prefix-map=' + str(ROOT) + '=/kalandra/src',
                        '-fdebug-compilation-dir=/kalandra/build', '-c',
                        str(ROOT / 'native/tests/CallFoundationRuntimeTests.cpp'), '-o', str(output)], check=True)
        objects[mode] = dict(file=output.name, sha256=sha(output.read_bytes()))
    metadata = dict(format=1, architecture='arm64' if architecture == 'arm64' else 'x86_64',
                    triple=triple, checkoutCommit=run('git', 'rev-parse', 'HEAD'),
                    gccDirectory='usr/lib/gcc/' + triple + '/13',
                    foundationObjects=objects, packages=packages)
    (a.output / 'sdk.json').write_text(json.dumps(metadata, indent=2) + '\n')


def fetch(a):
    manifest = json.loads((a.manifest / 'sdk.json').read_text())
    if manifest['format'] != 1: raise ValueError('Unsupported SDK manifest')
    a.output.mkdir(parents=True)  # Refuse to mix an old sysroot into this one.
    links = {}
    for package in manifest['packages']:
        url = package['url']
        parsed = urlsplit(url)
        if parsed.scheme != 'https' or parsed.hostname not in ('archive.ubuntu.com', 'security.ubuntu.com', 'ports.ubuntu.com'):
            raise ValueError('Unexpected SDK URL: ' + url)
        with urlopen(url, timeout=120) as response:
            data = response.read(package['size'] + 1)
        if len(data) != package['size'] or sha(data) != package['sha256']:
            raise ValueError('Ubuntu package checksum/size mismatch: ' + package['package'])
        unpack(data, a.output, links)
    materialize_links(a.output, links)
    gcc = a.output / manifest['gccDirectory']
    for name in ('crtbeginS.o', 'libgcc.a', 'libstdc++.so'):
        if not (gcc / name).is_file(): raise ValueError('Incomplete GNU SDK: ' + name)
    for record in manifest['foundationObjects'].values():
        path = a.manifest / safe_name(record['file'])
        if sha(path.read_bytes()) != record['sha256']: raise ValueError('Conformance object checksum mismatch')
    print('Verified Ubuntu SDK for ' + manifest['triple'])


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['lock', 'fetch'])
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--manifest', type=Path)
    p.add_argument('--clang', type=Path)
    a = p.parse_args()
    lock(a) if a.mode == 'lock' else fetch(a)
