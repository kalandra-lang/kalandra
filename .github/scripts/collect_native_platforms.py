"""Require successful same-commit platform runs and collect their exact archive assets."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import zipfile


def api(path):
    return json.loads(subprocess.check_output(["gh", "api", path], text=True))


def qualified_artifact(repository, commit, platform, architecture="x86_64"):
    artifact_name = f"native-{platform}-assets" if architecture == "x86_64" else f"native-{platform}-{architecture}-assets"
    deadline = time.monotonic() + 3600
    announced = False
    while True:
        runs = api(f"repos/{repository}/actions/workflows/native-{platform}.yml/runs?head_sha={commit}&per_page=100")["workflow_runs"]
        candidates = [run for run in runs if run["head_sha"] == commit and run["event"] == "push" and
                      run["head_branch"] in ("native", os.environ.get("RELEASE_TAG"))]
        for run in candidates:
            if run.get("status", "completed") != "completed" or run["conclusion"] != "success":
                continue
            artifacts = api(f"repos/{repository}/actions/runs/{run['id']}/artifacts?per_page=100")["artifacts"]
            available = [item for item in artifacts if item["name"] == artifact_name and not item["expired"]]
            if len(available) == 1:
                return run, available[0]
        pending = any(run.get("status", "completed") != "completed" for run in candidates)
        if not pending or time.monotonic() >= deadline:
            raise RuntimeError(f"No successful {platform} qualification for the exact release commit")
        if not announced:
            print(f"Waiting for same-commit {platform} qualification before creating a release", flush=True)
            announced = True
        time.sleep(30)


def collect():
    repository = os.environ["GH_REPO"]
    commit = os.environ["GITHUB_SHA"]
    destination = Path("release-assets")
    receipts = []
    for platform, architecture in (("linux", "x86_64"), ("linux", "arm64"), ("windows", "x86_64")):
        run, artifact = qualified_artifact(repository, commit, platform, architecture)
        archive_name = f"kalandra-native-cpu-standalone-{platform}-{architecture}.tar.gz"
        expected = {archive_name, archive_name + ".sha256"}
        with tempfile.TemporaryFile() as download:
            subprocess.run(["gh", "api", f"repos/{repository}/actions/artifacts/{artifact['id']}/zip"], stdout=download, check=True)
            download.seek(0)
            with zipfile.ZipFile(download) as package:
                members = package.infolist()
                if len(members) != 2 or {member.filename for member in members} != expected:
                    raise RuntimeError("Platform artifact has an unexpected file set")
                for member in members:
                    if member.file_size > 2 * 1024**3 or member.is_dir():
                        raise RuntimeError("Platform artifact exceeds its file contract")
                    with package.open(member) as source, (destination / member.filename).open("xb") as output:
                        import shutil
                        shutil.copyfileobj(source, output)
        receipts.append({"platform": platform, "architecture": architecture, "commit": commit, "run": run["html_url"], "artifactId": artifact["id"]})
    Path("platform-qualification.json").write_text(json.dumps(receipts, indent=2) + "\n")
    print(json.dumps(receipts), flush=True)


if __name__ == "__main__":
    collect()
