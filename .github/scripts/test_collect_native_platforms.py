import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import collect_native_platforms as collector


class QualificationGate(unittest.TestCase):
    def api(self, path):
        platform = "linux" if "linux" in path or "/runs/1/" in path else "windows"
        if "/workflows/" in path:
            return {"workflow_runs": [{"id": 1 if platform == "linux" else 2, "head_sha": "commit",
                                      "conclusion": "success", "event": "push", "head_branch": "native",
                                      "html_url": f"https://github.com/example/project/actions/runs/{platform}"}]}
        return {"artifacts": [{"id": 11, "name": "native-linux-arm64-assets", "expired": False}, {"id": 21, "name": "native-windows-arm64-assets", "expired": False}, {"id": 10 if platform == "linux" else 20,
                               "name": f"native-{platform}-assets", "expired": False}]}

    def download(self, arguments, stdout, check):
        platform = "linux" if any(f"/{n}/" in arguments[-1] for n in (10, 11)) else "windows"
        architecture = "arm64" if any(f"/{n}/" in arguments[-1] for n in (11, 21)) else "x86_64"
        with zipfile.ZipFile(stdout, "w") as archive:
            name = f"kalandra-native-cpu-standalone-{platform}-{architecture}.tar.gz"
            archive.writestr(name, b"qualified fixture")
            archive.writestr(name + ".sha256", b"fixture checksum")

    def run_in_workspace(self, function):
        with tempfile.TemporaryDirectory() as folder:
            original = Path.cwd()
            try:
                os.chdir(folder)
                Path("release-assets").mkdir()
                with patch.dict(os.environ, GH_REPO="example/project", GITHUB_SHA="commit"):
                    function()
            finally:
                os.chdir(original)

    def test_collects_only_successful_same_commit_artifacts(self):
        def check():
            with patch.object(collector, "api", side_effect=self.api), patch.object(collector.subprocess, "run", side_effect=self.download):
                collector.collect()
            self.assertEqual(len(list(Path("release-assets").iterdir())), 8)
            records = json.loads(Path("platform-qualification.json").read_text())
            self.assertEqual([record["commit"] for record in records], ["commit", "commit", "commit", "commit"])
        self.run_in_workspace(check)

    def test_waits_for_pending_same_commit_qualification(self):
        calls = 0
        def api(path):
            nonlocal calls
            response = self.api(path)
            if "/workflows/native-linux" in path:
                calls += 1
                if calls == 1:
                    response["workflow_runs"][0].update(status="in_progress", conclusion=None)
            return response
        def check():
            with patch.object(collector, "api", side_effect=api), patch.object(collector.subprocess, "run", side_effect=self.download), patch.object(collector.time, "sleep") as sleep:
                collector.collect()
                sleep.assert_called_once_with(30)
        self.run_in_workspace(check)

    def test_selected_tag_qualifies_without_a_native_branch_run(self):
        def api(path):
            response = self.api(path)
            if "workflow_runs" in response:
                response["workflow_runs"][0]["head_branch"] = "v1.2.3"
            return response
        def check():
            with patch.dict(os.environ, RELEASE_TAG="v1.2.3"), patch.object(collector, "api", side_effect=api), patch.object(collector.subprocess, "run", side_effect=self.download):
                collector.collect()
            self.assertEqual(len(list(Path("release-assets").iterdir())), 8)
        self.run_in_workspace(check)

    def test_failed_wrong_commit_and_unqualified_branch_cannot_publish(self):
        for field, value in (("conclusion", "failure"), ("head_sha", "other"),
                             ("head_branch", "develop"), ("event", "pull_request")):
            def api(path):
                response = self.api(path)
                if "workflow_runs" in response:
                    response["workflow_runs"][0][field] = value
                return response
            def check():
                with patch.object(collector, "api", side_effect=api), patch.object(collector.subprocess, "run") as download:
                    with self.assertRaisesRegex(RuntimeError, "exact release commit"):
                        collector.collect()
                    download.assert_not_called()
            self.run_in_workspace(check)

    def test_rehearsal_requires_same_repository_and_commit(self):
        for repository, commit, accepted in (("example/project", "commit", True),
                                              ("fork/project", "commit", False),
                                              ("example/project", "other", False)):
            def api(path):
                response = self.api(path)
                if "workflow_runs" in response:
                    response["workflow_runs"][0].update(event="pull_request", head_branch="feature",
                        head_sha=commit, head_repository={"full_name": repository})
                return response
            def check():
                with patch.object(collector, "api", side_effect=api), patch.object(collector.subprocess, "run", side_effect=self.download):
                    if accepted:
                        collector.collect(rehearsal=True)
                        self.assertEqual(len(list(Path("release-assets").iterdir())), 8)
                    else:
                        with self.assertRaisesRegex(RuntimeError, "exact release commit"):
                            collector.collect(rehearsal=True)
            self.run_in_workspace(check)

    def test_missing_arm64_artifact_blocks_release(self):
        for platform, staged_files in (("linux", 2), ("windows", 6)):
            def api(path):
                response = self.api(path)
                if "artifacts" in response:
                    response["artifacts"] = [a for a in response["artifacts"] if a["name"] != f"native-{platform}-arm64-assets"]
                return response
            def check():
                with patch.object(collector, "api", side_effect=api), patch.object(collector.subprocess, "run", side_effect=self.download):
                    with self.assertRaisesRegex(RuntimeError, "exact release commit"):
                        collector.collect()
                    self.assertEqual(len(list(Path("release-assets").iterdir())), staged_files)
                    self.assertFalse(Path("platform-qualification.json").exists())
            self.run_in_workspace(check)

    def test_unexpected_zip_members_are_rejected(self):
        def download(arguments, stdout, check):
            with zipfile.ZipFile(stdout, "w") as archive:
                archive.writestr("../../escape", b"unexpected")
        def check():
            with patch.object(collector, "api", side_effect=self.api), patch.object(collector.subprocess, "run", side_effect=download):
                with self.assertRaisesRegex(RuntimeError, "unexpected file set"):
                    collector.collect()
            self.assertFalse(list(Path("release-assets").iterdir()))
        self.run_in_workspace(check)


if __name__ == "__main__":
    unittest.main()
