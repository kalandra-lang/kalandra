"""Reject translated/mixed Windows packages and preserve explicit SDK limits."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/packaging'))
sys.path.insert(0, str(ROOT / 'native/tests'))
from WindowsPlatform import MACHINES, pe_machine, has_sanitizers
from BundlePE import imports, audit
from OrdinaryTestEnvironment import qualified_variants
from provision_native_platform import archive_key, windows_triplet


def fixture(machine):
    data = bytearray(64 + 24 + 240)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 64)
    data[64:68] = b'PE\0\0'
    struct.pack_into('<H', data, 68, machine)
    struct.pack_into('<H', data, 84, 240)
    struct.pack_into('<H', data, 88, 0x20b)
    return data


class WindowsArm64Tests(unittest.TestCase):
    def test_sdk_and_dependency_architectures(self):
        self.assertEqual(archive_key('Windows', 'ARM64'), 'windows-arm64')
        for machine, expected in (('ARM64', 'arm64-windows-static'), ('AMD64', 'x64-windows-static')):
            with patch('provision_native_platform.platform.machine', return_value=machine):
                self.assertEqual(windows_triplet(), expected)

    def test_pe_architecture_and_sanitizers(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'compiler.exe'
            for machine in MACHINES.values():
                path.write_bytes(fixture(machine))
                self.assertEqual(pe_machine(path), machine)
                self.assertEqual(has_sanitizers(path), machine == 0x8664)
                self.assertEqual(imports(path, machine), [])
                with self.assertRaisesRegex(ValueError, 'Wrong PE architecture'):
                    imports(path, 0x8664 if machine == 0xaa64 else 0xaa64)
            for machine in (0xa641, 0x14c):
                path.write_bytes(fixture(machine))
                with self.assertRaisesRegex(ValueError, 'Unsupported native Windows'):
                    pe_machine(path)
            path.write_bytes(b'MZ')
            with self.assertRaises(ValueError):
                pe_machine(path)

    def test_package_cannot_mix_arm64_and_x64(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            metadata = root / 'share/kalandra-native-tools/toolchain.json'
            metadata.parent.mkdir(parents=True)
            metadata.write_text(json.dumps({'platform': 'windows-arm64'}))
            (root / 'compiler.exe').write_bytes(fixture(0xaa64))
            audit(root)
            (root / 'dependency.dll').write_bytes(fixture(0x8664))
            with self.assertRaisesRegex(ValueError, 'Wrong PE architecture'):
                audit(root)

    def test_only_arm64_excludes_missing_sanitizer_modes(self):
        variants = [('debug',), ('optimized',), ('sanitized',), ('thread-sanitized',)]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'compiler.exe'
            for machine, expected in ((0x8664, variants), (0xaa64, variants[:2])):
                path.write_bytes(fixture(machine))
                with patch('OrdinaryTestEnvironment.sys.platform', 'win32'):
                    self.assertEqual(qualified_variants(variants, path), expected)


if __name__ == '__main__':
    unittest.main()
