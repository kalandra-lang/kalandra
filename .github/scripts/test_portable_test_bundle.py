"""The downloaded-package harness must work after the checkout is hidden."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "native/packaging"))
from PortablePackageTests import prepare_tests


class IsolatedHarnessTests(unittest.TestCase):
    def test_version_and_protocol_helpers_relocate_together(self):
        with tempfile.TemporaryDirectory() as folder:
            tests = Path(folder) / "unrelated" / "tests"
            tests.parent.mkdir()
            prepare_tests(ROOT, tests)
            self.assertEqual(
                {p.name for p in tests.iterdir()},
                {"PortableCPUAcceptance.py", "VersionCheck.py", "WindowsPlatform.py", "expected-version.txt",
                 "ProtocolClient.py", "ProtocolTests.py", "RecoveryProtocolTests.py"})
            result = subprocess.run(
                [sys.executable, "-I", "-c",
                 "import sys,json;sys.path.insert(0,sys.argv[1]);"
                 "from VersionCheck import expected_version;"
                 "from ProtocolClient import expected_server_version;"
                 "print(json.dumps([expected_version(),expected_server_version()]))", str(tests)],
                cwd=folder, capture_output=True, text=True, check=True)
            version = (ROOT / "VERSION").read_text().strip()
            self.assertEqual(json.loads(result.stdout), [version, version])
