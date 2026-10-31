"""Exchange SDK link inputs using receiver-owned ephemeral encryption keys.

Only public keys and authenticated ciphertext enter GitHub Actions artifacts.
Private keys, decrypted SDK libraries and plaintext archives stay inside jobs.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/packaging'))
from CreateArchive import create_archive

RECEIVERS = ('linux-x86_64', 'linux-arm64', 'windows-x86_64', 'windows-arm64', 'macos-arm64')


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True))


def wait_download(name, destination, seconds=4500):
    repo, run = os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_RUN_ID']
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        artifacts = api(f'repos/{repo}/actions/runs/{run}/artifacts?per_page=100')['artifacts']
        if any(v['name'] == name and not v['expired'] for v in artifacts):
            subprocess.run(['gh', 'run', 'download', run, '--repo', repo, '--name', name,
                            '--dir', str(destination)], check=True)
            return
        if name.startswith('sdk-receiver-'):
            producers = {'CPU cross-build from ' + name.removeprefix('sdk-receiver-'),
                         'packages', 'macos-package', 'runtime-bundle'}
        elif name.startswith('sdk-sealed-'):
            architecture = name.removeprefix('sdk-sealed-').split('-', 1)[0]
            producers = {'Encrypted Windows SDK ' + architecture}
        else:
            raise ValueError('Unknown SDK artifact protocol name: ' + name)
        jobs = api(f'repos/{repo}/actions/runs/{run}/jobs?per_page=100')['jobs']
        failed = [v['name'] for v in jobs if v['name'] in producers and
                  v['status'] == 'completed' and v['conclusion'] != 'success']
        if failed:
            raise RuntimeError('SDK producer failed before publishing ' + name + ': ' + ', '.join(failed))
        time.sleep(45)
    raise RuntimeError('Timed out waiting for CI transfer artifact ' + name)


def send(a):
    if platform.system() != 'Windows' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('SDK send requires native Windows CI')
    from windows_sdk_snapshot import prepare
    a.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='private SDK snapshot ') as folder:
        private = Path(folder)
        snapshot = private / 'sdk'
        prepare(snapshot)
        archive = private / 'sdk.tar.gz'
        create_archive(snapshot, archive)
        for receiver in ((a.receiver,) if a.receiver else RECEIVERS):
            public = private / receiver
            wait_download('sdk-receiver-' + receiver, public)
            destination = a.output / receiver
            destination.mkdir()
            subprocess.run([str(a.tool), 'seal', str(public / 'sdk-public.pem'), str(archive),
                            str(destination / 'sdk.sealed')], check=True)
    print('Only recipient-encrypted SDK payloads remain in the artifact directory')


def receive(a):
    if os.environ.get('GITHUB_ACTIONS') != 'true': raise RuntimeError('CI transfer only')
    a.output.mkdir(parents=True)
    for architecture in ('x86_64', 'arm64'):
        encrypted = a.output / ('encrypted-' + architecture)
        wait_download('sdk-sealed-' + architecture + '-' + a.receiver, encrypted)
        # Plaintext archive is private and removed immediately after extraction.
        with tempfile.TemporaryDirectory(prefix='private SDK receive ') as folder:
            archive = Path(folder) / 'sdk.tar.gz'
            subprocess.run([str(a.tool), 'open', str(a.key), str(encrypted / 'sdk.sealed'), str(archive)], check=True)
            target = a.output / architecture
            target.mkdir()
            with tarfile.open(archive) as package:
                members = package.getmembers()
                if not all(v.isdir() or v.isfile() for v in members): raise RuntimeError('Unexpected SDK archive entry')
                package.extractall(target, filter='data')
    print('SDK link inputs decrypted only inside the current receiver job')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('mode', choices=['send', 'receive'])
    p.add_argument('--tool', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--key', type=Path)
    p.add_argument('--receiver', choices=RECEIVERS)
    a = p.parse_args()
    a.tool = a.tool.resolve()
    send(a) if a.mode == 'send' else receive(a)
