import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import call, patch
from urllib.error import HTTPError

from linux_cross_sdk import deb_data, download_package, unpack, materialize_links, safe_name, ubuntu_url


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
    def test_hosted_apt_mirror_route_becomes_a_portable_upstream_url(self):
        uri = 'mirror+file:/etc/apt/apt-mirrors.txt/pool/main/g/glibc/libc6_2.39-0ubuntu8.9_amd64.deb'
        self.assertEqual(ubuntu_url(uri, 'amd64'),
                         'https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/libc6_2.39-0ubuntu8.9_amd64.deb')
        self.assertEqual(ubuntu_url('http://azure.archive.ubuntu.com/ubuntu/pool/main/g/gcc.deb', 'amd64'),
                         'https://archive.ubuntu.com/ubuntu/pool/main/g/gcc.deb')
        for url in ('file:/etc/passwd', 'https://other.example/pool/a.deb',
                    'mirror+file:/etc/apt/unexpected/pool/a.deb'):
            with self.subTest(url=url), self.assertRaises(ValueError): ubuntu_url(url, 'amd64')

    def test_archive_denial_uses_the_same_pool_path_on_the_official_mirror(self):
        url = 'https://archive.ubuntu.com/ubuntu/pool/main/a/a/x%2by.deb'
        mirror = 'https://azure.archive.ubuntu.com/ubuntu/pool/main/a/a/x%2by.deb'
        denied = HTTPError(url, 403, 'Forbidden', {}, io.BytesIO())
        with patch('linux_cross_sdk.urlopen', side_effect=[denied, io.BytesIO(b'abcdef')]) as get:
            self.assertEqual(download_package(url, 4), b'abcd')
        self.assertEqual(get.call_args_list, [call(url, timeout=120), call(mirror, timeout=120)])

    def test_other_origin_denial_is_not_retried_as_an_archive_download(self):
        url = 'https://ports.ubuntu.com/ubuntu-ports/pool/main/a/a/x.deb'
        denied = HTTPError(url, 403, 'Forbidden', {}, io.BytesIO())
        with patch('linux_cross_sdk.urlopen', side_effect=denied) as get:
            with self.assertRaises(HTTPError):
                download_package(url, 4)
        get.assert_called_once_with(url, timeout=120)

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
