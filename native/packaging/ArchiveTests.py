"""Archive portability, metadata normalization and atomic failure regression tests."""
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest

from CreateArchive import create_archive


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kalandra reproducible archive ")
        self.root = Path(self.temporary.name)
        self.stage = self.root / "first stage/bundle"
        (self.stage / "bin").mkdir(parents=True)
        (self.stage / "bin/tool").write_bytes(b"executable fixture\n")
        (self.stage / "bin/tool").chmod(0o755)
        (self.stage / "long unicode filename λ.txt").write_text("content\n")

    def tearDown(self):
        self.temporary.cleanup()

    def test_equivalent_stages_have_identical_archives(self):
        other = self.root / "other stage/bundle"
        shutil.copytree(self.stage, other)
        import os
        for path in other.rglob("*"):
            os.utime(path, (123456789, 123456789))
            if path.is_file():
                path.chmod(0o700 if path.name == "tool" else 0o444)
        first, second = self.root / "first.gz", self.root / "second.gz"
        create_archive(self.stage, first, 42)
        create_archive(other, second, 42)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(int.from_bytes(first.read_bytes()[4:8], "little"), 42)
        with tarfile.open(first) as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.uname, member.gname, member.mtime),
                                 (0, 0, "", "", 42))
                self.assertEqual(member.mode, 0o755 if member.isdir() or member.name.endswith("/tool") else 0o644)
            self.assertEqual(archive.extractfile("bundle/long unicode filename λ.txt").read(), b"content\n")

    def test_epoch_changes_header_without_mutating_source(self):
        before = (self.stage / "bin/tool").stat()
        first, second = self.root / "first.gz", self.root / "second.gz"
        create_archive(self.stage, first, 0)
        create_archive(self.stage, second, 1)
        self.assertNotEqual(first.read_bytes(), second.read_bytes())
        after = (self.stage / "bin/tool").stat()
        self.assertEqual((before.st_mtime_ns, before.st_mode), (after.st_mtime_ns, after.st_mode))

    def test_invalid_input_preserves_previous_archive(self):
        output = self.root / "archive.gz"
        output.write_bytes(b"previous archive")
        with self.assertRaises(ValueError):
            create_archive(self.stage, output, -1)
        (self.stage / "escape").symlink_to(self.root)
        with self.assertRaises(ValueError):
            create_archive(self.stage, output)
        self.assertEqual(output.read_bytes(), b"previous archive")
        self.assertEqual({p.name for p in self.root.iterdir()}, {"first stage", "archive.gz"})


if __name__ == "__main__":
    unittest.main()
