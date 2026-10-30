import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from provision_native_llvm import require_ci, save_verified


class ProvisioningTests(unittest.TestCase):
    def test_wrong_download_preserves_previous_bottle(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "llvm.bottle.tar.gz"
            path.write_bytes(b"previous verified bottle")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                save_verified(io.BytesIO(b"corrupt download"), path, "0" * 64)
            self.assertEqual(path.read_bytes(), b"previous verified bottle")
            self.assertEqual(list(Path(folder).iterdir()), [path])

    def test_verified_download_is_published(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "llvm.bottle.tar.gz"
            payload = b"verified bottle"
            save_verified(io.BytesIO(payload), path, hashlib.sha256(payload).hexdigest())
            self.assertEqual(path.read_bytes(), payload)

    def test_interrupted_download_leaves_no_partial_bottle(self):
        class Interrupted:
            def read(self, size):
                raise OSError("connection lost")
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "llvm.bottle.tar.gz"
            with self.assertRaises(OSError):
                save_verified(Interrupted(), path, "0" * 64)
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_managed_install_rejects_developer_machines(self):
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}):
            with self.assertRaisesRegex(RuntimeError, "only on GitHub"):
                require_ci()


if __name__ == "__main__":
    unittest.main()
