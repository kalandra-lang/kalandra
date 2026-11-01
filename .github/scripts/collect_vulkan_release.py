"""Attach qualified same-revision Vulkan code to the release's CPU runtime data."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "native/packaging"))
from collect_native_platforms import api
from RuntimeBundles import attach, digest

ARCHIVE = 'kalandra-native-vulkan-standalone-linux-x86_64.tar.gz'
ARTIFACT = 'native-vulkan-package-linux-x86_64'


def qualified_artifact(repository, commit, rehearsal=False):
    if not rehearsal and not os.environ.get("RELEASE_TAG"):
        raise RuntimeError("Vulkan publication requires an exact release tag")
    deadline = time.monotonic() + 3600
    while True:
        runs = api(f'repos/{repository}/actions/workflows/native-vulkan.yml/runs?head_sha={commit}&per_page=100')['workflow_runs']
        candidates = [run for run in runs if run['head_sha'] == commit and
                      (run.get('head_repository') or {}).get('full_name') == repository and
                      ((rehearsal and run['event'] in ('pull_request', 'push', 'workflow_dispatch')) or
                       (not rehearsal and run['event'] == 'push' and
                        run['head_branch'] == os.environ.get('RELEASE_TAG')))]
        for run in candidates:
            if run['status'] != 'completed' or run['conclusion'] != 'success':
                continue
            artifacts = api(f"repos/{repository}/actions/runs/{run['id']}/artifacts?per_page=100")['artifacts']
            available = [item for item in artifacts if item['name'] == ARTIFACT and not item['expired']]
            if len(available) == 1:
                return run, available[0]
        if not any(run['status'] != 'completed' for run in candidates) or time.monotonic() >= deadline:
            raise RuntimeError('No successful Vulkan qualification for the exact release tag/revision')
        print('Waiting for same-revision Vulkan qualification', flush=True)
        time.sleep(30)


def collect(runtime_archive, destination, rehearsal=False):
    repository, commit = os.environ['GH_REPO'], os.environ['GITHUB_SHA']
    run, artifact = qualified_artifact(repository, commit, rehearsal)
    with tempfile.TemporaryDirectory(prefix='vulkan release ') as folder:
        root = Path(folder)
        with tempfile.TemporaryFile() as download:
            subprocess.run(['gh', 'api', f"repos/{repository}/actions/artifacts/{artifact['id']}/zip"],
                           stdout=download, check=True)
            download.seek(0)
            with zipfile.ZipFile(download) as package:
                members = package.infolist()
                if len(members) != 2 or {member.filename for member in members} != {ARCHIVE, ARCHIVE+'.sha256'}:
                    raise RuntimeError('Vulkan artifact has an unexpected file set')
                for member in members:
                    maximum = 1024 if member.filename.endswith('.sha256') else 2 * 1024**3
                    if member.is_dir() or member.file_size > maximum:
                        raise RuntimeError('Vulkan artifact exceeds its file contract')
                    with package.open(member) as source, (root/member.filename).open('xb') as output:
                        shutil.copyfileobj(source, output)
        archive = root/ARCHIVE
        if archive.with_name(ARCHIVE+'.sha256').read_text().split() != [digest(archive), ARCHIVE]:
            raise RuntimeError('Vulkan artifact checksum mismatch')
        # Attach validates inventories, version/LLVM, all five CPU destinations,
        # macOS minimum, owned linkers and exclusion of destination SDK libraries.
        first, repeated = root/'first', root/'repeated'
        attach(archive, runtime_archive, first)
        attach(archive, runtime_archive, repeated)
        if digest(first/ARCHIVE) != digest(repeated/ARCHIVE):
            raise RuntimeError('Vulkan runtime composition is not reproducible')
        for name in (ARCHIVE, ARCHIVE+'.sha256'):
            with (first/name).open('rb') as source, (destination/name).open('xb') as output:
                shutil.copyfileobj(source, output)
    receipt = {'commit': commit, 'run': run['html_url'], 'artifactId': artifact['id'],
               'platform': 'linux-x86_64', 'archive': ARCHIVE}
    Path('vulkan-qualification.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print(json.dumps(receipt), flush=True)
