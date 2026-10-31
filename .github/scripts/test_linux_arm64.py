"""Guard architecture selection and reject mixed-architecture release packages."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "native/packaging"))
from BundleELF import check_machine, expected_machine, PLATFORM
from provision_native_platform import archive_key


class LinuxArchitectureTests(unittest.TestCase):
    def test_archive_selection(self):
        for machine in ("aarch64", "arm64", "ARM64"):
            self.assertEqual(archive_key("Linux", machine), "linux-arm64")
        self.assertEqual(archive_key("Linux", "x86_64"), "linux-x64")
        self.assertEqual(archive_key("Windows", "AMD64"), "windows-x64")
        for system, machine in (("Linux", "armv7l"), ("Windows", "arm64"), ("Darwin", "arm64")):
            with self.assertRaises(RuntimeError):
                archive_key(system, machine)

    def test_elf_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            metadata = root / "share/kalandra-native-tools/toolchain.json"
            metadata.parent.mkdir(parents=True)
            metadata.write_text(json.dumps({"platform": "linux-arm64"}))
            machine = expected_machine(root)
            path = root / "program"
            for actual in (62, 183):
                path.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + actual.to_bytes(2, "little"))
                if actual == machine:
                    check_machine(path, machine)
                else:
                    with self.assertRaisesRegex(RuntimeError, "Wrong ELF architecture"):
                        check_machine(path, machine)
            path.write_bytes(b"short")
            with self.assertRaises(RuntimeError):
                check_machine(path, machine)
            metadata.write_text(json.dumps({"platform": "linux-armv7"}))
            with self.assertRaises(RuntimeError):
                expected_machine(root)

    def test_system_loaders(self):
        self.assertTrue(PLATFORM.fullmatch("ld-linux-aarch64.so.1"))
        self.assertTrue(PLATFORM.fullmatch("ld-linux-x86-64.so.2"))
        self.assertFalse(PLATFORM.fullmatch("libLLVM.so.23.1"))


if __name__ == "__main__":
    unittest.main()
