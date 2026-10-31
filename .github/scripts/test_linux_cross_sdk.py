import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from linux_cross_sdk import deb_data, unpack, materialize_links, safe_name


def deb(entries):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w:xz') as archive:
        for name, content, link in entries:
            member = tarfile.TarInfo(name)
            if link:
                member.type, member.linkname = tarfile.SYMTYPE, content
                archive.addfile(member)
            else:
                payload = content.encode()
                member.size = len(payload)
                archive.addfile(member, io.BytesIO(payload))
    data = stream.getvalue()
    header = (f'{"data.tar.xz/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n').encode()
    return b'!<arch>\n' + header + data + (b'\n' if len(data) % 2 else b'')


class LinuxSDKTests(unittest.TestCase):
    def test_materializes_cross_package_links_and_usr_merge(self):
        with tempfile.TemporaryDirectory() as work:
            root, links = Path(work), {}
            unpack(deb([('usr/lib/gcc/x86_64-linux-gnu/13/libstdc++.so',
                         '../../../x86_64-linux-gnu/libstdc++.so.6', True)]), root, links)
            unpack(deb([('usr/lib/x86_64-linux-gnu/libstdc++.so.6', 'ELF', False),
                        ('usr/bin/unwanted', 'do not install', False)]), root, links)
            materialize_links(root, links)
            self.assertEqual((root / 'usr/lib/gcc/x86_64-linux-gnu/13/libstdc++.so').read_text(), 'ELF')
            self.assertEqual((root / 'lib/x86_64-linux-gnu/libstdc++.so.6').read_text(), 'ELF')
            self.assertFalse((root / 'usr/bin/unwanted').exists())
            self.assertFalse(any(p.is_symlink() for p in root.rglob('*')))

    def test_archive_traversal_and_unresolved_links_fail(self):
        for name in ('../escape', '/usr/lib/libc.so', 'C:/escape', 'usr\\escape'):
            with self.subTest(name=name), self.assertRaises(ValueError): safe_name(name)
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaises(ValueError):
                unpack(deb([('usr/lib/libc.so', '../../../escape', True)]), Path(work), {})
            with self.assertRaises(ValueError):
                materialize_links(Path(work), {'usr/lib/libc.so': 'missing'})

    def test_truncated_or_non_debian_input_fails(self):
        for data in (b'bad', deb([('usr/lib/libc.so', 'ELF', False)])[:-50]):
            with self.subTest(), self.assertRaises(ValueError): deb_data(data)


if __name__ == '__main__': unittest.main()
