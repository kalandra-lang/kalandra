"""Combine qualified CPU runtime data with each host's standalone compiler.

Destination executables, dynamic libraries and system SDKs are never copied.
The host tools and numerical runtime archive bytes are preserved unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile
import tempfile

from CreateArchive import create_archive

ROOT = Path(__file__).resolve().parents[2]
PLATFORMS = ('linux-x86_64', 'linux-arm64', 'windows-x86_64', 'windows-arm64', 'macos-arm64')
META = Path('share/kalandra-native-tools/toolchain.json')
TARGETS = Path('lib/kalandra/targets')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(prefix):
    files = sorted(p for p in prefix.rglob('*') if p.is_file() and p != prefix / 'SHA256SUMS')
    (prefix / 'SHA256SUMS').write_text(''.join(digest(p) + '  ' + p.relative_to(prefix).as_posix() + '\n' for p in files))


def verify(prefix):
    seen = set()
    for line in (prefix / 'SHA256SUMS').read_text().splitlines():
        checksum, name = line.split('  ', 1)
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or name in seen:
            raise ValueError('Unsafe or duplicate runtime inventory path')
        seen.add(name)
        file = prefix / path
        if file.is_symlink() or not file.is_file() or digest(file) != checksum:
            raise ValueError('Runtime bundle checksum mismatch: ' + name)
    actual = {p.relative_to(prefix).as_posix() for p in prefix.rglob('*') if p.is_file() and p != prefix / 'SHA256SUMS'}
    if seen != actual: raise ValueError('Runtime bundle contains unrecorded files')


def extract(archive, destination):
    checksum = archive.with_name(archive.name + '.sha256')
    if not checksum.is_file() or checksum.read_text().split()[0] != digest(archive):
        raise ValueError('Source archive checksum mismatch: ' + str(archive))
    # Containment checks compare resolved members with this prefix. Canonicalize
    # the root too (notably macOS /var -> /private/var and caller-owned aliases).
    destination = destination.resolve()
    destination.mkdir(parents=True)
    with tarfile.open(archive) as package:
        if not all(v.isdir() or v.isfile() for v in package.getmembers()):
            raise ValueError('Only regular runtime bundle files/directories are allowed')
        package.extractall(destination, filter='data')
    prefix, = destination.iterdir()
    verify(prefix)
    return prefix


def project(source, destination, platform):
    info = json.loads((source / META).read_text())
    version = (ROOT / 'VERSION').read_text().strip()
    llvm = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())['llvmVersion']
    if info['platform'] != platform or info['version'] != version or info['llvmVersion'] != llvm:
        raise ValueError('Target runtime version/platform/LLVM mismatch')
    files = []
    for file in (source / 'lib/kalandra/native').iterdir():
        if file.name in ('libkalandra-native-runtime.a', 'kalandra-native-runtime.lib') or \
                file.name.endswith(('-entry-debug.a', '-entry-debug.lib')):
            files.append(file)
    if len(files) < 10: raise ValueError('Incomplete normal CPU runtime entry set')
    resources = source / 'lib/kalandra/toolchain/clang'
    files.extend(p for p in (resources / 'include').rglob('*') if p.is_file())
    files.extend(p for p in (resources / 'lib').rglob('*') if p.is_file() and
                 (('builtins' in p.name and p.suffix in ('.a', '.lib')) or p.name == 'libclang_rt.osx.a'))
    imports = source / 'share/kalandra-native-tools/macos-link-imports'
    if platform == 'macos-arm64':
        if not imports.is_dir(): raise ValueError('Missing SDK-free macOS import declarations')
        files.extend(p for p in imports.rglob('*') if p.is_file())
    files.extend(p for p in (source / 'share/licenses').rglob('*') if p.is_file())
    files.extend(source / name for name in ('LICENSE', 'NOTICE') if (source / name).is_file())
    for file in files:
        if file.is_symlink() or file.suffix.lower() in ('.exe', '.dll', '.so', '.dylib'):
            raise ValueError('Destination runtime data must not contain executable tools or dynamic libraries')
        target = destination / file.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(file, target)
    (destination / 'lib/kalandra/toolchain/clang/lib').mkdir(parents=True, exist_ok=True)
    metadata = {key: info[key] for key in ('format', 'version', 'platform', 'llvmVersion')}
    metadata['package'] = 'kalandra-cpu-target-runtime'
    if platform == 'macos-arm64': metadata['minimumMacOS'] = info['minimumMacOS']
    (destination / META).parent.mkdir(parents=True, exist_ok=True)
    (destination / META).write_text(json.dumps(metadata, indent=2) + '\n')
    inventory(destination)


def assemble(assets, output, host=None):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='runtime bundle ', dir=output) as work:
        root = Path(work)
        runtimes = root / 'runtimes'
        sources = {}
        for platform in PLATFORMS:
            archive = assets / f'kalandra-native-cpu-standalone-{platform}.tar.gz'
            source = extract(archive, root / ('source-' + platform))
            project(source, runtimes / platform, platform)
            sources[platform] = source
        common = root / 'kalandra-cpu-target-runtimes'
        shutil.copytree(runtimes, common / TARGETS)
        inventory(common)
        common_archive = output / 'kalandra-cpu-target-runtimes.tar.gz'
        create_archive(common, common_archive)
        common_archive.with_name(common_archive.name + '.sha256').write_text(digest(common_archive) + '  ' + common_archive.name + '\n')
        inputs = [(assets / f'kalandra-native-cpu-standalone-{platform}.tar.gz', sources[platform]) for platform in PLATFORMS]
        metal = assets / 'kalandra-native-metal-standalone-macos-arm64.tar.gz'
        if metal.is_file(): inputs.append((metal, extract(metal, root / 'source-metal')))
        for archive, source in inputs:
            if host and json.loads((source / META).read_text())["platform"] != host: continue
            prefix = root / 'host' / source.name
            shutil.copytree(source, prefix)
            metadata = json.loads((prefix / META).read_text())
            if set(metadata.get('crossLinkers', {})) != {'elf', 'macho', 'coff'}:
                raise ValueError('Host compiler lacks its bundled cross linkers')
            for path in metadata['crossLinkers'].values():
                if not (prefix / path).is_file(): raise ValueError('Missing bundled host linker')
            shutil.copytree(runtimes, prefix / TARGETS)
            metadata['bundledCpuTargets'] = list(PLATFORMS)
            (prefix / META).write_text(json.dumps(metadata, indent=2) + '\n')
            inventory(prefix)
            result = output / archive.name
            create_archive(prefix, result)
            result.with_name(result.name + '.sha256').write_text(digest(result) + '  ' + result.name + '\n')
            shutil.rmtree(prefix.parent)
            print('Bundled five CPU runtimes: ' + result.name, flush=True)


def attach(host_archive, runtime_archive, output):
    """Add independently inventoried runtime data to an unchanged GPU host."""
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gpu runtime bundle ', dir=output) as work:
        root = Path(work)
        host = extract(host_archive, root / 'host')
        common = extract(runtime_archive, root / 'runtime-data')
        metadata = json.loads((host / META).read_text())
        version = (ROOT / 'VERSION').read_text().strip()
        llvm = json.loads((ROOT / 'native/toolchain-proof/toolchain.json').read_text())['llvmVersion']
        if (metadata['format'] != 1 or metadata['version'] != version or
                metadata['llvmVersion'] != llvm or metadata['platform'] not in PLATFORMS):
            raise ValueError('GPU host version/platform/LLVM mismatch')
        if set(metadata.get('crossLinkers', {})) != {'elf', 'macho', 'coff'}:
            raise ValueError('GPU host lacks its bundled cross linkers')
        for relative in metadata['crossLinkers'].values():
            linker = host / relative
            if not linker.resolve().is_relative_to(host) or not linker.is_file():
                raise ValueError('Missing or unsafe GPU host linker')
        runtimes = common / TARGETS
        if {path.name for path in runtimes.iterdir()} != set(PLATFORMS):
            raise ValueError('Expected exactly five CPU runtime targets')
        for platform in PLATFORMS:
            target = runtimes / platform
            verify(target)
            info = json.loads((target / META).read_text())
            if (info['format'] != 1 or info['package'] != 'kalandra-cpu-target-runtime' or info['platform'] != platform or
                    info['version'] != version or info['llvmVersion'] != llvm):
                raise ValueError('CPU target runtime version/platform/LLVM mismatch')
            if platform == 'macos-arm64' and info.get('minimumMacOS') != '26.0':
                raise ValueError('CPU target runtime macOS minimum mismatch')
            if any(path.suffix.lower() in ('.exe', '.dll', '.so', '.dylib') or '.so.' in path.name.lower()
                   for path in target.rglob('*') if path.is_file()):
                raise ValueError('CPU runtime bundle contains destination tools or dynamic libraries')
        shutil.copytree(runtimes, host / TARGETS)
        metadata['bundledCpuTargets'] = list(PLATFORMS)
        (host / META).write_text(json.dumps(metadata, indent=2) + '\n')
        inventory(host)
        result = output / host_archive.name
        create_archive(host, result)
        result.with_name(result.name + '.sha256').write_text(digest(result) + '  ' + result.name + '\n')
        print('Bundled five CPU runtimes into GPU host: ' + result.name, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--assets', type=Path)
    source.add_argument('--host-package', type=Path)
    parser.add_argument('--runtime-bundle', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--host', choices=PLATFORMS, help='Build only this host package; still include all five runtimes')
    args = parser.parse_args()
    if args.host_package:
        if not args.runtime_bundle or args.host:
            parser.error('--host-package requires --runtime-bundle and does not accept --host')
        attach(args.host_package.resolve(), args.runtime_bundle.resolve(), args.output.resolve())
    else:
        if args.runtime_bundle:
            parser.error('--runtime-bundle requires --host-package')
        assemble(args.assets.resolve(), args.output.resolve(), args.host)
