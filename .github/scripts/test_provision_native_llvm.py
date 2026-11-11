import hashlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

from provision_native_llvm import require_ci, save_verified, install


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

    def test_dependency_replacement_precedes_pinned_clang_validation(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cellar = root / "llvm"
            mlir = cellar / "23.1.3_2/lib/cmake/mlir/MLIRConfig.cmake"
            mlir.parent.mkdir(parents=True)
            mlir.touch()
            dependency = dict(formula="z3", version="5.3.1", tag="arm64_sequoia", sha256="a"*64)
            lock = dict(llvmVersion="23.1.3", homebrewBottle=dict(version="23.1.3_2"),
                        homebrewDependencies=[dependency])
            z3, llvm = root / "z3.bottle", root / "llvm.bottle"
            with patch.dict(os.environ, {"GITHUB_ACTIONS":"true", "GITHUB_ENV":str(root/"env")}), \
                 patch("provision_native_llvm.platform.system", return_value="Darwin"), \
                 patch("provision_native_llvm.platform.machine", return_value="arm64"), \
                 patch("provision_native_llvm.download_bottle", return_value=z3) as download, \
                 patch("provision_native_llvm.subprocess.run", return_value=Mock(returncode=0)) as run, \
                 patch("provision_native_llvm.subprocess.check_output", side_effect=[str(cellar),"clang version 23.1.3"]):
                install(lock, llvm)
            download.assert_called_once_with(dependency, root)
            commands = [call.args[0] for call in run.call_args_list]
            installs = [command for command in commands if command[:2]==["brew","install"]]
            self.assertEqual([command[-1] for command in installs], [str(z3),str(llvm)])
            self.assertIn(["brew","uninstall","--force","--ignore-dependencies","z3"], commands)
            self.assertIn("KALANDRA_LLVM_ROOT=", (root/"env").read_text())

    def test_install_refuses_developer_machine_before_commands(self):
        with patch.dict(os.environ, {"GITHUB_ACTIONS":"false"}), \
             patch("provision_native_llvm.subprocess.run") as run, \
             patch("provision_native_llvm.download_bottle") as download:
            with self.assertRaises(RuntimeError):
                install({}, Path("unverified.bottle"))
            run.assert_not_called()
            download.assert_not_called()


if __name__ == "__main__":
    unittest.main()
