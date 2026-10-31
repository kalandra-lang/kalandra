"""Archive inspection must distinguish COFF variants and reject damaged objects."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'native/packaging'))
from WindowsPlatform import coff_archive_machines


def member(name, contents):
    header = (name.ljust(16) + '0'.ljust(12) + '0'.ljust(6) + '0'.ljust(6) +
              '100644'.ljust(8) + str(len(contents)).ljust(10) + '`\n').encode()
    return header + contents + (b'\n' if len(contents) & 1 else b'')


class CoffArchives(unittest.TestCase):
    def inspect(self, contents):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'runtime.lib'
            path.write_bytes(contents)
            return coff_archive_machines(path)

    def test_regular_bigobj_and_mixed_architectures(self):
        x64 = struct.pack('<H', 0x8664) + bytes(18)
        arm = b'\0\0\xff\xff' + struct.pack('<HH', 2, 0xaa64) + bytes(48)
        index = member('/', b'index') + member('//', b'long filename/\n')
        self.assertEqual(self.inspect(b'!<arch>\n' + index + member('normal.obj/', x64)), {0x8664})
        self.assertEqual(self.inspect(b'!<arch>\n' + member('/0', arm)), {0xaa64})
        self.assertEqual(self.inspect(b'!<arch>\n' + member('one/', x64) + member('two/', arm)), {0x8664, 0xaa64})

    def test_truncated_empty_and_unsupported_archives(self):
        valid = b'!<arch>\n' + member('one/', struct.pack('<H', 0x8664) + bytes(18))
        invalid = [b'!<thin>\n', b'!<arch>\n', valid[:-1], valid[:30],
                   b'!<arch>\n' + member('one/', b'short'),
                   b'!<arch>\n' + member('one/', struct.pack('<H', 0x14c) + bytes(18))]
        for contents in invalid:
            with self.subTest(contents=contents), self.assertRaises(ValueError): self.inspect(contents)
