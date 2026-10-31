"""Require the exact tag revision's complete bundled cross-platform matrix."""
import json
import os
import subprocess
import time


def require(repository, commit, tag):
    for _ in range(100):
        runs = json.loads(subprocess.check_output([
            'gh', 'api', f'repos/{repository}/actions/workflows/native-cross-os.yml/runs?head_sha={commit}&per_page=100'
        ], text=True))['workflow_runs']
        candidates = [v for v in runs if v['head_sha'] == commit and v['event'] == 'push' and v['head_branch'] == tag]
        if any(v['status'] == 'completed' and v['conclusion'] == 'success' for v in candidates):
            print('Exact-tag bundled CPU cross-platform qualification passed')
            return
        if candidates and all(v['status'] == 'completed' for v in candidates):
            raise RuntimeError('Cross-platform qualification failed for the release tag')
        time.sleep(30)
    raise RuntimeError('Timed out waiting for exact-tag cross-platform qualification')


if __name__ == '__main__':
    require(*(os.environ[k] for k in ('GH_REPO', 'GITHUB_SHA', 'RELEASE_TAG')))
