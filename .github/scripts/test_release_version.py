import unittest
from release_version import classify, project_version, validate_project_tag, release_notes

class VersionTests(unittest.TestCase):
    def test_stable(self):
        for tag in ("v0.1.0", "v1.2.3", "v12.34.56+build.01"):
            with self.subTest(tag=tag):
                self.assertEqual(classify(tag)["prerelease"], "false")
                self.assertEqual(classify(tag)["version"], tag[1:])

    def test_prerelease(self):
        for suffix in ("alpha", "alpha.1", "beta.2", "rc.1", "preview.5", "dev", "0", "rc.1+build.9"):
            with self.subTest(suffix=suffix):
                self.assertEqual(classify("v1.2.3-" + suffix)["prerelease"], "true")

    def test_reject(self):
        for tag in ("1.2.3", "v1.2", "v01.2.3", "v1.02.3", "v1.2.03", "v1.2.3-01", "v1.2.3-rc.01",
                    "v1.2.3-", "v1.2.3+", "v1.2.3-rc..1", "v1.2.3\n", "v1.2.3;echo hi", "v1.2.3-α"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                classify(tag)


    def test_project_tag_must_match_exact_version(self):
        self.assertEqual(validate_project_tag("v0.99.0", "0.99.0")["version"], "0.99.0")
        for tag in ("v0.98.0", "v0.99.0-rc.1", "v0.99.0+build.1"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                validate_project_tag(tag, "0.99.0")
        self.assertEqual(validate_project_tag("v0.99.0-rc.1", "0.99.0-rc.1")["prerelease"], "true")

    def test_project_version_file(self):
        from pathlib import Path
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "VERSION").write_text("0.99.0\n")
            self.assertEqual(project_version(root), "0.99.0")
            (root / "VERSION").write_text("0.99.0-01\n")
            with self.assertRaises(ValueError):
                project_version(root)

    def test_release_notes_required_for_exact_version(self):
        from pathlib import Path
        import tempfile
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            notes = root / "docs/releases/0.99.0.md"
            with self.assertRaises(FileNotFoundError):
                release_notes("0.99.0", root)
            notes.parent.mkdir(parents=True)
            for text in ("", "# Kalandra 0.98.0\n", "# Kalandra 0.99.01\n"):
                notes.write_text(text)
                with self.assertRaises(ValueError):
                    release_notes("0.99.0", root)
            notes.write_text("# Kalandra 0.99.0 — Native revival\n", encoding="utf-8")
            self.assertEqual(release_notes("0.99.0", root), notes.read_text())


class CMakeVersionTests(unittest.TestCase):
    def test_cmake_and_release_parser_agree(self):
        from pathlib import Path
        import shutil
        import subprocess
        import tempfile
        cmake = shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is not installed")
        module = Path(__file__).resolve().parents[2] / "native/cmake/KalandraVersion.cmake"
        with tempfile.TemporaryDirectory(prefix="kalandra version ") as temporary:
            root = Path(temporary)
            copied = root / "native/cmake/KalandraVersion.cmake"
            copied.parent.mkdir(parents=True)
            shutil.copyfile(module, copied)
            script = root / "check.cmake"
            script.write_text(f'include("{copied.as_posix()}")\nmessage(STATUS "${{KALANDRA_VERSION}}|${{KALANDRA_VERSION_CORE}}")\n')
            for version in ("0.99.0", "0.99.0-rc.1", "0.99.0+build.01", "12.34.56-alpha.1+build.01", "0.99.0-01", "0.99.0-rc.01", "01.2.3", "0.99", "v0.99.0", "0.99.0+", "0.99.0-", "0.99.0\nextra"):
                (root / "VERSION").write_text(version + "\n")
                try:
                    classify("v" + version)
                    valid = True
                except ValueError:
                    valid = False
                result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
                with self.subTest(version=version):
                    self.assertEqual(result.returncode == 0, valid, result.stderr)
                    if valid:
                        self.assertIn(version + "|" + version.split("-")[0].split("+")[0], result.stdout)


class PublishTests(unittest.TestCase):
    def exercise(self, tag, fail=False):
        import os
        from pathlib import Path
        import runpy
        import subprocess
        import tempfile
        from unittest.mock import patch
        script = Path(__file__).with_name("publish_native_release.py").resolve()
        with tempfile.TemporaryDirectory() as temporary:
            previous = Path.cwd()
            os.chdir(temporary)
            try:
                from test_native_release_assets import write_fixture
                write_fixture(Path.cwd())
                with patch.dict(os.environ, {"RELEASE_TAG": tag, "RELEASE_PRERELEASE": classify(tag)["prerelease"]}), patch("release_version.project_version", return_value=classify(tag)["version"]), patch("release_version.release_notes", return_value="# Kalandra test\nNative CPU/GPU release.\n"), patch("subprocess.run") as run:
                    if fail:
                        run.side_effect = subprocess.CalledProcessError(1, "gh")
                        with self.assertRaises(subprocess.CalledProcessError):
                            runpy.run_path(str(script))
                        self.assertEqual(run.call_count, 1)
                    else:
                        runpy.run_path(str(script))
                        create, edit = [call.args[0] for call in run.call_args_list]
                        self.assertIn("--draft", create)
                        self.assertIn("--verify-tag", create)
                        self.assertIn("Native CPU/GPU release.", create[create.index("--notes") + 1])
                        self.assertEqual("--prerelease" in create, classify(tag)["prerelease"] == "true")
                        self.assertIn("--latest=" + str(classify(tag)["prerelease"] == "false").lower(), edit)
            finally:
                os.chdir(previous)

    def test_mismatched_tag_never_publishes(self):
        import os
        import runpy
        from pathlib import Path
        from unittest.mock import patch
        script = Path(__file__).with_name("publish_native_release.py").resolve()
        with patch.dict(os.environ, {"RELEASE_TAG": "v0.98.0"}), patch("subprocess.run") as run:
            with self.assertRaises(ValueError):
                runpy.run_path(str(script))
            run.assert_not_called()

    def test_stable_publication(self):
        self.exercise("v1.2.3")

    def test_prerelease_publication(self):
        self.exercise("v1.2.3-rc.1")

    def test_failed_creation_never_edits_release(self):
        self.exercise("v1.2.3", fail=True)

if __name__ == "__main__":
    unittest.main()
