"""Fail-closed dependency planning against real Mach-O staging copies."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

from BundleMachO import bundle, digest, inspect, run

checker, llvm = map(lambda value: Path(value).resolve(), sys.argv[1:])
sys.argv[1:] = []


class RejectionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="kalandra dependency rejection ")
        self.root = Path(self.directory.name)
        self.prefix = self.root / "stage"
        (self.prefix / "bin").mkdir(parents=True)
        self.tool = self.prefix / "bin/kalandra-check"
        self.original = digest(checker)
        shutil.copy2(checker, self.tool)
        self.load = next(load for load in inspect(checker)[0] if "libLLVM" in load)

    def tearDown(self):
        self.assertEqual(digest(checker), self.original, "modified the original build binary")
        self.directory.cleanup()

    def test_unresolved_rpath(self):
        run("/usr/bin/install_name_tool", "-change", self.load, "@rpath/missing.dylib", self.tool)
        with self.assertRaisesRegex(RuntimeError, "Unresolved load"):
            bundle(self.prefix, llvm)

    def test_unlicensed_dependency(self):
        unknown = self.root / "libunlicensed.dylib"
        # Any real Mach-O suffices: unknown upstreams must be rejected before copying.
        shutil.copy2(checker, unknown)
        run("/usr/bin/install_name_tool", "-change", self.load, unknown, self.tool)
        with self.assertRaisesRegex(RuntimeError, "no approved license"):
            bundle(self.prefix, llvm)

    def test_system_prefix_cannot_hide_external_dependency(self):
        unknown = self.root / "libunlicensed.dylib"
        shutil.copy2(checker, unknown)
        disguised = "/usr/lib/../.." + str(unknown)
        run("/usr/bin/install_name_tool", "-change", self.load, disguised, self.tool)
        with self.assertRaisesRegex(RuntimeError, "no approved license"):
            bundle(self.prefix, llvm)

    def test_basename_collision(self):
        fake = self.root / "fixture toolchain"
        fake.mkdir()
        (fake / "LICENSE.TXT").write_text("Fixture toolchain license\n")
        libraries = []
        for folder in ("first", "second"):
            library = fake / folder / "collision.dylib"
            library.parent.mkdir()
            shutil.copy2(checker, library)
            libraries.append(library)
        run("/usr/bin/install_name_tool", "-change", self.load, libraries[0],
            "-change", "/usr/lib/libc++.1.dylib", libraries[1], self.tool)
        with self.assertRaisesRegex(RuntimeError, "basename collision"):
            bundle(self.prefix, fake)


if __name__ == "__main__":
    unittest.main()
