"""Regression checks for foreign-host device resource admission."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('portable_resources', Path(__file__).with_name('portable_resources.py'))
resources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(resources)


class ResourceAdmission(unittest.TestCase):
    def test_roundtrip_and_reject_modified_payload_source_inventory(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build, bundle = root/'build', root/'bundle'
            for name in resources.expected():
                path = build/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'bytecode fixture\0'+name.encode())
            def invoke(*args):
                with patch.object(sys, 'argv', ['resources', '--directory', str(bundle), *args]), contextlib.redirect_stdout(io.StringIO()):
                    resources.main()
            with patch.object(resources, 'source_hash', return_value='source-a'):
                invoke('--export', str(build))
                invoke()
                payload = bundle/resources.expected()[0]
                original = payload.read_bytes()
                payload.write_bytes(original+b'changed')
                with self.assertRaisesRegex(RuntimeError, 'Invalid device resource'):
                    invoke()
                payload.write_bytes(original)
                extra = bundle/'driver.dll'
                extra.write_bytes(b'not device bytecode')
                with self.assertRaisesRegex(RuntimeError, 'Unexpected file'):
                    invoke()
                extra.unlink()
                manifest = bundle/'manifest.json'
                contents = json.loads(manifest.read_text())
                contents['sourceSHA256'] = 'source-b'
                manifest.write_text(json.dumps(contents))
                with self.assertRaisesRegex(RuntimeError, 'current source tree'):
                    invoke()


if __name__ == '__main__':
    unittest.main()
