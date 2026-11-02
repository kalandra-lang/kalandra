"""Collect exact-tag experimental Windows GPU packages and attach release CPU runtimes."""
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile
import zipfile

from collect_vulkan_release import qualified_artifact
from RuntimeBundles import attach, digest


def collect(runtime_archive, destination, architecture, rehearsal=False):
    if architecture not in ('x86_64', 'arm64'):
        raise ValueError('Unknown Windows GPU architecture')
    repository, commit = os.environ['GH_REPO'], os.environ['GITHUB_SHA']
    run, artifact = qualified_artifact(repository, commit, rehearsal,
                                       platform='windows-'+architecture)
    names = {backend: f'kalandra-native-{backend}-standalone-windows-{architecture}.tar.gz'
             for backend in ('cuda', 'vulkan')}
    receipts = []
    with tempfile.TemporaryDirectory(prefix='windows gpu release ') as folder:
        root = Path(folder)
        with tempfile.TemporaryFile() as download:
            subprocess.run(['gh', 'api', f"repos/{repository}/actions/artifacts/{artifact['id']}/zip"],
                           stdout=download, check=True)
            download.seek(0)
            with zipfile.ZipFile(download) as package:
                seen = set()
                for member in package.infolist():
                    path = PurePosixPath(member.filename)
                    if path.is_absolute() or '..' in path.parts or '\\' in member.filename:
                        raise RuntimeError('Unsafe Windows GPU artifact path')
                    if member.is_dir():
                        continue
                    if path.name in {*names.values(), *(name+'.sha256' for name in names.values())}:
                        output = path.name
                        maximum = 1024 if output.endswith('.sha256') else 2*1024**3
                    elif path.name == 'qualification.json' and path.parent.name in ('installed-cuda', 'installed-vulkan'):
                        output = path.parent.name+'.json'
                        maximum = 65536
                    else:
                        raise RuntimeError('Unexpected Windows GPU artifact member')
                    if output in seen or member.file_size > maximum:
                        raise RuntimeError('Duplicate or oversized Windows GPU artifact member')
                    seen.add(output)
                    with package.open(member) as source, (root/output).open('xb') as target:
                        shutil.copyfileobj(source, target)
        expected = {*names.values(), *(name+'.sha256' for name in names.values()),
                    'installed-cuda.json', 'installed-vulkan.json'}
        if seen != expected:
            raise RuntimeError('Incomplete Windows GPU artifact')
        for backend, name in names.items():
            archive = root/name
            if (root/(name+'.sha256')).read_text().split() != [digest(archive), name]:
                raise RuntimeError('Windows GPU artifact checksum mismatch')
            report = json.loads((root/f'installed-{backend}.json').read_text())
            if (report.get('backend') != backend or report.get('architecture') != architecture or
                    report.get('archiveSHA256') != digest(archive) or
                    report.get('installedCompiler') is not True or report.get('cpuExecution') is not True or
                    report.get('gpuExecution') is not False or report.get('performanceQualified') is not False or
                    report.get('bundledTargets') is not False or
                    report.get('gpuSourceBuilds') != (51 if backend == 'cuda' else 15)):
                raise RuntimeError('Windows GPU qualification report mismatch')
        for backend, name in names.items():
            first, repeated = root/('first-'+backend), root/('repeated-'+backend)
            attach(root/name, runtime_archive, first)
            attach(root/name, runtime_archive, repeated)
            if digest(first/name) != digest(repeated/name):
                raise RuntimeError('Windows GPU runtime composition is not reproducible')
        # Export only after both backends pass all input and composition gates.
        for backend, name in names.items():
            for filename in (name, name+'.sha256'):
                with (root/('first-'+backend)/filename).open('rb') as source, (destination/filename).open('xb') as target:
                    shutil.copyfileobj(source, target)
            receipts.append({'commit': commit, 'run': run['html_url'], 'artifactId': artifact['id'],
                             'platform': 'windows-'+architecture, 'archive': name,
                             'gpuExecutionQualified': False, 'performanceQualified': False})
    Path('windows-gpu-'+architecture+'-qualification.json').write_text(json.dumps(receipts, indent=2)+'\n')
    print(json.dumps(receipts), flush=True)
