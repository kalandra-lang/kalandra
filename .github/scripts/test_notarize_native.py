import os
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

from notarize_native import main, private_command
from BundleMachO import digest
from CreateArchive import create_archive
from SignDistribution import extract_verified


class CredentialTests(unittest.TestCase):
    def test_local_machine_is_rejected_before_keychain_access(self):
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}), patch("subprocess.run") as run:
            with self.assertRaisesRegex(RuntimeError, "only be imported"):
                main()
            run.assert_not_called()

    def test_credential_command_failure_does_not_echo_arguments_or_output(self):
        with patch("subprocess.run") as run:
            run.return_value.returncode = 1
            run.return_value.stderr = b"secret-value"
            with self.assertRaises(RuntimeError) as error:
                private_command("/usr/bin/security", "import", "secret-value")
            self.assertNotIn("secret-value", str(error.exception))

    def test_timeout_does_not_echo_credential_arguments(self):
        with patch("subprocess.run", side_effect=subprocess.TimeoutExpired(
                ["security", "secret-value"], 180)):
            with self.assertRaises(RuntimeError) as error:
                private_command("/usr/bin/security", "secret-value")
            self.assertNotIn("secret-value", str(error.exception))

    def test_notary_diagnostic_reports_only_http_status(self):
        with patch("subprocess.run") as run:
            run.return_value.returncode = 1
            run.return_value.stdout = b"secret-value"
            run.return_value.stderr = b"Error: HTTP status code: 401. secret-value"
            with self.assertRaisesRegex(RuntimeError, "HTTP 401") as error:
                private_command("/usr/bin/xcrun", "notarytool", "secret-value")
            self.assertNotIn("secret-value", str(error.exception))

    def test_agreement_failure_is_not_reported_as_bad_team_membership(self):
        with patch("subprocess.run") as run:
            run.return_value.returncode = 1
            run.return_value.stdout = b""
            run.return_value.stderr = b"HTTP status code: 403. A required agreement is missing or has expired. secret-value"
            with self.assertRaisesRegex(RuntimeError, "agreement is missing or expired") as error:
                private_command("/usr/bin/xcrun", "notarytool", "secret-value")
            self.assertNotIn("secret-value", str(error.exception))


class SigningInputTests(unittest.TestCase):
    def fixture(self, root, incorrect_inventory=False):
        prefix = root / "kalandra-native-cpu-standalone"
        prefix.mkdir()
        tool = prefix / "tool"
        tool.write_bytes(b"fixture")
        checksum = "0" * 64 if incorrect_inventory else digest(tool)
        (prefix / "SHA256SUMS").write_text(f"{checksum}  tool\n")
        archive = root / "input.tar.gz"
        create_archive(prefix, archive)
        archive.with_suffix(".gz.sha256").write_text(digest(archive) + "\n")
        return archive

    def test_valid_inventory_extracts(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            prefix = extract_verified(self.fixture(root), root / "extracted")
            self.assertEqual((prefix / "tool").read_bytes(), b"fixture")

    def test_changed_input_fails_before_signing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = self.fixture(root)
            archive.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                extract_verified(archive, root / "extracted")

    def test_wrong_extracted_inventory_fails_before_signing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with self.assertRaisesRegex(ValueError, "inventory mismatch"):
                extract_verified(self.fixture(root, True), root / "extracted")
