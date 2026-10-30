"""Publish qualified assets; reruns never overwrite an existing release."""
import os
import subprocess
from release_version import validate_project_tag, release_notes
from native_release_assets import validate_assets

version = validate_project_tag(os.environ["RELEASE_TAG"])
assert version["prerelease"] == os.environ["RELEASE_PRERELEASE"]
assets = validate_assets()
notes = release_notes(version["version"])
args = ["gh", "release", "create", version["tag"], "--verify-tag", "--draft", "--title", f"Kalandra {version['version']}", "--notes", notes]
if version["prerelease"] == "true":
    args.append("--prerelease")
subprocess.run([*args, *map(str, assets)], check=True)
# A failed upload leaves a draft for inspection. Existing releases are never edited
# unless this invocation successfully created the draft above.
subprocess.run(["gh", "release", "edit", version["tag"], "--draft=false",
                "--latest=" + ("false" if version["prerelease"] == "true" else "true")], check=True)
