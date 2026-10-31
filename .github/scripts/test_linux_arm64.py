"""Guard architecture selection and reject mixed-architecture release packages."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import qualify_raspberry_pi_os as pi_os

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "native/packaging"))
from BundleELF import check_machine, expected_machine, PLATFORM
from provision_native_platform import archive_key


def fixture_elf(machine, alignment=65536, address=65536):
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    header[18:20] = machine.to_bytes(2, "little")
    header[32:40] = (64).to_bytes(8, "little")
    header[54:56] = (56).to_bytes(2, "little")
    header[56:58] = (1).to_bytes(2, "little")
    segment = bytearray(56)
    segment[:4] = (1).to_bytes(4, "little")
    segment[16:24] = address.to_bytes(8, "little")
    segment[48:56] = alignment.to_bytes(8, "little")
    return header + segment


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
                path.write_bytes(fixture_elf(actual))
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

    def test_pi_16k_load_alignment(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "program"
            for alignment, address in ((4096, 65536), (65536, 4096)):
                path.write_bytes(fixture_elf(183, alignment, address))
                with self.assertRaisesRegex(RuntimeError, "16 KiB"):
                    check_machine(path, 183)
            for alignment in (16384, 65536):
                path.write_bytes(fixture_elf(183, alignment))
                check_machine(path, 183)
            path.write_bytes(fixture_elf(183)[:70])
            with self.assertRaisesRegex(RuntimeError, "Truncated"):
                check_machine(path, 183)

    def test_pi_image_preparation_rejects_other_hosts(self):
        for actions, system, machine in (("false", "Linux", "aarch64"),
                                         ("true", "Darwin", "arm64"),
                                         ("true", "Linux", "x86_64")):
            with patch.dict(pi_os.os.environ, GITHUB_ACTIONS=actions), \
                    patch.object(pi_os.platform, "system", return_value=system), \
                    patch.object(pi_os.platform, "machine", return_value=machine), \
                    patch.object(pi_os.urllib.request, "urlopen") as download:
                with self.assertRaisesRegex(RuntimeError, "disposable native ARM64"):
                    pi_os.main()
                download.assert_not_called()

    def test_system_loaders(self):
        self.assertTrue(PLATFORM.fullmatch("ld-linux-aarch64.so.1"))
        self.assertTrue(PLATFORM.fullmatch("ld-linux-x86-64.so.2"))
        self.assertFalse(PLATFORM.fullmatch("libLLVM.so.23.1"))


if __name__ == "__main__":
    unittest.main()
