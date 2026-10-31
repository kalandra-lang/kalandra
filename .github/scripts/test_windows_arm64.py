"""Reject translated/mixed Windows packages and preserve explicit SDK limits."""
import json
import io
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'native/packaging'))
sys.path.insert(0, str(ROOT / 'native/tests'))
from WindowsPlatform import MACHINES, pe_machine, has_sanitizers, rename_after_process_exit
from BundlePE import imports, audit
from OrdinaryTestEnvironment import qualified_variants
from provision_native_platform import archive_key, windows_triplet
import qualify_windows_arm64_editor as editor


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

    def test_editor_download_requires_disposable_arm64_windows(self):
        for actions, system, machine in (("false", "Windows", "ARM64"),
                                         ("true", "Darwin", "arm64"),
                                         ("true", "Windows", "AMD64")):
            with patch.dict(editor.os.environ, GITHUB_ACTIONS=actions), \
                    patch.object(editor.platform, 'system', return_value=system), \
                    patch.object(editor.platform, 'machine', return_value=machine), \
                    patch.object(editor.urllib.request, 'urlopen') as download:
                with self.assertRaisesRegex(RuntimeError, 'disposable native Windows ARM64'):
                    editor.main()
                download.assert_not_called()

    def test_transient_windows_lock_retries_the_real_rename(self):
        with tempfile.TemporaryDirectory() as folder:
            source, target = Path(folder) / 'source', Path(folder) / 'target'
            source.write_bytes(b'original')
            original = Path.rename
            calls = []
            def locked_once(path, destination):
                calls.append(path)
                if len(calls) == 1:
                    error = PermissionError('file busy')
                    error.winerror = 32
                    raise error
                return original(path, destination)
            with patch.object(Path, 'rename', autospec=True, side_effect=locked_once), \
                    patch('time.sleep'):
                rename_after_process_exit(source, target)
            self.assertEqual(len(calls), 2)
            self.assertFalse(source.exists())
            self.assertEqual(target.read_bytes(), b'original')
            error = PermissionError('persistent file lock')
            error.winerror = 32
            with patch.object(Path, 'rename', side_effect=error) as rename, patch('time.sleep'):
                with self.assertRaises(PermissionError):
                    rename_after_process_exit(target, source)
                self.assertEqual(rename.call_count, 6)
            self.assertEqual(target.read_bytes(), b'original')

    def test_editor_retry_keeps_original_digest_and_never_publishes_bad_bytes(self):
        payload = b"verified editor"
        lock = {"url": "https://example.invalid/editor.zip", "sha256": hashlib.sha256(payload).hexdigest()}
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'editor.zip'
            path.write_bytes(b'previous verified editor')
            with patch.object(editor.urllib.request, 'urlopen', side_effect=[
                    io.BytesIO(b'truncated'), io.BytesIO(payload)]) as download, \
                    patch.object(editor.time, 'sleep'):
                editor.download_editor(lock, path)
                self.assertEqual(download.call_count, 2)
                self.assertEqual(path.read_bytes(), payload)
            with patch.object(editor.urllib.request, 'urlopen', side_effect=[
                    io.BytesIO(b'wrong') for _ in range(3)]) as download, \
                    patch.object(editor.time, 'sleep'):
                with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                    editor.download_editor(lock, path)
                self.assertEqual(download.call_count, 3)
                self.assertEqual(path.read_bytes(), payload)

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
